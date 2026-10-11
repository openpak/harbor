// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "video_core/renderer_vulkan/vk_query_cache.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

/// Harbor GPU breadcrumbs. The marker buffer holds two sequence numbers: [0] the last command the
/// command processor reached (written at the top of the pipe), [1] the last one that finished
/// (bottom of the pipe). The ring maps sequence numbers back to where each command was recorded.
struct Scheduler::Breadcrumbs {
    struct Entry {
        u32 seq = 0;
        std::source_location where;
        std::array<u64, 2> tag{};
    };
    static constexpr u32 RING_SIZE = 16384; // a power of two; covers the frames in flight

    vk::Buffer buffer;
    u32 next_seq = 0;
    std::array<Entry, RING_SIZE> ring{};

    u32 Begin(vk::CommandBuffer cmdbuf, const Command& command) {
        if (++next_seq == 0) {
            next_seq = 1;
        }
        ring[next_seq & (RING_SIZE - 1)] = Entry{next_seq, command.where, command.tag};
        cmdbuf.WriteBufferMarkerAMD(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *buffer, 0, next_seq);
        return next_seq;
    }

    void End(vk::CommandBuffer cmdbuf, u32 seq) const {
        cmdbuf.WriteBufferMarkerAMD(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *buffer, sizeof(u32),
                                    seq);
    }
};

bool Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf, Breadcrumbs* crumbs) {
    bool device_ok = true;
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        // The submission (always a chunk's last command) ends the command buffer: no marker after.
        const bool ends_cmdbuf = submit && next == nullptr;
        const u32 seq = crumbs ? crumbs->Begin(cmdbuf, *command) : 0;
        try {
            command->Execute(cmdbuf, upload_cmdbuf);
        } catch (const vk::Exception& exception) {
            // Harbor: a lost GPU stops the console instead of aborting the process; anything
            // else is still fatal. The remaining commands still run (and are destroyed), so the
            // chunk is reusable.
            if (exception.GetResult() != VK_ERROR_DEVICE_LOST) {
                throw;
            }
            device_ok = false;
        }
        if (crumbs && !ends_cmdbuf) {
            crumbs->End(cmdbuf, seq);
        }
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
    return device_ok;
}

void Scheduler::EnableBreadcrumbs(MemoryAllocator& allocator) {
    if (!device.IsAmdBufferMarkerSupported()) {
        LOG_WARNING(Render_Vulkan, "GPU breadcrumbs need VK_AMD_buffer_marker, which this driver "
                                   "does not have");
        return;
    }
    auto crumbs = std::make_unique<Breadcrumbs>();
    crumbs->buffer = allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = 2 * sizeof(u32),
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::Download);
    if (crumbs->buffer.Mapped().size() < 2 * sizeof(u32)) {
        LOG_WARNING(Render_Vulkan, "GPU breadcrumbs: no host-visible marker buffer");
        return;
    }
    std::memset(crumbs->buffer.Mapped().data(), 0, 2 * sizeof(u32));
    std::scoped_lock lock{execution_mutex};
    breadcrumbs = std::move(crumbs);
    LOG_INFO(Render_Vulkan, "GPU breadcrumbs on: a device loss names the command it stopped at");
}

