// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/openpak/real_firmware.h"

namespace OpenPak::RealFirmware {

namespace {

// The firmware's system programs live in this id range; a name on the list is looked up here.
constexpr u64 SystemProgramFirst = 0x0100000000000000ULL;
constexpr u64 SystemProgramLast = 0x0100000000000080ULL;

// Every pure-logic module. Not here: account (acc:u0/u1/su are hosted by ns, which Harbor's
// loader-bound HLE still owns, so the real module alone would split the user list), ns, es,
// erpt/fatal (Harbor's crash reports), settings (Harbor's config), and anything with hardware.
constexpr u64 DefaultPrograms[] = {
    0x010000000000000CULL, // bcat: bcat, news, prepo, mnpp
    0x010000000000000EULL, // friends
    0x0100000000000025ULL, // nim: nim, ntc
    0x010000000000002EULL, // pctl
    0x010000000000002FULL, // npns
    0x0100000000000030ULL, // eupld
    0x0100000000000039ULL, // sdb: mii, pdm, pl:s, avm
    0x010000000000003AULL, // migration
    0x010000000000003EULL, // olsc
    0x0100000000000050ULL, // ngc
};

std::string NameOf(const FileSys::ProgramMetadata& metadata) {
    const auto& raw = metadata.GetName();
    return std::string{reinterpret_cast<const char*>(raw.data()),
                       strnlen(reinterpret_cast<const char*>(raw.data()), raw.size())};
}

std::optional<u64> ResolveName(Core::System& system, std::string_view name) {
    for (u64 id = SystemProgramFirst; id <= SystemProgramLast; ++id) {
        const auto metadata = FileSys::ProgramMetadata::LoadInstalled(system.GetContentProvider(), id);
        if (metadata && NameOf(*metadata) == name) {
            return id;
        }
    }
    return std::nullopt;
}

struct Resolved {
    std::vector<u64> programs;
    std::set<std::string> hosted;
    std::map<std::string, u64> host_of;
};

const Resolved& Get(Core::System& system) {
    static std::once_flag once;
    static Resolved resolved;
    std::call_once(once, [&system] {
        std::vector<std::string> tokens;
        if (const char* env = std::getenv("OPENPAK_REAL_PROGRAMS"); env != nullptr) {
            std::string list{env};
            std::size_t start = 0;
            while (start <= list.size()) {
                const auto stop = list.find(',', start);
                const auto token = list.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
                if (!token.empty() && token != "none") {
                    tokens.push_back(token);
                }
                if (stop == std::string::npos) {
                    break;
                }
                start = stop + 1;
            }
        } else {
            tokens.emplace_back("default");
        }
        for (const auto& token : tokens) {
            if (token == "default") {
                resolved.programs.insert(resolved.programs.end(), std::begin(DefaultPrograms),
                                         std::end(DefaultPrograms));
                continue;
            }
            char* end = nullptr;
            const u64 id = std::strtoull(token.c_str(), &end, 16);
            if (*end == '\0' && token.size() == 16) {
                resolved.programs.push_back(id);
            } else if (const auto by_name = ResolveName(system, token)) {
                resolved.programs.push_back(*by_name);
            } else {
                LOG_WARNING(Service, "real firmware: no installed system program is named '{}'", token);
            }
        }
        std::sort(resolved.programs.begin(), resolved.programs.end());
        resolved.programs.erase(std::unique(resolved.programs.begin(), resolved.programs.end()),
                                resolved.programs.end());
        for (const u64 id : resolved.programs) {
            const auto metadata = FileSys::ProgramMetadata::LoadInstalled(system.GetContentProvider(), id);
            if (!metadata) {
                LOG_WARNING(Service, "real firmware: program {:016X} is not installed; Harbor keeps serving its services", id);
                continue;
            }
            std::string names;
            for (const auto& entry : metadata->GetServiceAccessList()) {
                if (entry.is_host) {
                    resolved.hosted.insert(entry.name);
                    resolved.host_of[entry.name] = id;
                    names += " " + entry.name;
                }
            }
            LOG_INFO(Service, "real firmware: {} ({:016X}) will host:{}", NameOf(*metadata), id, names);
        }
    });
    return resolved;
}

} // namespace

const std::vector<u64>& Programs(Core::System& system) {
    return Get(system).programs;
}

std::optional<u64> ProgramHosting(Core::System& system, std::string_view service_name) {
    const auto& host_of = Get(system).host_of;
    const auto it = host_of.find(std::string{service_name});
    return it == host_of.end() ? std::nullopt : std::optional<u64>{it->second};
}

bool Hosts(Core::System& system, std::string_view service_name) {
    const auto& hosted = Get(system).hosted;
    return hosted.find(std::string{service_name}) != hosted.end();
}

} // namespace OpenPak::RealFirmware
