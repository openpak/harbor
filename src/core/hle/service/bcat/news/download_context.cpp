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
#include "core/hle/service/cmif_serialization.h"
#include "core/memory.h"

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::News {
namespace {

// 2125-0081: the download did not decode.
constexpr Result ResultNotDecoded{ErrorModule::News, 81};

} // namespace

// What the worker and the object share. The worker writes into the caller's memory and signals the
// event under `mutex` while `alive`; the object clears `alive` under it before the event goes away.
struct IDownloadContext::State {
    std::mutex mutex;
    bool alive{true};
    Result result{ResultNotDecoded};
    u64 size{};
};

IDownloadContext::IDownloadContext(Core::System& system_, std::shared_ptr<NewsShared> shared_,
                                   std::string url, Kernel::KTransferMemory* transfer_memory_,
                                   u64 capacity)
    : ServiceFramework{system_, "IDownloadContext"}, service_context{system_, "IDownloadContext"},
      transfer_memory{transfer_memory_}, state{std::make_shared<State>()} {
    static const FunctionInfo functions[] = {
        {0, D<&IDownloadContext::GetCompletionEvent>, "GetCompletionEvent"},
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

    // The worker owns copies of everything it needs. It never touches this object's members except
    // through `state`, which the object clears before it goes.
    std::thread([&kernel_system = system_, shared_state = state, done_event = completed, process, address,
                 capacity, url = std::move(url)] {
        Result result = ResultNotDecoded;
        u64 size = 0;

        const std::string route = LocalRoute(url);
        const auto fetcher = BCAT::GetOpenPakFetcher();

        std::vector<u8> payload;
        bool have = false;
        if (route.empty() || !fetcher) {
            LOG_INFO(Service_BCAT, "News download: {}",
                     route.empty() ? "no decoded route for it" : "no OpenPak fetcher");
        } else {
            const auto response = fetcher->Get(BCAT::FetchOrigin::NewsEdge, route);
            if (response.reached && response.status == 200) {
                payload = response.body;
                have = true;
                LOG_INFO(Service_BCAT, "[OpenPak] News download: {} bytes from {}", payload.size(),
                         route);
            } else {
                LOG_WARNING(Service_BCAT, "[OpenPak] News download from {} failed ({})", route,
                            response.reached ? response.status : 0);
            }
        }

        std::scoped_lock lock{shared_state->mutex};
        if (!shared_state->alive) {
            return;
        }

        if (have && process != nullptr && payload.size() <= capacity) {
            process->GetMemory().WriteBlock(address, payload.data(), payload.size());
            result = ResultSuccess;
            size = payload.size();
        } else if (have) {
            LOG_WARNING(Service_BCAT, "News download: {} bytes do not fit the caller's {}",
                        payload.size(), capacity);
        }

        shared_state->result = result;
        shared_state->size = size;
        done_event->Signal(kernel_system.Kernel());
    }).detach();
}

IDownloadContext::~IDownloadContext() {
    {
        std::scoped_lock lock{state->mutex};
        state->alive = false;
    }

    if (transfer_memory != nullptr) {
        transfer_memory->Close(system.Kernel());
    }
    service_context.CloseEvent(completed);
}

Result IDownloadContext::GetCompletionEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = &completed->GetReadableEvent();
    R_SUCCEED();
}

Result IDownloadContext::GetResult() {
    std::scoped_lock lock{state->mutex};
    R_RETURN(state->result);
}

Result IDownloadContext::GetSize(Out<u64> out_size) {
    std::scoped_lock lock{state->mutex};
    *out_size = state->size;
    R_SUCCEED();
}

} // namespace Service::News
