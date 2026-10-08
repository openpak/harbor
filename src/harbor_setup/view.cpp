// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The setup's pages drawn with Borealis views and a few nanovg shapes. Sizes are in the 1280x720 design
// canvas. Look and page layout follow the Ryujinx Harbor (MIT-licensed Ryujinx base), FirstRunView.axaml.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include <SDL3/SDL.h>

#include "common/logging.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/ui.h"
#include "harbor_setup/view.h"

namespace HarborSetup {

namespace {

const Palette light_palette = {
    Hex(0xebebeb), Hex(0xffffff), Hex(0x292929), Hex(0x424242), Hex(0x737373),
    Hex(0xc9c9c9), Hex(0x484848), Hex(0x3156ef), Hex(0x15803d), Hex(0xe7f6ec),
    Hex(0xb91c1c), Hex(0xfdecec), Hex(0xa16207), Hex(0xfdf5e1), Hex(0x39efd2),
};
const Palette dark_palette = {
    Hex(0x2d2d2d), Hex(0x202020), Hex(0xffffff), Hex(0xd6d6d6), Hex(0xa3a3a3),
    Hex(0x4a4a4a), Hex(0xbdbdbd), Hex(0x7998ff), Hex(0x4ade80), Hex(0x2c3b31),
    Hex(0xf87171), Hex(0x432f2f), Hex(0xfbbf24), Hex(0x403823), Hex(0x39efd2),
};
bool dark_theme = false;

constexpr float CanvasWidth = 1280;
constexpr float CanvasHeight = 720;
constexpr float HeaderHeight = 68;
constexpr float FooterHeight = 61;
constexpr float AdvanceHeight = 80;
constexpr float PaneWidth = 800;

// The colour mark on Welcome: the same drawing on its navy tile.
void PaintMark(NVGcontext* vg, float x, float y, float w, float h) {
    nvgTranslate(vg, x, y);
    nvgScale(vg, w / 512.0f, h / 512.0f);
    const auto fill = [&](NVGcolor colour) {
        nvgFillColor(vg, colour);
        nvgFill(vg);
    };

    nvgBeginPath(vg);
    nvgRoundedRect(vg, 0, 0, 512, 512, 112);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 512, 512, Hex(0x213b61), Hex(0x09172c)));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRectVarying(vg, 106, 142, 90, 212, 52, 0, 0, 52);
    fill(Hex(0x42decb));
    nvgBeginPath(vg);
    nvgRoundedRectVarying(vg, 316, 142, 90, 212, 0, 52, 52, 0);
    fill(Hex(0xff7b78));
    nvgBeginPath(vg);
    ControllerDots(vg);
    fill(Hex(0x16304b));
    nvgBeginPath(vg);
    Polygon(vg, {228, 153, 256, 119, 284, 153});
    nvgRoundedRect(vg, 228, 206, 56, 12, 6);
    Polygon(vg, {239, 229, 273, 229, 294, 355, 218, 355});
    fill(Hex(0xf4f7ff));
    nvgBeginPath(vg);
    nvgRect(vg, 235, 164, 42, 34);
    fill(Hex(0xffda88));
    nvgBeginPath(vg);
    nvgRect(vg, 249, 164, 14, 34);
    fill(Hex(0xfff4cf));
    nvgBeginPath(vg);
    Polygon(vg, {235, 253, 277, 253, 280, 271, 232, 271});
    Polygon(vg, {227, 304, 285, 304, 288, 322, 224, 322});
    fill(Hex(0x193251));
    nvgBeginPath(vg);
    Wave(vg);
    fill(Hex(0x59decf));
}

