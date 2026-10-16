// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/openpak/platform.h"
#include "core/openpak/platform_titles.h"
#include "core/openpak/session.h"

namespace OpenPak::PlatformTitles {

namespace {

constexpr auto RetryAfter = std::chrono::minutes(5);

std::mutex mutex;
/// When each id was last tried, so a server that does not have one is not asked every second.
std::map<std::uint64_t, std::chrono::steady_clock::time_point> tried;

std::filesystem::path PathOf(std::uint64_t id) {
    return Platform::CacheDir() / "platform-titles" / fmt::format("{:016x}.nca", id);
}

} // namespace

std::vector<std::uint8_t> Cached(std::uint64_t application_id) {
    if (application_id == 0) {
        return {};
    }
    std::ifstream file(PathOf(application_id), std::ios::binary);
    if (!file) {
        return {};
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Prefetch(std::uint64_t application_id, std::function<void()> done) {
    if (application_id == 0 || !Session::Enabled()) {
        return;
    }
    {
        std::lock_guard lock{mutex};
        const auto now = std::chrono::steady_clock::now();
        if (const auto it = tried.find(application_id); it != tried.end() && now - it->second < RetryAfter) {
            return;
        }
        tried[application_id] = now;
    }
    if (!Cached(application_id).empty()) {
        return;
    }
    std::thread([application_id, done = std::move(done)] {
        try {
            const std::vector<std::uint8_t> nca =
                Session::FetchAtumControl(fmt::format("{:016x}", application_id));
            if (nca.empty()) {
                return;
            }
            const auto path = PathOf(application_id);
            Platform::CreateParentDirs(path);
            // Beside itself, then renamed: a reader never sees half a file.
            const auto tmp = std::filesystem::path{path}.concat(".part");
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(nca.data()),
                          static_cast<std::streamsize>(nca.size()));
            }
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
                return;
            }
            LOG_INFO(WebService, "[OpenPak] A title the server named was fetched ({} bytes)", nca.size());
            if (done) {
                done();
            }
        } catch (const std::exception& e) {
            LOG_WARNING(WebService, "[OpenPak] Platform title: {}", e.what());
        }
    }).detach();
}

} // namespace OpenPak::PlatformTitles
