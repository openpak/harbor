// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include "common/assert.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/scheduler.h"
#include "video_core/gpu.h"

namespace Tegra::Control {

void Scheduler::Push(GPU& gpu, s32 channel, CommandList&& entries) {
    std::shared_ptr<ChannelState> channel_state;
    {
        std::unique_lock lk(scheduling_guard);
        auto it = channels.find(channel);
        if (it == channels.end()) {
            // The channel was closed after this list was queued (its process exited): the GPU
            // drops a closed channel's pending work instead of running it.
            return;
        }
        channel_state = it->second;
        gpu.BindChannel(channel_state->bind_id);
    }
    // Process commands outside the lock to reduce contention.
    // Multiple channels can prepare their commands in parallel.
    channel_state->payload->dma_pusher.Push(std::move(entries));
    channel_state->payload->dma_pusher.DispatchCalls();
}

void Scheduler::DeclareChannel(std::shared_ptr<ChannelState> new_channel) {
    s32 channel = new_channel->bind_id;
    std::unique_lock lk(scheduling_guard);
    channels.emplace(channel, new_channel);
}

void Scheduler::RemoveChannel(s32 channel) {
    std::unique_lock lk(scheduling_guard);
    channels.erase(channel);
}

} // namespace Tegra::Control
