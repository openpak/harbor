// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/redact.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/ns/virtual_game_cards.h"

namespace Service::NS::Vgc {

namespace {

constexpr const char* KindName(ICompletedAsync::Kind kind) {
    switch (kind) {
    case ICompletedAsync::Kind::Result:
        return "nn::ns::detail::IAsyncResult";
    case ICompletedAsync::Kind::Value:
        return "nn::ns::detail::IAsyncValue";
    case ICompletedAsync::Kind::Data:
        return "nn::ns::detail::IAsyncData";
    }
    return "nn::ns::detail::IAsyncResult";
}

std::mutex state_mutex;
bool guidance_loaded = false;
std::set<std::string> guidance_received;
std::set<u32> logged_commands;

std::filesystem::path GuidancePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system" / "harbor" /
           "ns-vgc-guidance.txt";
}

// Under state_mutex.
void LoadGuidance() {
    if (guidance_loaded) {
        return;
    }
    guidance_loaded = true;
    std::istringstream lines{
        Common::FS::ReadStringFromFile(GuidancePath(), Common::FS::FileType::TextFile)};
    for (std::string line; std::getline(lines, line);) {
        if (line.size() == 32) {
            guidance_received.insert(line);
        }
    }
}

} // namespace

ICompletedAsync::ICompletedAsync(Core::System& system_, Kind kind_, std::vector<u8> value_)
    : ServiceFramework{system_, KindName(kind_)}, service_context{system_, "ns:vgc"},
      kind{kind_}, value{std::move(value_)} {
    // clang-format off
    static const FunctionInfo result_functions[] = {
        {0, &ICompletedAsync::GetResult, "Get"},
        {1, &ICompletedAsync::Cancel, "Cancel"},
        {2, &ICompletedAsync::GetErrorContext, "GetErrorContext"},
    };
    static const FunctionInfo value_functions[] = {
        {0, &ICompletedAsync::GetSize, "GetSize"},
        {1, &ICompletedAsync::GetValue, "Get"},
        {2, &ICompletedAsync::Cancel, "Cancel"},
        {3, &ICompletedAsync::GetErrorContext, "GetErrorContext"},
    };
    static const FunctionInfo data_functions[] = {
        {0, &ICompletedAsync::GetSize, "GetSize"},
        {1, &ICompletedAsync::Read, "Read"},
        {2, &ICompletedAsync::Cancel, "Cancel"},
        {3, &ICompletedAsync::GetErrorContext, "GetErrorContext"},
    };
    // clang-format on
    switch (kind) {
    case Kind::Result:
        RegisterHandlers(result_functions);
        break;
    case Kind::Value:
        RegisterHandlers(value_functions);
        break;
    case Kind::Data:
        RegisterHandlers(data_functions);
        break;
    }

    event = service_context.CreateEvent("ns:vgc:Completion");
    // The work is done before the object is handed out: qlaunch waits on the event, then reads.
    event->Signal(system.Kernel());
}

ICompletedAsync::~ICompletedAsync() {
    service_context.CloseEvent(event);
}

Kernel::KReadableEvent& ICompletedAsync::Readable() {
    return event->GetReadableEvent();
}

void ICompletedAsync::GetSize(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<u64>(value.size());
}

void ICompletedAsync::GetResult(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void ICompletedAsync::GetValue(HLERequestContext& ctx) {
    if (!value.empty() && ctx.CanWriteBuffer()) {
        const std::size_t size = std::min(value.size(), ctx.GetWriteBufferSize());
        ctx.WriteBuffer(value.data(), size);
    }
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void ICompletedAsync::Read(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto offset = rp.Pop<u64>();
    u64 read = 0;
    if (offset < value.size() && ctx.CanWriteBuffer()) {
        read = std::min<u64>(value.size() - offset, ctx.GetWriteBufferSize());
        ctx.WriteBuffer(value.data() + offset, read);
    }
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<u64>(read);
}

void ICompletedAsync::Cancel(HLERequestContext& ctx) {
    // Nothing is in flight.
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void ICompletedAsync::GetErrorContext(HLERequestContext& ctx) {
    // No error: an empty nn::err::ErrorContext. qlaunch aborts if this IPC fails (0x0855A0F4).
    if (ctx.CanWriteBuffer()) {
        const std::vector<u8> zeros(ctx.GetWriteBufferSize());
        ctx.WriteBuffer(zeros);
    }
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

IOperation::IOperation(Core::System& system_, u32 command_)
    : ServiceFramework{system_, "ns:vgc:IOperation"}, command{command_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &IOperation::Start, "Unknown0"},
        {1, &IOperation::Start, "Unknown1"},
        {2, &IOperation::Start, "Unknown2"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

IOperation::~IOperation() = default;

void IOperation::Start(HLERequestContext& ctx) {
    LOG_UNKNOWN_CONTRACT(Service_NS,
                         "ns:am2 {} object command {}: the returned object's type is not "
                         "established; answered a completed IAsyncResult",
                         command, ctx.GetCommand());
    ReplyCompleted(system, ctx, ICompletedAsync::Kind::Result);
}

void ReplyCompleted(Core::System& system, HLERequestContext& ctx, ICompletedAsync::Kind kind,
                    std::vector<u8> value) {
    auto async = std::make_shared<ICompletedAsync>(system, kind, std::move(value));
    IPC::ResponseBuilder rb{ctx, 2, 1, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, async->Readable());
    rb.PushIpcInterface<ICompletedAsync>(ctx, std::move(async));
}

bool HasReceivedGuidance(const Common::UUID& user_id) {
    std::scoped_lock lock{state_mutex};
    LoadGuidance();
    return guidance_received.contains(user_id.RawString());
}

void SetReceivedGuidance(const Common::UUID& user_id) {
    std::scoped_lock lock{state_mutex};
    LoadGuidance();
    if (!guidance_received.insert(user_id.RawString()).second) {
        return;
    }
    std::string text;
    for (const auto& uid : guidance_received) {
        text += uid;
        text += '\n';
    }
    const auto path = GuidancePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (Common::FS::WriteStringToFile(path, Common::FS::FileType::TextFile, text) != text.size()) {
        LOG_WARNING(Service_NS, "ns vgc: the guidance flag could not be saved");
    }
}

void LogOnce(u32 command, const char* what) {
    {
        std::scoped_lock lock{state_mutex};
        if (!logged_commands.insert(command).second) {
            LOG_DEBUG(Service_NS, "ns:am2 {} (Virtual Game Cards): {}", command, what);
            return;
        }
    }
    LOG_UNKNOWN_CONTRACT(Service_NS, "ns:am2 {} (Virtual Game Cards): {}", command, what);
}

} // namespace Service::NS::Vgc
