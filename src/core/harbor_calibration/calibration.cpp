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
#include "core/openpak/platform.h"
#include "core/openpak/session.h"
#include "common/harbor_crash.h"

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
    const bool kept = OpenPak::StoreHostSecret(SecretName(device_id), hex);
    OPENSSL_cleanse(hex.data(), hex.size());
    return kept;
}

struct Loaded {
    std::vector<u8> binary;
    Cal0::DeviceKey device_key{};
    bool keys_pending = false;
};

std::filesystem::path CalibrationDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "calibration";
}

std::filesystem::path SealedKeyPath() {
    return CalibrationDir() / "device-key.hbk";
}

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
    auto secret = OpenPak::LoadHostSecret(SecretName(device_id));
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

    // A restored backup that carries its device key sealed: the keys wait for the passphrase
    // (RestoreDeviceKey) instead of being replaced.
    std::error_code ec;
    if (std::filesystem::exists(SealedKeyPath(), ec)) {
        out.keys_pending = true;
        LOG_WARNING(Service_SET, "calibration: loaded; the device key is in this NAND's backup and "
                                 "waits for its passphrase");
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

namespace {

struct Current {
    std::mutex mutex;
    std::filesystem::path loaded_from;
    std::unique_ptr<Calibration> current;
    // Replaced instances stay alive: a caller may still hold a reference to one.
    std::vector<std::unique_ptr<Calibration>> retired;
};

Current& CurrentState() {
    static Current state;
    return state;
}

} // namespace

const Calibration& Calibration::Get() {
    HARBOR_CRUMB_ONCE("calibration: first Get (MAC, device id, serial)");
    Current& state = CurrentState();
    std::scoped_lock lock{state.mutex};
    const std::filesystem::path path = CalibrationDir() / "prodinfo.bin";
    if (!state.current || path != state.loaded_from) {
        Loaded loaded = Load(path);
        auto next = std::make_unique<Calibration>();
        next->binary = std::move(loaded.binary);
        next->device_key = loaded.device_key;
        next->keys_pending = loaded.keys_pending;
        OPENSSL_cleanse(loaded.device_key.data(), loaded.device_key.size());
        if (state.current) {
            state.retired.push_back(std::move(state.current));
        }
        state.current = std::move(next);
        state.loaded_from = path;
    }
    return *state.current;
}

void Calibration::Reload() {
    Current& state = CurrentState();
    std::scoped_lock lock{state.mutex};
    if (state.current) {
        state.retired.push_back(std::move(state.current));
    }
    state.loaded_from.clear();
}

bool WriteDeviceKeyBackup(std::string_view passphrase) {
    const auto& calibration = Calibration::Get();
    if (calibration.Binary().empty() || calibration.keys_pending) {
        return false;
    }
    const auto sealed =
        Cal0::SealDeviceKey(calibration.device_key, calibration.DeviceId(), passphrase);
    const bool ok = !sealed.empty() && WriteFile(SealedKeyPath(), sealed);
    LOG_INFO(Service_SET, "calibration: device key sealed for a backup: {}", ok ? "ok" : "failed");
    return ok;
}

void RemoveDeviceKeyBackup() {
    std::error_code ec;
    std::filesystem::remove(SealedKeyPath(), ec);
}

bool DeviceKeyRestorePending() {
    return Calibration::Get().keys_pending;
}

RestoreResult RestoreDeviceKey(std::string_view passphrase) {
    const auto& calibration = Calibration::Get();
    const auto sealed = ReadFile(SealedKeyPath());
    if (!sealed || calibration.Binary().empty()) {
        return RestoreResult::NoBackup;
    }
    const auto key = Cal0::OpenDeviceKey(*sealed, calibration.DeviceId(), passphrase);
    if (!key) {
        LOG_WARNING(Service_SET, "calibration: the backup's device key did not open");
        return RestoreResult::WrongPassphrase;
    }
    if (!Cal0::EccDevicePrivateKey(calibration.Binary(), *key) ||
        !StoreDeviceKey(calibration.DeviceId(), *key)) {
        return RestoreResult::Failed;
    }
    RemoveDeviceKeyBackup();
    Calibration::Reload();
    LOG_INFO(Service_SET, "calibration: device key restored from the backup; same console");
    return RestoreResult::Restored;
}

bool MakeNewConsole() {
    const u64 old_id = Calibration::Get().DeviceId();
    const std::string old_namespace = OpenPak::CurrentSecretNamespace();

    // The old console's credentials, in this process and in the password store.
    OpenPak::Session::ForgetAllProfiles();
    OpenPak::EraseSecretNamespace(old_namespace);
    if (old_id != 0) {
        OpenPak::EraseHostSecret(SecretName(old_id));
    }

    // Its identity: the calibration, a sealed key, and the per-server TLS files made from it.
    std::error_code ec;
    std::filesystem::remove(CalibrationDir() / "prodinfo.bin", ec);
    RemoveDeviceKeyBackup();
    for (const auto& entry :
         std::filesystem::directory_iterator(OpenPak::Platform::DataDir(), ec)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("device-") && (name.ends_with(".pem") || name.ends_with(".key"))) {
            std::filesystem::remove(entry.path(), ec);
        }
    }

    Calibration::Reload();
    const bool made = Calibration::Get().DeviceId() != 0;
    LOG_INFO(Service_SET, "calibration: this NAND is a new console ({})",
             made ? "new calibration made" : "no calibration could be made");
    return made;
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

void InstallSecretNamespace() {
    OpenPak::SetSecretNamespace([] {
        const u64 device_id = Calibration::Get().DeviceId();
        if (device_id == 0) {
            return std::string{};
        }
        u8 hash[SHA256_DIGEST_LENGTH];
        const std::string input = "harbor-console:" + std::to_string(device_id);
        SHA256(reinterpret_cast<const u8*>(input.data()), input.size(), hash);
        return "console-" + Hex(hash, 8);
    });
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
