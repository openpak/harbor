// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Windows: Credential Manager. Generic credentials, target "org.openpak.harbor/<key>", kept on this
// machine for this user. A secret larger than a credential can hold (CRED_MAX_CREDENTIAL_BLOB_SIZE)
// goes into a DPAPI-sealed file under <user>/openpak/secrets/ instead, bound to this Windows user
// and, through the optional entropy, to its key's name.
// Untested: written against the Win32 documentation, not yet built or run on Windows.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include <windows.h>
#include <dpapi.h>
#include <wincred.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/string_util.h"
#include "core/openpak/secret_store.h"
#include "frontend_common/secret_store.h"

namespace FrontendCommon {

namespace {

std::wstring Target(std::string_view key) {
    return Common::UTF8ToUTF16W("org.openpak.harbor/" + std::string{key});
}

/// The sealed file of an oversized secret, named by the key in hex.
std::filesystem::path FileOf(std::string_view key) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string name;
    for (const unsigned char c : key) {
        name += digits[c >> 4];
        name += digits[c & 0xf];
    }
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "openpak" / "secrets" /
           (name + ".dpapi");
}

DATA_BLOB Blob(std::string_view bytes) {
    return DATA_BLOB{static_cast<DWORD>(bytes.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data()))};
}

class CredentialSecretStore final : public OpenPak::SecretStore {
public:
    std::optional<std::string> Load(std::string_view key, bool* unreadable) override {
        const std::wstring target = Target(key);
        PCREDENTIALW credential = nullptr;
        if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
            std::string value(reinterpret_cast<const char*>(credential->CredentialBlob),
                              credential->CredentialBlobSize);
            CredFree(credential);
            return value;
        }
        if (GetLastError() != ERROR_NOT_FOUND) {
            LOG_WARNING(Frontend, "harbor: Credential Manager could not be read (error {})",
                        GetLastError());
            if (unreadable) {
                *unreadable = true;
            }
            return std::nullopt;
        }
        return LoadFile(key, unreadable);
    }

    bool Store(std::string_view key, std::string_view secret) override {
        const std::wstring target = Target(key);
        if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
            if (!StoreFile(key, secret)) {
                return false;
            }
            CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
            return true;
        }
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<LPWSTR>(target.c_str());
        credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&credential, 0)) {
            LOG_WARNING(Frontend, "harbor: Credential Manager could not be written (error {})",
                        GetLastError());
            return false;
        }
        std::error_code ec;
        std::filesystem::remove(FileOf(key), ec);
        return true;
    }

    void Erase(std::string_view key) override {
        const std::wstring target = Target(key);
        CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
        std::error_code ec;
        std::filesystem::remove(FileOf(key), ec);
    }

private:
    static std::optional<std::string> LoadFile(std::string_view key, bool* unreadable) {
        std::ifstream file(FileOf(key), std::ios::binary);
        if (!file) {
            return std::nullopt;
        }
        const std::string sealed{std::istreambuf_iterator<char>(file), {}};
        DATA_BLOB in = Blob(sealed);
        DATA_BLOB entropy = Blob(key);
        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            LOG_WARNING(Frontend, "harbor: a sealed secret could not be opened (error {})",
                        GetLastError());
            if (unreadable) {
                *unreadable = true;
            }
            return std::nullopt;
        }
        std::string value(reinterpret_cast<const char*>(out.pbData), out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return value;
    }

    static bool StoreFile(std::string_view key, std::string_view secret) {
        DATA_BLOB in = Blob(secret);
        DATA_BLOB entropy = Blob(key);
        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"Harbor (OpenPak)", &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            LOG_WARNING(Frontend, "harbor: a secret could not be sealed (error {})", GetLastError());
            return false;
        }
        const std::string sealed(reinterpret_cast<const char*>(out.pbData), out.cbData);
        LocalFree(out.pbData);

        const auto path = FileOf(key);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file.write(sealed.data(), static_cast<std::streamsize>(sealed.size()))) {
                return false;
            }
        }
        std::filesystem::rename(temporary, path, ec);
        return !ec;
    }
};

} // namespace

std::shared_ptr<OpenPak::SecretStore> CreatePlatformSecretStore() {
    return std::make_shared<CredentialSecretStore>();
}

} // namespace FrontendCommon
