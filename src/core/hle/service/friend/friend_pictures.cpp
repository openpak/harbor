// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <map>
#include <mutex>
#include <set>
#include <string_view>

#include "common/logging.h"
#include "common/stb.h"
#include "core/hle/service/friend/friend_pictures.h"
#include "core/openpak/baas.h"
#include "core/openpak/session.h"

namespace Service::Friend::Pictures {

namespace {

std::mutex cache_mutex;
std::map<std::string, std::vector<std::uint8_t>> cache;
std::set<std::string> in_flight;

/// Whether `image` is a JPEG whose frames are all baseline (SOF0) or extended sequential
/// (SOF1): the kind the console's decoder takes. Scans the markers up to the first scan.
bool IsSequentialJpeg(const std::vector<std::uint8_t>& image) {
    if (image.size() < 4 || image[0] != 0xFF || image[1] != 0xD8) {
        return false;
    }
    std::size_t at = 2;
    while (at + 4 <= image.size()) {
        if (image[at] != 0xFF) {
            return false;
        }
        const std::uint8_t marker = image[at + 1];
        if (marker == 0xFF) {
            ++at;
            continue;
        }
        if (marker == 0xDA) {
            return true; // start of scan: every frame header seen was sequential
        }
        if (marker >= 0xC2 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
            marker != 0xCC) {
            return false; // progressive, lossless or arithmetic-coded
        }
        const std::size_t length = (std::size_t{image[at + 2]} << 8) | image[at + 3];
        if (length < 2) {
            return false;
        }
        at += 2 + length;
    }
    return false;
}

void WriteToVector(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/// "https://host/path?query" as host and path; nothing for anything else.
bool SplitUrl(std::string_view url, std::string& host, std::string& path) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) {
        return false;
    }
    url.remove_prefix(scheme.size());
    const auto slash = url.find('/');
    host = std::string{url.substr(0, slash)};
    path = slash == std::string_view::npos ? "/" : std::string{url.substr(slash)};
    return !host.empty() && host.find(':') == std::string::npos;
}

} // namespace

std::string UrlOf(std::uint64_t id) {
    if (const auto found = OpenPak::Baas::FindFriend(id)) {
        return found->thumbnail_url;
    }
    if (const auto user = OpenPak::Baas::CachedUser(id)) {
        return user->thumbnail_url;
    }
    return {};
}

std::optional<std::vector<std::uint8_t>> Cached(const std::string& url) {
    std::scoped_lock lock{cache_mutex};
    const auto it = cache.find(url);
    if (it == cache.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::vector<std::uint8_t>> NormalizeJpeg(const std::vector<std::uint8_t>& image) {
    if (image.size() <= MaxSize && IsSequentialJpeg(image)) {
        return image;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(image.data(), static_cast<int>(image.size()), &width,
                                            &height, &channels, STBI_rgb);
    if (pixels == nullptr) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> out;
    for (const int quality : {90, 75, 60}) {
        out.clear();
        if (!stbi_write_jpg_to_func(WriteToVector, &out, width, height, STBI_rgb, pixels,
                                    quality)) {
            out.clear();
            break;
        }
        if (out.size() <= MaxSize) {
            break;
        }
    }
    stbi_image_free(pixels);

    if (out.empty() || out.size() > MaxSize) {
        return std::nullopt;
    }
    return out;
}

bool Fetch(const std::string& url) {
    if (url.empty()) {
        return false;
    }
    {
        std::scoped_lock lock{cache_mutex};
        if (cache.contains(url) || !in_flight.insert(url).second) {
            return cache.contains(url);
        }
    }

    std::optional<std::vector<std::uint8_t>> picture;
    std::string host;
    std::string path;
    if (SplitUrl(url, host, path)) {
        const auto reply = OpenPak::Session::Send("GET", host, path, {}, "", "");
        if (reply.status == 200) {
            picture = NormalizeJpeg({reply.body.begin(), reply.body.end()});
            if (!picture) {
                LOG_DEBUG(Service_Friend, "friends: a profile picture could not be decoded");
            }
        } else {
            // The URL names the account: only the status goes to the log.
            LOG_DEBUG(Service_Friend, "friends: a profile picture fetch returned {}",
                      reply.status);
        }
    }

    std::scoped_lock lock{cache_mutex};
    in_flight.erase(url);
    if (!picture) {
        return false;
    }
    cache[url] = std::move(*picture);
    return true;
}

void FetchInBackground(const std::string& url) {
    if (url.empty()) {
        return;
    }
    {
        std::scoped_lock lock{cache_mutex};
        if (cache.contains(url) || in_flight.contains(url)) {
            return;
        }
    }
    OpenPak::Baas::RunInBackground([url] { Fetch(url); });
}

} // namespace Service::Friend::Pictures
