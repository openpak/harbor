// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/redact.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nim/nim.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/session.h"

namespace Service::NIM {
class IShopServiceAsync final : public ServiceFramework<IShopServiceAsync> {
public:
    explicit IShopServiceAsync(Core::System& system_)
        : ServiceFramework{system_, "IShopServiceAsync"},
          service_context{system_, "IShopServiceAsync"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IShopServiceAsync::Cancel>, "Cancel"},
            {1, D<&IShopServiceAsync::GetSize>, "GetSize"},
            {2, D<&IShopServiceAsync::Read>, "Read"},
            {3, D<&IShopServiceAsync::GetErrorCode>, "GetErrorCode"},
            {4, D<&IShopServiceAsync::Request>, "Request"},
            {5, D<&IShopServiceAsync::Prepare>, "Prepare"},
        };
        // clang-format on

        RegisterHandlers(functions);

        completion_event = service_context.CreateEvent("IShopServiceAsync:Completion");
    }

    ~IShopServiceAsync() override {
        CancelImpl();
        service_context.CloseEvent(completion_event);
    }

    Kernel::KReadableEvent* GetEvent() const {
        return &completion_event->GetReadableEvent();
    }

private:
    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completion_event;

    std::jthread worker;
    std::atomic<u32> error_code{0};

    std::mutex data_mutex;
    std::vector<u8> download_data;

    void CancelImpl() {
        worker.request_stop();
        if (worker.joinable()) {
            worker.join();
        }
    }

    Result Cancel() {
        LOG_DEBUG(Service_NIM, "called");
        CancelImpl();
        R_SUCCEED();
    }

    Result GetSize(Out<u64> out_size) {
        LOG_DEBUG(Service_NIM, "called");
        std::scoped_lock lock{data_mutex};
        *out_size = download_data.size();
        R_SUCCEED();
    }

    Result Read(Out<u64> out_size, u64 offset, OutBuffer<BufferAttr_HipcAutoSelect> out_buffer) {
        std::scoped_lock lock{data_mutex};

        u64 actual_read = 0;
        if (offset < download_data.size()) {
            actual_read = std::min<u64>(out_buffer.size(), download_data.size() - offset);
            std::memcpy(out_buffer.data(), download_data.data() + offset, actual_read);
        }

        *out_size = actual_read;
        R_SUCCEED();
    }

    Result GetErrorCode(Out<u32> out_error_code) {
        LOG_DEBUG(Service_NIM, "called");
        *out_error_code = error_code.load();
        R_SUCCEED();
    }

    Result Request() {
        LOG_DEBUG(Service_NIM, "(STUBBED) called");
        CancelImpl();

        // [Harbor] Answered inline: there is no network call behind it, a caller already waiting
        // must not find the event clear, and a host thread must not signal a guest event itself.
        // `{}` is the smallest well-formed body; nn::ec hands what it reads to a JSON parser.
        {
            std::scoped_lock lock{data_mutex};
            const std::string dummy_response = "{}";
            download_data.assign(dummy_response.begin(), dummy_response.end());
        }
        error_code.store(0);
        completion_event->Signal(system.Kernel());

        R_SUCCEED();
    }

    Result Prepare(InArray<char, BufferAttr_HipcMapAlias> in_path, InArray<char, BufferAttr_HipcMapAlias> in_post) {
        LOG_DEBUG(Service_NIM, "called");
        if (!in_path.empty()) {
            std::string url(in_path.data(), in_path.size());
            LOG_INFO(Service_NIM, "Preparing request for URL: {}", Common::RedactUrl(url));
        }
        completion_event->Clear(system.Kernel());
        R_SUCCEED();
    }
};

