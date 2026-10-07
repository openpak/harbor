// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "harbor_setup/device_check.h"
#include "harbor_setup/driver_catalog.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/host.h"
#include "harbor_setup/record.h"

namespace HarborSetup {

enum class Step { Welcome, Device, Keys, Firmware, Games, Dlc, Updates, Driver, Account, Ready };

enum class Tone { None, Ok, Warn, Bad };

struct Folder {
    std::string location; ///< path or tree URI, as the emulator reads it
    std::string name;     ///< the row's title: the folder's own name
    std::string detail;   ///< the row's second line: where it is
};

struct DriverChoice {
    std::string id; ///< empty: the phone's driver
    std::string title, detail;
};

struct SummaryRow {
    std::string label, value;
};

/// The OpenPak account page: sign in to an account, or create one and wait for its address to be
/// verified. An owner-approved exception to "the firmware does the account UI", for phones.
enum class AccountMode { SignIn, Create };
enum class AccountState { Form, Waiting, SignedIn };
enum class AccountField { Email, Password, Nickname, Birthday, Country };

/// The setup flow without any view: pages, their state, what Next/Back/Skip do, and the record.
/// A page's choices reach the record (and its file) only when Next succeeds on it.
/// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), FirstRunViewModel/FirstRunPages.
/// All methods run on the UI thread; slow work runs on a worker and comes back through `post`.
class Wizard {
public:
    using Post = std::function<void(std::function<void()>)>;

    Wizard(Host& host, std::filesystem::path record_path, const Readiness& readiness, DeviceInfo device,
           Post post);
    ~Wizard();

    Wizard(const Wizard&) = delete;
    Wizard& operator=(const Wizard&) = delete;

    /// Called after every change the view has to show.
    std::function<void()> changed;
    /// Called when only the progress words changed (the bar reads ProgressFraction every frame).
    std::function<void()> progress_changed;
    /// Called when a page is shown, with its note: one per visible page (Welcome is 0).
    std::function<void(int note)> page_shown;
    /// Called once "Start Harbor" saved the completed record.
    std::function<void()> finished;

    Step step() const {
        return current;
    }
    /// The current page's note for the cue: one per visible page, Welcome is 0.
    int CurrentNote() const {
        return Note(current);
    }
    const Record& record() const {
        return setup;
    }

    // Page chrome.
    std::string Title() const;
    std::string Lead() const;
    bool ShowHeader() const {
        return current != Step::Welcome;
    }
    bool Busy() const {
        return busy;
    }
    bool CanGoBack() const {
        return current != Step::Welcome && !busy;
    }
    bool CanGoNext() const;
    std::string NextLabel() const;
    bool CanSkip() const {
        return (current == Step::Driver || (current == Step::Account && account_state != AccountState::SignedIn)) &&
               !busy;
    }
    std::string SkipLabel() const {
        return current == Step::Account ? "Later" : "Skip";
    }
    bool CanCancel() const {
        return busy && cancellable && !cancel_requested;
    }
    /// The step dots: every shown page after Welcome. -1 done, 0 current, 1 upcoming.
    std::vector<int> StepDots() const;

    // The page's result strip and progress bar.
    const std::string& Status() const {
        return status;
    }
    Tone StatusTone() const {
        return status_tone;
    }
    const std::string& ProgressText() const {
        return progress_text;
    }
    /// Empty while the step's length is unknown.
    std::optional<double> ProgressFraction() const;

    // Device check.
    const std::vector<DeviceCheckRow>& Checks() const {
        return checks;
    }

    // Keys.
    bool KeysInstalled() const {
        return keys_usable;
    }
    std::string KeysChosen() const;
    void ChooseProdKeys();
    void ChooseTitleKeys();

    // Firmware.
    std::string FirmwareChosen() const;
    bool FirmwareFolderAllowed() const {
        return host.CanPickFirmwareFolder();
    }
    void ChooseFirmwareFile();
    void ChooseFirmwareFolder();

