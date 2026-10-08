// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// harbor-ksp-key: the key seed package body key for the saves server, derived with Harbor's own
// code from a console's keys and installed firmware, and an end-to-end check of a signed package.
//
//   harbor-ksp-key derive --keys <prod.keys> --nand <nand dir> --generation N --out <file>
//   harbor-ksp-key verify --keys <prod.keys> --nand <nand dir> --generation N
//                         --package-key <file> --signing-key <private key PEM>
//
// Keys are read in place (a temporary keys directory holds only a link to the given prod.keys
// and is removed before exit) and are never printed. derive writes 16 raw bytes, mode 0600.
// verify builds a package as the saves server does (signature with the PEM key, body encrypted
// with the package key) and checks that Harbor's SetKeySeedPackage path accepts it against the
// embedded OpenPak modulus with the key derived from the firmware.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_crypto.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_keys.h"
#include "core/hle/service/filesystem/fsp/save_transfer/transfer_service.h"

namespace st = Service::FileSystem::SaveTransfer;

namespace {

int Usage() {
    std::fputs("usage: harbor-ksp-key derive --keys <prod.keys> --nand <nand dir> --generation N "
               "--out <file>\n"
               "       harbor-ksp-key verify --keys <prod.keys> --nand <nand dir> --generation N "
               "--package-key <file> --signing-key <pem>\n",
               stderr);
    return 2;
}

/// A keys directory of its own that only links to the given prod.keys, so nothing Eden's key
/// manager writes lands beside the owner's keys. Removed on scope exit.
class KeysDirectory {
public:
    explicit KeysDirectory(const std::filesystem::path& prod_keys) {
        std::random_device random;
        const char* tmp = std::getenv("TMPDIR");
        path = std::filesystem::path{tmp && *tmp ? tmp : "."} /
               fmt::format("harbor-ksp-key-{:08x}", random());
        std::filesystem::create_directories(path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
        std::filesystem::create_symlink(std::filesystem::absolute(prod_keys), path / "prod.keys");
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, path);
    }
    ~KeysDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

private:
    std::filesystem::path path;
};

std::optional<st::Key128> DerivePackageKey(const std::filesystem::path& nand, int generation) {
    FileSys::RealVfsFilesystem filesystem;
    const auto registered =
        filesystem.OpenDirectory((nand / "system" / "Contents" / "registered").string(),
                                 FileSys::OpenMode::Read);
    if (!registered) {
        std::fputs("no system/Contents/registered under the NAND directory\n", stderr);
        return std::nullopt;
    }
    const FileSys::RegisteredCache cache{registered};
    const auto sources = st::FsKeySourcesFromNand(cache);
    if (!sources) {
        std::fputs("the FS key sources were not found in the installed firmware\n", stderr);
        return std::nullopt;
    }
    const auto key = st::DeriveTransferKey(*sources, st::KeyIndex::KeySeedPackage, generation);
    if (!key) {
        std::fputs("prod.keys lacks the master key or a generation source for this generation\n",
                   stderr);
    }
    return key;
}

std::optional<st::Key128> ReadPackageKey(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    st::Key128 key{};
    if (bytes.size() == key.size()) {
        std::memcpy(key.data(), bytes.data(), key.size());
        return key;
    }
    while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r')) {
        bytes.pop_back();
    }
    if (bytes.size() == key.size() * 2) {
        for (std::size_t i = 0; i < key.size(); ++i) {
            key[i] = static_cast<u8>(std::stoi(bytes.substr(i * 2, 2), nullptr, 16));
        }
        return key;
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return Usage();
    }
    const std::string command = argv[1];
    std::map<std::string, std::string> options;
    for (int i = 2; i + 1 < argc; i += 2) {
        options[argv[i]] = argv[i + 1];
    }
    if (!options.contains("--keys") || !options.contains("--nand") ||
        !options.contains("--generation")) {
        return Usage();
    }
    if (std::getenv("HARBOR_KSP_MODULUS_FILE") != nullptr) {
        std::fputs("unset HARBOR_KSP_MODULUS_FILE: the check uses the embedded OpenPak modulus\n",
                   stderr);
        return 2;
    }
    const int requested = std::stoi(options["--generation"]);
    // fs reads the package's key generation as max(generation, 4) (0x2a584).
    const int generation = std::max(requested, 4);

    const KeysDirectory keys{options["--keys"]};
    const auto derived = DerivePackageKey(options["--nand"], generation);
    if (!derived) {
        return 1;
    }

    if (command == "derive") {
        if (!options.contains("--out")) {
            return Usage();
        }
        const int fd = ::open(options["--out"].c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || ::fchmod(fd, 0600) != 0 ||
            ::write(fd, derived->data(), derived->size()) !=
                static_cast<ssize_t>(derived->size())) {
            std::fputs("the key file could not be written\n", stderr);
            if (fd >= 0) {
                ::close(fd);
            }
            return 1;
        }
        ::close(fd);
        std::printf("written, %zu bytes, key generation %d (package byte +0x08 = %d)\n",
                    derived->size(), generation, requested);
        return 0;
    }

    if (command == "verify") {
        if (!options.contains("--package-key") || !options.contains("--signing-key")) {
            return Usage();
        }
        const auto package_key = ReadPackageKey(options["--package-key"]);
        if (!package_key) {
            std::fputs("the package key file is not 16 bytes or 32 hex digits\n", stderr);
            return 1;
        }
        std::printf("package key file %s the key Harbor derives for generation %d\n",
                    *package_key == *derived ? "matches" : "DIFFERS FROM", generation);

        st::KeySeedPackageContent content;
        st::RandomBytes(content.key_seed);
        st::RandomBytes(content.initial_data_mac);
        st::RandomBytes(content.challenge);
        const auto package = st::BuildKeySeedPackage(content, static_cast<u8>(requested),
                                                     *package_key, options["--signing-key"]);
        if (!package) {
            std::fputs("the signing key could not sign\n", stderr);
            return 1;
        }
        std::array<u8, 0x200> bytes{};
        std::memcpy(bytes.data(), &*package, bytes.size());
        const auto opened =
            st::OpenKeySeedPackage(bytes, *derived, st::KeySeedPackageModulus(), content.challenge);
        const char* verdict = "accepted";
        switch (opened.error) {
        case st::PackageError::None:
            if (opened.content.key_seed != content.key_seed ||
                opened.content.initial_data_mac != content.initial_data_mac) {
                verdict = "accepted, but the seed or MAC came back different";
            }
            break;
        case st::PackageError::Version:
            verdict = "refused: version (2-7037)";
            break;
        case st::PackageError::Mac:
            verdict = "refused: body tag (2-7002), the package key does not match";
            break;
        case st::PackageError::Signature:
            verdict = "refused: signature (2-7003), the signing key is not OpenPak's";
            break;
        case st::PackageError::Challenge:
            verdict = "refused: challenge (2-7004)";
            break;
        }
        std::printf("a package signed with the given key and the package key: %s\n", verdict);
        return opened.error == st::PackageError::None ? 0 : 1;
    }
    return Usage();
}
