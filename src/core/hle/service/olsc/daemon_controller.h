// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>

#include "common/common_funcs.h"
#include "common/uuid.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/olsc/stopper_object.h"
#include "core/hle/service/service.h"

namespace OpenPak::Olsc {
class DaemonSettings;
}

namespace Service::OLSC {

// The setters' input: the bool first, then the Uid at +8 (nn::account::Uid is 8-aligned; the
// 23.0.1 sysmodule reads 0x18 bytes for 3 and 6, 0x20 for 1 with the application id at +0x18).
// Common::UUID has an alignment of 1, so a plain (bool, UUID) argument list reads the Uid at +1.
struct SetGlobalSettingParameters {
    bool is_enabled;
    INSERT_PADDING_BYTES(7);
    Common::UUID user_id;
};
static_assert(sizeof(SetGlobalSettingParameters) == 0x18);
static_assert(offsetof(SetGlobalSettingParameters, user_id) == 0x8);

struct SetApplicationSettingParameters {
    bool is_enabled;
    INSERT_PADDING_BYTES(7);
    Common::UUID user_id;
    u64 application_id;
};
static_assert(sizeof(SetApplicationSettingParameters) == 0x20);
static_assert(offsetof(SetApplicationSettingParameters, application_id) == 0x18);

class IDaemonController final : public ServiceFramework<IDaemonController> {
public:
    explicit IDaemonController(Core::System& system_,
                               std::shared_ptr<OpenPak::Olsc::DaemonSettings> settings_);
    ~IDaemonController() override;

private:
    Result GetApplicationAutoTransferSetting(Out<bool> out_is_enabled, Common::UUID user_id,
                                             u64 application_id);
    Result SetApplicationAutoTransferSetting(SetApplicationSettingParameters params);

    Result GetGlobalAutoUploadSetting(Out<bool> out_is_enabled, Common::UUID user_id);
    Result SetGlobalAutoUploadSetting(SetGlobalSettingParameters params);

    Result RunTransferTaskAutonomyRegistration(Common::UUID user_id, u64 application_id);

    Result GetGlobalAutoDownloadSetting(Out<bool> out_is_enabled, Common::UUID user_id);
    Result SetGlobalAutoDownloadSetting(SetGlobalSettingParameters params);

    Result StopAutonomyTaskExecution(Out<SharedPointer<IStopperObject>> out_stopper);

    Result GetAutonomyTaskStatus(Out<u8> out_status, Common::UUID user_id);

    // Kept across restarts, as the sysmodule keeps them in its system save.
    std::shared_ptr<OpenPak::Olsc::DaemonSettings> settings;
};

} // namespace Service::OLSC
