// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "common/logging.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"
#include "core/core_timing.h"
#include "core/hle/service/caps/caps_manager.h"
#include "core/hle/service/caps/caps_overlay_screenshot.h"
#include "core/hle/service/psc/ovln/ovln_agent.h"
#include "core/hle/service/psc/ovln/ovln_types.h"
#include "video_core/renderer_base.h"

namespace Service::Capture {

namespace {

constexpr u32 PictureWidth = 1280;
constexpr u32 PictureHeight = 720;
constexpr std::size_t PictureSize = PictureWidth * PictureHeight * 4;

// Nothing is presented while the picture stands still on some screens; a shot that never comes
// fails.
constexpr std::chrono::seconds FrameTimeout{3};

// HOME's own program id: what the album records for a shot of the HOME screen.
constexpr u64 HomeProgramId = 0x0100000000001000;

// vi's sender (the module "vi", on the overlay's source "overlay", the only one overlayDisp
// attaches): queue 4, appended, a full queue drops the new one.
constexpr u32 SenderQueueLength = 4;
constexpr u64 SendOption = 1;

std::mutex album_mutex;
std::weak_ptr<AlbumManager> overlay_album;

std::atomic<bool> busy{false};

std::mutex sender_mutex;
std::shared_ptr<PSC::OvlnAgent::SenderState> sender;

// One request to the renderer. The renderer fills `pixels` and calls back from its own thread
// when the next frame is presented; a frame that arrives after the wait gave up is dropped.
struct Pending {
    std::vector<u8> pixels = std::vector<u8>(PictureSize);
    std::mutex mutex;
    std::condition_variable cv;
    bool done{};
    bool invert_y{};
    bool abandoned{};
};

void SendOverlay(Core::System& system, u8 result_index, std::span<const u8> file_id) {
    const auto payload = MakeOverlayScreenShotPayload(result_index, file_id);
    const auto tag = OverlayScreenShotTag(result_index);
    const PSC::OverlayNotification message = PSC::MakeNotification(tag, payload);
    const u64 tick = system.CoreTiming().GetClockTicks();

    auto& agent = PSC::GetOvlnAgent();
    PSC::OvlnStatus status = PSC::OvlnStatus::InvalidArgument;
    std::scoped_lock lk{sender_mutex};
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!sender) {
            PSC::SourceName name{};
            constexpr std::string_view source = "overlay";
            std::copy(source.begin(), source.end(), name.name.begin());
            if (agent.OpenSender(name, SenderQueueLength, sender) != PSC::OvlnStatus::Success) {
                sender.reset();
                break;
            }
        }
        status = agent.Send(*sender, SendOption, message, tick);
        if (status != PSC::OvlnStatus::InvalidArgument) {
            break;
        }
        // A new ovln session closed this sender (OvlnAgent::Reset): open another, once.
        sender.reset();
    }
    LOG_INFO(Service_Capture, "caps: overlay notification {} sent -> {}", tag,
             status == PSC::OvlnStatus::Success ? "queued" : "not queued");
}

struct Foreground {
    u64 program_id{HomeProgramId};
    bool application{};
    bool refused{};
};

// What the screen shows: the application when it is in front, else HOME.
Foreground FindForeground(Core::System& system) {
    Foreground out;
    auto* const window_system = system.GetAppletManager().GetWindowSystem();
    if (window_system == nullptr) {
        return out;
    }
    const auto application = window_system->GetMainApplet();
    if (application == nullptr) {
        return out;
    }

    std::scoped_lock lk{application->lock};
    if (application->is_process_running &&
        application->lifecycle_manager.GetActivityState() == AM::ActivityState::ForegroundVisible) {
        out.program_id = application->program_id;
        out.application = true;
        out.refused = application->screenshot_permission == AM::ScreenshotPermission::Disable;
    }
    return out;
}

