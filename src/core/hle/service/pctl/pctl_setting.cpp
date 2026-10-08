// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include <algorithm>
#include <array>
#include <limits>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "core/hle/service/pctl/pctl_setting.h"

namespace Service::PCTL {

namespace {

// Keys keep the document's order, as the console's parser walks them.
using json = nlohmann::ordered_json;

constexpr std::array<std::string_view, 5> Levels{"NONE", "CUSTOM", "CHILDREN", "YOUNG_TEENS",
                                                 "OLDER_TEENS"};
constexpr std::array<std::string_view, 3> Restrictions{"UNRESTRICTED", "RESTRICTED", "UNDEFINED"};
constexpr std::array<std::string_view, 14> Organizations{
    "CERO",      "GRAC_GCRB", "GSRMR",   "ESRB", "CLASS_IND",    "USK",  "PEGI",
    "PEGI_PORTUGAL", "PEGI_BBFC", "RUSSIAN", "ACB", "OFLC", "IARC_GENERIC", "GMEDIA"};
constexpr std::array<std::string_view, 2> TimerModes{"DAILY", "EACH_DAY_OF_THE_WEEK"};
constexpr std::array<std::string_view, 2> RestrictionModes{"ALARM", "FORCED_TERMINATION"};
constexpr std::array<std::string_view, 7> Days{"sunday",   "monday", "tuesday", "wednesday",
                                               "thursday", "friday", "saturday"};
constexpr std::array<std::string_view, 2> SafeLaunch{"NONE", "ALLOW"};

/// A refused document: the parse ends as 2142-0234.
struct RefusedDocument {};

void Need(bool condition) {
    if (!condition) {
        throw RefusedDocument{};
    }
}

const json* Present(const json& parent, const char* name) {
    const auto it = parent.find(name);
    return it == parent.end() || it->is_null() ? nullptr : &*it;
}

std::optional<std::string> String(const json& parent, const char* name) {
    const json* value = Present(parent, name);
    if (value == nullptr) {
        return std::nullopt;
    }
    Need(value->is_string());
    return value->get<std::string>();
}

std::optional<std::int64_t> Integer(const json& parent, const char* name, std::int64_t max) {
    const json* value = Present(parent, name);
    if (value == nullptr) {
        return std::nullopt;
    }
    Need(value->is_number_integer());
    std::int64_t number;
    if (value->is_number_unsigned()) {
        const auto u = value->get<std::uint64_t>();
        Need(u <= static_cast<std::uint64_t>(max));
        number = static_cast<std::int64_t>(u);
    } else {
        number = value->get<std::int64_t>();
    }
    Need(number >= 0 && number <= max);
    return number;
}

bool Boolean(const json& parent, const char* name) {
    const json* value = Present(parent, name);
    if (value == nullptr) {
        return false;
    }
    Need(value->is_boolean());
    return value->get<bool>();
}

template <std::size_t N>
std::optional<int> Enum(const json& parent, const char* name,
                        const std::array<std::string_view, N>& values) {
    const auto text = String(parent, name);
    if (!text) {
        return std::nullopt;
    }
    const auto it = std::find(values.begin(), values.end(), *text);
    Need(it != values.end());
    return static_cast<int>(it - values.begin());
}

const json* Object(const json& parent, const char* name) {
    const json* value = Present(parent, name);
    if (value != nullptr) {
        Need(value->is_object());
    }
    return value;
}

std::uint64_t HexId(std::string_view text) {
    Need(!text.empty() && text.size() <= 16);
    std::uint64_t out = 0;
    for (const char c : text) {
        int digit = -1;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        }
        Need(digit >= 0);
        out = (out << 4) | static_cast<std::uint64_t>(digit);
    }
    return out;
}

void ReadRegulation(const json& regulation, std::uint8_t* output) {
    if (const json* play = Object(regulation, "timeToPlayInOneDay")) {
        output[5] = Boolean(*play, "enabled") ? 1 : 0;
        const auto limit = static_cast<std::uint16_t>(Integer(*play, "limitTime", 360).value_or(0));
        output[6] = static_cast<std::uint8_t>(limit & 0xFF);
        output[7] = static_cast<std::uint8_t>(limit >> 8);
    }
    if (const json* bedtime = Object(regulation, "bedtime")) {
        output[0] = Boolean(*bedtime, "enabled") ? 1 : 0;
        if (const json* ending = Object(*bedtime, "endingTime")) {
            output[1] = static_cast<std::uint8_t>(Integer(*ending, "hour", 23).value_or(0));
            output[2] = static_cast<std::uint8_t>(Integer(*ending, "minute", 59).value_or(0));
        }
        if (const json* starting = Object(*bedtime, "startingTime")) {
            output[3] = static_cast<std::uint8_t>(Integer(*starting, "hour", 23).value_or(0));
            output[4] = static_cast<std::uint8_t>(Integer(*starting, "minute", 59).value_or(0));
        }
    }
}

void ReadPlayTimer(const json& timer, PlayTimerBlock& output) {
    const int timer_mode = Enum(timer, "timerMode", TimerModes).value_or(0);
    output[1] = static_cast<std::uint8_t>(Enum(timer, "restrictionMode", RestrictionModes).value_or(0));
    output[2] = static_cast<std::uint8_t>(timer_mode);

    if (const json* daily = Object(timer, "dailyRegulations")) {
        ReadRegulation(*daily, output.data() + 4);
    }
    if (const json* week = Object(timer, "eachDayOfTheWeekRegulations")) {
        for (const auto& [name, day] : week->items()) {
            const auto it = std::find(Days.begin(), Days.end(), name);
            Need(it != Days.end() && day.is_object());
            ReadRegulation(day, output.data() + 0xc + 8 * (it - Days.begin()));
        }
    }

    // Unknown contract (as in the Ryujinx Harbor): no parse handler writes +0, the byte qlaunch
    // reads as "play timer on". Taken as on when the regulation in force limits the time or sets
    // a bedtime.
    bool enabled = false;
    if (timer_mode == 0) {
        enabled = output[4] != 0 || output[9] != 0;
    } else {
        for (int day = 0; day < 7; ++day) {
            enabled = enabled || output[0xc + 8 * day] != 0 || output[0xc + 8 * day + 5] != 0;
        }
    }
    output[0] = enabled ? 1 : 0;
}

Setting Read(const json& root) {
    Need(root.is_object());

    Setting setting;
    setting.etag = String(root, "etag").value_or("");
    setting.updated_at = Integer(root, "updatedAt", std::numeric_limits<std::int64_t>::max()).value_or(0);
    setting.unlock_code = String(root, "unlockCode");

    Need(setting.etag.size() < 96);
    if (setting.unlock_code) {
        const auto& pin = *setting.unlock_code;
        Need(pin.size() >= 4 && pin.size() <= 8 &&
             std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; }));
    }

    setting.safety_level =
        static_cast<std::uint8_t>(Enum(root, "functionalRestrictionLevel", Levels).value_or(0));

    if (const json* custom = Object(root, "customSettings")) {
        if (const json* age = Object(*custom, "ageRestriction")) {
            setting.age_restricted = Enum(*age, "restriction", Restrictions).value_or(0) != 0;
            setting.custom_rating_age =
                static_cast<std::uint8_t>(Integer(*age, "ratingAge", 255).value_or(0));
        }
        if (const json* ugc = Object(*custom, "ugcRestriction")) {
            setting.custom_free_communication_restricted =
                Enum(*ugc, "restriction", Restrictions).value_or(0) != 0;
            if (const json* applications = Object(*ugc, "applications")) {
                for (const auto& [name, application] : applications->items()) {
                    Need(application.is_object());
                    setting.free_communication_applications.emplace_back(
                        HexId(name), Enum(application, "restriction", Restrictions).value_or(0) != 0);
                }
            }
        }
        setting.custom_sns_post_restricted =
            Enum(*custom, "snsPostRestriction", Restrictions).value_or(0) != 0;
        setting.rating_organization = Enum(*custom, "ratingOrganization", Organizations).value_or(-1);

        const auto vr = Enum(*custom, "vrRestriction", Restrictions);
        if (vr == 0) {
            setting.stereo_vision_restricted = false;
        } else if (vr == 1) {
            setting.stereo_vision_restricted = true;
        }
        const auto vr_etag = String(*custom, "vrRestrictionEtag");
        Need(!vr_etag || vr_etag->size() < 96);
    }

    if (const json* timer = Object(root, "playTimerRegulations")) {
        ReadPlayTimer(*timer, setting.play_timer);
    }

    if (const json* whitelisted = Object(root, "whitelistedApplications")) {
        for (const auto& [name, application] : whitelisted->items()) {
            Need(application.is_object());
            if (Enum(application, "safeLaunch", SafeLaunch) == 1) {
                setting.exempt_applications.insert(HexId(name));
            }
        }
    }
    return setting;
}

} // namespace

