// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Harbor menu and Harbor Settings, drawn with Borealis views on the 1280x720 design canvas, in
// the console's colour set (Basic White or Basic Black) like the setup wizard.

#include <algorithm>
#include <chrono>
#include <cmath>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/openpak/session.h"
#include "harbor_setup/driver_recovery.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/menu_view.h"
#include "harbor_setup/title_settings.h"
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
    driver_cancel = true;
    if (driver_worker.joinable()) {
        driver_worker.join();
    }
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
    focus_key.clear();
    scope_game = false; // every opening starts on All games
    if (page == MenuPage::CrashReport) {
        sub = Sub::None;
        screen = Screen::Crash;
        if (Common::HarborCrash::GetConsent() == Common::HarborCrash::Consent::Unasked) {
            crash_view = CrashView::Consent;
            focus_key = "crash:decline"; // as the desktop's dialog had it: nothing kept unless chosen
            LOG_INFO(Frontend, "harbor: asking whether to keep crash reports");
        } else if (auto report = Common::HarborCrash::TakeOffer()) {
            crash_report = std::move(report);
            crash_view = CrashView::Offer;
            focus_key = "crash:keep-report";
            LOG_INFO(Frontend, "harbor: offering crash report {}", crash_report->name);
        } else {
            screen = Screen::Menu;
            Render();
            Close();
            return;
        }
        Render();
        return;
    }
    if (page == MenuPage::Status) {
        sub = Sub::None;
        screen = Screen::Host;
        host_page = {.kind = HostPageKind::Status, .text = MenuStatusText()};
        Render();
        return;
    }
    if (driver_busy) {
        // A driver download goes on while the menu is closed: it opens on its progress again.
        screen = Screen::Settings;
        category = Category::Display;
        sub = Sub::Downloads;
        Render();
        return;
    }
    sub = Sub::None;
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
    case MenuPage::Console:
        screen = Screen::Settings;
        category = Category::Harbor;
        focus_key = "console:new";
        break;
    case MenuPage::Drivers:
        screen = Screen::Settings;
        category = Category::Display;
        if (host.HasDriverChoice()) {
            sub = Sub::Driver;
            pane_return_key = "driver";
        } else {
            focus_key = "category:display";
        }
        break;
    case MenuPage::CrashReport:
        break; // handled above
    case MenuPage::Status:
        break; // handled above
    }
    Render();
}

void MenuActivity::ShowHost(const HostPage& page) {
    capture.End();
    screen = Screen::Host;
    sub = Sub::None;
    host_page = page;
    focus_key.clear();
    DriverStatus({}, Tone::None);
    Render();
}