void PaintVerdict(NVGcontext* vg, float x, float y, float w, float h, CheckLevel level) {
    const auto& p = CurrentPalette();
    nvgBeginPath(vg);
    nvgCircle(vg, x + w / 2, y + h / 2, w / 2);
    nvgFillColor(vg, level == CheckLevel::Pass ? p.ok : level == CheckLevel::Warn ? p.warn : p.bad);
    nvgFill(vg);

    nvgBeginPath(vg);
    if (level == CheckLevel::Pass) {
        nvgMoveTo(vg, x + 7, y + 12.25f);
        nvgLineTo(vg, x + 10.5f, y + 15.75f);
        nvgLineTo(vg, x + 17, y + 8.25f);
    } else if (level == CheckLevel::Warn) {
        nvgMoveTo(vg, x + 12, y + 6.9f);
        nvgLineTo(vg, x + 12, y + 13.4f);
        nvgMoveTo(vg, x + 12, y + 16.9f);
        nvgLineTo(vg, x + 12, y + 17.1f);
    } else {
        nvgMoveTo(vg, x + 8, y + 8);
        nvgLineTo(vg, x + 16, y + 16);
        nvgMoveTo(vg, x + 16, y + 8);
        nvgLineTo(vg, x + 8, y + 16);
    }
    nvgStrokeColor(vg, p.paper);
    nvgStrokeWidth(vg, 2.5f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
}

double Seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

/// A scrolling frame that can show its last line (a new result strip below the rows).
class RevealFrame : public brls::ScrollingFrame {
public:
    void ScrollToEnd() {
        const float end = getContentHeight() - getScrollingAreaHeight();
        if (end > 0) {
            setContentOffsetY(end, true);
        }
    }
};

bool SystemDarkTheme() {
    // HARBOR_SETUP_THEME=dark|light overrides, for comparing renders; else the phone's (or desktop's).
    if (const char* forced = std::getenv("HARBOR_SETUP_THEME")) {
        return std::strcmp(forced, "dark") == 0;
    }
    // The console's saved colour set once it has booted (it cannot change while the setup runs),
    // the phone's or desktop's before.
    static const auto console = SavedConsoleDarkTheme();
    return console.value_or(SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK);
}

const Palette& CurrentPalette() {
    return dark_theme ? dark_palette : light_palette;
}

bool DarkTheme() {
    return dark_theme;
}

void SetDarkTheme(bool dark) {
    dark_theme = dark;
    brls::Application::getPlatform()->setThemeVariant(dark ? brls::ThemeVariant::DARK
                                                           : brls::ThemeVariant::LIGHT);
}

void ApplyBorealisStyle() {
    // Focus: a static 3-unit mint ring over the lit row colour of each theme.
    brls::getStyle().addMetric("brls/highlight/stroke_width", 3);
    for (const bool dark : {false, true}) {
        brls::Theme theme = dark ? brls::Theme::getDarkTheme() : brls::Theme::getLightTheme();
        theme.addColor("brls/highlight/color1", nvgRGB(0x39, 0xef, 0xd2));
        theme.addColor("brls/highlight/color2", nvgRGB(0x39, 0xdf, 0xe4));
        theme.addColor("brls/highlight/background",
                       dark ? nvgRGB(0x20, 0x20, 0x20) : nvgRGB(0xff, 0xff, 0xff));
        theme.addColor("brls/click_pulse", nvgRGBA(0x39, 0xef, 0xd2, 38));
    }
}

void FitCanvas() {
    const float w = static_cast<float>(brls::Application::windowWidth);
    const float h = static_cast<float>(brls::Application::windowHeight);
    if (w <= 0 || h <= 0) {
        return;
    }
    brls::Application::setMaximumUIScale(std::min(w / CanvasWidth, h / CanvasHeight));
    brls::Application::setWindowSize(static_cast<int>(w), static_cast<int>(h));
}

WizardActivity::WizardActivity(Wizard& model_) : model{model_} {
    model.changed = [this] { Render(); };
    model.progress_changed = [this] { UpdateProgress(); };
    chime = std::make_unique<Chime>();
    model.page_shown = [this](int note) { chime->Play(note); };
}

WizardActivity::~WizardActivity() {
    brls::Application::getGlobalFocusChangeEvent()->unsubscribe(focus_subscription);
    brls::Application::getRunLoopEvent()->unsubscribe(theme_subscription);
    model.changed = nullptr;
    model.progress_changed = nullptr;
    model.page_shown = nullptr;
}

brls::View* WizardActivity::createContentView() {
    // The window: paper everywhere, the 1280x720 design canvas centred in it.
    root = new brls::Box(brls::Axis::COLUMN);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->setBackgroundColor(CurrentPalette().paper);

    frame = new brls::Box(brls::Axis::COLUMN);
    frame->setDimensions(CanvasWidth, CanvasHeight);
    frame->setPadding(0, 32, 0, 32);
    root->addView(frame);

    BindActions(root);
    focus_subscription = brls::Application::getGlobalFocusChangeEvent()->subscribe([this](brls::View* view) {
        if (rendering) {
            return;
        }
        const auto it = keys.find(view);
        if (it != keys.end()) {
            focus_key = it->second;
        }
    });
    // The phone switching between light and dark while the setup shows: about twice a second.
    theme_subscription = brls::Application::getRunLoopEvent()->subscribe([this] {
        if (++theme_frames % 30 == 0 && SystemDarkTheme() != DarkTheme()) {
            Render();
        }
    });
    return root;
}

void WizardActivity::onContentAvailable() {
    rendered_step = model.step();
    Render();
    chime->Play(model.CurrentNote());
}

void WizardActivity::BindActions(brls::View* target) {
    // Actions bubble from the focused view to the root, so the page's A, B and X live here. A on a
    // button runs the button; A on anything else is the page's own action.
    target->registerAction("Next", brls::BUTTON_A, [this](brls::View*) {
        model.Next();
        return true;
    }, true);
    target->registerAction("Back", brls::BUTTON_B, [this](brls::View*) {
        model.Back();
        return true;
    }, true);
    target->registerAction("Skip", brls::BUTTON_X, [this](brls::View*) {
        model.CanCancel() ? model.Cancel() : model.Skip();
        return true;
    }, true);
    // Plus confirms and Minus goes back, as on the console.
    target->registerAction("Confirm", brls::BUTTON_START, [this](brls::View*) {
        model.Next();
        return true;
    }, true);
    target->registerAction("Return", brls::BUTTON_BACK, [this](brls::View*) {
        model.Back();
        return true;
    }, true);
    // Keyboard: Enter and Escape already arrive as A and B. Space presses the focused button,
    // Backspace goes back, and the letter X is X.
    target->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_SPACE), [](brls::View*) {
        brls::Application::onControllerButtonPressed(brls::BUTTON_A, false);
        return true;
    });
    target->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_BACKSPACE), [this](brls::View*) {
        model.Back();
        return true;
    });
    target->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_X), [this](brls::View*) {
        model.CanCancel() ? model.Cancel() : model.Skip();
        return true;
    });
}