class IShopServiceAccessor final : public ServiceFramework<IShopServiceAccessor> {
public:
    explicit IShopServiceAccessor(Core::System& system_)
        : ServiceFramework{system_, "IShopServiceAccessor"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IShopServiceAccessor::CreateAsyncInterface, "CreateAsyncInterface"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateAsyncInterface(HLERequestContext& ctx) {LOG_DEBUG(Service_NIM, "called");
        auto async_interface = std::make_shared<IShopServiceAsync>(system);

        IPC::ResponseBuilder rb{ctx, 2, 1, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, async_interface->GetEvent());
        rb.PushIpcInterface<IShopServiceAsync>(ctx, std::move(async_interface));
    }
};

class IShopServiceAccessServer final : public ServiceFramework<IShopServiceAccessServer> {
public:
    explicit IShopServiceAccessServer(Core::System& system_)
        : ServiceFramework{system_, "IShopServiceAccessServer"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IShopServiceAccessServer::CreateAccessorInterface, "CreateAccessorInterface"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateAccessorInterface(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIM, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IShopServiceAccessor>(ctx, system);
    }
};

class NIM final : public ServiceFramework<NIM> {
public:
    explicit NIM(Core::System& system_) : ServiceFramework{system_, "nim"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "CreateSystemUpdateTask"},
            {1, nullptr, "DestroySystemUpdateTask"},
            {2, nullptr, "ListSystemUpdateTask"},
            {3, nullptr, "RequestSystemUpdateTaskRun"},
            {4, nullptr, "GetSystemUpdateTaskInfo"},
            {5, nullptr, "CommitSystemUpdateTask"},
            {6, nullptr, "CreateNetworkInstallTask"},
            {7, nullptr, "DestroyNetworkInstallTask"},
            {8, nullptr, "ListNetworkInstallTask"},
            {9, nullptr, "RequestNetworkInstallTaskRun"},
            {10, nullptr, "GetNetworkInstallTaskInfo"},
            {11, nullptr, "CommitNetworkInstallTask"},
            {12, nullptr, "RequestLatestSystemUpdateMeta"},
            {14, nullptr, "ListApplicationNetworkInstallTask"},
            {15, nullptr, "ListNetworkInstallTaskContentMeta"},
            {16, nullptr, "RequestLatestVersion"},
            {17, nullptr, "SetNetworkInstallTaskAttribute"},
            {18, nullptr, "AddNetworkInstallTaskContentMeta"},
            {19, nullptr, "GetDownloadedSystemDataPath"},
            {20, nullptr, "CalculateNetworkInstallTaskRequiredSize"},
            {21, nullptr, "IsExFatDriverIncluded"},
            {22, nullptr, "GetBackgroundDownloadStressTaskInfo"},
            {23, nullptr, "RequestDeviceAuthenticationToken"},
            {24, nullptr, "RequestGameCardRegistrationStatus"},
            {25, nullptr, "RequestRegisterGameCard"},
            {26, nullptr, "RequestRegisterNotificationToken"},
            {27, nullptr, "RequestDownloadTaskList"},
            {28, nullptr, "RequestApplicationControl"},
            {29, nullptr, "RequestLatestApplicationControl"},
            {30, nullptr, "RequestVersionList"},
            {31, nullptr, "CreateApplyDeltaTask"},
            {32, nullptr, "DestroyApplyDeltaTask"},
            {33, nullptr, "ListApplicationApplyDeltaTask"},
            {34, nullptr, "RequestApplyDeltaTaskRun"},
            {35, nullptr, "GetApplyDeltaTaskInfo"},
            {36, nullptr, "ListApplyDeltaTask"},
            {37, nullptr, "CommitApplyDeltaTask"},
            {38, nullptr, "CalculateApplyDeltaTaskRequiredSize"},
            {39, nullptr, "PrepareShutdown"},
            {40, nullptr, "ListApplyDeltaTask"},
            {41, nullptr, "ClearNotEnoughSpaceStateOfApplyDeltaTask"},
            {42, nullptr, "CreateApplyDeltaTaskFromDownloadTask"},
            {43, nullptr, "GetBackgroundApplyDeltaStressTaskInfo"},
            {44, nullptr, "GetApplyDeltaTaskRequiredStorage"},
            {45, nullptr, "CalculateNetworkInstallTaskContentsSize"},
            {46, nullptr, "PrepareShutdownForSystemUpdate"},
            {47, nullptr, "FindMaxRequiredApplicationVersionOfTask"},
            {48, nullptr, "CommitNetworkInstallTaskPartially"},
            {49, nullptr, "ListNetworkInstallTaskCommittedContentMeta"},
            {50, nullptr, "ListNetworkInstallTaskNotCommittedContentMeta"},
            {51, nullptr, "FindMaxRequiredSystemVersionOfTask"},
            {52, nullptr, "GetNetworkInstallTaskErrorContext"},
            {53, nullptr, "CreateLocalCommunicationReceiveApplicationTask"},
            {54, nullptr, "DestroyLocalCommunicationReceiveApplicationTask"},
            {55, nullptr, "ListLocalCommunicationReceiveApplicationTask"},
            {56, nullptr, "RequestLocalCommunicationReceiveApplicationTaskRun"},
            {57, nullptr, "GetLocalCommunicationReceiveApplicationTaskInfo"},
            {58, nullptr, "CommitLocalCommunicationReceiveApplicationTask"},
            {59, nullptr, "ListLocalCommunicationReceiveApplicationTaskContentMeta"},
            {60, nullptr, "CreateLocalCommunicationSendApplicationTask"},
            {61, nullptr, "RequestLocalCommunicationSendApplicationTaskRun"},
            {62, nullptr, "GetLocalCommunicationReceiveApplicationTaskErrorContext"},
            {63, nullptr, "GetLocalCommunicationSendApplicationTaskInfo"},
            {64, nullptr, "DestroyLocalCommunicationSendApplicationTask"},
            {65, nullptr, "GetLocalCommunicationSendApplicationTaskErrorContext"},
            {66, nullptr, "CalculateLocalCommunicationReceiveApplicationTaskRequiredSize"},
            {67, nullptr, "ListApplicationLocalCommunicationReceiveApplicationTask"},
            {68, nullptr, "ListApplicationLocalCommunicationSendApplicationTask"},
            {69, nullptr, "CreateLocalCommunicationReceiveSystemUpdateTask"},
            {70, nullptr, "DestroyLocalCommunicationReceiveSystemUpdateTask"},
            {71, nullptr, "ListLocalCommunicationReceiveSystemUpdateTask"},
            {72, nullptr, "RequestLocalCommunicationReceiveSystemUpdateTaskRun"},
            {73, nullptr, "GetLocalCommunicationReceiveSystemUpdateTaskInfo"},
            {74, nullptr, "CommitLocalCommunicationReceiveSystemUpdateTask"},
            {75, nullptr, "GetLocalCommunicationReceiveSystemUpdateTaskErrorContext"},
            {76, nullptr, "CreateLocalCommunicationSendSystemUpdateTask"},
            {77, nullptr, "RequestLocalCommunicationSendSystemUpdateTaskRun"},
            {78, nullptr, "GetLocalCommunicationSendSystemUpdateTaskInfo"},
            {79, nullptr, "DestroyLocalCommunicationSendSystemUpdateTask"},
            {80, nullptr, "GetLocalCommunicationSendSystemUpdateTaskErrorContext"},
            {81, nullptr, "ListLocalCommunicationSendSystemUpdateTask"},
            {82, nullptr, "GetReceivedSystemDataPath"},
            {83, nullptr, "CalculateApplyDeltaTaskOccupiedSize"},
            {84, nullptr, "ReloadErrorSimulation"},
            {85, nullptr, "ListNetworkInstallTaskContentMetaFromInstallMeta"},
            {86, nullptr, "ListNetworkInstallTaskOccupiedSize"},
            {87, nullptr, "RequestQueryAvailableELicenses"},
            {88, nullptr, "RequestAssignELicenses"},
            {89, nullptr, "RequestExtendELicenses"},
            {90, nullptr, "RequestSyncELicenses"},
            {91, nullptr, "Unknown91"}, //6.0.0-14.1.2
            {92, nullptr, "Unknown92"}, //21.0.0+
            {93, nullptr, "RequestReportActiveELicenses"},
            {94, nullptr, "RequestReportActiveELicensesPassively"},
            {95, nullptr, "RequestRegisterDynamicRightsNotificationToken"},
            {96, nullptr, "RequestAssignAllDeviceLinkedELicenses"},
            {97, nullptr, "RequestRevokeAllELicenses"},
            {98, nullptr, "RequestPrefetchForDynamicRights"},
            {99, nullptr, "CreateNetworkInstallTask"},
            {100, nullptr, "ListNetworkInstallTaskRightsIds"},
            {101, nullptr, "RequestDownloadETickets"},
            {102, nullptr, "RequestQueryDownloadableContents"},
            {103, nullptr, "DeleteNetworkInstallTaskContentMeta"},
            {104, nullptr, "RequestIssueEdgeTokenForDebug"},
            {105, nullptr, "RequestQueryAvailableELicenses2"},
            {106, nullptr, "RequestAssignELicenses2"},
            {107, nullptr, "GetNetworkInstallTaskStateCounter"},
            {108, nullptr, "InvalidateDynamicRightsNaIdTokenCacheForDebug"},
            {109, nullptr, "ListNetworkInstallTaskPartialInstallContentMeta"},
            {110, nullptr, "ListNetworkInstallTaskRightsIdsFromIndex"},
            {111, nullptr, "AddNetworkInstallTaskContentMetaForUser"},
            {112, nullptr, "RequestAssignELicensesAndDownloadETickets"},
            {113, nullptr, "RequestQueryAvailableCommonELicenses"},
            {114, nullptr, "SetNetworkInstallTaskExtendedAttribute"},
            {115, nullptr, "GetNetworkInstallTaskExtendedAttribute"},
            {116, nullptr, "GetAllocatorInfo"},
            {117, nullptr, "RequestQueryDownloadableContentsByApplicationId"},
            {118, nullptr, "MarkNoDownloadRightsErrorResolved"},
            {119, nullptr, "GetApplyDeltaTaskAllAppliedContentMeta"},
            {120, nullptr, "PrioritizeNetworkInstallTask"},
            {121, nullptr, "RequestQueryAvailableCommonELicenses2"},
            {122, nullptr, "RequestAssignCommonELicenses"},
            {123, nullptr, "RequestAssignCommonELicenses2"},
            {124, nullptr, "IsNetworkInstallTaskFrontOfQueue"},
            {125, nullptr, "PrioritizeApplyDeltaTask"},
            {126, nullptr, "RerouteDownloadingPatch"},
            {127, nullptr, "UnmarkNoDownloadRightsErrorResolved"},
            {128, nullptr, "RequestContentsSize"},
            {129, nullptr, "RequestContentsAuthorizationToken"},
            {130, nullptr, "RequestCdnVendorDiscovery"},
            {131, nullptr, "RefreshDebugAvailability"},
            {132, nullptr, "ClearResponseSimulationEntry"},
            {133, nullptr, "RegisterResponseSimulationEntry"},
            {134, nullptr, "GetProcessedCdnVendors"},
            {135, nullptr, "RefreshRuntimeBehaviorsForDebug"},
            {136, nullptr, "RequestOnlineSubscriptionFreeTrialAvailability"},
            {137, nullptr, "GetNetworkInstallTaskContentMetaCount"},
            {138, nullptr, "RequestRevokeELicenses"},
            {139, nullptr, "EnableNetworkConnectionToUseApplicationCore"},
            {140, nullptr, "DisableNetworkConnectionToUseApplicationCore"},
            {141, nullptr, "IsNetworkConnectionEnabledToUseApplicationCore"},
            {142, nullptr, "RequestCheckSafeSystemVersion"},
            {143, nullptr, "RequestApplicationIcon"},
            {144, nullptr, "RequestDownloadIdbeIconFile"},
            {147, nullptr, "Unknown147"}, //18.0.0+
            {148, nullptr, "Unknown148"}, //18.0.0+
            {150, nullptr, "Unknown150"}, //19.0.0+
            {151, nullptr, "Unknown151"}, //20.0.0+
            {152, nullptr, "Unknown152"}, //20.0.0+
            {153, nullptr, "Unknown153"}, //20.0.0+
            {154, nullptr, "Unknown154"}, //20.0.0+
            {155, nullptr, "Unknown155"}, //20.0.0+
            {156, nullptr, "Unknown156"}, //20.0.0+
            {157, nullptr, "Unknown157"}, //20.0.0+
            {158, nullptr, "Unknown158"}, //20.0.0+
            {159, nullptr, "Unknown159"}, //20.0.0+
            {160, nullptr, "Unknown160"}, //20.0.0+
            {161, nullptr, "Unknown161"}, //20.0.0+
            {162, nullptr, "Unknown162"}, //20.0.0+
            {163, nullptr, "Unknown163"}, //20.0.0+
            {164, nullptr, "Unknown164"}, //20.0.0+
            {165, nullptr, "Unknown165"}, //20.0.0+
            {166, nullptr, "Unknown166"}, //20.0.0+
            {167, nullptr, "Unknown167"}, //20.0.0+
            {168, nullptr, "Unknown168"}, //20.0.0+
            {169, nullptr, "Unknown169"}, //20.0.0+
            {170, nullptr, "Unknown170"}, //20.0.0+
            {171, nullptr, "Unknown171"}, //20.0.0+
            {172, nullptr, "Unknown172"}, //20.0.0+
            {173, nullptr, "Unknown173"}, //20.0.0+
            {174, nullptr, "Unknown174"}, //20.0.0+
            {175, nullptr, "Unknown175"}, //20.0.0+
            {176, nullptr, "Unknown176"}, //20.0.0+
            {177, nullptr, "Unknown177"}, //20.0.0+
            {2000, nullptr, "Unknown2000"}, //20.0.0+
            {2001, nullptr, "Unknown2001"}, //20.0.0+
            {2002, nullptr, "Unknown2002"}, //20.0.0+
            {2003, nullptr, "Unknown2003"}, //20.0.0+
            {2004, nullptr, "Unknown2004"}, //20.0.0+
            {2007, nullptr, "Unknown2007"}, //20.0.0+
            {2011, nullptr, "Unknown2011"}, //20.0.0+
            {2012, nullptr, "Unknown2012"}, //20.0.0+
            {2013, nullptr, "Unknown2013"}, //20.0.0+
            {2014, nullptr, "Unknown2014"}, //20.0.0+
            {2015, nullptr, "Unknown2015"}, //20.0.0+
            {2016, nullptr, "Unknown2016"}, //20.0.0+
            {2017, nullptr, "Unknown2017"}, //20.0.0+
            {2018, nullptr, "Unknown2018"}, //20.0.0+
            {2019, nullptr, "Unknown2019"}, //20.0.0+
            {2020, nullptr, "Unknown2020"}, //20.0.0+
            {2021, nullptr, "Unknown2021"}, //20.0.0+
            {2022, nullptr, "Unknown2022"}, //20.0.0+
            {2023, nullptr, "Unknown2023"}, //20.0.0+
            {2024, nullptr, "Unknown2024"}, //20.0.0+
            {2025, nullptr, "Unknown2025"}, //20.0.0+
            {2026, nullptr, "Unknown2026"}, //20.0.0+
            {2027, nullptr, "Unknown2027"}, //20.0.0+
            {2028, nullptr, "Unknown2028"}, //20.0.0+
            {2029, nullptr, "Unknown2029"}, //20.0.0+
            {2030, nullptr, "Unknown2030"}, //20.0.0+
            {2031, nullptr, "Unknown2031"}, //20.0.0+
            {2032, nullptr, "Unknown2032"}, //20.0.0+
            {2033, nullptr, "Unknown2033"}, //20.0.0+
            {2034, nullptr, "Unknown2034"}, //20.0.0+
            {2035, nullptr, "Unknown2035"}, //20.0.0+
            {2036, nullptr, "Unknown2036"}, //20.0.0+
            {2037, nullptr, "Unknown2037"}, //20.0.0+
            {2038, nullptr, "Unknown2038"}, //20.0.0+
            {2039, nullptr, "Unknown2039"}, //20.0.0+
            {2040, nullptr, "Unknown2040"}, //20.0.0+
            {2041, nullptr, "Unknown2041"}, //20.0.0+
            {2042, nullptr, "Unknown2042"}, //20.0.0+
            {2043, nullptr, "Unknown2043"}, //20.0.0+
            {2044, nullptr, "Unknown2044"}, //20.0.0+
            {2045, nullptr, "Unknown2045"}, //20.0.0+
            {2046, nullptr, "Unknown2046"}, //20.0.0+
            {2047, nullptr, "Unknown2047"}, //20.0.0+
            {2048, nullptr, "Unknown2048"}, //20.0.0+
            {2049, nullptr, "Unknown2049"}, //20.0.0+
            {2050, nullptr, "Unknown2050"}, //20.0.0+
            {2051, nullptr, "Unknown2051"}, //20.0.0+
            {3000, nullptr, "RequestLatestApplicationIcon"}, //17.0.0+
            {3001, nullptr, "RequestDownloadIdbeLatestIconFile"}, //17.0.0+
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class NIM_ECA final : public ServiceFramework<NIM_ECA> {
public:
    explicit NIM_ECA(Core::System& system_) : ServiceFramework{system_, "nim:eca"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &NIM_ECA::CreateServerInterface, "CreateServerInterface"},
            {1, nullptr, "RefreshDebugAvailability"},
            {2, nullptr, "ClearDebugResponse"},
            {3, nullptr, "RegisterDebugResponse"},
            {4, &NIM_ECA::IsLargeResourceAvailable, "IsLargeResourceAvailable"},
            {5, &NIM_ECA::CreateServerInterface2, "CreateServerInterface2"} // 17.0.0+
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateServerInterface(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIM, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IShopServiceAccessServer>(ctx, system);
    }

    void IsLargeResourceAvailable(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};

        const auto unknown{rp.Pop<u64>()};

        LOG_INFO(Service_NIM, "(STUBBED) called, unknown={}", unknown);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void CreateServerInterface2(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIM, "(STUBBED) called.");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IShopServiceAccessServer>(ctx, system);
    }
};

// [Harbor] nn::nim::detail::IAsyncValue: 0 GetSize, 1 Get, 2 Cancel, 3 GetErrorContext. systemWeb
// waits on the event, then Get, GetErrorContext only when Get failed, and Cancel after either.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
class IAsyncValue final : public ServiceFramework<IAsyncValue> {
public:
    explicit IAsyncValue(Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker_,
                         OpenPak::AsyncWorker::Work work)
        : ServiceFramework{system_, "IAsyncValue"}, service_context{system_, "nim:IAsyncValue"},
          worker{std::move(worker_)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IAsyncValue::GetSize, "GetSize"},
            {1, &IAsyncValue::Get, "Get"},
            {2, &IAsyncValue::Cancel, "Cancel"},
            {3, &IAsyncValue::GetErrorContext, "GetErrorContext"},
        };
        // clang-format on
        RegisterHandlers(functions);

