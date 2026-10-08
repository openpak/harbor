// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The transfer stream of a save (23.0.1 FS 0xc8700): [thumbnail] ∥ control area ∥ allocation
// table ∥ zero padding to 0x4000 ∥ block area. Built from and parsed into Eden host-directory
// saves through the core codec (save_image_core.h).

#include <algorithm>
#include <cstring>
#include <memory>

#include <openssl/evp.h>

#include "common/logging.h"
#include "core/file_sys/save_image/save_image.h"
#include "core/file_sys/save_image/save_image_core.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys::SaveImage {

namespace {

/// Eden keeps this host file inside every save directory; it is not part of the save.
constexpr std::string_view HostSaveSizeFile = ".yuzu_save_size";

bool HasThumbnail(u8 save_data_type) {
    // 0xc8700: Account (1) and Device (3) carry their meta-file thumbnail, System (0) a zero
    // one of the same size, every other type none.
    return save_data_type == 0 || save_data_type == 1 || save_data_type == 3;
}

/// Element sizes of the stream for a geometry.
struct StreamLayout {
    u64 thumbnail;
    u64 control_offset;
    u64 fat_offset;
    u64 padding;
    u64 raw_offset;
    u64 total;
};

StreamLayout LayoutFor(const CoreGeometry& g, u8 save_data_type) {
    StreamLayout l{};
    l.thumbnail = HasThumbnail(save_data_type) ? ThumbnailSize : 0;
    l.control_offset = l.thumbnail;
    l.fat_offset = l.control_offset + ControlAreaSize;
    const u64 before_pad = l.fat_offset + static_cast<u64>(g.fat_size);
    l.padding = (0 - before_pad) & (BlockSize - 1);
    l.raw_offset = before_pad + l.padding;
    l.total = l.raw_offset + static_cast<u64>(g.RawSize());
    return l;
}

/// The block area as a read-only file over the sparse core image.
class RawAreaFile final : public VfsFile {
public:
    explicit RawAreaFile(std::shared_ptr<const CoreImage> image_) : image{std::move(image_)} {}

    std::string GetName() const override {
        return "RawSaveDataWithZeroFree";
    }
    std::size_t GetSize() const override {
        return static_cast<std::size_t>(image->geometry.RawSize());
    }
    bool Resize(std::size_t) override {
        return false;
    }
    VirtualDir GetContainingDirectory() const override {
        return nullptr;
    }
    bool IsWritable() const override {
        return false;
    }
    bool IsReadable() const override {
        return true;
    }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        return image->ReadRaw(data, length, offset);
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view) override {
        return false;
    }

private:
    std::shared_ptr<const CoreImage> image;
};

bool ReadTree(const VirtualDir& dir, TreeDir& out, int depth) {
    if (depth > 64) {
        return false;
    }
    for (const auto& file : dir->GetFiles()) {
        if (depth == 0 && file->GetName() == HostSaveSizeFile) {
            continue;
        }
        auto data = file->ReadAllBytes();
        if (data.size() != file->GetSize()) {
            return false;
        }
        out.files.emplace_back(file->GetName(), std::move(data));
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        TreeDir child;
        if (!ReadTree(sub, child, depth + 1)) {
            return false;
        }
        out.dirs.emplace_back(sub->GetName(), std::move(child));
    }
    return true;
}

bool WriteTree(const VirtualDir& dir, const TreeDir& tree) {
    for (const auto& [name, data] : tree.files) {
        const auto file = dir->CreateFile(name);
        if (file == nullptr || !file->Resize(data.size()) ||
            (!data.empty() && file->Write(data.data(), data.size(), 0) != data.size())) {
            return false;
        }
    }
    for (const auto& [name, sub] : tree.dirs) {
        const auto child = dir->CreateSubdirectory(name);
        if (child == nullptr || !WriteTree(child, sub)) {
            return false;
        }
    }
    return true;
}

bool ClearDirectory(const VirtualDir& dir) {
    bool ok = true;
    for (const auto& file : dir->GetFiles()) {
        if (file->GetName() != HostSaveSizeFile) {
            ok &= dir->DeleteFile(file->GetName());
        }
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        ok &= dir->DeleteSubdirectoryRecursive(sub->GetName());
    }
    return ok;
}

/// A deterministic integrity seed: SHA-256 over the control area, the allocation table and the
/// used blocks. Any 32 bytes are valid; the console's own saves use random ones.
std::array<u8, 0x20> SeedFor(const CoreImage& image) {
    std::array<u8, 0x20> seed{};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return seed;
    }
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, image.control.data(), image.control.size());
    EVP_DigestUpdate(ctx, image.fat.data(), image.fat.size());
    for (const auto& [index, block] : image.blocks) {
        EVP_DigestUpdate(ctx, &index, sizeof(index));
        EVP_DigestUpdate(ctx, block.data(), block.size());
    }
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, seed.data(), &len);
    EVP_MD_CTX_free(ctx);
    return seed;
}

} // namespace

