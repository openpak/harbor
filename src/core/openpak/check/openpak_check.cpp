// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// harbor-openpak-check: the OpenPak host layer's self-check, built with -DHARBOR_OPENPAK_CHECK=ON.
//
//   harbor-openpak-check selftest [--online]
//       The signed ceiling (good and bad signatures, rollback), RedirectFor's precedence, the
//       fail-closed families, the CA pin (a mismatch is never written), the log redaction and
//       Save Data Cloud (the archive list, DataInfo's layout, the olsc daemon settings file).
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
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "common/logging.h"
#include "common/redact.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_archive.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_crypto.h"
#include "core/openpak/bcat_key_patch.h"
#include "core/openpak/boot_connection.h"
#include "core/openpak/network_profile.h"
#include "core/openpak/olsc.h"
#include "core/openpak/olsc_transfer.h"
#include "core/openpak/platform.h"
#include "core/openpak/secret_store.h"
#include "core/openpak/session.h"

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
           R"(,"issued":"2026-10-07T00:00:00Z","platforms":{"switch":[)" + switch_families + "]}}";
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

    // ip_redirects: hard-coded destinations, signed with the families.
    const auto with_ip = [](const std::string& rules) {
        return R"({"type":"openpak-ceiling","version":6,"platforms":{"switch":[".nintendo.net"]},)"
               R"("ip_redirects":{"switch":[)" + rules + "]}}";
    };
    const std::string ip_payload =
        with_ip(R"({"from":"192.0.2.7:23001","to":"203.0.113.10:22690","proto":"tcp"},)"
                R"({"from":"192.0.2.7:23001","to":"203.0.113.10:22691","proto":"udp"})");
    std::string ip_error;
    const auto ip_ceiling =
        np::VerifyCeiling(Envelope(ip_payload, Sign(ip_payload), keys[0].keyid), keys, ip_error);
    Check(ip_ceiling && ip_ceiling->IpRedirectsFor("switch").size() == 2 &&
              ip_ceiling->IpRedirectsFor("switch")[0].tcp &&
              ip_ceiling->IpRedirectsFor("switch")[0].to_port == 22690 &&
              !ip_ceiling->IpRedirectsFor("switch")[1].tcp &&
              ip_ceiling->IpRedirectsFor("wiiu").empty(),
          "signed ip redirects are read per platform and protocol");
    for (const char* bad : {
             R"({"from":"192.0.2.7","to":"203.0.113.10:22690","proto":"tcp"})",
             R"({"from":"192.0.2.07:23001","to":"203.0.113.10:22690","proto":"tcp"})",
             R"({"from":"192.0.2.7:23001","to":"203.0.113.10:0","proto":"tcp"})",
             R"({"from":"192.0.2.7:23001","to":"example.org:22690","proto":"tcp"})",
             R"({"from":"192.0.2.7:23001","to":"203.0.113.10:22690","proto":"sctp"})",
             R"({"from":"192.0.2.7:23001","to":"203.0.113.10:22690","proto":"tcp"},)"
             R"({"from":"192.0.2.7:23001","to":"203.0.113.11:22690","proto":"tcp"})",
         }) {
        const std::string p = with_ip(bad);
        Check(!Verifies(Envelope(p, Sign(p), keys[0].keyid), keys),
              "a malformed or duplicate ip redirect rejects the whole ceiling");
    }
    std::array<unsigned char, 4> ip{};
    unsigned short port = 0;
    Check(np::ParseIpv4Endpoint("255.0.0.1:65535", ip, port) && ip[0] == 255 && port == 65535 &&
              !np::ParseIpv4Endpoint("256.0.0.1:1", ip, port) &&
              !np::ParseIpv4Endpoint("1.2.3:4", ip, port) &&
              !np::ParseIpv4Endpoint("1.2.3.4.5:6", ip, port) &&
              !np::ParseIpv4Endpoint("1.2.3.4:65536", ip, port),
          "ip redirect endpoints are dotted-quad IPv4 with a port");

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

void FetchChecks() {
    std::puts("profile fetch");
    const std::string body =
        R"({"version":7,"platform":"switch","server":{"address":"203.0.113.10"},)"
        R"("redirect":{"suffixes":[".nintendo.net"]}})";
    const auto none = np::DecideFetched("switch", 0, {}, {}, {});
    Check(!none.confirmed && none.source == np::Source::BuiltIn,
          "no answer and nothing stored: built-in, not confirmed");
    const auto fresh = np::DecideFetched("switch", 200, body, "\"e1\"", {});
    Check(fresh.confirmed && fresh.source == np::Source::Fetched && fresh.profile.version == 7,
          "a 200 is confirmed and applied");
    const auto unchanged = np::DecideFetched("switch", 304, {}, {}, "\"e1\"");
    Check(unchanged.confirmed && unchanged.source == np::Source::Cached &&
              unchanged.profile.version == 7,
          "a 304 confirms the stored profile (online, not offline)");
    const auto silent = np::DecideFetched("switch", 0, {}, {}, "\"e1\"");
    Check(!silent.confirmed && silent.source == np::Source::Cached && !silent.error.empty(),
          "no answer falls back to the stored profile, not confirmed");
    const auto failed = np::DecideFetched("switch", 503, "busy", {}, "\"e1\"");
    Check(!failed.confirmed && failed.source == np::Source::Cached,
          "an HTTP error falls back to the stored profile, not confirmed");
    const auto refused = np::DecideFetched("switch", 200, "{", {}, "\"e1\"");
    Check(!refused.confirmed && refused.source == np::Source::Cached,
          "an unparseable 200 is not confirmed and leaves the stored profile");
}

