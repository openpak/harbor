// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>

#include "common/logging.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/erpt/erpt.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"

namespace Service::ERPT {

class ErrorReportContext final : public ServiceFramework<ErrorReportContext> {
public:
    explicit ErrorReportContext(Core::System& system_) : ServiceFramework{system_, "erpt:c"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, C<&ErrorReportContext::SubmitContext>, "SubmitContext"},
            {1, C<&ErrorReportContext::CreateReportV0>, "CreateReportV0"},
            {2, C<&ErrorReportContext::SetInitialLaunchSettingsCompletionTime>, "SetInitialLaunchSettingsCompletionTime"},
            {3, nullptr, "ClearInitialLaunchSettingsCompletionTime"},
            {4, nullptr, "UpdatePowerOnTime"},
            {5, D<&ErrorReportContext::UpdateAwakeTime>, "UpdateAwakeTime"},
            {6, C<&ErrorReportContext::SubmitMultipleContext>, "SubmitMultipleContext"},
            {7, C<&ErrorReportContext::RegisterRunningApplicationInfo>, "RegisterRunningApplicationInfo"},
            {8, C<&ErrorReportContext::UnregisterRunningApplicationInfo>, "UnregisterRunningApplicationInfo"},
            {9, nullptr, "SubmitAttachment"},
            {10, C<&ErrorReportContext::CreateReportWithAttachments>, "CreateReportWithAttachments"},
            {11, C<&ErrorReportContext::CreateReportV1>, "CreateReportV1"},
            {12, C<&ErrorReportContext::CreateReport>, "CreateReport"},
            {14, C<&ErrorReportContext::CreateReportWithSpecifiedReportId>, "CreateReportWithSpecifiedReportId"}, // 20.0.0+
            {20, C<&ErrorReportContext::RegisterRunningApplet>, "RegisterRunningApplet"},
            {21, C<&ErrorReportContext::UnregisterRunningApplet>, "UnregisterRunningApplet"},
            {22, C<&ErrorReportContext::UpdateAppletSuspendedDuration>, "UpdateAppletSuspendedDuration"},
            {30, C<&ErrorReportContext::InvalidateForcedShutdownDetection>, "InvalidateForcedShutdownDetection"},
            {40, C<&ErrorReportContext::WaitForReportCreation>, "WaitForReportCreation"}, // 21.0.0+
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    Result SubmitContext(InBuffer<BufferAttr_HipcMapAlias> context_entry,
                         InBuffer<BufferAttr_HipcMapAlias> field_list) {
        LOG_WARNING(Service_SET, "(STUBBED) called, context_entry_size={}, field_list_size={}",
                    context_entry.size(), field_list.size());
        R_SUCCEED();
    }

    Result CreateReportV0(u32 report_type, InBuffer<BufferAttr_HipcMapAlias> context_entry,
                          InBuffer<BufferAttr_HipcMapAlias> report_list,
                          InBuffer<BufferAttr_HipcMapAlias> report_meta_data) {
        LOG_WARNING(Service_SET, "(STUBBED) called, report_type={:#x}", report_type);
        R_SUCCEED();
    }

    Result CreateReportV1(u32 report_type, u32 unknown,
                          InBuffer<BufferAttr_HipcMapAlias> context_entry,
                          InBuffer<BufferAttr_HipcMapAlias> report_list,
                          InBuffer<BufferAttr_HipcMapAlias> report_meta_data) {
        LOG_WARNING(Service_SET, "(STUBBED) called, report_type={:#x}, unknown={:#x}", report_type,
                    unknown);
        R_SUCCEED();
    }

    Result CreateReport(u32 report_type, u32 unknown, u32 create_report_option_flag,
                        InBuffer<BufferAttr_HipcMapAlias> context_entry,
                        InBuffer<BufferAttr_HipcMapAlias> report_list,
                        InBuffer<BufferAttr_HipcMapAlias> report_meta_data) {
        LOG_WARNING(
            Service_SET,
            "(STUBBED) called, report_type={:#x}, unknown={:#x}, create_report_option_flag={:#x}",
            report_type, unknown, create_report_option_flag);
        R_SUCCEED();
    }

    // Error reports never leave this unit: the commands below are accepted and discarded, and no
    // report file is written. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
    Result SetInitialLaunchSettingsCompletionTime() {
        LOG_DEBUG(Service_SET, "initial launch settings completion time recorded (discarded)");
        R_SUCCEED();
    }

    Result SubmitMultipleContext() {
        LOG_DEBUG(Service_SET, "multiple error report contexts submitted (discarded)");
        R_SUCCEED();
    }

    Result RegisterRunningApplicationInfo() {
        LOG_DEBUG(Service_SET, "running application info registered (discarded)");
        R_SUCCEED();
    }

    Result UnregisterRunningApplicationInfo() {
        LOG_DEBUG(Service_SET, "running application info unregistered (discarded)");
        R_SUCCEED();
    }

    Result CreateReportWithAttachments() {
        LOG_DEBUG(Service_SET, "error report with attachments discarded");
        R_SUCCEED();
    }

    Result CreateReportWithSpecifiedReportId() {
        LOG_DEBUG(Service_SET, "error report discarded");
        R_SUCCEED();
    }

    Result RegisterRunningApplet(u32 applet_id) {
        LOG_DEBUG(Service_SET, "running applet {} registered (discarded)", applet_id);
        R_SUCCEED();
    }

    Result UnregisterRunningApplet(u32 applet_id) {
        LOG_DEBUG(Service_SET, "running applet {} unregistered (discarded)", applet_id);
        R_SUCCEED();
    }

    Result UpdateAppletSuspendedDuration() {
        LOG_DEBUG(Service_SET, "applet suspended duration updated (discarded)");
        R_SUCCEED();
    }

    Result InvalidateForcedShutdownDetection() {
        LOG_DEBUG(Service_SET, "forced shutdown detection invalidated (discarded)");
        R_SUCCEED();
    }

    Result WaitForReportCreation() {
        // Every report above is "created" at once; there is no output.
        LOG_DEBUG(Service_SET, "report creation waited for");
        R_SUCCEED();
    }

    Result UpdateAwakeTime(InBuffer<BufferAttr_HipcMapAlias> data_a,
                           InBuffer<BufferAttr_HipcMapAlias> data_b, u32 flag_a, u32 flag_b) {
        LOG_WARNING(Service_SET,
                    "(STUBBED) called, data_a_size={}, data_b_size={}, flag_a={}, flag_b={}",
                    data_a.size(), data_b.size(), flag_a, flag_b);
        R_SUCCEED();
    }
};

class ErrorReportSession final : public ServiceFramework<ErrorReportSession> {
public:
    explicit ErrorReportSession(Core::System& system_) : ServiceFramework{system_, "erpt:r"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "OpenReport"},
            {1, nullptr, "OpenManager"},
            {2, nullptr, "OpenAttachment"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("erpt:c", std::make_shared<ErrorReportContext>(system));
    server_manager->RegisterNamedService("erpt:r", std::make_shared<ErrorReportSession>(system));

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::ERPT
