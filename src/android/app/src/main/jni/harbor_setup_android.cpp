// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Harbor on Android: the setup wizard's host and SDL entry point, the readiness check and the console
// mode switches. The wizard runs in this library on the SDL thread of HarborSetupActivity (an
// SDLActivity), with the same SDL3 Eden's audio uses: one SDL copy, one org.libsdl.app.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <jni.h>

#include "android_config.h"
#include "android_settings.h"
#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"
#include "core/openpak/boot_connection.h"
#include "frontend_common/joycon_rails.h"
#include "harbor_setup/driver_catalog.h"
#include "harbor_setup/driver_recovery.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/host.h"
#include "harbor_setup/text_entry.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "harbor_setup_android.h"
#include "native.h"

extern std::unique_ptr<AndroidConfig> global_config;

namespace {

using namespace HarborSetup;

std::atomic_bool console_mode{false};
std::atomic_bool initial_console_dark{false};

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
jmethodID remove_driver = nullptr;
jmethodID finish_setup = nullptr;
jmethodID edit_text = nullptr;
jmethodID cancel_text = nullptr;
std::mutex text_mutex;
std::function<void(std::optional<std::string>)> text_finished;
jclass sleep_class = nullptr;
jmethodID sleep_changed = nullptr;
jmethodID dim_changed = nullptr;

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

/// Calls into Java for callbacks the emulator makes from its own threads. Those can run on a guest
/// fiber (am's, omm's and the services' threads), where a JNI call segfaults (ART checks the stack
/// it was attached with), and they hold the emulator's locks. One plain thread, attached to the VM
/// once, takes each call in order; the caller only queues it and never waits.
class JavaCallQueue {
public:
    void Post(std::function<void(JNIEnv*)> call) {
        {
            std::scoped_lock lk{mutex};
            calls.push_back(std::move(call));
            if (!started) {
                started = true;
                std::thread([this] { Run(); }).detach();
            }
        }
        cv.notify_one();
    }

private:
    [[noreturn]] void Run() {
        JNIEnv* env = Common::Android::GetEnvForThread();
        while (true) {
            std::function<void(JNIEnv*)> call;
            {
                std::unique_lock lk{mutex};
                cv.wait(lk, [this] { return !calls.empty(); });
                call = std::move(calls.front());
                calls.pop_front();
            }
            const bool framed = env->PushLocalFrame(16) == JNI_OK;
            call(env);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
            if (framed) {
                env->PopLocalFrame(nullptr);
            }
        }
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void(JNIEnv*)>> calls;
    bool started = false;
};

JavaCallQueue& JavaCalls() {
    static auto* queue = new JavaCallQueue; // lives as long as the process: its thread does
    return *queue;
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
                 record.AutoloadDirs().size());
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

