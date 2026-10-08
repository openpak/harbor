// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "core/harbor_calibration/calibration.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/set/factory_settings_server.h"
#include "core/openpak/session.h"

namespace Service::Set {

IFactorySettingsServer::IFactorySettingsServer(Core::System& system_)
    : ServiceFramework{system_, "set:cal"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, C<&IFactorySettingsServer::GetBluetoothBdAddress>, "GetBluetoothBdAddress"},
        {1, C<&IFactorySettingsServer::GetConfigurationId1>, "GetConfigurationId1"},
        {2, C<&IFactorySettingsServer::GetAccelerometerOffset>, "GetAccelerometerOffset"},
        {3, C<&IFactorySettingsServer::GetAccelerometerScale>, "GetAccelerometerScale"},
        {4, C<&IFactorySettingsServer::GetGyroscopeOffset>, "GetGyroscopeOffset"},
        {5, C<&IFactorySettingsServer::GetGyroscopeScale>, "GetGyroscopeScale"},
        {6, C<&IFactorySettingsServer::GetWirelessLanMacAddress>, "GetWirelessLanMacAddress"},
        {7, C<&IFactorySettingsServer::GetWirelessLanCountryCodeCount>, "GetWirelessLanCountryCodeCount"},
        {8, C<&IFactorySettingsServer::GetWirelessLanCountryCodes>, "GetWirelessLanCountryCodes"},
        {9, C<&IFactorySettingsServer::GetSerialNumber>, "GetSerialNumber"},
        {10, nullptr, "SetInitialSystemAppletProgramId"},
        {11, nullptr, "SetOverlayDispProgramId"},
        {12, C<&IFactorySettingsServer::GetBatteryLot>, "GetBatteryLot"},
        {14, C<&IFactorySettingsServer::GetEciDeviceCertificate>, "GetEciDeviceCertificate"},
        {15, C<&IFactorySettingsServer::GetEticketDeviceCertificate>, "GetEticketDeviceCertificate"},
        {16, C<&IFactorySettingsServer::GetSslKey>, "GetSslKey"},
        {17, C<&IFactorySettingsServer::GetSslCertificate>, "GetSslCertificate"},
        {18, nullptr, "GetGameCardKey"},
        {19, nullptr, "GetGameCardCertificate"},
        {20, C<&IFactorySettingsServer::GetEciDeviceKey>, "GetEciDeviceKey"},
        {21, C<&IFactorySettingsServer::GetEticketDeviceKey>, "GetEticketDeviceKey"},
        {22, nullptr, "GetSpeakerParameter"},
        {23, C<&IFactorySettingsServer::GetLcdVendorId>, "GetLcdVendorId"},
        {24, C<&IFactorySettingsServer::GetEciDeviceCertificate2>, "GetEciDeviceCertificate2"},
        {25, C<&IFactorySettingsServer::GetEciDeviceKey2>, "GetEciDeviceKey2"},
        {26, nullptr, "GetAmiiboKey"},
        {27, nullptr, "GetAmiiboEcqvCertificate"},
        {28, nullptr, "GetAmiiboEcdsaCertificate"},
        {29, nullptr, "GetAmiiboEcqvBlsKey"},
        {30, nullptr, "GetAmiiboEcqvBlsCertificate"},
        {31, nullptr, "GetAmiiboEcqvBlsRootCertificate"},
        {32, C<&IFactorySettingsServer::GetUsbTypeCPowerSourceCircuitVersion>, "GetUsbTypeCPowerSourceCircuitVersion"},
        {33, nullptr, "GetAnalogStickModuleTypeL"},
        {34, nullptr, "GetAnalogStickModelParameterL"},
        {35, nullptr, "GetAnalogStickFactoryCalibrationL"},
        {36, nullptr, "GetAnalogStickModuleTypeR"},
        {37, nullptr, "GetAnalogStickModelParameterR"},
        {38, nullptr, "GetAnalogStickFactoryCalibrationR"},
        {39, C<&IFactorySettingsServer::GetConsoleSixAxisSensorModuleType>, "GetConsoleSixAxisSensorModuleType"},
        {40, C<&IFactorySettingsServer::GetConsoleSixAxisSensorHorizontalOffset>, "GetConsoleSixAxisSensorHorizontalOffset"},
        {41, C<&IFactorySettingsServer::GetBatteryVersion>, "GetBatteryVersion"},
        {42, C<&IFactorySettingsServer::GetDeviceId>, "GetDeviceId"},
        {43, nullptr, "GetConsoleSixAxisSensorMountType"},
    };
    // clang-format on

    RegisterHandlers(functions);

    // A new NAND gets its calibration at its first boot, not at the first call that needs it.
    Harbor::Calibration::Get();

    // OpenPak servers this install meets from now on see the console's own TLS certificate.
    OpenPak::Session::SetClientIdentitySource([](std::vector<u8>& certificate, std::vector<u8>& key) {
        const auto& calibration = Harbor::Calibration::Get();
        auto private_key = calibration.SslPrivateKey();
        certificate = calibration.SslCertificate();
        if (certificate.empty() || !private_key) {
            return false;
        }
        key = std::move(*private_key);
        return true;
    });
}

