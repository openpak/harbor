// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <vector>

#include "common/uuid.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace Service::NS::Vgc {

// [Harbor] Virtual Game Cards (IApplicationManagerInterface 4000-4109) on a console that owns none.
// OpenPak serves no Nintendo licences, so the server state a real console syncs (nim 2025) is the
// empty one, and ns answers from its default record: every flag false, every list empty. Shapes are
// 23.0.1 ns's (dispatcher 0x0868577C, method infos 0x087CA3F6..0x087CB1A4) and qlaunch's proxies
// (research/vgc-re/notes.md).

/// The three async objects 23.0.1 ns hands out with a completion event:
/// Result (0x08677964): 0 Get, 1 Cancel, 2 GetErrorContext.
/// Value (0x086772F0): 0 GetSize, 1 Get (out buffer), 2 Cancel, 3 GetErrorContext.
/// Data (0x0867CA64): 0 GetSize, 1 Read (u64 offset -> u64 read, out buffer), 2 Cancel,
/// 3 GetErrorContext.
/// This one has completed: its event is signalled before it is handed out and Get answers
/// success with `value` (Value, Data) or nothing (Result).
class ICompletedAsync final : public ServiceFramework<ICompletedAsync> {
public:
    enum class Kind { Result, Value, Data };

    explicit ICompletedAsync(Core::System& system_, Kind kind_, std::vector<u8> value_ = {});
    ~ICompletedAsync() override;

    Kernel::KReadableEvent& Readable();

private:
    void GetSize(HLERequestContext& ctx);
    void GetResult(HLERequestContext& ctx);
    void GetValue(HLERequestContext& ctx);
    void Read(HLERequestContext& ctx);
    void Cancel(HLERequestContext& ctx);
    void GetErrorContext(HLERequestContext& ctx);

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event{};
    const Kind kind;
    const std::vector<u8> value;
};

/// 4026 and 4027 hand out an object (no event) whose commands 0, 1 and 2 each take nothing and
/// answer (event, object) (ns 0x086B14E4, 0x086B1B08). The object's type is not established.
class IOperation final : public ServiceFramework<IOperation> {
public:
    explicit IOperation(Core::System& system_, u32 command_);
    ~IOperation() override;

private:
    void Start(HLERequestContext& ctx);

    const u32 command;
};

/// Replies (copy handle: completion event, object: a completed async of `kind`).
void ReplyCompleted(Core::System& system, HLERequestContext& ctx, ICompletedAsync::Kind kind,
                    std::vector<u8> value = {});

/// $.activity.has_received_guidance of the user's VGC record (default false). 4073 sets it; it is
/// kept in the console's NAND, as ns keeps the record in its save.
bool HasReceivedGuidance(const Common::UUID& user_id);
void SetReceivedGuidance(const Common::UUID& user_id);

/// Logs the unknown-contract warning once per command, later calls at debug level.
void LogOnce(u32 command, const char* what);

} // namespace Service::NS::Vgc
