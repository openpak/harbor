// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <filesystem>
#include <mutex>

#include <SimpleIni.h>
#include <fmt/format.h>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/am/window_system.h"
#include "harbor_setup/title_settings.h"

namespace HarborSetup::TitleSettings {

namespace {

std::mutex mutex;
std::optional<u64> running;

/// A setting given the running title's own value, and what it was before.
struct Applied {
    Settings::BasicSetting* setting;
    bool was_global;
    std::string previous; ///< its per-session value when it had one
};
std::vector<Applied> applied;

std::filesystem::path FileFor(u64 title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "custom" /
           fmt::format("{:016X}.ini", title_id);
}

// Eden's per-game file: every one of these settings is in the [Renderer] group.
const char* Section() {
    return Settings::TranslateCategory(Settings::Category::Renderer);
}

std::string Key(const Settings::BasicSetting& setting) {
    return setting.GetLabel();
}

bool Load(CSimpleIniA& ini, u64 title_id) {
    ini.SetUnicode(true);
    const auto path = FileFor(title_id);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }
    return ini.LoadFile(Common::FS::PathToUTF8String(path).c_str()) >= 0;
}

bool Save(CSimpleIniA& ini, u64 title_id) {
    const auto path = FileFor(title_id);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return ini.SaveFile(Common::FS::PathToUTF8String(path).c_str()) >= 0;
}

/// The title's own value for `setting`, if it has one.
std::optional<std::string> Override(const CSimpleIniA& ini, const Settings::BasicSetting& setting) {
    const auto key = Key(setting);
    const std::string use_global = ini.GetValue(Section(), (key + "\\use_global").c_str(), "true");
    if (use_global != "false") {
        return std::nullopt;
    }
    const std::string is_default = ini.GetValue(Section(), (key + "\\default").c_str(), "false");
    if (is_default == "true") {
        return setting.DefaultToString();
    }
    return std::string(ini.GetValue(Section(), key.c_str(), setting.DefaultToString().c_str()));
}

// mutex held
void ApplyLocked(Settings::BasicSetting& setting, const std::string& value) {
    const auto it = std::find_if(applied.begin(), applied.end(),
                                 [&](const Applied& a) { return a.setting == &setting; });
    if (it == applied.end()) {
        applied.push_back({&setting, setting.UsingGlobal(), setting.UsingGlobal() ? "" : setting.ToString()});
    }
    setting.SetGlobal(false);
    setting.LoadString(value);
}

// mutex held
void RestoreLocked(Settings::BasicSetting* only = nullptr) {
    for (auto it = applied.begin(); it != applied.end();) {
        if (only && it->setting != only) {
            ++it;
            continue;
        }
        if (it->was_global) {
            it->setting->SetGlobal(true);
        } else {
            it->setting->LoadString(it->previous);
        }
        it = applied.erase(it);
    }
}

/// Games and their demos: the firmware's own programs run as applications too (the first-time
/// setup's starter, 0100000000001012), and they keep the console-wide values.
bool IsGame(u64 title_id) {
    return title_id >= 0x0100000000010000ULL;
}

void Started(u64 title_id) {
    std::scoped_lock lk{mutex};
    RestoreLocked();
    if (!IsGame(title_id)) {
        running.reset();
        return;
    }
    running = title_id;
    CSimpleIniA ini;
    if (!Load(ini, title_id)) {
        return;
    }
    for (auto* setting : PerGame()) {
        if (const auto value = Override(ini, *setting)) {
            ApplyLocked(*setting, *value);
            LOG_INFO(Frontend, "harbor: {:016X} runs with its own {} ({})", title_id, setting->GetLabel(), *value);
        }
    }
}

void Exited(u64 title_id) {
    std::scoped_lock lk{mutex};
    if (!applied.empty()) {
        LOG_INFO(Frontend, "harbor: {:016X} exited: the console-wide settings are back", title_id);
    }
    RestoreLocked();
    running.reset();
}

} // namespace

const std::vector<Settings::BasicSetting*>& PerGame() {
    static const std::vector<Settings::BasicSetting*> settings{
        &Settings::values.use_asynchronous_shaders,
        &Settings::values.frame_gen,
        &Settings::values.frame_gen_multiplier,
        &Settings::values.frame_gen_target_rate,
        &Settings::values.frame_gen_flow_scale_auto,
        &Settings::values.frame_gen_flow_scale,
    };
    return settings;
}

void Install(Core::System&) {
    static std::once_flag once;
    std::call_once(once, [] { Service::AM::SetApplicationHooks(Started, Exited); });
}

void SimulateForTest(std::optional<u64> title_id) {
    if (title_id) {
        Started(*title_id);
        return;
    }
    if (const auto title = RunningTitle()) {
        Exited(*title);
    }
}

std::optional<u64> RunningTitle() {
    std::scoped_lock lk{mutex};
    return running;
}

std::string TitleName(Core::System& system, u64 title_id) {
    try {
        const FileSys::PatchManager pm{title_id, system.GetFileSystemController(), system.GetContentProvider()};
        const auto metadata = pm.GetControlMetadata();
        if (metadata.first) {
            auto name = metadata.first->GetApplicationName();
            if (!name.empty()) {
                return name;
            }
        }
    } catch (...) {
    }
    return fmt::format("{:016X}", title_id);
}

bool HasOverride(u64 title_id, const Settings::BasicSetting& setting) {
    std::scoped_lock lk{mutex};
    CSimpleIniA ini;
    return Load(ini, title_id) && Override(ini, setting).has_value();
}

void SetOverride(u64 title_id, Settings::BasicSetting& setting, const std::string& value) {
    std::scoped_lock lk{mutex};
    CSimpleIniA ini;
    Load(ini, title_id);
    const auto key = Key(setting);
    ini.SetValue(Section(), (key + "\\use_global").c_str(), "false");
    ini.SetValue(Section(), (key + "\\default").c_str(), value == setting.DefaultToString() ? "true" : "false");
    ini.SetValue(Section(), key.c_str(), value.c_str());
    if (!Save(ini, title_id)) {
        LOG_ERROR(Frontend, "harbor: {:016X}'s own settings could not be saved", title_id);
    }
    LOG_INFO(Frontend, "harbor: {:016X} has its own {} ({})", title_id, key, value);
    if (running == title_id) {
        ApplyLocked(setting, value);
    }
}

void ClearOverride(u64 title_id, Settings::BasicSetting& setting) {
    std::scoped_lock lk{mutex};
    CSimpleIniA ini;
    Load(ini, title_id);
    const auto key = Key(setting);
    ini.SetValue(Section(), (key + "\\use_global").c_str(), "true");
    ini.Delete(Section(), (key + "\\default").c_str());
    ini.Delete(Section(), key.c_str());
    Save(ini, title_id);
    LOG_INFO(Frontend, "harbor: {:016X} uses the console-wide {} again", title_id, key);
    if (running == title_id) {
        RestoreLocked(&setting);
    }
}

void SetGlobalValue(Settings::BasicSetting& setting, const std::string& value) {
    std::scoped_lock lk{mutex};
    const bool was_global = setting.UsingGlobal();
    setting.SetGlobal(true);
    setting.LoadString(value);
    setting.SetGlobal(was_global);
}

std::string GlobalValue(const Settings::BasicSetting& setting) {
    return setting.ToStringGlobal();
}

} // namespace HarborSetup::TitleSettings
