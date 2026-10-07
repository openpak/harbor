// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include "core/hle/kernel/k_event.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/acc/openpak_async.h"
#include "core/hle/service/ipc_helpers.h"

namespace Service::Account {

namespace {

// nn::account::NetworkServiceLicenseKind (NoSubscription 0, Subscribed 1): OpenPak has no paid
// tier, every account is Subscribed.
constexpr u32 LicenseKindSubscribed = 1;

} // namespace

OpenPakAsyncContext::OpenPakAsyncContext(Core::System& system_,
                                         std::shared_ptr<OpenPak::AsyncWorker> worker_,
                                         OpenPak::AsyncWorker::Work work, bool license_kind)
    : ServiceFramework{system_, license_kind ? "IAsyncNetworkServiceLicenseKindContext"
                                             : "IAsyncContext"},
      service_context{system_, "acc:IAsyncContext"}, worker{std::move(worker_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &OpenPakAsyncContext::GetSystemEvent, "GetSystemEvent"},
        {1, &OpenPakAsyncContext::Cancel, "Cancel"},
        {2, &OpenPakAsyncContext::HasDone, "HasDone"},
        {3, &OpenPakAsyncContext::GetResult, "GetResult"},
    };
    static const FunctionInfo license_functions[] = {
        {100, &OpenPakAsyncContext::GetNetworkServiceLicenseKind, "GetNetworkServiceLicenseKind"},
    };
    // clang-format on

    RegisterHandlers(functions);
    if (license_kind) {
        RegisterHandlers(license_functions);
    }

    completion_event = service_context.CreateEvent("acc:IAsyncContext:Completion");
    // A failure the work does not map itself (a cancel, a refused request) reads back as the
    // connection error the account module's callers already handle.
    task = worker->Run(completion_event, ResultInternetRequestDenied, std::move(work));
}

OpenPakAsyncContext::~OpenPakAsyncContext() {
    task->Detach();
    service_context.CloseEvent(completion_event);
}

OpenPak::AsyncWorker::Work OpenPakAsyncContext::Done(Result result) {
    return [result](std::stop_token, std::vector<u8>&) { return result; };
}

void OpenPakAsyncContext::GetSystemEvent(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, completion_event->GetReadableEvent());
}

void OpenPakAsyncContext::Cancel(HLERequestContext& ctx) {
    task->Cancel();
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void OpenPakAsyncContext::HasDone(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(task->IsDone());
}

void OpenPakAsyncContext::GetResult(HLERequestContext& ctx) {
    // Before the work is done the module answers its wrong-step result.
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(task->IsDone() ? task->GetResult() : ResultInvalidProcedureState);
}

void OpenPakAsyncContext::GetNetworkServiceLicenseKind(HLERequestContext& ctx) {
    if (!task->IsDone()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultInvalidProcedureState);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(LicenseKindSubscribed);
}

} // namespace Service::Account
