// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ns/async_result.h"

#include <cstring>

namespace Service::NS {

IAsyncResult::IAsyncResult(Core::System& system_, Service::Event* event_)
    : ServiceFramework{system_, "nn::ns::detail::IAsyncResult"}, event{event_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IAsyncResult::Get>, "Get"},
        {1, D<&IAsyncResult::Cancel>, "Cancel"},
        {2, D<&IAsyncResult::GetErrorContext>, "GetErrorContext"}, // 4.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);
}

IAsyncResult::~IAsyncResult() = default;

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): every request completes at once
// and unchanged, so Get answers success.
Result IAsyncResult::Get() {
    LOG_DEBUG(Service_NS, "called");
    R_SUCCEED();
}

Result IAsyncResult::GetErrorContext(
    OutLargeData<std::array<u8, 0x200>, BufferAttr_HipcMapAlias> out_error_context) {
    LOG_DEBUG(Service_NS, "called");
    // No error happened: an empty nn::err::ErrorContext.
    *out_error_context = {};
    R_SUCCEED();
}

Result IAsyncResult::Cancel() {
    LOG_DEBUG(Service_NS, "called");
    if (event != nullptr) {
        event->Signal(system.Kernel());
    }

    R_SUCCEED();
}

} // namespace Service::NS
