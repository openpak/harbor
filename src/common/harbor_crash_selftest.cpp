// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// harbor_crash_selftest: the crash reports' scrubbing, consent, offer and pruning, and a real
// native crash (SIGSEGV and SIGABRT in child processes) turned into a report at the next start.
// Works in $TMPDIR (or the directory given as the first argument). Exits 0 and prints
// "harbor_crash_selftest: all N checks passed" when they hold.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/harbor_crash.h"

namespace {

namespace fs = std::filesystem;
namespace HC = Common::HarborCrash;

int checks = 0;
int failures = 0;

void Check(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void CheckScrub() {
    HC::SetHostIdentityForTest("/home/tester", "tester");
    const std::string in =
        "[   1.000000] Service.ACC <Info> acc.cpp:10:Open: user_id=0123456789abcdef0123456789abcdef\n"
        "uuid 01234567-89ab-cdef-0123-456789abcdef opened\n"
        "web https://accounts.nintendo.com/connect/1.0.0/authorize?code=abcdef&state=xyz done\n"
        "ns: account_id=0x1234abcd5678ef90, application_id=0100000000001000\n"
        "nickname: Tobi\n"
        "{\"id_token\":\"abc.def.ghi\",\"title\":\"ok\"}\n"
        "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.abcDEF123456\n"
        "mail me at someone.name@example.org.\n"
        "path /home/tester/REPOS/run/user/nand/system/save/8000000000000050 by tester-run and tester\n"
        "[   2.000000] Service.AM <Info> applet_software_keyboard.cpp:366:SubmitNormalText: hunter2\n"
        "fatal 2162-0002 in _ZN4Core6System3RunEv title 0100000000001000\n";
    const std::string out = HC::Scrub(in);
    Check(!Has(out, "0123456789abcdef0123456789abcdef"), "32-digit uid removed");
    Check(!Has(out, "01234567-89ab"), "dashed uuid removed");
    Check(Has(out, "https://accounts.nintendo.com/...") && !Has(out, "code=abcdef"),
          "URL kept to its host");
    Check(!Has(out, "1234abcd5678ef90") && Has(out, "application_id=0100000000001000"),
          "account id removed, application id kept");
    Check(!Has(out, "Tobi"), "nickname removed");
    Check(!Has(out, "abc.def.ghi") && Has(out, "\"title\":\"ok\""), "JSON token removed");
    Check(!Has(out, "eyJ"), "bearer token removed");
    Check(!Has(out, "someone.name") && !Has(out, "example.org"), "e-mail removed");
    Check(Has(out, "path ~/REPOS/run/user/nand/system/save/8000000000000050"),
          "home directory shortened, save id kept");
    Check(!Has(out, "tester") && Has(out, "by user-run and user"), "host user name replaced");
    Check(!Has(out, "hunter2") && Has(out, "SubmitNormalText: <withheld: keyboard>"),
          "keyboard text withheld");
    Check(Has(out, "fatal 2162-0002 in _ZN4Core6System3RunEv title 0100000000001000"),
          "error code, symbol and title id kept");
    Check(HC::Scrub(out) == out, "scrubbing twice changes nothing");
}

/// A child that keeps reports, logs a secret, then dies of `sig`.
void CrashChild(const fs::path& dir, int sig) {
    const pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_DUMPABLE, 0); // no core dump from the test
        HC::Install(dir);
        HC::OnLogLine("[   3.000000] Service.ACC <Info> acc.cpp:1:Get: token=SECRETTOKEN\n");
        HC::OnLogLine("[   3.100000] Core <Info> core.cpp:2:Run: about to crash\n");
        if (sig == SIGSEGV) {
            *static_cast<volatile int*>(nullptr) = 1;
        }
        std::abort();
    }
    int status = 0;
    waitpid(pid, &status, 0);
    Check(WIFSIGNALED(status) && WTERMSIG(status) == sig,
          "the crashed child still died of its signal (" + std::to_string(sig) + ")");
}

} // namespace

int main(int argc, char** argv) {
    const char* tmp = argc > 1 ? argv[1] : std::getenv("TMPDIR");
    const fs::path dir = fs::path{tmp != nullptr ? tmp : "."} /
                         ("harbor-crash-selftest-" + std::to_string(getpid()));
    fs::create_directories(dir);

    CheckScrub();

    // Native crashes, written by the handler and finished at the next start.
    std::ofstream(dir / "consent") << "accepted\n";
    CrashChild(dir, SIGSEGV);
    Check(fs::exists(dir / "native.partial"), "the SIGSEGV handler wrote its report");
    HC::Install(dir); // the "next start"
    Check(!fs::exists(dir / "native.partial"), "the half report was finished");
    auto reports = HC::List();
    Check(reports.size() == 1, "one native report kept");
    if (!reports.empty()) {
        const std::string text = HC::Read(reports.front());
        Check(Has(reports.front().summary, "Native crash: SIGSEGV at address 0x0"), "summary");
        Check(Has(reports.front().when, "UTC"), "dated");
        Check(Has(text, "about to crash") && !Has(text, "SECRETTOKEN"), "scrubbed log tail");
        Check(Has(text, "Backtrace:\n    0x"), "backtrace");
    }
    CrashChild(dir, SIGABRT);
    Check(fs::exists(dir / "native.partial"), "the SIGABRT handler wrote its report");

    // Consent, offer, guest fatal, pruning.
    Check(HC::GetConsent() == HC::Consent::Accepted && HC::Recording(), "recording with consent");
    auto offer = HC::TakeOffer();
    Check(offer.has_value(), "the newest report is offered");
    Check(!HC::TakeOffer().has_value(), "and only once");
    HC::OnLogLine("[   4.000000] Service.Fatal <Error> fatal.cpp:1:Throw: nickname=Someone\n");
    HC::RecordGuestFatal(0x0100000000001000ULL, "2162-0002", 0xa8u, "PC: 0000000000001234\n");
    reports = HC::List();
    Check(reports.size() == 2 && Has(reports.front().summary, "Guest fatal error 2162-0002 in 0100000000001000"),
          "guest fatal recorded as the newest");
    if (!reports.empty()) {
        const std::string text = HC::Read(reports.front());
        Check(Has(text, "Result: 0x000000a8") && Has(text, "nickname=<redacted>") &&
                  !Has(text, "Someone"),
              "guest fatal report scrubbed");
    }
    Check(HC::TakeOffer().has_value(), "a new report is offered again");
    for (int i = 0; i < 6; ++i) {
        HC::RecordGuestFatal(0, "2001-0001", 1, "");
    }
    Check(HC::List().size() == 5, "at most 5 kept");
    HC::SetConsent(false);
    Check(!HC::Recording() && HC::List().empty(), "declining stops recording and deletes reports");
    HC::RecordGuestFatal(0, "2001-0001", 1, "");
    Check(HC::List().empty(), "nothing recorded once declined");

    fs::remove_all(dir);
    if (failures != 0) {
        std::printf("harbor_crash_selftest: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("harbor_crash_selftest: all %d checks passed\n", checks);
    return 0;
}
