// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/openpak_fetch.h"

#include <fstream>
#include <iterator>
#include <mutex>

namespace Service::BCAT {
namespace {

std::mutex fetcher_mutex;
std::shared_ptr<OpenPakFetcher> current_fetcher;

} // namespace

void SetOpenPakFetcher(std::shared_ptr<OpenPakFetcher> fetcher) {
    std::scoped_lock lock{fetcher_mutex};
    current_fetcher = std::move(fetcher);
}

std::shared_ptr<OpenPakFetcher> GetOpenPakFetcher() {
    std::scoped_lock lock{fetcher_mutex};
    return current_fetcher;
}

FetchResponse FileFetcher::Get(FetchOrigin origin, std::string_view path) {
    // Query strings do not name files.
    path = path.substr(0, path.find('?'));

    const std::filesystem::path relative = std::filesystem::path{std::string(path)}.relative_path();
    for (const auto& part : relative) {
        if (part == "..") {
            return {true, 404, {}};
        }
    }

    const auto file = root / (origin == FetchOrigin::NewsEdge ? "news" : "website") / relative;
    std::ifstream stream{file, std::ios::binary};
    std::error_code ec;
    if (!stream || !std::filesystem::is_regular_file(file, ec)) {
        return {true, 404, {}};
    }

    FetchResponse response;
    response.reached = true;
    response.status = 200;
    response.body.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return response;
}

} // namespace Service::BCAT
