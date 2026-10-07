// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Harbor's logging rule: never log account ids, uids, tokens, names, nicknames, PINs, pairing
// codes or typed text. These keep a log line useful without the value:
//
//   LOG_INFO(Service_ACC, "opened user {}", Common::Redact(uid));          // "<redacted>"
//   LOG_INFO(Service_ACC, "nickname {}", Common::RedactLength(nickname));  // "<redacted, 7 bytes>"
//   LOG_INFO(Service_AM, "web page {}", Common::RedactUrl(url));           // "https://host/..."
//
// and an unknown contract (a command whose layout or meaning neither firmware RE nor the Ryujinx
// Harbor established) is logged the one greppable way:
//
//   LOG_UNKNOWN_CONTRACT(Service_NS, "ns:am2 4042 answered success with no data");

#pragma once

#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

#include "common/logging.h"

namespace Common {

/// A value that formats as "<redacted>", whatever it is.
template <typename T>
struct Redacted {
    const T& value;
};

template <typename T>
Redacted<T> Redact(const T& value) {
    return Redacted<T>{value};
}

/// A string that formats as its size only, which is often what a line needs (empty or not).
struct RedactedLength {
    std::size_t size;
};

inline RedactedLength RedactLength(std::string_view text) {
    return RedactedLength{text.size()};
}

/// A URL with everything after the host dropped: the path, query and fragment carry OAuth codes,
/// states, tokens and user ids. "https://accounts.nintendo.com/..." (or "<redacted url>" when it
/// has no scheme).
inline std::string RedactUrl(std::string_view url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) {
        return url.empty() ? std::string{} : std::string{"<redacted url>"};
    }
    const std::size_t host_begin = scheme + 3;
    std::size_t host_end = url.find_first_of("/?#", host_begin);
    // user:password@host keeps only the host.
    std::string_view authority = url.substr(host_begin, host_end == std::string_view::npos
                                                            ? std::string_view::npos
                                                            : host_end - host_begin);
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        authority = authority.substr(at + 1);
    }
    std::string out{url.substr(0, host_begin)};
    out += authority;
    if (host_end != std::string_view::npos) {
        out += "/...";
    }
    return out;
}

/// A file path with every 32-hex-digit run (a uid: save directories, avatar files) replaced by
/// "<uid>". Title and save ids (16 digits) stay, so the line still says what was opened.
inline std::string RedactPath(std::string_view path) {
    const auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    std::string out;
    out.reserve(path.size());
    std::size_t i = 0;
    while (i < path.size()) {
        if (!is_hex(path[i])) {
            out += path[i++];
            continue;
        }
        std::size_t run = i;
        while (run < path.size() && is_hex(path[run])) {
            ++run;
        }
        if (run - i == 32) {
            out += "<uid>";
        } else {
            out.append(path.substr(i, run - i));
        }
        i = run;
    }
    return out;
}

} // namespace Common

template <typename T>
struct fmt::formatter<Common::Redacted<T>> {
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(const Common::Redacted<T>&, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "<redacted>");
    }
};

template <>
struct fmt::formatter<Common::RedactedLength> {
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(const Common::RedactedLength& value, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "<redacted, {} bytes>", value.size);
    }
};

/// A warning that starts "unknown contract: ", so every guessed answer can be listed with one
/// grep of the log. The format must be a string literal.
#define LOG_UNKNOWN_CONTRACT(log_class, format, ...)                                               \
    LOG_WARNING(log_class, "unknown contract: " format __VA_OPT__(, ) __VA_ARGS__)
