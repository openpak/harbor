// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Harbor menu and Harbor Settings, drawn with Borealis views on the 1280x720 design canvas, in
// the console's colour set (Basic White or Basic Black) like the setup wizard.

#include <algorithm>
#include <chrono>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/menu_view.h"
#include "harbor_setup/ui.h"
#include "harbor_setup/view.h"
#ifdef HAS_LSFG
#include "video_core/frame_gen/lossless_dll.h"
#endif

namespace HarborSetup {

namespace {

constexpr float CanvasWidth = 1280;
constexpr float CanvasHeight = 720;
constexpr float HeaderHeight = 68;
constexpr float FooterHeight = 61;
constexpr float CategoryWidth = 400;
constexpr int CaptureSeconds = 5;

const char* const StickNames[] = {"Left stick", "Right stick"};

// A switch like the console's: a rounded track with a knob, the accent colour when on.
void PaintSwitch(NVGcontext* vg, float x, float y, float w, float h, bool on, float alpha) {
    const auto& p = CurrentPalette();
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, h / 2);
    nvgFillColor(vg, WithAlpha(on ? p.accent : p.line, alpha));
    nvgFill(vg);
    nvgBeginPath(vg);
    const float r = h / 2 - 3;
    nvgCircle(vg, on ? x + w - h / 2 : x + h / 2, y + h / 2, r);
    nvgFillColor(vg, WithAlpha(p.selected, alpha));
    nvgFill(vg);
}

} // namespace

MenuActivity::MenuActivity(MenuHost& host_, std::function<void(std::function<void()>)> close_)
    : host{host_}, close{std::move(close_)}, mapping{host_, HarborInputStore()}, capture{host_.Input()} {}

MenuActivity::~MenuActivity() {
    brls::Application::getGlobalFocusChangeEvent()->unsubscribe(focus_subscription);
}

brls::View* MenuActivity::createContentView() {
    root = new brls::Box(brls::Axis::COLUMN);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setAlignItems(brls::AlignItems::CENTER);
    BindActions(root);
    focus_subscription = brls::Application::getGlobalFocusChangeEvent()->subscribe([this](brls::View* view) {
        if (rendering) {
            return;
        }
        const auto it = keys.find(view);
        if (it == keys.end()) {
            return;
        }
        focus_key = it->second;
        // Settings: the focused category shows its rows at once, as on the console.
        if (screen == Screen::Settings && sub == Sub::None && focus_key.starts_with("category:")) {
            const auto wanted = focus_key == "category:display"       ? Category::Display
                                : focus_key == "category:controllers" ? Category::Controllers
                                : focus_key == "category:touch"       ? Category::Touch
                                                                      : Category::Harbor;
            if (wanted != category) {
                category = wanted;
                render_pending = true;
            }
        }
    });
    return root;
}

void MenuActivity::onContentAvailable() {
    Render();
}

void MenuActivity::Show(MenuPage page) {
    capture.End();
    sub = Sub::None;
    focus_key.clear();
    switch (page) {
    case MenuPage::Menu:
        screen = Screen::Menu;
        break;
    case MenuPage::Settings:
        screen = Screen::Settings;
        break;
    case MenuPage::FrameGeneration:
        screen = Screen::Settings;
        category = Category::Display;
        focus_key = "category:display";
        break;
    case MenuPage::Controllers:
        screen = Screen::Settings;
        category = Category::Controllers;
        focus_key = "category:controllers";
        break;
    }
    Render();
}

bool MenuActivity::Capturing() const {
    return capture.Active();
}

void MenuActivity::Close(std::function<void()> then) {
    capture.End();
    close(std::move(then));
}

void MenuActivity::Back() {
    if (capture.Active()) {
        capture.End();
        Render();
        return;
    }
    if (screen == Screen::Menu) {
        Close();
        return;
    }
    if (sub == Sub::Choice) {
        sub = choice_return;
        focus_key = pane_return_key;
        Render();
        return;
    }
    if (sub == Sub::Player) {
        sub = Sub::None;
        focus_key = "player:" + std::to_string(player);
        Render();
        return;
    }
    if (!focus_key.starts_with("category:")) {
        // From the rows back to the categories.
        focus_key = category == Category::Display       ? "category:display"
                    : category == Category::Controllers ? "category:controllers"
                    : category == Category::Touch       ? "category:touch"
                                                        : "category:harbor";
        Render();
        return;
    }
    screen = Screen::Menu;
    focus_key = "menu:settings";
    Render();
}

