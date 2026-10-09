// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>

#include <fmt/format.h>

#ifndef _WIN32
#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <unwind.h>
#ifdef __APPLE__
// <ucontext.h> wants _XOPEN_SOURCE there; the type alone is enough.
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <ucontext.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif
#endif

#include "common/demangle.h"
#include "common/fs/path_util.h"
#include "common/harbor_crash.h"
#include "common/logging.h"
#include "common/redact.h"
#include "common/scm_rev.h"
#ifndef _WIN32
#include <pthread.h>
#include "common/signal_chain.h"
#endif

namespace Common::HarborCrash {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t TailLines = 500;
constexpr std::size_t LineBytes = 1024;
constexpr std::size_t MaxKept = 5;
constexpr std::size_t MaxMessage = 2000;
constexpr std::size_t MaxReportBytes = 20 * 1024 * 1024;
constexpr const char* PartialName = "native.partial";
constexpr const char* ConsentName = "consent";
constexpr const char* OfferedName = "offered";
constexpr const char* ReportPrefix = "harbor-crash-";

// ---- state ----

std::mutex g_state_mutex; // directory, consent, report files
fs::path g_dir;
std::optional<Consent> g_consent;
bool g_installed = false;

std::atomic<bool> g_recording{false};

// The log tail: fixed slots, so a signal handler can walk it without allocating or locking.
struct Slot {
    std::atomic<u32> size{0};
    char data[LineBytes];
};
std::unique_ptr<Slot[]> g_slots;
std::atomic<u64> g_lines_written{0};
std::mutex g_tail_mutex;

std::string g_host_home;
std::string g_host_user;
bool g_host_identity_set = false;

// Lines written before the logger exists (desktop: Install runs before it) wait for it.
std::mutex g_notes_mutex;
std::vector<std::string> g_notes;
bool g_logger_ready = false;

void Note(std::string line) {
    {
        std::scoped_lock lk{g_notes_mutex};
        if (!g_logger_ready) {
            g_notes.push_back(std::move(line));
            return;
        }
    }
    LOG_INFO(Common, "{}", line);
}

void LoadHostIdentity() {
    if (g_host_identity_set) {
        return;
    }
    g_host_identity_set = true;
    if (const char* home = std::getenv("HOME"); home != nullptr && std::strlen(home) > 1) {
        g_host_home = home;
        while (g_host_home.size() > 1 && g_host_home.back() == '/') {
            g_host_home.pop_back();
        }
    }
    for (const char* var : {"LOGNAME", "USER", "USERNAME"}) {
        if (const char* user = std::getenv(var); user != nullptr && std::strlen(user) >= 3) {
            g_host_user = user;
            break;
        }
    }
}

// ---- scrubbing ----

bool IsHex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool IsIdent(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// The software keyboard's lines carry typed, initial and guide text: only their prefix is kept.
std::string WithholdKeyboardLines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find('\n', begin);
        const bool last = end == std::string_view::npos;
        if (last) {
            end = text.size();
        }
        std::string_view line = text.substr(begin, end - begin);
        if (const auto at = line.find("software_keyboard.cpp:"); at != std::string_view::npos) {
            const auto colon = line.find(": ", at);
            out.append(line.substr(0, colon == std::string_view::npos ? at : colon + 2));
            out += "<withheld: keyboard>";
        } else {
            out.append(line);
        }
        if (last) {
            break;
        }
        out += '\n';
        begin = end + 1;
    }
    return out;
}

std::string ReplaceHomeAndUser(std::string text) {
    if (!g_host_home.empty()) {
        std::size_t at = 0;
        while ((at = text.find(g_host_home, at)) != std::string::npos) {
            text.replace(at, g_host_home.size(), "~");
            at += 1;
        }
    }
    if (!g_host_user.empty() && g_host_user != "user") {
        std::size_t at = 0;
        while ((at = text.find(g_host_user, at)) != std::string::npos) {
            const std::size_t end = at + g_host_user.size();
            if ((at == 0 || !IsIdent(text[at - 1])) && (end >= text.size() || !IsIdent(text[end]))) {
                text.replace(at, g_host_user.size(), "user");
                at += 4;
            } else {
                at = end;
            }
        }
    }
    return text;
}

std::string RedactUrls(std::string text) {
    std::size_t search = 0;
    while ((search = text.find("://", search)) != std::string::npos) {
        std::size_t begin = search;
        while (begin > 0) {
            const char c = text[begin - 1];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '.' || c == '-') {
                --begin;
            } else {
                break;
            }
        }
        if (begin == search || !std::isalpha(static_cast<unsigned char>(text[begin]))) {
            search += 3;
            continue;
        }
        std::size_t end = search + 3;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
               std::strchr("\"'<>()[]{},;", text[end]) == nullptr) {
            ++end;
        }
        const std::string redacted = Common::RedactUrl(std::string_view{text}.substr(begin, end - begin));
        text.replace(begin, end - begin, redacted);
        search = begin + redacted.size();
    }
    return text;
}

