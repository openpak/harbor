// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Harbor's crash reports. Off until the user agrees: then a native crash (a fatal signal) or a
// guest fatal error (fatal:u / fatal:p) leaves one report in <data>/crash-reports/, scrubbed of
// account ids, tokens, names and typed text before it is written. Nothing is ever sent: the next
// launch offers the newest report once (view it, share it by the user's own action, delete it).
//
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): message capped at 2000
// characters, the last 500 log lines, under 20 MiB, at most 5 kept, half-written ones discarded.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Common::HarborCrash {

enum class Consent { Unasked, Accepted, Declined };

/// <data>/crash-reports.
std::filesystem::path DefaultDirectory();

/// Reads the consent, finishes or discards what a crash left half-written and, with consent,
/// installs the fatal-signal handler and starts keeping the log tail. Call once per process,
/// early: before the JIT installs its fault handler, so that one chains to this one. An empty
/// `dir` is DefaultDirectory().
void Install(std::filesystem::path dir = {});

Consent GetConsent();

/// Records the user's answer. Accepting starts recording at once; declining stops it and deletes
/// every report kept so far.
void SetConsent(bool accepted);

/// Whether log lines are being kept for a report (consent given and Install called).
bool Recording();

/// One formatted log line, from the logger. Scrubbed as it is kept.
void OnLogLine(std::string_view line);

/// A guest fatal error: writes a report at once. Never throws.
void RecordGuestFatal(u64 title_id, std::string_view error_code, u32 result,
                      std::string_view details);

struct Report {
    std::filesystem::path path;
    std::string name;    ///< file name, e.g. harbor-crash-20261008-101500-native.txt
    std::string summary; ///< one line: what crashed
    std::string when;    ///< "2026-10-08 10:15:00 UTC"
};

/// Saved reports, newest first.
std::vector<Report> List();

/// The newest report the user has not been offered yet, marked as offered: each one is offered
/// at most once. Nothing when the consent is not given.
std::optional<Report> TakeOffer();

/// The report's text (already scrubbed), empty when it cannot be read.
std::string Read(const Report& report);

void Delete(const Report& report);
void DeleteAll();

/// What every report goes through before it is written: 32-digit and dashed uids, URLs past
/// their host, e-mail addresses, sensitive key=value pairs (account_id, user_id, token, nickname,
/// password, text, ...), tokens (JWTs and long opaque strings), the host's home directory and
/// user name, and the software keyboard's lines (typed text) are removed.
std::string Scrub(std::string_view text);

/// Tests: point the module at another directory and home/user without touching the real ones.
void SetHostIdentityForTest(std::string home, std::string user);

} // namespace Common::HarborCrash
