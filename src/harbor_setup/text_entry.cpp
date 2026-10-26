// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The text entry page (see text_entry.h). Sizes are in the 1280x720 design canvas, as in view.cpp.
// The typed text is never logged.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "harbor_setup/text_entry.h"
#include "harbor_setup/ui.h"
#include "harbor_setup/view.h"

namespace HarborSetup {

namespace {

constexpr float CanvasW = 1280;
constexpr float CanvasH = 720;
constexpr float HeaderH = 68;
constexpr float FooterH = 61;
constexpr float KeyboardW = 1152; // twelve key units and eleven gaps
constexpr float KeyH = 64;
constexpr float KeyGap = 6;
constexpr float FieldH = 80;
constexpr float FieldPad = 24;
constexpr float FieldFont = 30;
constexpr float KeyFont = 26;
constexpr float FunctionFont = 20;
constexpr std::size_t KeyRows = 5;

double Seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool IsLead(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
}

std::size_t CodepointCount(const std::string& s) {
    std::size_t n = 0;
    for (const char c : s) {
        n += IsLead(c) ? 1 : 0;
    }
    return n;
}

/// The byte index where code point `cp` starts (the size when `cp` is past the end).
std::size_t ByteOffset(const std::string& s, std::size_t cp) {
    std::size_t i = 0;
    for (std::size_t n = 0; n < cp && i < s.size(); n++) {
        i++;
        while (i < s.size() && !IsLead(s[i])) {
            i++;
        }
    }
    return i;
}

/// Overwrites then empties a string that held a password (the compiler may not drop the stores).
void Wipe(std::string& s) {
    volatile char* p = s.data();
    for (std::size_t i = 0; i < s.size(); i++) {
        p[i] = '\0';
    }
    s.clear();
}

enum class Kind { Char, Shift, Layer, Backspace, Space, Show, Cancel, Done };
enum class Layer { Letters, Symbols };
enum class ShiftState { Off, Once, Caps };

struct Cell {
    Kind kind = Kind::Char;
    std::string plain;
    std::string shifted;
};

float WeightOf(Kind kind) {
    return kind == Kind::Shift || kind == Kind::Layer || kind == Kind::Backspace ? 2.0f : 1.0f;
}

std::vector<Cell> Chars(std::string_view plain, std::string_view shifted) {
    std::vector<Cell> out;
    for (std::size_t i = 0; i < plain.size(); i++) {
        out.push_back(Cell{Kind::Char, std::string(1, plain[i]), std::string(1, shifted[i])});
    }
    return out;
}

std::vector<Cell> Join(std::vector<Cell> head, const std::vector<Cell>& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

std::vector<Cell> Lead(Kind kind, const std::vector<Cell>& rest) {
    return Join({Cell{kind, "", ""}}, rest);
}

/// Four rows of eleven or twelve units each; the fifth row is the bar of bigger keys below.
using Layout = std::array<std::vector<Cell>, 4>;

Layout LettersLayout() {
    return {{
        Join(Chars("1234567890", "!@#$%^&*()"), {Cell{Kind::Backspace, "", ""}}),
        Chars("qwertyuiop@.", "QWERTYUIOP~:"),
        Lead(Kind::Shift, Chars("asdfghjkl-", "ASDFGHJKL_")),
        Lead(Kind::Layer, Chars("zxcvbnm,'/", "ZXCVBNM;\"?")),
    }};
}

Layout SymbolsLayout() {
    return {{
        Join(Chars("1234567890", "1234567890"), {Cell{Kind::Backspace, "", ""}}),
        Chars("!@#$%^&*()~`", "!@#$%^&*()~`"),
        Lead(Kind::Shift, Chars("-_=+[]{}\\|", "-_=+[]{}\\|")),
        Lead(Kind::Layer, Chars(";:'\",.<>?/", ";:'\",.<>?/")),
    }};
}

class TextEntryActivity;
TextEntryActivity* open_page = nullptr;

class TextEntryActivity final : public brls::Activity {
public:
    TextEntryActivity(TextEntryOptions options_, std::function<void(std::string)> done_)
        : options{std::move(options_)}, done{std::move(done_)} {
        if (options.max_length <= 0) {
            options.max_length = 1024;
        }
        text = options.initial;
        options.initial.clear();
        if (CodepointCount(text) > static_cast<std::size_t>(options.max_length)) {
            text.resize(ByteOffset(text, static_cast<std::size_t>(options.max_length)));
        }
        cursor = CodepointCount(text);
        last_input = Seconds();
        letters = LettersLayout();
        symbols = SymbolsLayout();
    }

    ~TextEntryActivity() override {
        StopInput();
        if (open_page == this) {
            open_page = nullptr;
        }
        Wipe(text);
    }

    /// After the page is pushed: the desktop keyboard's characters start arriving.
    void StartInput() {
        if (input_started || closing) {
            return;
        }
        input_started = true;
        SDL_AddEventWatch(&TextEntryActivity::Watch, this);
        run_loop = brls::Application::getRunLoopEvent()->subscribe([this] { Drain(); });
#ifndef __ANDROID__
        // Android would open its own keyboard for this: the page has its own there, and the
        // physical keyboard's text is not read (see the report).
        window = SDL_GetKeyboardFocus();
        if (window) {
            SDL_StartTextInput(window);
        }
#endif
    }

    void StopInput() {
        if (!input_started) {
            return;
        }
        input_started = false;
        SDL_RemoveEventWatch(&TextEntryActivity::Watch, this);
        brls::Application::getRunLoopEvent()->unsubscribe(run_loop);
#ifndef __ANDROID__
        if (window) {
            SDL_StopTextInput(window);
        }
#endif
        window = nullptr;
    }

    void Close(bool confirm) {
        if (closing) {
            return;
        }
        closing = true;
        StopInput();
        if (open_page == this) {
            open_page = nullptr;
        }
        auto payload = std::make_shared<std::string>();
        if (confirm) {
            *payload = text;
        }
        Wipe(text);
        std::function<void()> finish = [payload, confirm, done_cb = std::move(done),
                                        closed_cb = std::move(options.closed)]() {
            if (closed_cb) {
                closed_cb();
            }
            if (confirm && done_cb) {
                done_cb(*payload);
            }
            Wipe(*payload);
        };
        // The page leaves with Borealis' own fade, like its stock dialog: the activity is freed
        // after the fade, not inside the key press that closed it.
        if (!brls::Application::popActivity(brls::TransitionAnimation::FADE, finish)) {
            finish();
        }
    }

    brls::View* createContentView() override {
        const auto& p = CurrentPalette();
        root = new brls::Box(brls::Axis::COLUMN);
        root->setJustifyContent(brls::JustifyContent::CENTER);
        root->setAlignItems(brls::AlignItems::CENTER);
        root->setBackgroundColor(p.paper);

        auto* frame = new brls::Box(brls::Axis::COLUMN);
        frame->setDimensions(CanvasW, CanvasH);
        frame->setPadding(0, 32, 0, 32);
        frame->addView(Header());
        frame->addView(Body());
        frame->addView(Footer());
        root->addView(frame);

        BindActions();
        UpdateCounter();
        StyleKeys();
        return root;
    }

private:
    struct Key {
        Cell letters_cell;
        Cell symbols_cell;
        float weight = 1;
        float centre = 0;
        brls::Box* box = nullptr;
        brls::Label* label = nullptr;
    };

    // ---- the text ----

    std::string Display() const {
        if (!options.secret || shown) {
            return text;
        }
        std::string bullets;
        for (std::size_t i = 0, n = CodepointCount(text); i < n; i++) {
            bullets += "•";
        }
        return bullets;
    }

    void Touched() {
        last_input = Seconds(); // the caret stays solid while typing
        UpdateCounter();
    }

    void UpdateCounter() {
        if (counter) {
            counter->setText(std::to_string(CodepointCount(text)) + "/" + std::to_string(options.max_length));
        }
    }

    void Insert(const std::string& incoming) {
        for (std::size_t i = 0; i < incoming.size();) {
            std::size_t end = i + 1;
            while (end < incoming.size() && !IsLead(incoming[end])) {
                end++;
            }
            const unsigned char first = static_cast<unsigned char>(incoming[i]);
            const bool control = first < 0x20 || first == 0x7f;
            if (!control) {
                if (CodepointCount(text) >= static_cast<std::size_t>(options.max_length)) {
                    break;
                }
                text.insert(ByteOffset(text, cursor), incoming, i, end - i);
                cursor++;
            }
            i = end;
        }
        Touched();
    }

    void Backspace() {
        if (cursor == 0) {
            return;
        }
        const std::size_t from = ByteOffset(text, cursor - 1);
        text.erase(from, ByteOffset(text, cursor) - from);
        cursor--;
        Touched();
    }

    void DeleteForward() {
        if (cursor >= CodepointCount(text)) {
            return;
        }
        const std::size_t from = ByteOffset(text, cursor);
        text.erase(from, ByteOffset(text, cursor + 1) - from);
        Touched();
    }

    void MoveCursor(int by) {
        const std::size_t count = CodepointCount(text);
        if (by < 0 && cursor > 0) {
            cursor--;
        } else if (by > 0 && cursor < count) {
            cursor++;
        }
        Touched();
    }

    /// The caret index nearest to `x`, measured from the left edge of the field.
    std::size_t CursorFromX(float x) {
        NVGcontext* vg = brls::Application::getNVGContext();
        const std::string shown_text = Display();
        nvgSave(vg);
        nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
        nvgFontSize(vg, FieldFont);
        const float target = x - FieldPad + scroll_x;
        const char* begin = shown_text.c_str();
        std::size_t best = 0;
        float best_distance = std::abs(target);
        for (std::size_t i = 1, n = CodepointCount(shown_text); i <= n; i++) {
            const float advance = nvgTextBounds(vg, 0, 0, begin, begin + ByteOffset(shown_text, i), nullptr);
            if (const float distance = std::abs(target - advance); distance < best_distance) {
                best_distance = distance;
                best = i;
            }
        }
        nvgRestore(vg);
        return best;
    }

    // ---- the keys ----

    const Cell& CellOf(const Key& key) const {
        return layer == Layer::Letters ? key.letters_cell : key.symbols_cell;
    }

    bool PhysicalEnter() {
        const std::uint64_t at = enter_ticks.exchange(0);
        return at != 0 && SDL_GetTicks() - at < 250;
    }

    void Activate(std::size_t row, std::size_t col) {
        if (closing) {
            return;
        }
        if (PhysicalEnter()) {
            // A desktop keyboard's Enter reaches Borealis as the pad's A: here it confirms.
            Close(true);
            return;
        }
        const Cell& cell = CellOf(rows[row][col]);
        switch (cell.kind) {
        case Kind::Char:
            Insert(layer == Layer::Letters && shift != ShiftState::Off ? cell.shifted : cell.plain);
            if (shift == ShiftState::Once) {
                shift = ShiftState::Off;
                StyleKeys();
            }
            break;
        case Kind::Shift:
            if (layer == Layer::Letters) {
                shift = shift == ShiftState::Off    ? ShiftState::Once
                        : shift == ShiftState::Once ? ShiftState::Caps
                                                    : ShiftState::Off;
                StyleKeys();
            }
            break;
        case Kind::Layer:
            layer = layer == Layer::Letters ? Layer::Symbols : Layer::Letters;
            shift = ShiftState::Off;
            StyleKeys();
            break;
        case Kind::Backspace:
            Backspace();
            break;
        case Kind::Space:
            Insert(" ");
            break;
        case Kind::Show:
            shown = !shown;
            StyleKeys();
            Touched();
            break;
        case Kind::Cancel:
            Close(false);
            break;
        case Kind::Done:
            Close(true);
            break;
        }
    }

    std::string LabelOf(const Cell& cell) const {
        switch (cell.kind) {
        case Kind::Char:
            return layer == Layer::Letters && shift != ShiftState::Off ? cell.shifted : cell.plain;
        case Kind::Shift:
            return layer == Layer::Letters ? (shift == ShiftState::Caps ? "Caps" : "Shift") : "";
        case Kind::Layer:
            return layer == Layer::Letters ? "?123" : "abc";
        case Kind::Backspace:
            return "Delete";
        case Kind::Space:
            return "Space";
        case Kind::Show:
            return shown ? "Hide" : "Show";
        case Kind::Cancel:
            return "Cancel";
        case Kind::Done:
            return "Done";
        }
        return "";
    }

    /// Labels and colours of every key from the current layer and shift state.
    void StyleKeys() {
        const auto& p = CurrentPalette();
        for (auto& row : rows) {
            for (auto& key : row) {
                const Cell& cell = CellOf(key);
                const bool plain_key = cell.kind == Kind::Char;
                const bool active = (cell.kind == Kind::Shift && layer == Layer::Letters && shift != ShiftState::Off) ||
                                    (cell.kind == Kind::Show && shown);
                NVGcolor background = plain_key ? p.selected : WithAlpha(p.ink, 0.10f);
                NVGcolor ink = p.ink;
                if (cell.kind == Kind::Done) {
                    background = p.accent;
                    ink = p.selected;
                } else if (active) {
                    background = WithAlpha(p.accent, 0.25f);
                    ink = p.accent;
                }
                key.box->setBackgroundColor(background);
                key.label->setTextColor(ink);
                key.label->setFontSize(plain_key ? KeyFont : FunctionFont);
                key.label->setText(LabelOf(cell));
                key.box->setAlpha(cell.kind == Kind::Shift && layer == Layer::Symbols ? 0.4f : 1.0f);
            }
        }
    }

    brls::Box* MakeKey(std::size_t row, std::size_t col, float width, bool first) {
        const auto& p = CurrentPalette();
        auto& key = rows[row][col];
        const Cell& cell = key.letters_cell;
        auto* box = new brls::Box(brls::Axis::ROW);
        box->setDimensions(width, KeyH);
        if (!first) {
            box->setMarginLeft(KeyGap);
        }
        box->setJustifyContent(brls::JustifyContent::CENTER);
        box->setAlignItems(brls::AlignItems::CENTER);
        box->setCornerRadius(4);
        box->setBorderThickness(1);
        box->setBorderColor(p.line);
        box->setFocusable(true);
        box->setHighlightCornerRadius(4);
        box->setHighlightPadding(-3); // the ring sits inside the key: the gap is only 6
        // Only Delete repeats while A is held, like a keyboard; letters do not.
        box->registerAction("", brls::BUTTON_A, [this, row, col](brls::View*) {
            Activate(row, col);
            return true;
        }, true, cell.kind == Kind::Backspace, brls::SOUND_CLICK);
        box->addGestureRecognizer(new brls::TapGestureRecognizer(box));
        key.label = MakeText("", KeyFont, p.ink);
        box->addView(key.label);
        key.box = box;
        return box;
    }

    /// Pad navigation by position: up and down land on the key above or below the current one's
    /// centre (Borealis alone would land on the first or last key of the row), and left and right
    /// wrap around the row.
    void LinkNavigation() {
        for (std::size_t r = 0; r < rows.size(); r++) {
            const auto nearest = [this](std::size_t row, float centre) {
                brls::View* best = nullptr;
                float best_distance = 1e9f;
                for (auto& key : rows[row]) {
                    if (const float distance = std::abs(key.centre - centre); distance < best_distance) {
                        best_distance = distance;
                        best = key.box;
                    }
                }
                return best;
            };
            for (auto& key : rows[r]) {
                if (r > 0) {
                    key.box->setCustomNavigationRoute(brls::FocusDirection::UP, nearest(r - 1, key.centre));
                }
                if (r + 1 < rows.size()) {
                    key.box->setCustomNavigationRoute(brls::FocusDirection::DOWN, nearest(r + 1, key.centre));
                }
            }
            if (rows[r].size() > 1) {
                rows[r].front().box->setCustomNavigationRoute(brls::FocusDirection::LEFT, rows[r].back().box);
                rows[r].back().box->setCustomNavigationRoute(brls::FocusDirection::RIGHT, rows[r].front().box);
            }
        }
    }

    // ---- the page ----

    brls::View* Header() {
        const auto& p = CurrentPalette();
        auto* bar = new brls::Box(brls::Axis::ROW);
        bar->setHeight(HeaderH);
        bar->setPadding(0, 16, 0, 16);
        bar->setAlignItems(brls::AlignItems::CENTER);
        bar->setLineBottom(1);
        bar->setLineColor(p.rule);
        bar->addView(new Shape(28, 28, PaintEmblem));
        auto* title = MakeText(options.title, 24, p.ink);
        title->setMarginLeft(16);
        title->setGrow(1);
        bar->addView(title);
        return bar;
    }

    brls::View* Body() {
        const auto& p = CurrentPalette();
        auto* body = new brls::Box(brls::Axis::COLUMN);
        body->setGrow(1);
        body->setJustifyContent(brls::JustifyContent::CENTER);
        body->setAlignItems(brls::AlignItems::CENTER);

        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setWidth(KeyboardW);

        // The guidance on the left, the count on the right.
        auto* info = new brls::Box(brls::Axis::ROW);
        info->setHeight(28);
        info->setAlignItems(brls::AlignItems::CENTER);
        auto* hint = MakeText(options.hint, 18, p.muted);
        hint->setGrow(1);
        hint->setShrink(1);
        info->addView(hint);
        counter = MakeText("", 18, p.muted);
        counter->setMarginLeft(16);
        counter->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        info->addView(counter);
        content->addView(info);

        // The field: a tall box with large text and a caret, drawn in one piece.
        field = new Shape(KeyboardW, FieldH, [this](NVGcontext* vg, float x, float y, float w, float h) {
            PaintField(vg, x, y, w, h);
        });
        field->setMarginTop(8);
        field->addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound*) {
            if (status.state == brls::GestureState::END && !closing) {
                cursor = CursorFromX(status.position.x - field->getX());
                Touched();
            }
        }));
        content->addView(field);

