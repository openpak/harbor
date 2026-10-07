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

class IPrefetchAsyncResult;

class IECommerceInterface final : public ServiceFramework<IECommerceInterface> {
public:
    explicit IECommerceInterface(Core::System& system_);
    ~IECommerceInterface() override;

private:
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