Moon::Description ParseSetting(std::string_view body, Setting& setting) {
    const json document = json::parse(body, nullptr, false);
    if (document.is_discarded()) {
        return Moon::Malformed;
    }
    try {
        setting = Read(document);
        return Moon::Success;
    } catch (const RefusedDocument&) {
        setting = {};
        return Moon::Refused;
    }
}

std::string_view SafetyLevelName(std::uint8_t level) {
    return level < Levels.size() ? Levels[level] : "NONE";
}

namespace Rules {

Restriction Preset(int level, std::uint32_t region) {
    switch (level) {
    case 2:
        return {7, true, true};
    case 3:
        return {12, true, true};
    case 4:
        return {16, false, region == 0 || region == 4};
    default:
        return {};
    }
}

Restriction Current(int level, const Restriction& custom, std::uint32_t region) {
    return level == 1 ? custom : Preset(level, region);
}

int DefaultOrganization(std::uint32_t region, std::string_view language) {
    // By region row (region + 1, else 0) and language column (0x36260, table at 0xf9d8a).
    static constexpr std::array<std::array<std::uint8_t, 14>, 6> table{{
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {3, 3, 3, 3, 3, 3, 3, 3, 4, 3, 3, 3, 3, 4},
        {6, 8, 6, 6, 6, 6, 6, 5, 6, 9, 6, 6, 6, 6},
        {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1},
    }};
    int column = 0;
    if (language == "en-US" || language == "en-GB") {
        column = 1;
    } else if (language == "fr" || language == "fr-CA") {
        column = 2;
    } else if (language == "es" || language == "es-419") {
        column = 3;
    } else if (language == "it") {
        column = 4;
    } else if (language == "nl") {
        column = 5;
    } else if (language == "ja") {
        column = 6;
    } else if (language == "de") {
        column = 7;
    } else if (language == "pt") {
        column = 8;
    } else if (language == "ru") {
        column = 9;
    } else if (language == "zh-Hant") {
        column = 10;
    } else if (language == "zh-Hans") {
        column = 11;
    } else if (language == "ko") {
        column = 12;
    } else if (language == "pt-BR") {
        column = 13;
    }
    return table[region < 5 ? region + 1 : 0][column];
}

bool RatingPermits(std::uint8_t allowed_age, int organization,
                   std::span<const std::int8_t> ratings) {
    if (allowed_age == 0) {
        return true;
    }
    int rating = organization >= 0 && static_cast<std::size_t>(organization) < ratings.size()
                     ? ratings[organization]
                     : -1;
    if (rating < 0) {
        if (organization == 5) {
            return false;
        }
        rating = ratings.size() >= 13 ? ratings[12] : -1;
        if (rating < 0) {
            for (const auto value : ratings) {
                rating = std::max<int>(rating, value);
            }
        }
        if (rating < 0) {
            return false;
        }
    }
    return allowed_age >= rating;
}

std::optional<int> TodaysLimit(const PlayTimerBlock& timer, int day_of_week) {
    if (timer[0] == 0) {
        return std::nullopt;
    }
    const std::size_t regulation = timer[2] == 0 ? 4 : 0xc + 8 * static_cast<std::size_t>(day_of_week);
    if (timer[regulation + 5] == 0) {
        return std::nullopt;
    }
    return timer[regulation + 6] | (timer[regulation + 7] << 8);
}

} // namespace Rules

