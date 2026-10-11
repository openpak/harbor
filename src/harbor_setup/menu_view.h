// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <borealis.hpp>

#include "harbor_setup/driver_catalog.h"
#include "harbor_setup/menu.h"
#include "harbor_setup/menu_input.h"
#include "harbor_setup/wizard.h"

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
    enum class Screen { Menu, Settings, Crash };
    /// The crash-report page: the consent question, last time's report, or its text.
    enum class CrashView { Consent, Offer, Text };
    enum class Category { Display, Controllers, Touch, Harbor };
    /// A sub-page inside the Settings pane.
    enum class Sub { None, Player, Choice, Driver, Downloads };

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
    brls::View* CrashBody();
    std::string CrashTitle() const;
    brls::View* SettingsBody();
    void CategoryPane(brls::Box* pane);
    void DisplayPane(brls::Box* pane);
    void ControllersPane(brls::Box* pane);
    void PlayerPane(brls::Box* pane);
    void TouchPane(brls::Box* pane);
    void HarborPane(brls::Box* pane);
    void ChoicePane(brls::Box* pane);
    void DriverPane(brls::Box* pane);
    void DownloadsPane(brls::Box* pane);
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

    // Graphics driver downloads: the setup's GitHub catalogue and installs (driver_recovery.h),
    // run on a worker thread whose results come back to the menu's thread in Tick.
    bool CanDownloadDrivers();
    void OpenDownloads();
    void BrowseDrivers();
    void DownloadDriver(std::size_t index);
    void CancelDriverWork();
    void DriverWork(std::string stage, std::function<void()> work);
    void DriverPost(std::function<void()> done);
    void DriverProgress(std::string stage, std::optional<double> fraction);
    void DriverStatus(std::string text, Tone tone);

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

    CrashView crash_view = CrashView::Consent;
    std::optional<Common::HarborCrash::Report> crash_report;

    std::optional<DeviceInfo> driver_device;
    std::vector<DriverRelease> driver_releases;
    bool driver_listed = false;
    bool driver_busy = false;
    std::string driver_status;
    Tone driver_tone = Tone::None;
    std::string driver_installed; ///< downloaded and chosen here: "Restart now" restarts into it
    std::atomic_bool driver_cancel{false};
    std::atomic_bool driver_cancellable{false};
    std::atomic<double> driver_fraction{-1.0};
    std::mutex driver_mutex;
    std::string driver_stage; ///< the progress words (guarded by driver_mutex)
    std::vector<std::function<void()>> driver_posts; ///< guarded by driver_mutex
    std::thread driver_worker;
    brls::Label* driver_label = nullptr;

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
