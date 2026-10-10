// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMetaObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QProcess>
#include <QSettings>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWidget>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "frontend_common/joycon_rails.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "qt_common/config/qt_config.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "harbor_setup/menu_input.h"
#include "yuzu/harbor_menu.h"
#ifdef HAS_LSFG
#include "yuzu/harbor_frame_gen.h"
#endif

namespace {
QString HarborIni() {
    return QString::fromStdString(
        (Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "harbor.ini").string());
}

bool ReadFlag(const char* key) {
    return QSettings(HarborIni(), QSettings::IniFormat).value(QString::fromUtf8(key), false).toBool();
}

void WriteFlag(const char* key, bool value) {
    QSettings(HarborIni(), QSettings::IniFormat).setValue(QString::fromUtf8(key), value);
}

/// What the menu draws with (nanovg's GL3 back end): OpenGL 3.3 core, 8-bit RGBA. The picture
/// itself is the menu's own framebuffer, so the surface needs no depth or stencil.
QSurfaceFormat MenuGlFormat() {
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setRedBufferSize(8);
    format.setGreenBufferSize(8);
    format.setBlueBufferSize(8);
    format.setAlphaBufferSize(8);
    return format;
}

/// A Qt context on the offscreen surface, current on the menu's thread, which owns it.
class QtMenuGl final : public HarborSetup::MenuGl {
public:
    explicit QtMenuGl(std::unique_ptr<QOpenGLContext> context_) : context{std::move(context_)} {}
    ~QtMenuGl() override {
        context->doneCurrent();
    }
    void* ProcAddress(const char* name) override {
        return reinterpret_cast<void*>(context->getProcAddress(name));
    }

private:
    std::unique_ptr<QOpenGLContext> context;
};
} // namespace

HarborMenu::HarborMenu(QWidget* window_, InputCommon::InputSubsystem& input_, Hooks hooks_)
    : window{window_}, input{input_}, hooks{std::move(hooks_)} {
    show_fps = ReadFlag("show_fps");
    show_device_line = ReadFlag("show_device_line");
    // Before MenuSetHost: the menu's thread starts there and asks for its context at once.
    gl_surface = std::make_unique<QOffscreenSurface>();
    gl_surface->setFormat(MenuGlFormat());
    gl_surface->create();
    threaded_gl = QOpenGLContext::supportsThreadedOpenGL();
    HarborSetup::MenuSetHost(*this);
    WatchChord();
    // A known pad that connects gets its saved mapping back: a look once a second, on the GUI
    // thread, where the input configuration changes.
    auto* pads = new QTimer(window);
    QObject::connect(pads, &QTimer::timeout, window, [this] { HarborSetup::CheckControllers(*this); });
    pads->start(1000);
}

HarborMenu::~HarborMenu() {
    auto& hid = QtCommon::system->HIDCore();
    if (player1_callback >= 0) {
        hid.GetEmulatedController(Core::HID::NpadIdType::Player1)->DeleteCallback(player1_callback);
    }
    if (handheld_callback >= 0) {
        hid.GetEmulatedController(Core::HID::NpadIdType::Handheld)->DeleteCallback(handheld_callback);
    }
    HarborSetup::MenuShutdown();
}

void HarborMenu::Toggle() {
    if (HarborSetup::MenuIsOpen()) {
        HarborSetup::MenuClose();
    } else {
        HarborSetup::MenuOpen();
    }
}

/// Plus and Minus together open the menu, as on Android. (The press also reaches the console, as
/// Eden's own controller hotkeys do; while the menu is open the controllers are its own.)
void HarborMenu::WatchChord() {
    auto& hid = QtCommon::system->HIDCore();
    const auto watch = [](Core::HID::EmulatedController* controller) {
        return controller->SetCallback({
            .on_change =
                [controller](Core::HID::ControllerTriggerType type) {
                    if (type != Core::HID::ControllerTriggerType::Button || controller->IsConfiguring()) {
                        return;
                    }
                    const auto buttons = controller->GetNpadButtons();
                    if (buttons.plus && buttons.minus && !HarborSetup::MenuIsOpen()) {
                        LOG_INFO(Frontend, "harbor: menu asked for by Plus+Minus");
                        HarborSetup::MenuOpen();
                    }
                },
            .is_npad_service = false,
        });
    };
    player1_callback = watch(hid.GetEmulatedController(Core::HID::NpadIdType::Player1));
    handheld_callback = watch(hid.GetEmulatedController(Core::HID::NpadIdType::Handheld));
}

