// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Network work behind IAsyncContext / IAsyncResult / IAsyncValue-style commands, and guest events
// raised by host threads.
//
// A guest IPC handler never waits on the network: it starts the work here and answers at once,
// and the guest waits on its event. Two kinds of thread do this:
//
// - The work runs on a plain thread of its own (it may block for a long time, and nothing in it
//   touches a kernel object).
// - Every signal goes through the worker's one signalling thread, which is a host thread
//   registered with this emulation session's kernel and lives exactly as long as the worker.
//
// That is the session-owned path: a long-lived host thread (the Penne push, the heartbeat, an
// openpak worker) must never signal a KEvent itself, because the kernel's per-thread dummy KThread
// is thread_local and outlives the session that made it, so the next session would be signalled
// through a stale thread. Such threads Post() to the worker of the service that owns the event.
//
// Lifetime: the worker belongs to a service object (so to one session) and is destroyed with it;
// destruction joins the signalling thread at once, never the work. An IPC object that owns a
// task's event calls Detach() before it closes the event; after that nothing signals it.

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "core/hle/result.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
class KernelCore;
} // namespace Kernel

namespace OpenPak {

class AsyncWorker;

namespace detail {
struct SignalQueue;
}

/// One piece of work's state, shared by the IPC object that answers the guest and the work.
class AsyncTask : public std::enable_shared_from_this<AsyncTask> {
public:
    /// HasDone.
    bool IsDone() const;

    /// GetResult: what the work returned, the cancel result after Cancel, or ResultUnknown while
    /// it still runs.
    Result GetResult() const;

    /// IAsyncValue's payload (GetSize / Get): what the work wrote, once done.
    std::vector<u8> Value() const;

    /// Cancel: the task completes now with the cancel result and the event is signalled; the work
    /// is asked to stop (its stop_token) and whatever it returns later is dropped.
    void Cancel();

    /// The IPC object is going away: nothing signals its event from now on.
    void Detach();

private:
    friend class AsyncWorker;

    void Complete(Result result, std::vector<u8> value);
    void SignalLocked();

    mutable std::mutex mutex;
    bool done = false;
    Result result = ResultUnknown;
    Result cancelled_result = ResultUnknown;
    std::vector<u8> value;
    Kernel::KEvent* event = nullptr;
    std::stop_source stop;
    std::weak_ptr<detail::SignalQueue> queue;
};

/// A guest event that host threads raise (push deliveries, presence changes...). Raise() from
/// any thread; the signal itself happens on the worker's signalling thread.
class HostEvent : public std::enable_shared_from_this<HostEvent> {
public:
    void Raise();
    /// Call before the event is closed; after it Raise() does nothing.
    void Detach();

private:
    friend class AsyncWorker;

    std::mutex mutex;
    Kernel::KEvent* event = nullptr;
    std::weak_ptr<detail::SignalQueue> queue;
};

class AsyncWorker {
public:
    /// The work: runs on its own thread, may block, writes IAsyncValue bytes into `value`, and
    /// should give up early once `stop` is requested. Its return value is the task's result.
    using Work = std::function<Result(std::stop_token stop, std::vector<u8>& value)>;

    /// `name` names the signalling thread (and its host process), e.g. "acc:OpenPak".
    AsyncWorker(Core::System& system, std::string name);
    ~AsyncWorker();

    AsyncWorker(const AsyncWorker&) = delete;
    AsyncWorker& operator=(const AsyncWorker&) = delete;

    /// Start `work` and return at once. `event` (owned by the caller's ServiceContext) is
    /// signalled when the task completes or is cancelled; `cancelled_result` is what GetResult
    /// answers after Cancel.
    std::shared_ptr<AsyncTask> Run(Kernel::KEvent* event, Result cancelled_result, Work work);

    /// Run `fn` on the signalling thread, which is registered with this session's kernel: the
    /// one place a host thread's work may touch the guest's kernel objects. Any thread; never
    /// blocks. Dropped once the worker is gone.
    void Post(std::function<void()> fn);

    /// A guest event host threads may raise. The caller keeps it and Detaches it before closing
    /// the event.
    std::shared_ptr<HostEvent> MakeHostEvent(Kernel::KEvent* event);

    Kernel::KernelCore& Kernel();

private:
    void EnsureThread();

    Core::System& system;
    std::string name;
    std::shared_ptr<detail::SignalQueue> queue;
    std::mutex thread_mutex;
    std::jthread thread;
};

} // namespace OpenPak
