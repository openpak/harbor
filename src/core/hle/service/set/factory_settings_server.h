// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"
#include "core/hle/service/set/settings_types.h"

namespace Core {
class System;
}

namespace Service::Set {

// set:cal answers from this NAND's calibration binary (core/harbor_calibration), the way a
// console's answers from its PRODINFO partition.
class IFactorySettingsServer final : public ServiceFramework<IFactorySettingsServer> {
public:
    explicit IFactorySettingsServer(Core::System& system_);
    ~IFactorySettingsServer() override;

private:
    using Vector3 = std::array<u8, 6>;

    Result GetBluetoothBdAddress(Out<std::array<u8, 6>> out_address);
    Result GetConfigurationId1(Out<std::array<u8, 0x1E>> out_configuration_id);
    Result GetAccelerometerOffset(Out<Vector3> out_offset);
    Result GetAccelerometerScale(Out<Vector3> out_scale);
    Result GetGyroscopeOffset(Out<Vector3> out_offset);
    Result GetGyroscopeScale(Out<Vector3> out_scale);
    Result GetWirelessLanMacAddress(Out<std::array<u8, 6>> out_address);
    Result GetWirelessLanCountryCodeCount(Out<s32> out_count);
    Result GetWirelessLanCountryCodes(Out<s32> out_count,
                                      OutBuffer<BufferAttr_HipcMapAlias> out_codes);
    Result GetSerialNumber(Out<SerialNumber> out_serial_number);
    Result GetBatteryLot(Out<BatteryLot> out_battery_lot);
    Result GetEciDeviceCertificate(OutBuffer<BufferAttr_HipcMapAlias> out_certificate);
    Result GetEticketDeviceCertificate(OutBuffer<BufferAttr_HipcMapAlias> out_certificate);
    Result GetSslKey(OutBuffer<BufferAttr_HipcMapAlias> out_key);
    Result GetSslCertificate(OutBuffer<BufferAttr_HipcMapAlias> out_certificate);
    Result GetEciDeviceKey(Out<std::array<u8, 0x54>> out_key);
    Result GetEticketDeviceKey(OutBuffer<BufferAttr_HipcMapAlias> out_key);
    Result GetLcdVendorId(Out<u32> out_vendor_id);
    Result GetEciDeviceCertificate2(OutBuffer<BufferAttr_HipcMapAlias> out_certificate);
    Result GetEciDeviceKey2(OutBuffer<BufferAttr_HipcMapAlias> out_key);
    Result GetUsbTypeCPowerSourceCircuitVersion(Out<u8> out_version);
    Result GetConsoleSixAxisSensorModuleType(Out<u8> out_type);
    Result GetConsoleSixAxisSensorHorizontalOffset(Out<Vector3> out_offset);
    Result GetBatteryVersion(Out<u8> out_version);
    Result GetDeviceId(Out<u64> out_device_id);
};

} // namespace Service::Set
