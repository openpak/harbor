// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check for the capture button's thumbnail and ovln record. Eden's unit tests are off in the
// Harbor build, so this is a standalone program:
//
//   g++ -std=c++23 -I <src> caps_overlay_screenshot_selfcheck.cpp -o caps_selfcheck && ./caps_selfcheck
//
// It prints "caps overlay self-check: OK" and exits 0, or names the failed check and exits 1.

#include <cstdio>
#include <cstdlib>

#include "core/hle/service/caps/caps_overlay_screenshot.h"

using namespace Service::Capture;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond);                         \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

void Thumbnail() {
    // A picture of two halves, left red and right blue: the thumbnail keeps the split, is
    // 96 x 54 x 4 bytes, and is opaque.
    constexpr u32 w = 1280, h = 720;
    std::vector<u8> picture(w * h * 4);
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u8* p = &picture[(y * w + x) * 4];
            p[0] = x < w / 2 ? 255 : 0;
            p[1] = 0;
            p[2] = x < w / 2 ? 0 : 255;
            p[3] = 7; // the game's own alpha is ignored
        }
    }
    const auto thumbnail = MakeOverlayThumbnail(picture, w, h);
    CHECK(thumbnail.size() == 96 * 54 * 4);
    CHECK(thumbnail[(10 * 96 + 5) * 4 + 0] == 255 && thumbnail[(10 * 96 + 5) * 4 + 2] == 0);
    CHECK(thumbnail[(10 * 96 + 90) * 4 + 0] == 0 && thumbnail[(10 * 96 + 90) * 4 + 2] == 255);
    for (std::size_t i = 3; i < thumbnail.size(); i += 4) {
        CHECK(thumbnail[i] == 0xff);
    }

    // A picture that is too short for its size yields a blank thumbnail, not a read past the end.
    const auto blank = MakeOverlayThumbnail(std::span<const u8>{picture}.first(16), w, h);
    CHECK(blank.size() == 96 * 54 * 4 && blank[0] == 0);
}

void Payload() {
    std::vector<u8> id(0x18);
    for (std::size_t i = 0; i < id.size(); i++) {
        id[i] = static_cast<u8>(0xa0 + i);
    }

    const auto taken = MakeOverlayScreenShotPayload(OverlayResultTaken, id);
    CHECK(taken.size() == 0x28);
    CHECK(taken[0] == 0 && taken[1] == 5 && taken[4] == 1);
    for (int i = 8; i < 16; i++) {
        CHECK(taken[i] == 0xff);
    }
    CHECK(std::equal(id.begin(), id.end(), taken.begin() + 0x10));

    const auto saved = MakeOverlayScreenShotPayload(OverlayResultSaved, id);
    CHECK(saved[1] == 0 && saved[4] == 0 && saved[8] == 0);

    CHECK(OverlayScreenShotTag(OverlayResultSaved) == 8001);
    CHECK(OverlayScreenShotTag(OverlayResultFailed) == 8002);
    CHECK(OverlayScreenShotTag(OverlayResultRefused) == 8003);
    CHECK(OverlayScreenShotTag(OverlayResultTaken) == 8004);
}

} // namespace

int main() {
    Thumbnail();
    Payload();
    std::puts("caps overlay self-check: OK");
    return 0;
}