void Scheduler::LogBreadcrumbs() {
    Breadcrumbs* const crumbs = breadcrumbs.get();
    if (!crumbs) {
        return;
    }
    crumbs->buffer.Invalidate();
    u32 values[2];
    std::memcpy(values, crumbs->buffer.Mapped().data(), sizeof(values));
    const u32 reached = values[0];
    const u32 finished = values[1];
    const u32 recorded = crumbs->next_seq;
    LOG_CRITICAL(Render_Vulkan,
                 "GPU breadcrumbs: last command reached #{}, last finished #{}, last recorded #{}",
                 reached, finished, recorded);

    const auto entry_at = [crumbs](u32 seq) -> const Breadcrumbs::Entry* {
        const auto& entry = crumbs->ring[seq & (Breadcrumbs::RING_SIZE - 1)];
        return entry.seq == seq ? &entry : nullptr;
    };
    // The command that never finished, and the shaders bound when it ran.
    const u32 stuck = finished + 1;
    for (u32 seq = stuck, steps = 0; seq != 0 && steps < Breadcrumbs::RING_SIZE; --seq, ++steps) {
        const auto* const entry = entry_at(seq);
        if (!entry) {
            break;
        }
        if (entry->tag[0] != 0 || entry->tag[1] != 0) {
            LOG_CRITICAL(Render_Vulkan,
                         "GPU breadcrumbs: shaders bound at #{} (#{} before the stuck command): "
                         "{:016x} {:016x}",
                         seq, stuck - seq, entry->tag[0], entry->tag[1]);
            break;
        }
    }
    const u32 first = finished > 8 ? finished - 8 : 1;
    const u32 last = (std::min)(recorded, (std::max)(reached, stuck) + 4);
    for (u32 seq = first; seq <= last && seq - first < 64; ++seq) {
        const auto* const entry = entry_at(seq);
        if (!entry) {
            continue;
        }
        const char* const progress =
            seq <= finished ? "done" : (seq <= reached ? "RUNNING" : "queued");
        LOG_CRITICAL(Render_Vulkan, "GPU breadcrumbs: #{} {} {}:{} {}{}", seq, progress,
                     entry->where.file_name(), entry->where.line(), entry->where.function_name(),
                     entry->tag[0] != 0 || entry->tag[1] != 0
                         ? fmt::format(" [shaders {:016x} {:016x}]", entry->tag[0], entry->tag[1])
                         : std::string{});
    }
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)} {

    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });

    // Whoever sees the loss first (a submission, the fence waiter, a present), every wait on this
    // scheduler's ticks is released and the breadcrumbs are logged once.
    device.AddLossHandler([this, alive = std::weak_ptr<int>{loss_token}] {
        if (alive.lock()) {
            master_semaphore->MarkLost();
            LogBreadcrumbs();
        }
    });
}

Scheduler::~Scheduler() = default;

bool Scheduler::SimulatedLoss() {
    // Harbor test hook: HARBOR_SIMULATE_GPU_LOSS=N makes submission N report VK_ERROR_DEVICE_LOST
    // after it was sent (so nothing waits on it forever), to exercise the stop path without a GPU
    // hang. Worker thread only.
    static const u64 lose_at = [] {
        const char* const value = std::getenv("HARBOR_SIMULATE_GPU_LOSS");
        return value ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    if (lose_at == 0 || ++submissions != lose_at) {
        return false;
    }
    LOG_WARNING(Render_Vulkan, "harbor: submission {} simulates a device loss", lose_at);
    return true;
}

void Scheduler::OnDeviceLost() {
    master_semaphore->MarkLost();
    device.ReportLoss();
}

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);
    AllocateNewContext();
    return signal_value;
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    Wait(presubmit_tick);
    AllocateNewContext();
}

void Scheduler::WaitWorker() {
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
}

void Scheduler::DispatchWork() {
    if (chunk && !chunk->Empty()) {
        {
            std::scoped_lock ql{queue_mutex};
            work_queue.push(std::move(chunk));
        }
        event_cv.notify_all();
        AcquireNewChunk();
    }
}

void Scheduler::BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                                    const VkClearValue* clear_values, u32 clear_value_count) {
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    state.renderpass = renderpass;
    state.framebuffer = framebuffer_handle;
    state.render_area = render_area;

    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string render_pass_info =
            fmt::format("renderArea={}x{}, numImages={}", render_area.width, render_area.height,
                        framebuffer->NumImages());
        GPU::Logging::GPULogger::GetInstance().LogRenderPassBegin(render_pass_info);
    }

    std::array<VkClearValue, 9> values{};
    for (u32 i = 0; i < clear_value_count && i < values.size(); ++i) {
        values[i] = clear_values[i];
    }
    Record([renderpass, framebuffer_handle, render_area, values, clear_value_count](
               vk::CommandBuffer cmdbuf) {
        const VkRenderPassBeginInfo renderpass_bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = renderpass,
            .framebuffer = framebuffer_handle,
            .renderArea =
                {
                    .offset = {.x = 0, .y = 0},
                    .extent = render_area,
                },
            .clearValueCount = clear_value_count,
            .pClearValues = clear_value_count != 0 ? values.data() : nullptr,
        };
        cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
    });
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
    framebuffer->MarkResolveShadowsUpToDate();
}