void MenuActivity::Tick() {
    frames++;
    if (capture.Active()) {
        if (auto param = capture.Poll()) {
            auto done = std::move(capture_done);
            capture_done = nullptr;
            if (done) {
                done(*param);
            }
            Render();
            return;
        }
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(capture_deadline -
                                                                           std::chrono::steady_clock::now());
        if (left.count() < 0) {
            LOG_INFO(Frontend, "harbor: menu: nothing pressed, the mapping is unchanged");
            capture.End();
            Render();
            return;
        }
        if (static_cast<int>(left.count()) + 1 != capture_seconds || capture.StickStep() != capture_step) {
            if (capture.StickStep() != capture_step) {
                // Each direction of a stick from buttons gets its own seconds.
                capture_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(CaptureSeconds);
            }
            capture_seconds = static_cast<int>(left.count()) + 1;
            capture_step = capture.StickStep();
            render_pending = true;
        }
    }
    // The console's colour set can change in System Settings while the menu is closed or open.
    if (frames % 30 == 0) {
        const bool now = LiveConsoleDarkTheme(host.System())
                             .value_or(SavedConsoleDarkTheme().value_or(host.PlatformDarkTheme()));
        if (now != dark) {
            render_pending = true;
        }
    }
    if (std::exchange(render_pending, false)) {
        Render();
    }
}

void MenuActivity::BindActions(brls::View* target) {
    // Actions bubble from the focused view to the root: B goes back from anywhere.
    target->registerAction("Back", brls::BUTTON_B, [this](brls::View*) {
        Back();
        return true;
    }, true);
    // HOME leaves the menu, as it leaves a console's settings.
    target->registerAction("Close", brls::BUTTON_GUIDE, [this](brls::View*) {
        Close();
        return true;
    }, true);
    // X clears an assigned button or stick.
    target->registerAction("Clear", brls::BUTTON_X, [this](brls::View*) {
        if (screen != Screen::Settings) {
            return true;
        }
        if (sub == Sub::Player && focus_key.starts_with("button:")) {
            mapping.ClearButton(player, std::stoi(focus_key.substr(7)));
            Render();
        } else if (sub == Sub::Player && focus_key.starts_with("stick:")) {
            mapping.SetStick(player, std::stoi(focus_key.substr(6)), Common::ParamPackage{});
            Render();
        } else if (focus_key == "power") {
            mapping.ClearPower();
            Render();
        }
        return true;
    }, true);
}

void MenuActivity::Remember(brls::View* view, const std::string& key) {
    keys[view] = key;
    by_key[key] = view;
}

void MenuActivity::Render() {
    if (!root) {
        return;
    }
    rendering = true;
    dark = LiveConsoleDarkTheme(host.System()).value_or(SavedConsoleDarkTheme().value_or(host.PlatformDarkTheme()));
    if (dark != DarkTheme()) {
        SetDarkTheme(dark);
    }
    const auto& p = CurrentPalette();
    const float offset = pane_scroll ? pane_scroll->getContentOffsetY() : 0.0f;
    const bool keep_offset = screen == Screen::Settings;

    root->clearViews();
    root->setBackgroundColor(p.paper);
    keys.clear();
    by_key.clear();
    category_rows.clear();
    pane_rows.clear();
    pane_scroll = nullptr;
    capture_label = nullptr;

    auto* frame = new brls::Box(brls::Axis::COLUMN);
    frame->setDimensions(CanvasWidth, CanvasHeight);
    frame->setPadding(0, 32, 0, 32);
    root->addView(frame);
    if (screen == Screen::Menu) {
        frame->addView(Header("Harbor"));
        frame->addView(MenuBody());
    } else {
        frame->addView(Header("Harbor Settings"));
        frame->addView(SettingsBody());
    }
    frame->addView(Footer());

    // The categories and the pane reach each other sideways.
    if (!pane_rows.empty()) {
        for (auto* row : category_rows) {
            row->setCustomNavigationRoute(brls::FocusDirection::RIGHT, pane_rows.front());
        }
        brls::View* back = nullptr;
        for (auto* row : category_rows) {
            if (keys[row] == (category == Category::Display       ? "category:display"
                              : category == Category::Controllers ? "category:controllers"
                              : category == Category::Touch       ? "category:touch"
                                                                  : "category:harbor")) {
                back = row;
            }
        }
        for (auto* row : pane_rows) {
            if (back) {
                row->setCustomNavigationRoute(brls::FocusDirection::LEFT, back);
            }
        }
    }
    if (pane_scroll && keep_offset && offset > 0) {
        pane_scroll->setContentOffsetY(offset, false);
    }

    brls::View* focus = nullptr;
    if (const auto it = by_key.find(focus_key); it != by_key.end()) {
        focus = it->second;
    } else if (screen == Screen::Settings && sub == Sub::None && !category_rows.empty()) {
        // The current category, as the console opens its settings on the last one.
        const std::string current = category == Category::Display       ? "category:display"
                                    : category == Category::Controllers ? "category:controllers"
                                    : category == Category::Touch       ? "category:touch"
                                                                        : "category:harbor";
        focus = by_key.count(current) ? by_key[current] : category_rows.front();
    } else if (!pane_rows.empty()) {
        focus = pane_rows.front();
    }
    if (!focus && !category_rows.empty()) {
        focus = category_rows.front();
    }
    if (!focus && !pane_rows.empty()) {
        focus = pane_rows.front();
    }
    // Borealis keeps a pointer to the focused view: there is always a new one after a rebuild.
    if (focus) {
        brls::Application::giveFocus(focus);
        if (!capture.Active()) {
            focus_key = keys.count(focus) ? keys[focus] : "";
        }
    }
    rendering = false;
}

