// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// harbor-openpak-check: the OpenPak host layer's self-check, built with -DHARBOR_OPENPAK_CHECK=ON.
//
//   harbor-openpak-check selftest [--online]
//       The signed ceiling (good and bad signatures, rollback), RedirectFor's precedence, the
//       fail-closed families, the CA pin (a mismatch is never written) and the log redaction.
//       --online also pins the public CA with a wrong digest against openpak.org (read-only).
//       Prints "selftest: all checks passed" and exits 0, or names the failed check and exits 1.
//
//   harbor-openpak-check boot
//       BootConnection::Run on ./user (the portable user directory, which must exist), then lists
//       the files under ./user/openpak. Run it twice; the second run without a network
//       (unshare -rn) must come up "on (offline)" from the stored files.
//
// Logs go to ./user/log/eden_log.txt and the console, like the emulator's own.

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "common/logging.h"
#include "common/redact.h"
#include "core/openpak/boot_connection.h"
#include "core/openpak/network_profile.h"
#include "core/openpak/platform.h"

namespace np = OpenPak::NetworkProfile;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// RFC 8032 test 1's secret key: a fixture, never a real one.
constexpr std::array<unsigned char, 32> kSeed = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
    0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};

EVP_PKEY* FixtureKey() {
    static EVP_PKEY* key =
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, kSeed.data(), kSeed.size());
    return key;
}

np::PinnedKey FixturePinned() {
    std::array<unsigned char, 32> pub{};
    std::size_t len = pub.size();
    EVP_PKEY_get_raw_public_key(FixtureKey(), pub.data(), &len);
    return np::MakePinnedKey(pub);
}

std::string Base64(const std::string& bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(bytes.data()),
                                  static_cast<int>(bytes.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::string Sign(const std::string& payload) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, FixtureKey());
    std::string sig(64, '\0');
    std::size_t len = sig.size();
    EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len,
                   reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
    EVP_MD_CTX_free(ctx);
    return sig;
}

std::string Envelope(const std::string& payload, const std::string& sig, const std::string& keyid) {
    return R"({"payload":")" + Base64(payload) + R"(","signatures":[{"keyid":")" + keyid +
           R"(","sig":")" + Base64(sig) + "\"}]}\n";
}

std::string Payload(int version, const std::string& switch_families) {
    return R"({"type":"openpak-ceiling","version":)" + std::to_string(version) +
           R"(,"issued":"2026-10-07T00:00:00Z","platforms":{"switch":[)" + switch_families +
           "]}}";
}

bool Verifies(const std::string& envelope, const std::vector<np::PinnedKey>& keys) {
    std::string error;
    return np::VerifyCeiling(envelope, keys, error).has_value();
}

void CeilingChecks(const std::vector<np::PinnedKey>& keys) {
    std::puts("ceiling");
    Check(np::PinnedKeys().size() == 1 &&
              np::PinnedKeys()[0].keyid ==
                  "36e8bcdd93c2c1a1d7e5d87bbba05a7a4f97882131cf6878f1c053a25a377fe4",
          "the production key is pinned under its keyid");

    const std::string payload = Payload(3, R"(".nintendo.net",".nintendo.com",".klei.com")");
    const std::string good = Envelope(payload, Sign(payload), keys[0].keyid);
    Check(Verifies(good, keys), "a good signature verifies");

    const std::string other = Payload(3, R"(".nintendo.net",".evil.example")");
    Check(!Verifies(Envelope(other, Sign(payload), keys[0].keyid), keys),
          "another payload under the same signature is rejected");

    std::string flipped = Sign(payload);
    flipped[10] = static_cast<char>(flipped[10] ^ 0x01);
    Check(!Verifies(Envelope(payload, flipped, keys[0].keyid), keys),
          "a flipped signature bit is rejected");

    Check(!Verifies(good, np::PinnedKeys()), "a signature under an unpinned key is rejected");

    const std::string malformed = Payload(4, R"(".com")");
    Check(!Verifies(Envelope(malformed, Sign(malformed), keys[0].keyid), keys),
          "a malformed family rejects the whole ceiling");

    const auto rollback = np::DecideCeiling(std::nullopt, good, 5, keys);
    Check(rollback.result.source == np::CeilingSource::BuiltIn && !rollback.store,
          "a ceiling older than one accepted before is refused");
}

