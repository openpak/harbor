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

ICompletedAsyncValue::ICompletedAsyncValue(Core::System& system_, Service::Event* event_)
    : ServiceFramework{system_, "nn::ns::detail::IAsyncValue"}, event{event_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ICompletedAsyncValue::GetSize>, "GetSize"},
        {1, D<&ICompletedAsyncValue::Get>, "Get"},
        {2, D<&ICompletedAsyncValue::Cancel>, "Cancel"},
        {3, D<&ICompletedAsyncValue::GetErrorContext>, "GetErrorContext"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

ICompletedAsyncValue::~ICompletedAsyncValue() = default;

Result ICompletedAsyncValue::GetSize(Out<s64> out_size) {
    LOG_DEBUG(Service_NS, "called");
    *out_size = 0;
    R_SUCCEED();
}

Result ICompletedAsyncValue::Get(Out<u64> out_written, u64 offset,
                                 OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    LOG_DEBUG(Service_NS, "called, offset={:#x}", offset);
    std::memset(out_buffer.data(), 0, out_buffer.size());
    *out_written = 0;
    R_SUCCEED();
}

Result ICompletedAsyncValue::Cancel() {
    // Nothing is in flight: the work completed when the value was handed out.
    LOG_DEBUG(Service_NS, "called");
    R_SUCCEED();
}

Result ICompletedAsyncValue::GetErrorContext(
    OutLargeData<std::array<u8, 0x200>, BufferAttr_HipcMapAlias> out_error_context) {
    // No error happened: an empty nn::err::ErrorContext. 0x200 bytes, as 23.0.1 ns's handlers
    // (0x177b98, 0x17ce8c) expect.
    LOG_DEBUG(Service_NS, "called");
    *out_error_context = {};
    R_SUCCEED();
}

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
