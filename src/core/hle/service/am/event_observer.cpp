// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/event_observer.h"
#include "core/hle/service/am/window_system.h"

namespace Service::AM {

enum class UserDataTag : u32 {
    WakeupEvent,
    AppletProcess,
};

EventObserver::EventObserver(Core::System& system, WindowSystem& window_system)
    : m_system(system), m_context(system, "am:EventObserver")
    , m_window_system(window_system)
    , m_wakeup_event(m_context)
    , m_wakeup_holder(m_wakeup_event.GetHandle())
{
    m_window_system.SetEventObserver(this);
    m_wakeup_holder.SetUserData(static_cast<uintptr_t>(UserDataTag::WakeupEvent));
    m_wakeup_holder.LinkToMultiWait(std::addressof(m_multi_wait));
    m_thread = std::jthread([this](std::stop_token stop_token) {
        Common::SetCurrentThreadName("am:EventObserver");
        while (!stop_token.stop_requested()) {
            auto* signaled_holder = this->WaitSignaled(stop_token);
            if (!signaled_holder || stop_token.stop_requested())
                break;
            this->Process(signaled_holder);
        }
        std::scoped_lock lk{m_exit_lock};
        m_thread_exited = true;
        m_exit_cv.notify_all();
    });

    // This object lives on am's guest service thread, which never resumes once the cores are shut
    // down, so the kernel stops the thread before it tears itself down; otherwise the processes
    // the kernel terminates wake the thread into a dead kernel.
    m_kernel_stop_id = m_system.Kernel().AddHostThreadStop([this] { this->Stop(); });
}

EventObserver::~EventObserver() {
    m_system.Kernel().RemoveHostThreadStop(m_kernel_stop_id);
    this->Stop();

    // Free remaining owned sessions.
    auto it = m_process_holder_list.begin();
    while (it != m_process_holder_list.end()) {
        // Get the holder.
        auto* const holder = std::addressof(*it);

        // Remove from the list.
        it = m_process_holder_list.erase(it);

        // Free the holder.
        delete holder;
    }
}

void EventObserver::Stop() {
    if (!m_thread.joinable()) {
        return;
    }

    // Signal thread and wait for processing to finish. A recalculation blocked on a lock that a
    // suspended guest thread holds never finishes; the thread is then left behind, it stops at
    // its next wakeup.
    m_thread.request_stop();
    m_wakeup_event.Signal(m_system.Kernel());
    bool exited;
    {
        std::unique_lock lk{m_exit_lock};
        exited = m_exit_cv.wait_for(lk, std::chrono::seconds{2}, [this] { return m_thread_exited; });
    }
    if (exited) {
        m_thread.join();
    } else {
        LOG_WARNING(Service_AM, "am: event observer still busy at shutdown; detached");
        m_thread.detach();
    }
}

void EventObserver::TrackAppletProcess(Applet& applet) {
    // Don't observe dummy processes.
    if (!applet.process->IsInitialized()) {
        return;
    }

    // Allocate new holder.
    auto* holder = new ProcessHolder(applet, *applet.process);
    holder->SetUserData(static_cast<uintptr_t>(UserDataTag::AppletProcess));

    // Insert into list.
    {
        std::scoped_lock lk{m_lock};
        m_process_holder_list.push_back(*holder);
        holder->LinkToMultiWait(std::addressof(m_deferred_wait_list));
    }

    // Signal wakeup.
    m_wakeup_event.Signal(m_system.Kernel());
}

void EventObserver::RequestUpdate() {
    m_wakeup_event.Signal(m_system.Kernel());
}

void EventObserver::LinkDeferred() {
    std::scoped_lock lk{m_lock};
    m_multi_wait.MoveAll(std::addressof(m_deferred_wait_list));
}

MultiWaitHolder* EventObserver::WaitSignaled(std::stop_token stop_token) {
    while (true) {
        this->LinkDeferred();

        // If we're done, return before we start waiting.
        if (stop_token.stop_requested()) {
            return nullptr;
        }

        auto* selected = m_multi_wait.WaitAny(m_system.Kernel());
        if (selected != std::addressof(m_wakeup_holder)) {
            // Unlink the process.
            selected->UnlinkFromMultiWait();
        }

        return selected;
    }
}

void EventObserver::Process(MultiWaitHolder* holder) {
    switch (static_cast<UserDataTag>(holder->GetUserData())) {
    case UserDataTag::WakeupEvent:
        this->OnWakeupEvent(holder);
        break;
    case UserDataTag::AppletProcess:
        this->OnProcessEvent(static_cast<ProcessHolder*>(holder));
        break;
    default:
        UNREACHABLE();
    }
}

void EventObserver::OnWakeupEvent(MultiWaitHolder* holder) {
    m_wakeup_event.Clear(m_system.Kernel());

    // Perform recalculation.
    m_window_system.Update();
}

void EventObserver::OnProcessEvent(ProcessHolder* holder) {
    // Check process state.
    auto& applet = holder->GetApplet();
    auto& process = holder->GetProcess();
    {
        std::scoped_lock lk{m_lock, applet.lock};
        if (process.IsTerminated()) {
            // Destroy the holder.
            this->DestroyAppletProcessHolderLocked(holder);

            // The ended process gives its memory back now, as on the console; a winding applet's
            // is reloaded on unwind. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx
            // base).
            process.Finalize();
            applet.process_released = true;
            LOG_INFO(Service_AM, "am: applet_id={}{} process released, Applet pool {} MiB free",
                     static_cast<u32>(applet.applet_id), applet.is_winding ? " (winding)" : "",
                     m_system.Kernel().MemoryManager().GetFreeSize(
                         Kernel::KMemoryManager::Pool::Applet) / (1024 * 1024));
        } else {
            // Reset signaled state.
            process.ResetSignal();

            // Relink wakeup event.
            holder->LinkToMultiWait(std::addressof(m_deferred_wait_list));
        }

        // Set running.
        applet.is_process_running = process.IsRunning();
    }

    // Perform recalculation.
    m_window_system.Update();
}

void EventObserver::DestroyAppletProcessHolderLocked(ProcessHolder* holder) {
    // Remove from owned list.
    m_process_holder_list.erase(m_process_holder_list.iterator_to(*holder));
    // Destroy and free.
    delete holder;
}

} // namespace Service::AM