void WizardActivity::Remember(brls::View* view, const std::string& key) {
    keys[view] = key;
    by_key[key] = view;
    if (!first_focus) {
        first_focus = view;
    }
    last_content_focus = view;
}

void WizardActivity::Render() {
    if (!frame) {
        return;
    }
    rendering = true;
    const bool same_page = rendered_step == model.step();
    const float offset = same_page && scroll ? scroll->getContentOffsetY() : 0.0f;
    if (!same_page) {
        focus_key.clear();
    }
    rendered_step = model.step();

    // Before the console's own Basic White/Basic Black exists, light and dark follow the phone.
    if (const bool system_dark = SystemDarkTheme(); system_dark != DarkTheme()) {
        SetDarkTheme(system_dark);
    }
    if (root) {
        root->setBackgroundColor(CurrentPalette().paper);
    }

    frame->clearViews();
    keys.clear();
    by_key.clear();
    first_focus = nullptr;
    last_content_focus = nullptr;
    advance = nullptr;
    progress_label = nullptr;
    scroll = nullptr;

    if (model.ShowHeader()) {
        frame->addView(Header());
    }
    frame->addView(Body());
    frame->addView(Footer());

    // Borealis keeps the focus inside a scrolling frame; route the last row and the action to each other.
    if (advance && last_content_focus) {
        last_content_focus->setCustomNavigationRoute(brls::FocusDirection::DOWN, advance);
        advance->setCustomNavigationRoute(brls::FocusDirection::UP, last_content_focus);
    }
    if (scroll) {
        // Each new page starts at its top.
        scroll->setContentOffsetY(same_page ? offset : 0.0f, false);
    }

    brls::View* focus = nullptr;
    if (const auto it = by_key.find(focus_key); it != by_key.end()) {
        focus = it->second;
    } else if (model.step() == Step::Welcome || model.step() == Step::Ready) {
        focus = advance ? advance : first_focus;
    } else {
        focus = first_focus ? first_focus : advance;
    }
    if (focus) {
        brls::Application::giveFocus(focus);
        focus_key = keys.count(focus) ? keys[focus] : "";
    }
    rendering = false;

    // A new result strip, progress bar or download list (below the fold in landscape) is scrolled
    // into view and takes the focus. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx
    // base), FirstRunView.OnPagePropertyChanged.
    const std::string now_status = model.Status() + (model.Busy() ? "\x01busy" : "");
    const std::size_t download_count = model.step() == Step::Driver ? model.Downloads().size() : 0;
    std::string reveal;
    if (download_count > 0 && shown_downloads == 0) {
        reveal = "download:" + model.Downloads().front().download_url;
    } else if (now_status != shown_status && !model.Status().empty()) {
        reveal = "status";
    } else if (now_status != shown_status && model.Busy()) {
        reveal = "progress";
    }
    if (!reveal.empty()) {
        brls::delay(80, [this, reveal, alive = std::weak_ptr<bool>(alive)] {
            if (alive.expired() || !scroll) {
                return;
            }
            // The status and the progress bar are the page's last lines; a download row is
            // scrolled to by the focus itself.
            if (!reveal.starts_with("download:")) {
                scroll->ScrollToEnd();
            }
            if (const auto it = by_key.find(reveal); it != by_key.end()) {
                brls::Application::giveFocus(it->second);
            }
        });
    }
    shown_status = now_status;
    shown_downloads = download_count;
}

void WizardActivity::UpdateProgress() {
    if (progress_label) {
        progress_label->setText(model.ProgressText());
    }
}