std::unique_ptr<HarborSetup::MenuGl> HarborMenu::CreateMenuGl() {
    if (!gl_surface || !gl_surface->isValid()) {
        LOG_ERROR(Frontend, "harbor: menu: Qt made no offscreen surface");
        return nullptr;
    }
    if (!threaded_gl) {
        LOG_WARNING(Frontend, "harbor: menu: Qt reports no OpenGL off the GUI thread here; trying anyway");
    }
    // Made on this (the menu's) thread, so it belongs to it and may be made current here.
    auto context = std::make_unique<QOpenGLContext>();
    context->setFormat(MenuGlFormat());
    if (!context->create()) {
        LOG_ERROR(Frontend, "harbor: menu: Qt made no OpenGL context");
        return nullptr;
    }
    const auto format = context->format();
    if (format.renderableType() != QSurfaceFormat::OpenGL ||
        format.version() < qMakePair(3, 2)) {
        LOG_ERROR(Frontend, "harbor: menu: Qt gave OpenGL{} {}.{}, the menu needs OpenGL 3.2 or later",
                  format.renderableType() == QSurfaceFormat::OpenGLES ? " ES" : "",
                  format.majorVersion(), format.minorVersion());
        return nullptr;
    }
    if (!context->makeCurrent(gl_surface.get())) {
        LOG_ERROR(Frontend, "harbor: menu: the OpenGL context did not become current");
        return nullptr;
    }
    LOG_INFO(Frontend, "harbor: menu: Qt offscreen OpenGL {}.{} on {}", format.majorVersion(),
             format.minorVersion(), QGuiApplication::platformName().toStdString());
    return std::make_unique<QtMenuGl>(std::move(context));
}

void HarborMenu::OnGui(std::function<void()> work) {
    QMetaObject::invokeMethod(window, std::move(work), Qt::QueuedConnection);
}

Core::System& HarborMenu::System() {
    return *QtCommon::system;
}

InputCommon::InputSubsystem& HarborMenu::Input() {
    return input;
}

bool HarborMenu::PlatformDarkTheme() {
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void HarborMenu::SaveConfig() {
    OnGui([this] { hooks.save_config(); });
}

bool HarborMenu::JoyconsDetached() {
    return FrontendCommon::JoyconsDetached(QtCommon::system->HIDCore());
}

void HarborMenu::SetJoyconsDetached(bool detached) {
    OnGui([this, detached] { hooks.joycons(detached); });
}

void HarborMenu::Restart() {
    OnGui([this] { hooks.restart(); });
}

void HarborMenu::SetupAgain() {
    // The setup runs before Qt, in a new process (Borealis runs it once per process).
    OnGui([this] {
        LOG_INFO(Frontend, "harbor: set up again: starting the setup in a new process");
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {QStringLiteral("--harbor-setup")});
        hooks.exit();
    });
}

void HarborMenu::Exit() {
    OnGui([this] { hooks.exit(); });
}


bool HarborMenu::ShowFps() {
    return show_fps;
}

void HarborMenu::SetShowFps(bool show) {
    show_fps = show;
    WriteFlag("show_fps", show);
}

bool HarborMenu::ShowDeviceLine() {
    return show_device_line;
}

void HarborMenu::SetShowDeviceLine(bool show) {
    show_device_line = show;
    WriteFlag("show_device_line", show);
}

void HarborMenu::SetStats(double fps_, double speed_) {
    fps = fps_;
    speed = speed_;
}

/// "N FPS · speed N%", as on Android, and the GPU and its driver under it when chosen.
std::optional<std::string> HarborMenu::HudText() {
    auto& system = *QtCommon::system;
    std::string note;
    {
        std::scoped_lock lk{notice_mutex};
        if (!notice.empty() && std::chrono::steady_clock::now() < notice_until) {
            note = notice;
        }
    }
    if (!system.IsPoweredOn() || (!show_fps && note.empty())) {
        return std::nullopt;
    }
    std::string text;
    if (show_fps) {
        text = fmt::format("{:.0f} FPS · speed {:.0f}%", fps.load(), speed.load() * 100.0);
        if (show_device_line) {
            text += "\n" + system.GPU().Renderer().GetDeviceDescription();
        }
    }
    if (!note.empty()) {
        text += (text.empty() ? "" : "\n") + note;
    }
    return text;
}

void HarborMenu::HostNotice(const std::string& text) {
    LOG_INFO(Frontend, "harbor: notice: {}", text);
    std::scoped_lock lk{notice_mutex};
    notice = text;
    notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

bool HarborMenu::FrameGenSupported() {
#ifdef HAS_LSFG
    return HarborFrameGen::GpuSupported();
#else
    return false;
#endif
}

void HarborMenu::ChooseLosslessDll() {
#ifdef HAS_LSFG
    OnGui([this] { HarborFrameGen::ChooseDll(window, hooks.change_dll); });
#endif
}

void HarborMenu::RemoveLosslessDll() {
#ifdef HAS_LSFG
    OnGui([this] { HarborFrameGen::RemoveDll(hooks.change_dll, hooks.save_config); });
#endif
}

std::optional<HarborSetup::KeyboardDefaults> HarborMenu::DefaultKeyboard() {
    HarborSetup::KeyboardDefaults defaults;
    defaults.buttons = QtConfig::default_buttons;
    defaults.analogs = QtConfig::default_analogs;
    defaults.modifiers = QtConfig::default_stick_mod;
    return defaults;
}

std::string HarborMenu::KeyName(int code) {
    return QKeySequence(code).toString(QKeySequence::NativeText).toStdString();
}

void HarborMenu::ConsoleAction(const std::string& name) {
    OnGui([this, name] { hooks.console_action(name); });
}