brls::View* MenuActivity::Header(const std::string& title_text) {
    const auto& p = CurrentPalette();
    auto* bar = new brls::Box(brls::Axis::ROW);
    bar->setHeight(HeaderHeight);
    bar->setPadding(0, 16, 0, 16);
    bar->setAlignItems(brls::AlignItems::CENTER);
    bar->setLineBottom(1);
    bar->setLineColor(p.rule);
    bar->addView(new Shape(28, 28, PaintEmblem));
    auto* title = MakeText(title_text, 24, p.ink);
    title->setMarginLeft(16);
    title->setGrow(1);
    bar->addView(title);
    return bar;
}

brls::View* MenuActivity::Footer() {
    const auto& p = CurrentPalette();
    auto* bar = new brls::Box(brls::Axis::ROW);
    bar->setHeight(FooterHeight);
    bar->setLineTop(1);
    bar->setLineColor(p.rule);
    bar->setPadding(8, 8, 8, 0);
    bar->setJustifyContent(brls::JustifyContent::FLEX_END);
    bar->setAlignItems(brls::AlignItems::CENTER);

    const auto hint = [&](const char* letter, const std::string& word, std::function<void()> action) {
        auto* box = new brls::Box(brls::Axis::ROW);
        box->setMinWidth(112);
        box->setHeight(44);
        box->setPadding(0, 12, 0, 12);
        box->setMarginLeft(4);
        box->setAlignItems(brls::AlignItems::CENTER);
        const std::string glyph = letter;
        box->addView(new Shape(24, 24, [glyph](NVGcontext* vg, float x, float y, float w, float h) {
            PaintButtonGlyph(vg, x, y, w, h, glyph, 1.0f);
        }));
        auto* text = MakeText(word, 17, p.ink);
        text->setMarginLeft(8);
        box->addView(text);
        box->addGestureRecognizer(new brls::TapGestureRecognizer([action](brls::TapGestureStatus status, brls::Sound*) {
            if (status.state == brls::GestureState::END) {
                action();
            }
        }));
        bar->addView(box);
    };
    if (capture.Active()) {
        hint("B", "Cancel", [this] { Back(); });
        return bar;
    }
    const bool clearable = screen == Screen::Settings &&
                           (sub == Sub::Player || (sub == Sub::None && category == Category::Controllers));
    if (clearable) {
        hint("X", "Clear", [] { brls::Application::onControllerButtonPressed(brls::BUTTON_X, false); });
    }
    hint("B", screen == Screen::Menu ? "Close" : "Back", [this] { Back(); });
    hint("A", "OK", [] { brls::Application::onControllerButtonPressed(brls::BUTTON_A, false); });
    return bar;
}

brls::Box* MenuActivity::Row(const std::string& key, const std::string& title, const std::string& value,
                             std::function<void()> action, bool enabled, bool chevron) {
    const auto& p = CurrentPalette();
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setMinHeight(60);
    row->setPadding(10, 20, 10, 20);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setLineBottom(1);
    row->setLineColor(p.line);
    row->setFocusable(true);
    row->setHighlightCornerRadius(0);
    row->setHighlightPadding(-3);
    if (enabled) {
        row->registerClickAction([action](brls::View*) {
            action();
            return true;
        });
    }
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
    auto* label = MakeText(title, 22, enabled ? p.ink : WithAlpha(p.ink, 0.4f));
    label->setGrow(1);
    label->setShrink(1);
    row->addView(label);
    if (!value.empty()) {
        auto* text = MakeText(value, 18, enabled ? p.accent : WithAlpha(p.accent, 0.4f));
        text->setMarginLeft(16);
        text->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        row->addView(text);
    }
    if (chevron) {
        auto* glyph = new Shape(14, 14, [](NVGcontext* vg, float x, float y, float w, float h) {
            PaintLineGlyph(vg, x, y, w, h, ChevronGlyph, CurrentPalette().muted);
        });
        glyph->setMarginLeft(12);
        row->addView(glyph);
    }
    Remember(row, key);
    pane_rows.push_back(row);
    return row;
}

