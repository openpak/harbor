// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/redact.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/npns/npns.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::NPNS {

// [OpenPak] What the firmware's friends program gets from npns:s 61: an object whose methods all
// succeed and answer nothing, until the calls it makes are known.
class INpnsStubObject final : public ServiceFramework<INpnsStubObject> {
public:
    explicit INpnsStubObject(Core::System& system_)
        : ServiceFramework{system_, "INpnsStubObject"} {
        std::vector<FunctionInfo> functions;
        for (u32 id = 0; id < 64; ++id) {
            functions.push_back({id, &INpnsStubObject::Succeed, "Stub"});
        }
        RegisterHandlers(functions.data(), functions.size());
    }

private:
    void Succeed(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

class INpnsSystem final : public ServiceFramework<INpnsSystem> {
public:
    explicit INpnsSystem(Core::System& system_)
        : ServiceFramework{system_, "npns:s"}, service_context{system, "npns:s"},
          get_receive_event{service_context}, get_state_change_event{service_context},
          get_request_change_state_cancel_event{service_context} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {1, C<&INpnsSystem::ListenAll>, "ListenAll"},
            {2, C<&INpnsSystem::ListenTo>, "ListenTo"},
            {3, nullptr, "Receive"},
            {4, nullptr, "ReceiveRaw"},
            {5, C<&INpnsSystem::GetReceiveEvent>, "GetReceiveEvent"},
            {6, nullptr, "ListenUndelivered"},
            {7, C<&INpnsSystem::GetStateChangeEvent>, "GetStateChangeEvent"},
            {8, C<&INpnsSystem::ListenToByName>, "ListenToByName"},
            {11, nullptr, "SubscribeTopic"},
            {12, nullptr, "UnsubscribeTopic"},
            {13, nullptr, "QueryIsTopicExist"},
            {14, nullptr, "SubscribeTopicByAccount"}, // 18.0.0+
            {15, nullptr, "UnsubscribeTopicByAccount"}, // 18.0.0+
            {16, nullptr, "DownloadSubscriptionList"}, // 18.0.0+
            {21, nullptr, "CreateToken"},
            {22, nullptr, "CreateTokenWithApplicationId"},
            {23, nullptr, "DestroyToken"},
            {24, nullptr, "DestroyTokenWithApplicationId"},
            {25, nullptr, "QueryIsTokenValid"},
            {26, C<&INpnsSystem::ListenToMyApplicationId>, "ListenToMyApplicationId"},
            {27, nullptr, "DestroyTokenAll"},
            {28, nullptr, "CreateTokenWithName"}, // 18.0.0+
            {29, nullptr, "DestroyTokenWithName"}, // 18.0.0+
            {31, nullptr, "UploadTokenToBaaS"},
            {32, nullptr, "DestroyTokenForBaaS"},
            {33, nullptr, "CreateTokenForBaaS"},
            {34, nullptr, "SetBaaSDeviceAccountIdList"},
            {35, nullptr, "LinkNsaId"}, // 17.0.0+
            {36, nullptr, "UnlinkNsaId"}, // 17.0.0+
            {37, nullptr, "RelinkNsaId"}, // 18.0.0+
            {40, nullptr, "GetNetworkServiceAccountIdTokenRequestEvent"}, // 17.0.0+
            {41, nullptr, "TryPopNetworkServiceAccountIdTokenRequestUid"}, // 17.0.0+
            {42, nullptr, "SetNetworkServiceAccountIdTokenSuccess"}, // 17.0.0+
            {43, nullptr, "SetNetworkServiceAccountIdTokenFailure"}, // 17.0.0+
            {44, nullptr, "SetUidList"}, // 17.0.0+
            {45, &INpnsSystem::PutDigitalTwinKeyValue, "PutDigitalTwinKeyValue"}, // 17.0.0+
            {46, &INpnsSystem::PutDigitalTwinKeyValue, "DigitalTwin46"}, // [OpenPak] undocumented
            {47, &INpnsSystem::PutDigitalTwinKeyValue, "DigitalTwin47"}, // [OpenPak] undocumented
            {48, &INpnsSystem::PutDigitalTwinKeyValue, "DigitalTwin48"}, // [OpenPak] undocumented
            {49, &INpnsSystem::PutDigitalTwinKeyValue, "DigitalTwin49"}, // [OpenPak] undocumented
            {50, &INpnsSystem::PutDigitalTwinKeyValue, "DigitalTwin50"}, // [OpenPak] two input buffers, no output (friends)
            {51, nullptr, "DeleteDigitalTwinKeyValue"}, // 18.0.0+
            {61, &INpnsSystem::OpenStubObject, "OpenObject61"}, // [OpenPak] undocumented
            {101, nullptr, "Suspend"},
            {102, nullptr, "Resume"},
            {103, C<&INpnsSystem::GetState>, "GetState"},
            {104, nullptr, "GetStatistics"},
            {105, nullptr, "GetPlayReportRequestEvent"},
            {106, C<&INpnsSystem::GetLastNotifiedTime>, "GetLastNotifiedTime"}, // 18.0.0+
            {107, nullptr, "SetLastNotifiedTime"}, // 18.0.0+
            {111, nullptr, "GetJid"},
            {112, nullptr, "CreateJid"},
            {113, nullptr, "DestroyJid"},
            {114, nullptr, "AttachJid"},
            {115, nullptr, "DetachJid"},
            {120, nullptr, "CreateNotificationReceiver"},
            {151, C<&INpnsSystem::GetStateWithHandover>, "GetStateWithHandover"},
            {152, C<&INpnsSystem::GetStateChangeEventWithHandover>, "GetStateChangeEventWithHandover"},
            {153, C<&INpnsSystem::GetStateChangeEventWithHandover>, "GetDropEventWithHandover"},
            {154, nullptr, "CreateTokenAsync"},
            {155, nullptr, "CreateTokenAsyncWithApplicationId"},
            {156, nullptr, "CreateTokenWithNameAsync"}, // 18.0.0+
            {161, C<&INpnsSystem::GetRequestChangeStateCancelEvent>, "GetRequestChangeStateCancelEvent"}, // 10.0.0+
            {162, C<&INpnsSystem::RequestChangeStateForceTimedWithCancelEvent>, "RequestChangeStateForceTimedWithCancelEvent"},
            {201, C<&INpnsSystem::RequestChangeStateForceTimed>, "RequestChangeStateForceTimed"},
            {202, C<&INpnsSystem::RequestChangeStateForceAsync>, "RequestChangeStateForceAsync"},
            {301, nullptr, "GetPassword"}, // 18.0.0+
            {302, nullptr, "GetAllImmigration"}, // 18.0.0+
            {303, nullptr, "GetNotificationHistories"}, // 18.0.0+
            {304, nullptr, "GetPersistentConnectionSummary"}, // 18.0.0+
            {305, nullptr, "GetDigitalTwinSummary"}, // 18.0.0+
            {306, nullptr, "GetDigitalTwinValue"}, // 18.0.0+
        };
        // clang-format on

        RegisterHandlers(functions);
    }

