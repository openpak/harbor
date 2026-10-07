// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/openpak_fetch.h"

#include <fstream>
#include <iterator>
#include <mutex>

#include "core/openpak/session.h"

namespace Service::BCAT {
namespace {

// The news edge is reached by this name through the OpenPak console server.
constexpr const char* NewsEdgeHost = "bcat-topics-lp1.cdn.nintendo.net";

class SessionFetcher final : public OpenPakFetcher {
public:
    FetchResponse Get(std::string_view path) override {
        if (!OpenPak::Session::Enabled()) {
            return {};
        }
        const auto response =
            OpenPak::Session::Send("GET", NewsEdgeHost, std::string(path), {}, {}, {});
        if (response.status == 0) {
            return {};
        }
        FetchResponse out;
        out.reached = true;
        out.status = response.status;
        out.body.assign(response.body.begin(), response.body.end());
        return out;
    }
};

std::mutex fetcher_mutex;
std::shared_ptr<OpenPakFetcher> installed;

} // namespace

void SetOpenPakFetcher(std::shared_ptr<OpenPakFetcher> fetcher) {
    std::scoped_lock lock{fetcher_mutex};
    installed = std::move(fetcher);
}

std::shared_ptr<OpenPakFetcher> GetOpenPakFetcher() {
    static const auto session = std::make_shared<SessionFetcher>();

    std::scoped_lock lock{fetcher_mutex};
    return installed ? installed : session;
}

bool OpenPakFetcherIsStub() {
    std::scoped_lock lock{fetcher_mutex};
    return installed != nullptr;
}

FetchResponse FileFetcher::Get(std::string_view path) {
    // Query strings do not name files.
    path = path.substr(0, path.find('?'));

    const std::filesystem::path relative = std::filesystem::path{std::string(path)}.relative_path();
    for (const auto& part : relative) {
        if (part == "..") {
            return {true, 404, {}};
        }
    }

    const auto file = root / relative;
    std::error_code ec;
    std::ifstream stream{file, std::ios::binary};
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