std::string SerializeState(const Setting& setting) {
    json out;
    out["safetyLevel"] = setting.safety_level;
    out["ageRestricted"] = setting.age_restricted;
    out["customRatingAge"] = setting.custom_rating_age;
    out["customSnsPostRestricted"] = setting.custom_sns_post_restricted;
    out["customFreeCommunicationRestricted"] = setting.custom_free_communication_restricted;
    out["ratingOrganization"] = setting.rating_organization;
    if (setting.stereo_vision_restricted) {
        out["stereoVisionRestricted"] = *setting.stereo_vision_restricted;
    }
    auto free_list = json::array();
    for (const auto& [id, restricted] : setting.free_communication_applications) {
        free_list.push_back({fmt::format("{:016x}", id), restricted});
    }
    out["freeCommunication"] = std::move(free_list);
    auto exempt = json::array();
    for (const auto id : setting.exempt_applications) {
        exempt.push_back(fmt::format("{:016x}", id));
    }
    out["exempt"] = std::move(exempt);
    std::string timer;
    for (const auto byte : setting.play_timer) {
        timer += fmt::format("{:02x}", byte);
    }
    out["playTimer"] = std::move(timer);
    return out.dump();
}

bool LoadState(std::string_view text, Setting& setting) {
    try {
        const auto in = json::parse(text);
        Setting loaded;
        loaded.safety_level = in.at("safetyLevel").get<std::uint8_t>();
        Need(loaded.safety_level < Levels.size());
        loaded.age_restricted = in.at("ageRestricted").get<bool>();
        loaded.custom_rating_age = in.at("customRatingAge").get<std::uint8_t>();
        loaded.custom_sns_post_restricted = in.at("customSnsPostRestricted").get<bool>();
        loaded.custom_free_communication_restricted =
            in.at("customFreeCommunicationRestricted").get<bool>();
        loaded.rating_organization = in.at("ratingOrganization").get<int>();
        if (in.contains("stereoVisionRestricted")) {
            loaded.stereo_vision_restricted = in.at("stereoVisionRestricted").get<bool>();
        }
        const auto parse_id = [](const json& value) {
            return static_cast<std::uint64_t>(std::stoull(value.get<std::string>(), nullptr, 16));
        };
        for (const auto& entry : in.at("freeCommunication")) {
            loaded.free_communication_applications.emplace_back(parse_id(entry.at(0)),
                                                                entry.at(1).get<bool>());
        }
        for (const auto& entry : in.at("exempt")) {
            loaded.exempt_applications.insert(parse_id(entry));
        }
        const auto timer = in.at("playTimer").get<std::string>();
        Need(timer.size() == 2 * PlayTimerSize);
        for (std::size_t i = 0; i < PlayTimerSize; ++i) {
            loaded.play_timer[i] =
                static_cast<std::uint8_t>(std::stoul(timer.substr(2 * i, 2), nullptr, 16));
        }
        setting = std::move(loaded);
        return true;
    } catch (...) {
        return false;
    }
}