std::string RedactEmails(std::string text) {
    std::size_t search = 0;
    while ((search = text.find('@', search)) != std::string::npos) {
        std::size_t begin = search;
        while (begin > 0) {
            const char c = text[begin - 1];
            if (std::isalnum(static_cast<unsigned char>(c)) || std::strchr("._%+-", c) != nullptr) {
                --begin;
            } else {
                break;
            }
        }
        std::size_t end = search + 1;
        std::size_t last_dot = std::string::npos;
        while (end < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '.' ||
                text[end] == '-')) {
            if (text[end] == '.') {
                last_dot = end;
            }
            ++end;
        }
        // Trailing dots end the sentence, not the domain.
        while (end > search + 1 && text[end - 1] == '.') {
            --end;
        }
        last_dot = text.rfind('.', end - 1);
        if (begin == search || last_dot == std::string::npos || last_dot <= search + 1 ||
            last_dot + 1 >= end) {
            search += 1;
            continue;
        }
        text.replace(begin, end - begin, "<redacted>");
        search = begin + 10;
    }
    return text;
}

bool IsSensitiveKey(std::string key) {
    std::ranges::transform(key, key.begin(),
                           [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    static constexpr std::array<std::string_view, 28> keys{
        "account_id", "accountid",   "user_id",      "userid",      "uid",
        "uuid",       "nsa_id",      "na_id",        "nickname",    "nick_name",
        "username",   "user_name",   "token",        "password",    "passwd",
        "pin",        "pairing_code", "friend_code", "text",        "initial_text",
        "guide_text", "header_text", "sub_text",     "ok_text",     "email",
        "mail",       "cookie",      "authorization"};
    static constexpr std::array<std::string_view, 8> suffixes{
        "_token", "_password", "_uid", "_user_id", "_account_id", "_nickname", "_text", "_secret"};
    if (std::ranges::find(keys, key) != keys.end()) {
        return true;
    }
    return std::ranges::any_of(suffixes, [&](std::string_view s) { return key.ends_with(s); });
}

/// key=value, key: value, "key":"value" with a sensitive key: the value goes.
std::string RedactKeyValues(std::string text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '=' && text[i] != ':') {
            continue;
        }
        std::size_t key_end = i;
        if (key_end > 0 && (text[key_end - 1] == '"' || text[key_end - 1] == '\'')) {
            --key_end;
        }
        std::size_t key_begin = key_end;
        while (key_begin > 0 && IsIdent(text[key_begin - 1])) {
            --key_begin;
        }
        if (key_begin == key_end || !IsSensitiveKey(text.substr(key_begin, key_end - key_begin))) {
            continue;
        }
        std::size_t value = i + 1;
        while (value < text.size() && text[value] == ' ') {
            ++value;
        }
        if (value >= text.size() || text[value] == '\n') {
            continue;
        }
        std::size_t begin = value;
        std::size_t end;
        if (text[value] == '"' || text[value] == '\'') {
            begin = value + 1;
            end = text.find(text[value], begin);
            if (end == std::string::npos) {
                end = text.find('\n', begin);
                if (end == std::string::npos) {
                    end = text.size();
                }
            }
        } else {
            end = begin;
            while (end < text.size() && std::strchr(" ,;)]}\n\t\r", text[end]) == nullptr) {
                ++end;
            }
        }
        const std::string_view current = std::string_view{text}.substr(begin, end - begin);
        if (current.empty() || current.starts_with("<redacted") || current == "<uid>") {
            continue;
        }
        text.replace(begin, end - begin, "<redacted>");
        i = begin + 9;
    }
    return text;
}

/// 8-4-4-4-12 hex digits, as a formatted UUID prints.
std::string RedactDashedUuids(std::string text) {
    static constexpr std::array<std::size_t, 5> groups{8, 4, 4, 4, 12};
    for (std::size_t i = 0; i + 36 <= text.size(); ++i) {
        if (i > 0 && IsHex(text[i - 1])) {
            continue;
        }
        std::size_t at = i;
        bool match = true;
        for (std::size_t g = 0; g < groups.size() && match; ++g) {
            for (std::size_t k = 0; k < groups[g]; ++k, ++at) {
                if (at >= text.size() || !IsHex(text[at])) {
                    match = false;
                    break;
                }
            }
            if (match && g + 1 < groups.size()) {
                if (at >= text.size() || text[at] != '-') {
                    match = false;
                }
                ++at;
            }
        }
        if (!match || (at < text.size() && IsHex(text[at]))) {
            continue;
        }
        text.replace(i, 36, "<uid>");
    }
    return text;
}

/// Long runs that look like credentials (a JWT, a base64 token): at least 32 characters mixing
/// upper case, lower case and digits. Paths are not runs ('/' ends one); mangled C++ names
/// ("_Z...") are kept.
std::string RedactOpaqueRuns(std::string text) {
    const auto in_run = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || std::strchr("+=_.-", c) != nullptr;
    };
    std::size_t i = 0;
    while (i < text.size()) {
        if (!in_run(text[i])) {
            ++i;
            continue;
        }
        std::size_t end = i;
        int upper = 0, lower = 0, digit = 0;
        while (end < text.size() && in_run(text[end])) {
            const auto c = static_cast<unsigned char>(text[end]);
            upper += std::isupper(c) ? 1 : 0;
            lower += std::islower(c) ? 1 : 0;
            digit += std::isdigit(c) ? 1 : 0;
            ++end;
        }
        const std::string_view run = std::string_view{text}.substr(i, end - i);
        if (run.size() >= 32 && upper >= 2 && lower >= 2 && digit >= 2 && !run.starts_with("_Z")) {
            text.replace(i, run.size(), "<redacted>");
            i += 10;
        } else {
            i = end;
        }
    }
    return text;
}

