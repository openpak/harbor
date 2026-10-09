// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The keys of save-data transfer v2 on this console: FS's key sources found in the user's own
// installed firmware (BootImagePackage 0100000000000819 → nx/package2 → kernel → INI1 → FS KIP
// .rodata, matched by SHA-256 only), combined with prod.keys through the spl derivation
// (fs-transfer.md §9.1). Nothing secret is in the source; derived keys stay in memory.

#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <span>

#include "core/hle/service/filesystem/fsp/save_transfer/transfer_crypto.h"

namespace Core {
class System;
}

namespace FileSys {
class RegisteredCache;
}

namespace Service::FileSystem::SaveTransfer {

/// The FS .rodata constants the cloud-backup keys need.
struct FsKeySources {
    Key128 kek_source_ksp{};    ///< KekSource[2]
    Key128 kek_source_stream{}; ///< KekSource[3]
    Key128 kek_source_mac{};    ///< KekSource[5]
    Key128 key_source_ksp{};    ///< key source of index 2
    Key128 key_source_mac{};    ///< key source of index 5
};

/// Finds the sources in a package2 image (as stored in the BootImagePackage RomFS: header at
/// offset 0). nullopt when it cannot be decrypted or a source is missing.
std::optional<FsKeySources> FindFsKeySources(std::span<const u8> package2);

/// The key of `index` for `key_generation`; `source` is the archive's key seed for
/// KeyIndex::Stream and ignored otherwise. nullopt when a key is missing.
std::optional<Key128> DeriveTransferKey(const FsKeySources& sources, KeyIndex index,
                                        int key_generation, const Key128& source = {});

/// The sources in the BootImagePackage (0100000000000819) of a system NAND's contents.
std::optional<FsKeySources> FsKeySourcesFromNand(const FileSys::RegisteredCache& system_nand);

/// This console's sources, found once from the installed firmware.
std::optional<FsKeySources> InstalledFsKeySources(Core::System& system);

} // namespace Service::FileSystem::SaveTransfer