brls::Box* MenuActivity::Toggle(const std::string& key, const std::string& title, bool on,
                                std::function<void(bool)> changed, bool enabled) {
    auto* row = Row(key, title, "", [this, on, changed] {
        changed(!on);
        Render();
    }, enabled);
    const float alpha = enabled ? 1.0f : 0.4f;
    auto* knob = new Shape(52, 28, [on, alpha](NVGcontext* vg, float x, float y, float w, float h) {
        PaintSwitch(vg, x, y, w, h, on, alpha);
    });
    knob->setMarginLeft(16);
    row->addView(knob);
    return row;
}

brls::Box* MenuActivity::Info(const std::string& key, const std::string& title, const std::string& value) {
    return Row(key, title, value, [] {});
}

brls::View* MenuActivity::Heading(const std::string& text) {
    auto* label = MakeText(text, 18, CurrentPalette().muted);
    label->setMargins(22, 20, 8, 20);
    return label;
}

brls::View* MenuActivity::Note(const std::string& text) {
    auto* label = MakeText(text, 16, CurrentPalette().ink2, true);
    label->setMargins(10, 20, 4, 20);
    return label;
}

// ---- The menu ----

brls::View* MenuActivity::MenuBody() {
    auto* body = new brls::Box(brls::Axis::COLUMN);
    body->setGrow(1);
    body->setAlignItems(brls::AlignItems::CENTER);
    body->setJustifyContent(brls::JustifyContent::CENTER);

    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setWidth(620);
    list->setLineTop(1);
    list->setLineColor(CurrentPalette().line);
    const bool detached = host.JoyconsDetached();
    list->addView(Row("menu:resume", "Resume", "", [this] { Close(); }));
    list->addView(Row("menu:joycons", detached ? "Attach Joy-Con" : "Detach Joy-Con", "", [this, detached] {
        Close([this, detached] { host.SetJoyconsDetached(!detached); });
    }));
    list->addView(Row("menu:restart", "Restart", "", [this] { Close([this] { host.Restart(); }); }));
    list->addView(Row("menu:settings", "Harbor Settings", "", [this] {
        screen = Screen::Settings;
        sub = Sub::None;
        focus_key.clear();
        Render();
    }, true, true));
    if (host.HasControllerHomeKey() && !host.ControllerHomeKeyEnabled()) {
        list->addView(Row("menu:home-key", "Controller HOME key", "Off", [this] {
            Close([this] { host.OpenControllerHomeKeySettings(); });
        }, true, true));
    }
    list->addView(Row("menu:threads", "Log threads", "", [this] {
        Close([this] { host.System().LogGuestThreads(); });
    }));
    list->addView(Row("menu:setup", "Set up again", "", [this] { Close([this] { host.SetupAgain(); }); }));
    list->addView(Row("menu:exit", "Exit", "", [this] { Close([this] { host.Exit(); }); }));
    body->addView(list);
    return body;
}

// ---- Settings ----

brls::View* MenuActivity::SettingsBody() {
    const auto& p = CurrentPalette();
    auto* body = new brls::Box(brls::Axis::ROW);
    body->setGrow(1);
    body->setClipsToBounds(true);

    auto* categories = new brls::Box(brls::Axis::COLUMN);
    categories->setWidth(CategoryWidth);
    categories->setPadding(24, 16, 24, 0);
    categories->setLineRight(1);
    categories->setLineColor(p.line);
    const auto add_category = [&](Category which, const std::string& key, const std::string& title) {
        const bool current = which == category;
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setHeight(60);
        row->setPadding(0, 16, 0, 0);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setFocusable(true);
        row->setHighlightCornerRadius(0);
        row->setHighlightPadding(-3);
        row->registerClickAction([this](brls::View*) {
            // A on a category goes to its rows, as on the console.
            if (!pane_rows.empty()) {
                brls::Application::giveFocus(pane_rows.front());
            }
            return true;
        });
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
        auto* bar = new brls::Box(brls::Axis::COLUMN);
        bar->setDimensions(4, 36);
        bar->setBackgroundColor(current ? p.accent : p.paper);
        row->addView(bar);
        auto* label = MakeText(title, 22, current ? p.accent : p.ink);
        label->setMarginLeft(16);
        row->addView(label);
        Remember(row, key);
        category_rows.push_back(row);
        categories->addView(row);
    };
    add_category(Category::Display, "category:display", "Display & Performance");
    add_category(Category::Controllers, "category:controllers", "Controllers and Input");
    if (host.HasTouchControls()) {
        add_category(Category::Touch, "category:touch", "Touch Controls");
    }
    add_category(Category::Harbor, "category:harbor", "Harbor");
    body->addView(categories);

    pane_scroll = new brls::ScrollingFrame();
    pane_scroll->setGrow(1);
    auto* pane = new brls::Box(brls::Axis::COLUMN);
    pane->setPadding(16, 40, 32, 40);
    if (capture.Active()) {
        pane->addView(CapturePanel());
    } else {
        CategoryPane(pane);
    }
    pane_scroll->setContentView(pane);
    body->addView(pane_scroll);
    return body;
}

