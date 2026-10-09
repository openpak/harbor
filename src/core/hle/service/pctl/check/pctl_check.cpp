// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check of pctl's parent-app pairing as Harbor speaks it to the moon API: the console's parse
// tables (a wrong JSON type on a known key is 2142-0234), the error identities, the IPC structs'
// layout, the setting document, the restriction rules, the changed-settings mask, the request
// headers and the async slots. No emulator, no network. Translated from the Ryujinx Harbor's
// PctlPairingTests. Build and run from the source root:
//
//   c++ -std=c++20 -Isrc -I<nlohmann_json>/include src/core/hle/service/pctl/check/pctl_check.cpp
//       src/core/hle/service/pctl/moon.cpp src/core/hle/service/pctl/pctl_setting.cpp -lfmt
//       -o pctl_check
//   ./pctl_check
//
// Prints "pctl_check: all checks passed" and exits 0, or names the failed check and exits 1.

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/hle/service/pctl/moon.h"
#include "core/hle/service/pctl/pctl_async.h"
#include "core/hle/service/pctl/pctl_setting.h"

namespace pctl = Service::PCTL;
namespace moon = Service::PCTL::Moon;
namespace rules = Service::PCTL::Rules;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) {
        ++failures;
    }
}

std::string Base64Url(const std::string& text) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < text.size(); i += 3) {
        const unsigned n = (static_cast<unsigned char>(text[i]) << 16) |
                           (static_cast<unsigned char>(text[i + 1]) << 8) |
                           static_cast<unsigned char>(text[i + 2]);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (i + 1 == text.size()) {
        const unsigned n = static_cast<unsigned char>(text[i]) << 16;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
    } else if (i + 2 == text.size()) {
        const unsigned n = (static_cast<unsigned char>(text[i]) << 16) |
                           (static_cast<unsigned char>(text[i + 1]) << 8);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
    }
    return out;
}

void Structs() {
    Check(sizeof(pctl::AsyncData) == 8 && offsetof(pctl::AsyncData, index) == 4,
          "AsyncData: 8 bytes, index at +4");
    Check(sizeof(pctl::PairingInfoBase) == 0x10 && offsetof(pctl::PairingInfoBase, state) == 8 &&
              offsetof(pctl::PairingInfoBase, is_copy) == 0xc,
          "PairingInfoBase: 0x10 bytes, state at +8, copy flag at +0xc");
    Check(sizeof(pctl::PairingAccountInfoBase) == 0x10 &&
              offsetof(pctl::PairingAccountInfoBase, nintendo_account_id) == 8,
          "PairingAccountInfoBase: 0x10 bytes, NA id at +8");
}

void Search() {
    std::uint64_t id = 0;
    Check(moon::ParseSearch(R"({"count":1,"items":[{"id":"0000b9932f2e93d5","deviceId":12345}]})",
                            id) == moon::Success &&
              id == 0x0000b9932f2e93d5ULL,
          "search reads a hex-string id");
    const std::pair<const char*, int> cases[] = {
        {R"({"count":1,"items":[{"id":203893428}]})", 234},
        {R"({"count":1,"items":[{"id":"00000000000000001"}]})", 234},
        {R"({"count":1,"items":[{"id":"12g4"}]})", 234},
        {R"({"count":"1","items":[]})", 234},
        {R"({"count":0,"items":[]})", 201},
        {R"({"count":2,"items":[{"id":"1"},{"id":"2"}]})", 201},
        {R"({"count":1,)", 232},
    };
    bool all = true;
    for (const auto& [body, description] : cases) {
        all = all && moon::ParseSearch(body, id) == description;
    }
    Check(all, "search: numeric/long/non-hex id and string count 0234, 0 or 2 hits 0201, cut 0232");
}

