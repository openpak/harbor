// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Harbor on Android: the setup wizard's host and SDL entry point, the readiness check and the console
// mode switches. The wizard runs in this library on the SDL thread of HarborSetupActivity (an
// SDLActivity), with the same SDL3 Eden's audio uses: one SDL copy, one org.libsdl.app.

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include <jni.h>

#include "android_config.h"
#include "android_settings.h"
#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/openpak/boot_connection.h"
#include "frontend_common/joycon_rails.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/host.h"
#include "harbor_setup_android.h"
#include "native.h"

extern std::unique_ptr<AndroidConfig> global_config;

namespace {

using namespace HarborSetup;

std::atomic_bool console_mode{false};

/// The setup's folders as Eden's: game folders (tree URIs, scanned deep) and the DLC and update
/// folders as external content. GameHelper.getGames() then registers everything under them.
void ApplyRecordToSettings(const Record& record) {
    AndroidSettings::values.game_dirs.clear();
    for (const auto& dir : record.game_dirs.value_or(std::vector<std::string>{})) {
        AndroidSettings::values.game_dirs.push_back({dir, true});
    }
    HarborSetup::ApplyRecordFolders(record);
}

// Java classes and methods, looked up once on a Java thread (a native thread's FindClass sees only
// the system class loader).
jclass bridge_class = nullptr;
jmethodID pick_file = nullptr;
jmethodID pick_folder = nullptr;
jmethodID http_get = nullptr;
jmethodID download = nullptr;
jmethodID installed_drivers = nullptr;
jmethodID current_driver = nullptr;
jmethodID inspect_driver = nullptr;
jmethodID commit_driver = nullptr;
jmethodID discard_driver = nullptr;
jmethodID select_driver = nullptr;
jmethodID finish_setup = nullptr;

std::mutex picks_mutex;
std::map<int, PickCallback> picks;
int next_pick = 1;

std::vector<std::string> Strings(JNIEnv* env, jobjectArray array) {
    std::vector<std::string> out;
    if (!array) {
        return out;
    }
    const jsize count = env->GetArrayLength(array);
    for (jsize i = 0; i < count; i++) {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        out.push_back(item ? Common::Android::GetJString(env, item) : std::string{});
        env->DeleteLocalRef(item);
    }
    env->DeleteLocalRef(array);
    return out;
}

bool ClearException(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    return false;
}

int Remember(PickCallback done) {
    std::scoped_lock lock{picks_mutex};
    const int id = next_pick++;
    picks.emplace(id, std::move(done));
    return id;
}

// Download progress goes back to the wizard through this, by its address.
struct DownloadHandle {
    const std::function<void(std::uint64_t)>* progress;
    const std::atomic_bool* cancel;
};

class AndroidHost final : public Host {
public:
    bool IsAndroid() const override {
        return true;
    }

    DeviceInfo Probe() override {
        return ProbeDevice(Common::FS::GetEdenPathString(Common::FS::EdenPath::EdenDir), true);
    }

    void PickFile(const std::string& title, const std::vector<std::string>& extensions,
                  PickCallback done) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        const int id = Remember(std::move(done));
        env->CallStaticVoidMethod(bridge_class, pick_file, id, Common::Android::ToJString(env, title),
                                  Common::Android::ToJStringArray(env, extensions));
        ClearException(env);
    }