void MenuActivity::CategoryPane(brls::Box* pane) {
    if (sub == Sub::Choice) {
        ChoicePane(pane);
        return;
    }
    if (sub == Sub::Player) {
        PlayerPane(pane);
        return;
    }
    switch (category) {
    case Category::Display:
        DisplayPane(pane);
        break;
    case Category::Controllers:
        ControllersPane(pane);
        break;
    case Category::Touch:
        TouchPane(pane);
        break;
    case Category::Harbor:
        HarborPane(pane);
        break;
    }
}

void MenuActivity::OpenChoice(Choice next) {
    choice = std::move(next);
    choice_return = sub;
    pane_return_key = focus_key;
    sub = Sub::Choice;
    focus_key = "choice:" + choice.current;
    Render();
}

void MenuActivity::ChoicePane(brls::Box* pane) {
    const auto& p = CurrentPalette();
    auto* title = MakeText(choice.title, 22, p.ink);
    title->setMargins(8, 20, 12, 20);
    pane->addView(title);
    for (const auto& [id, label] : choice.options) {
        const bool current = id == choice.current;
        auto* row = Row("choice:" + id, label, "", [this, id = id] {
            const auto chosen = choice.chosen;
            sub = choice_return;
            focus_key = pane_return_key;
            if (chosen) {
                chosen(id);
            }
            Render();
        });
        if (current) {
            auto* tick = new Shape(16, 16, [](NVGcontext* vg, float x, float y, float w, float h) {
                PaintLineGlyph(vg, x, y, w, h, CheckGlyph, CurrentPalette().accent);
            });
            tick->setMarginLeft(12);
            row->addView(tick);
        }
        pane->addView(row);
    }
}