// ---- report files ----

std::string Utc(std::time_t when, const char* format) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &when);
#else
    gmtime_r(&when, &tm);
#endif
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return buffer;
}

std::string HostSystem() {
#ifndef _WIN32
    utsname name{};
    if (uname(&name) == 0) {
        // Not the node name: the host's name is the owner's.
        return fmt::format("{} {} {}", name.sysname, name.release, name.machine);
    }
#endif
    return "unknown";
}

std::string Version() {
    return fmt::format("{} ({})", Common::g_build_fullname, Common::g_scm_desc);
}

bool WriteFile(const fs::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

std::string ReadFile(const fs::path& path, std::size_t limit = MaxReportBytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::string text(limit, '\0');
    file.read(text.data(), static_cast<std::streamsize>(limit));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}

fs::path NewReportPath(std::time_t when, std::string_view kind) {
    const std::string stem =
        fmt::format("{}{}-{}", ReportPrefix, Utc(when, "%Y%m%d-%H%M%S"), kind);
    fs::path path = g_dir / (stem + ".txt");
    std::error_code ec;
    for (int n = 2; fs::exists(path, ec) && n < 100; ++n) {
        path = g_dir / fmt::format("{}-{}.txt", stem, n);
    }
    return path;
}

std::string HeaderLine(std::string_view text, std::string_view key) {
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const auto line = text.substr(at, end - at);
        if (line.starts_with("----")) {
            break;
        }
        if (line.starts_with(key)) {
            return std::string{line.substr(key.size())};
        }
        at = end + 1;
    }
    return {};
}

std::vector<Report> ListLocked() {
    std::vector<std::pair<fs::file_time_type, Report>> found;
    std::error_code ec;
    if (g_dir.empty() || !fs::is_directory(g_dir, ec)) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(g_dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (!entry.is_regular_file(ec) || !name.starts_with(ReportPrefix) || !name.ends_with(".txt")) {
            continue;
        }
        const std::string head = ReadFile(entry.path(), 4096);
        found.emplace_back(entry.last_write_time(ec), Report{
                                                          .path = entry.path(),
                                                          .name = name,
                                                          .summary = HeaderLine(head, "Summary: "),
                                                          .when = HeaderLine(head, "When: "),
                                                      });
    }
    // Newest saved first (names carry the time only to the second).
    std::ranges::sort(found, [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second.name > b.second.name;
    });
    std::vector<Report> reports;
    reports.reserve(found.size());
    for (auto& entry : found) {
        reports.push_back(std::move(entry.second));
    }
    return reports;
}

void PruneLocked() {
    auto reports = ListLocked();
    std::error_code ec;
    for (std::size_t i = MaxKept; i < reports.size(); ++i) {
        fs::remove(reports[i].path, ec);
    }
}

std::string BuildReport(std::string_view summary, std::string_view kind, std::time_t when,
                        u64 title_id, std::string_view details,
                        const std::vector<std::string>& lines) {
    std::string message{summary.substr(0, MaxMessage)};
    std::ranges::replace(message, '\n', ' ');
    std::string text;
    text += "Harbor crash report\n";
    text += fmt::format("Summary: {}\n", message);
    text += fmt::format("Kind: {}\n", kind);
    text += fmt::format("When: {}\n", Utc(when, "%Y-%m-%d %H:%M:%S UTC"));
    text += fmt::format("Version: {}\n", Version());
    text += fmt::format("System: {}\n", HostSystem());
    if (title_id != 0) {
        text += fmt::format("Title: {:016x}\n", title_id);
    }
    text += "\n---- details ----\n";
    text.append(details);
    if (!text.ends_with('\n')) {
        text += '\n';
    }
    text += fmt::format("\n---- last {} log lines ----\n", lines.size());
    // Under the size limit by dropping the oldest lines, never the crash itself.
    std::size_t budget = MaxReportBytes > text.size() + 1024 ? MaxReportBytes - text.size() - 1024 : 0;
    std::size_t first = lines.size();
    while (first > 0 && lines[first - 1].size() + 1 <= budget) {
        budget -= lines[first - 1].size() + 1;
        --first;
    }
    for (std::size_t i = first; i < lines.size(); ++i) {
        text += lines[i];
        text += '\n';
    }
    return text;
}

std::vector<std::string> SnapshotTail() {
    std::vector<std::string> lines;
    std::scoped_lock lk{g_tail_mutex};
    if (!g_slots) {
        return lines;
    }
    const u64 written = g_lines_written.load();
    const u64 count = std::min<u64>(written, TailLines);
    lines.reserve(count);
    for (u64 n = written - count; n < written; ++n) {
        const Slot& slot = g_slots[n % TailLines];
        lines.emplace_back(slot.data, slot.size.load());
    }
    return lines;
}

// ---- native crashes ----

#ifndef _WIN32