bool UpsertFreeCommunication(std::vector<std::pair<std::uint64_t, bool>>& list, std::uint64_t id,
                             bool restricted) {
    for (auto& entry : list) {
        if (entry.first == id) {
            entry.second = restricted;
            return true;
        }
    }
    if (list.size() >= FreeCommunicationCapacity) {
        return false;
    }
    list.emplace_back(id, restricted);
    return true;
}

std::uint32_t ChangedMask(const Setting* before, std::string_view pin_before, bool stereo_before,
                          const Setting* after, std::string_view pin_after, bool stereo_after) {
    static const Setting none{};
    const Setting& a = before != nullptr ? *before : none;
    const Setting& b = after != nullptr ? *after : none;

    std::uint32_t mask = 0;
    if (a.custom_rating_age != b.custom_rating_age || a.rating_organization != b.rating_organization ||
        a.age_restricted != b.age_restricted) {
        mask |= RatingBit;
    }
    if (a.custom_sns_post_restricted != b.custom_sns_post_restricted) {
        mask |= SnsPostBit;
    }
    if (a.custom_free_communication_restricted != b.custom_free_communication_restricted ||
        a.free_communication_applications != b.free_communication_applications) {
        mask |= FreeCommunicationBit;
    }
    if (pin_before != pin_after) {
        mask |= PinBit;
    }
    if (a.play_timer != b.play_timer) {
        mask |= PlayTimerBit;
    }
    if (a.safety_level != b.safety_level) {
        mask |= SafetyLevelBit;
    }
    if (a.exempt_applications != b.exempt_applications) {
        mask |= ExemptionBit;
    }
    if (stereo_before != stereo_after) {
        mask |= StereoVisionBit;
    }
    return mask;
}

} // namespace Service::PCTL
