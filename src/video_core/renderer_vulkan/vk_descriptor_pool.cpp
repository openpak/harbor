// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <fmt/format.h>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"
#include <ranges>
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

// Prefer small grow rates to avoid saturating the descriptor pool with barely used pipelines
constexpr size_t SETS_GROW_RATE = 16;
constexpr s32 SCORE_THRESHOLD = 3;

struct DescriptorBank {
    DescriptorBankInfo info;
    std::mutex pools_mutex;
    std::vector<vk::DescriptorPool> pools;
};

namespace {
// Totals for the diagnostics line; only ever read for logging.
std::atomic<u64> stat_banks{};
std::atomic<u64> stat_pools{};
std::atomic<u64> stat_sets_allocated{};
std::atomic<u64> stat_pool_retries{};
std::atomic<u64> stat_growth_failures{};
} // Anonymous namespace

std::string DescriptorPoolStats() {
    // Sets are never given back (pools are created without the free bit, as upstream does), so
    // this only grows with the number of pipelines.
    return fmt::format("descriptors: banks={} pools={} sets={} pool_retries={} grow_failures={}",
                       stat_banks.load(), stat_pools.load(), stat_sets_allocated.load(),
                       stat_pool_retries.load(), stat_growth_failures.load());
}

bool DescriptorBankInfo::IsSuperset(const DescriptorBankInfo& subset) const noexcept {
    return uniform_buffers >= subset.uniform_buffers && storage_buffers >= subset.storage_buffers &&
           texture_buffers >= subset.texture_buffers && image_buffers >= subset.image_buffers &&
           textures >= subset.textures && images >= subset.images;
}

template <typename Descriptors>
static u32 Accumulate(const Descriptors& descriptors) {
    u32 count = 0;
    for (const auto& descriptor : descriptors) {
        count += descriptor.count;
    }
    return count;
}

static DescriptorBankInfo MakeBankInfo(std::span<const Shader::Info> infos) {
    DescriptorBankInfo bank;
    for (const Shader::Info& info : infos) {
        bank.uniform_buffers += Accumulate(info.constant_buffer_descriptors);
        bank.storage_buffers += Accumulate(info.storage_buffers_descriptors);
        bank.texture_buffers += Accumulate(info.texture_buffer_descriptors);
        bank.image_buffers += Accumulate(info.image_buffer_descriptors);
        bank.textures += Accumulate(info.texture_descriptors);
        bank.images += Accumulate(info.image_descriptors);
    }
    bank.score = bank.uniform_buffers + bank.storage_buffers + bank.texture_buffers +
                 bank.image_buffers + bank.textures + bank.images;
    return bank;
}

static void AllocatePool(const Device& device, DescriptorBank& bank) {
    std::array<VkDescriptorPoolSize, 6> pool_sizes;
    size_t pool_cursor{};
    const u32 sets_per_pool = device.GetSetsPerPool();
    const auto add = [&](VkDescriptorType type, u32 count) {
        if (count > 0) {
            pool_sizes[pool_cursor++] = {
                .type = type,
                .descriptorCount = count * sets_per_pool,
            };
        }
    };
    const auto& info{bank.info};
    add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, info.uniform_buffers);
    add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, info.storage_buffers);
    add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, info.texture_buffers);
    add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, info.image_buffers);
    add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, info.textures);
    add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, info.images);
    stat_pools.fetch_add(1, std::memory_order_relaxed);
    bank.pools.push_back(device.GetLogical().CreateDescriptorPool({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = sets_per_pool,
        .poolSizeCount = static_cast<u32>(pool_cursor),
        .pPoolSizes = std::data(pool_sizes),
    }));
}

DescriptorAllocator::DescriptorAllocator(const Device& device_, MasterSemaphore& master_semaphore_,
                                         DescriptorBank& bank_, VkDescriptorSetLayout layout_)
    : ResourcePool(master_semaphore_, SETS_GROW_RATE), device{&device_}, bank{&bank_},
      layout{layout_} {}

