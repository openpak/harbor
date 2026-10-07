// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// nn::account::detail::IAsyncContext (and IAsyncNetworkServiceLicenseKindContext) answered by
// work on an OpenPak::AsyncWorker: the IPC handler starts it and returns, the guest waits on the
// event, and the work's result is what GetResult reads back.

#pragma once

#include <memory>

#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"
#include "core/openpak/async_worker.h"

namespace Service::Account {

class OpenPakAsyncContext final : public ServiceFramework<OpenPakAsyncContext> {
public:
    /// `license_kind` adds IAsyncNetworkServiceLicenseKindContext 100, which answers Subscribed.
    OpenPakAsyncContext(Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker_,
                        OpenPak::AsyncWorker::Work work, bool license_kind = false);
    ~OpenPakAsyncContext() override;

    /// Work that is done the moment it starts (nothing to wait on).
    static OpenPak::AsyncWorker::Work Done(Result result);

private:
    void GetSystemEvent(HLERequestContext& ctx);
    void Cancel(HLERequestContext& ctx);
    void HasDone(HLERequestContext& ctx);
    void GetResult(HLERequestContext& ctx);
    void GetNetworkServiceLicenseKind(HLERequestContext& ctx);

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completion_event;
    std::shared_ptr<OpenPak::AsyncWorker> worker;
    std::shared_ptr<OpenPak::AsyncTask> task;
};

} // namespace Service::Account
