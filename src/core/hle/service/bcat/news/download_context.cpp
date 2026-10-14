// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <mutex>
#include <thread>

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/bcat/news/download_context.h"
#include "core/hle/service/bcat/news/news_shared.h"
#include "core/hle/service/bcat/openpak_fetch.h"
#include "core/openpak/async_worker.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/memory.h"

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::News {
namespace {

// 2125-0081: the download did not decode.
constexpr Result ResultNotDecoded{ErrorModule::News, 81};

} // namespace

// What the work and the object share. The work writes into the caller's memory under `mutex` while
// `alive`; the object clears `alive` under it before the memory's owner can go.
struct IDownloadContext::State {
    std::mutex mutex;
    bool alive{true};
    u64 size{};
};

IDownloadContext::IDownloadContext(Core::System& system_, std::shared_ptr<NewsShared> shared_,
                                   std::string url, Kernel::KTransferMemory* transfer_memory_,
                                   u64 capacity)
    : ServiceFramework{system_, "IDownloadContext"}, service_context{system_, "IDownloadContext"},
      transfer_memory{transfer_memory_}, state{std::make_shared<State>()} {
    static const FunctionInfo functions[] = {
        {0, D<&IDownloadContext::GetCompletionEvent>, "GetCompletionEvent"},
        {1, D<&IDownloadContext::Unknown1>, "Unknown1"},
        {2, D<&IDownloadContext::GetResult>, "GetResult"},
        {3, D<&IDownloadContext::GetSize>, "GetSize"},
    };
    RegisterHandlers(functions);

    completed = service_context.CreateEvent("IDownloadContext::Completed");

    Kernel::KProcess* process = nullptr;
    u64 address = 0;
    if (transfer_memory != nullptr) {
        transfer_memory->Open(system_.Kernel());
        process = transfer_memory->GetOwner();
        address = GetInteger(transfer_memory->GetSourceAddress());
        capacity = (std::min)(capacity, static_cast<u64>(transfer_memory->GetSize()));
    } else {
        capacity = 0;
    }

    // The fetch blocks, so it runs on the news worker's own thread; the worker signals `completed`
    // from its signalling thread when the work returns.
    task = shared_->worker->Run(
        completed, ResultNotDecoded,
        [shared_state = state, process, address, capacity,
         url = std::move(url)](std::stop_token, std::vector<u8>&) -> Result {
            const std::string route = LocalRoute(url);

            std::vector<u8> payload;
            if (route.empty()) {
                LOG_INFO(Service_BCAT, "News download: no decoded route for it");
                return ResultNotDecoded;
            }

            const auto response = BCAT::GetOpenPakFetcher()->Get(route);
            if (!response.reached || response.status != 200) {
                LOG_WARNING(Service_BCAT, "[OpenPak] News download from {} failed ({})", route,
                            response.reached ? response.status : 0);
                return ResultNotDecoded;
            }
            payload = response.body;
            LOG_INFO(Service_BCAT, "[OpenPak] News download: {} bytes from {}", payload.size(), route);

            if (payload.size() > capacity) {
                LOG_WARNING(Service_BCAT, "News download: {} bytes do not fit the caller's {}",
                            payload.size(), capacity);
                return ResultNotDecoded;
            }

            std::scoped_lock lock{shared_state->mutex};
            if (!shared_state->alive || process == nullptr) {
                return ResultNotDecoded;
            }
            process->GetMemory().WriteBlock(address, payload.data(), payload.size());
            shared_state->size = payload.size();
            return ResultSuccess;
        });
}

IDownloadContext::~IDownloadContext() {
    {
        std::scoped_lock lock{state->mutex};
        state->alive = false;
    }

    // Nothing signals the event after this.
    task->Detach();

    if (transfer_memory != nullptr) {
        transfer_memory->Close(system.Kernel());
        transfer_memory = nullptr;
    }
    service_context.CloseEvent(completed);
}

void IDownloadContext::ReleaseMemory() {
    if (transfer_memory == nullptr || !task->IsDone()) {
        return;
    }
    // The work has returned, so it writes nothing more; `alive` keeps a late one out regardless.
    {
        std::scoped_lock lock{state->mutex};
        state->alive = false;
    }
    transfer_memory->Close(system.Kernel());
    transfer_memory = nullptr;
}

Result IDownloadContext::GetCompletionEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = &completed->GetReadableEvent();
    R_SUCCEED();
}

Result IDownloadContext::Unknown1() {
    // Unknown contract: HOME's News scene sends command 1 to a download context (seen when leaving
    // a channel page or unfollowing: unanswered, qlaunch dies with fatal 2010-0212). Its meaning is
    // not read yet; success with no output lets the scene go on.
    LOG_WARNING(Service_BCAT, "unknown contract: IDownloadContext 1; returning success with no output");
    R_SUCCEED();
}

Result IDownloadContext::GetResult() {
    ReleaseMemory();
    R_RETURN(task->GetResult());
}

Result IDownloadContext::GetSize(Out<u64> out_size) {
    ReleaseMemory();
    std::scoped_lock lock{state->mutex};
    *out_size = state->size;
    R_SUCCEED();
}

} // namespace Service::News
