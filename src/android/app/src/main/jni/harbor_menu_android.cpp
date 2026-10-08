// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Android's side of the Harbor menu (harbor_setup/menu.h): the menu draws onto the console's screen
// from its own thread; what it asks of the app goes to HarborMenuBridge (Kotlin) by name.

#include <jni.h>

#include <mutex>
#include <sstream>

#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/logging.h"
#include "frontend_common/joycon_rails.h"
#include "harbor_setup/menu.h"
#include "harbor_setup_android.h"
#include "native.h"

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

class AndroidMenuHost final : public HarborSetup::MenuHost {
public:
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
};

} // namespace

extern "C" {

/// Once the console boots: the menu's host, the Power button binding, and the bridge it calls.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_initMenu(JNIEnv* env, jobject) {
    static std::once_flag once;
    std::call_once(once, [env] {
        jclass local = env->FindClass("org/yuzu/yuzu_emu/harbor/HarborMenuBridge");
        bridge = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        bridge_call =
            env->GetStaticMethodID(bridge, "call", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        static AndroidMenuHost host;
        HarborSetup::MenuSetHost(host);
    });
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

} // extern "C"