VkDescriptorSet DescriptorAllocator::Commit() {
    const size_t index = CommitResource();
    const size_t chunk = index / SETS_GROW_RATE;
    if (chunk >= sets.size() || sets[chunk].size() == 0) {
        // Never hand out a set that does not exist (a failed grow); the caller treats the
        // memory error like any other.
        throw vk::Exception(VK_ERROR_OUT_OF_HOST_MEMORY);
    }
    const VkDescriptorSet set = sets[chunk][index % SETS_GROW_RATE];
    if (set == VK_NULL_HANDLE) {
        throw vk::Exception(VK_ERROR_OUT_OF_HOST_MEMORY);
    }
    return set;
}

void DescriptorAllocator::Allocate(size_t begin, size_t end) {
    sets.push_back(AllocateDescriptors(end - begin));
}

vk::DescriptorSets DescriptorAllocator::AllocateDescriptors(size_t count) {
    const std::vector<VkDescriptorSetLayout> layouts(count, layout);
    VkDescriptorSetAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = VK_NULL_HANDLE,
        .descriptorSetCount = static_cast<u32>(count),
        .pSetLayouts = layouts.data(),
    };
    const auto note = [count](vk::DescriptorSets& result) {
        stat_sets_allocated.fetch_add(count, std::memory_order_relaxed);
        return std::move(result);
    };
    try {
        std::scoped_lock lock{bank->pools_mutex};
        vk::DescriptorPool& current = bank->pools.back();
        allocate_info.descriptorPool = *current;
        vk::DescriptorSets first = current.Allocate(allocate_info);
        if (!first.IsOutOfPoolMemory()) {
            return note(first);
        }
        // The newest pool is out of memory (or too fragmented). Allocate a new one and retry.
        stat_pool_retries.fetch_add(1, std::memory_order_relaxed);
        AllocatePool(*device, *bank);
        vk::DescriptorPool& pool = bank->pools.back();
        allocate_info.descriptorPool = *pool;
        vk::DescriptorSets new_sets = pool.Allocate(allocate_info);
        if (!new_sets.IsOutOfPoolMemory()) {
            return note(new_sets);
        }
    } catch (...) {
        stat_growth_failures.fetch_add(1, std::memory_order_relaxed);
        throw;
    }
    // After allocating a new pool, we are out of memory again. We can't handle this from here.
    stat_growth_failures.fetch_add(1, std::memory_order_relaxed);
    throw vk::Exception(VK_ERROR_OUT_OF_POOL_MEMORY);
}

DescriptorPool::DescriptorPool(const Device& device_, Scheduler& scheduler) {}
DescriptorPool::~DescriptorPool() = default;

DescriptorAllocator DescriptorPool::Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, std::span<const Shader::Info> infos) {
    return Allocator(device, scheduler, layout, MakeBankInfo(infos));
}

DescriptorAllocator DescriptorPool::Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, const Shader::Info& info) {
    return Allocator(device, scheduler, layout, MakeBankInfo(std::array{info}));
}

DescriptorAllocator DescriptorPool::Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, const DescriptorBankInfo& info) {
    return DescriptorAllocator(device, scheduler.GetMasterSemaphore(), Bank(device, info), layout);
}

DescriptorBank& DescriptorPool::Bank(const Device& device, const DescriptorBankInfo& reqs) {
    std::shared_lock read_lock{banks_mutex};
    const auto it = std::ranges::find_if(bank_infos, [&reqs](const DescriptorBankInfo& bank) {
        return std::abs(bank.score - reqs.score) < SCORE_THRESHOLD && bank.IsSuperset(reqs);
    });
    if (it != bank_infos.end()) {
        return *banks[std::distance(bank_infos.begin(), it)].get();
    }
    read_lock.unlock();

    std::unique_lock write_lock{banks_mutex};
    // Built completely before it is published: a failing pool creation (out of memory) must not
    // leave a bank without pools for the next caller to find.
    auto new_bank = std::make_unique<DescriptorBank>();
    new_bank->info = reqs;
    AllocatePool(device, *new_bank);
    stat_banks.fetch_add(1, std::memory_order_relaxed);
    bank_infos.push_back(reqs);
    auto& bank = *banks.emplace_back(std::move(new_bank));
    return bank;
}

} // namespace Vulkan