    void PickFolder(const std::string& title, PickCallback done) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        const int id = Remember(std::move(done));
        env->CallStaticVoidMethod(bridge_class, pick_folder, id, Common::Android::ToJString(env, title));
        ClearException(env);
    }

    // A tree URI is not a path; Eden reads game folders by tree URI, but firmware needs a .zip.
    bool CanPickFirmwareFolder() const override {
        return false;
    }

    bool ApplyFolders(const Record& record, std::string* error) override {
        ApplyRecordToSettings(record);
        if (!global_config) {
            if (error) {
                *error = "the configuration is not loaded";
            }
            return false;
        }
        global_config->SaveAllValues();
        LOG_INFO(Frontend, "harbor: {} game and {} add-on folders saved", AndroidSettings::values.game_dirs.size(),
                 Settings::values.external_content_dirs.size());
        return true;
    }

    void FirmwareInstalled() override {
        // The emulator core rescans its NAND, as Eden's own firmware install does.
        EmulationSession::GetInstance().InitializeSystem(true);
    }

    void Finished(const Record&) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        env->CallStaticVoidMethod(bridge_class, finish_setup, JNI_TRUE);
        ClearException(env);
    }

    bool HasDriverStore() const override {
        return true;
    }

    std::vector<InstalledDriver> InstalledDrivers() override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        const auto flat = Strings(env, static_cast<jobjectArray>(
                                           env->CallStaticObjectMethod(bridge_class, installed_drivers)));
        ClearException(env);
        std::vector<InstalledDriver> drivers;
        for (size_t i = 0; i + 2 < flat.size(); i += 3) {
            drivers.push_back({flat[i], flat[i + 1], flat[i + 2]});
        }
        return drivers;
    }

    std::string CurrentDriver() override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        auto value = static_cast<jstring>(env->CallStaticObjectMethod(bridge_class, current_driver));
        ClearException(env);
        return value ? Common::Android::GetJString(env, value) : std::string{};
    }

    DriverPackage InspectDriver(const std::string& location) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        const auto fields = Strings(env, static_cast<jobjectArray>(env->CallStaticObjectMethod(
                                             bridge_class, inspect_driver,
                                             Common::Android::ToJString(env, location))));
        if (ClearException(env) || fields.size() < 7) {
            return {.error = "The driver package could not be read."};
        }
        DriverPackage package{
            .staged = fields[0],
            .name = fields[1],
            .version = fields[2],
            .vendor = fields[3],
            .library_name = fields[4],
            .error = fields[6],
        };
        package.min_api = std::atoi(fields[5].c_str());
        return package;
    }

    std::optional<InstalledDriver> CommitDriver(const DriverPackage& package, std::string* error) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        const auto fields = Strings(env, static_cast<jobjectArray>(env->CallStaticObjectMethod(
                                             bridge_class, commit_driver,
                                             Common::Android::ToJString(env, package.staged))));
        if (ClearException(env) || fields.size() < 3 || fields[0].empty()) {
            if (error) {
                *error = fields.size() >= 4 ? fields[3] : "The driver could not be installed.";
            }
            return std::nullopt;
        }
        return InstalledDriver{fields[0], fields[1], fields[2]};
    }

    void DiscardDriver(const DriverPackage& package) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        env->CallStaticVoidMethod(bridge_class, discard_driver, Common::Android::ToJString(env, package.staged));
        ClearException(env);
    }

    bool SelectDriver(const std::string& id, std::string* error) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        const bool ok = env->CallStaticBooleanMethod(bridge_class, select_driver,
                                                     Common::Android::ToJString(env, id)) == JNI_TRUE;
        if (ClearException(env) || !ok) {
            if (error) {
                *error = "The driver choice could not be saved.";
            }
            return false;
        }
        return true;
    }

    std::optional<std::string> HttpGet(const std::string& url, std::uint64_t max_bytes,
                                       const std::atomic_bool& cancel, std::string* error) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        const auto result = Strings(env, static_cast<jobjectArray>(env->CallStaticObjectMethod(
                                             bridge_class, http_get, Common::Android::ToJString(env, url),
                                             static_cast<jlong>(max_bytes))));
        if (ClearException(env) || result.size() < 2) {
            if (error) {
                *error = "The request failed.";
            }
            return std::nullopt;
        }
        if (result[0] != "ok") {
            if (error) {
                *error = result[1];
            }
            return std::nullopt;
        }
        return result[1];
    }

    DownloadResult Download(const std::string& url, std::uint64_t max_bytes,
                            const std::function<void(std::uint64_t)>& progress,
                            const std::atomic_bool& cancel) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        DownloadHandle handle{&progress, &cancel};
        const auto result = Strings(env, static_cast<jobjectArray>(env->CallStaticObjectMethod(
                                             bridge_class, download, Common::Android::ToJString(env, url),
                                             static_cast<jlong>(max_bytes),
                                             reinterpret_cast<jlong>(&handle))));
        if (ClearException(env) || result.size() < 2) {
            return {.error = "The download failed."};
        }
        if (result[0] != "ok" || result.size() < 4) {
            return {.error = result[1]};
        }
        return {.path = result[1], .sha256 = result[2], .size = std::stoull(result[3])};
    }
};

} // namespace

namespace Harbor {

bool ConsoleMode() {
    return console_mode.load();
}

} // namespace Harbor

/// SDL's entry point for HarborSetupActivity: SDLActivity runs it on its own thread, and finishes the
/// activity when it returns.
extern "C" JNIEXPORT int SDL_main(int argc, char* argv[]) {
    AndroidHost host;
    const int result = HarborSetup::RunWizard(host);
    LOG_INFO(Frontend, "harbor: setup activity main returns {}", result);
    return result;
}