void RedirectChecks() {
    std::puts("redirects");
    np::Result applied;
    applied.ok = true;
    applied.source = np::Source::Fetched;
    applied.profile.server_address = "203.0.113.10";
    applied.profile.suffixes = {".nintendo.net"};
    applied.profile.exact = {"lobby-v2.klei.com"};
    applied.profile.never = {"ctest.cdn.nintendo.net", "nncs1-lp1.n.n.srv.nintendo.net"};
    applied.profile.overrides = {{"nncs2-lp1.n.n.srv.nintendo.net", "203.0.113.20"},
                                 {"nncs1-lp1.n.n.srv.nintendo.net", "203.0.113.30"}};
    np::Apply(applied);

    const auto r = [](const char* host) { return np::RedirectFor(host, "198.51.100.1"); };
    Check(!r("ctest.cdn.nintendo.net"), "never wins over a suffix");
    Check(!r("nncs1-lp1.n.n.srv.nintendo.net"), "never wins over an override");
    Check(r("nncs2-lp1.n.n.srv.nintendo.net") == "203.0.113.20", "an override wins over a suffix");
    Check(r("lobby-v2.klei.com") == "203.0.113.10", "an exact name goes to the profile's address");
    Check(r("BAAS.Nintendo.NET") == "203.0.113.10", "a suffix matches, case-insensitively");
    Check(!r("nintendo.net.evil.example") && !r("evilnintendo.net"),
          "a suffix matches on a label boundary only");

    // Fail closed, against the fixture ceiling (switch: .nintendo.net .nintendo.com .klei.com).
    const auto c = [](const char* host, const char* server = "198.51.100.1") {
        return np::ClassifyGuestName(host, "switch", server).kind;
    };
    Check(c("accounts.nintendo.com") == np::GuestNameKind::Blocked,
          "a ceiling family the profile does not redirect is blocked");
    Check(c("galette.klei.com") == np::GuestNameKind::Blocked,
          "a third-party ceiling family is blocked too");
    Check(c("ctest.cdn.nintendo.net") == np::GuestNameKind::Direct, "a never name stays direct");
    Check(c("example.org") == np::GuestNameKind::Direct, "a name outside the ceiling is direct");
    Check(c("dauth-lp1.ndas.srv.nintendo.net") == np::GuestNameKind::Redirect,
          "a redirected name is redirected");

    np::Result no_address = applied;
    no_address.profile.server_address.clear();
    np::Apply(no_address);
    Check(c("dauth-lp1.ndas.srv.nintendo.net", "") == np::GuestNameKind::Blocked,
          "a redirect with nowhere to go is blocked");

    np::Result built_in;
    built_in.ok = true;
    built_in.source = np::Source::BuiltIn;
    built_in.profile = np::BuiltIn("switch");
    np::Apply(built_in);
    Check(c("nncs2-lp1.n.n.srv.nintendo.net") == np::GuestNameKind::Redirect &&
              np::RedirectFor("nncs2-lp1.n.n.srv.nintendo.net", "198.51.100.1") ==
                  "145.241.228.207",
          "the built-in profile keeps the NAT check's second address");
    Check(c("accounts.nintendo.com") == np::GuestNameKind::Redirect,
          "the built-in profile redirects the Nintendo families to the console server");
}

void CaChecks(bool online) {
    std::puts("ca pin");
    const std::string pem = "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n";
    // sha256 of the bytes above, by its own helper's definition: compare against a recomputation.
    np::Profile profile;
    profile.version = 9;
    profile.server_address = "203.0.113.10";
    profile.ca_url = "https://127.0.0.1:1/ca.pem";
    profile.ca_sha256 = std::string(64, '0');
    Check(!np::CaMatchesPin(pem, profile.ca_sha256), "a CA that does not hash to the pin fails");
    Check(!np::CaMatchesPin({}, profile.ca_sha256), "no CA fails the pin");

    OpenPak::Platform::WriteAtomically(OpenPak::Platform::CaPath(), pem);
    Check(!np::EnsureCA(profile), "a CA that cannot be fetched under its pin is not trusted");
    Check(OpenPak::Platform::ReadFile(OpenPak::Platform::CaPath()) == pem,
          "a failed pin leaves ca.pem as it was");

    np::Profile http = profile;
    http.ca_url = "http://openpak.org/ca.pem";
    Check(np::Validate(http, "switch").has_value(), "a CA over plain http rejects the profile");
    np::Profile short_digest = profile;
    short_digest.ca_sha256 = "abc";
    Check(np::Validate(short_digest, "switch").has_value(),
          "a CA without a SHA-256 digest rejects the profile");

    if (online) {
        np::Profile wrong = profile;
        wrong.ca_url = "https://openpak.org/api/v1/network/ca.pem";
        Check(!np::EnsureCA(wrong), "the public CA under a wrong digest is refused (online)");
        Check(OpenPak::Platform::ReadFile(OpenPak::Platform::CaPath()) == pem,
              "and ca.pem is not overwritten (online)");
    }
}

