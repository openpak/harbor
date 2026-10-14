// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/news/news_storage.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "core/hle/service/bcat/news/news_query.h"

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::News {
namespace {

bool SameRow(const NewsRow& a, const NewsRow& b) {
    return a.news_id == b.news_id && a.user_id == b.user_id;
}

} // namespace

NewsStorage::NewsStorage(std::filesystem::path directory_) : directory{std::move(directory_)} {
    Load();
}

std::filesystem::path NewsStorage::RecordsPath() const {
    return directory / "records.json";
}

void NewsStorage::Load() {
    std::error_code ec;
    if (!std::filesystem::exists(RecordsPath(), ec)) {
        return;
    }

    try {
        std::ifstream stream{RecordsPath()};
        const auto json = nlohmann::json::parse(stream);

        for (const auto& item : json) {
            NewsRow row;
            row.news_id = item.at("news_id").get<std::string>();
            row.user_id = item.at("user_id").get<std::string>();
            row.topic_id = item.at("topic_id").get<std::string>();
            row.application_ids = item.at("application_ids").get<std::string>();
            for (size_t i = 0; i < IntegerColumnNames.size(); ++i) {
                row.ints[i] = item.value(std::string(IntegerColumnNames[i]), s64{0});
            }
            rows.push_back(std::move(row));
        }
    } catch (const std::exception& e) {
        LOG_WARNING(Service_BCAT, "News records unreadable ({}); starting empty", e.what());
        rows.clear();
    }
}

void NewsStorage::Save() const {
    try {
        std::filesystem::create_directories(directory);

        auto json = nlohmann::json::array();
        for (const auto& row : rows) {
            nlohmann::json item;
            item["news_id"] = row.news_id;
            item["user_id"] = row.user_id;
            item["topic_id"] = row.topic_id;
            item["application_ids"] = row.application_ids;
            for (size_t i = 0; i < IntegerColumnNames.size(); ++i) {
                item[std::string(IntegerColumnNames[i])] = row.ints[i];
            }
            json.push_back(std::move(item));
        }

        std::ofstream stream{RecordsPath(), std::ios::trunc};
        stream << json.dump();
    } catch (const std::exception& e) {
        LOG_WARNING(Service_BCAT, "News records could not be saved: {}", e.what());
    }
}

bool NewsStorage::Post(std::span<const u8> payload, s64 received_at) {
    auto row = NewsRow::FromPayload(payload, received_at);
    if (!row) {
        LOG_WARNING(Service_BCAT,
                    "News payload of {} bytes has no msgpack map with an integer news_id; not stored",
                    payload.size());
        return false;
    }

    std::scoped_lock lock{mutex};

    try {
        std::filesystem::create_directories(directory);
        std::ofstream stream{directory / row->FileName(), std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(payload.data()),
                     static_cast<std::streamsize>(payload.size()));
        if (!stream) {
            throw std::runtime_error("write failed");
        }
    } catch (const std::exception& e) {
        LOG_WARNING(Service_BCAT, "News article could not be written: {}", e.what());
        return false;
    }

    std::erase_if(rows, [&](const NewsRow& other) { return SameRow(other, *row); });
    rows.push_back(std::move(*row));
    Save();
    return true;
}

s64 NewsStorage::StoredSize(std::string_view news_id) const {
    std::scoped_lock lock{mutex};

    const auto it = std::find_if(rows.begin(), rows.end(), [&](const NewsRow& row) {
        return row.news_id == news_id && row.user_id.empty();
    });
    if (it == rows.end()) {
        return -1;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(directory / it->FileName(), ec);
    return ec ? -1 : static_cast<s64>(size);
}

std::vector<NewsRow> NewsStorage::Query(std::string_view where, std::string_view order) const {
    const auto match = ParseWhere(where);

    std::vector<NewsRow> matching;
    {
        std::scoped_lock lock{mutex};
        std::copy_if(rows.begin(), rows.end(), std::back_inserter(matching), match);
    }
    return OrderRows(std::move(matching), order);
}

int NewsStorage::Update(std::string_view column, s64 value, bool add, std::string_view where) {
    const auto index = NewsRow::FindIntegerColumn(column);
    if (!index) {
        throw QueryError("'" + std::string(column) + "' is not an integer column");
    }

    const auto match = ParseWhere(where);

    std::scoped_lock lock{mutex};
    int changed = 0;
    for (auto& row : rows) {
        if (match(row)) {
            row.Int(*index) = add ? row.Int(*index) + value : value;
            ++changed;
        }
    }
    if (changed > 0) {
        Save();
    }
    return changed;
}

std::optional<std::vector<u8>> NewsStorage::Payload(std::string_view file_name) const {
    std::scoped_lock lock{mutex};

    const bool stored = std::any_of(rows.begin(), rows.end(),
                                    [&](const NewsRow& row) { return row.FileName() == file_name; });
    if (!stored) {
        return std::nullopt;
    }

    std::ifstream stream{directory / std::string(file_name), std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::vector<std::string> NewsStorage::Topics() const {
    std::scoped_lock lock{mutex};

    std::vector<std::string> topics;
    for (const auto& row : rows) {
        if (!row.topic_id.empty() &&
            std::find(topics.begin(), topics.end(), row.topic_id) == topics.end()) {
            topics.push_back(row.topic_id);
        }
    }
    return topics;
}

u64 NewsStorage::UsedBytes() const {
    std::scoped_lock lock{mutex};

    u64 total = 0;
    for (const auto& row : rows) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(directory / row.FileName(), ec);
        if (!ec) {
            total += size;
        }
    }
    return total;
}

int NewsStorage::RemoveTopic(std::string_view topic) {
    std::scoped_lock lock{mutex};
    int removed = 0;
    for (auto it = rows.begin(); it != rows.end();) {
        if (it->topic_id == topic) {
            std::error_code ec;
            std::filesystem::remove(directory / it->FileName(), ec);
            it = rows.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        Save();
    }
    return removed;
}

int NewsStorage::RemoveTopicsNotIn(const std::vector<std::string>& keep) {
    std::scoped_lock lock{mutex};
    int removed = 0;
    for (auto it = rows.begin(); it != rows.end();) {
        if (std::find(keep.begin(), keep.end(), it->topic_id) == keep.end()) {
            std::error_code ec;
            std::filesystem::remove(directory / it->FileName(), ec);
            it = rows.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        Save();
    }
    return removed;
}

void NewsStorage::Clear() {
    std::scoped_lock lock{mutex};

    for (const auto& row : rows) {
        std::error_code ec;
        std::filesystem::remove(directory / row.FileName(), ec);
    }
    rows.clear();
    Save();
}

} // namespace Service::News
