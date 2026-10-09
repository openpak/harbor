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

// One button's lock (0 = HOME), a reader/writer lock as its accessors' names say: any number of
// readers hold it together while no writer does; a writer holds it alone.
struct LockState {
    explicit LockState(Core::System& system);

    // Signals each event while a lock of its kind can be taken (called with mutex held).
    void UpdateEventsLocked(Kernel::KernelCore& kernel);

    KernelHelpers::ServiceContext context;
    Event reader_event;  ///< signalled while no writer holds the lock
    Event writer_event;  ///< signalled while nobody holds it
    std::mutex mutex{};
    u32 readers{};
    bool writer{};
};

enum class LockKind { Reader, Writer };

class ILockAccessor final : public ServiceFramework<ILockAccessor> {
public:
    // Without a shared state the accessor has a lock of its own.
    explicit ILockAccessor(Core::System& system_, std::shared_ptr<LockState> state = nullptr,
                           LockKind kind = LockKind::Writer);
    ~ILockAccessor() override;

private:
    Result TryLock(Out<bool> out_is_locked, OutCopyHandle<Kernel::KReadableEvent> out_handle,
                   bool return_handle);
    Result Unlock();
    Result GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_handle);
    Result IsLocked(Out<bool> out_is_locked);

    Event& KindEvent();

private:
    const std::shared_ptr<LockState> m_state;
    const LockKind m_kind;
    // TryLock calls this accessor holds (it may take its lock again before unlocking).
    u32 m_held{};
};

} // namespace Service::AM