        content->addView(Keyboard());
        body->addView(content);
        return body;
    }

    brls::View* Keyboard() {
        const float unit = (KeyboardW - 11 * KeyGap) / 12;
        auto* keyboard = new brls::Box(brls::Axis::COLUMN);
        keyboard->setMarginTop(22);

        // Four rows from the layouts, and the bar: Show (a password), Space, Cancel, Done.
        std::array<std::vector<Cell>, KeyRows> bar_and_rows;
        for (std::size_t r = 0; r < 4; r++) {
            bar_and_rows[r] = letters[r];
        }
        std::vector<Cell> bar;
        if (options.secret) {
            bar.push_back(Cell{Kind::Show, "", ""});
        }
        bar.push_back(Cell{Kind::Space, "", ""});
        bar.push_back(Cell{Kind::Cancel, "", ""});
        bar.push_back(Cell{Kind::Done, "", ""});
        bar_and_rows[4] = bar;

        rows.clear();
        rows.resize(KeyRows);
        for (std::size_t r = 0; r < KeyRows; r++) {
            auto* row_box = new brls::Box(brls::Axis::ROW);
            row_box->setHeight(KeyH);
            if (r > 0) {
                row_box->setMarginTop(KeyGap);
            }
            float x = 0;
            const std::size_t count = bar_and_rows[r].size();
            for (std::size_t c = 0; c < count; c++) {
                Key key;
                key.letters_cell = bar_and_rows[r][c];
                key.symbols_cell = r < 4 ? symbols[r][c] : key.letters_cell;
                float weight = WeightOf(key.letters_cell.kind);
                if (r == 4) {
                    // 12 units: Show 2 + Space 6 + Cancel 2 + Done 2, or Space 8 without Show.
                    const Kind kind = key.letters_cell.kind;
                    weight = kind == Kind::Space ? (options.secret ? 6.0f : 8.0f) : 2.0f;
                }
                key.weight = weight;
                const float width = weight * unit + (weight - 1) * KeyGap;
                key.centre = x + width / 2;
                x += width + KeyGap;
                rows[r].push_back(std::move(key));
            }
            for (std::size_t c = 0; c < count; c++) {
                const float width = rows[r][c].weight * unit + (rows[r][c].weight - 1) * KeyGap;
                row_box->addView(MakeKey(r, c, width, c == 0));
            }
            keyboard->addView(row_box);
        }
        LinkNavigation();
        return keyboard;
    }