#ifdef __ANDROID__
// Android: abort only. NCE runs guest code natively and takes SIGSEGV and SIGBUS (and the ART
// signal chain SIGSEGV) during normal execution; Harbor stays out of those chains entirely.
// Native faults are left to the platform's crash reporter; guest fatal errors still report.
constexpr std::array<int, 1> FatalSignals{SIGABRT};
#else
// Desktop: installed at start-up, before dynarmic's fastmem handler, which chains to this one only
// for faults it does not own.
constexpr std::array<int, 5> FatalSignals{SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
#endif
struct sigaction g_previous[FatalSignals.size()];
std::atomic<bool> g_in_handler{false};
char g_partial_path[4096];
char g_header[1024];

const char* SignalName(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "signal";
    }
}

/// Writes with no allocation, locale or lock: safe in a signal handler.
struct RawOut {
    int fd;
    void Str(const char* s) {
        Mem(s, std::strlen(s));
    }
    void Mem(const char* s, std::size_t n) {
        while (n > 0) {
            const ssize_t w = ::write(fd, s, n);
            if (w <= 0) {
                return;
            }
            s += w;
            n -= static_cast<std::size_t>(w);
        }
    }
    void Hex(u64 v) {
        char buffer[19] = "0x";
        char digits[16];
        int n = 0;
        do {
            digits[n++] = "0123456789abcdef"[v & 0xf];
            v >>= 4;
        } while (v != 0);
        int at = 2;
        while (n > 0) {
            buffer[at++] = digits[--n];
        }
        Mem(buffer, static_cast<std::size_t>(at));
    }
    void Dec(u64 v) {
        char digits[24];
        int n = 0;
        do {
            digits[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0);
        char buffer[24];
        int at = 0;
        while (n > 0) {
            buffer[at++] = digits[--n];
        }
        Mem(buffer, static_cast<std::size_t>(at));
    }
};

struct UnwindState {
    uintptr_t frames[64];
    int count;
};

_Unwind_Reason_Code UnwindFrame(_Unwind_Context* context, void* arg) {
    auto* state = static_cast<UnwindState*>(arg);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc != 0) {
        state->frames[state->count++] = pc;
    }
    return state->count < 64 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void WriteFrame(RawOut& out, uintptr_t pc) {
    out.Str("    ");
    out.Hex(pc);
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(pc), &info) != 0 && info.dli_fname != nullptr) {
        // The module's file name only: its directory can hold the host user's name.
        const char* base = std::strrchr(info.dli_fname, '/');
        out.Str(" ");
        out.Str(base != nullptr ? base + 1 : info.dli_fname);
        out.Str("+");
        out.Hex(pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
        if (info.dli_sname != nullptr) {
            out.Str(" ");
            out.Str(info.dli_sname);
            out.Str("+");
            out.Hex(pc - reinterpret_cast<uintptr_t>(info.dli_saddr));
        }
    }
    out.Str("\n");
}

uintptr_t ContextPc(void* raw_context) {
    if (raw_context == nullptr) {
        return 0;
    }
    [[maybe_unused]] auto* uc = static_cast<ucontext_t*>(raw_context);
#if defined(__linux__) && defined(__x86_64__)
    return static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
    return static_cast<uintptr_t>(uc->uc_mcontext.pc);
#else
    return 0;
#endif
}

void WritePartial(int sig, siginfo_t* info, void* raw_context) {
    const int fd = ::open(g_partial_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return;
    }
    RawOut out{fd};
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);

    out.Str("Harbor crash report\nSummary: Native crash: ");
    out.Str(SignalName(sig));
    if (sig == SIGSEGV || sig == SIGBUS) {
        out.Str(" at address ");
        out.Hex(reinterpret_cast<uintptr_t>(info != nullptr ? info->si_addr : nullptr));
    }
    out.Str("\nKind: native\nEpoch: ");
    out.Dec(static_cast<u64>(now.tv_sec));
    out.Str("\n");
    out.Str(g_header);
    out.Str("\n---- details ----\nSignal: ");
    out.Str(SignalName(sig));
    out.Str(" (");
    out.Dec(static_cast<u64>(sig));
    out.Str("), code ");
    out.Dec(static_cast<u64>(info != nullptr ? info->si_code : 0));
    out.Str("\nProgram counter: ");
    out.Hex(ContextPc(raw_context));
    out.Str("\nBacktrace:\n");
    if (const uintptr_t pc = ContextPc(raw_context); pc != 0) {
        WriteFrame(out, pc);
    }
    UnwindState state{};
    _Unwind_Backtrace(UnwindFrame, &state);
    for (int i = 0; i < state.count; ++i) {
        WriteFrame(out, state.frames[i]);
    }

    // The tail, already scrubbed as it was kept. A line being written right now may be torn.
    if (g_slots) {
        const u64 written = g_lines_written.load(std::memory_order_acquire);
        const u64 count = written < TailLines ? written : TailLines;
        out.Str("\n---- last ");
        out.Dec(count);
        out.Str(" log lines ----\n");
        for (u64 n = written - count; n < written; ++n) {
            const Slot& slot = g_slots[n % TailLines];
            const u32 size = slot.size.load(std::memory_order_acquire);
            out.Mem(slot.data, size < LineBytes ? size : LineBytes - 1);
            out.Str("\n");
        }
    }
    ::fsync(fd);
    ::close(fd);
}