brls::Label* WizardActivity::Text(const std::string& text, float size, NVGcolor colour, bool wrap) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setSingleLine(!wrap);
    label->setAutoAnimate(false);
    label->setTextColor(colour);
    if (wrap) {
        label->setLineHeight(1.4f);
    }
    return label;
}

brls::View* WizardActivity::Header() {
    const auto& p = CurrentPalette();
    auto* bar = new brls::Box(brls::Axis::ROW);
    bar->setHeight(HeaderHeight);
    bar->setPadding(0, 16, 0, 16);
    bar->setAlignItems(brls::AlignItems::CENTER);
    bar->setLineBottom(1);
    bar->setLineColor(p.rule);

    bar->addView(new Shape(28, 28, PaintEmblem));
    auto* title = Text(model.Title(), 24, p.ink);
    title->setMarginLeft(16);
    title->setGrow(1);
    bar->addView(title);

    const auto dots = model.StepDots();
    const float width = static_cast<float>(dots.size()) * 8 + static_cast<float>(dots.size() - 1) * 12;
    bar->addView(new Shape(width, 8, [dots](NVGcontext* vg, float x, float y, float, float) {
        const auto& colours = CurrentPalette();
        for (size_t i = 0; i < dots.size(); i++) {
            nvgBeginPath(vg);
            nvgCircle(vg, x + 4 + static_cast<float>(i) * 20, y + 4, dots[i] == 1 ? 3.5f : 4.0f);
            if (dots[i] == 1) {
                nvgStrokeColor(vg, colours.muted);
                nvgStrokeWidth(vg, 1);
                nvgStroke(vg);
            } else {
                nvgFillColor(vg, dots[i] == 0 ? colours.accent : colours.ink);
                nvgFill(vg);
            }
        }
    }));
    return bar;
}

brls::Box* WizardActivity::List() {
    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setLineTop(1);
    list->setLineColor(CurrentPalette().line);
    list->setMarginTop(14);
    return list;
}

brls::Box* WizardActivity::TextColumn(const std::string& title, const std::string& sub, bool accent_title,
                                      float title_size) {
    const auto& p = CurrentPalette();
    auto* column = new brls::Box(brls::Axis::COLUMN);
    column->setGrow(1);
    column->setShrink(1);
    column->setJustifyContent(brls::JustifyContent::CENTER);
    column->addView(Text(title, title_size, accent_title ? p.accent : p.ink));
    if (!sub.empty()) {
        auto* line = Text(sub, 14, p.muted, true);
        line->setMarginTop(2);
        column->addView(line);
    }
    return column;
}

brls::Box* WizardActivity::ButtonRow(const std::string& key, const std::string& title, const std::string& value,
                                     std::function<void()> action, bool accent, int glyph) {
    const auto& p = CurrentPalette();
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setMinHeight(64);
    row->setPadding(12, 20, 12, 20);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setLineBottom(1);
    row->setLineColor(p.line);
    row->setFocusable(true);
    row->setHighlightCornerRadius(0);
    row->setHighlightPadding(-3); // the 3-unit ring sits inside the row, as in the design
    row->registerClickAction([action](brls::View*) {
        action();
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    if (glyph != NoGlyph) {
        row->addView(new Shape(14, 14, [glyph](NVGcontext* vg, float x, float y, float w, float h) {
            PaintLineGlyph(vg, x, y, w, h, glyph, CurrentPalette().accent);
        }));
        auto* label = Text(title, 22, accent ? p.accent : p.ink);
        label->setMarginLeft(14);
        label->setGrow(1);
        row->addView(label);
    } else {
        auto* label = Text(title, 22, accent ? p.accent : p.ink);
        label->setGrow(1);
        row->addView(label);
    }
    if (!value.empty()) {
        auto* text = Text(value, 18, p.accent);
        text->setMarginLeft(16);
        text->setShrink(1);
        text->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        row->addView(text);
    }
    Remember(row, key);
    return row;
}

brls::Box* WizardActivity::InfoRow(const std::string& key, float min_height) {
    // An information row only takes the focus, so a pad can scroll through it; A on it is the page's A.
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setMinHeight(min_height);
    row->setPadding(12, 20, 12, 20);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setLineBottom(1);
    row->setLineColor(CurrentPalette().line);
    row->setFocusable(true);
    row->setHighlightCornerRadius(0);
    row->setHighlightPadding(-3);
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row, [] {}));
    Remember(row, key);
    return row;
}