IFactorySettingsServer::~IFactorySettingsServer() = default;

namespace {

namespace Cal0 = Harbor::Cal0;

std::span<const u8> BlockData(const Cal0::Block& block) {
    return Harbor::Calibration::Get().BlockData(block);
}

/// The first sizeof(T) bytes of a block, as the console's set:cal copies a block's value out.
template <typename T>
T Value(const Cal0::Block& block) {
    T out{};
    const auto data = BlockData(block);
    std::memcpy(&out, data.data(), std::min(sizeof(T), data.size()));
    return out;
}

/// The first `size` bytes of a block into the caller's buffer.
void Copy(std::span<u8> out, const Cal0::Block& block, std::size_t size) {
    const auto data = BlockData(block);
    const std::size_t count = std::min({out.size(), size, data.size()});
    std::memcpy(out.data(), data.data(), count);
    std::fill(out.begin() + count, out.end(), u8{0});
}

} // namespace

// Identity values (addresses, serial, ids, keys) are never logged.

Result IFactorySettingsServer::GetBluetoothBdAddress(Out<std::array<u8, 6>> out_address) {
    LOG_DEBUG(Service_SET, "called");
    *out_address = Value<std::array<u8, 6>>(Cal0::BdAddress);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetConfigurationId1(
    Out<std::array<u8, 0x1E>> out_configuration_id) {
    LOG_DEBUG(Service_SET, "called");
    *out_configuration_id = Value<std::array<u8, 0x1E>>(Cal0::ConfigurationId1);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetAccelerometerOffset(Out<Vector3> out_offset) {
    LOG_DEBUG(Service_SET, "called");
    *out_offset = Value<Vector3>(Cal0::AccelerometerOffset);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetAccelerometerScale(Out<Vector3> out_scale) {
    LOG_DEBUG(Service_SET, "called");
    *out_scale = Value<Vector3>(Cal0::AccelerometerScale);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetGyroscopeOffset(Out<Vector3> out_offset) {
    LOG_DEBUG(Service_SET, "called");
    *out_offset = Value<Vector3>(Cal0::GyroscopeOffset);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetGyroscopeScale(Out<Vector3> out_scale) {
    LOG_DEBUG(Service_SET, "called");
    *out_scale = Value<Vector3>(Cal0::GyroscopeScale);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetWirelessLanMacAddress(Out<std::array<u8, 6>> out_address) {
    LOG_DEBUG(Service_SET, "called");
    *out_address = Value<std::array<u8, 6>>(Cal0::WlanMacAddress);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetWirelessLanCountryCodeCount(Out<s32> out_count) {
    LOG_DEBUG(Service_SET, "called");
    *out_count = Value<s32>(Cal0::WlanCountryCodes);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetWirelessLanCountryCodes(
    Out<s32> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_codes) {
    // The block: u32 count, u32 last index, then 3-byte codes.
    const s32 count = Value<s32>(Cal0::WlanCountryCodes);
    const auto data = BlockData(Cal0::WlanCountryCodes);
    const s32 fits = static_cast<s32>(out_codes.size() / 3);
    const s32 written = std::clamp(std::min(count, fits), 0, 0x80);
    LOG_DEBUG(Service_SET, "called, count={} written={}", count, written);
    std::memcpy(out_codes.data(), data.data() + 8, static_cast<std::size_t>(written) * 3);
    *out_count = written;
    R_SUCCEED();
}

Result IFactorySettingsServer::GetSerialNumber(Out<SerialNumber> out_serial_number) {
    LOG_DEBUG(Service_SET, "called");
    *out_serial_number = Value<SerialNumber>(Cal0::SerialNumber);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetBatteryLot(Out<BatteryLot> out_battery_lot) {
    LOG_DEBUG(Service_SET, "called");
    *out_battery_lot = Value<BatteryLot>(Cal0::BatteryLot);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetEciDeviceCertificate(
    OutBuffer<BufferAttr_HipcMapAlias> out_certificate) {
    LOG_DEBUG(Service_SET, "called, size={}", out_certificate.size());
    Copy(out_certificate, Cal0::EccB233DeviceCertificate, Cal0::EccB233CertificateSize);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetEticketDeviceCertificate(
    OutBuffer<BufferAttr_HipcMapAlias> out_certificate) {
    LOG_DEBUG(Service_SET, "called, size={}", out_certificate.size());
    Copy(out_certificate, Cal0::Rsa2048ETicketCertificate, Cal0::Rsa2048CertificateSize);
    R_SUCCEED();
}

// The key getters hand out a block as stored: a console's are encrypted under its own keys and
// opened by spl, Harbor's are wrapped under the install's device key and opened only by Harbor.

Result IFactorySettingsServer::GetSslKey(OutBuffer<BufferAttr_HipcMapAlias> out_key) {
    LOG_WARNING(Service_SET, "unknown contract: set:cal GetSslKey (size={}); the wrapped block",
                out_key.size());
    Copy(out_key, Cal0::ExtendedSslKey, 0x134);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetSslCertificate(
    OutBuffer<BufferAttr_HipcMapAlias> out_certificate) {
    LOG_DEBUG(Service_SET, "called, size={}", out_certificate.size());
    Copy(out_certificate, Cal0::SslCertificate, Cal0::SslCertificateMax);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetEciDeviceKey(Out<std::array<u8, 0x54>> out_key) {
    LOG_WARNING(Service_SET, "unknown contract: set:cal GetEciDeviceKey; the wrapped block");
    *out_key = Value<std::array<u8, 0x54>>(Cal0::ExtendedEccB233DeviceKey);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetEticketDeviceKey(OutBuffer<BufferAttr_HipcMapAlias> out_key) {
    LOG_WARNING(Service_SET,
                "unknown contract: set:cal GetEticketDeviceKey (size={}); the wrapped block",
                out_key.size());
    Copy(out_key, Cal0::ExtendedRsa2048ETicketKey, 0x244);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetLcdVendorId(Out<u32> out_vendor_id) {
    LOG_DEBUG(Service_SET, "called");
    *out_vendor_id = Value<u32>(Cal0::LcdVendorId);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetEciDeviceCertificate2(
    OutBuffer<BufferAttr_HipcMapAlias> out_certificate) {
    LOG_DEBUG(Service_SET, "called, size={}", out_certificate.size());
    Copy(out_certificate, Cal0::Rsa2048DeviceCertificate, Cal0::Rsa2048CertificateSize);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetEciDeviceKey2(OutBuffer<BufferAttr_HipcMapAlias> out_key) {
    LOG_WARNING(Service_SET,
                "unknown contract: set:cal GetEciDeviceKey2 (size={}); the wrapped block",
                out_key.size());
    Copy(out_key, Cal0::ExtendedRsa2048DeviceKey, 0x244);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetUsbTypeCPowerSourceCircuitVersion(Out<u8> out_version) {
    LOG_DEBUG(Service_SET, "called");
    *out_version = Value<u8>(Cal0::UsbTypeCPowerSourceCircuit);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetConsoleSixAxisSensorModuleType(Out<u8> out_type) {
    LOG_DEBUG(Service_SET, "called");
    *out_type = Value<u8>(Cal0::ConsoleSixAxisSensorModuleType);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetConsoleSixAxisSensorHorizontalOffset(Out<Vector3> out_offset) {
    LOG_DEBUG(Service_SET, "called");
    *out_offset = Value<Vector3>(Cal0::ConsoleSixAxisSensorHorizontalOffset);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetBatteryVersion(Out<u8> out_version) {
    LOG_DEBUG(Service_SET, "called");
    *out_version = Value<u8>(Cal0::BatteryVersion);
    R_SUCCEED();
}

Result IFactorySettingsServer::GetDeviceId(Out<u64> out_device_id) {
    LOG_DEBUG(Service_SET, "called");
    *out_device_id = Harbor::Calibration::Get().DeviceId();
    R_SUCCEED();
}

} // namespace Service::Set
