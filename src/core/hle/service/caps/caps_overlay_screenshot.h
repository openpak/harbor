// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
//
// The capture button's screenshot in a HOME session, as vi takes it on a console: the picture on
// the screen (the foreground application's layers, or the whole HOME screen when HOME is in
// front) is saved to the album, its 96x54 thumbnail is kept for caps:a 301, and the overlay
// applet is told over ovln (vi's sender on the source "overlay": 8004 before the save, then 8001; 8002 on failure, 8003
// when the application does not allow screenshots) so it shows its "Captured" pop-up.

#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Service::Capture {

class AlbumManager;

// The overlay thumbnail's size, caps:a 301.
constexpr u32 OverlayThumbnailWidth = 96;
constexpr u32 OverlayThumbnailHeight = 54;

/// The album the capture button saves into: the one the caps services of this session serve.
void SetOverlayScreenShotAlbum(std::weak_ptr<AlbumManager> album);

/// The capture button was pressed short and no applet handles it. Does nothing outside a HOME
/// session. Returns at once: the picture is taken on the next presented frame on a worker thread.
void TakeOverlayScreenShot(Core::System& system);

// Result indices of vi's record.
constexpr u8 OverlayResultSaved = 0;
constexpr u8 OverlayResultFailed = 1;
constexpr u8 OverlayResultRefused = 2;
constexpr u8 OverlayResultTaken = 5;

/// RGBA picture of `width` x `height` (rows top to bottom) scaled down to the overlay thumbnail
/// by averaging the source pixels each thumbnail pixel covers; the alpha is forced opaque.
/// The next three need only the standard library, so the self-check can build them alone.
inline std::vector<u8> MakeOverlayThumbnail(std::span<const u8> rgba, u32 width, u32 height) {
    std::vector<u8> out(std::size_t{OverlayThumbnailWidth} * OverlayThumbnailHeight * 4);
    if (rgba.size() < std::size_t{width} * height * 4 || width == 0 || height == 0) {
        return out;
    }

    for (u32 ty = 0; ty < OverlayThumbnailHeight; ty++) {
        const u32 y0 = ty * height / OverlayThumbnailHeight;
        const u32 y1 = std::max(y0 + 1, (ty + 1) * height / OverlayThumbnailHeight);
        for (u32 tx = 0; tx < OverlayThumbnailWidth; tx++) {
            const u32 x0 = tx * width / OverlayThumbnailWidth;
            const u32 x1 = std::max(x0 + 1, (tx + 1) * width / OverlayThumbnailWidth);
            u32 sum[3]{};
            for (u32 y = y0; y < y1; y++) {
                for (u32 x = x0; x < x1; x++) {
                    const u8* p = rgba.data() + (std::size_t{y} * width + x) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            const u32 count = (y1 - y0) * (x1 - x0);
            u8* dst = out.data() + (std::size_t{ty} * OverlayThumbnailWidth + tx) * 4;
            dst[0] = static_cast<u8>(sum[0] / count);
            dst[1] = static_cast<u8>(sum[1] / count);
            dst[2] = static_cast<u8>(sum[2] / count);
            dst[3] = 0xff;
        }
    }
    return out;
}

inline std::vector<u8> MakeOverlayScreenShotPayload(u8 result_index, std::span<const u8> file_id) {
    // vi's 0x28-byte record: u8 mode flag (0, the capture button), u8 result index, u16 0, u32 (1
    // on the pre-notification), u64 (-1 on the pre-notification), then the AlbumFileId.
    std::vector<u8> payload(0x28);
    payload[1] = result_index;
    if (result_index == OverlayResultTaken) {
        payload[4] = 1;
        std::fill(payload.begin() + 8, payload.begin() + 16, u8{0xff});
    }
    std::memcpy(payload.data() + 0x10, file_id.data(), std::min<std::size_t>(file_id.size(), 0x18));
    return payload;
}

inline u32 OverlayScreenShotTag(u8 result_index) {
    switch (result_index) {
    case OverlayResultSaved:
        return 8001;
    case OverlayResultRefused:
        return 8003;
    case OverlayResultTaken:
        return 8004;
    default:
        return 8002;
    }
}

} // namespace Service::Capture
