// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <map>
#include <memory>
#include <string>

#include <borealis.hpp>

#include "harbor_setup/chime.h"
#include "harbor_setup/wizard.h"

namespace HarborSetup {

/// The setup's two palettes: the firmware's neutral Basic White and charcoal Basic Black, blue
/// values, mint focus.
struct Palette {
    NVGcolor paper, selected, ink, ink2, muted, line, rule, accent;
    NVGcolor ok, ok_weak, bad, bad_weak, warn, warn_weak, focus;
};

const Palette& CurrentPalette();
/// Whether the phone (or desktop) is in dark mode.
bool SystemDarkTheme();
void SetDarkTheme(bool dark);
bool DarkTheme();

/// The focus ring and lit-row colours of both themes (after Borealis has started).
void ApplyBorealisStyle();

/// Scales the 1280x720 design canvas uniformly into the window (Borealis alone scales by width).
void FitCanvas();

class RevealFrame;

/// The whole wizard in one Borealis activity: each change rebuilds the page from the model, keeping
/// the focus on the same row. Laid out like a console's system settings page, on a 1280x720 canvas.
class WizardActivity : public brls::Activity {
public:
    explicit WizardActivity(Wizard& model);
    ~WizardActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    void Render();
    void UpdateProgress();
    void BindActions(brls::View* target);
    void Remember(brls::View* view, const std::string& key);

    brls::View* Header();
    brls::View* Body();
    brls::View* Footer();
    brls::View* PageContent();

    brls::Box* List();
    brls::Box* ButtonRow(const std::string& key, const std::string& title, const std::string& value,
                         std::function<void()> action, bool accent = false, int glyph = 0);
    brls::Box* InfoRow(const std::string& key, float min_height);
    brls::Box* TextColumn(const std::string& title, const std::string& sub, bool accent_title,
                          float title_size = 22);
    brls::Label* Text(const std::string& text, float size, NVGcolor colour, bool wrap = false);
    brls::View* Hint(const char* letter, const std::string& word, bool enabled, float min_width,
                     std::function<void()> action);
    brls::View* StatusStrip();
    brls::View* ProgressPanel();
    /// A text field: the on-screen keyboard on Android, typing on desktop.
    void EditText(const std::string& title, const std::string& hint, const std::string& initial, int max_length,
                  std::function<void(std::string)> done);

    Wizard& model;
    std::unique_ptr<Chime> chime;
    brls::Box* root = nullptr;
    brls::Box* frame = nullptr;
    RevealFrame* scroll = nullptr;
    brls::Label* progress_label = nullptr;
    std::map<brls::View*, std::string> keys;
    std::map<std::string, brls::View*> by_key;
    std::string focus_key;
    brls::View* first_focus = nullptr;
    brls::View* last_content_focus = nullptr;
    brls::View* advance = nullptr;
    std::string shown_status;
    std::size_t shown_downloads = 0;
    Step rendered_step = Step::Welcome;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool reveal_end = false;
    bool rendering = false;
    brls::GenericEvent::Subscription focus_subscription;
    brls::VoidEvent::Subscription theme_subscription;
    unsigned theme_frames = 0;
};

} // namespace HarborSetup