    brls::View* Hint(const char* letter, const std::string& word, float min_width, std::function<void()> action) {
        const auto& p = CurrentPalette();
        auto* box = new brls::Box(brls::Axis::ROW);
        box->setMinWidth(min_width);
        box->setHeight(44);
        box->setPadding(0, 12, 0, 12);
        box->setMarginLeft(4);
        box->setAlignItems(brls::AlignItems::CENTER);
        box->setCornerRadius(6);
        const std::string glyph = letter;
        box->addView(new Shape(24, 24, [glyph](NVGcontext* vg, float x, float y, float w, float h) {
            PaintButtonGlyph(vg, x, y, w, h, glyph, 1.0f);
        }));
        auto* label = MakeText(word, 17, p.ink);
        label->setMarginLeft(8);
        box->addView(label);
        if (action) {
            box->addGestureRecognizer(new brls::TapGestureRecognizer(
                [action](brls::TapGestureStatus status, brls::Sound*) {
                    if (status.state == brls::GestureState::END) {
                        action();
                    }
                }));
        }
        return box;
    }

    brls::View* Footer() {
        auto* bar = new brls::Box(brls::Axis::ROW);
        bar->setHeight(FooterH);
        bar->setLineTop(1);
        bar->setLineColor(CurrentPalette().rule);
        bar->setPadding(8, 8, 8, 0);
        bar->setJustifyContent(brls::JustifyContent::FLEX_END);
        bar->setAlignItems(brls::AlignItems::CENTER);
        bar->addView(Hint("Y", "Space", 112, [this] {
            if (!closing) {
                Insert(" ");
            }
        }));
        bar->addView(Hint("X", "Delete", 112, [this] {
            if (!closing) {
                Backspace();
            }
        }));
        bar->addView(Hint("B", "Cancel", 112, [this] { Close(false); }));
        bar->addView(Hint("A", "Select", 112, nullptr));
        bar->addView(Hint("+", "Done", 112, [this] { Close(true); }));
        return bar;
    }