void Scheduler::RealizeDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    const DeferredClear dc = deferred_clear;
    deferred_clear = {};

    std::array<VkClearValue, 9> clear_values{};
    u32 count = 0;
    const RenderPassKey& base = dc.framebuffer->RenderPassKeyBase();
    for (u32 slot = 0; slot < 8; ++slot) {
        if (base.color_formats[slot] == VideoCore::Surface::PixelFormat::Invalid) {
            continue;
        }
        clear_values[count++] = dc.color_values[slot];
    }
    if (base.depth_format != VideoCore::Surface::PixelFormat::Invalid) {
        clear_values[count++] = dc.depth_stencil_value;
    }
    const u32 color_discard_mask =
        dc.framebuffer->DiscardsMsaaColor() ? dc.color_clear_mask : 0u;
    const bool depth_stencil_discard =
        dc.depth_stencil && dc.framebuffer->DiscardsMsaaDepthStencil();
    const VkRenderPass renderpass = dc.framebuffer->RenderPassVariant(
        dc.color_clear_mask, dc.depth_stencil, color_discard_mask, depth_stencil_discard);
    EndRenderPass();
    BeginRenderPassImpl(dc.framebuffer, renderpass, clear_values.data(), count);
}

bool Scheduler::DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot,
                                const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.color_clear_mask |= 1u << rt_slot;
    deferred_clear.color_values[rt_slot] = value;
    return true;
}

bool Scheduler::DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.depth_stencil = true;
    deferred_clear.depth_stencil_value = value;
    return true;
}

void Scheduler::FlushDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    RealizeDeferredClear();
    EndRenderPass();
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer) {
    if (deferred_clear.framebuffer == framebuffer) {
        RealizeDeferredClear();
        return;
    }
    const VkRenderPass renderpass = framebuffer->RenderPass();
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    if (renderpass == state.renderpass && framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        return;
    }
    // Ends any active pass and realizes a deferred clear
    EndRenderPass();
    BeginRenderPassImpl(framebuffer, renderpass, nullptr, 0);
}

void Scheduler::RequestOutsideRenderPassOperationContext() {
    EndRenderPass();
}

bool Scheduler::UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
    if (state.graphics_pipeline == pipeline) {
        if (pipeline && pipeline->UsesExtendedDynamicState() &&
            state.needs_state_enable_refresh) {
            state_tracker.InvalidateStateEnableFlag();
            state.needs_state_enable_refresh = false;
        }
        return false;
    }

    state.graphics_pipeline = pipeline;

    if (!pipeline) {
        return true;
    }

    if (!pipeline->UsesExtendedDynamicState()) {
        state.needs_state_enable_refresh = true;
    } else if (state.needs_state_enable_refresh) {
        state_tracker.InvalidateStateEnableFlag();
        state.needs_state_enable_refresh = false;
    }

    return true;
}

bool Scheduler::UpdateRescaling(bool is_rescaling) {
    if (state.rescaling_defined && is_rescaling == state.is_rescaling) {
        return false;
    }
    state.rescaling_defined = true;
    state.is_rescaling = is_rescaling;
    return true;
}

