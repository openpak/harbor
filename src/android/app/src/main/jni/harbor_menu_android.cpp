// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Android's side of the Harbor menu (harbor_setup/menu.h): the menu draws onto the console's screen
// from its own thread; what it asks of the app goes to HarborMenuBridge (Kotlin) by name.

#include <jni.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>
#include <sstream>
#include <thread>

#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "frontend_common/joycon_rails.h"
#include "harbor_setup/device_check.h"
#include "harbor_setup/host.h"
#include "harbor_setup/menu.h"
#include "harbor_setup/menu_input.h"
#include "video_core/host_screen.h"
#include "harbor_setup_android.h"
#include "native.h"
#include "common/harbor_crash.h"

namespace {

jclass bridge = nullptr;
jmethodID bridge_call = nullptr;

/// HarborMenuBridge.call(what, argument): every request the menu makes of the app, by name.
std::string Call(const std::string& what, const std::string& argument = {}) {
    if (!bridge) {
        return {};
    }
    JNIEnv* env = Common::Android::GetEnvForThread();
    jstring j_what = Common::Android::ToJString(env, what);
    jstring j_argument = Common::Android::ToJString(env, argument);
    auto* result = static_cast<jstring>(env->CallStaticObjectMethod(bridge, bridge_call, j_what, j_argument));
    std::string out;
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOG_WARNING(Frontend, "harbor: menu: the app could not do \"{}\"", what);
    } else if (result) {
        out = Common::Android::GetJString(env, result);
    }
    env->DeleteLocalRef(j_what);
    env->DeleteLocalRef(j_argument);
    if (result) {
        env->DeleteLocalRef(result);
    }
    return out;
}

bool Flag(const std::string& what) {
    return Call(what) == "1";
}

int Number(const std::string& what, int fallback) {
    try {
        return std::stoi(Call(what));
    } catch (...) {
        return fallback;
    }
}

// InputCommon's Android devices are Java objects the UI thread adds and removes: the menu's thread
// reads a copy taken on the UI thread (HarborNative.checkControllers).
std::mutex devices_mutex;
std::vector<Common::ParamPackage> devices;

/// A notice waits for four seconds of the console's pictures (past its boot logo, a notice made
/// while it restarts), then stays eight seconds.
constexpr u64 NoticeWaitFrames = 240;

class AndroidMenuHost final : public HarborSetup::MenuHost {
public:
    std::vector<Common::ParamPackage> InputDevices() override {
        std::scoped_lock lk{devices_mutex};
        return devices;
    }
    void MenuThreadFrame(const std::function<void()>& frame) override {
        // The menu's thread stays attached to the VM: its JNI local references go with each frame.
        JNIEnv* env = Common::Android::GetEnvForThread();
        const bool framed = env->PushLocalFrame(64) == JNI_OK;
        frame();
        if (framed) {
            env->PopLocalFrame(nullptr);
        }
    }
    bool IsAndroid() const override {
        return true;
    }
    Core::System& System() override {
        return EmulationSession::GetInstance().System();
    }
    InputCommon::InputSubsystem& Input() override {
        return EmulationSession::GetInstance().GetInputSubsystem();
    }
    bool PlatformDarkTheme() override {
        return Flag("platformDark");
    }
    void SaveConfig() override {
        Call("saveConfig");
    }

    bool JoyconsDetached() override {
        return FrontendCommon::JoyconsDetached(System().HIDCore());
    }
    void SetJoyconsDetached(bool detached) override {
        Harbor::SetJoyconsDetached(System().HIDCore(), detached);
    }
    void Restart() override {
        Call("restart");
    }
    void SetupAgain() override {
        Call("setupAgain");
    }
    void Exit() override {
        Call("exit");
    }

    bool ShowFps() override {
        return Flag("showFps");
    }
    void SetShowFps(bool show) override {
        Call("setShowFps", show ? "1" : "0");
    }
    bool HasDeviceLine() override {
        return true;
    }
    bool ShowDeviceLine() override {
        return Flag("showDeviceLine");
    }
    void SetShowDeviceLine(bool show) override {
        Call("setShowDeviceLine", show ? "1" : "0");
    }
    bool FrameGenSupported() override {
        return Flag("frameGenSupported");
    }
    void ChooseLosslessDll() override {
        Call("chooseLosslessDll");
    }
    void RemoveLosslessDll() override {
        Call("removeLosslessDll");
    }
    bool HasDriverChoice() override {
        return Flag("hasDriverChoice");
    }
    std::vector<HarborSetup::MenuChoice> Drivers() override {
        // Lines of id, name and detail, three per driver; the phone's own first.
        std::vector<HarborSetup::MenuChoice> out;
        std::istringstream lines(Call("drivers"));
        std::string id, label, detail;
        while (std::getline(lines, id) && std::getline(lines, label) && std::getline(lines, detail)) {
            out.push_back({id, label, detail});
        }
        return out;
    }
    std::string CurrentDriver() override {
        return Call("currentDriver");
    }
    void ChooseDriver(const std::string& id) override {
        Call("chooseDriver", id);
    }
    HarborSetup::Host* DriverHost() override {
        return HasDriverChoice() ? &Harbor::DriverHost() : nullptr;
    }
    HarborSetup::DeviceInfo DriverDevice() override {
        return Harbor::Device();
    }