void Owners() {
    const std::string nickname = std::string(30, 'a') + "\xC3\xA9\xC3\xA9";
    std::string items;
    for (int i = 0; i < 7; ++i) {
        char id[17];
        std::snprintf(id, sizeof(id), "%016x", i + 1);
        items += std::string(i == 0 ? "" : ",") +
                 R"({"deviceId":"00000683f2fccda6","user":{"nintendoAccountId":")" + id +
                 R"(","nickname":")" + nickname +
                 R"(","miiUri":{"small":"https://api-lp1.pctl.srv.nintendo.net/moon/v1/mii/x/face.png"}}})";
    }
    std::vector<moon::Owner> owners;
    const auto result = moon::ParseOwners(R"({"count":7,"items":[)" + items + "]}", owners);
    Check(result == moon::Success && owners.size() == moon::MaxOwners,
          "owners: the first five are kept");
    Check(!owners.empty() && owners[0].device_id == 0x00000683f2fccda6ULL &&
              owners[4].nintendo_account_id == 5,
          "owners: device id and NA ids read as hex strings");
    Check(!owners.empty() && owners[0].nickname == std::string(30, 'a') + "\xC3\xA9",
          "owners: 32 bytes of a nickname, cut on a character");
    Check(!owners.empty() && owners[0].mii_url.ends_with("/face.png"), "owners: the picture URL");

    Check(moon::ParseOwners(
              R"({"count":1,"items":[{"deviceId":"1","user":{"nintendoAccountId":"2","nickname":"n","miiUri":{"small":"https://h/mii.png"},"iconUri":{"small":"https://h/icon-1.png"}}}]})",
              owners) == moon::Success &&
              owners[0].mii_url == "https://h/icon-1.png",
          "owners: 23.0.1's iconUri is preferred");
    Check(moon::ParseOwners(
              R"({"count":1,"items":[{"deviceId":"1","user":{"nickname":"n","miiUri":{"small":"https://h/mii.png"}}}]})",
              owners) == moon::Success &&
              owners[0].mii_url == "https://h/mii.png",
          "owners: a 22.x server's miiUri still gives the picture");
    Check(moon::ParseOwners(R"({"count":1,"items":[{"deviceId":"1","user":{"iconUri":null}}]})",
                            owners) == moon::Refused,
          "owners: iconUri null refuses (non-nullable on 23.0.1)");
    Check(moon::ParseOwners(R"({"count":1,"items":[{"deviceId":"1","user":{"nintendoAccountId":1}}]})",
                            owners) == moon::Refused &&
              moon::ParseOwners(R"({"count":1,"items":[{"deviceId":"1","user":{"nickname":7}}]})",
                                owners) == moon::Refused &&
              moon::ParseOwners(R"({"count":1,"items":[{"deviceId":"1","user":"someone"}]})",
                                owners) == moon::Refused,
          "owners: a numeric NA id, numeric nickname or string user refuses");
}

void Activate() {
    std::string etag;
    std::int64_t at = 0;
    Check(moon::ParseActivate(
              R"({"id":"1","synchronizedParentalControlSetting":{"synchronizedEtag":"\"abc\"","synchronizedAt":1790511687}})",
              etag, at) == moon::Success &&
              etag == "\"abc\"" && at == 1790511687,
          "activate answer gives the etag and time");
    Check(moon::ParseActivate(R"({"synchronizedParentalControlSetting":{"synchronizedEtag":")" +
                                  std::string(96, 'e') + "\"}}",
                              etag, at) == moon::Refused,
          "activate: an etag of 96 bytes refuses");

    const auto body = nlohmann::json::parse(
        moon::ActivateBody("en-GB", moon::RegionName(2), "XKJ70000000001", "22.5.0", 0x160500,
                           1790511687, ""));
    Check(body["language"] == "en-GB" && body["region"] == "EUROPE" &&
              body["serialNumber"] == "XKJ70000000001" &&
              body["firmwareVersion"]["displayedVersion"] == "22.5.0" &&
              body["firmwareVersion"]["internalVersion"] == 1443072 && body["activated"] == true &&
              body["lastOnlineCheckedAt"] == 1790511687 && !body.contains("parentalControlSetting") &&
              !body.contains("notificationToken"),
          "activate body has the console's keys");
    const std::string ordered =
        moon::ActivateBody("en-GB", "EUROPE", "S", "23.0.1", 1, 2, "penne");
    Check(ordered.find("\"language\"") < ordered.find("\"region\"") &&
              ordered.find("\"lastOnlineCheckedAt\"") < ordered.find("\"notificationToken\""),
          "activate body keeps the hardware key order");

    const auto receipt = nlohmann::json::parse(moon::ReceiptBody("\"t\"", 1790511700, "", ""));
    Check(receipt["synchronizedParentalControlSetting"]["synchronizedEtag"] == "\"t\"" &&
              receipt["synchronizedParentalControlSetting"]["synchronizedAt"] == 1790511700 &&
              !receipt.contains("synchronizedUnlockCode"),
          "receipt names the etag held, no PIN when none is set");
    const auto with_pin = nlohmann::json::parse(moon::ReceiptBody("\"t\"", 1790511700, "4827", ""));
    Check(with_pin["synchronizedUnlockCode"] == "4827", "receipt names the PIN in force");
}