bool Scheduler::UpdateDescriptorBufferChunk(u32 descriptor_chunk) {
    if (state.descriptor_buffer_bound && descriptor_chunk == state.descriptor_buffer_chunk) {
        return false;
    }
    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        {
            std::unique_lock lk{queue_mutex};

            // Wait for work.
            event_cv.wait(lk, stop_token, [&] { return TryPopQueue(work); });

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            void(std::exchange(lk, std::unique_lock{execution_mutex}));

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            if (!work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf, breadcrumbs.get())) {
                OnDeviceLost();
            }

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
        }

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    EndPendingOperations();
    InvalidateState();

    const u64 signal_value = master_semaphore->NextTick();
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        upload_cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        upload_cmdbuf.End();
        cmdbuf.End();

        if (on_submit) {
            on_submit();
        }

        std::scoped_lock lock{submit_mutex};
        VkResult submit_result = master_semaphore->SubmitQueue(
            cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, signal_value);
        if (submit_result == VK_SUCCESS && SimulatedLoss()) {
            submit_result = VK_ERROR_DEVICE_LOST;
        }
        switch (const VkResult result = submit_result) {
        case VK_SUCCESS:
            // Log successful queue submission
            if (GPU::Logging::IsActive() &&
                Settings::values.gpu_log_vulkan_calls.GetValue()) {
                GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                    "vkQueueSubmit", "", VK_SUCCESS);
            }
            break;
        case VK_ERROR_DEVICE_LOST:
            // Harbor: no exception (it would leave this thread and abort the process). The loss
            // handlers stop the console; nothing submitted from now on will signal.
            OnDeviceLost();
            break;
        default:
            vk::Check(result);
            break;
        }
    });
    chunk->MarkSubmit();
    DispatchWork();
    return signal_value;
}

void Scheduler::AllocateNewContext() {
    // Enable counters once again. These are disabled when a command buffer is finished.
}

void Scheduler::InvalidateState() {
    state.graphics_pipeline = nullptr;
    state.rescaling_defined = false;
    state.descriptor_buffer_bound = false;
    state_tracker.InvalidateCommandBufferState();
}

void Scheduler::EndPendingOperations() {
    query_cache->CounterReset(VideoCommon::QueryType::ZPassPixelCount64);
    EndRenderPass();
}

void Scheduler::EndRenderPass()
    {
        RealizeDeferredClear();
        if (!state.renderpass) {
            return;
        }

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);

        // Log render pass end
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogRenderPassEnd();
        }

        query_cache->CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);
        query_cache->NotifySegment(false);

        Record([num_images = num_renderpass_images,
                       images = renderpass_images,
                       ranges = renderpass_image_ranges,
                       has_transform_feedback = device.IsExtTransformFeedbackSupported()](
                          vk::CommandBuffer cmdbuf) {
            std::array<VkImageMemoryBarrier, 9> barriers;
            for (size_t i = 0; i < num_images; ++i) {
                const VkImageSubresourceRange& range = ranges[i];
                const bool is_color = (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
                const bool is_depth_stencil = (range.aspectMask
                                              & (VK_IMAGE_ASPECT_DEPTH_BIT
                                                 | VK_IMAGE_ASPECT_STENCIL_BIT)) !=0;

                VkAccessFlags src_access = 0;

                if (is_color)
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                else if (is_depth_stencil)
                    src_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                else
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

                barriers[i] = VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = src_access,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = images[i],
                        .subresourceRange = range,
                };
            }
            cmdbuf.EndRenderPass();
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   0, nullptr, nullptr, vk::Span(barriers.data(), num_images));
            if (has_transform_feedback) {
                static constexpr VkMemoryBarrier XFB_OUTPUT_BARRIER{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                    .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, XFB_OUTPUT_BARRIER);
            }
        });

        state.renderpass = VkRenderPass{};
        num_renderpass_images = 0;
    }


void Scheduler::AcquireNewChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        chunk = std::make_unique<CommandChunk>();
    } else {
        // Otherwise, we can just take from the reserve.
        chunk = std::move(chunk_reserve.back());
        chunk_reserve.pop_back();
    }
}

} // namespace Vulkan