void RedactChecks() {
    std::puts("redaction");
    Check(fmt::format("{}", Common::Redact(0x1234ULL)) == "<redacted>", "Redact hides a value");
    Check(fmt::format("{}", Common::RedactLength("abc")) == "<redacted, 3 bytes>",
          "RedactLength keeps the size only");
    Check(Common::RedactUrl("https://accounts.nintendo.com/connect/1.0.0/authorize?code=x") ==
              "https://accounts.nintendo.com/...",
          "RedactUrl keeps the scheme and host");
    Check(Common::RedactUrl("nintendo://callback#code=x") == "nintendo://callback/...",
          "RedactUrl drops a fragment");
}

int SelfTest(bool online) {
    const auto dir = std::filesystem::current_path() / "user" / "openpak-selftest";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    OpenPak::Platform::SetDirectories(dir, dir / "cache");

    const std::vector<np::PinnedKey> keys = {FixturePinned()};
    CeilingChecks(keys);

    // The process ceiling becomes the fixture's, as a verified boot would make it.
    const std::string payload = Payload(3, R"(".nintendo.net",".nintendo.com",".klei.com")");
    const auto loaded = np::LoadCeiling(Envelope(payload, Sign(payload), keys[0].keyid), keys);
    Check(loaded.source == np::CeilingSource::Fetched && loaded.ceiling.version == 3,
          "a verified ceiling is applied and cached");
    Check(OpenPak::Platform::ReadFile(OpenPak::Platform::CeilingVersionPath()) == "3",
          "the highest accepted version is kept in network-ceiling-version");

    RedirectChecks();
    CaChecks(online);
    RedactChecks();

    std::filesystem::remove_all(dir, ec);
    if (g_failures != 0) {
        std::printf("selftest: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("selftest: all checks passed");
    return 0;
}

int Boot() {
    const auto user = std::filesystem::current_path() / "user";
    const OpenPak::BootResult result = OpenPak::BootConnection::Run(user);
    std::printf("boot: %s (%s), website %s, console server %s, profile %s v%lld, ceiling %s v%lld%s%s\n",
                result.enabled ? "on" : "off", result.online ? "online" : "offline",
                result.website.c_str(), result.console_server.c_str(),
                result.profile_source.c_str(), result.profile_version,
                result.ceiling_source.c_str(), result.ceiling_version,
                result.error.empty() ? "" : ", error: ", result.error.c_str());
    const auto kind = OpenPak::Network::ResolveGuestName("dauth-lp1.ndas.srv.nintendo.net");
    std::printf("boot: dauth-lp1.ndas.srv.nintendo.net -> %s %s\n",
                kind.kind == np::GuestNameKind::Redirect  ? "redirect"
                : kind.kind == np::GuestNameKind::Blocked ? "blocked"
                                                          : "direct",
                kind.address.c_str());
    std::printf("boot: ca.pem %zu bytes in memory, %zu DER bytes\n",
                OpenPak::Network::CaPem().size(), OpenPak::Network::CaDer().size());
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(user / "openpak", ec)) {
        std::printf("file: %s (%ju bytes)\n", entry.path().filename().string().c_str(),
                    static_cast<std::uintmax_t>(entry.is_regular_file(ec) ? entry.file_size(ec) : 0));
    }
    OpenPak::BootConnection::Stop();
    return result.enabled ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    // The portable user directory only: never the owner's own data directory.
    if (!std::filesystem::is_directory(std::filesystem::current_path() / "user")) {
        std::fputs("run from a directory that has a user/ directory (the portable layout)\n",
                   stderr);
        return 64;
    }
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();

    const std::string mode = argc > 1 ? argv[1] : "";
    int status = 64;
    if (mode == "selftest") {
        status = SelfTest(argc > 2 && std::string{argv[2]} == "--online");
    } else if (mode == "boot") {
        status = Boot();
    } else {
        std::fputs("usage: harbor-openpak-check selftest [--online] | boot\n", stderr);
    }
    Common::Log::Stop();
    return status;
}