void Settings() {
    pctl::Setting setting;
    Check(pctl::ParseSetting(R"({"etag":"\"t\"","functionalRestrictionLevel":"NONE","updatedAt":1790500000})",
                             setting) == moon::Success &&
              setting.etag == "\"t\"" && setting.updated_at == 1790500000 &&
              !setting.unlock_code.has_value(),
          "setting gives its etag and update time; no PIN leaves the console's");
    Check(pctl::ParseSetting(R"({"updatedAt":"yesterday"})", setting) == moon::Refused,
          "setting: a string updatedAt refuses");

    const char* body = R"({"etag":"\"e\"","unlockCode":"4827","functionalRestrictionLevel":"CUSTOM",
         "customSettings":{"ageRestriction":{"restriction":"RESTRICTED","ratingAge":12},
           "ugcRestriction":{"restriction":"RESTRICTED","applications":{"0100B3C014BDA000":{"applicationId":"0100B3C014BDA000","restriction":"UNRESTRICTED"},
                                                                 "01007D8018FE2000":{"restriction":"RESTRICTED"}}},
           "snsPostRestriction":"RESTRICTED","ratingOrganization":"PEGI_BBFC","vrRestriction":"RESTRICTED"},
         "whitelistedApplications":{"0100000000010000":{"safeLaunch":"ALLOW"},"0100000000020000":{"safeLaunch":"NONE"}},
         "updatedAt":1790500000})";
    const bool parsed = pctl::ParseSetting(body, setting) == moon::Success;
    Check(parsed && setting.unlock_code == "4827" && setting.safety_level == 1 &&
              setting.age_restricted && setting.custom_rating_age == 12 &&
              setting.custom_sns_post_restricted && setting.custom_free_communication_restricted &&
              setting.rating_organization == 8 && setting.stereo_vision_restricted == true,
          "setting reads every restriction");
    const std::vector<std::pair<std::uint64_t, bool>> list{{0x0100B3C014BDA000ULL, false},
                                                           {0x01007D8018FE2000ULL, true}};
    Check(parsed && setting.free_communication_applications == list,
          "setting: the free-communication list in document order");
    Check(parsed && setting.exempt_applications == std::set<std::uint64_t>{0x0100000000010000ULL},
          "setting: only safeLaunch ALLOW is exempt");
    bool zero = true;
    for (const auto b : setting.play_timer) {
        zero = zero && b == 0;
    }
    Check(parsed && zero, "setting: no playTimerRegulations, no play timer");

    const char* refused[] = {
        R"({"unlockCode":""})",
        R"({"unlockCode":"123"})",
        R"({"unlockCode":"12a4"})",
        R"({"functionalRestrictionLevel":"STRICT"})",
        R"({"customSettings":{"ageRestriction":{"ratingAge":256}}})",
        R"({"customSettings":{"ugcRestriction":{"applications":{"xyz":{"restriction":"RESTRICTED"}}}}})",
        R"({"playTimerRegulations":{"dailyRegulations":{"timeToPlayInOneDay":{"enabled":true,"limitTime":361}}}})",
        R"({"playTimerRegulations":{"eachDayOfTheWeekRegulations":{"someday":{}}}})",
    };
    bool all = true;
    for (const char* r : refused) {
        all = all && pctl::ParseSetting(r, setting) == moon::Refused;
    }
    Check(all, "setting refuses bad PINs, levels, ages, titles, limits and days with 0234");
    Check(pctl::ParseSetting("{", setting) == moon::Malformed, "setting: cut JSON is 0232");

    const char* timer_body = R"({"playTimerRegulations":{"timerMode":"EACH_DAY_OF_THE_WEEK","restrictionMode":"FORCED_TERMINATION",
          "dailyRegulations":{"timeToPlayInOneDay":{"enabled":true,"limitTime":90}},
          "eachDayOfTheWeekRegulations":{"wednesday":{"timeToPlayInOneDay":{"enabled":true,"limitTime":120},
            "bedtime":{"enabled":true,"endingTime":{"hour":21,"minute":30},"startingTime":{"hour":7,"minute":5}}}}}})";
    const bool timer_ok = pctl::ParseSetting(timer_body, setting) == moon::Success;
    const auto& t = setting.play_timer;
    Check(timer_ok && t[0] == 1 && t[1] == 1 && t[2] == 1, "play timer: on, forced termination, per day");
    Check(timer_ok && t[4 + 5] == 1 && (t[4 + 6] | (t[4 + 7] << 8)) == 90,
          "play timer: the daily regulation at +4");
    const int wednesday = 0xc + 8 * 3;
    const std::uint8_t expected[8] = {1, 21, 30, 7, 5, 1, 120, 0};
    bool day = timer_ok;
    for (int i = 0; i < 8; ++i) {
        day = day && t[wednesday + i] == expected[i];
    }
    Check(day, "play timer: Wednesday's regulation at +0x24");
    Check(rules::TodaysLimit(t, 3) == 120 && !rules::TodaysLimit(t, 1).has_value(),
          "play timer: today's limit by day of the week");
}

