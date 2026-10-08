// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/harbor_calibration/calibration.h"
#include "core/openpak/secret_store.h"
#include "core/openpak/session.h"

namespace Harbor {

namespace {

constexpr char HexDigits[] = "0123456789abcdef";

std::string Hex(const u8* data, std::size_t size) {
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out += HexDigits[data[i] >> 4];
        out += HexDigits[data[i] & 0xF];
    }
    return out;
}

std::optional<Cal0::DeviceKey> FromHex(std::string_view text) {
    Cal0::DeviceKey key{};
    if (text.size() != key.size() * 2) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char* at = std::strchr(HexDigits, text[i]);
        if (at == nullptr || *at == '\0') {
            return std::nullopt;
        }
        key[i / 2] = u8((key[i / 2] << 4) | (at - HexDigits));
    }
    return key;
}

/// The password-store entry of one install's device key. Named by a hash of the device id, so the
/// store's item list does not show the id itself.
std::string SecretName(u64 device_id) {
    u8 hash[SHA256_DIGEST_LENGTH];
    const std::string input = "harbor-device-key:" + std::to_string(device_id);
    SHA256(reinterpret_cast<const u8*>(input.data()), input.size(), hash);
    return "harbor-device-key-" + Hex(hash, 8);
}

std::optional<std::vector<u8>> ReadFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    std::vector<u8> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return data;
}

bool WriteFile(const std::filesystem::path& path, std::span<const u8> data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temporary = path.string() + ".new";
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        if (!file.write(reinterpret_cast<const char*>(data.data()),
                        static_cast<std::streamsize>(data.size()))) {
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    return !ec;
}

bool StoreDeviceKey(u64 device_id, const Cal0::DeviceKey& key) {
    std::string hex = Hex(key.data(), key.size());
    const bool kept = OpenPak::StoreSecret(SecretName(device_id), hex);
    OPENSSL_cleanse(hex.data(), hex.size());
    return kept;
}

struct Loaded {
    std::vector<u8> binary;
    Cal0::DeviceKey device_key{};
};

Loaded Make(const std::filesystem::path& path) {
    Loaded out;
    out.device_key = Cal0::NewDeviceKey();
    out.binary = Cal0::Generate(out.device_key);
    if (out.binary.empty()) {
        LOG_ERROR(Service_SET, "calibration: could not make one (no ECC B233 or RSA in this "
                               "OpenSSL); set:cal answers zeros");
        return out;
    }
    const bool kept = StoreDeviceKey(Cal0::DeviceId(out.binary), out.device_key);
    if (!WriteFile(path, out.binary)) {
        LOG_ERROR(Service_SET, "calibration: could not write {}; it is made again next boot",
                  path.filename().string());
    }
    LOG_INFO(Service_SET,
             "calibration: made this NAND's own ({} bytes, serial present, device key {})",
             out.binary.size(), kept ? "in the password store" : "NOT kept");
    return out;
}

Loaded Load(const std::filesystem::path& path) {
    auto binary = ReadFile(path);
    if (!binary) {
        return Make(path);
    }
    std::string why;
    if (!Cal0::Verify(*binary, &why)) {
        // Kept aside, never overwritten: it is the identity this NAND had.
        std::error_code ec;
        std::filesystem::rename(path, path.string() + ".invalid", ec);
        LOG_ERROR(Service_SET, "calibration: {} failed its check ({}); kept as .invalid, making "
                               "a new one",
                  path.filename().string(), why);
        return Make(path);
    }

    Loaded out;
    out.binary = std::move(*binary);
    const u64 device_id = Cal0::DeviceId(out.binary);
    auto secret = OpenPak::LoadSecret(SecretName(device_id));
    std::optional<Cal0::DeviceKey> key;
    if (secret) {
        key = FromHex(*secret);
        OPENSSL_cleanse(secret->data(), secret->size());
    }
    if (key && Cal0::EccDevicePrivateKey(out.binary, *key)) {
        out.device_key = *key;
        LOG_INFO(Service_SET, "calibration: loaded ({} bytes, checks pass, device key present)",
                 out.binary.size());
        return out;
    }

    // A NAND from another host (or a host whose password store was reset): same console, its
    // keys unreadable here, so new key pairs under a new device key. Serial, addresses and device
    // id stay.
    out.device_key = Cal0::NewDeviceKey();
    if (!Cal0::RegenerateKeys(out.binary, out.device_key)) {
        LOG_ERROR(Service_SET, "calibration: could not make new key pairs");
        return out;
    }
    const bool kept = StoreDeviceKey(device_id, out.device_key);
    WriteFile(path, out.binary);
    LOG_WARNING(Service_SET,
                "calibration: this host has no device key for this NAND; identity kept, new key "
                "pairs made (device key {})",
                kept ? "in the password store" : "NOT kept");
    return out;
}

} // namespace

const Calibration& Calibration::Get() {
    static std::mutex mutex;
    static std::filesystem::path loaded_from;
    static std::unique_ptr<Calibration> current;

    std::scoped_lock lock{mutex};
    const std::filesystem::path path =
        Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "calibration" / "prodinfo.bin";
    if (!current || path != loaded_from) {
        Loaded loaded = Load(path);
        auto next = std::make_unique<Calibration>();
        next->binary = std::move(loaded.binary);
        next->device_key = loaded.device_key;
        OPENSSL_cleanse(loaded.device_key.data(), loaded.device_key.size());
        current = std::move(next);
        loaded_from = path;
    }
    return *current;
}

std::span<const u8> Calibration::BlockData(const Cal0::Block& block) const {
    static const std::vector<u8> blank(Cal0::BinarySize, 0);
    return Cal0::Data(binary.empty() ? std::span<const u8>{blank} : std::span<const u8>{binary},
                      block);
}

std::string Calibration::SerialNumber() const {
    return binary.empty() ? std::string{} : Cal0::Serial(binary);
}

u64 Calibration::DeviceId() const {
    return binary.empty() ? 0 : Cal0::DeviceId(binary);
}

std::array<u8, 6> Calibration::WlanMacAddress() const {
    return binary.empty() ? std::array<u8, 6>{} : Cal0::WlanMac(binary);
}

std::array<u8, 6> Calibration::BluetoothAddress() const {
    return binary.empty() ? std::array<u8, 6>{} : Cal0::BluetoothAddress(binary);
}

std::vector<u8> Calibration::SslCertificate() const {
    return binary.empty() ? std::vector<u8>{} : Cal0::SslCertificateDer(binary);
}

std::optional<std::vector<u8>> Calibration::SslPrivateKey() const {
    if (binary.empty()) {
        return std::nullopt;
    }
    return Cal0::RsaPrivateKeyDer(binary, Cal0::ExtendedSslKey, device_key);
}

Cal0::DeviceKey Calibration::DeriveDeviceKey(std::string_view label) const {
    return Cal0::DeriveKey(device_key, label);
}

void InstallClientIdentity() {
    OpenPak::Session::SetClientIdentitySource([](std::vector<u8>& certificate,
                                                 std::vector<u8>& key) {
        const auto& calibration = Calibration::Get();
        auto private_key = calibration.SslPrivateKey();
        certificate = calibration.SslCertificate();
        if (certificate.empty() || !private_key) {
            return false;
        }
        key = std::move(*private_key);
        return true;
    });
}

} // namespace Harbor