brls::View* WizardActivity::StatusStrip() {
    const auto& p = CurrentPalette();
    const auto tone = model.StatusTone();
    const NVGcolor strong = tone == Tone::Ok ? p.ok : tone == Tone::Warn ? p.warn : p.bad;
    const NVGcolor weak = tone == Tone::Ok ? p.ok_weak : tone == Tone::Warn ? p.warn_weak : p.bad_weak;

    auto* strip = new brls::Box(brls::Axis::ROW);
    strip->setMarginTop(20);
    strip->setCornerRadius(4);
    strip->setBackgroundColor(weak);
    // Focusable like an information row, so a pad can scroll it into view.
    strip->setFocusable(true);
    strip->setHighlightCornerRadius(4);
    strip->setHighlightPadding(-3);
    strip->addGestureRecognizer(new brls::TapGestureRecognizer(strip, [] {}));

    auto* bar = new brls::Box(brls::Axis::COLUMN);
    bar->setWidth(4);
    bar->setAlignSelf(brls::AlignSelf::STRETCH);
    bar->setBackgroundCornerRadii(4, 0, 0, 4);
    bar->setBackgroundColor(strong);
    strip->addView(bar);

    auto* message = Text(model.Status(), 18, p.ink, true);
    message->setMargins(10, 14, 10, 14);
    message->setShrink(1);
    message->setGrow(1);
    strip->addView(message);
    Remember(strip, "status");
    return strip;
}

brls::View* WizardActivity::ProgressPanel() {
    const auto& p = CurrentPalette();
    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setMarginTop(20);
    // Focusable like the status strip, so it can take the focus as it appears.
    panel->setFocusable(true);
    panel->setHighlightCornerRadius(0);
    panel->setHighlightPadding(-3);
    panel->addGestureRecognizer(new brls::TapGestureRecognizer(panel, [] {}));
    Remember(panel, "progress");
    progress_label = Text(model.ProgressText(), 18, p.ink2, true);
    panel->addView(progress_label);
    Wizard* wizard = &model;
    auto* bar = new Shape(PaneWidth, 4, [wizard](NVGcontext* vg, float x, float y, float w, float h) {
        const auto& colours = CurrentPalette();
        nvgBeginPath(vg);
        nvgRect(vg, x, y, w, h);
        nvgFillColor(vg, colours.line);
        nvgFill(vg);
        nvgBeginPath(vg);
        if (const auto fraction = wizard->ProgressFraction()) {
            nvgRect(vg, x, y, w * static_cast<float>(std::clamp(*fraction, 0.0, 1.0)), h);
        } else {
            // Indeterminate: a third of the bar sweeping across.
            const float t = static_cast<float>(std::fmod(Seconds(), 1.4) / 1.4);
            const float start = x - w / 3 + t * (w + w / 3);
            const float left = std::max(start, x);
            const float right = std::min(start + w / 3, x + w);
            if (right > left) {
                nvgRect(vg, left, y, right - left, h);
            }
        }
        nvgFillColor(vg, colours.accent);
        nvgFill(vg);
    });
    bar->setMarginTop(8);
    panel->addView(bar);
    return panel;
}

namespace {

// Test hook: HARBOR_SETUP_TEXT="<file>|<n>|=<text>|..." answers the next text fields in order without
// a keyboard: <n> is line n of the file (only the value of a key=value line), =<text> the text itself.
// Values are never logged.
std::vector<std::string> ScriptedTexts() {
    std::vector<std::string> out;
    const char* env = std::getenv("HARBOR_SETUP_TEXT");
    if (!env) {
        return out;
    }
    std::vector<std::string> parts;
    std::string list = env;
    for (size_t start = 0; start <= list.size();) {
        const size_t end = std::min(list.find('|', start), list.size());
        parts.push_back(list.substr(start, end - start));
        start = end + 1;
    }
    if (parts.empty()) {
        return out;
    }
    std::vector<std::string> lines;
    if (std::ifstream file(parts[0]); file) {
        for (std::string line; std::getline(file, line);) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (const auto eq = line.find('='); eq != std::string::npos) {
                line.erase(0, eq + 1);
            }
            lines.push_back(line);
        }
    }
    for (size_t i = 1; i < parts.size(); i++) {
        if (!parts[i].empty() && parts[i][0] == '=') {
            out.push_back(parts[i].substr(1));
        } else if (const int n = std::atoi(parts[i].c_str()); n >= 1 && static_cast<size_t>(n) <= lines.size()) {
            out.push_back(lines[n - 1]);
        }
    }
    return out;
}

} // namespace

void WizardActivity::EditText(const std::string& title, const std::string& hint, const std::string& initial,
                              int max_length, std::function<void(std::string)> done) {
    static std::vector<std::string> scripted = ScriptedTexts();
    static size_t next_scripted = 0;
    if (next_scripted < scripted.size()) {
        LOG_INFO(Frontend, "harbor: scripted text");
        brls::sync([done, text = scripted[next_scripted++]] { done(text); });
        return;
    }
    brls::Application::getImeManager()->openForText(
        [done](std::string text) { done(std::move(text)); }, title, hint, max_length, initial);
}

