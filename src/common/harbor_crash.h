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

#include <atomic>
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

/// For the Harbor menu's consent row (MenuHost::CrashReportConsent / SetCrashReportConsent on
/// every platform): nullopt while the question has not been answered, else whether reports are kept.
inline std::optional<bool> CrashReportConsent() {
    switch (GetConsent()) {
    case Consent::Accepted:
        return true;
    case Consent::Declined:
        return false;
    case Consent::Unasked:
        break;
    }
    return std::nullopt;
}

inline void SetCrashReportConsent(bool accepted) {
    SetConsent(accepted);
}

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

/// Native fault dumps (diagnostics, written whatever the consent: signal, registers, code addresses
/// and module file names only). Install opens <dir>/native-fault.txt after logging what the last
/// process left in it; PrepareFaultDump does the same for a process that has not called Install.
void PrepareFaultDump();

/// Async-signal-safe: appends a dump of a fault (signal, si_code, fault address, registers, the
/// frame-pointer backtrace) for `who`. Returns where it starts in the file, for DiscardFaultDump.
long DumpFault(int sig, void* info, void* ucontext, const char* who);

/// Async-signal-safe: the fault was handled after all; its dump is dropped.
void DiscardFaultDump(long start);

/// Diagnostics (temporary): "harbor: crumb: <what> [thread <name> tid <n>]", written with write()
/// to crash-reports/crumbs.txt (O_SYNC) by the calling thread before it returns, then to the log.
/// The next start copies crumbs.txt into eden_log.txt ("harbor: last run crumb: ...").
void Breadcrumb(std::string_view what);

/// A new emulation run: every "once" crumb may log again.
void NewCrumbRun();
/// The current run's number (for HARBOR_CRUMB_ONCE).
unsigned CrumbRun();

/// Diagnostics: logs, for each fatal signal, which handler would take it now: the one the kernel
/// calls (libc's view, past ART's wrapper) and the one ART's signal chain forwards to.
void LogSignalHandlers(std::string_view when);

/// Tests: point the module at another directory and home/user without touching the real ones.
void SetHostIdentityForTest(std::string home, std::string user);

} // namespace Common::HarborCrash

/// A breadcrumb, logged every time.
#define HARBOR_CRUMB(what) ::Common::HarborCrash::Breadcrumb(what)
/// A breadcrumb, logged the first time this line runs in each emulation run.
#define HARBOR_CRUMB_ONCE(what)                                                                    \
    do {                                                                                           \
        static std::atomic<unsigned> harbor_crumb_run_{0};                                         \
        const unsigned harbor_crumb_now_ = ::Common::HarborCrash::CrumbRun();                      \
        if (harbor_crumb_run_.exchange(harbor_crumb_now_) != harbor_crumb_now_) {                  \
            ::Common::HarborCrash::Breadcrumb(what);                                               \
        }                                                                                          \
    } while (0)
