// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/service.h"

namespace Service::AM {

// One lock (the HOME button lock, or another button's): readers and writers share it.
struct LockState {
    explicit LockState(Core::System& system);

    KernelHelpers::ServiceContext context;
    Event event;
    std::mutex mutex{};
    bool is_locked{};
};

class ILockAccessor final : public ServiceFramework<ILockAccessor> {
public:
    // Without a shared state the accessor has a lock of its own.
    explicit ILockAccessor(Core::System& system_, std::shared_ptr<LockState> state = nullptr);
    ~ILockAccessor() override;

private:
    Result TryLock(Out<bool> out_is_locked, OutCopyHandle<Kernel::KReadableEvent> out_handle,
                   bool return_handle);
    Result Unlock();
    Result GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_handle);
    Result IsLocked(Out<bool> out_is_locked);

private:
    const std::shared_ptr<LockState> m_state;
};

} // namespace Service::AM
