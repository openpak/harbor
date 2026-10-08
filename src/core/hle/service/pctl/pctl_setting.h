// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The parent's parental_control_setting document, read the way pctl 22.5.0 reads it (parse table
// 0x13c8c0 and the handlers behind each key, .text 0x60310..0x60cd0), pctl's restriction rules
// over it (.text 0x40a30..0x41c90, 0x36260, 0x36560), and the changed-settings mask of its 4000
// overlay notification (0x425c0, play timer 0x52ef0). Pure functions; check/pctl_check.cpp builds
// them alone. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/hle/service/pctl/moon.h"

namespace Service::PCTL {

/// nn::pctl::PlayTimerSettings as GetPlayTimerSettings answers it (0x44 bytes since 21.0.0): +0
/// enabled, +1 restriction mode (ALARM 0, FORCED_TERMINATION 1), +2 timer mode (DAILY 0,
/// EACH_DAY_OF_THE_WEEK 1), +4 the daily regulation, +0xc one regulation per day from Sunday. A
/// regulation is 8 bytes: bedtime enabled, ending hour and minute, starting hour and minute,
/// time-to-play enabled, then a u16 limit in minutes.
inline constexpr std::size_t PlayTimerSize = 0x44;
using PlayTimerBlock = std::array<std::uint8_t, PlayTimerSize>;

struct Setting {
    std::string etag;
    std::int64_t updated_at = 0;
    /// The PIN, or nothing when the document carries none (the console then keeps its own).
    std::optional<std::string> unlock_code;

    std::uint8_t safety_level = 0; ///< NONE 0, CUSTOM 1, CHILDREN 2, YOUNG_TEENS 3, OLDER_TEENS 4
    bool age_restricted = false;
    std::uint8_t custom_rating_age = 0;
    bool custom_sns_post_restricted = false;
    bool custom_free_communication_restricted = false;
    /// nn::ns order CERO 0 .. GMEDIA 13, or -1 when the document names none.
    int rating_organization = -1;
    /// vrRestriction: nothing when absent or UNDEFINED.
    std::optional<bool> stereo_vision_restricted;

    std::vector<std::pair<std::uint64_t, bool>> free_communication_applications;
    std::set<std::uint64_t> exempt_applications;
    PlayTimerBlock play_timer{};

    bool operator==(const Setting&) const = default;
};

/// Read a setting document. A key with the wrong JSON type, an enum value outside its list, or a
/// value its handler refuses (a PIN that is not 4-8 digits, an hour past 23, a limit past 360
/// minutes) is 2142-0234, as on the console; malformed JSON is 2142-0232.
Moon::Description ParseSetting(std::string_view body, Setting& setting);

std::string_view SafetyLevelName(std::uint8_t level);

/// The setting in force as the console keeps it (its own document, not the parent's): what the
/// parent sent and what the console's own setters changed. The PIN is kept apart, not in here.
std::string SerializeState(const Setting& setting);
/// False when the text is not a state this build wrote (the setting is then left as it was).
bool LoadState(std::string_view text, Setting& setting);

/// The free-communication list pctl keeps (a title once, with whether it is restricted), at most
/// this many titles (its table, 94 blocks of 32).
inline constexpr std::size_t FreeCommunicationCapacity = 2976;
/// Add a title, or change its flag when it is listed (.text 0x3d210). False when the list is full.
bool UpsertFreeCommunication(std::vector<std::pair<std::uint64_t, bool>>& list, std::uint64_t id,
                             bool restricted);

namespace Rules {

/// nn::pctl::RestrictionSettings: the allowed rating age, SNS posting and free communication
/// restricted.
struct Restriction {
    std::uint8_t rating_age = 0;
    bool sns_post_restricted = false;
    bool free_communication_restricted = false;
    bool operator==(const Restriction&) const = default;
};

/// A safety level's preset (0x36560, tables at 0x138e38). NONE and CUSTOM are all zero; Japan and
/// Hong Kong/Taiwan/Korea also restrict free communication at OLDER_TEENS.
Restriction Preset(int level, std::uint32_t region);

/// CUSTOM takes the custom settings (SetSafetyLevel .text 0x418d4), the rest a preset.
Restriction Current(int level, const Restriction& custom, std::uint32_t region);

/// The organisation pctl falls back on when none is set: by region and system language (0x36260).
int DefaultOrganization(std::uint32_t region, std::string_view language);

/// The rating gate of ConfirmLaunchApplicationPermission (.text 0x40b40), past the PIN, temporary
/// unlock and exemption checks: no allowed age is no gate; an unrated title is refused under USK,
/// else rated by IARC generic, else by its highest rating; no rating at all is refused.
bool RatingPermits(std::uint8_t allowed_age, int organization, std::span<const std::int8_t> ratings);

/// Today's time limit in minutes, or nothing when the day has none. `day_of_week` is 0 (Sunday)
/// to 6.
std::optional<int> TodaysLimit(const PlayTimerBlock& timer, int day_of_week);

} // namespace Rules

// The diff bits of pctl's settings diff (0x425c0, play timer 0x52ef0).
inline constexpr std::uint32_t RatingBit = 0x01;
inline constexpr std::uint32_t SnsPostBit = 0x02;
inline constexpr std::uint32_t FreeCommunicationBit = 0x04;
inline constexpr std::uint32_t PinBit = 0x08;
inline constexpr std::uint32_t PlayTimerBit = 0x10;
inline constexpr std::uint32_t SafetyLevelBit = 0x20;
inline constexpr std::uint32_t ExemptionBit = 0x40;
inline constexpr std::uint32_t StereoVisionBit = 0x80;

/// What changed between the setting in force and a new one (nothing is a console with no
/// setting, which answers the defaults). The PIN and stereo vision are the values in force.
std::uint32_t ChangedMask(const Setting* before, std::string_view pin_before, bool stereo_before,
                          const Setting* after, std::string_view pin_after, bool stereo_after);

} // namespace Service::PCTL
