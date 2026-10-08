// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// erpt's report store, laid out as firmware 23.0.1's erpt keeps it in its system save
// (0x80000000000000D1): "journal" holds a u32 count and that many 0x70-byte ReportInfo records,
// newest first; each report's MessagePack body is a file named after its UUID
// ("%08x-%04x-%04x-%02x%02x-%04x%08x"). At most 50 reports: when full, the oldest report of the
// type with more stored reports goes first, as erpt's journal evicts. Settings > Support > Error
// History lists the Visible ones after a restart, as on a console.
//
// Standard library only (no Eden types), so it can be checked on its own: see
// erpt_store_selfcheck.cpp.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Service::ERPT {

/// 16 random UUID bytes and a u32 (0 unless the caller chose the id).
using ReportId = std::array<std::uint8_t, 0x14>;
using ReportMetaData = std::array<std::uint8_t, 0x20>;

enum ReportType : std::uint32_t {
    ReportType_Visible = 0,
    ReportType_Invisible = 1,
    ReportType_Any = 2, // list filter only
};

/// nn::erpt::ReportInfo, 0x70 bytes.
struct ReportInfo {
    std::uint32_t type;
    ReportId id;
    ReportMetaData meta_data;
    std::uint32_t flags;
    std::uint32_t flags_set; // erpt keeps the flags in two words; GetFlags answers both ORed
    std::int64_t timestamp_user;
    std::int64_t timestamp_network;
    std::int64_t size;
    std::array<std::uint64_t, 3> reserved;
};
static_assert(sizeof(ReportInfo) == 0x70);

class ReportStore {
public:
    static constexpr std::size_t MaxReports = 50;

    explicit ReportStore(std::filesystem::path directory_) : directory{std::move(directory_)} {}

    /// Reads the journal from the save; a missing or damaged journal is an empty store.
    void Load() {
        std::scoped_lock lk{mutex};
        records.clear();
        std::ifstream file{directory / "journal", std::ios::binary};
        if (!file) {
            return;
        }
        std::uint32_t count{};
        if (!Read(file, count) || count > MaxReports) {
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            ReportInfo info{};
            if (!Read(file, info) || info.type > ReportType_Invisible) {
                records.clear();
                return;
            }
            // A record whose report file is gone is dropped.
            if (std::filesystem::exists(BodyPath(info.id))) {
                records.push_back(info);
            }
        }
    }