brls::View* WizardActivity::PageContent() {
    const auto& p = CurrentPalette();
    auto* pane = new brls::Box(brls::Axis::COLUMN);
    pane->setWidth(PaneWidth);

    const Step step = model.step();
    if (step == Step::Welcome) {
        pane->setAlignItems(brls::AlignItems::CENTER);
        pane->addView(new Shape(64, 64, PaintMark));
        auto* title = Text("Welcome to Harbor", 28, p.ink);
        title->setMarginTop(12);
        pane->addView(title);
        auto* lead = Text(model.Lead(), 18, p.ink2, true);
        lead->setMarginTop(12);
        lead->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        pane->addView(lead);
        return pane;
    }

    pane->addView(Text(model.Lead(), 18, p.ink2, true));

    switch (step) {
    case Step::Device: {
        auto* list = List();
        int index = 0;
        for (const auto& check : model.Checks()) {
            auto* row = InfoRow("check:" + std::to_string(index++), 70);
            const CheckLevel level = check.level;
            row->addView(new Shape(24, 24, [level](NVGcontext* vg, float x, float y, float w, float h) {
                PaintVerdict(vg, x, y, w, h, level);
            }));
            auto* text = TextColumn(check.title, check.reason, false);
            text->setMarginLeft(14);
            row->addView(text);
            auto* value = Text(check.value, 18, p.accent);
            value->setMarginLeft(14);
            row->addView(value);
            list->addView(row);
        }
        pane->addView(list);
        break;
    }
    case Step::Keys: {
        auto* list = List();
        list->addView(ButtonRow("keys:prod", "Choose prod.keys", model.KeysChosen(),
                                [this] { model.ChooseProdKeys(); }));
        list->addView(ButtonRow("keys:title", "Add title.keys", "Optional",
                                [this] { model.ChooseTitleKeys(); }));
        pane->addView(list);
        break;
    }
    case Step::Firmware: {
        auto* list = List();
        list->addView(ButtonRow("firmware:file", "Choose a file", ".zip",
                                [this] { model.ChooseFirmwareFile(); }));
        if (model.FirmwareFolderAllowed()) {
            list->addView(ButtonRow("firmware:folder", "Choose a folder", "Folder of .nca files",
                                    [this] { model.ChooseFirmwareFolder(); }));
        }
        pane->addView(list);
        auto* chosen = Text(model.FirmwareChosen(), 14, p.muted);
        chosen->setMargins(14, 16, 0, 16);
        pane->addView(chosen);
        break;
    }
    case Step::Games:
    case Step::Dlc:
    case Step::Updates: {
        auto* list = List();
        const auto& folders = model.Folders();
        for (size_t i = 0; i < folders.size(); i++) {
            auto* row = new brls::Box(brls::Axis::ROW);
            row->setMinHeight(64);
            row->setPadding(10, 8, 10, 20);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setLineBottom(1);
            row->setLineColor(p.line);
            row->addView(TextColumn(folders[i].name, folders[i].detail, false));

            auto* remove = new brls::Box(brls::Axis::ROW);
            remove->setDimensions(44, 44);
            remove->setMarginLeft(8);
            remove->setCornerRadius(22);
            remove->setJustifyContent(brls::JustifyContent::CENTER);
            remove->setAlignItems(brls::AlignItems::CENTER);
            remove->setFocusable(true);
            remove->setHighlightCornerRadius(22);
            remove->registerClickAction([this, i](brls::View*) {
                model.RemoveFolder(i);
                return true;
            });
            remove->addGestureRecognizer(new brls::TapGestureRecognizer(remove));
            remove->addView(new Shape(14, 14, [](NVGcontext* vg, float x, float y, float w, float h) {
                PaintLineGlyph(vg, x, y, w, h, CrossGlyph, CurrentPalette().muted);
            }));
            Remember(remove, "remove:" + folders[i].location);
            row->addView(remove);
            list->addView(row);
        }
        list->addView(ButtonRow("folders:add", model.AddFolderLabel(), "", [this] { model.AddFolder(); }, true,
                                PlusGlyph));
        pane->addView(list);
        break;
    }
    case Step::Driver: {
        auto* list = List();
        const auto& choices = model.DriverChoices();
        for (size_t i = 0; i < choices.size(); i++) {
            const bool chosen = choices[i].id == model.SelectedDriver();
            auto* row = new brls::Box(brls::Axis::ROW);
            row->setMinHeight(64);
            row->setPadding(12, 20, 12, 20);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setLineBottom(1);
            row->setLineColor(p.line);
            row->setFocusable(true);
            row->setHighlightCornerRadius(0);
            row->setHighlightPadding(-3);
            row->registerClickAction([this, i](brls::View*) {
                model.SelectDriver(i);
                return true;
            });
            row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
            row->addView(TextColumn(choices[i].title, choices[i].detail, chosen));
            if (chosen) {
                auto* tick = new Shape(16, 16, [](NVGcontext* vg, float x, float y, float w, float h) {
                    PaintLineGlyph(vg, x, y, w, h, CheckGlyph, CurrentPalette().accent);
                });
                tick->setMarginLeft(12);
                row->addView(tick);
            }
            Remember(row, "driver:" + choices[i].id);
            list->addView(row);
        }
        list->addView(ButtonRow("driver:file", "Install a driver file…", "",
                                [this] { model.InstallDriverFile(); }, true, PlusGlyph));
        if (model.CanBrowseDrivers()) {
            list->addView(ButtonRow("driver:browse", model.BrowseLabel(), "", [this] { model.BrowseDrivers(); },
                                    true, DownloadGlyph));
        }
        pane->addView(list);

        auto* lead = Text(model.DownloadLead(), 18, p.ink2, true);
        lead->setMarginTop(14);
        pane->addView(lead);

        const auto& downloads = model.Downloads();
        if (!downloads.empty()) {
            auto* heading = Text("Available on GitHub", 22, p.ink);
            heading->setMarginTop(14);
            pane->addView(heading);
            auto* available = List();
            for (size_t i = 0; i < downloads.size(); i++) {
                auto* row = new brls::Box(brls::Axis::ROW);
                row->setMinHeight(64);
                row->setPadding(12, 20, 12, 20);
                row->setAlignItems(brls::AlignItems::CENTER);
                row->setLineBottom(1);
                row->setLineColor(p.line);
                row->setFocusable(true);
                row->setHighlightCornerRadius(0);
                row->setHighlightPadding(-3);
                row->registerClickAction([this, i](brls::View*) {
                    model.Download(i);
                    return true;
                });
                row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
                auto* text = TextColumn(downloads[i].title, downloads[i].Detail(), false);
                auto* file = Text(downloads[i].file_name, 14, p.muted);
                text->addView(file);
                row->addView(text);
                auto* action = Text("Download", 18, p.accent);
                action->setMarginLeft(12);
                row->addView(action);
                Remember(row, "download:" + downloads[i].download_url);
                available->addView(row);
            }
            pane->addView(available);
        }
        break;
    }
    case Step::Account: {
        auto* list = List();
        const auto state = model.GetAccountState();
        if (state == AccountState::Form) {
            const bool signing_in = model.GetAccountMode() == AccountMode::SignIn;
            const auto field = [this, list](const std::string& key, const std::string& title, AccountField which,
                                            const std::string& hint, int max_length) {
                const std::string value = model.AccountValue(which);
                list->addView(ButtonRow(key, title, value.empty() ? hint : value,
                                        [this, title, which, hint, max_length] {
                                            EditText(title, hint, model.AccountEditValue(which), max_length,
                                                     [this, which](std::string text) {
                                                         model.SetAccountField(which, std::move(text));
                                                     });
                                        }));
            };
            field("account:email", "Email address", AccountField::Email, "Not entered", 254);
            field("account:password", "Password", AccountField::Password, "Not entered", 128);
            if (!signing_in) {
                field("account:nickname", "Nickname", AccountField::Nickname, "3 to 32 characters", 32);
                field("account:birthday", "Birthday", AccountField::Birthday, "YYYY-MM-DD", 10);
                field("account:country", "Country", AccountField::Country, "Two letters, e.g. GB", 2);
            }
            list->addView(ButtonRow("account:mode",
                                    signing_in ? "Create an OpenPak account" : "Sign in with an existing account",
                                    "",
                                    [this, signing_in] {
                                        model.SetAccountMode(signing_in ? AccountMode::Create : AccountMode::SignIn);
                                    },
                                    true));
        } else if (state == AccountState::Waiting) {
            auto* row = InfoRow("account:waiting", 64);
            auto* label = Text("Waiting for the address to be verified", 22, p.ink);
            label->setGrow(1);
            row->addView(label);
            list->addView(row);
            list->addView(ButtonRow("account:resend", "Send the mail again", "",
                                    [this] { model.ResendVerification(); }, true));
            list->addView(ButtonRow("account:another", "Use another account", "",
                                    [this] { model.UseAnotherAccount(); }, true));
        } else {
            auto* row = InfoRow("account:signed-in", 64);
            auto* label = Text("Signed in", 22, p.ink);
            label->setGrow(1);
            row->addView(label);
            row->addView(Text(model.AccountName(), 18, p.accent));
            list->addView(row);
            list->addView(ButtonRow("account:another", "Use another account", "",
                                    [this] { model.UseAnotherAccount(); }, true));
        }
        pane->addView(list);
        break;
    }
    case Step::Ready: {
        auto* list = List();
        int index = 0;
        for (const auto& row_data : model.Summary()) {
            auto* row = InfoRow("summary:" + std::to_string(index++), 50);
            auto* label = Text(row_data.label, 22, p.ink);
            label->setGrow(1);
            row->addView(label);
            row->addView(Text(row_data.value, 18, p.accent));
            list->addView(row);
        }
        pane->addView(list);
        break;
    }
    default:
        break;
    }

    if (model.Busy()) {
        pane->addView(ProgressPanel());
    }
    if (!model.Status().empty()) {
        pane->addView(StatusStrip());
    }
    return pane;
}

