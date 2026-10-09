// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/service/am/service/lock_accessor.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::AM {

LockState::LockState(Core::System& system)
    : context{system, "ILockAccessor"}, reader_event{context}, writer_event{context} {
    reader_event.Signal(system.Kernel());
    writer_event.Signal(system.Kernel());
}

void LockState::UpdateEventsLocked(Kernel::KernelCore& kernel) {
    // qlaunch waits on the event before it tries the lock (a zero-timeout wait, else a real one).
    if (writer) {
        reader_event.Clear(kernel);
    } else {
        reader_event.Signal(kernel);
    }
    if (writer || readers != 0) {
        writer_event.Clear(kernel);
    } else {
        writer_event.Signal(kernel);
    }
}

ILockAccessor::ILockAccessor(Core::System& system_, std::shared_ptr<LockState> state,
                             LockKind kind)
    : ServiceFramework{system_, "ILockAccessor"},
      m_state{state ? std::move(state) : std::make_shared<LockState>(system_)}, m_kind{kind} {
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

ILockAccessor::~ILockAccessor() {
    // An accessor closed while it holds the lock lets it go.
    std::scoped_lock lk{m_state->mutex};
    if (m_held == 0) {
        return;
    }
    if (m_kind == LockKind::Writer) {
        m_state->writer = false;
    } else if (m_state->readers != 0) {
        --m_state->readers;
    }
    m_held = 0;
    m_state->UpdateEventsLocked(system.Kernel());
}

Event& ILockAccessor::KindEvent() {
    return m_kind == LockKind::Writer ? m_state->writer_event : m_state->reader_event;
}

Result ILockAccessor::TryLock(Out<bool> out_is_locked,
                              OutCopyHandle<Kernel::KReadableEvent> out_handle,
                              bool return_handle) {
    {
        std::scoped_lock lk{m_state->mutex};
        bool acquired = m_held != 0;
        if (!acquired) {
            if (m_kind == LockKind::Writer) {
                acquired = !m_state->writer && m_state->readers == 0;
                m_state->writer |= acquired;
            } else {
                acquired = !m_state->writer;
                m_state->readers += acquired ? 1 : 0;
            }
        }
        if (acquired) {
            ++m_held;
        }
        *out_is_locked = acquired;
        m_state->UpdateEventsLocked(system.Kernel());

        // qlaunch takes and drops the HOME lock to decide whether a friend invitation pop-up
        // may be acted on, and holds a reader lock across a title's launch.
        LOG_DEBUG(Service_AM, "called, {} return_handle={} acquired={} (readers {}, writer {})",
                  m_kind == LockKind::Writer ? "writer" : "reader", return_handle, acquired,
                  m_state->readers, m_state->writer);
    }

    if (return_handle) {
        *out_handle = KindEvent().GetHandle();
    }

    R_SUCCEED();
}

Result ILockAccessor::Unlock() {
    std::scoped_lock lk{m_state->mutex};
    if (m_held != 0 && --m_held == 0) {
        if (m_kind == LockKind::Writer) {
            m_state->writer = false;
        } else if (m_state->readers != 0) {
            --m_state->readers;
        }
    }
    m_state->UpdateEventsLocked(system.Kernel());
    LOG_DEBUG(Service_AM, "called, {} (readers {}, writer {})",
              m_kind == LockKind::Writer ? "writer" : "reader", m_state->readers,
              m_state->writer);
    R_SUCCEED();
}

Result ILockAccessor::GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_handle) {
    LOG_DEBUG(Service_AM, "called");
    *out_handle = KindEvent().GetHandle();
    R_SUCCEED();
}

Result ILockAccessor::IsLocked(Out<bool> out_is_locked) {
    LOG_DEBUG(Service_AM, "called");
    std::scoped_lock lk{m_state->mutex};
    *out_is_locked = m_state->writer || m_state->readers != 0;
    R_SUCCEED();
}

} // namespace Service::AM
