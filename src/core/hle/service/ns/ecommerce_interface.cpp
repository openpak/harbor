// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <string>

#include "common/redact.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/acc/openpak_account.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/ns/ecommerce_interface.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/session.h"

namespace Service::NS {

// [Harbor] nn::ns::detail::IAsyncResult for 8/10: the prefetch runs on an OpenPak::AsyncWorker
// task, the guest waits on the event, and Get answers what the work returned. (ns's shared
// IAsyncResult completes everything at once with success, so it cannot carry a sign-in's result.)
class IPrefetchAsyncResult final : public ServiceFramework<IPrefetchAsyncResult> {
public:
    IPrefetchAsyncResult(Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker_,
                         OpenPak::AsyncWorker::Work work)
        : ServiceFramework{system_, "nn::ns::detail::IAsyncResult"},
          service_context{system_, "ns:ec:IAsyncResult"}, worker{std::move(worker_)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IPrefetchAsyncResult::Get>, "Get"},
            {1, D<&IPrefetchAsyncResult::Cancel>, "Cancel"},
            {2, D<&IPrefetchAsyncResult::GetErrorContext>, "GetErrorContext"},
        };
        // clang-format on
        RegisterHandlers(functions);

        event = service_context.CreateEvent("ns:ec:IAsyncResult:Completion");
        // HOME cancels the prefetch before every launch and never reads the result. The work
        // (Session::Ensure) runs to its end regardless, so a cancel never aborts a sign-in.
        task = worker->Run(event, Account::ResultCancelledByUser, std::move(work));
    }

    ~IPrefetchAsyncResult() override {
        task->Detach();
        service_context.CloseEvent(event);
    }

    Kernel::KReadableEvent* Readable() {
        return &event->GetReadableEvent();
    }

private:
    Result Get() {
        // The guest asks once the event fired; before that the work has no result yet.
        R_RETURN(task->GetResult());
    }

    Result Cancel() {
        task->Cancel();
        R_SUCCEED();
    }

    Result GetErrorContext(
        OutLargeData<std::array<u8, 0x200>, BufferAttr_HipcMapAlias> out_error_context) {
        *out_error_context = {};
        R_SUCCEED();
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event{};
    std::shared_ptr<OpenPak::AsyncWorker> worker;
    std::shared_ptr<OpenPak::AsyncTask> task;
};

IECommerceInterface::IECommerceInterface(Core::System& system_)
    : ServiceFramework{system_, "IECommerceInterface"},
      worker{std::make_shared<OpenPak::AsyncWorker>(system_, "ns:ec:OpenPak")} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "RequestLinkDevice"},
        {1, nullptr, "RequestCleanupAllPreInstalledApplications"},
        {2, nullptr, "RequestCleanupPreInstalledApplication"},
        {3, nullptr, "RequestSyncRights"},
        {4, nullptr, "RequestUnlinkDevice"},
        {5, nullptr, "RequestRevokeAllELicense"},
        {6, nullptr, "RequestSyncRightsBasedOnAssignedELicenses"},
        {8, D<&IECommerceInterface::PrefetchForLhub>, "PrefetchForLhub"},
        {9, &IECommerceInterface::LoadPrefetchedForLhub, "LoadPrefetchedForLhub"},
        {10, D<&IECommerceInterface::PrefetchForShop>, "PrefetchForShop"},
        {11, &IECommerceInterface::LoadPrefetchedForShop, "LoadPrefetchedForShop"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IECommerceInterface::~IECommerceInterface() = default;

// [Harbor] Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base). HOME sends 8/10 from
// its StPrefetchNaIdToken state; systemWeb sends them for window.nx.shop.requestNaIdToken, reads
// the IAsyncResult once the event fires and, only on success, 9/11 into a 0x601-byte buffer. The
// token handed over is the user's BAAS id_token, which OpenPak's Lhub and shop pages accept for a
// linked user, under the rule IAdministrator 250 answers by.

Result IECommerceInterface::PrefetchForLhub(OutCopyHandle<Kernel::KReadableEvent> out_event,
                                            OutInterface<IPrefetchAsyncResult> out_result,
                                            Common::UUID user_id, u64 /*time_span*/) {
    return Prefetch(out_event, out_result, user_id, "Lhub");
}

Result IECommerceInterface::PrefetchForShop(OutCopyHandle<Kernel::KReadableEvent> out_event,
                                            OutInterface<IPrefetchAsyncResult> out_result,
                                            Common::UUID user_id, u64 /*time_span*/) {
    return Prefetch(out_event, out_result, user_id, "shop");
}

Result IECommerceInterface::Prefetch(OutCopyHandle<Kernel::KReadableEvent>& out_event,
                                     OutInterface<IPrefetchAsyncResult>& out_result,
                                     const Common::UUID& user_id, const char* target) {
    LOG_DEBUG(Service_NS, "called, {}", target);
    // The span asks for a token valid that long; the session refreshes a token near its expiry,
    // so whatever Ensure leaves is what 9/11 hand over.
    auto result = std::make_shared<IPrefetchAsyncResult>(
        system, worker, [user_id](std::stop_token, std::vector<u8>&) {
            if (Account::OpenPakAccount::Enabled()) {
                OpenPak::Session::Ensure(Account::OpenPakAccount::UidString(user_id));
            }
            std::string token;
            return Account::OpenPakAccount::LinkedIdToken(user_id, token);
        });
    *out_event = result->Readable();
    *out_result = std::move(result);
    R_SUCCEED();
}

void IECommerceInterface::LoadPrefetchedForLhub(HLERequestContext& ctx) {
    LoadPrefetched(ctx);
}

void IECommerceInterface::LoadPrefetchedForShop(HLERequestContext& ctx) {
    LoadPrefetched(ctx);
}

// (nn::account::Uid) -> (u64, buffer): the token, NUL terminated, and what is left of its life.
void IECommerceInterface::LoadPrefetched(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto user_id = rp.PopRaw<Common::UUID>();

    std::string token;
    if (const Result result = Account::OpenPakAccount::LinkedIdToken(user_id, token);
        result.IsError()) {
        LOG_DEBUG(Service_NS, "called, no linked token");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
        return;
    }

    token.push_back('\0');
    if (!ctx.CanWriteBuffer() || token.size() > ctx.GetWriteBufferSize()) {
        LOG_WARNING(Service_NS, "ns:ec id token is {} bytes, the buffer is smaller", token.size());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Account::ResultInvalidIdTokenCacheBufferSize);
        return;
    }
    ctx.WriteBuffer(token.data(), token.size());

    // systemWeb reports elapsedTime = 1 h - this (0x08dcaf10 / 0x08dcb310). The session keeps no
    // expiry to subtract from, and a prefetch has just ensured the token: a full hour.
    LOG_UNKNOWN_CONTRACT(Service_NS,
                         "ns:ec LoadPrefetched u64 (token life left, ns): answered 1 h");
    constexpr u64 OneHourNs = 3'600'000'000'000ULL;
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(OneHourNs);
}

} // namespace Service::NS