// ---- native fault dumps ----
// Diagnostics for a process a signal is about to end, written whatever the crash-report consent:
// only the signal, registers and code addresses with their module file names, nothing the user
// typed or owns. The file is opened at start-up so a handler only writes; the next start logs it.

constexpr const char* FaultDumpName = "native-fault.txt";
std::atomic<int> g_fault_fd{-1};

void WriteRegister(RawOut& out, const char* name, u64 value) {
    out.Str(name);
    out.Str("=");
    out.Hex(value);
    out.Str(" ");
}

/// Reads `size` bytes of this process's memory without faulting (false when unreadable).
bool SafeRead(uintptr_t address, void* to, std::size_t size) {
#ifdef __APPLE__
    mach_vm_size_t read = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
                                  reinterpret_cast<mach_vm_address_t>(to), &read) == KERN_SUCCESS &&
           read == size;
#else
    iovec local{to, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(size);
#endif
}

/// The calling thread's id as the system shows it (async-signal-safe).
u64 ThreadId() {
#ifdef __APPLE__
    std::uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid;
#else
    return static_cast<u64>(syscall(SYS_gettid));
#endif
}

long DumpFaultImpl(int sig, siginfo_t* info, void* raw_context, const char* who) {
    const int fd = g_fault_fd.load();
    if (fd < 0) {
        return -1;
    }
    const long start = static_cast<long>(::lseek(fd, 0, SEEK_END));
    RawOut out{fd};
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    out.Str("==== native fault (");
    out.Str(who);
    out.Str(") epoch ");
    out.Dec(static_cast<u64>(now.tv_sec));
    out.Str(" tid ");
    out.Dec(ThreadId());
    out.Str("\nsignal ");
    out.Dec(static_cast<u64>(sig));
    out.Str(" (");
    out.Str(SignalName(sig));
    out.Str(") code ");
    out.Dec(static_cast<u64>(static_cast<u32>(info != nullptr ? info->si_code : 0)));
    out.Str(" fault address ");
    out.Hex(reinterpret_cast<uintptr_t>(info != nullptr ? info->si_addr : nullptr));
    out.Str("\n");

    uintptr_t pc = 0, lr = 0, fp = 0, sp = 0;
    [[maybe_unused]] auto* uc = static_cast<ucontext_t*>(raw_context);
#if defined(__linux__) && defined(__aarch64__)
    if (uc != nullptr) {
        const auto& m = uc->uc_mcontext;
        pc = m.pc;
        lr = m.regs[30];
        fp = m.regs[29];
        sp = m.sp;
        WriteRegister(out, "pc", m.pc);
        WriteRegister(out, "lr", m.regs[30]);
        WriteRegister(out, "sp", m.sp);
        WriteRegister(out, "fp", m.regs[29]);
        WriteRegister(out, "pstate", m.pstate);
        out.Str("\n");
        static constexpr const char* names[29] = {
            "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",
            "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19",
            "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28"};
        for (int i = 0; i < 29; ++i) {
            WriteRegister(out, names[i], m.regs[i]);
            if (i % 4 == 3) {
                out.Str("\n");
            }
        }
        out.Str("\n");
    }
#elif defined(__linux__) && defined(__x86_64__)
    if (uc != nullptr) {
        const auto* g = uc->uc_mcontext.gregs;
        pc = static_cast<uintptr_t>(g[REG_RIP]);
        fp = static_cast<uintptr_t>(g[REG_RBP]);
        sp = static_cast<uintptr_t>(g[REG_RSP]);
        static constexpr std::pair<const char*, int> regs[] = {
            {"rip", REG_RIP}, {"rsp", REG_RSP}, {"rbp", REG_RBP}, {"rax", REG_RAX},
            {"rbx", REG_RBX}, {"rcx", REG_RCX}, {"rdx", REG_RDX}, {"rsi", REG_RSI},
            {"rdi", REG_RDI}, {"r8", REG_R8},   {"r9", REG_R9},   {"r10", REG_R10},
            {"r11", REG_R11}, {"r12", REG_R12}, {"r13", REG_R13}, {"r14", REG_R14},
            {"r15", REG_R15}};
        int n = 0;
        for (const auto& [name, index] : regs) {
            WriteRegister(out, name, static_cast<u64>(g[index]));
            if (++n % 4 == 0) {
                out.Str("\n");
            }
        }
        out.Str("\n");
    }
#endif

    // The frame-pointer chain, every read checked: [fp] is the caller's fp, [fp + 8] its return.
    out.Str("backtrace (frame pointers):\n");
    if (pc != 0) {
        WriteFrame(out, pc);
    }
    if (lr != 0) {
        WriteFrame(out, lr);
    }
    for (int depth = 0; depth < 64 && fp != 0 && (fp & 7) == 0; ++depth) {
        if (sp != 0 && (fp < sp || fp - sp > 8 * 1024 * 1024)) {
            break;
        }
        uintptr_t frame[2]{};
        if (!SafeRead(fp, frame, sizeof(frame)) || frame[1] == 0) {
            break;
        }
        WriteFrame(out, frame[1]);
        if (frame[0] <= fp) {
            break;
        }
        fp = frame[0];
    }
    out.Str("\n");
    ::fsync(fd);
    return start;
}