void MenuActivity::DisplayPane(brls::Box* pane) {
    pane->addView(Toggle("fps", "Show FPS", host.ShowFps(), [this](bool on) { host.SetShowFps(on); }));
    if (host.HasDeviceLine()) {
        pane->addView(Toggle("device-line", "Show the device and driver under the FPS", host.ShowDeviceLine(),
                             [this](bool on) { host.SetShowDeviceLine(on); }, host.ShowFps()));
    }
    if (host.HasDriverChoice()) {
        const auto drivers = host.Drivers();
        const auto current = host.CurrentDriver();
        std::string current_name = "The phone's own";
        Choice next{.title = "Graphics driver", .current = current};
        for (const auto& driver : drivers) {
            const auto label = driver.detail.empty() ? driver.label : driver.label + " (" + driver.detail + ")";
            next.options.emplace_back(driver.id, label);
            if (driver.id == current) {
                current_name = driver.label;
            }
        }
        next.chosen = [this, current](const std::string& id) {
            if (id != current) {
                // A driver loads when the process starts: Harbor restarts into the console.
                Close([this, id] { host.ChooseDriver(id); });
            }
        };
        pane->addView(Row("driver", "Graphics driver", current_name, [this, next] { OpenChoice(next); }, true, true));
    }

    pane->addView(Heading("Frame generation (experimental)"));
#ifdef HAS_LSFG
    namespace FrameGen = VideoCore::FrameGen;
    if (!host.FrameGenSupported()) {
        pane->addView(Note("This GPU cannot run frame generation (it needs the Vulkan memory model, 16-bit "
                           "float shaders and null descriptors)."));
        return;
    }
    const auto status = FrameGen::GetInstalledLosslessStatus();
    if (status != FrameGen::LosslessStatus::Ok) {
        pane->addView(Note("Frame generation uses the shaders in your own copy of Lossless.dll, from Lossless "
                           "Scaling. Choose it to turn frame generation on."));
        pane->addView(Row("lossless:choose", "Choose Lossless.dll…", "", [this] {
            Close([this] { host.ChooseLosslessDll(); });
        }, true, true));
        return;
    }
    auto& v = Settings::values;
    const bool on = v.frame_gen.GetValue();
    pane->addView(Toggle("framegen", "Frame generation", on, [this](bool enable) {
        auto& values = Settings::values;
        values.frame_gen.SetValue(enable);
        if (enable && values.frame_gen_multiplier.GetValue() < 2) {
            values.frame_gen_multiplier.SetValue(2);
        }
        LOG_INFO(Frontend, "harbor: frame generation {}", enable ? "on" : "off");
        host.SaveConfig();
    }));
    const bool fixed = v.frame_gen_target_rate.GetValue() == 0;
    {
        Choice next{.title = "Frame multiplier", .current = std::to_string(v.frame_gen_multiplier.GetValue())};
        for (const int m : {2, 3, 4}) {
            next.options.emplace_back(std::to_string(m), std::to_string(m) + "x");
        }
        next.chosen = [this](const std::string& id) {
            Settings::values.frame_gen_multiplier.SetValue(static_cast<u32>(std::stoi(id)));
            Settings::values.frame_gen_target_rate.SetValue(0);
            host.SaveConfig();
        };
        pane->addView(Row("framegen:multiplier", "Frame multiplier",
                          std::to_string(v.frame_gen_multiplier.GetValue()) + "x",
                          [this, next] { OpenChoice(next); }, on && fixed, true));
    }
    {
        const auto target = v.frame_gen_target_rate.GetValue();
        Choice next{.title = "Target frame rate", .current = std::to_string(target)};
        next.options.emplace_back("0", "Use a fixed multiplier");
        for (const int rate : {60, 90, 120}) {
            next.options.emplace_back(std::to_string(rate), std::to_string(rate) + " FPS");
        }
        next.chosen = [this](const std::string& id) {
            Settings::values.frame_gen_target_rate.SetValue(static_cast<u32>(std::stoi(id)));
            host.SaveConfig();
        };
        pane->addView(Row("framegen:target", "Target frame rate",
                          target == 0 ? "Fixed multiplier" : std::to_string(target) + " FPS",
                          [this, next] { OpenChoice(next); }, on, true));
    }
    {
        const bool automatic = v.frame_gen_flow_scale_auto.GetValue();
        const auto scale = v.frame_gen_flow_scale.GetValue();
        Choice next{.title = "Motion estimation resolution",
                    .current = automatic ? std::string{"0"} : std::to_string(scale)};
        next.options.emplace_back("0", "Auto");
        for (const int value : {50, 75, 100}) {
            next.options.emplace_back(std::to_string(value), std::to_string(value) + "%");
        }
        next.chosen = [this](const std::string& id) {
            const int value = std::stoi(id);
            Settings::values.frame_gen_flow_scale_auto.SetValue(value == 0);
            if (value != 0) {
                Settings::values.frame_gen_flow_scale.SetValue(static_cast<u32>(value));
            }
            host.SaveConfig();
        };
        pane->addView(Row("framegen:flow", "Motion estimation resolution",
                          automatic ? "Auto" : std::to_string(scale) + "%",
                          [this, next] { OpenChoice(next); }, on, true));
    }
    pane->addView(Row("lossless:replace", "Replace Lossless.dll…", "", [this] {
        Close([this] { host.ChooseLosslessDll(); });
    }, true, true));
    pane->addView(Row("lossless:remove", "Remove Lossless.dll", "The console restarts", [this] {
        Close([this] { host.RemoveLosslessDll(); });
    }));
#else
    pane->addView(Note("This build of Harbor has no frame generation."));
#endif
}

void MenuActivity::ControllersPane(brls::Box* pane) {
    for (int i = 0; i < InputMapping::Players; i++) {
        const bool connected = mapping.Connected(i);
        std::string value = mapping.DeviceName(i);
        if (!connected) {
            value = "Not connected";
        }
        pane->addView(Row("player:" + std::to_string(i), "Player " + std::to_string(i + 1), value, [this, i] {
            player = i;
            sub = Sub::Player;
            focus_key.clear();
            Render();
        }, true, true));
    }
    pane->addView(Heading("Console buttons"));
    pane->addView(Row("power", "Power button", mapping.PowerText(), [this] {
        StartCapture("the Power button", InputCapture::Kind::Button,
                     [this](const Common::ParamPackage& param) { mapping.SetPower(param); });
    }));
    pane->addView(Note("A short press puts the console to sleep or wakes it; held for a second it opens the "
                       "power menu. X clears it."));
    if (host.HasControllerHomeKey()) {
        pane->addView(Row("home-key", "Controller HOME key",
                          host.ControllerHomeKeyEnabled() ? "On" : "Off",
                          [this] { Close([this] { host.OpenControllerHomeKeySettings(); }); }, true, true));
        pane->addView(Note("For controllers whose HOME button Android keeps: Harbor's accessibility service "
                           "passes it on."));
    }
}