    bool RemoveDriver(const std::string& id, std::string* error) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        const bool ok = env->CallStaticBooleanMethod(bridge_class, remove_driver,
                                                     Common::Android::ToJString(env, id)) == JNI_TRUE;
        if (ClearException(env) || !ok) {
            if (error) {
                *error = "The driver could not be removed.";
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

/// This phone, probed once (the system's Vulkan driver names the GPU).
const DeviceInfo& HarborDevice() {
    static const DeviceInfo device =
        ProbeDevice(Common::FS::GetEdenPathString(Common::FS::EdenPath::EdenDir), true);
    return device;
}

} // namespace

namespace Harbor {

void ApplyInitialConsoleTheme(Core::System& system) {
    const auto settings =
        system.ServiceManager().GetService<Service::Set::ISystemSettingsServer>("set:sys", false);
    if (!settings) {
        return;
    }
    Service::Set::InitialLaunchSettings initial{};
    if (settings->GetInitialLaunchSettings(&initial).IsError() ||
        initial.flags.InitialLaunchCompletionFlag.Value() != 0) {
        return;
    }
    const auto color = initial_console_dark.load() ? Service::Set::ColorSet::BasicBlack
                                                  : Service::Set::ColorSet::BasicWhite;
    Service::Set::ColorSet current{};
    if (settings->GetColorSetId(&current).IsSuccess() && current != color) {
        settings->SetColorSetId(color);
    }
}

bool ConsoleMode() {
    return console_mode.load();
}

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): physical pads and the touch
// overlay share the handheld controller (docs/android-port/controller-support.md).
void StartWithJoyconsAttached() {
    constexpr std::size_t HandheldIndex = 8;
    auto& players = Settings::values.players.GetValue();
    auto& player1 = players[0];
    auto& handheld = players[HandheldIndex];
    // A pad bound before HOME and Capture were mapped gets them now (see AddSystemButtons).
    auto& home = player1.buttons[Settings::NativeButton::Home];
    auto& capture = player1.buttons[Settings::NativeButton::Screenshot];
    for (const auto button : {Settings::NativeButton::Plus, Settings::NativeButton::Minus,
                              Settings::NativeButton::A, Settings::NativeButton::B}) {
        const Common::ParamPackage param{player1.buttons[button]};
        if (param.Get("engine", "") != "android" || !param.Has("button")) {
            continue;
        }
        if (home.empty()) {
            auto mode = param;
            mode.Set("button", 110); // KEYCODE_BUTTON_MODE
            home = mode.Serialize();
            LOG_INFO(Frontend, "harbor: the controller's Mode button is HOME");
        }
        if (capture.empty() && param.Get("guid", "").ends_with("0b05")) {
            auto button9 = param;
            button9.Set("button", 196); // KEYCODE_BUTTON_9 (BUTTON_10 arrives as it too)
            capture = button9.Serialize();
            LOG_INFO(Frontend, "harbor: the ASUS controller's BUTTON_9/BUTTON_10 are Capture");
        }
        break;
    }
    handheld.buttons = player1.buttons;
    handheld.analogs = player1.analogs;
    handheld.motions = player1.motions;
    player1.controller_type = Settings::ControllerType::Handheld;
    player1.connected = false;
    handheld.controller_type = Settings::ControllerType::Handheld;
    handheld.connected = true;
    LOG_INFO(Frontend, "harbor: the console starts with its Joy-Cons attached (Handheld connected, "
                       "Player 1 disconnected)");
}

HarborSetup::Host& DriverHost() {
    static AndroidHost host;
    return host;
}

const HarborSetup::DeviceInfo& Device() {
    return HarborDevice();
}

void SetJoyconsDetached(Core::HID::HIDCore& hid, bool detached) {
    using Core::HID::NpadIdType;
    using Core::HID::NpadStyleIndex;
    auto* const handheld = hid.GetEmulatedController(NpadIdType::Handheld);
    auto* const player1 = hid.GetEmulatedController(NpadIdType::Player1);
    // A style changes only while its controller is disconnected.
    player1->Disconnect();
    handheld->Disconnect();
    if (detached) {
        player1->SetNpadStyleIndex(NpadStyleIndex::JoyconDual);
        player1->Connect();
    } else {
        player1->SetNpadStyleIndex(NpadStyleIndex::Handheld);
        handheld->SetNpadStyleIndex(NpadStyleIndex::Handheld);
        handheld->Connect();
    }
    LOG_INFO(Frontend, "harbor: Joy-Cons {} (Handheld {}, Player 1 {})",
             detached ? "detached" : "attached",
             handheld->IsConnected() ? "connected" : "disconnected",
             player1->IsConnected() ? "connected" : "disconnected");
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

namespace HarborSetup {
void OpenAndroidTextEntry(const TextEntryOptions& options,
                         std::function<void(std::optional<std::string>)> finished) {
    {
        std::scoped_lock lock{text_mutex};
        text_finished = std::move(finished);
    }
    // Copy only field data; the UI callback remains on the wizard thread.
    JavaCalls().Post([title = options.title, hint = options.hint, initial = options.initial,
                      max_length = options.max_length, secret = options.secret](JNIEnv* env) {
        env->CallStaticVoidMethod(bridge_class, edit_text,
            Common::Android::ToJString(env, title), Common::Android::ToJString(env, hint),
            Common::Android::ToJString(env, initial), max_length, secret ? JNI_TRUE : JNI_FALSE);
    });
}

void CancelAndroidTextEntry() {
    JavaCalls().Post([](JNIEnv* env) { env->CallStaticVoidMethod(bridge_class, cancel_text); });
}
} // namespace HarborSetup

extern "C" {

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_onTextEntered(
    JNIEnv* env, jobject, jstring text) {
    std::function<void(std::optional<std::string>)> finished;
    {
        std::scoped_lock lock{text_mutex};
        finished = std::move(text_finished);
        text_finished = {};
    }
    if (finished) {
        finished(text ? std::optional{Common::Android::GetJString(env, text)} : std::nullopt);
    }
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_initialize(JNIEnv* env, jobject) {
    // Every log line reaches eden_log.txt at once (Eden buffers below Error): a crash in the setup
    // or the console then leaves its last lines in the file.
    Settings::values.log_flush_line.SetValue(true);
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
    remove_driver = env->GetStaticMethodID(bridge_class, "removeDriver", "(Ljava/lang/String;)Z");
    edit_text = env->GetStaticMethodID(bridge_class, "editText",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IZ)V");
    cancel_text = env->GetStaticMethodID(bridge_class, "cancelText", "()V");
    finish_setup = env->GetStaticMethodID(bridge_class, "finishSetup", "(Z)V");
    jclass sleep_local = env->FindClass("org/yuzu/yuzu_emu/harbor/HarborSleep");
    sleep_class = static_cast<jclass>(env->NewGlobalRef(sleep_local));
    env->DeleteLocalRef(sleep_local);
    sleep_changed = env->GetStaticMethodID(sleep_class, "onSleepChanged", "(Z)V");
    dim_changed = env->GetStaticMethodID(sleep_class, "onDimChanged", "(Z)V");
    // am calls this at the sleep sequence's deepest point and on wake, idle:sys when the screen dims
    // and comes back; both from emulator threads with locks held, so the call to HarborSleep goes
    // through the Java call queue, and HarborSleep moves the work to the UI thread.
    if (sleep_class && sleep_changed && dim_changed) {
        auto& applet_manager = EmulationSession::GetInstance().System().GetAppletManager();
        applet_manager.SetSleepHandler([](bool sleeping) {
            JavaCalls().Post([sleeping](JNIEnv* call_env) {
                call_env->CallStaticVoidMethod(sleep_class, sleep_changed,
                                               sleeping ? JNI_TRUE : JNI_FALSE);
            });
        });
        applet_manager.SetDimHandler([](bool dimmed) {
            JavaCalls().Post([dimmed](JNIEnv* call_env) {
                call_env->CallStaticVoidMethod(sleep_class, dim_changed,
                                               dimmed ? JNI_TRUE : JNI_FALSE);
            });
        });
    } else {
        ClearException(env);
        LOG_ERROR(Frontend, "harbor: HarborSleep's callbacks are missing; no sleep or dim screen");
    }
}

/// "" when the console can boot, "setup" when the setup must run first, else why the check itself
/// failed (the launcher's "Harbor cannot start").
JNIEXPORT jstring JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_checkConsole(JNIEnv* env, jobject) {
    HarborSetup::Readiness readiness;
    try {
        readiness = HarborSetup::CheckReadiness();
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "harbor: readiness check failed: {}", e.what());
        return Common::Android::ToJString(env, std::string{e.what()}.empty() ? "The readiness check failed."
                                                                             : e.what());
    }
    LOG_INFO(Frontend, "harbor: readiness keys={} firmware={} qlaunch={} record_completed={}",
             readiness.keys_usable, readiness.firmware_version.empty() ? "none" : readiness.firmware_version,
             readiness.qlaunch, readiness.record_completed);
    return Common::Android::ToJString(env, readiness.Ready() ? "" : "setup");
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

/// A picker that could not open: the wizard says why and keeps the earlier choice.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_onPickFailed(JNIEnv* env, jobject, jint id,
                                                                               jstring message) {
    Java_org_yuzu_yuzu_1emu_harbor_HarborNative_onPicked(env, nullptr, id, nullptr, nullptr);
    HarborSetup::ReportPickerFailure(message ? Common::Android::GetJString(env, message) : std::string{});
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

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_setInitialConsoleDarkTheme(
    JNIEnv*, jobject, jboolean dark) {
    initial_console_dark = dark == JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_setConsoleMode(JNIEnv*, jobject,
                                                                                 jboolean active) {
    console_mode = active == JNI_TRUE;
}

/// What a console session needs every boot: the firmware's own applets, not Eden's stand-ins, and
/// the setup's folders as the library (so the scan that follows registers the latest add-ons).
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_applyConsolePreset(JNIEnv*, jobject) {
    try {
        HarborSetup::ApplyConsolePreset();
        const auto record = LoadRecord(RecordPath());
        ApplyRecordToSettings(record);
        if (global_config) {
            global_config->SaveAllValues();
        }
        LOG_INFO(Frontend, "harbor: console preset applied (every applet LLE, handheld, {} game and {} "
                           "add-on folders)",
                 AndroidSettings::values.game_dirs.size(), record.AutoloadDirs().size());
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "harbor: console preset failed: {}", e.what());
    }
}

/// The setup's DLC and update folders (tree URIs): HarborConsole registers the files under them.
JNIEXPORT jobjectArray JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_addonFolders(JNIEnv* env,
                                                                                        jobject) {
    std::vector<std::string> dirs;
    try {
        dirs = LoadRecord(RecordPath()).AutoloadDirs();
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "harbor: the setup record could not be read: {}", e.what());
    }
    return Common::Android::ToJStringArray(env, dirs);
}

/// OpenPak before HOME boots, with its credentials in the Android Keystore. Blocking (up to ~10 s
/// on a first connect): called on the launcher's boot thread, a Java thread, which the Keystore
/// store needs to find its Kotlin class.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_connectOpenPak(JNIEnv*, jobject) {
    try {
        HarborSetup::ConnectOpenPak();
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "harbor: OpenPak boot connection failed: {}", e.what());
    }
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_stopOpenPak(JNIEnv*, jobject) {
    OpenPak::BootConnection::Stop();
}

/// 0 none, 1 turn off, 2 restart: what HOME's power menu asked for when the console stopped.
JNIEXPORT jint JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_powerRequest(JNIEnv*, jobject) {
    return static_cast<jint>(HarborSetup::ConsolePowerRequest(EmulationSession::GetInstance().System()));
}

/// Why the host stopped the console (e.g. the GPU was lost), cleared once read; "" when it did not.
JNIEXPORT jstring JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_hostStopReason(JNIEnv* env,
                                                                                    jobject) {
    return Common::Android::ToJString(env,
                                      EmulationSession::GetInstance().System().TakeHostStopReason());
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_setJoyconsDetached(JNIEnv*, jobject,
                                                                                     jboolean detached) {
    Harbor::SetJoyconsDetached(EmulationSession::GetInstance().System().HIDCore(),
                               detached == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_logGuestThreads(JNIEnv*, jobject) {
    EmulationSession::GetInstance().System().LogGuestThreads();
}

/// Loads the console's Vulkan driver and checks it is the one the setup chose: "" when it is, else
/// why the console must not boot (EmulationSession::CheckGpuDriver). Not on the UI thread.
JNIEXPORT jstring JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_checkGpuDriver(JNIEnv* env,
                                                                                    jobject) {
    return Common::Android::ToJString(env, EmulationSession::GetInstance().CheckGpuDriver());
}

JNIEXPORT jboolean JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_joyconsDetached(JNIEnv*, jobject) {
    return FrontendCommon::JoyconsDetached(EmulationSession::GetInstance().System().HIDCore()) ? JNI_TRUE
                                                                                             : JNI_FALSE;
}

/// The console's power button held (a middle press: the overlay's power menu with Restart and Power
/// Off), as the Ryujinx Harbor's F5. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_pressPower(JNIEnv*, jobject) {
    auto& session = EmulationSession::GetInstance();
    if (!session.IsRunning()) {
        LOG_INFO(Frontend, "harbor: power button ignored: the console is not running");
        return;
    }
    session.System().GetAppletManager().PowerButtonMiddlePress();
}

/// The console's power button pressed briefly: it goes to sleep, or wakes when it sleeps
/// (AppletManager::PowerButtonShortPress; HarborSleep does the host side).
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_powerShortPress(JNIEnv*, jobject) {
    auto& session = EmulationSession::GetInstance();
    if (!session.IsRunning()) {
        LOG_INFO(Frontend, "harbor: power button ignored: the console is not running");
        return;
    }
    session.System().GetAppletManager().PowerButtonShortPress();
}

/// Android's audio focus as a gain on the console's output (1 full, 0.25 ducked, 0 muted). The
/// saved volume is untouched: a lower gain is the setting's per-session value, which the global
/// config never writes, and every new session starts from the saved one.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_setHostGain(JNIEnv*, jobject, jfloat gain) {
    auto& volume = Settings::values.volume;
    if (gain >= 1.0f) {
        volume.SetGlobal(true);
        return;
    }
    const auto saved = volume.GetValue(true);
    volume.SetGlobal(false);
    volume.SetValue(static_cast<u8>(static_cast<float>(saved) * std::clamp(gain, 0.0f, 1.0f)));
}

/// The console's theme for the touch controls: 1 Basic Black, 0 Basic White, -1 while no console runs.
JNIEXPORT jint JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_consoleTheme(JNIEnv*, jobject) {
    auto& session = EmulationSession::GetInstance();
    if (!session.IsRunning()) {
        return -1;
    }
    const auto set_sys = session.System().ServiceManager().GetService<Service::Set::ISystemSettingsServer>(
        "set:sys", false);
    if (!set_sys) {
        return -1;
    }
    Service::Set::ColorSet color{};
    if (set_sys->GetColorSetId(&color).IsError()) {
        return -1;
    }
    return color == Service::Set::ColorSet::BasicBlack ? 1 : 0;
}

/// The console's saved colour set for Harbor's own screens: 1 Basic Black, 0 Basic White, -1 before
/// the console has booted once (the phone's theme then).
JNIEXPORT jint JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_savedConsoleTheme(JNIEnv*, jobject) {
    const auto dark = HarborSetup::SavedConsoleDarkTheme();
    return dark ? (*dark ? 1 : 0) : -1;
}

/// Where the console's picture is on the emulation surface: left, top, right, bottom in pixels, or
/// an empty array while no console runs. Touches outside it are not the touch screen.
JNIEXPORT jintArray JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_pictureRect(JNIEnv* env, jobject) {
    auto& session = EmulationSession::GetInstance();
    if (!session.IsRunning()) {
        return env->NewIntArray(0);
    }
    const auto& screen = session.Window().GetFramebufferLayout().screen;
    const jint rect[4] = {static_cast<jint>(screen.left), static_cast<jint>(screen.top),
                          static_cast<jint>(screen.right), static_cast<jint>(screen.bottom)};
    jintArray out = env->NewIntArray(4);
    env->SetIntArrayRegion(out, 0, 4, rect);
    return out;
}

/// Why an installed driver does not fit this GPU (its library's Adreno generation, its Android
/// version), or "".
JNIEXPORT jstring JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_driverProblem(JNIEnv* env, jobject,
                                                                                   jstring name,
                                                                                   jstring library,
                                                                                   jint min_api) {
    return Common::Android::ToJString(
        env, HarborSetup::DriverPackageProblem(Common::Android::GetJString(env, name),
                                               Common::Android::GetJString(env, library), min_api,
                                               HarborDevice()));
}

/// A driver package the user picked (the stopped screen's "Install a driver file..."): read, checked
/// as the setup checks it, installed and chosen: [id, name, error]. Blocking; not on the UI thread.
JNIEXPORT jobjectArray JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_installDriverFile(JNIEnv* env, jobject,
                                                                                            jstring location,
                                                                                            jstring name) {
    auto& host = Harbor::DriverHost();
    const auto result = HarborSetup::InstallDriverPackage(host, HarborDevice(),
                                                          Common::Android::GetJString(env, location),
                                                          Common::Android::GetJString(env, name));
    if (!result.installed) {
        return Common::Android::ToJStringArray(env, {"", "", result.error});
    }
    std::string error;
    if (!host.SelectDriver(result.installed->id, &error)) {
        return Common::Android::ToJStringArray(
            env, {"", "", error.empty() ? "The driver choice could not be saved." : error});
    }
    return Common::Android::ToJStringArray(env, {result.installed->id, result.installed->name, ""});
}

} // extern "C"