void RulesCheck() {
    Check(rules::Preset(0, 2) == rules::Restriction{0, false, false} &&
              rules::Preset(2, 2) == rules::Restriction{7, true, true} &&
              rules::Preset(3, 1) == rules::Restriction{12, true, true} &&
              rules::Preset(4, 2) == rules::Restriction{16, false, false} &&
              rules::Preset(4, 0) == rules::Restriction{16, false, true},
          "safety levels are pctl's presets (Japan restricts free communication at 4)");
    const rules::Restriction custom{3, false, true};
    Check(rules::Current(1, custom, 2) == custom && rules::Current(2, custom, 2) == rules::Preset(2, 2),
          "CUSTOM takes the custom settings, other levels the preset");

    const std::tuple<std::uint32_t, const char*, int> organisations[] = {
        {0, "ja", 0},       {1, "en-US", 3},  {1, "pt-BR", 4}, {2, "en-GB", 8}, {2, "de", 5},
        {2, "fr", 6},       {3, "en-GB", 10}, {4, "zh-Hant", 2}, {4, "ko", 1},  {9, "en-GB", 0},
    };
    bool all = true;
    for (const auto& [region, language, organisation] : organisations) {
        all = all && rules::DefaultOrganization(region, language) == organisation;
    }
    Check(all, "the default organisation follows region and language");

    using R = std::vector<std::int8_t>;
    const std::tuple<int, int, R, bool> gates[] = {
        {0, 6, R{-1, -1, -1, -1, -1, -1, 18}, true},
        {12, 6, R{-1, -1, -1, -1, -1, -1, 12}, true},
        {12, 6, R{-1, -1, -1, -1, -1, -1, 16}, false},
        {12, 6, R{18, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 7}, true},
        {12, 6, R{18, -1, -1, 10, -1, -1, -1}, false},
        {12, 5, R{3, -1, -1, 3, -1, -1, 3}, false},
        {12, 6, R{-1, -1, -1}, false},
    };
    all = true;
    for (const auto& [allowed, organisation, ratings, permitted] : gates) {
        all = all && rules::RatingPermits(static_cast<std::uint8_t>(allowed), organisation, ratings) ==
                         permitted;
    }
    Check(all, "the rating gate is pctl's (IARC fallback, highest rating, USK refusal)");
}

void Errors() {
    const std::pair<const char*, int> identities[] = {
        {"503/under_maintenance", 2001}, {"500/internal_server_error", 2003},
        {"403/forbidden", 2008},         {"404/resource_is_not_found", 2009},
        {"400/devices_maxed", 2017},     {"418/teapot", 139},
    };
    bool all = true;
    for (const auto& [identity, description] : identities) {
        const std::string body = std::string(R"({"type":"https://moon.nintendo.com/errors/v1/)") +
                                 identity + R"(","title":"x","errorCode":"x","status":400,"detail":"x"})";
        all = all && moon::MapError(body) == description;
    }
    Check(all, "errors are classified by their type");
    Check(moon::MapError("<html>bad gateway</html>") == 139 && moon::MapError("") == 139,
          "a body that is no moon error is an unknown identity (0139)");
}

