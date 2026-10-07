// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/uuid.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/olsc/stopper_object.h"
#include "core/hle/service/service.h"

namespace OpenPak::Olsc {
class DaemonSettings;
}

namespace Service::OLSC {

class IDaemonController final : public ServiceFramework<IDaemonController> {
public:
    explicit IDaemonController(Core::System& system_,
                               std::shared_ptr<OpenPak::Olsc::DaemonSettings> settings_);
    ~IDaemonController() override;

private:
    Result GetApplicationAutoTransferSetting(Out<bool> out_is_enabled, Common::UUID user_id,
                                             u64 application_id);
    Result SetApplicationAutoTransferSetting(bool is_enabled, Common::UUID user_id,
                                             u64 application_id);

    Result GetGlobalAutoUploadSetting(Out<bool> out_is_enabled, Common::UUID user_id);
    Result SetGlobalAutoUploadSetting(bool is_enabled, Common::UUID user_id);

    Result RunTransferTaskAutonomyRegistration(Common::UUID user_id, u64 application_id);

    Result GetGlobalAutoDownloadSetting(Out<bool> out_is_enabled, Common::UUID user_id);
    Result SetGlobalAutoDownloadSetting(bool is_enabled, Common::UUID user_id);

    Result StopAutonomyTaskExecution(Out<SharedPointer<IStopperObject>> out_stopper);

    Result GetAutonomyTaskStatus(Out<u8> out_status, Common::UUID user_id);

    // Kept across restarts, as the sysmodule keeps them in its system save.
    std::shared_ptr<OpenPak::Olsc::DaemonSettings> settings;
};

} // namespace Service::OLSC