    ~INpnsSystem() override = default;

private:
    // Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): no push server delivers to
    // the HOME menu, so the receive and state-change events exist for it to wait on and never
    // signal; listen registrations are only recorded.
    // [OpenPak] The firmware's friends program puts "c-friends.inv.subsTypes" here at start-up;
    // there is no digital twin store, so the value is accepted and dropped.
    void OpenStubObject(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NPNS, "called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<INpnsStubObject>(ctx, system);
    }

    void PutDigitalTwinKeyValue(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NPNS, "called, digital twin value dropped");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    Result ListenAll() {
        LOG_DEBUG(Service_NPNS, "called, system listener registered for all programs");
        R_SUCCEED();
    }

    Result GetStateChangeEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_NPNS, "called");

        *out_event = get_state_change_event.GetHandle();
        R_SUCCEED();
    }

    // [OpenPak] The "with handover" forms take a u32 the firmware's friends program passes; the
    // state is the same one GetState answers and the events never fire.
    Result GetStateWithHandover(Out<u32> out_state, u32 handover) {
        LOG_DEBUG(Service_NPNS, "called, handover={}", handover);
        *out_state = 0;
        R_SUCCEED();
    }

    Result GetStateChangeEventWithHandover(OutCopyHandle<Kernel::KReadableEvent> out_event,
                                           u32 handover) {
        LOG_DEBUG(Service_NPNS, "called, handover={}", handover);
        *out_event = get_state_change_event.GetHandle();
        R_SUCCEED();
    }

    Result ListenToMyApplicationId() {
        LOG_DEBUG(Service_NPNS, "called, system listener registered for the caller's application id");
        R_SUCCEED();
    }

    Result ListenTo(u32 program_id) {
        LOG_WARNING(Service_NPNS, "(STUBBED) called, program_id={}", program_id);
        R_SUCCEED();
    }

    Result GetReceiveEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_WARNING(Service_NPNS, "(STUBBED) called");

        *out_event = get_receive_event.GetHandle();
        R_SUCCEED();
    }

    // The overlay applet registers its listener by name ("ntf") in a pointer (X) buffer; other
    // callers may use a map alias one, so the buffer is auto-selected. Recorded only: nothing is
    // delivered to listeners here (behaviour follows the Ryujinx Harbor).
    Result ListenToByName(InBuffer<BufferAttr_HipcAutoSelect> name_buffer) {
        const std::string name(reinterpret_cast<const char*>(name_buffer.data()),
                               strnlen(reinterpret_cast<const char*>(name_buffer.data()),
                                       name_buffer.size()));
        LOG_DEBUG(Service_NPNS, "called, system listener registered by name '{}'", name);
        R_SUCCEED();
    }

    Result GetState(Out<u32> out_state) {
        LOG_WARNING(Service_NPNS, "(STUBBED) called");
        *out_state = 0;
        R_SUCCEED();
    }

    Result GetLastNotifiedTime(Out<s64> out_last_notified_time) {
        LOG_WARNING(Service_NPNS, "(STUBBED) called");

        *out_last_notified_time = 0;
        R_SUCCEED();
    }

    Result GetRequestChangeStateCancelEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_NPNS, "(STUBBED) called.");

        // TODO (jarrodnorwell)

        *out_event = get_request_change_state_cancel_event.GetHandle();

        R_SUCCEED();
    }

    // [OpenPak] QLaunch's "Update" button on the Friends viewer calls these to force a push
    // state refresh; unimplemented, pressing it threw a fatal (2010-0212).
    Result RequestChangeStateForceTimedWithCancelEvent() {
        LOG_WARNING(Service_NPNS, "(STUBBED) called");
        R_SUCCEED();
    }

    Result RequestChangeStateForceTimed() {
        LOG_WARNING(Service_NPNS, "(STUBBED) called");
        R_SUCCEED();
    }

    Result RequestChangeStateForceAsync() {
        LOG_WARNING(Service_NPNS, "(STUBBED) called");
        R_SUCCEED();
    }

    KernelHelpers::ServiceContext service_context;
    Event get_receive_event;
    Event get_state_change_event;
    Event get_request_change_state_cancel_event;
};