void Misc() {
    const std::string claims = Base64Url(R"({"sub":"cert-00","nintendo":{"sn":"XKJ01234567890"}})");
    Check(moon::SerialFromToken("e30." + claims + ".sig") == "XKJ01234567890" &&
              !moon::SerialFromToken("not-a-token").has_value(),
          "the serial is the one dauth gave");
    Check(moon::LanguageName(0x0000004247'2D6E65ULL) == "en-GB" &&
              moon::LanguageName(0x736E61482D687AULL) == "zh-Hans",
          "the language code reads as text");
    Check(moon::RegionName(0) == "JAPAN" &&
              moon::RegionName(4) == "HONGKONG_TAIWAN_KOREA_SOUTHEASTASIA" &&
              moon::RegionName(5) == "UNDEFINED",
          "regions are pctl's names");
    Check(moon::SearchPath("123456") == "/moon/v1/devices?filter.links.pairingCode.code.$eq=123456" &&
              moon::DevicePath(0xb9932f2e93d5ULL, "/activate") ==
                  "/moon/v1/devices/0000b9932f2e93d5/activate",
          "request paths");
    const auto headers = moon::RequestHeaders("tok", "\"e\"");
    const decltype(headers) expected{
        {"User-Agent", "HAC-pctl/2.0/P00 (SDK 23.3.0.0; Add-on 23.3.0.0)"},
        {"X-Nintendo-SDK-Version", "23.3.0.0"},
        {"Accept", "application/json"},
        {"Authorization", "Bearer tok"},
        {"If-None-Match", "\"e\""},
    };
    Check(headers == expected, "requests carry the console's headers");
}

void Mask() {
    pctl::Setting before;
    pctl::Setting after;
    Check(pctl::ChangedMask(nullptr, "", false, nullptr, "", false) == 0, "mask: nothing changed is 0");
    after.play_timer[0] = 1;
    after.safety_level = 2;
    Check(pctl::ChangedMask(&before, "", false, &after, "1234", true) ==
              (pctl::PlayTimerBit | pctl::SafetyLevelBit | pctl::PinBit | pctl::StereoVisionBit),
          "mask: play timer, level, PIN and stereo vision bits");
    after = {};
    after.exempt_applications.insert(1);
    after.free_communication_applications.emplace_back(2, true);
    Check(pctl::ChangedMask(&before, "", false, &after, "", false) ==
              (pctl::ExemptionBit | pctl::FreeCommunicationBit),
          "mask: exemption and free-communication bits");
}

void Slots() {
    pctl::AsyncSlots slots;
    Check(!slots.Find({7, 0}).has_value() && !slots.Find({7, 2}).has_value() &&
              !slots.Find({}).has_value(),
          "async: a Finish naming no slot is refused (0221)");
    const auto first = slots.Begin();
    const auto second = slots.Begin();
    Check(first.index == 0 && second.index == 1 && first.id != 0 && second.id != 0 &&
              first.id != second.id,
          "async: two slots, round robin, fresh non-zero ids");
    Check(slots.Find(first) == 0 && slots.Find(second) == 1, "async: live slots are found");
    Check(!slots.Find({first.id, 1}).has_value(), "async: an id in the wrong slot is refused");
    slots.Release(0);
    Check(!slots.Find(first).has_value(), "async: a released slot names nothing");
    const auto third = slots.Begin();
    Check(third.index == 0 && !slots.Find(first).has_value() && slots.Find(third) == 0,
          "async: the next work reuses slot 0 with a new id");
    const auto fourth = slots.Begin();
    Check(!slots.Find(second).has_value() && slots.Find(fourth) == 1,
          "async: starting over a running slot drops its old id");
}

} // namespace