void MenuActivity::PlayerPane(brls::Box* pane) {
    const auto& p = CurrentPalette();
    auto* title = MakeText("Player " + std::to_string(player + 1), 22, p.ink);
    title->setMargins(8, 20, 12, 20);
    pane->addView(title);

    {
        const auto devices = mapping.Devices();
        Choice next{.title = "Controller", .current = ""};
        const auto name = mapping.DeviceName(player);
        for (std::size_t i = 0; i < devices.size(); i++) {
            next.options.emplace_back(std::to_string(i), devices[i].name);
            if (devices[i].name == name) {
                next.current = std::to_string(i);
            }
        }
        next.chosen = [this, devices](const std::string& id) {
            const auto index = static_cast<std::size_t>(std::stoi(id));
            if (index < devices.size()) {
                mapping.UseDevice(player, devices[index]);
            }
        };
        pane->addView(Row("device", "Controller", name, [this, next] { OpenChoice(next); }, true, true));
    }
    if (player > 0) {
        pane->addView(Toggle("connected", "Connected", mapping.Connected(player),
                             [this](bool on) { mapping.SetConnected(player, on); }));
    }

    pane->addView(Heading("Buttons (A to assign, X to clear)"));
    for (const auto& button : MappableButtons) {
        const int index = button.index;
        const std::string name = button.name;
        pane->addView(Row("button:" + std::to_string(index), name, mapping.ButtonText(player, index), [this, index, name] {
            StartCapture(name, InputCapture::Kind::Button,
                         [this, index](const Common::ParamPackage& param) { mapping.SetButton(player, index, param); });
        }));
    }

    pane->addView(Heading("Sticks"));
    for (int stick = 0; stick < Settings::NativeAnalog::NumAnalogs; stick++) {
        const std::string name = StickNames[stick];
        pane->addView(Row("stick:" + std::to_string(stick), name, mapping.StickText(player, stick), [this, stick, name] {
            StartCapture(name, InputCapture::Kind::Stick,
                         [this, stick](const Common::ParamPackage& param) { mapping.SetStick(player, stick, param); });
        }));
        if (const auto deadzone = mapping.DeadZone(player, stick)) {
            Choice next{.title = name + " dead zone", .current = std::to_string(*deadzone / 5 * 5)};
            for (int value = 0; value <= 50; value += 5) {
                next.options.emplace_back(std::to_string(value), std::to_string(value) + "%");
            }
            next.chosen = [this, stick](const std::string& id) { mapping.SetDeadZone(player, stick, std::stoi(id)); };
            pane->addView(Row("deadzone:" + std::to_string(stick), name + " dead zone",
                              std::to_string(*deadzone) + "%", [this, next] { OpenChoice(next); }, true, true));
        }
        if (const auto invert = mapping.Invert(player, stick)) {
            const char* labels[] = {"None", "Horizontal", "Vertical", "Both"};
            Choice next{.title = name + " inversion", .current = std::to_string(static_cast<int>(*invert))};
            for (int value = 0; value < 4; value++) {
                next.options.emplace_back(std::to_string(value), labels[value]);
            }
            next.chosen = [this, stick](const std::string& id) {
                mapping.SetInvert(player, stick, static_cast<StickInvert>(std::stoi(id)));
            };
            pane->addView(Row("invert:" + std::to_string(stick), name + " inversion",
                              labels[static_cast<int>(*invert)], [this, next] { OpenChoice(next); }, true, true));
        }
    }

    pane->addView(Heading("Controller"));
    pane->addView(Row("defaults", "Restore defaults", "", [this] {
        mapping.RestoreDefaults(player);
        Render();
    }));
    pane->addView(Note("Changes are saved at once and kept for this controller: when it comes back, its "
                       "mapping comes back with it."));
}