extern "C" {

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_initialize(JNIEnv* env, jobject) {
    if (bridge_class) {
        return;
    }
    jclass local = env->FindClass("org/yuzu/yuzu_emu/harbor/HarborSetupBridge");
    bridge_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    pick_file = env->GetStaticMethodID(bridge_class, "pickFile", "(ILjava/lang/String;[Ljava/lang/String;)V");
    pick_folder = env->GetStaticMethodID(bridge_class, "pickFolder", "(ILjava/lang/String;)V");
    http_get = env->GetStaticMethodID(bridge_class, "httpGet", "(Ljava/lang/String;J)[Ljava/lang/String;");
    download = env->GetStaticMethodID(bridge_class, "download", "(Ljava/lang/String;JJ)[Ljava/lang/String;");
    installed_drivers = env->GetStaticMethodID(bridge_class, "installedDrivers", "()[Ljava/lang/String;");
    current_driver = env->GetStaticMethodID(bridge_class, "currentDriver", "()Ljava/lang/String;");
    inspect_driver =
        env->GetStaticMethodID(bridge_class, "inspectDriver", "(Ljava/lang/String;)[Ljava/lang/String;");
    commit_driver =
        env->GetStaticMethodID(bridge_class, "commitDriver", "(Ljava/lang/String;)[Ljava/lang/String;");
    discard_driver = env->GetStaticMethodID(bridge_class, "discardDriver", "(Ljava/lang/String;)V");
    select_driver = env->GetStaticMethodID(bridge_class, "selectDriver", "(Ljava/lang/String;)Z");
    finish_setup = env->GetStaticMethodID(bridge_class, "finishSetup", "(Z)V");
}

JNIEXPORT jboolean JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_isConsoleReady(JNIEnv*, jobject) {
    const auto readiness = HarborSetup::CheckReadiness();
    LOG_INFO(Frontend, "harbor: readiness keys={} firmware={} qlaunch={} record_completed={}",
             readiness.keys_usable, readiness.firmware_version.empty() ? "none" : readiness.firmware_version,
             readiness.qlaunch, readiness.record_completed);
    return readiness.Ready() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_onPicked(JNIEnv* env, jobject, jint id,
                                                                           jstring uri, jstring name) {
    PickCallback done;
    {
        std::scoped_lock lock{picks_mutex};
        const auto it = picks.find(id);
        if (it == picks.end()) {
            return;
        }
        done = std::move(it->second);
        picks.erase(it);
    }
    if (!uri) {
        done(std::nullopt);
        return;
    }
    const auto location = Common::Android::GetJString(env, uri);
    done(Picked{name ? Common::Android::GetJString(env, name) : location, location,
                location.rfind("content://", 0) != 0});
}

JNIEXPORT jboolean JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_downloadProgress(JNIEnv*, jobject,
                                                                                       jlong handle,
                                                                                       jlong received) {
    const auto* download_handle = reinterpret_cast<const DownloadHandle*>(handle);
    (*download_handle->progress)(static_cast<std::uint64_t>(received));
    return download_handle->cancel->load() ? JNI_FALSE : JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_onBackPressed(JNIEnv*, jobject) {
    HarborSetup::RequestBack();
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_setConsoleMode(JNIEnv*, jobject,
                                                                                 jboolean active) {
    console_mode = active == JNI_TRUE;
}

/// What a console session needs every boot: the firmware's own applets, not Eden's stand-ins, and
/// the setup's folders as the library (so the scan that follows registers the latest add-ons).
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_applyConsolePreset(JNIEnv*, jobject) {
    HarborSetup::ApplyConsolePreset();
    ApplyRecordToSettings(LoadRecord(RecordPath()));
    if (global_config) {
        global_config->SaveAllValues();
    }
    LOG_INFO(Frontend, "harbor: console preset applied (every applet LLE, handheld, {} game and {} "
                       "add-on folders)",
             AndroidSettings::values.game_dirs.size(), Settings::values.external_content_dirs.size());
}

/// OpenPak before HOME boots, with its credentials in the Android Keystore. Blocking (up to ~10 s
/// on a first connect): called on the launcher's boot thread, a Java thread, which the Keystore
/// store needs to find its Kotlin class.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_connectOpenPak(JNIEnv*, jobject) {
    HarborSetup::ConnectOpenPak();
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_stopOpenPak(JNIEnv*, jobject) {
    OpenPak::BootConnection::Stop();
}

/// 0 none, 1 turn off, 2 restart: what HOME's power menu asked for when the console stopped.
JNIEXPORT jint JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_powerRequest(JNIEnv*, jobject) {
    return static_cast<jint>(HarborSetup::ConsolePowerRequest(EmulationSession::GetInstance().System()));
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_setJoyconsDetached(JNIEnv*, jobject,
                                                                                     jboolean detached) {
    FrontendCommon::SetJoyconsDetached(EmulationSession::GetInstance().System().HIDCore(),
                                       detached == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_joyconsDetached(JNIEnv*, jobject) {
    return FrontendCommon::JoyconsDetached(EmulationSession::GetInstance().System().HIDCore()) ? JNI_TRUE
                                                                                             : JNI_FALSE;
}

} // extern "C"