void FatalSignalHandler(int sig, siginfo_t* info, void* raw_context) {
    std::size_t index = 0;
    while (index < FatalSignals.size() && FatalSignals[index] != sig) {
        ++index;
    }
    DumpFaultImpl(sig, info, raw_context, "harbor fatal-signal handler");
    if (!g_in_handler.exchange(true) && g_recording.load()) {
        WritePartial(sig, info, raw_context);
    }
    // Then whatever handled this signal before Harbor (the default: a core dump; on Android the
    // platform's crash reporter) gets it.
    if (index < FatalSignals.size()) {
        sigaction(sig, &g_previous[index], nullptr);
    } else {
        signal(sig, SIG_DFL);
    }
    // A fault re-runs its instruction on return; a sent signal (abort, kill) is sent again.
    if (info == nullptr || info->si_code <= 0 || sig == SIGABRT) {
        raise(sig);
    }
}

void InstallSignalHandlers() {
    const std::string partial = (g_dir / PartialName).string();
    std::snprintf(g_partial_path, sizeof(g_partial_path), "%s", partial.c_str());
    std::snprintf(g_header, sizeof(g_header), "Version: %s\nSystem: %s\n", Version().c_str(),
                  HostSystem().c_str());
    for (std::size_t i = 0; i < FatalSignals.size(); ++i) {
        struct sigaction action {};
        action.sa_sigaction = &FatalSignalHandler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(FatalSignals[i], &action, &g_previous[i]);
    }
}

#endif // !_WIN32

/// Demangled C++ names in a native report's backtrace.
std::string DemangleBacktrace(std::string text) {
    std::size_t at = 0;
    while ((at = text.find(" _Z", at)) != std::string::npos) {
        const std::size_t begin = at + 1;
        std::size_t end = begin;
        while (end < text.size() && text[end] != '+' && text[end] != '\n' && text[end] != ' ') {
            ++end;
        }
        const std::string demangled = Common::DemangleSymbol(text.substr(begin, end - begin));
        if (!demangled.empty()) {
            text.replace(begin, end - begin, demangled);
            at = begin + demangled.size();
        } else {
            at = end;
        }
    }
    return text;
}

/// A crash's report written by the signal handler: dated, demangled, scrubbed and kept.
void FinishPartialLocked() {
    const fs::path partial = g_dir / PartialName;
    std::error_code ec;
    if (!fs::exists(partial, ec)) {
        return;
    }
    if (g_consent != Consent::Accepted) {
        fs::remove(partial, ec);
        return;
    }
    std::string text = ReadFile(partial);
    const std::string epoch = HeaderLine(text, "Epoch: ");
    std::time_t when = std::time(nullptr);
    if (!epoch.empty()) {
        when = static_cast<std::time_t>(std::strtoll(epoch.c_str(), nullptr, 10));
        const std::string line = "Epoch: " + epoch + "\n";
        if (const auto at = text.find(line); at != std::string::npos) {
            text.replace(at, line.size(), "When: " + Utc(when, "%Y-%m-%d %H:%M:%S UTC") + "\n");
        }
    }
    // Half-written (the process died writing it): nothing worth keeping.
    if (text.find("---- details ----") == std::string::npos) {
        fs::remove(partial, ec);
        return;
    }
    if (WriteFile(NewReportPath(when, "native"), Scrub(DemangleBacktrace(std::move(text))))) {
        fs::remove(partial, ec);
    }
    PruneLocked();
}

Consent ReadConsentLocked() {
    const std::string value = ReadFile(g_dir / ConsentName, 64);
    if (value.starts_with("accepted")) {
        return Consent::Accepted;
    }
    if (value.starts_with("declined")) {
        return Consent::Declined;
    }
    return Consent::Unasked;
}

void StartRecordingLocked() {
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    {
        std::scoped_lock lk{g_tail_mutex};
        if (!g_slots) {
            g_slots = std::make_unique<Slot[]>(TailLines);
        }
    }
    g_recording.store(true);
}

std::atomic<int>& CrumbFd() {
    static std::atomic<int> fd{-1};
    return fd;
}

/// The last run's breadcrumbs (written synchronously) go to the log; the file then starts again
/// for this run.
void OpenCrumbsLocked() {
#ifndef _WIN32
    if (CrumbFd().load() >= 0) {
        return;
    }
    const fs::path path = g_dir / "crumbs.txt";
    const std::string previous = ReadFile(path, 512 * 1024);
    if (!previous.empty()) {
        // The tail matters: the last lines before the process ended.
        std::size_t begin = previous.size() > 64 * 1024 ? previous.find('\n', previous.size() - 64 * 1024) + 1 : 0;
        while (begin < previous.size()) {
            std::size_t end = previous.find('\n', begin);
            if (end == std::string::npos) {
                end = previous.size();
            }
            if (end > begin) {
                Note(fmt::format("harbor: last run crumb: {}",
                                 std::string_view{previous}.substr(begin, end - begin)));
            }
            begin = end + 1;
        }
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_SYNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        CrumbFd() = fd;
    }
#endif
}

