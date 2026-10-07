// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <cstring>
#include <vector>

#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/loader/loader.h"

namespace FileSys {

ProgramMetadata::ProgramMetadata() = default;

ProgramMetadata::~ProgramMetadata() = default;

Loader::ResultStatus ProgramMetadata::Load(VirtualFile file) {
    const std::size_t total_size = file->GetSize();
    if (total_size < sizeof(Header)) {
        return Loader::ResultStatus::ErrorBadNPDMHeader;
    }

    if (sizeof(Header) != file->ReadObject(&npdm_header)) {
        return Loader::ResultStatus::ErrorBadNPDMHeader;
    }

    if (sizeof(AcidHeader) != file->ReadObject(&acid_header, npdm_header.acid_offset)) {
        return Loader::ResultStatus::ErrorBadACIDHeader;
    }

    if (sizeof(AciHeader) != file->ReadObject(&aci_header, npdm_header.aci_offset)) {
        return Loader::ResultStatus::ErrorBadACIHeader;
    }

    // Load acid_file_access per-component instead of the entire struct, since this struct does not
    // reflect the layout of the real data.
    std::size_t current_offset = acid_header.fac_offset;
    if (sizeof(FileAccessControl::version) != file->ReadBytes(&acid_file_access.version,
                                                              sizeof(FileAccessControl::version),
                                                              current_offset)) {
        return Loader::ResultStatus::ErrorBadFileAccessControl;
    }
    if (sizeof(FileAccessControl::permissions) !=
        file->ReadBytes(&acid_file_access.permissions, sizeof(FileAccessControl::permissions),
                        current_offset += sizeof(FileAccessControl::version) + 3)) {
        return Loader::ResultStatus::ErrorBadFileAccessControl;
    }
    if (sizeof(FileAccessControl::unknown) !=
        file->ReadBytes(&acid_file_access.unknown, sizeof(FileAccessControl::unknown),
                        current_offset + sizeof(FileAccessControl::permissions))) {
        return Loader::ResultStatus::ErrorBadFileAccessControl;
    }

    // Load aci_file_access per-component instead of the entire struct, same as acid_file_access
    current_offset = aci_header.fah_offset;
    if (sizeof(FileAccessHeader::version) != file->ReadBytes(&aci_file_access.version,
                                                             sizeof(FileAccessHeader::version),
                                                             current_offset)) {
        return Loader::ResultStatus::ErrorBadFileAccessHeader;
    }
    if (sizeof(FileAccessHeader::permissions) !=
        file->ReadBytes(&aci_file_access.permissions, sizeof(FileAccessHeader::permissions),
                        current_offset += sizeof(FileAccessHeader::version) + 3)) {
        return Loader::ResultStatus::ErrorBadFileAccessHeader;
    }
    if (sizeof(FileAccessHeader::unk_offset) !=
        file->ReadBytes(&aci_file_access.unk_offset, sizeof(FileAccessHeader::unk_offset),
                        current_offset += sizeof(FileAccessHeader::permissions))) {
        return Loader::ResultStatus::ErrorBadFileAccessHeader;
    }
    if (sizeof(FileAccessHeader::unk_size) !=
        file->ReadBytes(&aci_file_access.unk_size, sizeof(FileAccessHeader::unk_size),
                        current_offset += sizeof(FileAccessHeader::unk_offset))) {
        return Loader::ResultStatus::ErrorBadFileAccessHeader;
    }
    if (sizeof(FileAccessHeader::unk_offset_2) !=
        file->ReadBytes(&aci_file_access.unk_offset_2, sizeof(FileAccessHeader::unk_offset_2),
                        current_offset += sizeof(FileAccessHeader::unk_size))) {
        return Loader::ResultStatus::ErrorBadFileAccessHeader;
    }
    if (sizeof(FileAccessHeader::unk_size_2) !=
        file->ReadBytes(&aci_file_access.unk_size_2, sizeof(FileAccessHeader::unk_size_2),
                        current_offset + sizeof(FileAccessHeader::unk_offset_2))) {
        return Loader::ResultStatus::ErrorBadFileAccessHeader;
    }

    aci_kernel_capabilities.resize(aci_header.kac_size / sizeof(u32));
    const u64 read_size = aci_header.kac_size;
    const u64 read_offset = npdm_header.aci_offset + aci_header.kac_offset;
    if (file->ReadBytes(aci_kernel_capabilities.data(), read_size, read_offset) != read_size) {
        return Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors;
    }

    // The service access control and the file access header as stored, both relative to ACI0.
    // Nothing here is required to start a program, so a short read just leaves them empty.
    const auto read_aci_block = [&](std::vector<u8>& out, u32 offset, u32 size) {
        out.resize(size);
        if (file->ReadBytes(out.data(), size, npdm_header.aci_offset + offset) != size) {
            out.clear();
        }
    };
    read_aci_block(aci_service_access, aci_header.sac_offset, aci_header.sac_size);
    read_aci_block(aci_file_access_data, aci_header.fah_offset, aci_header.fah_size);

    // The per-field read above takes fah_offset as a file offset, but it is relative to ACI0.
    if (aci_file_access_data.size() >= 4 + sizeof(u64)) {
        std::memcpy(&aci_file_access.permissions, aci_file_access_data.data() + 4, sizeof(u64));
    }

    return Loader::ResultStatus::Success;
}

std::vector<ProgramMetadata::ServiceAccessEntry> ProgramMetadata::GetServiceAccessList() const {
    std::vector<ServiceAccessEntry> list;
    const auto& sac = aci_service_access;
    for (std::size_t i = 0; i < sac.size();) {
        const u8 control = sac[i++];
        const std::size_t length = (control & 7) + 1;
        if (i + length > sac.size()) {
            break;
        }
        list.push_back({std::string(reinterpret_cast<const char*>(sac.data() + i), length),
                        (control & 0x80) != 0});
        i += length;
    }
    return list;
}

bool ProgramMetadata::IsServiceAccessAllowed(std::span<const ServiceAccessEntry> list,
                                             std::string_view name, bool is_host) {
    for (const auto& entry : list) {
        if (entry.is_host != is_host) {
            continue;
        }
        const std::string_view entry_name{entry.name};
        if (entry_name.ends_with('*') ? name.starts_with(entry_name.substr(0, entry_name.size() - 1))
                                      : name == entry_name) {
            return true;
        }
    }
    return false;
}

bool ProgramMetadata::CanWriteSaveDataOwnedBy(u64 owner_id) const {
    // File access header: +0x14 offset and +0x18 size of the save data owner block, which holds a
    // u32 count, count accessibility bytes (bit 1 = write) padded to 4, then count u64 owner ids.
    const auto& fah = aci_file_access_data;
    const auto read_u32 = [&](std::size_t at) {
        u32 value{};
        std::memcpy(&value, fah.data() + at, sizeof(value));
        return value;
    };
    if (fah.size() < 0x1C) {
        return false;
    }

    const std::size_t offset = read_u32(0x14);
    const std::size_t size = read_u32(0x18);
    if (size < 4 || offset + size > fah.size()) {
        return false;
    }

    const std::size_t count = read_u32(offset);
    const std::size_t ids = offset + 4 + ((count + 3) & ~std::size_t{3});
    for (std::size_t i = 0; i < count && ids + (i + 1) * sizeof(u64) <= offset + size; i++) {
        u64 id{};
        std::memcpy(&id, fah.data() + ids + i * sizeof(u64), sizeof(id));
        if (id == owner_id && (fah[offset + 4 + i] & 2) != 0) {
            return true;
        }
    }
    return false;
}

std::optional<ProgramMetadata> ProgramMetadata::LoadInstalled(const ContentProvider& provider,
                                                              u64 program_id) {
    const auto nca = provider.GetEntry(program_id, ContentRecordType::Program);
    if (nca == nullptr) {
        return std::nullopt;
    }

    const auto exefs = nca->GetExeFS();
    const auto npdm = exefs != nullptr ? exefs->GetFile("main.npdm") : nullptr;
    if (npdm == nullptr) {
        return std::nullopt;
    }

    ProgramMetadata metadata;
    if (metadata.Load(npdm) != Loader::ResultStatus::Success) {
        return std::nullopt;
    }
    return metadata;
}

Loader::ResultStatus ProgramMetadata::Reload(VirtualFile file) {
    const u64 original_program_id = aci_header.title_id;
    SCOPE_EXIT {
        aci_header.title_id = original_program_id;
    };

    return this->Load(file);
}

/*static*/ ProgramMetadata ProgramMetadata::GetDefault() {
    // Allow use of cores 0~3 and thread priorities 16~63.
    constexpr u32 default_thread_info_capability = 0x30043F7;

    ProgramMetadata result;

    result.LoadManual(
        true /*is_64_bit*/, FileSys::ProgramAddressSpaceType::Is39Bit /*address_space*/,
        0x2c /*main_thread_prio*/, 0 /*main_thread_core*/, 0x100000 /*main_thread_stack_size*/,
        0 /*title_id*/, 0xFFFFFFFFFFFFFFFF /*filesystem_permissions*/, 0 /*system_resource_size*/,
        {default_thread_info_capability} /*capabilities*/);

    return result;
}

void ProgramMetadata::LoadManual(bool is_64_bit, ProgramAddressSpaceType address_space,
                                 s32 main_thread_prio, u32 main_thread_core,
                                 u32 main_thread_stack_size, u64 title_id,
                                 u64 filesystem_permissions, u32 system_resource_size,
                                 KernelCapabilityDescriptors capabilities) {
    npdm_header.has_64_bit_instructions.Assign(is_64_bit);
    npdm_header.address_space_type.Assign(address_space);
    npdm_header.main_thread_priority = static_cast<u8>(main_thread_prio);
    npdm_header.main_thread_cpu = static_cast<u8>(main_thread_core);
    npdm_header.main_stack_size = main_thread_stack_size;
    aci_header.title_id = title_id;
    aci_file_access.permissions = filesystem_permissions;
    npdm_header.system_resource_size = system_resource_size;
    aci_kernel_capabilities = std::move(capabilities);
}

bool ProgramMetadata::Is64BitProgram() const {
    return bool(npdm_header.has_64_bit_instructions);
}

ProgramAddressSpaceType ProgramMetadata::GetAddressSpaceType() const {
    return npdm_header.address_space_type;
}

u8 ProgramMetadata::GetMainThreadPriority() const {
    return npdm_header.main_thread_priority;
}

u8 ProgramMetadata::GetMainThreadCore() const {
    return npdm_header.main_thread_cpu;
}

u32 ProgramMetadata::GetMainThreadStackSize() const {
    return npdm_header.main_stack_size;
}

u64 ProgramMetadata::GetTitleID() const {
    return aci_header.title_id;
}

u64 ProgramMetadata::GetFilesystemPermissions() const {
    return aci_file_access.permissions;
}

u32 ProgramMetadata::GetSystemResourceSize() const {
    return npdm_header.system_resource_size;
}

PoolPartition ProgramMetadata::GetPoolPartition() const {
    return acid_header.pool_partition;
}

const ProgramMetadata::KernelCapabilityDescriptors& ProgramMetadata::GetKernelCapabilities() const {
    return aci_kernel_capabilities;
}

void ProgramMetadata::Print() const {
    LOG_DEBUG(Service_FS, "Magic:                  {:.4}", npdm_header.magic.data());
    LOG_DEBUG(Service_FS, "Main thread priority:   {:#02x}", npdm_header.main_thread_priority);
    LOG_DEBUG(Service_FS, "Main thread core:       {}", npdm_header.main_thread_cpu);
    LOG_DEBUG(Service_FS, "Main thread stack size: {:#x} bytes", npdm_header.main_stack_size);
    LOG_DEBUG(Service_FS, "Process category:       {}", npdm_header.process_category);
    LOG_DEBUG(Service_FS, "Flags:                  {:#02x}", npdm_header.flags);
    LOG_DEBUG(Service_FS, " > 64-bit instructions: {}",
              npdm_header.has_64_bit_instructions ? "YES" : "NO");

    const char* address_space = "Unknown";
    switch (npdm_header.address_space_type) {
    case ProgramAddressSpaceType::Is36Bit:
        address_space = "64-bit (36-bit address space)";
        break;
    case ProgramAddressSpaceType::Is39Bit:
        address_space = "64-bit (39-bit address space)";
        break;
    case ProgramAddressSpaceType::Is32Bit:
        address_space = "32-bit";
        break;
    case ProgramAddressSpaceType::Is32BitNoMap:
        address_space = "32-bit (no map region)";
        break;
    }

    LOG_DEBUG(Service_FS, " > Address space:       {}\n", address_space);

    // Begin ACID printing (potential perms, signed)
    LOG_DEBUG(Service_FS, "Magic:                  {:.4}", acid_header.magic.data());
    LOG_DEBUG(Service_FS, "Flags:                  {:#02x}", acid_header.flags);
    LOG_DEBUG(Service_FS, " > Is Retail:           {}", acid_header.production_flag ? "YES" : "NO");
    LOG_DEBUG(Service_FS, "Title ID Min:           {:#016x}", acid_header.title_id_min);
    LOG_DEBUG(Service_FS, "Title ID Max:           {:#016x}", acid_header.title_id_max);
    LOG_DEBUG(Service_FS, "Filesystem Access:      {:#016x}\n", acid_file_access.permissions);

    // Begin ACI0 printing (actual perms, unsigned)
    LOG_DEBUG(Service_FS, "Magic:                  {:.4}", aci_header.magic.data());
    LOG_DEBUG(Service_FS, "Title ID:               {:#016x}", aci_header.title_id);
    LOG_DEBUG(Service_FS, "Filesystem Access:      {:#016x}\n", aci_file_access.permissions);
}
} // namespace FileSys