void OlscChecks(const std::filesystem::path& dir) {
    namespace olsc = OpenPak::Olsc;
    std::puts("save data cloud");
    // The saves service's list reply (internal/switchscsi archive JSON): complete records.
    const auto record = [](int id, const char* status, int saved_at, const char* finished,
                           const char* extra) {
        return fmt::format(
            R"({{"id":{},"nsa_id":"00000000000000aa","application_id":"0100A00000BB0000",)"
            R"("device_id":"0000000000000001","device_serial":"XAW0","data_size":4096,)"
            R"("datatype":0,"status":"{}","num_of_partitions":2,"launch_required_version":65536,)"
            R"("previous_save_data_archive_id":null,"series_id":9,"desc":null,)"
            R"("encoded_digest":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",)"
            R"("saved_at_as_unixtime":{},"finished_at_as_unixtime":{}{}}})",
            id, status, saved_at, finished, extra);
    };
    const auto wrap = [](const std::string& records) {
        return R"({"save_data_archives":[)" + records + "]}";
    };
    const std::string body = wrap(
        record(7, "fixed", 1000, "1010", R"(,"acd_index":3,"platform_key_generation":1)") + "," +
        record(8, "fixed", 2000, "null", "") + "," + record(9, "uploading", 3000, "null", ""));
    const auto list = olsc::ParseArchiveList(body);
    Check(list && list->size() == 3, "a complete archive list is read");
    Check(list && (*list)[0].nsa_id == 0xaa && (*list)[0].application_id == 0x0100A00000BB0000 &&
              (*list)[0].launch_required_version == 65536 && (*list)[0].acd_index == 3 &&
              (*list)[1].acd_index == 0 && (*list)[2].status == olsc::StatusUploading,
          "archive fields parse as the sysmodule reads them");
    Check(!olsc::ParseArchiveList("{}") && !olsc::ParseArchiveList("not json"),
          "a reply that is not a list is refused");
    {
        std::string with_commit = record(11, "fixed", 1, "null", "");
        with_commit.replace(with_commit.find("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), 43,
                            "iHdmVUQzIhEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
        const auto parsed = olsc::ParseArchiveList(wrap(with_commit));
        Check(parsed && parsed->size() == 1 && (*parsed)[0].commit_id == 0x1122334455667788,
              "an archive's commit id is read from encoded_digest");
    }
    Check(olsc::ParseArchiveList(wrap("")) && olsc::ParseArchiveList(wrap(""))->empty(),
          "an empty cloud reads as an empty list");
    // A record the sysmodule's record check refuses fails the whole list (2179-3201), so a
    // console keeps the cache it had: an empty cloud stays empty.
    const std::string good = record(8, "fixed", 2000, "null", "");
    std::string no_digest = good;
    no_digest.replace(no_digest.find("AAAAAAAA"), 43, "");
    std::string no_serial = good;
    no_serial.replace(no_serial.find(R"("device_serial":"XAW0",)"), 23, "");
    std::string null_size = good;
    null_size.replace(null_size.find(R"("data_size":4096)"), 16, R"("data_size":null)");
    Check(!olsc::ParseArchiveList(wrap(good + "," + no_digest)),
          "an archive without a 32-byte digest refuses the whole list");
    Check(!olsc::ParseArchiveList(wrap(no_serial + "," + good)),
          "an archive missing a required key refuses the whole list");
    Check(!olsc::ParseArchiveList(wrap(null_size)), "a null data_size is not accepted");
    Check(!olsc::ParseArchiveList(wrap(record(10, "hand_over", 1, "null", ""))),
          "an unknown status refuses the list");
    const auto latest = list ? olsc::LatestFor(*list, 0x0100A00000BB0000) : std::nullopt;
    Check(latest && latest->id == 8, "GetDataInfo answers the newest fixed archive");
    Check(list && !olsc::LatestFor(*list, 0x0100000000000001), "another title has no cloud data");
    if (list) {
        auto more = *list;
        olsc::ArchiveInfo other;
        other.id = 20;
        other.application_id = 0x0100000000000001;
        other.saved_at = 5000;
        more.push_back(other);
        const auto per_title = olsc::LatestPerApplication(more);
        Check(per_title.size() == 2 && per_title[0].id == 20 && per_title[1].id == 8,
              "ListDataInfo lists one archive per title, newest first");
    }

    olsc::ArchiveInfo a;
    a.id = 0x1122334455667788;
    a.application_id = 0x0100A00000BB0000;
    a.status = olsc::StatusUploading;
    a.data_size = 0x4000;
    a.saved_at = 0x0A0B;
    a.finished_at = 0x0C0D;
    a.acd_index = 5;
    const auto info = olsc::PackDataInfo(a);
    std::uint64_t word = 0;
    std::memcpy(&word, info.data() + 0x08, 8);
    Check(info[0] == 0x88 && word == 0x0100A00000BB0000 && info[0x10] == 2 && info[0x11] == 0 &&
              info[0x18] == 0x00 && info[0x19] == 0x40 && info[0x20] == 0x0B &&
              info[0x28] == 0x0D && info[0x30] == 5 && info[0x37] == 0,
          "DataInfo is laid out as GetDataInfo copies it");

    const auto file = dir / "olsc-settings.json";
    const std::string uid = "00112233445566778899aabbccddeeff";
    std::filesystem::remove(file);
    {
        olsc::DaemonSettings settings{file};
        Check(settings.GlobalAutoUpload(uid) && !settings.GlobalAutoDownload(uid) &&
                  settings.ApplicationAutoTransfer(uid, 0x0100000000000001),
              "an unknown user has the settings defaults (backup on, download off)");
        Check(!settings.Backup(uid, 1) && !settings.SetBackupCustomData(uid, 1, "island"),
              "device backup custom data requires an existing setting record");
        settings.SetBackup(uid, 1, true);
        settings.SetBackupCustomData(uid, 1, "island \"quoted\"");
        settings.SetBackup(uid, 1, false);
        Check(settings.Backup(uid, 1)->desc == "island \"quoted\"" &&
                  !settings.Backup(uid, 1)->enabled && !settings.Backup("other", 1) &&
                  !settings.SetBackupCustomData(uid, 1, std::string(0x400, 'x')),
              "device settings preserve custom data, isolate users and enforce the buffer limit");
        for (std::uint64_t app = 1; app <= 17; ++app)
            settings.SetBackup("other", app, true);
        Check(!settings.Backup("other", 1) && settings.EnabledBackups("other").size() == 16,
              "device settings evict the oldest record at sixteen applications");
        settings.SetGlobalAutoUpload(uid, false);
        settings.SetApplicationAutoTransfer(uid, 0x0100A00000BB0000, true);
        settings.SetSeries(uid, 0x0100A00000BB0000, {7, 9});
        settings.SetLastError(uid, 0x0100A00000BB0000, {1, 1700000000, 0x1234});
        settings.SetLastError(uid, 0x0100000000000002, {0, 1, 2});
        settings.RemoveLastError(uid, 0x0100000000000002);
    }
    olsc::DaemonSettings reread{file};
    const auto series = reread.Series(uid, 0x0100A00000BB0000);
    Check(reread.Backup(uid, 1) && !reread.Backup(uid, 1)->enabled &&
              reread.Backup(uid, 1)->desc == "island \"quoted\"",
          "device backup enabled state and custom data survive a restart");
    Check(!reread.GlobalAutoUpload(uid) && !reread.GlobalAutoDownload(uid) &&
              reread.ApplicationAutoTransfer(uid, 0x0100A00000BB0000) &&
              !reread.ApplicationAutoTransfer(uid, 0x0100000000000001) && series &&
              series->series_id == 7 && series->commit_id == 9 &&
              !reread.Series(uid, 0x0100000000000001),
          "the olsc daemon settings and SeriesInfo survive a restart");
    const auto last_error = reread.LastErrorOf(uid, 0x0100A00000BB0000);
    Check(last_error && last_error->task_type == 1 && last_error->time == 1700000000 &&
              last_error->result == 0x1234 && !reread.LastErrorOf(uid, 0x0100000000000002),
          "the last transfer error survives a restart and a removed one stays gone");

    // Newness (olsc-autonomy.md §4.1).
    olsc::ArchiveInfo cloud;
    cloud.series_id = 7;
    cloud.commit_id = 9;
    const olsc::SeriesInfo synced{7, 9};
    Check(olsc::Newness(std::nullopt, 5, std::nullopt) == 2 &&
              olsc::Newness(std::nullopt, std::nullopt, std::nullopt) == 3 &&
              olsc::Newness(synced, std::nullopt, cloud) == 1 &&
              olsc::Newness(synced, 9, cloud) == 0 && olsc::Newness(synced, 10, cloud) == 2 &&
              olsc::Newness(olsc::SeriesInfo{7, 8}, 8, cloud) == 1 &&
              olsc::Newness(olsc::SeriesInfo{7, 8}, 10, cloud) == 3 &&
              olsc::Newness(std::nullopt, 9, cloud) == 3,
          "newness: in sync, cloud newer, local newer and conflict as the sysmodule decides");

    if (list) {
        const auto store = dir / "olsc-archives.json";
        std::filesystem::remove(store);
        auto& cache = olsc::ArchiveCache::Instance();
        cache.SetStore(store);
        cache.Put(uid, *list);
        auto island = (*list)[0];
        island.id = 123456;
        island.application_id = 0x01006f8002326000;
        island.datatype = 3;
        island.desc = "island backup";
        cache.Put(uid, {island}, 3);
        Check(cache.Get(uid)->size() == list->size() && cache.Get(uid, 3)->size() == 1 &&
                  cache.Get(uid, 3)->front().id == island.id &&
                  cache.Get(uid)->front().id == list->front().id,
              "device and account archives have separate caches for the same user");
        cache.Clear();
        Check(!cache.Get(uid), "a cleared cache has no list");
        cache.SetStore(store);
        const auto kept = cache.Get(uid);
        Check(kept && kept->size() == list->size() && (*kept)[0].id == (*list)[0].id &&
                  (*kept)[0].nsa_id == 0xaa && (*kept)[0].acd_index == 3 &&
                  (*kept)[2].status == olsc::StatusUploading,
              "the archive list survives a restart, as the sysmodule's cache does");
        const auto kept_island = cache.Get(uid, 3);
        Check(kept_island && kept_island->size() == 1 && kept_island->front().id == island.id &&
                  kept_island->front().datatype == 3 && kept_island->front().desc == island.desc,
              "device archives retain their type and custom data across a restart");
        cache.SetStore(dir / "olsc-archives-other.json");
        Check(!cache.Get(uid), "another data directory starts without the list");
        cache.Clear();
    }
}

void SaveTransferChecks(const std::filesystem::path& dir) {
    namespace st = Service::FileSystem::SaveTransfer;
    std::puts("save transfer crypto");
    const auto hex = [](std::string_view text) {
        st::Key128 out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<u8>(std::stoi(std::string{text.substr(i * 2, 2)}, nullptr, 16));
        }
        return out;
    };
    // Random inputs, expected value from openssl enc -aes-128-ecb -d chained as spl does.
    Check(st::DeriveKey(
              hex("b752f982ab4f9189815a04e3d3d11039"), hex("702d17df75e47669fd76fcf6a3a8ebfd"),
              hex("098ee32b29a899ec731ebdc51b0ab22e"), hex("f95b61685f1b14b7ccfeb0f34bd7c8a6"),
              hex("a5dd2526e7d4b63caadf35a00ec4a24d")) == hex("1211b7f6923cea2257ba681808b341e3"),
          "key derivation chains GenerateAesKek and GenerateAesKey as spl does");
    Check(st::MasterKeyIndex(5) == 4 && st::MasterKeyIndex(1) == 0 && st::MasterKeyIndex(0) == 0,
          "key generation N uses master key N-1");
    // RFC 4493, example 2.
    const auto rfc_key = hex("2b7e151628aed2a6abf7158809cf4f3c");
    const auto rfc_msg = hex("6bc1bee22e409f96e93d7e117393172a");
    Check(st::Cmac(rfc_key, rfc_msg) == hex("070a16b46b4d4144f79bdd9dd04a287c"),
          "AES-CMAC matches RFC 4493");

    st::Key128 key{};
    st::RandomBytes(key);
    st::Key128 iv{};
    st::RandomBytes(iv);
    std::vector<u8> payload(0x1fd0);
    st::RandomBytes(payload);
    st::Key128 tag{};
    const auto stream = st::EncryptStream(key, iv, st::StreamKeyGeneration, payload, &tag);
    Check(stream.size() == 0x2000 && std::memcmp(stream.data(), "AGS0", 4) == 0 && stream[6] == 5 &&
              stream[7] == 0 && std::memcmp(stream.data() + 0x10, iv.data(), 16) == 0,
          "an initial data stream is 0x2000 bytes: AGS0, key generation 5, IV, tag");
    const auto back = st::DecryptStream(key, stream);
    Check(back && *back == payload, "a stream decrypts to its payload");
    auto tampered = stream;
    tampered[0x40] ^= 1;
    Check(!st::DecryptStream(key, tampered), "a changed stream byte fails the GCM tag");

    std::vector<u8> plain(0x30000);
    for (std::size_t i = 0; i < plain.size(); ++i) {
        plain[i] = static_cast<u8>((i / 0x1000) & 0xff);
    }
    const auto packed = st::RawDeflate(plain);
    const auto unpacked = st::RawInflate(packed, plain.size());
    Check(packed.size() < plain.size() && packed[0] != 0x78 && unpacked && *unpacked == plain,
          "raw deflate (no zlib header) round trips");

    // A throwaway RSA key made here; the console's check is RSA-PSS-SHA256 against a modulus.
    EVP_PKEY* rsa = EVP_RSA_gen(2048);
    const auto pem = dir / "ksp-test.pem";
    std::FILE* file = std::fopen(pem.string().c_str(), "wb");
    PEM_write_PrivateKey(file, rsa, nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(file);
    BIGNUM* n = nullptr;
    EVP_PKEY_get_bn_param(rsa, OSSL_PKEY_PARAM_RSA_N, &n);
    st::Modulus modulus{};
    BN_bn2binpad(n, modulus.data(), static_cast<int>(modulus.size()));
    BN_free(n);
    EVP_PKEY_free(rsa);

    st::KeySeedPackageContent content;
    st::RandomBytes(content.key_seed);
    st::RandomBytes(content.initial_data_mac);
    st::RandomBytes(content.challenge);
    st::Key128 body_key{};
    st::RandomBytes(body_key);
    const auto package = st::BuildKeySeedPackage(content, 5, body_key, pem.string());
    std::filesystem::remove(pem);
    Check(package.has_value(), "a key seed package is built with a PEM key");
    if (package) {
        const auto bytes = [](const st::KeySeedPackage& p) {
            std::array<u8, 0x200> out{};
            std::memcpy(out.data(), &p, sizeof(p));
            return out;
        };
        const auto good = bytes(*package);
        const auto opened = st::OpenKeySeedPackage(good, body_key, modulus, content.challenge);
        Check(opened.error == st::PackageError::None &&
                  opened.content.key_seed == content.key_seed &&
                  opened.content.initial_data_mac == content.initial_data_mac,
              "SetKeySeedPackage opens it and recovers the seed and the MAC");
        st::Key128 other{};
        st::RandomBytes(other);
        Check(st::OpenKeySeedPackage(good, body_key, modulus, other).error ==
                  st::PackageError::Challenge,
              "another session's challenge is 2-7004");
        auto bad_tag = good;
        bad_tag[0x20] ^= 1;
        Check(st::OpenKeySeedPackage(bad_tag, body_key, modulus, content.challenge).error ==
                  st::PackageError::Mac,
              "a changed tag is 2-7002");
        auto other_modulus = modulus;
        other_modulus[0x80] ^= 1;
        Check(st::OpenKeySeedPackage(good, body_key, other_modulus, content.challenge).error ==
                  st::PackageError::Signature,
              "another signing key is 2-7003");
        auto bad_version = good;
        bad_version[1] = 1;
        Check(st::OpenKeySeedPackage(bad_version, body_key, modulus, content.challenge).error ==
                  st::PackageError::Version,
              "version 0x100 is refused first");
    }
}

void SaveArchiveChecks() {
    namespace st = Service::FileSystem::SaveTransfer;
    std::puts("save transfer archive");
    st::ArchiveSource source;
    source.stream.resize(5 * 0x4000 + 0x1234);
    for (std::size_t i = 0; i < source.stream.size(); ++i) {
        source.stream[i] = static_cast<u8>(i % 251 < 200 ? 0 : i);
    }
    st::RandomBytes(source.extra_data);
    st::RandomBytes(source.integrity_seed);
    st::Key128 stream_key{};
    st::Key128 mac_key{};
    st::RandomBytes(stream_key);
    st::RandomBytes(mac_key);
    st::ArchiveWriter writer{source, stream_key, mac_key};
    Check(!writer.SetDivisionCount(65) && writer.SetDivisionCount(3), "division count 1..64");
    const auto ids = writer.ChunkIds();
    Check(ids.size() == 4 && ids[0] == 0x1000 && ids[2] == 0x1002 && ids[3] == 0,
          "data chunks are 0x1000+n, the meta chunk 0 comes last");
    writer.SetAad(st::MakeAad(0x0100A5A020D5E000, 42));
    const auto first = writer.Chunk(0x1001);
    const auto again = writer.Chunk(0x1001);
    Check(first && again && *first == *again, "a chunk reads the same on every open");
    const auto meta = writer.Chunk(0);
    Check(meta && meta->size() == st::MetaChunkSize, "the meta chunk is 8 KiB");
    const auto& content = writer.Content();
    Check(content.division_count == 3 && content.chunk_sizes[0] > 0 &&
              content.extra_data == source.extra_data && content.hash_salt == source.integrity_seed,
          "the initial data records the chunks, the extra data and the seed");
    const auto mac = writer.InitialDataMac();

    st::ArchiveReader reader;
    Check(reader.Open(*meta, stream_key, mac_key, mac) == st::ArchiveReader::Error::None,
          "the importer opens the meta chunk and the package's MAC matches");
    Check(reader.Content().total_stream_size == static_cast<s64>(source.stream.size()),
          "initial data carries uncompressed save-image length for native chunk ranges");
    Check(reader.Content().aad == st::MakeAad(0x0100A5A020D5E000, 42),
          "the AAD travels in the initial data");
    // Fixed public fixture: two allocated blocks (one all zero) and six free blocks.
    st::ArchiveSource native;
    native.extra_data[0x20] = 1;
    for (std::size_t i = 0; i < native.integrity_seed.size(); ++i)
        native.integrity_seed[i] = i;
    native.stream.resize(0x68000);
    auto put32 = [&](std::size_t at, u32 value) {
        std::memcpy(native.stream.data() + at, &value, 4);
    };
    auto put64 = [&](std::size_t at, u64 value) {
        std::memcpy(native.stream.data() + at, &value, 8);
    };
    put32(0x40060, 0x45564153);
    put32(0x40064, 0x60000);
    put64(0x40068, 8);
    put64(0x40070, 0x4000);
    const std::size_t fat = 0x400a8;
    put32(fat + 4, 3);
    put32(fat + 8, 0x80000000);
    put32(fat + 16, 0x80000000);
    put32(fat + 24, 0x80000000);
    put32(fat + 28, 0x80000000);
    put32(fat + 32, 0x80000003);
    put32(fat + 36, 8);
    put32(fat + 64, 0x80000003);
    put32(fat + 68, 8);
    std::fill(native.stream.begin() + 0x48000, native.stream.begin() + 0x4c000, 0x21);
    const auto native_hash = st::NativeChunkHashes(native.stream, native.extra_data,
                                                   native.integrity_seed, 0, native.stream.size());
    Check(native_hash &&
              native_hash->full == std::array<u8, 32>{0x70, 0x2b, 0xaa, 0x36, 0x6f, 0xaa, 0xb0,
                                                      0x3b, 0x09, 0x8d, 0x1b, 0x24, 0x0c, 0x76,
                                                      0x0f, 0xc7, 0xae, 0xb7, 0xd1, 0x4f, 0xa2,
                                                      0xaa, 0x33, 0xde, 0x29, 0xc7, 0x12, 0xd4,
                                                      0xd6, 0xd7, 0xd3, 0xd3} &&
              native_hash->halves ==
                  std::array<u8, 8>{0x61, 0x24, 0x7d, 0x20, 0x2b, 0xfd, 0x73, 0x01},
          "native hashes include salted allocated blocks and zero hashes for free blocks");
    st::ArchiveWriter native_writer{native, stream_key, mac_key};
    Check(native_writer.SetDivisionCount(1), "native fixture division count");
    const auto native_meta = native_writer.Chunk(0);
    st::ArchiveReader native_reader;
    Check(
        native_meta &&
            native_reader.Open(*native_meta, stream_key, mac_key, native_writer.InitialDataMac()) ==
                st::ArchiveReader::Error::None &&
            native_reader.Push(0x1000, *native_writer.Chunk(0x1000)) ==
                st::ArchiveReader::Error::None &&
            native_reader.Complete() && native_reader.Stream() == native.stream,
        "native hash metadata imports through authenticated chunks");
    auto bad_content = native_writer.Content();
    bad_content.short_hashes[0] ^= 1;
    const auto bad_bytes =
        std::span<const u8>{reinterpret_cast<const u8*>(&bad_content), sizeof(bad_content)};
    st::Key128 bad_iv{};
    st::RandomBytes(bad_iv);
    const auto bad_meta = st::EncryptStream(stream_key, bad_iv, st::StreamKeyGeneration, bad_bytes);
    st::ArchiveReader bad_hash_reader;
    Check(bad_hash_reader.Open(bad_meta, stream_key, mac_key, st::Cmac(mac_key, bad_bytes)) ==
                  st::ArchiveReader::Error::None &&
              bad_hash_reader.Push(0x1000, *native_writer.Chunk(0x1000)) ==
                  st::ArchiveReader::Error::Chunk &&
              !bad_hash_reader.Complete(),
          "incorrect native short hashes are rejected");
    st::ArchiveWriter prefix_writer{native, stream_key, mac_key};
    Check(prefix_writer.SetDivisionCount(4), "native prefix-only chunks");
    auto prefix_content = prefix_writer.Content();
    prefix_content.short_hashes[0] ^= 1;
    const auto prefix_bytes =
        std::span<const u8>{reinterpret_cast<const u8*>(&prefix_content), sizeof(prefix_content)};
    const auto prefix_meta =
        st::EncryptStream(stream_key, bad_iv, st::StreamKeyGeneration, prefix_bytes);
    st::ArchiveReader prefix_reader;
    bool prefix_rejected =
        prefix_reader.Open(prefix_meta, stream_key, mac_key, st::Cmac(mac_key, prefix_bytes)) ==
        st::ArchiveReader::Error::None;
    for (u16 id = 0x1000; id < 0x1004; ++id) {
        const auto rc = prefix_reader.Push(id, *prefix_writer.Chunk(id));
        prefix_rejected = prefix_rejected && rc == (id == 0x1003 ? st::ArchiveReader::Error::Chunk
                                                                 : st::ArchiveReader::Error::None);
    }
    Check(prefix_rejected && !prefix_reader.Complete(),
          "native short hashes are checked for prefix chunks whose full hashes equal plaintext "
          "hashes");
    st::ArchiveReader wrong;
    st::Key128 other_mac{};
    st::RandomBytes(other_mac);
    Check(wrong.Open(*meta, stream_key, mac_key, other_mac) == st::ArchiveReader::Error::Mac,
          "another MAC is refused");
    auto bad = *writer.Chunk(0x1000);
    bad[0x30] ^= 1;
    Check(reader.Push(0x1000, bad) == st::ArchiveReader::Error::Chunk,
          "a changed chunk is refused");
    bool all = true;
    for (const u16 id : {u16{0x1002}, u16{0x1000}, u16{0x1001}}) {
        all = all && reader.Push(id, *writer.Chunk(id)) == st::ArchiveReader::Error::None;
    }
    Check(all && reader.Complete() && reader.Stream() == source.stream,
          "chunks in any order give back the save image stream");
}

namespace transfer_check {

namespace tr = OpenPak::Olsc::Transfer;
namespace st = Service::FileSystem::SaveTransfer;

/// The saves server's console protocol as olsc-findings.md §6.1 specifies it, in memory.
class FakeServer final : public tr::Rest {
public:
    struct Part {
        std::uint64_t id;
        std::uint32_t index;
        std::string status = "uploading";
        tr::Bytes blob;
    };
    struct Version {
        std::uint64_t id;
        std::string status = "uploading";
        int partitions;
        std::vector<Part> parts;
        std::string key_seed, mac;
        std::uint64_t series = 7;
        std::uint64_t previous = 0;
        nlohmann::json metadata = nlohmann::json::object();
    };
    std::vector<Version> versions;
    std::vector<std::string> calls;
    std::vector<tr::Headers> call_headers;
    int partitions = 3;
    int puts = 0;
    bool delta_sent_seed = false;
    std::uint64_t next_id = 100;
    bool recovery_granted = true;
    bool deny_account_policy = false;
    std::optional<std::string> permission_reply;
    tr::Progress* cancel_on_finish_download{};
    std::stop_source* stop_on_finish_download{};
    std::stop_source* stop_on_blob{};
    std::uint32_t stop_on_blob_index{};

    std::uint64_t nsa_id = 0x1122334455667788ULL;
    std::uint64_t NetworkServiceAccountId() override {
        return nsa_id;
    }
    std::string DeviceToken() override {
        return "dtok";
    }

    tr::HttpResponse Request(const std::string& method, const std::string& host,
                             const std::string& path, const std::string& body,
                             const tr::Headers& headers) override {
        calls.push_back(method + " " + path);
        call_headers.push_back(headers);
        const auto json = nlohmann::json::parse(body.empty() ? "{}" : body, nullptr, false);
        if (host == tr::PolicyHost) {
            if (deny_account_policy)
                return {403, R"({"code":"0000"})"};
            return {200, R"({"policy_type":"ALL_OK"})"};
        }
        if (path.ends_with("/permissions")) {
            if (permission_reply)
                return {200, *permission_reply};
            auto permissions = nlohmann::json::array();
            for (const auto& v : versions) {
                if (recovery_granted && v.status == "fixed" && v.metadata.value("datatype", 0) == 3)
                    permissions.push_back(
                        {{"id", v.id},
                         {"save_data_archive_id", v.id},
                         {"nsa_id", fmt::format("{:016x}", nsa_id)},
                         {"application_id", v.metadata.value("application_id", std::string{})},
                         {"created_at_as_unixtime", 1700000000}});
            }
            return {200, nlohmann::json{{"permissions", permissions}}.dump()};
        }
        if (path.find("/save_data_archives?") != std::string::npos) {
            nlohmann::json list = nlohmann::json::array();
            for (const auto& v : versions) {
                list.push_back(nlohmann::json::parse(Archive(v, false))["save_data_archive"]);
            }
            return {200, nlohmann::json{{"save_data_archives", list}}.dump()};
        }
        if (path.ends_with("/save_data_archives/start_upload")) {
            versions.push_back({next_id++, "uploading", partitions, {}, {}, {}});
            versions.back().metadata = json;
            return {200, Archive(versions.back(), false)};
        }
        const auto v = FindVersion(path);
        const auto p = FindPart(path);
        if (path.ends_with("/start_upload") && v) {
            // A delta (saves §6.1): the base's components mirrored, handed over.
            Version next{next_id++, "uploading", v->partitions, {}, {}, {}, v->series, v->id};
            next.metadata = json;
            for (const auto& part : v->parts) {
                next.parts.push_back({next_id++, part.index, "hand_over", part.blob});
            }
            versions.push_back(next);
            return {200, Archive(versions.back(), false)};
        }
        if (path.ends_with("/component_files/create") && v) {
            v->parts.push_back({next_id++, json.value("index", 0u)});
            return {200, Component(v->parts.back(), "")};
        }
        if (path.ends_with("/update") && p) {
            p->status = "uploading";
            return {200, Component(*p, "put")};
        }
        if (path.ends_with("/signed_uri") && p) {
            const bool write = body.find("write") != std::string::npos;
            return {200, Component(*p, write ? "put" : "get")};
        }
        if (path.ends_with("/finish_upload") && p) {
            p->status = "fixed";
            const bool ok = json.value("archive_size", 0ull) == p->blob.size() &&
                            json.value("encoded_archive_digest", std::string{}) ==
                                tr::Base64Url(tr::Md5(p->blob));
            return {ok ? 200 : 400, ok ? "{}" : R"({"code":"0003"})"};
        }
        if (path.ends_with("/finish_upload") && v) {
            v->status = "fixed";
            v->key_seed = json.value("encoded_key_seed", std::string{});
            v->mac = json.value("encoded_mac", std::string{});
            if (v->previous != 0) {
                // The server change saves needs: a handed-over part keeps the base's bytes, and
                // the delta keeps the base's key seed.
                delta_sent_seed = delta_sent_seed || !v->key_seed.empty();
                for (const auto& base : versions) {
                    if (base.id == v->previous) {
                        v->key_seed = base.key_seed;
                    }
                }
                for (auto& part : v->parts) {
                    part.status = "fixed";
                }
            }
            return {200, Archive(*v, false)};
        }
        if (path.ends_with("/extend_upload_timeout") && v) {
            return {200, Archive(*v, false)};
        }
        if (path.ends_with("/generate_key_seed_package") && v) {
            // The fake fs reads this "package" back: challenge ∥ seed ∥ MAC, not the real format.
            const auto challenge =
                tr::FromBase64Url(json.value("encoded_challenge", std::string{}));
            tr::Bytes package(0x200);
            const auto seed = tr::FromBase64Url(v->key_seed);
            const auto mac = tr::FromBase64Url(v->mac);
            std::memcpy(package.data(), challenge->data(), 16);
            std::memcpy(package.data() + 16, seed->data(), 16);
            std::memcpy(package.data() + 32, mac->data(), 16);
            return {200,
                    nlohmann::json{{"encoded_key_seed_package", tr::Base64Url(package)}}.dump()};
        }
        if ((path.ends_with("/start_download") || path.ends_with("/finish_download")) && v) {
            if (path.ends_with("/finish_download") && cancel_on_finish_download)
                cancel_on_finish_download->cancelled = true;
            if (path.ends_with("/finish_download") && stop_on_finish_download)
                stop_on_finish_download->request_stop();
            return {200, Archive(*v, true)};
        }
        if (method == "GET" && v) {
            return {200, Archive(*v, true)};
        }
        return {404, R"({"code":"0001"})"};
    }

    tr::HttpResponse Blob(const std::string& method, const std::string& url,
                          std::span<const std::uint8_t> body) override {
        const auto id = std::stoull(url.substr(url.rfind('/') + 1));
        for (auto& v : versions) {
            for (auto& p : v.parts) {
                if (p.id == id) {
                    if (method == "PUT") {
                        ++puts;
                        p.blob.assign(body.begin(), body.end());
                        return {200, {}};
                    }
                    if (stop_on_blob && p.index == stop_on_blob_index)
                        stop_on_blob->request_stop();
                    return {200, std::string(p.blob.begin(), p.blob.end())};
                }
            }
        }
        return {404, {}};
    }

private:
    Version* FindVersion(const std::string& path) {
        const auto at = path.find("/save_data_archives/");
        if (at == std::string::npos) {
            return nullptr;
        }
        const auto id = std::strtoull(path.c_str() + at + 20, nullptr, 10);
        for (auto& v : versions) {
            if (v.id == id) {
                return &v;
            }
        }
        return nullptr;
    }
    Part* FindPart(const std::string& path) {
        const auto at = path.find("/component_files/");
        if (at == std::string::npos) {
            return nullptr;
        }
        const auto id = std::strtoull(path.c_str() + at + 17, nullptr, 10);
        for (auto& v : versions) {
            for (auto& p : v.parts) {
                if (p.id == id) {
                    return &p;
                }
            }
        }
        return nullptr;
    }
    static std::string Component(const Part& p, const std::string& url) {
        nlohmann::json c{{"id", p.id},
                         {"index", p.index},
                         {"datatype", p.index == 0 ? "meta" : "save"},
                         {"status", p.status},
                         {"archive_size", p.blob.size()}};
        if (url == "put") {
            c["put_url"] =
                fmt::format("https://scsi-upload.lp1.scsi.srv.nintendo.net/blob/{}", p.id);
        } else {
            c["get_url"] =
                fmt::format("https://scsi-download.lp1.scsi.srv.nintendo.net/blob/{}", p.id);
        }
        return nlohmann::json{{"component_file", c}}.dump();
    }
    std::string Archive(const Version& v, bool with_urls) {
        nlohmann::json parts = nlohmann::json::array();
        for (const auto& p : v.parts) {
            nlohmann::json c{{"id", p.id},
                             {"index", p.index},
                             {"status", p.status},
                             {"archive_size", p.blob.size()}};
            if (with_urls) {
                c["get_url"] =
                    fmt::format("https://scsi-download.lp1.scsi.srv.nintendo.net/blob/{}", p.id);
            }
            parts.push_back(c);
        }
        auto archive = v.metadata;
        archive["id"] = v.id;
        archive["status"] = v.status;
        archive["series_id"] = v.series;
        archive["num_of_partitions"] = v.partitions;
        archive["component_files"] = parts;
        archive["nsa_id"] = fmt::format("{:016x}", nsa_id);
        archive["device_id"] = "old-device";
        archive["device_serial"] = "";
        archive["previous_save_data_archive_id"] =
            v.previous ? nlohmann::json(v.previous) : nlohmann::json(nullptr);
        archive["finished_at_as_unixtime"] =
            v.status == "fixed" ? nlohmann::json(1700000001) : nlohmann::json(nullptr);
        return nlohmann::json{{"save_data_archive", archive}}.dump();
    }
};

class FakeExporter final : public tr::Exporter {
public:
    FakeExporter(const st::ArchiveSource& source, const st::Key128& k, const st::Key128& m,
                 const tr::Key& seed_)
        : writer{source, k, m}, seed{seed_} {}
    bool SetDivisionCount(int count) override {
        return writer.SetDivisionCount(count);
    }
    std::vector<std::uint16_t> ChunkIds() override {
        return writer.ChunkIds();
    }
    std::optional<tr::Bytes> Chunk(std::uint16_t id) override {
        return writer.Chunk(id);
    }
    void SetAad(const tr::Aad& aad) override {
        writer.SetAad(aad);
    }
    tr::Aad ImportAad() override {
        return writer.Aad();
    }
    tr::Key KeySeed() override {
        return seed;
    }
    std::optional<tr::Key> InitialDataMac() override {
        return writer.InitialDataMac();
    }
    bool Finalize() override {
        return true;
    }
    st::ArchiveWriter writer;
    tr::Key seed;
};

class FakeImporter final : public tr::Importer {
public:
    tr::Aad ImportAad() override {
        return reader.Content().aad;
    }
    std::vector<std::uint16_t> ChunkIds() override {
        return reader.ChunkIds();
    }
    bool Push(std::uint16_t id, std::span<const std::uint8_t> bytes) override {
        return reader.Push(id, bytes) == st::ArchiveReader::Error::None;
    }
    bool Finalize() override {
        if (!reader.Complete()) {
            return false;
        }
        *imported = reader.Stream();
        return true;
    }
    st::ArchiveReader reader;
    tr::Bytes* imported = nullptr;
};

/// One console: a stream key derived from the seed by a stand-in (the real derivation is
/// checked above); the transfer flow only needs both ends to agree.
class FakeFs final : public tr::Fs {
public:
    explicit FakeFs(st::ArchiveSource source_) : source{std::move(source_)} {
        st::RandomBytes(challenge);
        st::RandomBytes(own_seed);
        st::RandomBytes(mac_key);
    }
    static st::Key128 StreamKey(const tr::Key& seed) {
        return st::Cmac(seed, std::array<std::uint8_t, 4>{'k', 'e', 'y', '3'});
    }
    tr::Key Challenge() override {
        return challenge;
    }
    bool SetKeySeedPackage(std::span<const std::uint8_t> package) override {
        if (std::memcmp(package.data(), challenge.data(), 16) != 0) {
            return false;
        }
        std::memcpy(package_seed.data(), package.data() + 16, 16);
        std::memcpy(package_mac.data(), package.data() + 32, 16);
        has_package = true;
        return true;
    }
    std::unique_ptr<tr::Exporter> OpenFullExporter() override {
        return std::make_unique<FakeExporter>(source, StreamKey(own_seed), *shared_mac_key,
                                              own_seed);
    }
    std::unique_ptr<tr::Exporter> OpenDiffExporter(std::span<const std::uint8_t> meta) override {
        st::ArchiveReader base;
        if (!has_package || base.Open(meta, StreamKey(package_seed), *shared_mac_key,
                                      package_mac) != st::ArchiveReader::Error::None) {
            return nullptr;
        }
        auto exporter = std::make_unique<FakeExporter>(source, StreamKey(package_seed),
                                                       *shared_mac_key, package_seed);
        if (!exporter->writer.SetBase(base.Content())) {
            return nullptr;
        }
        return exporter;
    }
    std::unique_ptr<tr::Importer> OpenImporter(std::span<const std::uint8_t> meta) override {
        if (!has_package) {
            return nullptr;
        }
        auto importer = std::make_unique<FakeImporter>();
        if (importer->reader.Open(meta, StreamKey(package_seed), *shared_mac_key, package_mac) !=
            st::ArchiveReader::Error::None) {
            return nullptr;
        }
        importer->imported = &imported;
        return importer;
    }
    st::ArchiveSource source;
    st::Key128 challenge{}, own_seed{}, mac_key{}, package_seed{}, package_mac{};
    const st::Key128* shared_mac_key = &mac_key;
    bool has_package = false;
    tr::Bytes imported;
};

} // namespace transfer_check

void TransferFlowChecks() {
    using namespace transfer_check;
    std::puts("save data cloud transfer");
    Check(tr::Base64Url(std::array<std::uint8_t, 4>{0xfb, 0xff, 0x00, 0x01}) == "-_8AAQ" &&
              tr::FromBase64Url("-_8AAQ") == tr::Bytes({0xfb, 0xff, 0x00, 0x01}),
          "base64url without padding, both ways");
    tr::LocalSave save;
    save.application_id = 0x0100A5A020D5E000;
    save.data_size = 0x80000;
    save.saved_at = 1700000000;
    save.commit_id = 0x1122334455667788;
    save.acd_index = 2;
    save.launch_required_version = 3;
    const auto body = tr::StartUploadBody(save, 9);
    Check(body.starts_with(
              R"({"application_id":"0100a5a020d5e000","datatype":0,"data_size":524288,)") &&
              body.find(R"("encoded_digest":"iHdmVUQzIhEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",)") !=
                  std::string::npos &&
              body.ends_with(
                  R"("has_thumbnail":false,"desc":null,"auto_backup":false,"acd_index":2})"),
          "start_upload's body has the console's keys, order and digest");

    st::ArchiveSource source;
    source.stream.resize(7 * 0x4000);
    st::RandomBytes(source.stream);
    auto device = save;
    device.application_id = 0x01006f8002326000ULL;
    device.datatype = 3;
    device.desc = "island \"quoted\"";
    const auto device_body = nlohmann::json::parse(tr::StartUploadBody(device, 9));
    Check(device_body["datatype"] == 3 && device_body["desc"] == device.desc,
          "island uploads use datatype 3 and escape their custom data");
    FakeServer device_server;
    auto device_source = source;
    device_source.extra_data[0x20] = 3;
    FakeFs device_fs{device_source};
    tr::Progress device_progress;
    const auto device_up = tr::Upload(device_server, device_fs, device, 0, &device_progress);
    Check(device_up.error == tr::Error::None && device_progress.done == device_progress.total &&
              !device_server.calls.empty() &&
              device_server.calls.front().find(
                  "datatype=3&application_id=01006f8002326000&device_id=me") != std::string::npos,
          "island upload uses its device-scoped list and completes all components");
    const auto device_list = tr::ListDeviceBackups(device_server, device.application_id, 1);
    Check(device_list.error == tr::Error::None && device_list.archives.size() == 1 &&
              device_list.archives.front().id == device_up.archive_id &&
              device_list.archives.front().desc == device.desc,
          "island recovery joins fixed archives with the owner's server grants");
    const auto packed_island = OpenPak::Olsc::PackBackupInfo(device_list.archives.front());
    std::uint64_t packed_id{}, packed_time{};
    std::memcpy(&packed_id, packed_island.data(), 8);
    std::memcpy(&packed_time, packed_island.data() + 8, 8);
    Check(packed_id == device_up.archive_id && packed_time == 1700000001 &&
              std::string(reinterpret_cast<const char*>(packed_island.data() + 0x10)) ==
                  device.desc &&
              packed_island.back() == 0,
          "downloadable island records use the native 0x410 layout");
    Check(tr::ListDeviceBackups(device_server, device.application_id, 0).error ==
              tr::Error::BackupListOverflow,
          "a small transfer-memory capacity refuses excess island records");
    device_server.recovery_granted = false;
    Check(tr::ListDeviceBackups(device_server, device.application_id, 1).archives.empty(),
          "an island without a recovery grant is not offered");
    device_server.recovery_granted = true;
    device_server.permission_reply = R"({"permissions":[{"id":1,"save_data_archive_id":100}]})";
    Check(tr::ListDeviceBackups(device_server, device.application_id, 1).error ==
              tr::Error::PermissionIncomplete,
          "incomplete native recovery grants return 2179-3320");
    device_server.permission_reply.reset();
    Check(
        !OpenPak::Olsc::ParseBackupPermissions(
            R"({"permissions":[{"id":"18446744073709551616","save_data_archive_id":1,"nsa_id":"1122334455667788","application_id":"01006f8002326000","created_at_as_unixtime":1}]})"),
        "overflowing permission ids are rejected without throwing");
    auto granted = *OpenPak::Olsc::ParseBackupPermissions(fmt::format(
        R"({{"permissions":[{{"id":1,"save_data_archive_id":{},"nsa_id":"1122334455667788","application_id":"01006f8002326000","created_at_as_unixtime":1}}]}})",
        device_up.archive_id));
    ++granted.front().nsa_id;
    Check(OpenPak::Olsc::DownloadableBackups(device_list.archives, granted, device.application_id,
                                             device_server.nsa_id)
              .empty(),
          "another account's recovery grant cannot authorize an island");

    FakeFs device_destination{st::ArchiveSource{}};
    device_destination.shared_mac_key = &device_fs.mac_key;
    device_server.deny_account_policy = true;
    tr::Progress device_download_progress;
    Check(tr::Download(device_server, device_destination, device.application_id,
                       device_up.archive_id, 3, &device_download_progress, 3)
                      .error == tr::Error::None &&
              device_destination.imported == device_source.stream,
          "a second device restores the same island through the device policy");
    Check(tr::Download(device_server, device_destination, device.application_id,
                       device_up.archive_id, 2, nullptr, 3)
                  .error == tr::Error::ApplicationTooOld,
          "an older game version cannot import an island backup");
    FakeFs cancelled_destination{st::ArchiveSource{}};
    cancelled_destination.shared_mac_key = &device_fs.mac_key;
    tr::Progress cancelled_progress;
    device_server.cancel_on_finish_download = &cancelled_progress;
    Check(tr::Download(device_server, cancelled_destination, device.application_id,
                       device_up.archive_id, 3, &cancelled_progress, 3)
                      .error == tr::Error::Cancelled &&
              cancelled_destination.imported.empty(),
          "a cancelled island restore stops before writing the destination save");
    device_server.cancel_on_finish_download = nullptr;
    std::stop_source request_cancellation;
    tr::Progress request_progress;
    request_progress.cancellation = request_cancellation.get_token();
    device_server.stop_on_finish_download = &request_cancellation;
    Check(tr::Download(device_server, cancelled_destination, device.application_id,
                       device_up.archive_id, 3, &request_progress, 3)
                      .error == tr::Error::Cancelled &&
              cancelled_destination.imported.empty(),
          "IPC cancellation reaches island import immediately through the request's stop token");
    device_server.stop_on_finish_download = nullptr;
    std::stop_source blob_cancellation;
    tr::Progress blob_progress;
    blob_progress.cancellation = blob_cancellation.get_token();
    device_server.stop_on_blob = &blob_cancellation;
    Check(tr::Download(device_server, cancelled_destination, device.application_id,
                       device_up.archive_id, 3, &blob_progress, 3)
                      .error == tr::Error::Cancelled &&
              cancelled_destination.imported.empty(),
          "cancellation during metadata download leaves the island untouched");
    device_server.stop_on_blob = nullptr;
    std::stop_source chunk_cancellation;
    tr::Progress chunk_progress;
    chunk_progress.cancellation = chunk_cancellation.get_token();
    device_server.stop_on_blob = &chunk_cancellation;
    device_server.stop_on_blob_index = 4096;
    Check(tr::Download(device_server, cancelled_destination, device.application_id,
                       device_up.archive_id, 3, &chunk_progress, 3)
                      .error == tr::Error::Cancelled &&
              cancelled_destination.imported.empty(),
          "cancellation during an island chunk download stops before importing it");
    device_server.stop_on_blob = nullptr;
    FakeServer pre_cancelled_server;
    Check(tr::Download(pre_cancelled_server, cancelled_destination, device.application_id,
                       device_up.archive_id, 3, &blob_progress, 3)
                      .error == tr::Error::Cancelled &&
              pre_cancelled_server.calls.empty(),
          "a restore cancelled before execution makes no network request");
    Check(tr::Download(device_server, cancelled_destination, 0x0100e6c01163c000ULL,
                       device_up.archive_id, 3, nullptr, 3)
                      .error == tr::Error::ArchiveIncomplete &&
              cancelled_destination.imported.empty(),
          "an archive for another application cannot replace the requested island");
    device.application_id = 0x0100000000000001ULL;
    FakeServer forbidden_device_server;
    Check(tr::Upload(forbidden_device_server, device_fs, device, 0, &device_progress).error ==
                  tr::Error::PolicyForbidden &&
              forbidden_device_server.calls.empty(),
          "device policy refuses other titles before making network requests");
    FakeServer server;
    FakeFs harbor_a{source};
    tr::Progress progress;
    const auto up = tr::Upload(server, harbor_a, save, 0, &progress);
    Check(up.error == tr::Error::None && server.versions.size() == 1 &&
              server.versions[0].status == "fixed" && server.versions[0].parts.size() == 4 &&
              progress.done == progress.total && progress.total > 0,
          "an upload creates every component (meta included), PUTs them and finishes");
    Check(server.versions[0].key_seed == tr::Base64Url(harbor_a.own_seed),
          "finish_upload carries the exporter's key seed and MAC");
    const auto& uploaded_meta =
        *std::find_if(server.versions[0].parts.begin(), server.versions[0].parts.end(),
                      [](const auto& part) { return part.index == 0; });
    const auto native_meta =
        st::DecryptStream(FakeFs::StreamKey(harbor_a.own_seed), uploaded_meta.blob);
    std::uint64_t bound_account = 0, bound_archive = 0;
    if (native_meta && native_meta->size() == sizeof(st::InitialDataContent)) {
        std::memcpy(&bound_account, native_meta->data() + 0x1020, 8);
        std::memcpy(&bound_archive, native_meta->data() + 0x1028, 8);
    }
    Check(bound_account == server.nsa_id && bound_archive == up.archive_id &&
              bound_account != save.application_id,
          "initial data binds X-NSA-ID and archive ID, as native OLSC checks");
    Check(up.commit_id == save.commit_id && up.series_id != 0,
          "an upload reports the archive's series and commit for SeriesInfo");
    // Download on a second console of the same account (shares the MAC key: same keys).
    FakeFs harbor_b{st::ArchiveSource{}};
    harbor_b.shared_mac_key = &harbor_a.mac_key;
    Check(server.calls[0].ends_with("/application_policy/0100a5a020d5e000/00003?dtoken=dtok"),
          "the policy request carries the launch version and the device token");
    bool sda_on_archive_calls = true;
    bool ignorable_counts = true;
    unsigned extend = 0;
    for (std::size_t i = 0; i < server.calls.size(); ++i) {
        const auto& h = server.call_headers[i];
        const bool has_sda = std::any_of(h.begin(), h.end(), [&](const auto& kv) {
            return kv.first == "X-SDA-ID" && kv.second == std::to_string(up.archive_id);
        });
        const bool per_archive =
            server.calls[i].find("/save_data_archives/1") != std::string::npos ||
            server.calls[i].find("/component_files/") != std::string::npos;
        sda_on_archive_calls = sda_on_archive_calls && has_sda == per_archive;
        if (server.calls[i].ends_with("/extend_upload_timeout")) {
            ignorable_counts =
                ignorable_counts && std::any_of(h.begin(), h.end(), [&](const auto& kv) {
                    return kv.first == "X-Ignorable" &&
                           kv.second == fmt::format("counter={}", extend);
                });
            ++extend;
        }
    }
    Check(sda_on_archive_calls && ignorable_counts && extend == 4,
          "X-SDA-ID on every request about the archive, X-Ignorable counts the chunks");
    const auto down =
        tr::Download(server, harbor_b, save.application_id, up.archive_id, 3, nullptr);
    Check(down.error == tr::Error::None && harbor_b.imported == source.stream,
          "the second console downloads the same save image");

    ++server.nsa_id;
    Check(tr::Download(server, harbor_b, save.application_id, up.archive_id, 3, nullptr).error ==
              tr::Error::AadMismatch,
          "native metadata for a different network account is rejected");
    --server.nsa_id;

    // Read an old Harbor backup and use it as a base, while emitting native account AAD.
    auto& legacy_meta =
        *std::find_if(server.versions[0].parts.begin(), server.versions[0].parts.end(),
                      [](const auto& part) { return part.index == 0; });
    auto legacy_plain = *native_meta;
    std::memcpy(legacy_plain.data() + 0x1020, &save.application_id, 8);
    st::Key128 legacy_iv{};
    st::RandomBytes(legacy_iv);
    legacy_meta.blob = st::EncryptStream(FakeFs::StreamKey(harbor_a.own_seed), legacy_iv,
                                         st::StreamKeyGeneration, legacy_plain);
    server.versions[0].mac = tr::Base64Url(st::Cmac(harbor_a.mac_key, legacy_plain));
    Check(tr::Download(server, harbor_b, save.application_id, up.archive_id, 3, nullptr).error ==
              tr::Error::None,
          "older Harbor metadata remains readable after the account binding correction");

    // A changed block: the next upload is a delta of the one chunk that holds it.
    auto changed = source;
    changed.stream[3 * 0x4000 + 5] ^= 0xff; // block 3: chunk 1 of 3 (2 + 2 + 3 blocks)
    FakeFs harbor_e{changed};
    harbor_e.shared_mac_key = &harbor_a.mac_key;
    server.puts = 0;
    const auto delta = tr::Upload(server, harbor_e, save, 0, nullptr);
    Check(delta.error == tr::Error::None && server.versions.size() == 2 &&
              server.versions[1].previous == up.archive_id && server.puts == 2 &&
              !server.delta_sent_seed,
          "a delta upload sends the changed chunk and the meta, and the MAC without a seed");
    const auto& delta_meta =
        *std::find_if(server.versions[1].parts.begin(), server.versions[1].parts.end(),
                      [](const auto& part) { return part.index == 0; });
    const auto delta_plain =
        st::DecryptStream(FakeFs::StreamKey(harbor_a.own_seed), delta_meta.blob);
    bound_account = 0;
    if (delta_plain) {
        std::memcpy(&bound_account, delta_plain->data() + 0x1020, 8);
    }
    Check(bound_account == server.nsa_id,
          "a delta of a legacy Harbor backup emits the native account binding");
    FakeFs harbor_f{st::ArchiveSource{}};
    harbor_f.shared_mac_key = &harbor_a.mac_key;
    const auto down2 =
        tr::Download(server, harbor_f, save.application_id, delta.archive_id, 3, nullptr);
    Check(down2.error == tr::Error::None && harbor_f.imported == changed.stream,
          "the delta archive downloads as the changed save image");

    FakeFs harbor_c{st::ArchiveSource{}};
    harbor_c.shared_mac_key = &harbor_a.mac_key;
    const auto gone = tr::Download(server, harbor_c, save.application_id, 999999, 3, nullptr);
    Check(gone.error == tr::Error::ArchiveGone, "a missing archive is 2179-2100");
    FakeServer empty;
    empty.partitions = 0;
    FakeFs harbor_d{source};
    Check(tr::Upload(empty, harbor_d, save, 0, nullptr).error == tr::Error::Fs,
          "num_of_partitions 0 fails at SetDivisionCount");
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

// A store that holds "device-x" but cannot read it, and a pre-namespace "legacy" entry.
class UnreadableStore final : public OpenPak::SecretStore {
public:
    std::optional<std::string> Load(std::string_view key, bool* unreadable) override {
        if (key.ends_with("device-x")) {
            if (unreadable) {
                *unreadable = true;
            }
            return std::nullopt;
        }
        return key == "legacy" ? std::optional<std::string>{"old"} : std::nullopt;
    }
    bool Store(std::string_view key, std::string_view) override {
        stored.emplace_back(key);
        return true;
    }
    void Erase(std::string_view) override {}
    std::vector<std::string> stored;
};

// A secret the store holds but cannot read is not "none": the session must never replace a
// linked device account because of it (session.cpp LoadDeviceAccount).
void SecretChecks() {
    const auto store = std::make_shared<UnreadableStore>();
    OpenPak::SetSecretStore(store);
    OpenPak::SetSecretNamespace([] { return std::string{"ns"}; });
    bool unreadable = false;
    Check(!OpenPak::LoadSecret("device-x", &unreadable) && unreadable,
          "an unreadable secret reports unreadable through the namespace");
    Check(store->stored.empty(), "an unreadable secret is not migrated or rewritten");
    unreadable = false;
    Check(!OpenPak::LoadSecret("missing", &unreadable) && !unreadable,
          "a missing secret is only missing");
    Check(OpenPak::LoadSecret("legacy") == "old", "a pre-namespace entry still loads");
    OpenPak::SetSecretNamespace({});
    OpenPak::SetSecretStore(nullptr);
}

// aauth refusal codes as the account module maps them to 2124 results (versions-research §3).
void AauthChecks() {
    namespace se = OpenPak::Session;
    Check(se::AauthRefusalDescription("0111") == 3001u, "0111 is 2124-3001, a software update");
    Check(se::AauthRefusalDescription("0105") == 4605u &&
              se::AauthRefusalDescription("0121") == 4621u,
          "the 01xx restrictions are 2124-45xx + code");
    Check(!se::AauthRefusalDescription("0104") && !se::AauthRefusalDescription(""),
          "an unmapped code refuses nothing");
    // The gate's answer for an old version (versions-research §3).
    const std::string refused =
        R"({"errors":[{"code":"0111","message":"Application update is required."}]})";
    Check(se::AauthErrorCode(400, refused) == "0111", "a 400 with errors[0].code is read");
    Check(se::AauthErrorCode(200, refused).empty() && se::AauthErrorCode(400, "nope").empty(),
          "a success or a body without errors is no refusal");
}

void BcatKeyPatchChecks(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    const fs::path load = dir / "bcat-load";
    const fs::path other = load / "0100000000000000" / "some_mod" / "exefs" / "x.ips";
    OpenPak::Platform::WriteFile(other, "OTHER");
    Check(OpenPak::BcatKeyPatch::Install(load), "the bcat key patch installs into a fresh load dir");
    const fs::path ips = load / "010000000000000C" / "openpak_bcat_key" / "exefs" /
                         "84EF6747BB15A01707B883B29A2A1CE5FC1734C2000000000000000000000000.ips";
    const std::string first = OpenPak::Platform::ReadFile(ips);
    Check(first.size() == 270 && first.compare(0, 5, "PATCH") == 0 &&
              first.compare(first.size() - 3, 3, "EOF") == 0,
          "the installed patch is the 270-byte IPS");
    const auto stamp = fs::last_write_time(ips);
    Check(OpenPak::BcatKeyPatch::Install(load) && fs::last_write_time(ips) == stamp &&
              OpenPak::Platform::ReadFile(ips) == first,
          "a second install leaves the patch alone");
    OpenPak::Platform::WriteFile(ips, "stale");
    Check(OpenPak::BcatKeyPatch::Install(load) && OpenPak::Platform::ReadFile(ips) == first,
          "a different file is replaced");
    Check(OpenPak::Platform::ReadFile(other) == "OTHER", "another mod is untouched");
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
    FetchChecks();
    RedactChecks();
    OlscChecks(dir);
    SaveTransferChecks(dir);
    SaveArchiveChecks();
    TransferFlowChecks();
    SecretChecks();
    AauthChecks();
    BcatKeyPatchChecks(dir);

    // Last, because it replaces the process ceiling: the bsd hook's lookup.
    std::puts("ip redirects");
    const std::array<unsigned char, 4> listed = {192, 0, 2, 7};
    Check(!np::IpRedirectFor(listed, 23001, true, "switch"),
          "no ip redirect while the ceiling lists none");
    const std::string ip_payload =
        R"({"type":"openpak-ceiling","version":7,"platforms":{"switch":[".nintendo.net"]},)"
        R"("ip_redirects":{"switch":[{"from":"192.0.2.7:23001","to":"203.0.113.10:22690",)"
        R"("proto":"tcp"}]}})";
    np::LoadCeiling(Envelope(ip_payload, Sign(ip_payload), keys[0].keyid), keys);
    const auto hit = np::IpRedirectFor(listed, 23001, true, "switch");
    Check(hit && hit->first == std::array<unsigned char, 4>{203, 0, 113, 10} &&
              hit->second == 22690,
          "a listed destination goes to its signed target");
    Check(!np::IpRedirectFor(listed, 23002, true, "switch") &&
              !np::IpRedirectFor(listed, 23001, false, "switch") &&
              !np::IpRedirectFor(listed, 23001, true, "wiiu") &&
              !np::IpRedirectFor({192, 0, 2, 8}, 23001, true, "switch"),
          "another port, protocol, platform or address is left alone");

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
    std::printf(
        "boot: %s (%s), website %s, console server %s, profile %s v%lld, ceiling %s v%lld%s%s\n",
        result.enabled ? "on" : "off", result.online ? "online" : "offline", result.website.c_str(),
        result.console_server.c_str(), result.profile_source.c_str(), result.profile_version,
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
        std::printf(
            "file: %s (%ju bytes)\n", entry.path().filename().string().c_str(),
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