// The console's own setting: what is kept and read back after a restart, and the free-communication
// list the setters edit.
void LocalState() {
    pctl::Setting setting;
    setting.safety_level = 1;
    setting.age_restricted = true;
    setting.custom_rating_age = 12;
    setting.custom_sns_post_restricted = true;
    setting.rating_organization = 3;
    setting.stereo_vision_restricted = true;
    setting.free_communication_applications = {{0x0100000000010000ull, false},
                                               {0x0100abcd00000000ull, true}};
    setting.exempt_applications = {0x0100000000002000ull};
    setting.play_timer[0] = 1;
    setting.play_timer[5] = 0x2a;
    pctl::Setting back;
    Check(pctl::LoadState(pctl::SerializeState(setting), back) && back == setting,
          "a setting is read back from its saved state");
    pctl::Setting untouched;
    untouched.safety_level = 4;
    Check(!pctl::LoadState("{}", untouched) && untouched.safety_level == 4,
          "a state with keys missing is refused and changes nothing");
    Check(!pctl::LoadState("not json", untouched), "a state that is not JSON is refused");
    std::string bad = pctl::SerializeState(setting);
    bad.replace(bad.find("\"safetyLevel\":1"), 15, "\"safetyLevel\":9");
    Check(!pctl::LoadState(bad, untouched), "a safety level past 4 is refused");

    std::vector<std::pair<std::uint64_t, bool>> list;
    Check(pctl::UpsertFreeCommunication(list, 7, false) && list.size() == 1, "a title is listed");
    Check(pctl::UpsertFreeCommunication(list, 7, true) && list.size() == 1 && list[0].second,
          "a title listed twice changes its flag");
    for (std::uint64_t id = 100; list.size() < pctl::FreeCommunicationCapacity; ++id) {
        pctl::UpsertFreeCommunication(list, id, false);
    }
    Check(!pctl::UpsertFreeCommunication(list, 1, false) &&
              pctl::UpsertFreeCommunication(list, 7, false),
          "a full list refuses a new title but still changes a listed one");
}

// 1459/1460 (research/g3pctl-mii/notes.md section 5): what qlaunch's play-timer pill reads.
void DisplayInfoCheck() {
    pctl::PlayTimerBlock timer{};
    rules::PlayTimerNow now{.day_of_week = 3, .hour = 14, .minute = 0, .second = 0,
                            .posix = 1'000'000, .played_ns = 0};
    Check(rules::DisplayInfo(timer, false, false, true, now, false).state == 7,
          "display: no PIN hides the pill (7)");
    Check(rules::DisplayInfo(timer, true, false, true, now, false).state == 6,
          "display: PIN, no play timer (6)");
    Check(rules::DisplayInfo(timer, true, true, false, now, false).state == 5,
          "display: temporarily unlocked (5) unless ignored");
    timer[0] = 1;            // enabled, DAILY
    timer[4 + 5] = 1;        // time to play enabled
    timer[4 + 6] = 60;       // 60 minutes
    auto shown = rules::DisplayInfo(timer, true, false, true, now, false);
    Check(shown.state == 2 && shown.remaining_ns == 3'600'000'000'000 && shown.bedtime == 0,
          "display: 60 min left, nothing played (2, 3.6e12 ns)");
    now.played_ns = 3'600'000'000'000;
    Check(rules::DisplayInfo(timer, true, false, true, now, false).state == 3,
          "display: limit used up (3)");
    Check(rules::DisplayInfo(timer, true, false, true, now, true).state == 0,
          "display: 1459 reads a used-up day as 0");
    now.played_ns = 0;
    timer[4] = 1;            // bedtime 14:30 to 07:00
    timer[4 + 1] = 7;
    timer[4 + 3] = 14;
    timer[4 + 4] = 30;
    shown = rules::DisplayInfo(timer, true, false, true, now, false);
    Check(shown.state == 1 && shown.bedtime == now.posix + 1800 &&
              shown.remaining_ns == 1'800'000'000'000,
          "display: bedtime before the limit's end (1)");
    now.hour = 15;
    Check(rules::DisplayInfo(timer, true, false, true, now, false).state == 3,
          "display: inside bedtime (3)");
}

int main() {
    Structs();
    Search();
    Owners();
    Activate();
    Settings();
    RulesCheck();
    Errors();
    Misc();
    Mask();
    Slots();
    LocalState();
    DisplayInfoCheck();
    if (failures != 0) {
        std::printf("pctl_check: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("pctl_check: all checks passed\n");
    return 0;
}