    bool HasControllerHomeKey() override {
        return true;
    }
    bool ControllerHomeKeyEnabled() override {
        return Flag("homeKeyEnabled");
    }
    void OpenControllerHomeKeySettings() override {
        Call("openHomeKeySettings");
    }

    bool HasTouchControls() override {
        return true;
    }
    bool TouchControlsHidden() override {
        return Flag("touchHidden");
    }
    void SetTouchControlsHidden(bool hidden) override {
        Call("setTouchHidden", hidden ? "1" : "0");
    }
    bool PowerControlShown() override {
        return Flag("powerControlShown");
    }
    void SetPowerControlShown(bool shown) override {
        Call("setPowerControlShown", shown ? "1" : "0");
    }
    int TouchOpacity() override {
        return Number("touchOpacity", 100);
    }
    void SetTouchOpacity(int percent) override {
        Call("setTouchOpacity", std::to_string(percent));
    }
    int TouchScale() override {
        return Number("touchScale", 50);
    }
    void SetTouchScale(int percent) override {
        Call("setTouchScale", std::to_string(percent));
    }
    void EditTouchLayout() override {
        Call("editTouchLayout");
    }
    void ResetTouchLayout() override {
        Call("resetTouchLayout");
    }

    void ConsoleAction(const std::string& name) override {
        Call("consoleAction", name);
    }

    void ShareCrashReport(const Common::HarborCrash::Report& report) override {
        Call("shareCrashReport", Common::FS::PathToUTF8String(report.path) + "\n" + report.name);
    }

    void HostPageAction(const std::string& action, const std::string& argument) override {
        Call("hostPage", action + "\n" + argument);
    }
    std::vector<HarborSetup::MenuChoice> FittingDrivers() override {
        // Lines of id and label, two per driver.
        std::vector<HarborSetup::MenuChoice> out;
        std::istringstream lines(Call("fittingDrivers"));
        std::string id, label;
        while (std::getline(lines, id) && std::getline(lines, label)) {
            out.push_back({id, label, ""});
        }
        return out;
    }

    // Notices are a line on the console's screen (the menu's overlay, as on the desktop), drawn by
    // the menu's thread: no JNI from the caller, which may be guest service code on a Fiber.
    bool HasHud() const override {
        return true;
    }
    std::optional<std::string> HudText() override {
        std::scoped_lock lk{notice_mutex};
        if (notice.empty()) {
            return std::nullopt;
        }
        const auto now = std::chrono::steady_clock::now();
        // The time runs from when the console's picture is there to show it (NoticeWaitFrames).
        if (!notice_shown) {
            if (VideoCore::HostScreen::GuestFrames() < notice_frames + NoticeWaitFrames) {
                return std::nullopt;
            }
            notice_shown = true;
            notice_until = now + std::chrono::seconds(8);
        }
        if (now >= notice_until) {
            notice.clear();
            return std::nullopt;
        }
        return notice;
    }

    void HostNotice(const std::string& text) override {
        LOG_INFO(Frontend, "harbor: notice: {}", text);
        std::scoped_lock lk{notice_mutex};
        notice = text;
        notice_shown = false;
        notice_frames = VideoCore::HostScreen::GuestFrames();
    }

private:
    std::mutex notice_mutex;
    std::string notice;
    bool notice_shown = false;
    u64 notice_frames = 0;
    std::chrono::steady_clock::time_point notice_until{};

public:
};

AndroidMenuHost* menu_host = nullptr;

// Harbor's host pages (HarborScreenView): the menu's pictures copied into the view's surface.
std::mutex surface_mutex;
ANativeWindow* surface = nullptr;
std::pair<u32, u32> surface_size{};

void DrawToSurface(std::shared_ptr<const VideoCore::HostScreen::Frame> frame) {
    std::scoped_lock lk{surface_mutex};
    if (!surface || !frame) {
        return;
    }
    if (surface_size != std::pair{frame->width, frame->height}) {
        ANativeWindow_setBuffersGeometry(surface, static_cast<int32_t>(frame->width),
                                         static_cast<int32_t>(frame->height), WINDOW_FORMAT_RGBA_8888);
        surface_size = {frame->width, frame->height};
    }
    ANativeWindow_Buffer buffer{};
    if (ANativeWindow_lock(surface, &buffer, nullptr) != 0) {
        return;
    }
    const u32 rows = std::min<u32>(frame->height, static_cast<u32>(buffer.height));
    const std::size_t row_bytes = std::min<std::size_t>(frame->width, static_cast<std::size_t>(buffer.width)) * 4;
    for (u32 y = 0; y < rows; y++) {
        std::memcpy(static_cast<u8*>(buffer.bits) + static_cast<std::size_t>(y) * buffer.stride * 4,
                    frame->rgba.data() + static_cast<std::size_t>(y) * frame->width * 4, row_bytes);
    }
    ANativeWindow_unlockAndPost(surface);
}