class INpnsUser final : public ServiceFramework<INpnsUser> {
public:
    explicit INpnsUser(Core::System& system_)
        : ServiceFramework{system_, "npns:u"}, service_context{system, "npns:u"}, get_receive_event{service_context},
          get_state_change_event{service_context} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {1, C<&INpnsUser::ListenAll>, "ListenAll"},
            {2, C<&INpnsUser::ListenTo>, "ListenTo"},
            {3, nullptr, "Receive"},
            {4, nullptr, "ReceiveRaw"},
            {5, C<&INpnsUser::GetReceiveEvent>, "GetReceiveEvent"},
            {7, C<&INpnsUser::GetStateChangeEvent>, "GetStateChangeEvent"},
            {8, C<&INpnsUser::ListenToByName>, "ListenToByName"}, // 18.0.0+
            {21, C<&INpnsUser::CreateToken>, "CreateToken"},
            {23, C<&INpnsUser::DestroyToken>, "DestroyToken"},
            {25, C<&INpnsUser::QueryIsTokenValid>, "QueryIsTokenValid"},
            {26, C<&INpnsUser::ListenToMyApplicationId>, "ListenToMyApplicationId"},
            {101, C<&INpnsUser::Suspend>, "Suspend"},
            {102, C<&INpnsUser::Resume>, "Resume"},
            {103, C<&INpnsUser::GetState>, "GetState"},
            {104, nullptr, "GetStatistics"},
            {111, nullptr, "GetJid"},
            {120, nullptr, "CreateNotificationReceiver"},
            {151, nullptr, "GetStateWithHandover"},
            {152, nullptr, "GetStateChangeEventWithHandover"},
            {153, nullptr, "GetDropEventWithHandover"},
            {154, nullptr, "CreateTokenAsync"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    // Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): the subscribe calls succeed
    // and only record the intent; the receive and state-change events exist but never signal (no
    // push backend), so a title waiting on them falls back to polling its own servers.
    Result ListenAll() {
        LOG_DEBUG(Service_NPNS, "called, user listener registered for all programs");
        R_SUCCEED();
    }

    Result ListenTo(u64 program_id) {
        LOG_DEBUG(Service_NPNS, "called, program_id={:016X}", program_id);
        R_SUCCEED();
    }

    Result GetStateChangeEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_NPNS, "called");

