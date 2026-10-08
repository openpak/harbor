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

/// nn::ns::detail::IAsyncValue as 23.0.1 ns hands it out for IApplicationManagerInterface 4008
/// (dispatcher 0x17ca64): 0 GetSize -> s64; 1 Get(u64 offset) -> u64 bytes written + out buffer;
/// 2 Cancel; 3 GetErrorContext -> fixed-size out buffer. This one has completed with an empty
/// value.
class ICompletedAsyncValue final : public ServiceFramework<ICompletedAsyncValue> {
public:
    explicit ICompletedAsyncValue(Core::System& system_, Service::Event* event_);
    ~ICompletedAsyncValue() override;

private:
    Result GetSize(Out<s64> out_size);
    Result Get(Out<u64> out_written, u64 offset, OutBuffer<BufferAttr_HipcMapAlias> out_buffer);
    Result Cancel();
    Result GetErrorContext(
        OutLargeData<std::array<u8, 0x200>, BufferAttr_HipcMapAlias> out_error_context);

    Service::Event* event{};
};

} // namespace Service::NS