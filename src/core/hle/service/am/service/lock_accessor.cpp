// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/service/am/service/lock_accessor.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::AM {

LockState::LockState(Core::System& system) : context{system, "ILockAccessor"}, event{context} {
    event.Signal(system.Kernel());
}

ILockAccessor::ILockAccessor(Core::System& system_, std::shared_ptr<LockState> state)
    : ServiceFramework{system_, "ILockAccessor"},
      m_state{state ? std::move(state) : std::make_shared<LockState>(system_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {1, D<&ILockAccessor::TryLock>, "TryLock"},
        {2, D<&ILockAccessor::Unlock>, "Unlock"},
        {3, D<&ILockAccessor::GetEvent>, "GetEvent"},
        {4, D<&ILockAccessor::IsLocked>, "IsLocked"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

ILockAccessor::~ILockAccessor() = default;

Result ILockAccessor::TryLock(Out<bool> out_is_locked,
                              OutCopyHandle<Kernel::KReadableEvent> out_handle,
                              bool return_handle) {
    {
        std::scoped_lock lk{m_state->mutex};
        if (m_state->is_locked) {
            *out_is_locked = false;
        } else {
            m_state->is_locked = true;
            *out_is_locked = true;
        }
    }

    // qlaunch takes and drops the HOME lock to decide whether a friend invitation pop-up may be
    // acted on.
    LOG_DEBUG(Service_AM, "called, return_handle={} acquired={}", return_handle, *out_is_locked);

    if (return_handle) {
        *out_handle = m_state->event.GetHandle();
    }

    R_SUCCEED();
}

Result ILockAccessor::Unlock() {
    LOG_DEBUG(Service_AM, "called");

    {
        std::scoped_lock lk{m_state->mutex};
        m_state->is_locked = false;
    }

    m_state->event.Signal(system.Kernel());
    R_SUCCEED();
}

Result ILockAccessor::GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_handle) {
    LOG_DEBUG(Service_AM, "called");
    *out_handle = m_state->event.GetHandle();
    R_SUCCEED();
}

Result ILockAccessor::IsLocked(Out<bool> out_is_locked) {
    LOG_DEBUG(Service_AM, "called");
    std::scoped_lock lk{m_state->mutex};
    *out_is_locked = m_state->is_locked;
    R_SUCCEED();
}

} // namespace Service::AM