brls::View* WizardActivity::Body() {
    const auto& p = CurrentPalette();
    auto* body = new brls::Box(brls::Axis::COLUMN);
    body->setGrow(1);
    body->setClipsToBounds(true);

    const float visible =
        CanvasHeight - (model.ShowHeader() ? HeaderHeight : 0) - FooterHeight - AdvanceHeight;
    auto* page = new brls::Box(brls::Axis::COLUMN);
    page->setPadding(24, 0, 24, 0);
    page->setAlignItems(brls::AlignItems::CENTER);
    page->setJustifyContent(brls::JustifyContent::CENTER);
    page->setMinHeight(visible);
    page->addView(PageContent());

    scroll = new RevealFrame();
    scroll->setGrow(1);
    scroll->setContentView(page);
    body->addView(scroll);

    // The main action stays above the footer, outside the scrolling rows.
    auto* button = new brls::Box(brls::Axis::ROW);
    button->setDimensions(500, 56);
    button->setAlignSelf(brls::AlignSelf::CENTER);
    button->setMargins(8, 24, 16, 24);
    button->setJustifyContent(brls::JustifyContent::CENTER);
    button->setAlignItems(brls::AlignItems::CENTER);
    button->setCornerRadius(2);
    button->setBorderThickness(1);
    button->setBorderColor(p.line);
    button->setBackgroundColor(p.selected);
    button->setHighlightCornerRadius(2);
    button->setHighlightPadding(-3);
    button->addView(Text(model.NextLabel(), 22, p.ink));
    if (model.CanGoNext()) {
        button->setFocusable(true);
        button->registerClickAction([this](brls::View*) {
            model.Next();
            return true;
        });
        button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
        keys[button] = "advance";
        by_key["advance"] = button;
        advance = button;
    } else {
        button->setAlpha(0.4f);
    }
    body->addView(button);
    return body;
}