/// The last process's fault dumps go to the log; then the file is opened, empty, for this one.
void PrepareFaultDumpLocked() {
#ifndef _WIN32
    if (g_fault_fd.load() >= 0) {
        return;
    }
    if (g_dir.empty()) {
        g_dir = DefaultDirectory();
    }
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    const fs::path path = g_dir / FaultDumpName;
    const std::string previous = ReadFile(path, 256 * 1024);
    if (!previous.empty()) {
        std::size_t at = 0;
        while (at < previous.size()) {
            std::size_t end = previous.find('\n', at);
            if (end == std::string::npos) {
                end = previous.size();
            }
            if (end > at) {
                Note(fmt::format("harbor: native fault: {}",
                                 Scrub(std::string_view{previous}.substr(at, end - at))));
            }
            at = end + 1;
        }
    }
    OpenCrumbsLocked();
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOG_WARNING(Common, "harbor: the native fault dump file could not be opened");
        return;
    }
    g_fault_fd = fd;
#endif
}

} // namespace

fs::path DefaultDirectory() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "crash-reports";
}

void Install(fs::path dir) {
    std::scoped_lock lk{g_state_mutex};
    if (g_installed) {
        return;
    }
    g_installed = true;
    LoadHostIdentity();
    g_dir = dir.empty() ? DefaultDirectory() : std::move(dir);
    g_consent = ReadConsentLocked();
    FinishPartialLocked();
    PrepareFaultDumpLocked();
    if (g_consent == Consent::Declined) {
        LogSignalHandlers("at start-up");
        return;
    }
    // Installed while the answer is still due too, but it writes nothing until consent is given:
    // installing it later would put it in front of the JIT's fault handler.
#ifndef _WIN32
    InstallSignalHandlers();
#endif
    if (g_consent == Consent::Accepted) {
        StartRecordingLocked();
    }
    LogSignalHandlers("at start-up");
}

Consent GetConsent() {
    std::scoped_lock lk{g_state_mutex};
    if (!g_consent) {
        if (g_dir.empty()) {
            g_dir = DefaultDirectory();
        }
        g_consent = ReadConsentLocked();
    }
    return *g_consent;
}

void SetConsent(bool accepted) {
    std::scoped_lock lk{g_state_mutex};
    if (g_dir.empty()) {
        g_dir = DefaultDirectory();
    }
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    WriteFile(g_dir / ConsentName, accepted ? "accepted\n" : "declined\n");
    g_consent = accepted ? Consent::Accepted : Consent::Declined;
    if (accepted) {
        StartRecordingLocked();
    } else {
        g_recording.store(false);
        for (const auto& report : ListLocked()) {
            fs::remove(report.path, ec);
        }
        fs::remove(g_dir / PartialName, ec);
        fs::remove(g_dir / OfferedName, ec);
    }
}

bool Recording() {
    return g_recording.load(std::memory_order_relaxed);
}

