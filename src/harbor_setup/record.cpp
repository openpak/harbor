// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "harbor_setup/record.h"

namespace HarborSetup {

namespace {

template <typename T>
void Put(nlohmann::json& json, const char* key, const std::optional<T>& value) {
    if (value) {
        json[key] = *value;
    } else {
        json[key] = nullptr;
    }
}

template <typename T>
std::optional<T> Get(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
        return std::nullopt;
    }
    return it->get<T>();
}

} // namespace

std::vector<std::string> Record::AutoloadDirs() const {
    std::vector<std::string> dirs;
    for (const auto* list : {&dlc_dirs, &update_dirs}) {
        if (!*list) {
            continue;
        }
        for (const auto& dir : **list) {
            if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) {
                dirs.push_back(dir);
            }
        }
    }
    return dirs;
}

std::filesystem::path RecordPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / RecordFileName;
}

std::string ToJson(const Record& record) {
    nlohmann::json json;
    json["keys_present"] = record.keys_present;
    Put(json, "firmware_version", record.firmware_version);
    Put(json, "game_dirs", record.game_dirs);
    Put(json, "dlc_dirs", record.dlc_dirs);
    Put(json, "update_dirs", record.update_dirs);
    Put(json, "gpu_driver", record.gpu_driver);
    Put(json, "openpak_account", record.openpak_account);
    json["completed"] = record.completed;
    return json.dump(2);
}

std::optional<Record> FromJson(const std::string& text) {
    try {
        const auto json = nlohmann::json::parse(text);
        if (!json.is_object()) {
            return std::nullopt;
        }
        Record record;
        record.keys_present = json.value("keys_present", false);
        record.firmware_version = Get<std::string>(json, "firmware_version");
        record.game_dirs = Get<std::vector<std::string>>(json, "game_dirs");
        record.dlc_dirs = Get<std::vector<std::string>>(json, "dlc_dirs");
        record.update_dirs = Get<std::vector<std::string>>(json, "update_dirs");
        record.gpu_driver = Get<std::string>(json, "gpu_driver");
        record.openpak_account = Get<std::string>(json, "openpak_account");
        record.completed = json.value("completed", false);
        return record;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Record LoadRecord(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::stringstream text;
    text << file.rdbuf();
    if (auto record = FromJson(text.str())) {
        return *record;
    }
    LOG_WARNING(Frontend, "harbor: {} could not be read; setup starts again", path.string());
    return {};
}

bool SaveRecord(const Record& record, const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << ToJson(record) << '\n';
        file.flush();
        if (!file) {
            if (error) {
                *error = "the file could not be written";
            }
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        if (error) {
            *error = ec.message();
        }
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

} // namespace HarborSetup