        completion_event = service_context.CreateEvent("nim:IAsyncValue:Completion");
        // The event is signalled however the work ends, so a Cancel followed by a wait
        // (systemWeb 0x08669a90) always returns.
        task = worker->Run(completion_event, ResultTokenRequestFailed, std::move(work));
    }

    ~IAsyncValue() override {
        task->Detach();
        service_context.CloseEvent(completion_event);
    }

    Kernel::KReadableEvent& GetEvent() const {
        return completion_event->GetReadableEvent();
    }

    // The one result nim 22.5.0 itself puts on a failed dauth:0 token request
    // (0x11a858..0x11a870); systemWeb maps it to its own error before the page sees it.
    static constexpr Result ResultTokenRequestFailed{ErrorModule::NIM, 70};

private:
    void GetSize(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<u64>(task->IsDone() ? task->Value().size() : 0);
    }

    void Get(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        if (!task->IsDone()) {
            // systemWeb asks only after the event; an IPC handler never waits on the network.
            LOG_UNKNOWN_CONTRACT(Service_NIM, "nim IAsyncValue Get before the value is ready, "
                                              "answered as a failed token request");
            rb.Push(ResultTokenRequestFailed);
            return;
        }
        const Result result = task->GetResult();
        const std::vector<u8> value = task->Value();
        if (result.IsError() || value.empty() || !ctx.CanWriteBuffer() ||
            value.size() > ctx.GetWriteBufferSize()) {
            rb.Push(result.IsError() ? result : ResultTokenRequestFailed);
            return;
        }
        ctx.WriteBuffer(value);
        rb.Push(ResultSuccess);
    }

