// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Service::BCAT {

struct FetchResponse {
    // False when no answer arrived (offline, no OpenPak, timeout). Then `status` and `body` are empty.
    bool reached{};
    int status{};
    std::vector<u8> body;
};

// The narrow HTTP surface the news store needs: GET on OpenPak's news edge (the
// bcat-topics-lp1.cdn.nintendo.net name the OpenPak console server answers for, plaintext local
// surface /openpak/v1/local/*). Get blocks, so it runs on a worker thread, never on an IPC thread,
// and is bounded by the implementation's own timeouts. The default implementation goes through
// OpenPak::Session::Send (pinned CA, redirected to the console server, no sign-in); FileFetcher
// stands in for tests. A title's BCAT data goes through OpenPak::Bcat::Fetch instead.
class OpenPakFetcher {
public:
    virtual ~OpenPakFetcher() = default;
    virtual FetchResponse Get(std::string_view path) = 0;
};

// The fetcher in use: the one installed, else the OpenPak session's. Never null; the session's
// answers "not reached" while OpenPak is off, so nothing is fetched then.
void SetOpenPakFetcher(std::shared_ptr<OpenPakFetcher> fetcher);
std::shared_ptr<OpenPakFetcher> GetOpenPakFetcher();

// Whether a fetcher other than the session's was installed (tests).
bool OpenPakFetcherIsStub();

// Answers from files: <root>/<path>, 200 for a file, 404 for none. For self-checks and desktop runs
// without a server.
class FileFetcher final : public OpenPakFetcher {
public:
    explicit FileFetcher(std::filesystem::path root_) : root{std::move(root_)} {}
    FetchResponse Get(std::string_view path) override;

private:
    std::filesystem::path root;
};

} // namespace Service::BCAT
