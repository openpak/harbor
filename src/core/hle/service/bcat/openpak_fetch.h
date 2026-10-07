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

// Which OpenPak origin a request goes to. Harbor contacts no other network.
enum class FetchOrigin {
    // The news edge (the cdn.nintendo.net name the OpenPak console server answers for), plaintext
    // local surface: /openpak/v1/local/*.
    NewsEdge,
    // OpenPak's website: /api/emulator/v1/bcat/titles/<tid> and the file URLs it lists.
    Website,
};

struct FetchResponse {
    // False when no answer arrived (offline, no OpenPak, timeout). Then `status` and `body` are empty.
    bool reached{};
    int status{};
    std::vector<u8> body;
};

// The narrow HTTP surface the news store and the BCAT backend need. Get runs on a worker thread, never
// on an IPC thread, and must be bounded by the implementation's own timeouts. The real implementation
// comes from the core stream (src/core/openpak/); FileFetcher below stands in for tests.
class OpenPakFetcher {
public:
    virtual ~OpenPakFetcher() = default;
    virtual FetchResponse Get(FetchOrigin origin, std::string_view path) = 0;
};

// The fetcher in use, or null while OpenPak is off or not up yet. Nothing is fetched without one.
void SetOpenPakFetcher(std::shared_ptr<OpenPakFetcher> fetcher);
std::shared_ptr<OpenPakFetcher> GetOpenPakFetcher();

// Answers from files: <root>/news/<path> for the news edge and <root>/website/<path> for the website,
// 200 for a file, 404 for none. For self-checks and desktop runs without a server.
class FileFetcher final : public OpenPakFetcher {
public:
    explicit FileFetcher(std::filesystem::path root_) : root{std::move(root_)} {}
    FetchResponse Get(FetchOrigin origin, std::string_view path) override;

private:
    std::filesystem::path root;
};

} // namespace Service::BCAT