    void BindActions() {
        // Actions bubble from the focused key to the root. Hidden: the page draws its own prompts.
        root->registerAction("", brls::BUTTON_B, [this](brls::View*) {
            Close(false);
            return true;
        }, true);
        const auto erase = [this](brls::View*) {
            if (!closing) {
                Backspace();
            }
            return true;
        };
        root->registerAction("", brls::BUTTON_X, erase, true, true);
        root->registerAction("", brls::BUTTON_BACK, erase, true, true);
        root->registerAction("", brls::BUTTON_Y, [this](brls::View*) {
            if (!closing) {
                Insert(" ");
            }
            return true;
        }, true);
        root->registerAction("", brls::BUTTON_START, [this](brls::View*) {
            Close(true);
            return true;
        }, true);
        // The shoulder buttons move the caret, as on the console's keyboard.
        root->registerAction("", brls::BUTTON_LB, [this](brls::View*) {
            if (!closing) {
                MoveCursor(-1);
            }
            return true;
        }, true, true);
        root->registerAction("", brls::BUTTON_RB, [this](brls::View*) {
            if (!closing) {
                MoveCursor(1);
            }
            return true;
        }, true, true);
        // A desktop keyboard: Backspace, Delete and paste. The letters come as text (see Watch).
        root->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_BACKSPACE), [this](brls::View*) {
            if (!closing) {
                Backspace();
            }
            return true;
        }, true);
        root->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_DELETE), [this](brls::View*) {
            if (!closing) {
                DeleteForward();
            }
            return true;
        }, true);
        int paste_modifier = brls::BRLS_KBD_MODIFIER_CTRL;
