// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/service.h"

namespace Service::NS {

class IAsyncResult final : public ServiceFramework<IAsyncResult> {
public:
    explicit IAsyncResult(Core::System& system_, Service::Event* event_);
    ~IAsyncResult() override;

private:
    Result Get();
    Result Cancel();
    Result GetErrorContext(
        OutLargeData<std::array<u8, 0x200>, BufferAttr_HipcMapAlias> out_error_context);

    Service::Event* event{};
};

} // namespace Service::NS