    void Cancel(HLERequestContext& ctx) {
        task->Cancel();
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetErrorContext(HLERequestContext& ctx) {
        // No context: the failure is the result code alone. systemWeb asserts if this command
        // fails (0x08669b70).
        if (ctx.CanWriteBuffer()) {
            std::vector<u8> zeros(ctx.GetWriteBufferSize());
            ctx.WriteBuffer(zeros);
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completion_event;
    std::shared_ptr<OpenPak::AsyncWorker> worker;
    std::shared_ptr<OpenPak::AsyncTask> task;
};

class NIM_SHP final : public ServiceFramework<NIM_SHP> {
public:
    explicit NIM_SHP(Core::System& system_)
        : ServiceFramework{system_, "nim:shp"},
          worker{std::make_shared<OpenPak::AsyncWorker>(system_, "nim:OpenPak")} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "RequestDeviceAuthenticationToken"},
            {1, &NIM_SHP::RequestCachedDeviceAuthenticationToken, "RequestCachedDeviceAuthenticationToken"},
            {2, nullptr, "RequestEdgeToken"},
            {3, &NIM_SHP::RequestCachedEdgeToken, "RequestCachedEdgeToken"},
            {100, nullptr, "RequestRegisterDeviceAccount"},
            {101, nullptr, "RequestUnregisterDeviceAccount"},
            {102, nullptr, "RequestDeviceAccountStatus"},
            {103, nullptr, "GetDeviceAccountInfo"},
            {104, nullptr, "RequestDeviceRegistrationInfo"},
            {105, nullptr, "RequestTransferDeviceAccount"},
            {106, nullptr, "RequestSyncRegistration"},
            {107, nullptr, "IsOwnDeviceId"},
            {200, nullptr, "RequestRegisterNotificationToken"},
            {300, nullptr, "RequestUnlinkDevice"},
            {301, nullptr, "RequestUnlinkDeviceIntegrated"},
            {302, nullptr, "RequestLinkDevice"},
            {303, nullptr, "HasDeviceLink"},
            {304, nullptr, "RequestUnlinkDeviceAll"},
            {305, nullptr, "RequestCreateVirtualAccount"},
            {306, nullptr, "RequestDeviceLinkStatus"},
            {400, nullptr, "GetAccountByVirtualAccount"},
            {401, nullptr, "GetVirtualAccount"},
            {500, nullptr, "RequestSyncTicketLegacy"},
            {501, nullptr, "RequestDownloadTicket"},
            {502, nullptr, "RequestDownloadTicketForPrepurchasedContents"},
            {503, nullptr, "RequestSyncTicket"},
            {504, nullptr, "RequestDownloadTicketForPrepurchasedContents2"},
            {505, nullptr, "RequestDownloadTicketForPrepurchasedContentsForAccount"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    // The dauth client NIM asks dauth:0 for both tokens with, compiled into nim 22.5.0
    // (0x11a82c, 0x11a8ec).
    static constexpr const char* ShopClientId = "93af0acb26258de9";

    // 1 and 3 take an s64, how long the token must still be valid (systemWeb sends 3 hours).
    // 23.0.1 NIM keeps the last token of each kind and hands it out again while its expiry minus
    // now is at least that long, else it asks dauth:0 for a new one (task 0x08621CC0; nim:shp
    // method infos 0x086DBA9E, 0x086DBAE2: 8 raw bytes in, an object and a copy handle out). The
    // unit is taken to be nn::TimeSpan's nanoseconds. The longest token accepted is NIM's own
    // dauth:0 buffer less its NUL (0x801 and 0x401 bytes).
    void RequestCachedDeviceAuthenticationToken(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        RequestToken(ctx, device_token, "", 0x800, rp.Pop<s64>());
    }

    void RequestCachedEdgeToken(HLERequestContext& ctx) {
        // NIM passes vendor index 0 to dauth:0; that index 0 is the module's "akamai" (beside
        // "fastly") is inferred from the string order, not traced.
        IPC::RequestParser rp{ctx};
        RequestToken(ctx, edge_token, "akamai", 0x400, rp.Pop<s64>());
    }

    struct CachedToken {
        std::mutex mutex;
        std::string token;
        s64 expires_at{}; // Unix seconds, from the token's own exp.
    };

    // When a token expires, in Unix seconds; 0 when it cannot be read. The device token is a JWT
    // (the payload's exp); the edge token is a CDN token whose "~"-separated fields hold exp=.
    static s64 TokenExpiry(const std::string& token) {
        for (std::size_t start = 0; start < token.size();) {
            const auto end = std::min(token.find('~', start), token.size());
            const std::string_view field{token.data() + start, end - start};
            if (field.starts_with("exp=")) {
                s64 exp = 0;
                for (const char c : field.substr(4)) {
                    if (c < '0' || c > '9') {
                        return 0;
                    }
                    exp = exp * 10 + (c - '0');
                }
                return exp;
            }
            start = end + 1;
        }
        const auto first = token.find('.');
        const auto second = token.find('.', first == std::string::npos ? first : first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            return 0;
        }
        std::string payload;
        u32 bits = 0;
        int count = 0;
        for (const char c : std::string_view{token}.substr(first + 1, second - first - 1)) {
            int v;
            if (c >= 'A' && c <= 'Z') {
                v = c - 'A';
            } else if (c >= 'a' && c <= 'z') {
                v = c - 'a' + 26;
            } else if (c >= '0' && c <= '9') {
                v = c - '0' + 52;
            } else if (c == '-' || c == '+') {
                v = 62;
            } else if (c == '_' || c == '/') {
                v = 63;
            } else {
                break;
            }
            bits = (bits << 6) | static_cast<u32>(v);
            count += 6;
            if (count >= 8) {
                count -= 8;
                payload.push_back(static_cast<char>((bits >> count) & 0xFF));
            }
        }
        const auto json = nlohmann::json::parse(payload, nullptr, false);
        if (!json.is_object() || !json.contains("exp") || !json["exp"].is_number_integer()) {
            return 0;
        }
        return json["exp"].get<s64>();
    }

    void RequestToken(HLERequestContext& ctx, std::shared_ptr<CachedToken> cache,
                      std::string vendor, std::size_t max_length, s64 min_validity_ns) {
        LOG_DEBUG(Service_NIM, "called, {}, valid for at least {} s",
                  vendor.empty() ? "device token" : "edge token", min_validity_ns / 1'000'000'000);
        auto value = std::make_shared<IAsyncValue>(
            system, worker,
            [cache, vendor, max_length, min_validity_ns](std::stop_token,
                                                         std::vector<u8>& out) -> Result {
                const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
                std::string token;
                {
                    std::scoped_lock lock{cache->mutex};
                    if (!cache->token.empty() &&
                        (cache->expires_at - now) >= min_validity_ns / 1'000'000'000) {
                        token = cache->token;
                    }
                }
                if (token.empty()) {
                    if (!OpenPak::Session::Enabled()) {
                        LOG_WARNING(Service_NIM, "nim:shp token: no OpenPak server configured");
                        return IAsyncValue::ResultTokenRequestFailed;
                    }
                    token = OpenPak::Session::DauthToken(ShopClientId, vendor);
                    if (token.empty() || token.size() > max_length) {
                        LOG_WARNING(Service_NIM,
                                    "nim:shp token request failed ({} bytes, {} taken)",
                                    token.size(), max_length);
                        return IAsyncValue::ResultTokenRequestFailed;
                    }
                    std::scoped_lock lock{cache->mutex};
                    cache->token = token;
                    cache->expires_at = TokenExpiry(token);
                    LOG_INFO(Service_NIM, "nim:shp {} token issued, valid {} s",
                             vendor.empty() ? "device" : "edge",
                             cache->expires_at > now ? cache->expires_at - now : 0);
                } else {
                    LOG_DEBUG(Service_NIM, "nim:shp cached {} token handed out",
                              vendor.empty() ? "device" : "edge");
                }
                out.assign(token.begin(), token.end());
                out.push_back(0);
                return ResultSuccess;
            });

        IPC::ResponseBuilder rb{ctx, 2, 1, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, value->GetEvent());
        rb.PushIpcInterface<IAsyncValue>(ctx, std::move(value));
    }

    std::shared_ptr<CachedToken> device_token{std::make_shared<CachedToken>()};
    std::shared_ptr<CachedToken> edge_token{std::make_shared<CachedToken>()};
    std::shared_ptr<OpenPak::AsyncWorker> worker;
};

class IEnsureNetworkClockAvailabilityService final
    : public ServiceFramework<IEnsureNetworkClockAvailabilityService> {
public:
    explicit IEnsureNetworkClockAvailabilityService(Core::System& system_)
        : ServiceFramework{system_, "IEnsureNetworkClockAvailabilityService"},
          service_context{system_, "IEnsureNetworkClockAvailabilityService"} {
        static const FunctionInfo functions[] = {
            {0, &IEnsureNetworkClockAvailabilityService::StartTask, "StartTask"},
            {1, &IEnsureNetworkClockAvailabilityService::GetFinishNotificationEvent,
             "GetFinishNotificationEvent"},
            {2, &IEnsureNetworkClockAvailabilityService::GetResult, "GetResult"},
            {3, &IEnsureNetworkClockAvailabilityService::Cancel, "Cancel"},
            {4, &IEnsureNetworkClockAvailabilityService::IsProcessing, "IsProcessing"},
            {5, &IEnsureNetworkClockAvailabilityService::GetServerTime, "GetServerTime"},
        };
        RegisterHandlers(functions);

        finished_event =
            service_context.CreateEvent("IEnsureNetworkClockAvailabilityService:FinishEvent");
    }

    ~IEnsureNetworkClockAvailabilityService() override {
        service_context.CloseEvent(finished_event);
    }

private:
    void StartTask(HLERequestContext& ctx) {
        // No need to connect to the internet, just finish the task straight away.
        LOG_DEBUG(Service_NIM, "called");
        finished_event->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetFinishNotificationEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIM, "called");

        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, finished_event->GetReadableEvent());
    }

    void GetResult(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIM, "called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Cancel(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIM, "called");
        finished_event->Clear(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsProcessing(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIM, "called");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushRaw<u32>(0); // We instantly process the request
    }

    void GetServerTime(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIM, "called");

        const s64 server_time{std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count()};
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.PushRaw<s64>(server_time);
    }

    KernelHelpers::ServiceContext service_context;

    Kernel::KEvent* finished_event;
};

class NTC final : public ServiceFramework<NTC> {
public:
    explicit NTC(Core::System& system_) : ServiceFramework{system_, "ntc"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &NTC::OpenEnsureNetworkClockAvailabilityService, "OpenEnsureNetworkClockAvailabilityService"},
            {100, &NTC::SuspendAutonomicTimeCorrection, "SuspendAutonomicTimeCorrection"},
            {101, &NTC::ResumeAutonomicTimeCorrection, "ResumeAutonomicTimeCorrection"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void OpenEnsureNetworkClockAvailabilityService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIM, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IEnsureNetworkClockAvailabilityService>(ctx, system);
    }

    // TODO(ogniK): Do we need these?
    void SuspendAutonomicTimeCorrection(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ResumeAutonomicTimeCorrection(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

class NIM_ECAS final : public ServiceFramework<NIM_ECAS> {
public:
    explicit NIM_ECAS(Core::System& system_) : ServiceFramework{system_, "nim:ecas"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "RegisterSpecialClient"},
            {1, nullptr, "UnregisterSpecialClient"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("nim", std::make_shared<NIM>(system));
    server_manager->RegisterNamedService("nim:eca", std::make_shared<NIM_ECA>(system));
    server_manager->RegisterNamedService("nim:ecas", std::make_shared<NIM_ECAS>(system));
    server_manager->RegisterNamedService("nim:shp", std::make_shared<NIM_SHP>(system));
    server_manager->RegisterNamedService("ntc", std::make_shared<NTC>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::NIM