    // Games, DLC and updates.
    const std::vector<Folder>& Folders() const;
    std::string AddFolderLabel() const;
    void AddFolder();
    void RemoveFolder(size_t index);

    // Graphics driver.
    bool DriverPageShown() const {
        return driver_page;
    }
    const std::vector<DriverChoice>& DriverChoices() const {
        return driver_choices;
    }
    const std::string& SelectedDriver() const {
        return selected_driver;
    }
    void SelectDriver(size_t index);
    void InstallDriverFile();
    bool CanBrowseDrivers() const;
    std::string BrowseLabel() const {
        return downloads.empty() ? "Browse GitHub drivers" : "Refresh GitHub drivers";
    }
    std::string DownloadLead() const;
    void BrowseDrivers();
    const std::vector<DriverRelease>& Downloads() const {
        return downloads;
    }
    void Download(size_t index);

    // OpenPak account.
    AccountMode GetAccountMode() const {
        return account_mode;
    }
    AccountState GetAccountState() const {
        return account_state;
    }
    void SetAccountMode(AccountMode mode);
    /// The value as the row shows it (the password as dots), or empty.
    std::string AccountValue(AccountField field) const;
    /// The value to start an edit from (empty for the password).
    std::string AccountEditValue(AccountField field) const;
    void SetAccountField(AccountField field, std::string value);
    const std::string& AccountName() const {
        return account_name;
    }
    void ResendVerification();
    void UseAnotherAccount();

    // Ready.
    std::vector<SummaryRow> Summary() const;

    // Footer actions.
    void Next();
    void Back();
    void Skip();
    void Cancel();

    /// The first page that still needs the user, judged from the files and the record.
    static Step FirstIncomplete(const Readiness& readiness, const Record& record, bool has_driver_page);

private:
    void GoTo(Step step);
    bool Shown(Step step) const;
    Step Neighbour(Step step, int direction) const;
    int Note(Step step) const;
    void Enter();
    bool Save();
    void ShowStatus(std::string text, Tone tone);
    void StartWork(std::string stage, bool can_cancel, std::function<void()> work);
    void EndWork();
    void ReportProgress(std::string stage, std::optional<double> fraction);
    void Notify();
    void FillDriverChoices(const std::string& selected);
    void ChooseRecommendedDriver();
    void InstallDriverPackage(const std::string& location, const std::string& file_name);
    std::vector<Folder>* CurrentFolders();
    std::string AccountProblem() const;
    void EnterAccount();
    void SubmitAccount();
    void StartPolling();
    void StopPolling();
    void AccountSignedIn(std::string name);

    Host& host;
    std::filesystem::path record_path;
    DeviceInfo device;
    Post post;
    Record setup;

    Step current = Step::Welcome;
    bool driver_page = false;
    bool busy = false;
    std::atomic_bool cancellable{false};
    std::atomic_bool cancel_requested{false};
    std::string status;
    Tone status_tone = Tone::None;
    std::string progress_text;
    std::atomic<double> progress_fraction{-1.0};

    std::vector<DeviceCheckRow> checks;

    bool keys_usable = false;
    std::string keys_chosen;

    std::string firmware_installed; ///< version of the installed, ready firmware
    std::string firmware_problem;
    std::string firmware_chosen;
    std::string firmware_pending;   ///< version of a staged firmware waiting for Install

    std::vector<Folder> games, dlc, updates;

    std::vector<DriverChoice> driver_choices;
    std::string selected_driver;
    std::vector<DriverRelease> downloads;
    bool driver_auto_tried = false;     ///< Adreno 6xx: T30 chosen or fetched once per setup
    bool driver_auto_download = false;  ///< the next listing installs its recommended release

    AccountMode account_mode = AccountMode::SignIn;
    AccountState account_state = AccountState::Form;
    std::string account_email, account_password, account_nickname, account_birthday, account_country;
    std::string account_name;
    /// The verification poll's token: cleared to stop it (the poll thread is never waited for).
    std::shared_ptr<std::atomic_bool> poll_token;

    std::thread worker;
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
};

} // namespace HarborSetup
