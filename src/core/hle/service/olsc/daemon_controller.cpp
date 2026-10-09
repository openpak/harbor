// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/redact.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/olsc/daemon_controller.h"
#include "core/hle/service/olsc/transfer_runner.h"
#include "core/openpak/olsc.h"

namespace Service::OLSC {

IDaemonController::IDaemonController(Core::System& system_, std::shared_ptr<TransferQueue> queue_)
    : ServiceFramework{system_, "IDaemonController"}, queue{std::move(queue_)},
      settings{queue->Settings()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDaemonController::GetApplicationAutoTransferSetting>, "GetApplicationAutoTransferSetting"},
        {1, D<&IDaemonController::SetApplicationAutoTransferSetting>, "SetApplicationAutoTransferSetting"},
        {2, D<&IDaemonController::GetGlobalAutoUploadSetting>, "GetGlobalAutoUploadSetting"},
        {3, D<&IDaemonController::SetGlobalAutoUploadSetting>, "SetGlobalAutoUploadSetting"},
        {4, D<&IDaemonController::RunTransferTaskAutonomyRegistration>, "RunTransferTaskAutonomyRegistration"},
        {5, D<&IDaemonController::GetGlobalAutoDownloadSetting>, "GetGlobalAutoDownloadSetting"}, // 11.0.0+
        {6, D<&IDaemonController::SetGlobalAutoDownloadSetting>, "SetGlobalAutoDownloadSetting"}, // 11.0.0+
        {10, nullptr, "CreateForbiddenSaveDataInidication"},
        {11, D<&IDaemonController::StopAutonomyTaskExecution>, "StopAutonomyTaskExecution"},
        {12, D<&IDaemonController::GetAutonomyTaskStatus>, "GetAutonomyTaskStatus"},
        {13, nullptr, "Unknown13_20_0_0_Plus"}, // 20.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);
}

IDaemonController::~IDaemonController() = default;

Result IDaemonController::GetApplicationAutoTransferSetting(Out<bool> out_is_enabled,
                                                            Common::UUID user_id,
                                                            u64 application_id) {
    LOG_INFO(Service_OLSC, "called, user_id={} application_id={:016X}", Common::Redact(user_id),
             application_id);
    *out_is_enabled = settings->ApplicationAutoTransfer(user_id.RawString(), application_id);
    R_SUCCEED();
}

Result IDaemonController::SetApplicationAutoTransferSetting(
    SetApplicationSettingParameters params) {
    LOG_INFO(Service_OLSC, "called, user_id={} application_id={:016X} is_enabled={}",
             Common::Redact(params.user_id), params.application_id, params.is_enabled);
    settings->SetApplicationAutoTransfer(params.user_id.RawString(), params.application_id,
                                         params.is_enabled);
    if (!params.is_enabled) {
        // 0x1eff00: the title's queued automatic tasks go.
        queue->RemoveQueued(params.user_id, params.application_id, 0);
        queue->RemoveQueued(params.user_id, params.application_id, 1);
    }
    R_SUCCEED();
}

Result IDaemonController::GetGlobalAutoUploadSetting(Out<bool> out_is_enabled,
                                                     Common::UUID user_id) {
    LOG_INFO(Service_OLSC, "called, user_id={}", Common::Redact(user_id));
    *out_is_enabled = settings->GlobalAutoUpload(user_id.RawString());
    LOG_DEBUG(Service_OLSC, "global auto upload is {}", *out_is_enabled);
    R_SUCCEED();
}

Result IDaemonController::SetGlobalAutoUploadSetting(SetGlobalSettingParameters params) {
    LOG_INFO(Service_OLSC, "called, user_id={} is_enabled={}", Common::Redact(params.user_id),
             params.is_enabled);
    settings->SetGlobalAutoUpload(params.user_id.RawString(), params.is_enabled);
    if (!params.is_enabled) {
        queue->RemoveQueued(params.user_id, std::nullopt, 0); // 0x1aa5e0
    }
    R_SUCCEED();
}

Result IDaemonController::RunTransferTaskAutonomyRegistration(Common::UUID user_id,
                                                              u64 application_id) {
    LOG_INFO(Service_OLSC, "called, user_id={} application_id={:016X}", Common::Redact(user_id),
             application_id);
    // 0x1f0330: the user's periodic autonomy sweep is due now.
    queue->MakeDue(user_id);
    R_SUCCEED();
}

Result IDaemonController::GetGlobalAutoDownloadSetting(Out<bool> out_is_enabled,
                                                       Common::UUID user_id) {
    LOG_INFO(Service_OLSC, "called, user_id={}", Common::Redact(user_id));
    *out_is_enabled = settings->GlobalAutoDownload(user_id.RawString());
    LOG_DEBUG(Service_OLSC, "global auto download is {}", *out_is_enabled);
    R_SUCCEED();
}

Result IDaemonController::SetGlobalAutoDownloadSetting(SetGlobalSettingParameters params) {
    LOG_INFO(Service_OLSC, "called, user_id={} is_enabled={}", Common::Redact(params.user_id),
             params.is_enabled);
    settings->SetGlobalAutoDownload(params.user_id.RawString(), params.is_enabled);
    if (!params.is_enabled) {
        queue->RemoveQueued(params.user_id, std::nullopt, 1); // 0x1aa720
    }
    R_SUCCEED();
}

Result IDaemonController::StopAutonomyTaskExecution(Out<SharedPointer<IStopperObject>> out_stopper) {
    // While the stopper is open the autonomy agent registers nothing.
    LOG_DEBUG(Service_OLSC, "called");
    queue->HoldAutonomy(true);
    *out_stopper = std::make_shared<IStopperObject>(
        system, [queue = queue] { queue->HoldAutonomy(false); });
    R_SUCCEED();
}

Result IDaemonController::GetAutonomyTaskStatus(Out<u8> out_status, Common::UUID user_id) {
    LOG_INFO(Service_OLSC, "called, user_id={}", Common::Redact(user_id));

    *out_status = 0;
    R_SUCCEED();
}

} // namespace Service::OLSC