void Run(Core::System& system, std::shared_ptr<AlbumManager> album) {
    const Foreground foreground = FindForeground(system);

    if (foreground.refused) {
        LOG_INFO(Service_Capture, "caps: the foreground application does not allow screenshots");
        std::array<u8, sizeof(AlbumFileId)> id{};
        AlbumFileId file_id{.application_id = foreground.program_id};
        std::memcpy(id.data(), &file_id, id.size());
        SendOverlay(system, OverlayResultRefused, id);
        return;
    }

    auto& renderer = system.Renderer();
    if (renderer.IsScreenshotPending()) {
        LOG_WARNING(Service_Capture, "caps: a screenshot is already in progress");
        return;
    }

    // The application's layers only (no HOME, no overlay) while it is in front; with HOME in
    // front, everything shown.
    const auto layer_stack = foreground.application ? Service::Nvnflinger::LayerStackId::Screenshot
                                                    : Service::Nvnflinger::LayerStackId::Default;

    const auto pending = std::make_shared<Pending>();
    renderer.RequestScreenshot(
        pending->pixels.data(),
        [pending](bool invert_y) {
            std::scoped_lock lk{pending->mutex};
            pending->invert_y = invert_y;
            pending->done = true;
            pending->cv.notify_all();
        },
        Layout::DefaultFrameLayout(PictureWidth, PictureHeight), layer_stack);

    std::array<u8, sizeof(AlbumFileId)> id{};
    AlbumFileId file_id{.application_id = foreground.program_id};
    std::memcpy(id.data(), &file_id, id.size());

    {
        std::unique_lock lk{pending->mutex};
        if (!pending->cv.wait_for(lk, FrameTimeout, [&] { return pending->done; })) {
            pending->abandoned = true;
            lk.unlock();
            LOG_WARNING(Service_Capture, "caps: screenshot failed: no frame was presented");
            SendOverlay(system, OverlayResultFailed, id);
            return;
        }
    }

    // The renderer gives BGRA, upside down when it says so; the album and the thumbnail are
    // upright RGBA with an opaque alpha.
    std::vector<u8> rgba(PictureSize);
    for (u32 y = 0; y < PictureHeight; y++) {
        const u32 source_row = pending->invert_y ? PictureHeight - 1 - y : y;
        const u8* src = pending->pixels.data() + std::size_t{source_row} * PictureWidth * 4;
        u8* dst = rgba.data() + std::size_t{y} * PictureWidth * 4;
        for (u32 x = 0; x < PictureWidth; x++) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 0xff;
        }
    }
    const auto thumbnail = MakeOverlayThumbnail(rgba, PictureWidth, PictureHeight);

    AlbumFileId saved{};
    const Result result =
        album->SaveOverlayScreenShot(saved, foreground.program_id, rgba, thumbnail);
    if (result.IsError()) {
        LOG_WARNING(Service_Capture, "caps: screenshot not saved ({:#x})", result.raw);
        SendOverlay(system, OverlayResultFailed, id);
        return;
    }

    std::memcpy(id.data(), &saved, id.size());
    LOG_INFO(Service_Capture, "caps: screenshot of {:016x} saved to the album",
             foreground.program_id);

    // The pre-notification (what the overlay shows, "OvlPhotoOk"), then the final result (which
    // the overlay drops for a capture-button shot).
    SendOverlay(system, OverlayResultTaken, id);
    SendOverlay(system, OverlayResultSaved, id);
}

} // namespace

void SetOverlayScreenShotAlbum(std::weak_ptr<AlbumManager> album) {
    std::scoped_lock lk{album_mutex};
    overlay_album = std::move(album);
}

void TakeOverlayScreenShot(Core::System& system) {
    if (!system.GetAppletManager().IsHomeSession()) {
        return;
    }

    std::shared_ptr<AlbumManager> album;
    {
        std::scoped_lock lk{album_mutex};
        album = overlay_album.lock();
    }
    if (album == nullptr) {
        LOG_WARNING(Service_Capture, "caps: no album to save the screenshot into");
        return;
    }

    if (busy.exchange(true)) {
        LOG_INFO(Service_Capture, "caps: a screenshot is already being taken");
        return;
    }

    // The caller holds am's lock: the work waits for a frame, so it runs on its own thread.
    std::thread{[&system, album = std::move(album)]() mutable {
        Run(system, std::move(album));
        busy = false;
    }}.detach();
}

} // namespace Service::Capture