        *out_event = get_state_change_event.GetHandle();
        R_SUCCEED();
    }

    Result DestroyToken(u64 unknown, ClientProcessId pid) {
        LOG_UNKNOWN_CONTRACT(Service_NPNS, "npns:u 23 DestroyToken(u64 {:#x}, pid {}): nothing stored, success",
                             unknown, pid.pid);
        R_SUCCEED();
    }

    Result QueryIsTokenValid(Out<bool> out_is_valid) {
        LOG_UNKNOWN_CONTRACT(Service_NPNS, "npns:u 25 QueryIsTokenValid: input not established, answered true");
        // The only tokens in circulation are the ones handed out here.
        *out_is_valid = true;
        R_SUCCEED();
    }

    Result ListenToMyApplicationId(u64 unknown, ClientProcessId pid) {
        LOG_UNKNOWN_CONTRACT(Service_NPNS, "npns:u 26 ListenToMyApplicationId(u64 {:#x}, pid {}): recorded only",
                             unknown, pid.pid);
        R_SUCCEED();
    }

    Result Suspend() {
        LOG_DEBUG(Service_NPNS, "called");
        R_SUCCEED();
    }

    Result Resume() {
        LOG_DEBUG(Service_NPNS, "called");
        R_SUCCEED();
    }

    Result ListenToByName(InBuffer<BufferAttr_HipcAutoSelect> name_buffer) {
        const std::string name(reinterpret_cast<const char*>(name_buffer.data()),
                               strnlen(reinterpret_cast<const char*>(name_buffer.data()),
                                       name_buffer.size()));
        LOG_DEBUG(Service_NPNS, "called, user listener registered by name '{}' (nothing is delivered)",
                  name);

        // Recorded only, as the Ryujinx Harbor does.
        R_SUCCEED();
    }

    Result GetReceiveEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_NPNS, "called");

        *out_event = get_receive_event.GetHandle();
        R_SUCCEED();
    }

    Result CreateToken() {
        LOG_WARNING(Service_NPNS, "(STUBBED) called");
        R_SUCCEED();
    }

    Result GetState(Out<u32> out_state) {
        LOG_WARNING(Service_NPNS, "(STUBBED) called");
        *out_state = 0;
        R_SUCCEED();
    }

    KernelHelpers::ServiceContext service_context;
    Event get_receive_event;
    Event get_state_change_event;
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    // [OpenPak] With the firmware's npns program on the real-firmware list, these register nothing
    // (server_manager leaves hosted names to it).
    server_manager->RegisterNamedService("npns:s", std::make_shared<INpnsSystem>(system));
    server_manager->RegisterNamedService("npns:u", std::make_shared<INpnsUser>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::NPNS