brls::View* WizardActivity::Hint(const char* letter, const std::string& word, bool enabled, float min_width,
                                 std::function<void()> action) {
    const auto& p = CurrentPalette();
    const float alpha = enabled ? 1.0f : 0.4f;
    auto* box = new brls::Box(brls::Axis::ROW);
    box->setMinWidth(min_width);
    box->setHeight(44);
    box->setPadding(0, 12, 0, 12);
    box->setMarginLeft(4);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setCornerRadius(6);
    const std::string glyph = letter;
    box->addView(new Shape(24, 24, [glyph, alpha](NVGcontext* vg, float x, float y, float w, float h) {
        PaintButtonGlyph(vg, x, y, w, h, glyph, alpha);
    }));
    auto* text = Text(word, 17, WithAlpha(p.ink, alpha));
    text->setMarginLeft(8);
    box->addView(text);
    if (enabled) {
        box->addGestureRecognizer(new brls::TapGestureRecognizer(
            [action](brls::TapGestureStatus status, brls::Sound*) {
                if (status.state == brls::GestureState::END) {
                    action();
                }
            }));
    }
    return box;
}

brls::View* WizardActivity::Footer() {
    auto* bar = new brls::Box(brls::Axis::ROW);
    bar->setHeight(FooterHeight);
    bar->setLineTop(1);
    bar->setLineColor(CurrentPalette().rule);
    bar->setPadding(8, 8, 8, 0);
    bar->setJustifyContent(brls::JustifyContent::FLEX_END);
    bar->setAlignItems(brls::AlignItems::CENTER);

    if (model.CanCancel()) {
        bar->addView(Hint("X", "Cancel", true, 112, [this] { model.Cancel(); }));
    } else if (model.step() == Step::Driver || model.step() == Step::Account) {
        bar->addView(Hint("X", model.SkipLabel(), model.CanSkip(), 112, [this] { model.Skip(); }));
    }
    if (model.ShowHeader()) {
        bar->addView(Hint("B", "Back", model.CanGoBack(), 112, [this] { model.Back(); }));
    }
    bar->addView(Hint("A", model.NextLabel(), model.CanGoNext(), 156, [this] { model.Next(); }));
    return bar;
}

} // namespace HarborSetup
