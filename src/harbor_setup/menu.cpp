// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The menu's thread: an offscreen GL context (EGL, the same on desktop Linux and Android), Borealis
// on a platform that draws into it and reads the console's own input, and the picture handed to
// the presenter (video_core/host_screen.h) once a frame while the menu is open.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include <dlfcn.h>

#define EGL_NO_X11
#define MESA_EGL_NO_X11_HEADERS
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <glad/glad.h>
#ifdef __ANDROID__
#define NANOVG_GLES2
#else
#define NANOVG_GL3
#endif
#include <borealis.hpp>
#include <borealis/platforms/desktop/desktop_platform.hpp>
#include <nanovg_gl.h>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "harbor_setup/menu.h"
#include "harbor_setup/menu_input.h"
#include "harbor_setup/menu_view.h"
#include "harbor_setup/view.h"
#include "hid_core/frontend/emulated_console.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "video_core/host_screen.h"
#include "common/harbor_crash.h"

// The text font (Noto Sans, OFL-1.1), embedded by CMake.
extern const unsigned char harbor_setup_font[];
extern const std::size_t harbor_setup_font_size;

namespace HarborSetup {

namespace {

namespace NativeButton = Settings::NativeButton;

// ---- The offscreen GL context ----

struct Egl {
    void* library = nullptr;
    void* gl_library = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    PFNEGLGETPROCADDRESSPROC get_proc = nullptr;
} egl;

template <typename T>
T EglFunction(const char* name) {
    return reinterpret_cast<T>(dlsym(egl.library, name));
}

void* GlProc(const char* name) {
    void* proc = egl.get_proc ? reinterpret_cast<void*>(egl.get_proc(name)) : nullptr;
    if (!proc && egl.gl_library) {
        proc = dlsym(egl.gl_library, name);
    }
    return proc;
}

bool CreateGlContext() {
#ifdef __ANDROID__
    egl.library = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    egl.gl_library = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
#else
    egl.library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    egl.gl_library = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!egl.gl_library) {
        egl.gl_library = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    }
#endif
    if (!egl.library) {
        LOG_ERROR(Frontend, "harbor: menu: no EGL library");
        return false;
    }
    const auto get_display = EglFunction<PFNEGLGETDISPLAYPROC>("eglGetDisplay");
    HARBOR_CRUMB("menu: EGL setup");
    const auto initialize = EglFunction<PFNEGLINITIALIZEPROC>("eglInitialize");
    const auto bind_api = EglFunction<PFNEGLBINDAPIPROC>("eglBindAPI");
    const auto choose_config = EglFunction<PFNEGLCHOOSECONFIGPROC>("eglChooseConfig");
    const auto create_pbuffer = EglFunction<PFNEGLCREATEPBUFFERSURFACEPROC>("eglCreatePbufferSurface");
    const auto create_context = EglFunction<PFNEGLCREATECONTEXTPROC>("eglCreateContext");
    const auto make_current = EglFunction<PFNEGLMAKECURRENTPROC>("eglMakeCurrent");
    const auto get_error = EglFunction<PFNEGLGETERRORPROC>("eglGetError");
    egl.get_proc = EglFunction<PFNEGLGETPROCADDRESSPROC>("eglGetProcAddress");
    if (!get_display || !initialize || !bind_api || !choose_config || !create_pbuffer ||
        !create_context || !make_current || !get_error || !egl.get_proc) {
        LOG_ERROR(Frontend, "harbor: menu: incomplete EGL library");
        return false;
    }