#ifdef __APPLE__
        paste_modifier = brls::BRLS_KBD_MODIFIER_META;
#endif
        root->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_V, paste_modifier), [this](brls::View*) {
            if (!closing) {
                Insert(brls::Application::getPlatform()->pasteFromClipboard());
            }
            return true;
        });
    }

    void PaintField(NVGcontext* vg, float x, float y, float w, float h) {
        const auto& p = CurrentPalette();
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, w, h, 4);
        nvgFillColor(vg, p.selected);
        nvgFill(vg);
        // The field being edited wears the selection cursor's cyan.
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 1.5f, y + 1.5f, w - 3, h - 3, 3);
        nvgStrokeColor(vg, p.focus);
        nvgStrokeWidth(vg, 3);
        nvgStroke(vg);

        const std::string shown_text = Display();
        const char* begin = shown_text.c_str();
        nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
        nvgFontSize(vg, FieldFont);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        const float inner = w - 2 * FieldPad;
        const float caret = nvgTextBounds(vg, 0, 0, begin, begin + ByteOffset(shown_text, cursor), nullptr);
        const float total = nvgTextBounds(vg, 0, 0, begin, begin + shown_text.size(), nullptr);
        // Long text scrolls sideways to keep the caret in view.
        if (caret - scroll_x > inner - 4) {
            scroll_x = caret - (inner - 4);
        }
        if (caret < scroll_x) {
            scroll_x = caret;
        }
        scroll_x = std::clamp(scroll_x, 0.0f, std::max(0.0f, total + 4 - inner));

        nvgScissor(vg, x + FieldPad - 2, y + 2, inner + 4, h - 4);
        nvgFillColor(vg, p.ink);
        nvgText(vg, x + FieldPad - scroll_x, y + h / 2, begin, begin + shown_text.size());
        if (std::fmod(Seconds() - last_input, 1.06) < 0.62) {
            nvgBeginPath(vg);
            nvgRect(vg, x + FieldPad - scroll_x + caret - 1, y + h / 2 - 20, 2, 40);
            nvgFillColor(vg, p.focus);
            nvgFill(vg);
        }
    }

    // ---- desktop keyboard ----

    /// SDL's event watch: text and the Enter key, from whichever thread SDL pushes the event on.
    static bool SDLCALL Watch(void* userdata, SDL_Event* event) {
        auto* self = static_cast<TextEntryActivity*>(userdata);
        if (event->type == SDL_EVENT_TEXT_INPUT && event->text.text) {
            std::lock_guard lock{self->pending_mutex};
            self->pending.emplace_back(event->text.text);
        } else if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
                   (event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER)) {
            self->enter_ticks = SDL_GetTicks();
        }
        return true;
    }

    /// On Borealis' own thread, each frame: the text SDL delivered since the last one.
    void Drain() {
        std::vector<std::string> batch;
        {
            std::lock_guard lock{pending_mutex};
            batch.swap(pending);
        }
        for (auto& part : batch) {
            if (!closing) {
                Insert(part);
            }
            Wipe(part);
        }
    }

    TextEntryOptions options;
    std::function<void(std::string)> done;
    std::string text;
    std::size_t cursor = 0;
    bool shown = false;
    bool closing = false;
    Layer layer = Layer::Letters;
    ShiftState shift = ShiftState::Off;
    double last_input = 0;
    float scroll_x = 0;

    Layout letters;
    Layout symbols;
    std::vector<std::vector<Key>> rows;
    brls::Box* root = nullptr;
    brls::Label* counter = nullptr;
    Shape* field = nullptr;

    bool input_started = false;
    SDL_Window* window = nullptr;
    brls::VoidEvent::Subscription run_loop;
    std::mutex pending_mutex;
    std::vector<std::string> pending;
    std::atomic<std::uint64_t> enter_ticks{0};
};

} // namespace

