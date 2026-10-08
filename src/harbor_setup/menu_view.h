// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "harbor_setup/menu.h"
#include "harbor_setup/menu_input.h"

namespace HarborSetup {

/// The Harbor menu and Harbor Settings in one Borealis activity, on the 1280x720 design canvas.
/// Each change rebuilds the page from the state, keeping the focus on the same row (as the
/// wizard). Settings is laid out like the console's System Settings: categories on the left, the
/// chosen category's rows on the right.
class MenuActivity : public brls::Activity {
public:
    /// `close` hides the menu and then runs the given action (or nothing).
    MenuActivity(MenuHost& host, std::function<void(std::function<void()>)> close);
    ~MenuActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

    /// Shows `page` from its top, as the menu opens.
    void Show(MenuPage page);
    /// B, Android back: one level up; on the menu itself it closes.
    void Back();
    /// While a button is being assigned, the controllers belong to the capture, not the menu.
    bool Capturing() const;
    /// Once a frame: the capture, its countdown and the console's theme.
    void Tick();

private:
    enum class Screen { Menu, Settings };
    enum class Category { Display, Controllers, Touch, Harbor };
    /// A sub-page inside the Settings pane.
    enum class Sub { None, Player, Choice };

    struct Choice {
        std::string title;
        std::vector<std::pair<std::string, std::string>> options; ///< id, label
        std::string current;
        std::function<void(const std::string&)> chosen;
    };

    void Render();
    void Remember(brls::View* view, const std::string& key);
    void BindActions(brls::View* target);
    void Close(std::function<void()> then = {});

    brls::View* Header(const std::string& title);
    brls::View* Footer();
    brls::View* MenuBody();
    brls::View* SettingsBody();
    void CategoryPane(brls::Box* pane);
    void DisplayPane(brls::Box* pane);
    void ControllersPane(brls::Box* pane);
    void PlayerPane(brls::Box* pane);
    void TouchPane(brls::Box* pane);
    void HarborPane(brls::Box* pane);
    void ChoicePane(brls::Box* pane);
    brls::View* CapturePanel();

    brls::Box* Row(const std::string& key, const std::string& title, const std::string& value,
                   std::function<void()> action, bool enabled = true, bool chevron = false);
    brls::Box* Toggle(const std::string& key, const std::string& title, bool on,
                      std::function<void(bool)> changed, bool enabled = true);
    brls::Box* Info(const std::string& key, const std::string& title, const std::string& value);
    brls::View* Heading(const std::string& text);
    brls::View* Note(const std::string& text);
    void OpenChoice(Choice choice);

    void StartCapture(const std::string& what, InputCapture::Kind kind,
                      std::function<void(const Common::ParamPackage&)> done);

    MenuHost& host;
    std::function<void(std::function<void()>)> close;
    InputMapping mapping;
    InputCapture capture;

    Screen screen = Screen::Menu;
    Category category = Category::Display;
    Sub sub = Sub::None;
    int player = 0;
    Choice choice;
    Sub choice_return = Sub::None;
    std::string pane_return_key; ///< the row a sub-page returns to

    // Assigning a button.
    std::string capture_what;
    std::function<void(const Common::ParamPackage&)> capture_done;
    std::chrono::steady_clock::time_point capture_deadline;
    brls::Label* capture_label = nullptr;
    int capture_seconds = 0;
    int capture_step = -1;

    brls::Box* root = nullptr;
    brls::ScrollingFrame* pane_scroll = nullptr;
    std::map<brls::View*, std::string> keys;
    std::map<std::string, brls::View*> by_key;
    std::vector<brls::View*> category_rows;
    std::vector<brls::View*> pane_rows;
    std::string focus_key;
    bool rendering = false;
    bool render_pending = false;
    bool dark = false;
    unsigned frames = 0;
    brls::GenericEvent::Subscription focus_subscription;
};

} // namespace HarborSetup