    // Offscreen needs no window system: Mesa's surfaceless platform first on desktop, then the
    // default display (Android, and drivers without it).
    EGLint major = 0, minor = 0;
    egl.display = EGL_NO_DISPLAY;
#ifndef __ANDROID__
    if (const auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            egl.get_proc("eglGetPlatformDisplayEXT"))) {
        egl.display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (egl.display != EGL_NO_DISPLAY && !initialize(egl.display, &major, &minor)) {
            LOG_WARNING(Frontend, "harbor: menu: no surfaceless EGL (0x{:x}), trying the default display",
                        get_error());
            egl.display = EGL_NO_DISPLAY;
        }
    }
#endif
    if (egl.display == EGL_NO_DISPLAY) {
        egl.display = get_display(EGL_DEFAULT_DISPLAY);
        if (egl.display == EGL_NO_DISPLAY || !initialize(egl.display, &major, &minor)) {
            LOG_ERROR(Frontend, "harbor: menu: EGL did not start (0x{:x})", get_error());
            egl.display = EGL_NO_DISPLAY;
            return false;
        }
    }
#ifdef __ANDROID__
    bind_api(EGL_OPENGL_ES_API);
    constexpr EGLint renderable = EGL_OPENGL_ES3_BIT_KHR;
#else
    bind_api(EGL_OPENGL_API);
    constexpr EGLint renderable = EGL_OPENGL_BIT;
#endif
    const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, renderable,
                                     EGL_RED_SIZE,     8,               EGL_GREEN_SIZE,      8,
                                     EGL_BLUE_SIZE,    8,               EGL_ALPHA_SIZE,      8,
                                     EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    if (!choose_config(egl.display, config_attribs, &config, 1, &count) || count < 1) {
        LOG_ERROR(Frontend, "harbor: menu: no EGL config (0x{:x})", get_error());
        return false;
    }
    const EGLint surface_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    egl.surface = create_pbuffer(egl.display, config, surface_attribs);
#ifdef __ANDROID__
    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
#else
    const EGLint context_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                      EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                      EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
#endif
    egl.context = create_context(egl.display, config, EGL_NO_CONTEXT, context_attribs);
    if (egl.surface == EGL_NO_SURFACE || egl.context == EGL_NO_CONTEXT ||
        !make_current(egl.display, egl.surface, egl.surface, egl.context)) {
        LOG_ERROR(Frontend, "harbor: menu: no offscreen GL context (0x{:x})", get_error());
        return false;
    }
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(GlProc))) {
        LOG_ERROR(Frontend, "harbor: menu: GL functions did not load");
        return false;
    }
    LOG_INFO(Frontend, "harbor: menu: offscreen GL {} (EGL {}.{})",
             reinterpret_cast<const char*>(glGetString(GL_VERSION)), major, minor);
    return true;
}

/// The picture the menu draws into, at the console's resolution.
struct Target {
    GLuint framebuffer = 0;
    GLuint colour = 0;
    GLuint depth_stencil = 0;
    int width = 0;
    int height = 0;

    void Resize(int w, int h) {
        if (w == width && h == height) {
            return;
        }
        if (framebuffer) {
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteRenderbuffers(1, &colour);
            glDeleteRenderbuffers(1, &depth_stencil);
        }
        width = w;
        height = h;
        glGenFramebuffers(1, &framebuffer);
        glGenRenderbuffers(1, &colour);
        glGenRenderbuffers(1, &depth_stencil);
        glBindRenderbuffer(GL_RENDERBUFFER, colour);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_stencil);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colour);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_stencil);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_stencil);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR(Frontend, "harbor: menu: the {}x{} picture is incomplete", w, h);
        }
    }
};

// ---- Borealis on the console's screen ----