void MenuActivity::TouchPane(brls::Box* pane) {
    const bool hidden = host.TouchControlsHidden();
    pane->addView(Toggle("touch:show", "Show touch controls", !hidden,
                         [this](bool on) { host.SetTouchControlsHidden(!on); }));
    pane->addView(Toggle("touch:power", "Show the power button", host.PowerControlShown(),
                         [this](bool on) { host.SetPowerControlShown(on); }, !hidden));
    {
        const int opacity = host.TouchOpacity();
        Choice next{.title = "Opacity", .current = std::to_string(std::clamp((opacity + 5) / 10 * 10, 20, 100))};
        for (int value = 20; value <= 100; value += 10) {
            next.options.emplace_back(std::to_string(value), std::to_string(value) + "%");
        }
        next.chosen = [this](const std::string& id) { host.SetTouchOpacity(std::stoi(id)); };
        pane->addView(Row("touch:opacity", "Opacity", std::to_string(opacity) + "%",
                          [this, next] { OpenChoice(next); }, !hidden, true));
    }
    {
        const int scale = host.TouchScale();
        Choice next{.title = "Size", .current = std::to_string(std::clamp((scale + 5) / 10 * 10, 10, 100))};
        for (int value = 10; value <= 100; value += 10) {
            next.options.emplace_back(std::to_string(value), std::to_string(value) + "%");
        }
        next.chosen = [this](const std::string& id) { host.SetTouchScale(std::stoi(id)); };
        pane->addView(Row("touch:size", "Size", std::to_string(scale) + "%", [this, next] { OpenChoice(next); },
                          !hidden, true));
    }
    pane->addView(Row("touch:edit", "Move the controls", "", [this] {
        Close([this] { host.EditTouchLayout(); });
    }, !hidden, true));
    pane->addView(Row("touch:reset", "Reset the layout", "", [this] {
        host.ResetTouchLayout();
        Render();
    }, !hidden));
}

void MenuActivity::HarborPane(brls::Box* pane) {
    pane->addView(Row("setup", "Set up again", "", [this] { Close([this] { host.SetupAgain(); }); }, true, true));
    pane->addView(Note("The setup runs again on the saved choices: keys, firmware, folders and drivers."));
    pane->addView(Heading("This console"));
    pane->addView(Row("console:backup", "Back up console…", "", [this] {
        Close([this] { host.ConsoleAction("backup"); });
    }, true, true));
    pane->addView(Row("console:restore", "Restore console…", "", [this] {
        Close([this] { host.ConsoleAction("restore"); });
    }, true, true));
    pane->addView(Row("console:new", "Make this a new console…", "", [this] {
        Close([this] { host.ConsoleAction("new-console"); });
    }, true, true));
    pane->addView(Note("A backup holds the console's system memory and its device key, locked with a "
                       "passphrase. The console stops while it is written or restored."));
    pane->addView(Toggle("crash-reports", "Send crash reports", host.CrashReportConsent().value_or(false),
                         [this](bool on) { host.SetCrashReportConsent(on); }));
    pane->addView(Note("When Harbor crashes it keeps a report with the end of its log, so the fault can be "
                       "found. Turning this off deletes the reports kept so far."));
    pane->addView(Heading("About"));
    pane->addView(Info("about:version", "Version", Common::g_build_version));
    pane->addView(Info("about:licence", "Licence", "AGPL-3.0-only"));
}

// ---- Assigning a button ----

void MenuActivity::StartCapture(const std::string& what, InputCapture::Kind kind,
                                std::function<void(const Common::ParamPackage&)> done) {
    capture_what = what;
    capture_done = std::move(done);
    capture_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(CaptureSeconds);
    capture_seconds = CaptureSeconds;
    capture_step = -1;
    capture.Begin(kind);
    LOG_INFO(Frontend, "harbor: menu: waiting for an input for {}", what);
    Render();
}

brls::View* MenuActivity::CapturePanel() {
    const auto& p = CurrentPalette();
    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setMarginTop(120);
    panel->setAlignItems(brls::AlignItems::CENTER);
    // The panel holds the focus while it waits (the rows behind it are gone), with no ring.
    panel->setFocusable(true);
    panel->setHideHighlight(true);
    Remember(panel, "capture");
    pane_rows.push_back(panel);
    std::string prompt = "Press the button for " + capture_what;
    if (const int step = capture.StickStep(); step >= 0) {
        const char* directions[] = {"up", "down", "left", "right"};
        prompt = capture_what + ": press the button for " + directions[step];
    } else if (capture_what.ends_with("stick")) {
        prompt = "Move the " + capture_what + " around, or press the button for up";
    }
    auto* title = MakeText(prompt, 24, p.ink, true);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    panel->addView(title);
    auto* sub_line = MakeText("on any controller or the keyboard", 18, p.ink2);
    sub_line->setMarginTop(10);
    panel->addView(sub_line);
    capture_label = MakeText(std::to_string(capture_seconds), 40, p.accent);
    capture_label->setMarginTop(28);
    panel->addView(capture_label);
    return panel;
}

} // namespace HarborSetup