void InitMenu(JNIEnv* env);

} // namespace

extern "C" {

/// Once the console boots: the menu's host, the Power button binding, and the bridge it calls.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_initMenu(JNIEnv* env, jobject) {
    InitMenu(env);
}

} // extern "C"

namespace {

void InitMenu(JNIEnv* env) {
    HARBOR_CRUMB("menu: initMenu");
    static std::once_flag once;
    std::call_once(once, [env] {
        jclass local = env->FindClass("org/yuzu/yuzu_emu/harbor/HarborMenuBridge");
        bridge = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        bridge_call =
            env->GetStaticMethodID(bridge, "call", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        static AndroidMenuHost host;
        menu_host = &host;
        HarborSetup::MenuSetHost(host);
        // The crash-report question, or last time's report, once the console's picture is up.
        HarborSetup::MenuOfferCrashReport();
    });
    HARBOR_CRUMB("menu: initMenu done");
}

} // namespace

extern "C" {

/// On the UI thread when a controller connects, changes or goes, and as the menu opens: the device
/// list the menu shows, and a known pad's saved mapping applied (HarborSetup::CheckControllers).
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_checkControllers(JNIEnv*, jobject) {
    if (!bridge) {
        return;
    }
    auto& session = EmulationSession::GetInstance();
    {
        auto list = session.GetInputSubsystem().GetInputDevices();
        std::scoped_lock lk{devices_mutex};
        devices = std::move(list);
    }
    if (menu_host) {
        HarborSetup::CheckControllers(*menu_host);
    }
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_openMenu(JNIEnv* env, jobject, jint page) {
    HarborSetup::MenuOpen(static_cast<HarborSetup::MenuPage>(page));
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_closeMenu(JNIEnv*, jobject) {
    HarborSetup::MenuClose();
}

JNIEXPORT jboolean JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_menuOpen(JNIEnv*, jobject) {
    return HarborSetup::MenuIsOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_menuBack(JNIEnv*, jobject) {
    HarborSetup::MenuBack();
}

/// Harbor's own pages outside the console (HarborScreenActivity): 0 status, 1 stopped, 2 cannot
/// start, 3 driver failed, with its text. Their pictures go to the surface given by hostSurface.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_showHostPage(JNIEnv* env, jobject, jint kind,
                                                                               jstring text) {
    InitMenu(env);
    HarborSetup::MenuShowHostPage({.kind = static_cast<HarborSetup::HostPageKind>(kind),
                                   .text = text ? Common::Android::GetJString(env, text) : std::string{}},
                                  DrawToSurface);
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_endHostPage(JNIEnv*, jobject) {
    HarborSetup::MenuEndHostPage();
}

/// The view's surface appeared (a Surface) or went (null).
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_hostSurface(JNIEnv* env, jobject,
                                                                              jobject view_surface) {
    std::scoped_lock lk{surface_mutex};
    if (surface) {
        ANativeWindow_release(surface);
        surface = nullptr;
    }
    surface_size = {};
    if (view_surface) {
        surface = ANativeWindow_fromSurface(env, view_surface);
    }
}

/// A button by the console's positions (HarborSetup::HostButton: A, B, X, Y, up, down, left, right).
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_hostButton(JNIEnv*, jobject, jint button,
                                                                             jboolean down) {
    if (button >= 0 && button <= static_cast<jint>(HarborSetup::HostButton::Right)) {
        HarborSetup::MenuHostButton(static_cast<HarborSetup::HostButton>(button), down == JNI_TRUE);
    }
}

/// A finger on the view, `x` and `y` from 0 to 1 across the picture.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_hostTouch(JNIEnv*, jobject, jint id, jfloat x,
                                                                            jfloat y, jboolean down) {
    HarborSetup::MenuHostTouch(id, x, y, down == JNI_TRUE);
}

/// A status line on the console's screen while Harbor stops it.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_menuStatus(JNIEnv* env, jobject, jstring text) {
    HarborSetup::MenuShowStatus(text ? Common::Android::GetJString(env, text) : std::string{});
}

/// A short notice from Harbor itself: a line on the console's screen (the menu's overlay).
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_hostNotice(JNIEnv* env, jobject, jstring text) {
    if (menu_host && text) {
        menu_host->HostNotice(Common::Android::GetJString(env, text));
    }
}

/// A line for the open host page (a driver file installed through the system's picker).
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_hostPageNotice(JNIEnv* env, jobject,
                                                                                 jstring text, jboolean bad) {
    HarborSetup::MenuHostPageNotice(text ? Common::Android::GetJString(env, text) : std::string{},
                                    bad == JNI_TRUE);
}

} // extern "C"
