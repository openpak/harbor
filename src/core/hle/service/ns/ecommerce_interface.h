// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/uuid.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KReadableEvent;
}

namespace OpenPak {
class AsyncWorker;
}

namespace Service::NS {

class ICompletedAsyncResult;
class IPrefetchAsyncResult;

class IECommerceInterface final : public ServiceFramework<IECommerceInterface> {
public:
    explicit IECommerceInterface(Core::System& system_);
    ~IECommerceInterface() override;

private:
    // [Harbor] 0-4: requests a console sends to the eShop's licence server through nim. OpenPak
    // runs no licence service, so each completes at once and changes nothing.
    Result RequestLinkDevice(OutCopyHandle<Kernel::KReadableEvent> out_event,
                             OutInterface<ICompletedAsyncResult> out_result, Common::UUID user_id);
    Result RequestUnlinkDevice(OutCopyHandle<Kernel::KReadableEvent> out_event,
                               OutInterface<ICompletedAsyncResult> out_result,
                               Common::UUID user_id);
    Result RequestCleanupAllPreInstalledApplications(
        OutCopyHandle<Kernel::KReadableEvent> out_event,
        OutInterface<ICompletedAsyncResult> out_result);
    Result RequestCleanupPreInstalledApplication(OutCopyHandle<Kernel::KReadableEvent> out_event,
                                                 OutInterface<ICompletedAsyncResult> out_result,
                                                 u64 application_id);
    Result RequestSyncRights(OutCopyHandle<Kernel::KReadableEvent> out_event,
                             OutInterface<ICompletedAsyncResult> out_result);

    void Completed(OutCopyHandle<Kernel::KReadableEvent>& out_event,
                   OutInterface<ICompletedAsyncResult>& out_result);

    // [Harbor] 8/10 prefetch the user's Nintendo Account token for the Lhub and shop pages, 9/11
    // read it back. No public names.
    Result PrefetchForLhub(OutCopyHandle<Kernel::KReadableEvent> out_event,
                           OutInterface<IPrefetchAsyncResult> out_result, Common::UUID user_id,
                           u64 time_span);
    Result PrefetchForShop(OutCopyHandle<Kernel::KReadableEvent> out_event,
                           OutInterface<IPrefetchAsyncResult> out_result, Common::UUID user_id,
                           u64 time_span);
    void LoadPrefetchedForLhub(HLERequestContext& ctx);
    void LoadPrefetchedForShop(HLERequestContext& ctx);

    Result Prefetch(OutCopyHandle<Kernel::KReadableEvent>& out_event,
                    OutInterface<IPrefetchAsyncResult>& out_result, const Common::UUID& user_id,
                    const char* target);
    void LoadPrefetched(HLERequestContext& ctx);

    std::shared_ptr<OpenPak::AsyncWorker> worker;
};

} // namespace Service::NS
