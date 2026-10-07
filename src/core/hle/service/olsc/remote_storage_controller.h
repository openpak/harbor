// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/uuid.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Service::OLSC {

class IRemoteStorageController final : public ServiceFramework<IRemoteStorageController> {
public:
    explicit IRemoteStorageController(Core::System& system_, Common::UUID user_id_);
    ~IRemoteStorageController() override;

private:
    Result GetSecondarySave(Out<bool> out_has_secondary_save, Out<std::array<u64, 3>> out_unknown,
                            u64 application_id);
    Result GetDataNewnessByApplicationId(Out<u8> out_newness, u64 application_id);
    Result GetDataInfoV1(Out<std::array<u8, 0x30>> out_data, u64 application_id);
    Result GetDataInfo(Out<std::array<u8, 0x38>> out_data, u64 application_id);

    /// The user this controller speaks for, as the 32 hex digits the OpenPak layer names it by.
    std::string uid;
};

} // namespace Service::OLSC