std::optional<ExportedImage> ExportSaveImage(const VirtualDir& save_dir,
                                             const SaveGeometry& geometry,
                                             std::span<const u8> thumbnail) {
    const auto core_geometry = CoreGeometryFor(geometry.data_size);
    if (save_dir == nullptr || !core_geometry) {
        LOG_WARNING(Service_FS, "save image: no save directory or invalid data size {:#x}",
                    geometry.data_size);
        return std::nullopt;
    }
    TreeDir tree;
    if (!ReadTree(save_dir, tree, 0)) {
        LOG_WARNING(Service_FS, "save image: could not read the save directory");
        return std::nullopt;
    }
    std::string error;
    auto image = BuildCore(tree, *core_geometry, &error);
    if (!image) {
        LOG_WARNING(Service_FS, "save image: cannot build the image: {}", error);
        return std::nullopt;
    }
    const auto layout = LayoutFor(*core_geometry, geometry.save_data_type);
    auto shared = std::make_shared<const CoreImage>(std::move(*image));

    ExportedImage out;
    out.parameters.hash_algorithm = 0;     // layout[0x15d] of a v5 save
    out.parameters.hash_salt_enabled = true; // !layout[0x15c] of a v5 save
    out.parameters.integrity_seed = SeedFor(*shared);
    if (layout.thumbnail != 0) {
        std::vector<u8> thumb(static_cast<std::size_t>(layout.thumbnail), 0);
        std::copy_n(thumbnail.begin(), std::min(thumbnail.size(), thumb.size()), thumb.begin());
        out.storages.emplace_back(InternalStorage::Thumbnail,
                                  std::make_shared<VectorVfsFile>(std::move(thumb), "Thumbnail"));
    }
    out.storages.emplace_back(
        InternalStorage::SaveDataControlArea,
        std::make_shared<VectorVfsFile>(
            std::vector<u8>(shared->control.begin(), shared->control.end()),
            "SaveDataControlArea"));
    out.storages.emplace_back(InternalStorage::AllocationTableMeta,
                              std::make_shared<VectorVfsFile>(shared->fat, "AllocationTableMeta"));
    out.storages.emplace_back(
        InternalStorage::Padding,
        std::make_shared<VectorVfsFile>(
            std::vector<u8>(static_cast<std::size_t>(layout.padding), 0), "Padding"));
    out.storages.emplace_back(InternalStorage::RawSaveDataWithZeroFree,
                              std::make_shared<RawAreaFile>(shared));
    LOG_INFO(Service_FS, "save image: exported {} blocks of {} ({:#x} stream bytes)",
             shared->blocks.size(), core_geometry->block_count, layout.total);
    return out;
}

bool ImportSaveImage(const VirtualFile& stream, const ImageParameters&,
                     const SaveGeometry& geometry, const VirtualDir& save_dir,
                     std::vector<u8>* out_thumbnail) {
    const auto core_geometry = CoreGeometryFor(geometry.data_size);
    if (stream == nullptr || save_dir == nullptr || !core_geometry) {
        LOG_WARNING(Service_FS, "save image: no stream/directory or invalid data size {:#x}",
                    geometry.data_size);
        return false;
    }
    const auto layout = LayoutFor(*core_geometry, geometry.save_data_type);
    if (stream->GetSize() < layout.total) {
        LOG_WARNING(Service_FS, "save image: stream is {:#x} bytes, the geometry needs {:#x}",
                    stream->GetSize(), layout.total);
        return false;
    }
    const auto control = stream->ReadBytes(ControlAreaSize, layout.control_offset);
    const auto fat =
        stream->ReadBytes(static_cast<std::size_t>(core_geometry->fat_size), layout.fat_offset);
    const RawReader raw = [&](u8* out, std::size_t length, u64 offset) {
        return stream->Read(out, length, layout.raw_offset + offset) == length;
    };
    std::string error;
    auto tree = ParseCore(control, fat, raw, static_cast<u64>(core_geometry->RawSize()), &error);
    if (!tree) {
        LOG_WARNING(Service_FS, "save image: the stream does not parse: {}", error);
        return false;
    }
    if (out_thumbnail != nullptr) {
        *out_thumbnail = stream->ReadBytes(static_cast<std::size_t>(layout.thumbnail), 0);
    }
    if (!ClearDirectory(save_dir) || !WriteTree(save_dir, *tree)) {
        LOG_ERROR(Service_FS, "save image: could not write the imported save");
        return false;
    }
    return true;
}

} // namespace FileSys::SaveImage
