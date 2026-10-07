// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/kernel.h"
#include "core/openpak/async_worker.h"

namespace OpenPak {

namespace detail {

struct SignalQueue {
    explicit SignalQueue(Kernel::KernelCore& kernel_) : kernel{kernel_} {}

    Kernel::KernelCore& kernel;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> items;
    bool stopping = false;

    void Push(std::function<void()> fn) {
        {
            std::lock_guard lock{mutex};
            if (stopping) {
                return;
            }
            items.push_back(std::move(fn));
        }
        wake.notify_one();
    }

    void Loop() {
        while (true) {
            std::function<void()> fn;
            {
                std::unique_lock lock{mutex};
                wake.wait(lock, [this] { return stopping || !items.empty(); });
                if (stopping) {
                    return;
                }
                fn = std::move(items.front());
                items.pop_front();
            }
            fn();
        }
    }
};

} // namespace detail

bool AsyncTask::IsDone() const {
    std::lock_guard lock{mutex};
    return done;
}

Result AsyncTask::GetResult() const {
    std::lock_guard lock{mutex};
    return done ? result : ResultUnknown;
}

std::vector<u8> AsyncTask::Value() const {
    std::lock_guard lock{mutex};
    return done ? value : std::vector<u8>{};
}

void AsyncTask::Cancel() {
    stop.request_stop();
    std::lock_guard lock{mutex};
    if (done) {
        return;
    }
    done = true;
    result = cancelled_result;
    value.clear();
    SignalLocked();
}

void AsyncTask::Detach() {
    stop.request_stop();
    std::lock_guard lock{mutex};
    event = nullptr;
}

void AsyncTask::Complete(Result result_, std::vector<u8> value_) {
    std::lock_guard lock{mutex};
    if (done) {
        return; // cancelled first: what the work found is dropped
    }
    done = true;
    result = result_;
    value = std::move(value_);
    SignalLocked();
}

void AsyncTask::SignalLocked() {
    if (event == nullptr) {
        return;
    }
    if (const auto q = queue.lock()) {
        // The event is looked up again on the signalling thread: Detach may have run meanwhile.
        q->Push([this_task = shared_from_this(), q_weak = std::weak_ptr{q}] {
            const auto alive = q_weak.lock();
            if (!alive) {
                return;
            }
            std::lock_guard lock{this_task->mutex};
            if (this_task->event != nullptr) {
                this_task->event->Signal(alive->kernel);
            }
        });
    }
}

void HostEvent::Raise() {
    std::lock_guard lock{mutex};
    if (event == nullptr) {
        return;
    }
    if (const auto q = queue.lock()) {
        q->Push([this_event = shared_from_this(), q_weak = std::weak_ptr{q}] {
            const auto alive = q_weak.lock();
            if (!alive) {
                return;
            }
            std::lock_guard inner{this_event->mutex};
            if (this_event->event != nullptr) {
                this_event->event->Signal(alive->kernel);
            }
        });
    }
}

void HostEvent::Detach() {
    std::lock_guard lock{mutex};
    event = nullptr;
}

AsyncWorker::AsyncWorker(Core::System& system_, std::string name_)
    : system{system_}, name{std::move(name_)},
      queue{std::make_shared<detail::SignalQueue>(system_.Kernel())} {}

AsyncWorker::~AsyncWorker() {
    {
        std::lock_guard lock{queue->mutex};
        queue->stopping = true;
        queue->items.clear();
    }
    queue->wake.notify_all();
    std::lock_guard lock{thread_mutex};
    if (thread.joinable()) {
        thread.join();
    }
}

void AsyncWorker::EnsureThread() {
    std::lock_guard lock{thread_mutex};
    if (thread.joinable()) {
        return;
    }
    // A host thread of its own host process, registered with this session's kernel for exactly
    // its own life.
    thread = system.Kernel().RunOnHostCoreProcess(std::string{name},
                                                  [q = queue] { q->Loop(); });
}

std::shared_ptr<AsyncTask> AsyncWorker::Run(Kernel::KEvent* event, Result cancelled_result,
                                            Work work) {
    EnsureThread();

    auto task = std::make_shared<AsyncTask>();
    task->event = event;
    task->cancelled_result = cancelled_result;
    task->queue = queue;

    // Detached on purpose: the work may sit in a network timeout, and neither the guest nor a
    // shutdown waits for it. It holds the task, never the worker.
    std::thread([task, work = std::move(work), thread_name = name] {
        Common::SetCurrentThreadName((thread_name + " work").c_str());
        std::vector<u8> value;
        Result result = ResultUnknown;
        try {
            result = work(task->stop.get_token(), value);
        } catch (const std::exception& e) {
            LOG_ERROR(WebService, "[OpenPak] Async work failed: {}", e.what());
        }
        task->Complete(result, std::move(value));
    }).detach();

    return task;
}

void AsyncWorker::Post(std::function<void()> fn) {
    EnsureThread();
    queue->Push(std::move(fn));
}

std::shared_ptr<HostEvent> AsyncWorker::MakeHostEvent(Kernel::KEvent* event) {
    EnsureThread();
    auto host_event = std::make_shared<HostEvent>();
    host_event->event = event;
    host_event->queue = queue;
    return host_event;
}

Kernel::KernelCore& AsyncWorker::Kernel() {
    return system.Kernel();
}

} // namespace OpenPak
