// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/hle/service/bcat/news/news_record.h"

namespace Service::News {

// The news database HOME's News queries and the article files it opens. A console keeps them in an
// SQLite database and news:/data on save 0x8000000000000090; here they are one JSON file of rows and
// one file per article in the Harbor data directory. PostLocalNews writes into it, and so does the
// OpenPak fill, which takes the news server's local surface rather than BCAT delivery.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
class NewsStorage {
public:
    explicit NewsStorage(std::filesystem::path directory_);

    // Store an article payload; false (logged) when it is not a news msgpack or cannot be written. A
    // repost replaces the row whole, the reader's read/displayed state included.
    bool Post(std::span<const u8> payload, s64 received_at);

    // The stored payload size of the delivered article `news_id`, or -1 when it is not stored.
    s64 StoredSize(std::string_view news_id) const;

    // The rows `where` matches, in `order`. Throws QueryError.
    std::vector<NewsRow> Query(std::string_view where, std::string_view order) const;

    // Set, or add to, an integer column of every matching row; the number changed. Throws QueryError.
    int Update(std::string_view column, s64 value, bool add, std::string_view where);

    // An article by its news:/data name. Only names of stored rows are looked up.
    std::optional<std::vector<u8>> Payload(std::string_view file_name) const;

    std::vector<std::string> Topics() const;
    u64 UsedBytes() const;
    void Clear();

    // Remove every row of `topic` and its article files; the number removed.
    int RemoveTopic(std::string_view topic);
    // Remove every row whose topic is not in `keep`; the number removed.
    int RemoveTopicsNotIn(const std::vector<std::string>& keep);

private:
    std::filesystem::path RecordsPath() const;
    void Load();
    void Save() const;

    std::filesystem::path directory;
    mutable std::mutex mutex;
    std::vector<NewsRow> rows;
};

} // namespace Service::News
