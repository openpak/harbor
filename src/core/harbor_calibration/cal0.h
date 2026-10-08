// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// A console's calibration binary (CAL0, the PRODINFO partition): the header, the CRC16 and SHA-256
// blocks, and a generator that makes a new, unique one for a Harbor install. The layout is the
// documented one (switchbrew "Calibration", Atmosphere's prodinfo blocks); docs/harbor/calibration.md
// lists every field Harbor fills and why.
//
// Private keys are never in the clear here: their blocks hold them wrapped (AES-128-GCM) under a key
// derived from the install's device key, the stand-in for the per-console key a Switch keeps in its
// SoC. The device key itself lives in the host's password store (see calibration.h).
//
// No emulator, logging or file code: the self-check (check/calibration_check.cpp) builds this file
// alone against OpenSSL.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Harbor::Cal0 {

constexpr std::size_t BinarySize = 0x8000;
constexpr std::size_t HeaderSize = 0x40;
constexpr u32 Magic = 0x304C4143; // "CAL0"

using DeviceKey = std::array<u8, 16>;

/// One block: where it sits, how big it is (its check included), and what protects it.
struct Block {
    std::size_t offset;
    std::size_t size;
    bool sha256; // false: the last two bytes are a CRC16 over the rest
};

// clang-format off
constexpr Block ConfigurationId1           {0x0040, 0x0020, false};
constexpr Block WlanCountryCodes           {0x0080, 0x0190, false};
constexpr Block WlanMacAddress             {0x0210, 0x0010, false};
constexpr Block BdAddress                  {0x0220, 0x0010, false};
constexpr Block AccelerometerOffset        {0x0230, 0x0008, false};
constexpr Block AccelerometerScale         {0x0238, 0x0008, false};
constexpr Block GyroscopeOffset            {0x0240, 0x0008, false};
constexpr Block GyroscopeScale             {0x0248, 0x0008, false};
constexpr Block SerialNumber               {0x0250, 0x0020, false};
constexpr Block EccB233DeviceCertificate   {0x0480, 0x0190, false};
constexpr Block SslKey                     {0x09B0, 0x0120, false}; // legacy slot, left blank
constexpr Block SslCertificateSize         {0x0AD0, 0x0010, false};
constexpr Block SslCertificate             {0x0AE0, 0x0820, true};
constexpr Block RandomNumber               {0x1300, 0x1020, true};
constexpr Block GameCardCertificate        {0x2440, 0x0420, true};  // no game card slot: blank
constexpr Block Rsa2048ETicketCertificate  {0x2A90, 0x0250, false};
constexpr Block BatteryLot                 {0x2CE0, 0x0020, false};
constexpr Block SpeakerCalibrationValue    {0x2D00, 0x0810, false};
constexpr Block RegionCode                 {0x3510, 0x0010, false};
constexpr Block ProductModel               {0x3740, 0x0010, false};
constexpr Block ColorVariation             {0x3750, 0x0010, false};
constexpr Block ExtendedEccB233DeviceKey   {0x3770, 0x0060, false};
constexpr Block ExtendedRsa2048ETicketKey  {0x3890, 0x0250, false};
constexpr Block ExtendedSslKey             {0x3AE0, 0x0140, false};
constexpr Block LcdVendorId                {0x3D60, 0x0010, false};
constexpr Block ExtendedRsa2048DeviceKey   {0x3D70, 0x0250, false};
constexpr Block Rsa2048DeviceCertificate   {0x3FC0, 0x0250, false};
constexpr Block UsbTypeCPowerSourceCircuit {0x4210, 0x0010, false};
constexpr Block ConsoleSixAxisSensorModuleType {0x42F0, 0x0010, false};
constexpr Block ConsoleSixAxisSensorHorizontalOffset {0x4300, 0x0010, false};
constexpr Block BatteryVersion             {0x4310, 0x0010, false};
// clang-format on

/// Every block Harbor writes or keeps valid, in offset order.
std::span<const Block> AllBlocks();

// Sizes of the values the firmware's set:cal returns.
constexpr std::size_t EccB233CertificateSize = 0x180;
constexpr std::size_t Rsa2048CertificateSize = 0x240;
constexpr std::size_t SslCertificateMax = 0x800;

/// The certificate authority Harbor's device certificates name as issuer. Its key is OpenPak's,
/// held by the owner; until a server has signed them the signatures are zero
/// (docs/harbor/calibration.md, "Certificates").
constexpr const char* DeviceCaIssuer = "Root-OP00000001-HD00000001";

/// Top byte of every Harbor device id. A console's device id is its 56-bit fuse id (top byte 0),
/// so a Harbor id can never equal one.
constexpr u64 DeviceIdPrefix = 0x7EULL << 56;

/// Serial numbers are "HBW" + ten digits + a check digit: a console's always starts with 'X'.
constexpr const char* SerialPrefix = "HBW";

u16 Crc16(const u8* data, std::size_t size);

/// What a block holds without its check bytes (CRC blocks) or hash (SHA-256 blocks).
std::span<const u8> Data(std::span<const u8> cal0, const Block& block);

/// Header magic, header CRC, body hash, and every block's CRC or hash.
bool Verify(std::span<const u8> cal0, std::string* why = nullptr);

/// A new calibration binary for a new install, wrapped under `device_key`. Empty on failure
/// (OpenSSL could not make a key).
std::vector<u8> Generate(const DeviceKey& device_key);

/// A new device key from the OS's random source.
DeviceKey NewDeviceKey();

/// Make new key pairs (and unsigned certificates for them) for an existing binary, keeping the
/// serial, addresses and device id: for an install whose device key is gone.
bool RegenerateKeys(std::vector<u8>& cal0, const DeviceKey& device_key);

/// Recompute the header's body hash and CRC after a block changed.
void Seal(std::vector<u8>& cal0);

// Readers. They assume Verify() passed.
std::string Serial(std::span<const u8> cal0);
u64 DeviceId(std::span<const u8> cal0);
std::array<u8, 6> WlanMac(std::span<const u8> cal0);
std::array<u8, 6> BluetoothAddress(std::span<const u8> cal0);
std::vector<u8> SslCertificateDer(std::span<const u8> cal0);

/// A private key, unwrapped: an ECC B233 scalar (30 bytes) or an RSA-2048 key as PKCS#8 DER.
/// Nothing when the device key does not open it.
std::optional<std::vector<u8>> EccDevicePrivateKey(std::span<const u8> cal0, const DeviceKey& key);
std::optional<std::vector<u8>> RsaPrivateKeyDer(std::span<const u8> cal0, const Block& key_block,
                                                const DeviceKey& key);

/// Whether a certificate block carries a signature (zero until OpenPak's device CA signs it).
bool IsSigned(std::span<const u8> cal0, const Block& certificate_block);

/// The device key sealed with a passphrase for a console backup: scrypt (N 2^15, r 8, p 1) to an
/// AES-256-GCM key, the console's device id bound in as associated data. The passphrase is not kept.
std::vector<u8> SealDeviceKey(const DeviceKey& device_key, u64 device_id, std::string_view passphrase);

/// The device key from a sealed backup, or nothing when the passphrase is wrong, the blob is
/// damaged or it belongs to another console.
std::optional<DeviceKey> OpenDeviceKey(std::span<const u8> sealed, u64 device_id,
                                       std::string_view passphrase);

/// A key derived from the device key for one purpose (HMAC-SHA256, first 16 bytes). Every use of
/// the device key goes through a label of its own.
DeviceKey DeriveKey(const DeviceKey& device_key, std::string_view label);

} // namespace Harbor::Cal0