void OnLogLine(std::string_view line) {
    if (!Recording()) {
        return;
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    const std::string scrubbed = Scrub(line);
    std::scoped_lock lk{g_tail_mutex};
    if (!g_slots) {
        return;
    }
    const u64 n = g_lines_written.load(std::memory_order_relaxed);
    Slot& slot = g_slots[n % TailLines];
    const std::size_t size = std::min(scrubbed.size(), LineBytes - 1);
    slot.size.store(0, std::memory_order_release);
    std::memcpy(slot.data, scrubbed.data(), size);
    slot.size.store(static_cast<u32>(size), std::memory_order_release);
    g_lines_written.store(n + 1, std::memory_order_release);
}

void RecordGuestFatal(u64 title_id, std::string_view error_code, u32 result,
                      std::string_view details) {
    if (!Recording()) {
        return;
    }
    std::string saved;
    try {
        const std::time_t now = std::time(nullptr);
        const std::string summary =
            title_id != 0 ? fmt::format("Guest fatal error {} in {:016x}", error_code, title_id)
                          : fmt::format("Guest fatal error {}", error_code);
        const std::string text = BuildReport(
            summary, "guest_fatal", now, title_id,
            fmt::format("Error code: {}\nResult: {:#010x}\n{}", error_code, result, details),
            SnapshotTail());
        std::scoped_lock lk{g_state_mutex};
        std::error_code ec;
        fs::create_directories(g_dir, ec);
        const fs::path path = NewReportPath(now, "fatal");
        if (WriteFile(path, Scrub(text))) {
            saved = path.filename().string();
        }
        PruneLocked();
    } catch (...) {
        // A failure here must not become a second crash.
    }
    if (saved.empty()) {
        LOG_WARNING(Common, "harbor: the crash report for a guest fatal error could not be saved");
    } else {
        LOG_INFO(Common, "harbor: crash report {} saved; it is offered at the next launch", saved);
    }
}

std::vector<Report> List() {
    std::scoped_lock lk{g_state_mutex};
    return ListLocked();
}

std::optional<Report> TakeOffer() {
    std::scoped_lock lk{g_state_mutex};
    if (g_consent != Consent::Accepted) {
        return std::nullopt;
    }
    auto reports = ListLocked();
    if (reports.empty()) {
        return std::nullopt;
    }
    const std::string offered = ReadFile(g_dir / OfferedName, 512);
    if (offered == reports.front().name) {
        return std::nullopt;
    }
    WriteFile(g_dir / OfferedName, reports.front().name);
    return reports.front();
}

std::string Read(const Report& report) {
    return ReadFile(report.path);
}

void Delete(const Report& report) {
    std::scoped_lock lk{g_state_mutex};
    std::error_code ec;
    // Only a report in the reports directory.
    if (report.path.parent_path() == g_dir && report.name.starts_with(ReportPrefix)) {
        fs::remove(report.path, ec);
    }
}

void DeleteAll() {
    std::scoped_lock lk{g_state_mutex};
    std::error_code ec;
    for (const auto& report : ListLocked()) {
        fs::remove(report.path, ec);
    }
}

std::string Scrub(std::string_view text) {
    LoadHostIdentity();
    std::string out = WithholdKeyboardLines(text);
    out = ReplaceHomeAndUser(std::move(out));
    out = RedactUrls(std::move(out));
    out = RedactEmails(std::move(out));
    out = RedactKeyValues(std::move(out));
    out = RedactDashedUuids(std::move(out));
    out = Common::RedactPath(out);
    out = RedactOpaqueRuns(std::move(out));
    return out;
}

void PrepareFaultDump() {
    std::scoped_lock lk{g_state_mutex};
    PrepareFaultDumpLocked();
}

long DumpFault(int sig, void* info, void* ucontext, const char* who) {
#ifndef _WIN32
    return DumpFaultImpl(sig, static_cast<siginfo_t*>(info), ucontext, who);
#else
    return -1;
#endif
}

void DiscardFaultDump(long start) {
#ifndef _WIN32
    const int fd = g_fault_fd.load();
    if (fd >= 0 && start >= 0) {
        [[maybe_unused]] const int result = ::ftruncate(fd, static_cast<off_t>(start));
    }
#endif
}

namespace {
std::atomic<unsigned> g_crumb_run{1};
} // namespace

void LoggerReady() {
    std::vector<std::string> notes;
    {
        std::scoped_lock lk{g_notes_mutex};
        g_logger_ready = true;
        notes.swap(g_notes);
    }
    for (const auto& line : notes) {
        LOG_INFO(Common, "{}", line);
    }
}

void NewCrumbRun() {
    g_crumb_run.fetch_add(1);
}

unsigned CrumbRun() {
    return g_crumb_run.load(std::memory_order_relaxed);
}

void Breadcrumb(std::string_view what) {
    char name[32] = "?";
    long tid = 0;
#ifndef _WIN32
    pthread_getname_np(pthread_self(), name, sizeof(name));
    tid = static_cast<long>(ThreadId());
    if (const int fd = CrumbFd().load(); fd >= 0) {
        timespec now{};
        clock_gettime(CLOCK_REALTIME, &now);
        char line[512];
        const int size = std::snprintf(line, sizeof(line), "%lld.%03ld crumb: %.*s [thread %s tid %ld]\n",
                                       static_cast<long long>(now.tv_sec), now.tv_nsec / 1000000,
                                       static_cast<int>(std::min<std::size_t>(what.size(), 300)),
                                       what.data(), name, tid);
        if (size > 0) {
            // O_SYNC: on the disk before this returns.
            [[maybe_unused]] const ssize_t written =
                ::write(fd, line, std::min<std::size_t>(static_cast<std::size_t>(size), sizeof(line) - 1));
        }
    }
#endif
    LOG_INFO(Common, "harbor: crumb: {} [thread {} tid {}]", what, name, tid);
}

void LogSignalHandlers(std::string_view when) {
#ifndef _WIN32
    const auto describe = [](const struct sigaction& action) -> std::string {
        const bool siginfo = (action.sa_flags & SA_SIGINFO) != 0;
        const void* handler = siginfo ? reinterpret_cast<const void*>(action.sa_sigaction)
                                      : reinterpret_cast<const void*>(action.sa_handler);
        if (!siginfo && action.sa_handler == SIG_DFL) {
            return "SIG_DFL";
        }
        if (!siginfo && action.sa_handler == SIG_IGN) {
            return "SIG_IGN";
        }
        Dl_info info{};
        if (handler != nullptr && dladdr(handler, &info) != 0 && info.dli_fname != nullptr) {
            const char* base = std::strrchr(info.dli_fname, '/');
            return fmt::format("{}+{:#x} {}", base != nullptr ? base + 1 : info.dli_fname,
                               reinterpret_cast<uintptr_t>(handler) -
                                   reinterpret_cast<uintptr_t>(info.dli_fbase),
                               info.dli_sname != nullptr ? info.dli_sname : "?");
        }
        return fmt::format("{} (no module)", handler);
    };
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) {
        struct sigaction kernel {};
        struct sigaction chain {};
        Common::SigAction(sig, nullptr, &kernel);
        ::sigaction(sig, nullptr, &chain);
        Note(fmt::format("harbor: signal handlers {}: {} kernel={} chain={}", when, sig,
                         describe(kernel), describe(chain)));
    }
#endif
}

void SetHostIdentityForTest(std::string home, std::string user) {
    g_host_identity_set = true;
    g_host_home = std::move(home);
    g_host_user = std::move(user);
}

} // namespace Common::HarborCrash