    /// Stores a new report at the head of the journal, evicting one first when full.
    bool Add(ReportInfo info, const std::vector<std::uint8_t>& body) {
        std::scoped_lock lk{mutex};
        if (std::any_of(records.begin(), records.end(),
                        [&](const ReportInfo& r) { return r.id == info.id; })) {
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        {
            std::ofstream file{BodyPath(info.id), std::ios::binary | std::ios::trunc};
            if (!file) {
                return false;
            }
            file.write(reinterpret_cast<const char*>(body.data()),
                       static_cast<std::streamsize>(body.size()));
            if (!file) {
                return false;
            }
        }
        if (records.size() >= MaxReports) {
            EvictLocked(info.type);
        }
        info.size = static_cast<std::int64_t>(body.size());
        records.insert(records.begin(), info);
        return SaveLocked();
    }

    /// The records of one type, or all of them for ReportType_Any, newest first, at most 50.
    std::vector<ReportInfo> List(std::uint32_t type) const {
        std::scoped_lock lk{mutex};
        std::vector<ReportInfo> out;
        for (const auto& r : records) {
            if (type == ReportType_Any || r.type == type) {
                out.push_back(r);
            }
        }
        return out;
    }

    std::optional<ReportInfo> Find(const ReportId& id) const {
        std::scoped_lock lk{mutex};
        for (const auto& r : records) {
            if (r.id == id) {
                return r;
            }
        }
        return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> ReadBody(const ReportId& id) const {
        std::scoped_lock lk{mutex};
        std::ifstream file{BodyPath(id), std::ios::binary};
        if (!file) {
            return std::nullopt;
        }
        return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{file},
                                         std::istreambuf_iterator<char>{}};
    }

    /// IReport 2: the bits are added (erpt ORs them in).
    bool SetFlags(const ReportId& id, std::uint32_t flags) {
        std::scoped_lock lk{mutex};
        for (auto& r : records) {
            if (r.id == id) {
                r.flags_set |= flags;
                r.flags = r.flags_set;
                return SaveLocked();
            }
        }
        return false;
    }

    bool Delete(const ReportId& id) {
        std::scoped_lock lk{mutex};
        const auto it = std::find_if(records.begin(), records.end(),
                                     [&](const ReportInfo& r) { return r.id == id; });
        if (it == records.end()) {
            return false;
        }
        std::error_code ec;
        std::filesystem::remove(BodyPath(id), ec);
        records.erase(it);
        return SaveLocked();
    }

    void Clear() {
        std::scoped_lock lk{mutex};
        std::error_code ec;
        for (const auto& r : records) {
            std::filesystem::remove(BodyPath(r.id), ec);
        }
        records.clear();
        SaveLocked();
    }

    std::uint64_t UsedSize() const {
        std::scoped_lock lk{mutex};
        std::uint64_t total = 0;
        for (const auto& r : records) {
            total += static_cast<std::uint64_t>(r.size);
        }
        return total;
    }

    /// The report file name erpt uses for an id.
    static std::string FileName(const ReportId& id) {
        const auto le = [&](std::size_t at, std::size_t bytes) {
            std::uint32_t v = 0;
            for (std::size_t i = 0; i < bytes; ++i) {
                v |= static_cast<std::uint32_t>(id[at + i]) << (8 * i);
            }
            return v;
        };
        char name[40];
        std::snprintf(name, sizeof(name), "%08x-%04x-%04x-%02x%02x-%04x%08x", le(0, 4), le(4, 2),
                      le(6, 2), id[8], id[9], le(10, 2), le(12, 4));
        return name;
    }

private:
    std::filesystem::path BodyPath(const ReportId& id) const {
        return directory / FileName(id);
    }

    // Journal::StoreRecord: the victim type is the one with more reports (on a tie the new
    // report's type before Visible, either before Invisible); its oldest report goes.
    void EvictLocked(std::uint32_t new_type) {
        std::array<std::size_t, 2> count{};
        for (const auto& r : records) {
            ++count[r.type & 1];
        }
        std::uint32_t victim = count[0] <= count[new_type & 1] ? new_type : ReportType_Visible;
        if (std::max(count[0], count[new_type & 1]) < count[1]) {
            victim = ReportType_Invisible;
        }
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            if (it->type == victim) {
                std::error_code ec;
                std::filesystem::remove(BodyPath(it->id), ec);
                records.erase(std::next(it).base());
                return;
            }
        }
        std::error_code ec;
        std::filesystem::remove(BodyPath(records.back().id), ec);
        records.pop_back();
    }

    template <typename T>
    static bool Read(std::istream& in, T& value) {
        return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
    }

    bool SaveLocked() const {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        const auto temp = directory / "journal.tmp";
        {
            std::ofstream file{temp, std::ios::binary | std::ios::trunc};
            if (!file) {
                return false;
            }
            const auto count = static_cast<std::uint32_t>(records.size());
            file.write(reinterpret_cast<const char*>(&count), sizeof(count));
            file.write(reinterpret_cast<const char*>(records.data()),
                       static_cast<std::streamsize>(records.size() * sizeof(ReportInfo)));
            if (!file) {
                return false;
            }
        }
        std::filesystem::rename(temp, directory / "journal", ec);
        return !ec;
    }

    std::filesystem::path directory;
    mutable std::mutex mutex;
    std::vector<ReportInfo> records;
};

} // namespace Service::ERPT