void MenuActivity::HostPageNotice(const std::string& text, bool bad) {
    DriverStatus(text, bad ? Tone::Bad : Tone::Ok);
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
    if (dialog) {
        // B is the dialog's first button (Cancel, Close).
        const auto action = dialog->buttons.empty() ? std::function<void()>{} : dialog->buttons.front().second;
        CloseDialog();
        if (action) {
            action();
        }
        return;
    }
    if (screen == Screen::Host) {
        if (driver_busy) {
            return; // X cancels a download
        }
        switch (sub) {
        case Sub::Downloads:
            sub = Sub::Driver;
            focus_key = "driver:download";
            break;
        case Sub::Driver:
            sub = Sub::None;
            focus_key = "host:driver";
            break;
        case Sub::ConfirmPhone:
            sub = Sub::None;
            focus_key = "host:phone";
            break;
        default:
            host.HostPageAction("back", {});
            return;
        }
        Render();
        return;
    }
    if (screen == Screen::Crash) {
        if (crash_view == CrashView::Text) {
            crash_view = CrashView::Offer;
            focus_key = "crash:view";
            Render();
            return;
        }
        // Closed without an answer: the question comes again next time; until then nothing is kept.
        if (crash_view == CrashView::Consent) {
            LOG_INFO(Frontend, "harbor: crash-report question closed without an answer");
        }
        screen = Screen::Menu;
        Close();
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
    if (sub == Sub::Downloads) {
        if (driver_busy) {
            // Locked while a download runs: X cancels it.
            return;
        }
        sub = Sub::Driver;
        focus_key = "driver:download";
        Render();
        return;
    }
    if (sub == Sub::Driver) {
        sub = Sub::None;
        focus_key = "driver";
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
    std::vector<std::function<void()>> posts;
    std::string stage;
    {
        std::scoped_lock lk{driver_mutex};
        posts.swap(driver_posts);
        stage = driver_stage;
    }
    for (auto& post : posts) {
        post();
    }
    if (driver_label && driver_busy && driver_label->getFullText() != stage) {
        driver_label->setText(stage);
    }
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
    // HOME leaves the menu, as it leaves a console's settings (not while a driver downloads).
    target->registerAction("Close", brls::BUTTON_GUIDE, [this](brls::View*) {
        if (!driver_busy) {
            Close();
        }
        return true;
    }, true);
    // X clears an assigned button or stick, and cancels a driver download.
    target->registerAction("Clear", brls::BUTTON_X, [this](brls::View*) {
        if (driver_busy) {
            CancelDriverWork();
            return true;
        }
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
    driver_label = nullptr;

    auto* frame = new brls::Box(brls::Axis::COLUMN);
    frame->setDimensions(CanvasWidth, CanvasHeight);
    frame->setPadding(0, 32, 0, 32);
    if (dialog) {
        // The page behind a dialog is not drawn: the dialog stands alone on the dimmed paper.
        delete frame;
        root->setBackgroundColor(dark ? nvgRGB(0x10, 0x10, 0x10) : nvgRGB(0x9a, 0x9a, 0x9a));
        root->addView(DialogView());
        brls::View* focus = by_key.count(focus_key) ? by_key[focus_key] : nullptr;
        if (!focus && !pane_rows.empty()) {
            focus = pane_rows.front();
        }
        if (focus) {
            brls::Application::giveFocus(focus);
            focus_key = keys.count(focus) ? keys[focus] : "";
        }
        rendering = false;
        return;
    }
    root->addView(frame);
    if (screen == Screen::Menu) {
        frame->addView(Header("Harbor"));
        frame->addView(MenuBody());
    } else if (screen == Screen::Crash) {
        frame->addView(Header(CrashTitle()));
        frame->addView(CrashBody());
    } else if (screen == Screen::Host && sub == Sub::None && host_page.kind == HostPageKind::Status) {
        // As a console starts: black, Harbor's mark in the middle, a spinner and one line.
        root->setBackgroundColor(nvgRGB(0, 0, 0));
        frame->addView(HostBody());
    } else if (screen == Screen::Host) {
        frame->addView(Header(HostTitle()));
        frame->addView(HostBody());
    } else {
        frame->addView(Header("Harbor Settings"));
        frame->addView(SettingsBody());
    }
    if (!(screen == Screen::Host && sub == Sub::None && host_page.kind == HostPageKind::Status)) {
        frame->addView(Footer());
    }

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
            if (driver_busy && sub == Sub::Downloads) {
                // Locked while a driver downloads.
                row->setCustomNavigationRoute(brls::FocusDirection::LEFT, row);
            } else if (back) {
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
    if (driver_busy) {
        // Navigation is locked while a driver downloads; X cancels until it installs.
        if (driver_cancellable) {
            hint("X", "Cancel", [this] { CancelDriverWork(); });
        }
        return bar;
    }
    const bool clearable = screen == Screen::Settings &&
                           (sub == Sub::Player || (sub == Sub::None && category == Category::Controllers));
    if (clearable) {
        hint("X", "Clear", [] { brls::Application::onControllerButtonPressed(brls::BUTTON_X, false); });
    }
    if (screen == Screen::Host) {
        // B on a host page itself leaves Harbor on Android (as the system's back does).
        if (sub != Sub::None) {
            hint("B", "Back", [this] { Back(); });
        } else if (host.IsAndroid() && host_page.kind != HostPageKind::Status) {
            hint("B", "Exit", [this] { Back(); });
        }
        if (host_page.kind != HostPageKind::Status || sub != Sub::None) {
            hint("A", "OK", [] { brls::Application::onControllerButtonPressed(brls::BUTTON_A, false); });
        }
        return bar;
    }
    const bool closes = screen == Screen::Menu || (screen == Screen::Crash && crash_view != CrashView::Text);
    hint("B", closes ? "Close" : "Back", [this] { Back(); });
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

// ---- System dialogs ----

void MenuActivity::OpenDialog(Dialog next) {
    dialog = std::move(next);
    focus_key = "dialog:0";
    Render();
}

void MenuActivity::CloseDialog() {
    if (!dialog) {
        return;
    }
    focus_key = dialog->return_key;
    dialog.reset();
    Render();
}

brls::View* MenuActivity::DialogView() {
    const auto& p = CurrentPalette();
    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(720);
    panel->setBackgroundColor(p.selected);
    panel->setCornerRadius(8);
    auto* body = new brls::Box(brls::Axis::COLUMN);
    body->setPadding(40, 48, 36, 48);
    if (!dialog->title.empty()) {
        auto* title = MakeText(dialog->title, 24, p.ink, true);
        title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        body->addView(title);
    }
    auto* text = MakeText(dialog->text, 18, p.ink2, true);
    text->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    text->setMarginTop(dialog->title.empty() ? 0 : 18);
    body->addView(text);
    panel->addView(body);

    auto* buttons = new brls::Box(brls::Axis::ROW);
    buttons->setLineTop(1);
    buttons->setLineColor(p.line);
    std::vector<brls::View*> row;
    for (std::size_t i = 0; i < dialog->buttons.size(); i++) {
        auto* button = new brls::Box(brls::Axis::ROW);
        button->setGrow(1);
        button->setHeight(72);
        button->setJustifyContent(brls::JustifyContent::CENTER);
        button->setAlignItems(brls::AlignItems::CENTER);
        button->setFocusable(true);
        button->setHighlightCornerRadius(0);
        button->setHighlightPadding(-3);
        if (i > 0) {
            button->setLineLeft(1);
            button->setLineColor(p.line);
        }
        button->registerClickAction([this, i](brls::View*) {
            auto action = dialog->buttons[i].second;
            CloseDialog();
            if (action) {
                action();
            }
            return true;
        });
        button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
        button->addView(MakeText(dialog->buttons[i].first, 20, i + 1 == dialog->buttons.size() ? p.accent : p.ink));
        Remember(button, "dialog:" + std::to_string(i));
        pane_rows.push_back(button);
        row.push_back(button);
        buttons->addView(button);
    }
    for (std::size_t i = 0; i < row.size(); i++) {
        row[i]->setCustomNavigationRoute(brls::FocusDirection::UP, row[i]);
        row[i]->setCustomNavigationRoute(brls::FocusDirection::DOWN, row[i]);
    }
    panel->addView(buttons);

    auto* frame = new brls::Box(brls::Axis::COLUMN);
    frame->setDimensions(CanvasWidth, CanvasHeight);
    frame->setJustifyContent(brls::JustifyContent::CENTER);
    frame->setAlignItems(brls::AlignItems::CENTER);
    frame->addView(panel);
    return frame;
}

// ---- Host pages (outside the console) ----

std::string MenuActivity::HostTitle() const {
    switch (sub) {
    case Sub::Driver:
    case Sub::Downloads:
        return "Graphics driver";
    case Sub::ConfirmPhone:
        return "Use the phone's own driver";
    default:
        break;
    }
    switch (host_page.kind) {
    case HostPageKind::Status:
        return "Harbor";
    case HostPageKind::Stopped:
        return "Harbor stopped";
    case HostPageKind::CannotStart:
        return "Harbor cannot start";
    case HostPageKind::DriverFailed:
        return "The graphics driver couldn't be loaded";
    }
    return "Harbor";
}

void MenuActivity::DownloadRecommended() {
    Host* store = host.DriverHost();
    if (!store || driver_busy) {
        return;
    }
    if (!driver_device) {
        driver_device = host.DriverDevice();
    }
    const DeviceInfo device = *driver_device;
    DriverWork("Downloading the recommended driver", [this, store, device] {
        const auto result = InstallRecommendedDriver(*store, device, driver_cancel);
        DriverPost([this, result] {
            driver_busy = false;
            driver_cancellable = false;
            if (result.error.empty()) {
                LOG_INFO(Frontend, "harbor: host page: recommended driver {} installed; restarting into it",
                         result.id);
                host.HostPageAction("restart-with", result.id);
                return;
            }
            DriverStatus(result.error, Tone::Bad);
            focus_key = "host:recommended";
            Render();
        });
    });
}

brls::View* MenuActivity::HostBody() {
    const auto& p = CurrentPalette();

    if (sub == Sub::Driver || sub == Sub::Downloads) {
        pane_scroll = new brls::ScrollingFrame();
        pane_scroll->setGrow(1);
        auto* pane = new brls::Box(brls::Axis::COLUMN);
        pane->setPadding(16, 160, 32, 160);
        if (sub == Sub::Driver) {
            DriverPane(pane);
        } else {
            DownloadsPane(pane);
        }
        pane_scroll->setContentView(pane);
        return pane_scroll;
    }

    auto* body = new brls::Box(brls::Axis::COLUMN);
    body->setGrow(1);
    body->setAlignItems(brls::AlignItems::CENTER);
    body->setJustifyContent(brls::JustifyContent::CENTER);
    auto* column = new brls::Box(brls::Axis::COLUMN);
    column->setWidth(760);
    body->addView(column);

    if (host_page.kind == HostPageKind::Status) {
        column->setAlignItems(brls::AlignItems::CENTER);
        column->addView(new Shape(144, 144, PaintMark));
        // A spinner as the console's: dots on a ring, one lit, moving round.
        auto* spinner = new Shape(40, 40, [](NVGcontext* vg, float x, float y, float w, float h) {
            constexpr int Dots = 8;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            const int lit = static_cast<int>((ms / 110) % Dots);
            for (int i = 0; i < Dots; i++) {
                const float angle = static_cast<float>(i) * 2.0f * NVG_PI / Dots - NVG_PI / 2;
                const int age = (lit - i + Dots) % Dots;
                nvgBeginPath(vg);
                nvgCircle(vg, x + w / 2 + std::cos(angle) * w * 0.38f, y + h / 2 + std::sin(angle) * h * 0.38f,
                          w * 0.07f);
                nvgFillColor(vg, nvgRGBA(255, 255, 255, static_cast<unsigned char>(255 - age * 28)));
                nvgFill(vg);
            }
        });
        spinner->setMarginTop(56);
        column->addView(spinner);
        auto* label = MakeText(host_page.text, 18, nvgRGB(0xa0, 0xa0, 0xa0), true);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        label->setMarginTop(20);
        column->addView(label);
        // Nothing to choose: the column holds the focus (Borealis always has a focused view).
        column->setFocusable(true);
        column->setHideHighlight(true);
        Remember(column, "host:status");
        pane_rows.push_back(column);
        return body;
    }

    const auto paragraph = [&](const std::string& text, float size, NVGcolor colour, float top) {
        auto* label = MakeText(text, size, colour, true);
        label->setMargins(top, 20, 0, 20);
        column->addView(label);
    };
    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setMarginTop(24);
    list->setLineTop(1);
    list->setLineColor(p.line);
    const auto setup_and_exit = [&] {
        list->addView(Row("host:setup", "Set up again", "", [this] {
            host.HostPageAction("set-up-again", {});
        }));
        list->addView(Row("host:exit", "Exit", "", [this] { host.HostPageAction("exit", {}); }));
    };

    if (sub == Sub::ConfirmPhone) {
        paragraph("The phone's own driver may crash the HOME menu on this GPU. Harbor restarts with it; "
                  "choose another driver from the Harbor menu if it does.",
                  18, p.ink2, 0);
        list->addView(Row("host:phone-confirm", "Use the phone's own driver", "", [this] {
            host.HostPageAction("restart-with", "");
        }));
        list->addView(Row("host:phone-back", "Back", "", [this] { Back(); }));
        column->addView(list);
        return body;
    }

    if (host_page.kind == HostPageKind::DriverFailed) {
        paragraph(host_page.text, 20, p.ink, 0);
        paragraph("Choose the driver Harbor restarts with.", 18, p.ink2, 12);
        if (driver_busy) {
            auto* pane = new brls::Box(brls::Axis::COLUMN);
            DriverProgressPanel(pane);
            column->addView(pane);
            return body;
        }
        if (!driver_status.empty()) {
            paragraph(driver_status, 18, p.bad, 12);
        }
        for (const auto& driver : host.FittingDrivers()) {
            list->addView(Row("host:fit:" + driver.id, driver.label, "", [this, id = driver.id] {
                host.HostPageAction("restart-with", id);
            }));
        }
        if (!driver_device) {
            driver_device = host.DriverDevice();
        }
        if (host.DriverHost() && HasRecommendedDriver(*driver_device)) {
            list->addView(Row("host:recommended", "Download the recommended driver", "",
                              [this] { DownloadRecommended(); }));
        }
        list->addView(Row("host:phone", "Use the phone's own driver", "", [this] {
            sub = Sub::ConfirmPhone;
            focus_key = "host:phone-back";
            Render();
        }, true, true));
        setup_and_exit();
        column->addView(list);
        return body;
    }

    // Harbor stopped, Harbor cannot start.
    paragraph(host_page.text, 20, p.ink, 0);
    list->addView(Row("host:start", "Start again", "", [this] { host.HostPageAction("start-again", {}); }));
    if (host.HasDriverChoice()) {
        const auto current = host.CurrentDriver();
        std::string current_name = "The phone's own";
        for (const auto& driver : host.Drivers()) {
            if (driver.id == current) {
                current_name = driver.label;
            }
        }
        list->addView(Row("host:driver", "Graphics driver", current_name, [this] {
            sub = Sub::Driver;
            focus_key.clear();
            DriverStatus({}, Tone::None);
            Render();
        }, true, true));
    }
    setup_and_exit();
    column->addView(list);
    return body;
}

// ---- Crash reports ----

std::string MenuActivity::CrashTitle() const {
    switch (crash_view) {
    case CrashView::Consent:
        return "Crash reports";
    case CrashView::Offer:
        return "Harbor crashed last time";
    case CrashView::Text:
        return crash_report ? crash_report->name : "Crash report";
    }
    return "Crash reports";
}

brls::View* MenuActivity::CrashBody() {
    const auto& p = CurrentPalette();
    const std::string device = host.IsAndroid() ? "phone" : "computer";

    if (crash_view == CrashView::Text) {
        // The report's text (already scrubbed), in parts the pad and the touch screen scroll through.
        auto* frame = new brls::ScrollingFrame();
        frame->setGrow(1);
        auto* column = new brls::Box(brls::Axis::COLUMN);
        column->setPadding(16, 40, 24, 40);
        const std::string text = crash_report ? Common::HarborCrash::Read(*crash_report) : std::string{};
        constexpr std::size_t LinesPerPart = 16;
        constexpr std::size_t MaxLines = 4000;
        std::size_t start = 0, lines = 0, part = 0;
        std::string chunk;
        while (start < text.size() && lines < MaxLines) {
            const std::size_t end = std::min(text.find('\n', start), text.size());
            chunk += (chunk.empty() ? "" : "\n") + text.substr(start, end - start);
            start = end + 1;
            lines++;
            if (lines % LinesPerPart == 0 || start >= text.size() || lines == MaxLines) {
                auto* box = new brls::Box(brls::Axis::COLUMN);
                box->setFocusable(true);
                box->setHideHighlight(true);
                box->addView(MakeText(chunk, 15, p.ink, true));
                // Borealis measures a wrapped label one leading short of what it draws.
                box->setMarginBottom(6);
                Remember(box, "crash:text:" + std::to_string(part++));
                pane_rows.push_back(box);
                column->addView(box);
                chunk.clear();
            }
        }
        if (part == 0) {
            auto* box = new brls::Box(brls::Axis::COLUMN);
            box->setFocusable(true);
            box->setHideHighlight(true);
            box->addView(MakeText("The report could not be read.", 18, p.ink2, true));
            Remember(box, "crash:text:0");
            pane_rows.push_back(box);
            column->addView(box);
        }
        frame->setContentView(column);
        return frame;
    }

    auto* body = new brls::Box(brls::Axis::COLUMN);
    body->setGrow(1);
    body->setAlignItems(brls::AlignItems::CENTER);
    body->setJustifyContent(brls::JustifyContent::CENTER);
    auto* column = new brls::Box(brls::Axis::COLUMN);
    column->setWidth(760);
    const auto paragraph = [&](const std::string& text, float size, NVGcolor colour, float top) {
        auto* label = MakeText(text, size, colour, true);
        label->setMargins(top, 20, 0, 20);
        column->addView(label);
    };
    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setMarginTop(24);
    list->setLineTop(1);
    list->setLineColor(p.line);

    if (crash_view == CrashView::Consent) {
        paragraph("Keep crash reports on this " + device + "?", 24, p.ink, 0);
        paragraph("If Harbor or a game crashes, Harbor can save a report on this " + device +
                      ": what crashed and the last lines of its log, with account ids, names, tokens and "
                      "typed text removed. Nothing is sent anywhere. At the next start Harbor offers the "
                      "report: you can read it, " +
                      (host.IsAndroid() ? std::string("share it yourself") : std::string("save a copy to share it yourself")) +
                      ", or delete it.",
                  18, p.ink2, 14);
        const auto answer = [this](bool accepted) {
            return [this, accepted] {
                host.SetCrashReportConsent(accepted);
                LOG_INFO(Frontend, "harbor: crash reports {}", accepted ? "kept from now on" : "not kept");
                screen = Screen::Menu;
                Close();
            };
        };
        list->addView(Row("crash:keep", "Keep reports", "", answer(true)));
        list->addView(Row("crash:decline", "Don't keep", "", answer(false)));
        column->addView(list);
        auto* note = MakeText("Closing this asks again at the next start; until then nothing is kept. Harbor "
                              "Settings › Harbor changes the answer later.",
                              16, p.muted, true);
        note->setMargins(14, 20, 0, 20);
        column->addView(note);
    } else {
        const auto& report = *crash_report;
        paragraph("Harbor saved a crash report: " + report.summary + " (" + report.when + ").", 22, p.ink, 0);
        paragraph("It is kept on this " + device + " only; nothing has been sent. View it, " +
                      (host.IsAndroid() ? std::string("share it yourself") : std::string("save a copy to share it yourself")) +
                      ", or delete it.",
                  18, p.ink2, 14);
        list->addView(Row("crash:view", "View", "", [this] {
            crash_view = CrashView::Text;
            focus_key.clear();
            Render();
        }, true, true));
        list->addView(Row("crash:share", host.ShareCrashReportLabel(), "", [this] {
            const auto shared = *crash_report;
            LOG_INFO(Frontend, "harbor: crash report {} passed on by the user", shared.name);
            screen = Screen::Menu;
            Close([this, shared] { host.ShareCrashReport(shared); });
        }));
        list->addView(Row("crash:delete", "Delete", "", [this] {
            Common::HarborCrash::Delete(*crash_report);
            LOG_INFO(Frontend, "harbor: crash report {} deleted", crash_report->name);
            screen = Screen::Menu;
            Close();
        }));
        list->addView(Row("crash:keep-report", "Keep", "", [this] {
            screen = Screen::Menu;
            Close();
        }));
        column->addView(list);
    }
    body->addView(column);
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
    if (sub == Sub::Driver) {
        DriverPane(pane);
        return;
    }
    if (sub == Sub::Downloads) {
        DownloadsPane(pane);
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

namespace {

/// The scales the Resolution row offers, with what the console draws at in handheld and docked.
struct ScaleOption {
    Settings::ResolutionSetup setup;
    const char* label;
};
constexpr ScaleOption ScaleOptions[] = {
    {Settings::ResolutionSetup::Res1_2X, "0.5× (360p handheld / 540p docked)"},
    {Settings::ResolutionSetup::Res3_4X, "0.75× (540p handheld / 810p docked)"},
    {Settings::ResolutionSetup::Res1X, "1× (720p handheld / 1080p docked)"},
    {Settings::ResolutionSetup::Res3_2X, "1.5× (1080p handheld / 1620p docked)"},
    {Settings::ResolutionSetup::Res2X, "2× (1440p handheld / 2160p docked)"},
};

std::string ScaleLabel(Settings::ResolutionSetup setup) {
    for (const auto& option : ScaleOptions) {
        if (option.setup == setup) {
            return option.label;
        }
    }
    return std::string(Settings::CanonicalizeEnum(setup));
}

} // namespace

std::optional<u64> MenuActivity::ScopeTitle() const {
    if (!scope_game || !host.System().IsPoweredOn()) {
        return std::nullopt;
    }
    return TitleSettings::RunningTitle();
}

std::string MenuActivity::ScopedValue(Settings::BasicSetting& setting) const {
    // This game: what it runs with (its own value, else the console-wide one).
    return ScopeTitle() ? setting.ToString() : TitleSettings::GlobalValue(setting);
}

void MenuActivity::SetScoped(Settings::BasicSetting& setting, const std::string& value) {
    if (const auto title = ScopeTitle()) {
        TitleSettings::SetOverride(*title, setting, value);
        return;
    }
    TitleSettings::SetGlobalValue(setting, value);
    host.SaveConfig();
}

void MenuActivity::DisplayPane(brls::Box* pane) {
    const auto running = host.System().IsPoweredOn() ? TitleSettings::RunningTitle() : std::nullopt;
    if (!running) {
        scope_game = false;
    }
    if (running) {
        // While a game runs: the rows below change every game's values, or this game's own.
        const auto name = TitleSettings::TitleName(host.System(), *running);
        Choice next{.title = "Settings for", .current = scope_game ? "game" : "all"};
        next.options.emplace_back("all", "All games");
        next.options.emplace_back("game", "This game (" + name + ")");
        next.chosen = [this](const std::string& id) { scope_game = id == "game"; };
        pane->addView(Row("scope", "Settings for", scope_game ? "This game (" + name + ")" : "All games",
                          [this, next] { OpenChoice(next); }, true, true));
    }
    const auto title = ScopeTitle();
    // A row's value, marked when it is this game's own.
    const auto mark = [&](const Settings::BasicSetting& setting, std::string text) {
        if (title && TitleSettings::HasOverride(*title, setting)) {
            text += " (this game)";
        }
        return text;
    };
    bool any_override = false;
    if (running) {
        for (const auto* setting : TitleSettings::PerGame()) {
            any_override |= TitleSettings::HasOverride(*running, *setting);
        }
    }
    if (running && !title && any_override) {
        pane->addView(Note("This game has values of its own for some of these: Settings for, This game."));
    }

    pane->addView(Toggle("fps", "Show FPS", host.ShowFps(), [this](bool on) { host.SetShowFps(on); }));
    if (host.HasDeviceLine()) {
        pane->addView(Toggle("device-line", "Show the device and driver under the FPS", host.ShowDeviceLine(),
                             [this](bool on) { host.SetShowDeviceLine(on); }, host.ShowFps()));
    }

    // Resolution: the console's, fixed when it starts (Eden builds its images at that scale).
    {
        auto& setting = Settings::values.resolution_setup;
        const bool automatic = host.HasAutoResolution() && host.AutoResolution();
        const auto global = static_cast<Settings::ResolutionSetup>(std::stoll(TitleSettings::GlobalValue(setting)));
        Choice next{.title = "Resolution",
                    .current = automatic ? std::string{"auto"} : std::to_string(static_cast<int>(global))};
        if (host.HasAutoResolution()) {
            next.options.emplace_back("auto", "Automatic (follows the window)");
        }
        for (const auto& option : ScaleOptions) {
            next.options.emplace_back(std::to_string(static_cast<int>(option.setup)), option.label);
        }
        next.chosen = [this, current = next.current](const std::string& id) {
            if (id == current) {
                return;
            }
            if (id == "auto") {
                host.SetAutoResolution(true);
            } else {
                if (host.HasAutoResolution()) {
                    host.SetAutoResolution(false);
                }
                TitleSettings::SetGlobalValue(Settings::values.resolution_setup, id);
                host.SaveConfig();
            }
            LOG_INFO(Frontend, "harbor: resolution {} chosen; used from the console's next start",
                     id == "auto" ? "automatic" : id);
            restart_needed = host.System().IsPoweredOn();
        };
        pane->addView(Row("resolution", "Resolution", automatic ? "Automatic" : ScaleLabel(global),
                          [this, next] { OpenChoice(next); }, !title, true));
        pane->addView(Note(title ? "The resolution is the whole console's: it is set when the console starts, "
                                   "so it is the same for every game."
                                 : "Used from the console's next start (Restart)."));
    }

    // Asynchronous shaders: taken at each draw, so a change applies at once.
    {
        auto& setting = Settings::values.use_asynchronous_shaders;
        const bool on = ScopedValue(setting) == "true";
        pane->addView(Toggle("async", mark(setting, "Asynchronous shaders"), on, [this](bool enable) {
            SetScoped(Settings::values.use_asynchronous_shaders, enable ? "true" : "false");
            LOG_INFO(Frontend, "harbor: asynchronous shaders {}{}", enable ? "on" : "off",
                     ScopeTitle() ? " for this game" : "");
        }));
        pane->addView(Note("Draws without the shaders still being built, for fewer stutters, at the cost of "
                           "brief missing effects. Applies at once."));
    }
    if (restart_needed) {
        pane->addView(Row("display:restart", "Restart now", "", [this] {
            restart_needed = false;
            Close([this] { host.Restart(); });
        }));
    }
    if (title && any_override) {
        pane->addView(Row("display:defaults", "Use the default", "Every game's values", [this, title] {
            for (auto* setting : TitleSettings::PerGame()) {
                if (TitleSettings::HasOverride(*title, *setting)) {
                    TitleSettings::ClearOverride(*title, *setting);
                }
            }
            focus_key = "scope";
            Render();
        }));
    }

    if (host.HasDriverChoice()) {
        const auto current = host.CurrentDriver();
        std::string current_name = "The phone's own";
        for (const auto& driver : host.Drivers()) {
            if (driver.id == current) {
                current_name = driver.label;
            }
        }
        pane->addView(Row("driver", "Graphics driver", current_name, [this] {
            pane_return_key = "driver";
            sub = Sub::Driver;
            focus_key.clear();
            Render();
        }, !title, true));
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
        }, !title, true));
        return;
    }
    auto& v = Settings::values;
    const bool on = ScopedValue(v.frame_gen) == "true";
    const u32 multiplier = static_cast<u32>(std::stoul(ScopedValue(v.frame_gen_multiplier)));
    const u32 target = static_cast<u32>(std::stoul(ScopedValue(v.frame_gen_target_rate)));
    pane->addView(Toggle("framegen", mark(v.frame_gen, "Frame generation"), on, [this, multiplier](bool enable) {
        auto& values = Settings::values;
        SetScoped(values.frame_gen, enable ? "true" : "false");
        if (enable && multiplier < 2) {
            SetScoped(values.frame_gen_multiplier, "2");
        }
        LOG_INFO(Frontend, "harbor: frame generation {}{}", enable ? "on" : "off",
                 ScopeTitle() ? " for this game" : "");
    }));
    const bool fixed = target == 0;
    {
        Choice next{.title = "Frame multiplier", .current = std::to_string(multiplier)};
        for (const int m : {2, 3, 4}) {
            next.options.emplace_back(std::to_string(m), std::to_string(m) + "x");
        }
        next.chosen = [this](const std::string& id) {
            SetScoped(Settings::values.frame_gen_multiplier, id);
            SetScoped(Settings::values.frame_gen_target_rate, "0");
        };
        pane->addView(Row("framegen:multiplier", "Frame multiplier",
                          mark(v.frame_gen_multiplier, std::to_string(multiplier) + "x"),
                          [this, next] { OpenChoice(next); }, on && fixed, true));
    }
    {
        Choice next{.title = "Target frame rate", .current = std::to_string(target)};
        next.options.emplace_back("0", "Use a fixed multiplier");
        for (const int rate : {60, 90, 120}) {
            next.options.emplace_back(std::to_string(rate), std::to_string(rate) + " FPS");
        }
        next.chosen = [this](const std::string& id) { SetScoped(Settings::values.frame_gen_target_rate, id); };
        pane->addView(Row("framegen:target", "Target frame rate",
                          mark(v.frame_gen_target_rate, target == 0 ? "Fixed multiplier" : std::to_string(target) + " FPS"),
                          [this, next] { OpenChoice(next); }, on, true));
    }
    {
        const bool automatic = ScopedValue(v.frame_gen_flow_scale_auto) == "true";
        const auto scale = ScopedValue(v.frame_gen_flow_scale);
        Choice next{.title = "Motion estimation resolution", .current = automatic ? std::string{"0"} : scale};
        next.options.emplace_back("0", "Auto");
        for (const int value : {50, 75, 100}) {
            next.options.emplace_back(std::to_string(value), std::to_string(value) + "%");
        }
        next.chosen = [this](const std::string& id) {
            SetScoped(Settings::values.frame_gen_flow_scale_auto, id == "0" ? "true" : "false");
            if (id != "0") {
                SetScoped(Settings::values.frame_gen_flow_scale, id);
            }
        };
        pane->addView(Row("framegen:flow", "Motion estimation resolution",
                          mark(v.frame_gen_flow_scale_auto, automatic ? "Auto" : scale + "%"),
                          [this, next] { OpenChoice(next); }, on, true));
    }
    pane->addView(Row("lossless:replace", "Replace Lossless.dll…", "", [this] {
        Close([this] { host.ChooseLosslessDll(); });
    }, !title, true));
    pane->addView(Row("lossless:remove", "Remove Lossless.dll", "The console restarts", [this] {
        Close([this] { host.RemoveLosslessDll(); });
    }, !title));
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
                           "passes it on. Android's accessibility settings open: turn on \"Harbor controller "
                           "HOME key\" there, then go back to Harbor."));
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
    if (OpenPak::Session::DeviceIdInUse()) {
        pane->addView(Note("Another console is using this console's identity, so OpenPak refused this "
                           "one. A copy of this console (its system memory on another computer, or a "
                           "second copy here) is the usual cause: make this one a new console and its "
                           "users link their Nintendo Accounts again. If you only restarted Harbor, "
                           "wait up to a minute: Harbor tries to sign in again by itself."));
    }
    pane->addView(Row("console:backup", "Back up console…", "", [this] {
        Close([this] { host.ConsoleAction("backup"); });
    }, true, true));
    pane->addView(Row("console:restore", "Restore console…", "", [this] {
        Close([this] { host.ConsoleAction("restore"); });
    }, true, true));
    pane->addView(Row("console:new", "Make this a new console…", "", [this] {
        OpenDialog({
            .title = "Make this a new console",
            .text = "This console gets a new identity: a new serial number, device id, network addresses "
                    "and keys. Its OpenPak sign-ins are dropped, so every user links their Nintendo "
                    "Account again. Users, games and saves stay.\n\nThe console stops while this is done "
                    "and starts again.",
            .buttons = {{"Cancel", {}},
                        {"Make a new console",
                         [this] {
                             LOG_INFO(Frontend, "harbor: menu: making this a new console");
                             Close([this] { host.ConsoleAction("new-console"); });
                         }}},
            .return_key = "console:new",
        });
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

// ---- Graphics drivers ----

void MenuActivity::DriverPane(brls::Box* pane) {
    const auto& p = CurrentPalette();
    auto* title = MakeText("Graphics driver", 22, p.ink);
    title->setMargins(8, 20, 12, 20);
    pane->addView(title);
    const auto current = host.CurrentDriver();
    for (const auto& driver : host.Drivers()) {
        const auto label = driver.detail.empty() ? driver.label : driver.label + " (" + driver.detail + ")";
        auto* row = Row("driver:" + driver.id, label, "", [this, id = driver.id, current] {
            if (screen == Screen::Host) {
                // Outside the console: saved for the next start (Start again starts a new process).
                std::string error;
                if (host.DriverHost() && !host.DriverHost()->SelectDriver(id, &error)) {
                    DriverStatus("The choice could not be saved.", Tone::Bad);
                } else {
                    LOG_INFO(Frontend, "harbor: host page: driver {} chosen for the next start",
                             id.empty() ? "of the phone" : id);
                    DriverStatus("Saved. The console uses it from its next start.", Tone::Ok);
                    if (id != current) {
                        host.HostPageAction("driver-changed", id);
                    }
                }
                focus_key = "driver:" + id;
                Render();
                return;
            }
            if (id != current) {
                // A driver loads when the process starts: Harbor restarts into the console.
                Close([this, id] { host.ChooseDriver(id); });
            }
        });
        if (driver.id == current) {
            auto* tick = new Shape(16, 16, [](NVGcontext* vg, float x, float y, float w, float h) {
                PaintLineGlyph(vg, x, y, w, h, CheckGlyph, CurrentPalette().accent);
            });
            tick->setMarginLeft(12);
            row->addView(tick);
        }
        pane->addView(row);
    }
    if (host.DriverHost()) {
        pane->addView(Row("driver:download", "Download a driver…", "", [this] { OpenDownloads(); }, true, true));
    }
    if (screen == Screen::Host && host.IsAndroid()) {
        // The system's file picker (Android's own), then the same checks as the setup's.
        pane->addView(Row("driver:file", "Install a driver file…", "", [this] {
            host.HostPageAction("install-file", {});
        }));
    }
    if (screen == Screen::Host) {
        if (!driver_status.empty()) {
            const auto& palette = CurrentPalette();
            auto* status = MakeText(driver_status, 18, driver_tone == Tone::Bad ? palette.bad : palette.ok, true);
            status->setMargins(12, 20, 4, 20);
            pane->addView(status);
        }
        pane->addView(Note("The Vulkan driver the console starts with. Start with the phone's driver; try an "
                           "installed Turnip driver when something draws wrong. A choice made here is used "
                           "from the console's next start."));
        return;
    }
    pane->addView(Note("The Vulkan driver the console starts with. Choosing an installed driver restarts "
                       "Harbor with it; a downloaded driver is chosen and used from the console's next "
                       "start."));
}

bool MenuActivity::CanDownloadDrivers() {
    if (!host.DriverHost()) {
        return false;
    }
    if (!driver_device) {
        driver_device = host.DriverDevice();
    }
    return CanBrowseDrivers(*driver_device);
}

void MenuActivity::OpenDownloads() {
    sub = Sub::Downloads;
    focus_key.clear();
    // The catalogue is fetched when the page is first opened, never before (G4.GPU.3).
    if (!driver_listed && !driver_busy && CanDownloadDrivers()) {
        BrowseDrivers();
        return;
    }
    Render();
}

void MenuActivity::DownloadsPane(brls::Box* pane) {
    const auto& p = CurrentPalette();
    auto* title = MakeText("Download a driver", 22, p.ink);
    title->setMargins(8, 20, 4, 20);
    pane->addView(title);
    const bool can = CanDownloadDrivers();
    pane->addView(Note(DriverDownloadLead(driver_device.value_or(DeviceInfo{}))));

    if (driver_busy) {
        DriverProgressPanel(pane);
        return;
    }

    if (!driver_status.empty()) {
        const NVGcolor colour = driver_tone == Tone::Bad ? p.bad : driver_tone == Tone::Warn ? p.warn : p.ok;
        auto* status = MakeText(driver_status, 18, colour, true);
        status->setMargins(12, 20, 4, 20);
        pane->addView(status);
    }
    if (!driver_installed.empty()) {
        if (screen == Screen::Host) {
            pane->addView(Row("download:restart", "Start again", "", [this] {
                host.HostPageAction("start-again", {});
            }));
        } else {
            pane->addView(Row("download:restart", "Restart now", "", [this, id = driver_installed] {
                Close([this, id] { host.ChooseDriver(id); });
            }));
        }
    }
    pane->addView(Row("download:browse", driver_listed ? "Refresh GitHub drivers" : "Browse GitHub drivers", "",
                      [this] { BrowseDrivers(); }, can));
    if (driver_releases.empty()) {
        return;
    }
    pane->addView(Heading("Available on GitHub"));
    for (std::size_t i = 0; i < driver_releases.size(); i++) {
        const auto& release = driver_releases[i];
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setMinHeight(64);
        row->setPadding(10, 20, 10, 20);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setLineBottom(1);
        row->setLineColor(p.line);
        row->setFocusable(true);
        row->setHighlightCornerRadius(0);
        row->setHighlightPadding(-3);
        row->registerClickAction([this, i](brls::View*) {
            DownloadDriver(i);
            return true;
        });
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
        auto* text = new brls::Box(brls::Axis::COLUMN);
        text->setGrow(1);
        text->setShrink(1);
        auto* name = MakeText(release.title, 20, release.recommended ? p.accent : p.ink, true);
        text->addView(name);
        auto* detail = MakeText(release.Detail(), 15, p.ink2, true);
        detail->setMarginTop(2);
        text->addView(detail);
        auto* file = MakeText(release.file_name, 14, p.muted, true);
        text->addView(file);
        row->addView(text);
        auto* action = MakeText("Download", 18, p.accent);
        action->setMarginLeft(12);
        row->addView(action);
        Remember(row, "download:" + release.download_url);
        pane_rows.push_back(row);
        pane->addView(row);
    }
}

void MenuActivity::DriverProgressPanel(brls::Box* pane) {
    const auto& p = CurrentPalette();
    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setMargins(24, 20, 16, 20);
    std::string stage;
    {
        std::scoped_lock lk{driver_mutex};
        stage = driver_stage;
    }
    driver_label = MakeText(stage, 18, p.ink, true);
    panel->addView(driver_label);
    auto* bar = new Shape(640, 8, [this](NVGcontext* vg, float x, float y, float w, float h) {
        const auto& palette = CurrentPalette();
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, w, h, h / 2);
        nvgFillColor(vg, palette.line);
        nvgFill(vg);
        const double fraction = driver_fraction.load();
        float start = 0, width = 0;
        if (fraction >= 0) {
            width = w * static_cast<float>(std::clamp(fraction, 0.0, 1.0));
        } else {
            // The length is not known yet: a block sweeps along the track.
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            width = w / 4;
            start = (w - width) * static_cast<float>((ms % 1600) / 1600.0);
        }
        if (width > 0) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + start, y, width, h, h / 2);
            nvgFillColor(vg, palette.accent);
            nvgFill(vg);
        }
    });
    bar->setMarginTop(14);
    panel->addView(bar);
    pane->addView(panel);
    if (driver_cancellable) {
        pane->addView(Row("download:cancel", "Cancel", "", [this] { CancelDriverWork(); }));
    } else {
        // Installing: nothing to choose, the panel holds the focus.
        panel->setFocusable(true);
        panel->setHideHighlight(true);
        Remember(panel, "download:busy");
        pane_rows.push_back(panel);
    }
}

void MenuActivity::DriverPost(std::function<void()> done) {
    std::scoped_lock lk{driver_mutex};
    driver_posts.push_back(std::move(done));
}

void MenuActivity::DriverProgress(std::string stage, std::optional<double> fraction) {
    driver_fraction = fraction.value_or(-1.0);
    std::scoped_lock lk{driver_mutex};
    driver_stage = std::move(stage);
}

void MenuActivity::DriverStatus(std::string text, Tone tone) {
    driver_status = std::move(text);
    driver_tone = driver_status.empty() ? Tone::None : tone;
}

void MenuActivity::DriverWork(std::string stage, std::function<void()> work) {
    if (driver_worker.joinable()) {
        driver_worker.join();
    }
    driver_busy = true;
    driver_cancel = false;
    driver_cancellable = true;
    DriverProgress(std::move(stage), std::nullopt);
    DriverStatus({}, Tone::None);
    driver_worker = std::thread(std::move(work));
    focus_key = "download:cancel";
    Render();
}

void MenuActivity::CancelDriverWork() {
    if (driver_busy && driver_cancellable) {
        LOG_INFO(Frontend, "harbor: menu: driver download cancelled by the user");
        driver_cancel = true;
    }
}

void MenuActivity::BrowseDrivers() {
    Host* store = host.DriverHost();
    if (!store || driver_busy || !CanDownloadDrivers()) {
        return;
    }
    const DeviceInfo device = *driver_device;
    DriverWork("Checking GitHub releases", [this, store, device] {
        auto listing = FetchDriverListing(*store, device, driver_cancel);
        const bool cancelled = driver_cancel.load();
        DriverPost([this, listing = std::move(listing), cancelled] {
            driver_busy = false;
            driver_cancellable = false;
            if (cancelled) {
                DriverStatus("Cancelled. Your selected driver is unchanged.", Tone::Warn);
            } else {
                driver_releases = listing.releases;
                driver_listed = true;
                if (listing.notice) {
                    DriverStatus(*listing.notice, Tone::Warn);
                } else if (driver_releases.empty()) {
                    DriverStatus("No matching releases were found. You can keep the phone's driver.", Tone::Warn);
                }
            }
            focus_key = driver_releases.empty() ? "download:browse" : "download:" + driver_releases.front().download_url;
            Render();
        });
    });
}

void MenuActivity::DownloadDriver(std::size_t index) {
    Host* store = host.DriverHost();
    if (!store || driver_busy || index >= driver_releases.size() || !driver_device) {
        return;
    }
    const DriverRelease release = driver_releases[index];
    const DeviceInfo device = *driver_device;
    DriverWork("Downloading " + release.file_name, [this, store, device, release] {
        auto result = HarborSetup::DownloadDriver(
            *store, device, release,
            [this, &release](std::uint64_t received) {
                DriverProgress(DownloadProgressText(release, received),
                               static_cast<double>(received) / static_cast<double>(release.size));
            },
            [this, &release] {
                driver_cancellable = false;
                DriverProgress("Installing " + release.file_name, std::nullopt);
                DriverPost([this] { Render(); }); // the Cancel row goes
            },
            driver_cancel);
        std::string select_error;
        const bool selected = result.installed && store->SelectDriver(result.installed->id, &select_error);
        DriverPost([this, result = std::move(result), selected, select_error] {
            driver_busy = false;
            driver_cancellable = false;
            if (result.installed && selected) {
                LOG_INFO(Frontend, "harbor: menu: driver {} downloaded and chosen for the next start",
                         result.installed->id);
                driver_installed = result.installed->id;
                if (screen == Screen::Host) {
                    host.HostPageAction("driver-changed", driver_installed);
                }
                DriverStatus(result.installed->name + " is installed and chosen. The console uses it from its next "
                                                      "start.",
                             Tone::Ok);
                focus_key = "download:restart";
            } else if (result.installed) {
                DriverStatus(result.installed->name + " is installed, but it could not be chosen: " +
                                 (select_error.empty() ? "the choice could not be saved." : select_error),
                             Tone::Bad);
                focus_key = "download:browse";
            } else {
                DriverStatus(result.error, result.cancelled ? Tone::Warn : Tone::Bad);
                focus_key = "download:browse"; // next to the status, at the top
            }
            Render();
        });
    });
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