class MenuVideo final : public VideoContext {
public:
    MenuVideo() {
#ifdef __ANDROID__
        vg = nvgCreateGLES2(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
#else
        vg = nvgCreateGL3(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
#endif
    }
    void clear(NVGcolor color) override {
        glClearColor(color.r, color.g, color.b, color.a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    void beginFrame() override {}
    void endFrame() override {}
    void resetState() override {
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
    }
    double getScaleFactor() override {
        return 1.0;
    }
    NVGcontext* getNVGContext() override {
        return vg;
    }
    void setSwapInterval(int) override {}

private:
    NVGcontext* vg = nullptr;
};

/// Borealis' input, filled once a frame from the console's controllers and touch screen.
class MenuInput final : public brls::InputManager {
public:
    brls::ControllerState state{};
    std::vector<brls::RawTouchState> touches;

    short getControllersConnectedCount() override {
        return 1;
    }
    void updateUnifiedControllerState(brls::ControllerState* out) override {
        for (int i = 0; i < brls::_BUTTON_MAX; i++) {
            out->buttons[i] = state.buttons[i];
        }
        for (int i = 0; i < brls::_AXES_MAX; i++) {
            out->axes[i] = state.axes[i];
        }
    }
    void updateControllerState(brls::ControllerState* out, int) override {
        updateUnifiedControllerState(out);
    }
    bool getKeyboardKeyState(brls::BrlsKeyboardScancode) override {
        return false;
    }
    void updateTouchStates(std::vector<brls::RawTouchState>* out) override {
        *out = touches;
    }
    void updateMouseStates(brls::RawMouseState*) override {}
    void sendRumble(unsigned short, unsigned short, unsigned short) override {}
    void sendRumble(unsigned short, unsigned short, unsigned short, unsigned short, unsigned short) override {}
};

class MenuPlatform final : public brls::DesktopPlatform {
public:
    std::string getName() override {
        return "Harbor";
    }
    void createWindow(std::string, uint32_t, uint32_t, float, float) override {}
    bool mainLoopIteration() override {
        return true;
    }
    brls::AudioPlayer* getAudioPlayer() override {
        return &audio;
    }
    VideoContext* getVideoContext() override {
        return &video;
    }
    brls::InputManager* getInputManager() override {
        return &input;
    }

    brls::NullAudioPlayer audio;
    MenuVideo video;
    MenuInput input;
};

MenuPlatform* platform = nullptr;

// ---- State shared with the frontend's threads ----

MenuHost* menu_host = nullptr;
std::mutex mutex;
std::condition_variable cv;
std::thread thread;
bool stopping = false;
bool want_open = false;
bool want_back = false;
bool page_pending = false;
MenuPage want_page = MenuPage::Menu;
std::atomic_bool open{false};
std::atomic_bool failed{false};

// ---- Input from the console ----

constexpr std::size_t ControllerCount = 9; // Players 1-8 and Handheld

/// Keys that drive the menu on the desktop keyboard (Eden's keyboard engine, port 0).
struct MenuKey {
    std::unique_ptr<Common::Input::InputDevice> device;
    std::atomic_bool pressed{false};
    brls::ControllerButton button;
};
std::vector<std::unique_ptr<MenuKey>> menu_keys;

void BindMenuKeys() {
    // Qt key codes: the desktop's keyboard engine names keys by them. Android keyboards reach the
    // menu through the controller mapping instead.
    if (menu_host->IsAndroid()) {
        return;
    }
    const std::pair<int, brls::ControllerButton> keys[] = {
        {0x01000004, brls::BUTTON_A},     // Return
        {0x01000005, brls::BUTTON_A},     // Enter (keypad)
        {0x20, brls::BUTTON_A},           // Space
        {0x01000000, brls::BUTTON_B},     // Escape
        {0x01000003, brls::BUTTON_B},     // Backspace
        {0x01000013, brls::BUTTON_UP},    // Up
        {0x01000015, brls::BUTTON_DOWN},  // Down
        {0x01000012, brls::BUTTON_LEFT},  // Left
        {0x01000014, brls::BUTTON_RIGHT}, // Right
        {0x58, brls::BUTTON_X},           // X
    };
    for (const auto& [code, button] : keys) {
        auto key = std::make_unique<MenuKey>();
        key->button = button;
        key->device = Common::Input::CreateInputDevice(
            Common::ParamPackage{InputCommon::GenerateKeyboardParam(code)});
        if (!key->device) {
            continue;
        }
        auto* raw = key.get();
        key->device->SetCallback({.on_change = [raw](const Common::Input::CallbackStatus& status) {
            raw->pressed = status.button_status.value;
        }});
        menu_keys.push_back(std::move(key));
    }
}

/// Buttons held when the menu opened (the Plus+Minus that opened it) count once released.
std::array<bool, brls::_BUTTON_MAX> held_at_open{};

void ReadInput(MenuActivity* activity) {
    auto& input = platform->input;
    brls::ControllerState state{};
    auto& hid = menu_host->System().HIDCore();
    bool plus = false;
    bool minus = false;
    for (std::size_t index = 0; index < ControllerCount; index++) {
        const auto* controller = hid.GetEmulatedControllerByIndex(index);
        if (!controller) {
            continue;
        }
        const auto buttons = controller->GetButtonsValues();
        const auto sticks = controller->GetSticksValues();
        const auto pressed = [&](int button) { return buttons[button].value; };
        // The console's own buttons, by name: A confirms, B goes back.
        state.buttons[brls::BUTTON_A] |= pressed(NativeButton::A);
        state.buttons[brls::BUTTON_B] |= pressed(NativeButton::B);
        state.buttons[brls::BUTTON_X] |= pressed(NativeButton::X);
        state.buttons[brls::BUTTON_Y] |= pressed(NativeButton::Y);
        state.buttons[brls::BUTTON_LB] |= pressed(NativeButton::L);
        state.buttons[brls::BUTTON_RB] |= pressed(NativeButton::R);
        state.buttons[brls::BUTTON_LT] |= pressed(NativeButton::ZL);
        state.buttons[brls::BUTTON_RT] |= pressed(NativeButton::ZR);
        state.buttons[brls::BUTTON_START] |= pressed(NativeButton::Plus);
        state.buttons[brls::BUTTON_BACK] |= pressed(NativeButton::Minus);
        state.buttons[brls::BUTTON_GUIDE] |= pressed(NativeButton::Home);
        state.buttons[brls::BUTTON_UP] |= pressed(NativeButton::DUp);
        state.buttons[brls::BUTTON_DOWN] |= pressed(NativeButton::DDown);
        state.buttons[brls::BUTTON_LEFT] |= pressed(NativeButton::DLeft);
        state.buttons[brls::BUTTON_RIGHT] |= pressed(NativeButton::DRight);
        const auto& left = sticks[Settings::NativeAnalog::LStick];
        state.buttons[brls::BUTTON_NAV_UP] |= left.y.value > 0.5f;
        state.buttons[brls::BUTTON_NAV_DOWN] |= left.y.value < -0.5f;
        state.buttons[brls::BUTTON_NAV_LEFT] |= left.x.value < -0.5f;
        state.buttons[brls::BUTTON_NAV_RIGHT] |= left.x.value > 0.5f;
        plus |= pressed(NativeButton::Plus);
        minus |= pressed(NativeButton::Minus);
    }
    for (const auto& key : menu_keys) {
        state.buttons[key->button] |= key->pressed.load();
    }
    state.buttons[brls::BUTTON_NAV_UP] |= state.buttons[brls::BUTTON_UP];
    state.buttons[brls::BUTTON_NAV_DOWN] |= state.buttons[brls::BUTTON_DOWN];
    state.buttons[brls::BUTTON_NAV_LEFT] |= state.buttons[brls::BUTTON_LEFT];
    state.buttons[brls::BUTTON_NAV_RIGHT] |= state.buttons[brls::BUTTON_RIGHT];

    for (int i = 0; i < brls::_BUTTON_MAX; i++) {
        if (held_at_open[i]) {
            held_at_open[i] = state.buttons[i];
            state.buttons[i] = false;
        }
    }
    // Assigning a button: the controllers belong to the capture.
    if (activity->Capturing()) {
        state = {};
        held_at_open.fill(true);
    } else if (plus && minus && !held_at_open[brls::BUTTON_START]) {
        // Plus and Minus together close the menu again, as they opened it.
        held_at_open.fill(true);
        state = {};
        MenuClose();
    }
    input.state = state;

    // The touch screen (and the desktop's mouse as touch), in the canvas' units.
    input.touches.clear();
    const auto touch = hid.GetEmulatedConsole()->GetHostTouch();
    for (const auto& finger : touch) {
        if (!finger.pressed) {
            continue;
        }
        brls::RawTouchState raw;
        raw.fingerId = static_cast<int>(finger.id);
        raw.pressed = true;
        raw.position.x = finger.position.x * brls::Application::contentWidth;
        raw.position.y = finger.position.y * brls::Application::contentHeight;
        input.touches.push_back(raw);
    }
}

/// While the menu is open the console gets no input: its controllers are in Eden's configuration
/// mode (Eden's own mapping dialog does the same) and the touch screen is the menu's.
void TakeConsoleInput(bool take) {
    auto& hid = menu_host->System().HIDCore();
    for (std::size_t index = 0; index < ControllerCount; index++) {
        if (auto* controller = hid.GetEmulatedControllerByIndex(index)) {
            take ? controller->EnableConfiguration() : controller->DisableConfiguration();
        }
    }
    hid.GetEmulatedConsole()->SetHostOwnsTouch(take);
}

std::pair<int, int> ConsoleResolution() {
    return Settings::IsDockedMode() ? std::pair{1920, 1080} : std::pair{1280, 720};
}

// ---- The thread ----

MenuActivity* activity = nullptr;
Target target;
std::function<void()> after_close;

bool InitBorealis() {
    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_EN_US;
    brls::Platform::factory = [] () -> brls::Platform* {
        platform = new MenuPlatform();
        return platform;
    };
    try {
        if (!brls::Application::init()) {
            LOG_ERROR(Frontend, "harbor: menu: Borealis did not start");
            return false;
        }
        brls::Application::createWindow("Harbor");
        brls::Application::loadFontFromMemory(brls::FONT_REGULAR, const_cast<unsigned char*>(harbor_setup_font),
                                              harbor_setup_font_size, false);
        brls::Application::setGlobalQuit(false);
        ApplyBorealisStyle();
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "harbor: menu: Borealis stopped: {}", e.what());
        return false;
    }
    BindMenuKeys();
    return true;
}

void CloseNow() {
    if (!open) {
        return;
    }
    open = false;
    VideoCore::HostScreen::Hide();
    TakeConsoleInput(false);
    LOG_INFO(Frontend, "harbor: menu closed");
    if (auto then = std::exchange(after_close, nullptr)) {
        then();
    }
}

void OpenNow(MenuPage page) {
    if (!open) {
        held_at_open.fill(true);
        TakeConsoleInput(true);
        open = true;
    }
    activity->Show(page);
    VideoCore::HostScreen::Show();
}

void DrawFrame() {
    const auto [width, height] = ConsoleResolution();
    if (target.width != width || target.height != height) {
        target.Resize(width, height);
        // At once (onWindowResized waits 100 ms): the design canvas scales to the new size.
        brls::Application::setWindowSize(width, height);
        FitCanvas();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, width, height);

    ReadInput(activity);
    activity->Tick();
    brls::Application::mainLoop();

    auto frame = std::make_shared<VideoCore::HostScreen::Frame>();
    frame->width = static_cast<u32>(width);
    frame->height = static_cast<u32>(height);
    frame->rgba.resize(static_cast<std::size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    std::vector<u8> rows(frame->rgba.size());
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    // GL's rows start at the bottom.
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; y++) {
        std::memcpy(frame->rgba.data() + static_cast<std::size_t>(y) * stride,
                    rows.data() + static_cast<std::size_t>(height - 1 - y) * stride, stride);
    }
    if (open) {
        VideoCore::HostScreen::Publish(std::move(frame));
    }
}

bool hud_shown = false;

/// The FPS line on the console's screen while the menu is closed (desktop): white text with a
/// shadow at the top left, on an otherwise clear picture laid over the console's.
void DrawHud() {
    const auto text = menu_host->HudText();
    if (!text) {
        if (hud_shown) {
            VideoCore::HostScreen::PublishOverlay(nullptr);
            hud_shown = false;
        }
        return;
    }
    const auto [width, height] = ConsoleResolution();
    target.Resize(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    NVGcontext* vg = platform->video.getNVGContext();
    const float scale = static_cast<float>(width) / 1280.0f;
    nvgBeginFrame(vg, static_cast<float>(width), static_cast<float>(height), 1.0f);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFontSize(vg, 17.0f * scale);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    float y = 10.0f * scale;
    std::size_t start = 0;
    while (start <= text->size()) {
        const std::size_t end = std::min(text->find('\n', start), text->size());
        const std::string line = text->substr(start, end - start);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 200));
        nvgText(vg, 13.0f * scale, y + 1.5f * scale, line.c_str(), nullptr);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
        nvgText(vg, 12.0f * scale, y, line.c_str(), nullptr);
        y += 22.0f * scale;
        start = end + 1;
    }
    nvgEndFrame(vg);

    auto frame = std::make_shared<VideoCore::HostScreen::Frame>();
    frame->width = static_cast<u32>(width);
    frame->height = static_cast<u32>(height);
    frame->rgba.resize(static_cast<std::size_t>(width) * height * 4);
    std::vector<u8> rows(frame->rgba.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    for (int row = 0; row < height; row++) {
        std::memcpy(frame->rgba.data() + static_cast<std::size_t>(row) * stride,
                    rows.data() + static_cast<std::size_t>(height - 1 - row) * stride, stride);
    }
    VideoCore::HostScreen::PublishOverlay(std::move(frame));
    hud_shown = true;
}

void Run() {
    HARBOR_CRUMB("menu: thread runs");
    Common::SetCurrentThreadName("HarborMenu");
    // No GL is retried at the next open; Borealis that would not start is not (it starts once).
    const bool gl = CreateGlContext();
    if (!gl || !InitBorealis()) {
        failed = !gl;
        std::scoped_lock lk{mutex};
        want_open = false;
        return;
    }
    activity = new MenuActivity(*menu_host, [](std::function<void()> then) {
        after_close = std::move(then);
        std::scoped_lock lk{mutex};
        want_open = false;
    });
    brls::Application::pushActivity(activity, brls::TransitionAnimation::NONE);
    LOG_INFO(Frontend, "harbor: menu ready");

    using Clock = std::chrono::steady_clock;
    constexpr auto FrameTime = std::chrono::microseconds(16'667);
    std::unique_lock lk{mutex};
    while (!stopping) {
        if (!want_open) {
            lk.unlock();
            CloseNow();
            if (menu_host->HasHud()) {
                DrawHud();
            }
            lk.lock();
            if (menu_host->HasHud()) {
                // The FPS line is redrawn twice a second.
                cv.wait_for(lk, std::chrono::milliseconds(500), [] { return stopping || want_open; });
            } else {
                cv.wait(lk, [] { return stopping || want_open; });
            }
            continue;
        }
        const auto page = want_page;
        const bool show_page = std::exchange(page_pending, false);
        const bool back = std::exchange(want_back, false);
        lk.unlock();
        const auto start = Clock::now();
        if (!open || show_page) {
            OpenNow(page);
        }
        if (back) {
            activity->Back();
        }
        try {
            menu_host->MenuThreadFrame([] { DrawFrame(); });
        } catch (const std::exception& e) {
            LOG_ERROR(Frontend, "harbor: menu: {}", e.what());
        }
        lk.lock();
        cv.wait_until(lk, start + FrameTime, [] { return stopping || want_back || !want_open; });
    }
}

} // namespace

void MenuSetHost(MenuHost& host) {
    HARBOR_CRUMB("menu: MenuSetHost");
    menu_host = &host;
    StartPowerKey(host.System());
    if (host.HasHud()) {
        // The FPS line is drawn by the menu's thread, so it starts now (closed).
        std::scoped_lock lk{mutex};
        thread = std::thread(Run);
    }
}

void MenuOpen(MenuPage page) {
    if (!menu_host) {
        LOG_WARNING(Frontend, "harbor: the menu cannot open here");
        return;
    }
    std::scoped_lock lk{mutex};
    if (failed && thread.joinable()) {
        // The last start found no GL: try again.
        thread.join();
        failed = false;
    }
    LOG_INFO(Frontend, "harbor: menu open ({})", static_cast<int>(page));
    want_open = true;
    want_page = page;
    page_pending = true;
    if (!thread.joinable()) {
        thread = std::thread(Run);
    }
    cv.notify_all();
}

void MenuClose() {
    std::scoped_lock lk{mutex};
    want_open = false;
    cv.notify_all();
}

bool MenuIsOpen() {
    std::scoped_lock lk{mutex};
    return want_open;
}

void MenuBack() {
    std::scoped_lock lk{mutex};
    if (want_open) {
        want_back = true;
        cv.notify_all();
    }
}

void MenuShutdown() {
    {
        std::scoped_lock lk{mutex};
        stopping = true;
        want_open = false;
        cv.notify_all();
    }
    if (thread.joinable()) {
        thread.join();
    }
}

} // namespace HarborSetup