#ifdef ANDROID
namespace {
bool android_text_open = false;
}
#endif

bool OpenTextEntry(TextEntryOptions options, std::function<void(std::string)> done) {
    if (TextEntryOpen()) {
        return false;
    }
#ifdef ANDROID
    android_text_open = true;
    auto closed = options.closed;
    OpenAndroidTextEntry(options, [closed = std::move(closed), done = std::move(done)](
                                     std::optional<std::string> text) mutable {
        brls::sync([closed = std::move(closed), done = std::move(done), text = std::move(text)]() mutable {
            android_text_open = false;
            if (closed) {
                closed();
            }
            if (text && done) {
                done(std::move(*text));
            }
        });
    });
    return true;
#endif
    auto* page = new TextEntryActivity(std::move(options), std::move(done));
    open_page = page;
    brls::Application::pushActivity(page, brls::TransitionAnimation::FADE);
    page->StartInput();
    return true;
}

bool CancelTextEntry() {
#ifdef ANDROID
    if (android_text_open) {
        CancelAndroidTextEntry();
        return true;
    }
#endif
    if (!open_page) {
        return false;
    }
    open_page->Close(false);
    return true;
}

bool TextEntryOpen() {
#ifdef ANDROID
    if (android_text_open) {
        return true;
    }
#endif
    return open_page != nullptr;
}

} // namespace HarborSetup
