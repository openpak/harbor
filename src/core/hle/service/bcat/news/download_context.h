// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <memory>
#include <string>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"
#include "core/openpak/async_worker.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
class KReadableEvent;
class KTransferMemory;
} // namespace Kernel

namespace Service::News {

class NewsShared;

// The object INewsService 30500 returns: a download the news module runs in the background. On a
// console it fetches a signed, encrypted container and writes the decoded payload into the caller's
// transfer memory. Here the decoded payload comes straight from OpenPak's news edge, on a worker
// thread. When there is nothing to fetch the download ends as a console's did against a server
// without containers: 2125-0081, size 0. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx
// base).
class IDownloadContext final : public ServiceFramework<IDownloadContext> {
public:
    explicit IDownloadContext(Core::System& system_, std::shared_ptr<NewsShared> shared_,
                              std::string url, Kernel::KTransferMemory* transfer_memory,
                              u64 capacity);
    ~IDownloadContext() override;

private:
    struct State;

    Result GetCompletionEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetResult();
    Result GetSize(Out<u64> out_size);
    /// Let go of the caller's transfer memory once the work is done.
    void ReleaseMemory();

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completed{};
    // Held open while the worker may write into it, and let go as soon as the caller reads the
    // outcome (GetResult or GetSize after the event): the memory stays locked for transfer while
    // this holds it, and qlaunch frees its buffer right after a failed download, before the session
    // is closed. Holding it to the destructor made that free fail (qlaunch panicked with 2001-0106,
    // invalid current memory, when Find Channels asked for a channel with no detail).
    Kernel::KTransferMemory* transfer_memory{};
    std::shared_ptr<State> state;
    std::shared_ptr<OpenPak::AsyncTask> task;
};

} // namespace Service::News
