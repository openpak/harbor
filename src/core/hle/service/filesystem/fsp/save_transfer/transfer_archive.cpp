// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/filesystem/fsp/save_transfer/transfer_archive.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace Service::FileSystem::SaveTransfer {

namespace {

Sha256Hash Hash(std::span<const u8> data) {
    Sha256Hash out{};
    SHA256(data.data(), data.size(), out.data());
    return out;
}

std::span<const u8> Bytes(const InitialDataContent& content) {
    return {reinterpret_cast<const u8*>(&content), sizeof(content)};
}

} // namespace

std::array<u8, 0x20> MakeAad(u64 nsa_id, u64 archive_id) {
    std::array<u8, 0x20> aad{};
    std::memcpy(aad.data(), &nsa_id, 8);
    std::memcpy(aad.data() + 8, &archive_id, 8);
    return aad;
}

std::optional<ChunkVerificationHashes> NativeChunkHashes(std::span<const u8> stream,
                                                         const std::array<u8, 0x200>& extra_data,
                                                         const std::array<u8, 0x20>& integrity_seed,
                                                         std::size_t offset, std::size_t length) {
    const u8 type = extra_data[0x20];
    const std::size_t control = type == 0 || type == 1 || type == 3 ? 0x40060 : 0;
    if (offset > stream.size() || length > stream.size() - offset || control > stream.size() ||
        stream.size() - control < 0x48) {
        return std::nullopt;
    }
    auto read32 = [&](std::size_t at) {
        u32 out;
        std::memcpy(&out, stream.data() + at, 4);
        return out;
    };
    auto read64 = [&](std::size_t at) {
        u64 out;
        std::memcpy(&out, stream.data() + at, 8);
        return out;
    };
    if (read32(control) != 0x45564153 || read32(control + 4) != 0x60000 ||
        read64(control + 0x10) != DivisionAlignment) {
        return std::nullopt;
    }
    const u64 blocks = read64(control + 8);
    if (blocks < 2 || blocks > stream.size() / DivisionAlignment)
        return std::nullopt;
    const std::size_t fat = control + 0x48;
    const std::size_t fat_size =
        ((blocks + 1) * 8 + DivisionAlignment - 1) & ~(DivisionAlignment - 1);
    const std::size_t raw = (fat + fat_size + DivisionAlignment - 1) & ~(DivisionAlignment - 1);
    if (raw > stream.size() || blocks * DivisionAlignment != stream.size() - raw)
        return std::nullopt;
    // FAT indices are one greater than raw block indices. Follow the free list's
    // bounded ranges; an allocated block full of zero bytes still has an IVFC hash.
    std::vector<bool> free(blocks, false);
    u32 head = read32(fat + 4) & 0x7fffffff;
    for (u64 visited = 0; head != 0; ++visited) {
        if (visited >= blocks || head > blocks)
            return std::nullopt;
        const u32 next = read32(fat + head * 8 + 4);
        u32 end = head;
        if ((next & 0x80000000) != 0) {
            if (head >= blocks)
                return std::nullopt;
            end = read32(fat + (head + 1) * 8 + 4) & 0x7fffffff;
        }
        if (end < head || end > blocks)
            return std::nullopt;
        for (u32 i = head; i <= end; ++i) {
            if (free[i - 1])
                return std::nullopt;
            free[i - 1] = true;
        }
        head = next & 0x7fffffff;
    }
    constexpr std::string_view domain = "HierarchicalIntegrityVerificationStorage::L3";
    Sha256Hash salt{};
    unsigned int salt_size = 0;
    HMAC(EVP_sha256(), domain.data(), domain.size(), integrity_seed.data(), integrity_seed.size(),
         salt.data(), &salt_size);
    if (salt_size != salt.size())
        return std::nullopt;
    std::vector<u8> represented;
    const std::size_t end = offset + length;
    if (offset < raw) {
        const auto prefix_end = std::min(raw, end);
        represented.insert(represented.end(), stream.begin() + offset, stream.begin() + prefix_end);
    }
    if (end > raw) {
        const std::size_t first = (std::max(raw, offset) - raw) / DivisionAlignment;
        const std::size_t last = (end - raw + DivisionAlignment - 1) / DivisionAlignment;
        std::vector<u8> salted(32 + DivisionAlignment);
        std::copy(salt.begin(), salt.end(), salted.begin());
        for (std::size_t block = first; block < last; ++block) {
            Sha256Hash hash{};
            if (!free[block]) {
                std::memcpy(salted.data() + 32, stream.data() + raw + block * DivisionAlignment,
                            DivisionAlignment);
                hash = Hash(salted);
                hash.back() |= 0x80;
            }
            represented.insert(represented.end(), hash.begin(), hash.end());
        }
    }
    ChunkVerificationHashes out;
    out.full = Hash(represented);
    const auto bytes = std::span<const u8>{represented};
    const auto left = Hash(bytes.first(bytes.size() / 2));
    const auto right = Hash(bytes.subspan(bytes.size() / 2));
    std::copy_n(left.begin(), 4, out.halves.begin());
    std::copy_n(right.begin(), 4, out.halves.begin() + 4);
    return out;
}

ArchiveWriter::ArchiveWriter(ArchiveSource source_, const Key128& stream_key_,
                             const Key128& mac_key_)
    : source{std::move(source_)}, stream_key{stream_key_}, mac_key{mac_key_} {
    content.extra_data = source.extra_data;
    content.hash_salt = source.integrity_seed;
    content.division_alignment = DivisionAlignment;
    content.total_stream_size = static_cast<s64>(source.stream.size());
    // As the console's exporter: every IV drawn once, at construction (0xc15f0).
    for (auto& iv : ivs) {
        RandomBytes(iv);
    }
    RandomBytes(meta_iv);
}

bool ArchiveWriter::SetDivisionCount(int count) {
    if (count < 1 || count > MaxDivisionCount || !slices.empty()) {
        return false;
    }
    // Whole blocks, spread as evenly as the console's ChunkSizeCalculator spreads them (the
    // first chunks one block shorter than the rest).
    const std::size_t total = source.stream.size();
    const std::size_t blocks =
        (total + static_cast<std::size_t>(DivisionAlignment) - 1) / DivisionAlignment;
    const std::size_t base = blocks / count;
    const std::size_t longer = blocks % count;
    std::size_t at = 0;
    for (int n = 0; n < count; ++n) {
        const std::size_t length_blocks =
            base + (static_cast<std::size_t>(n) >= static_cast<std::size_t>(count) - longer ? 1 : 0);
        const std::size_t length =
            std::min(length_blocks * DivisionAlignment, total - std::min(at, total));
        slices.emplace_back(at, length);
        at += length;
    }
    content.division_count = count;
    chunks.assign(count + 1, std::nullopt);
    unchanged.assign(count, false);
    return true;
}

bool ArchiveWriter::SetBase(const InitialDataContent& base) {
    if (!SetDivisionCount(base.division_count)) {
        return false;
    }
    content.aad = base.aad;
    // A legacy Harbor archive used plaintext hashes. Either matching representation
    // allows reuse of its encrypted bytes; new metadata always records native hashes.
    for (int n = 0; n < content.division_count; ++n) {
        const auto [at, length] = slices[n];
        const auto verified =
            NativeChunkHashes(source.stream, source.extra_data, source.integrity_seed, at, length);
        const bool same = (verified && verified->full == base.chunk_hashes[n]) ||
                          Hash({source.stream.data() + at, length}) == base.chunk_hashes[n];
        if (base.chunk_sizes[n] > 0 && same) {
            unchanged[n] = true;
            content.chunk_hashes[n] = verified ? verified->full : base.chunk_hashes[n];
            content.chunk_sizes[n] = base.chunk_sizes[n];
            content.chunk_macs[n] = base.chunk_macs[n];
            if (verified) {
                std::copy(verified->halves.begin(), verified->halves.end(),
                          content.short_hashes.begin() + n * 8);
            }
        }
    }
    return true;
}

int ArchiveWriter::DivisionCount() const {
    return content.division_count;
}

std::vector<u16> ArchiveWriter::ChunkIds() const {
    std::vector<u16> ids;
    for (int n = 0; n < content.division_count; ++n) {
        if (!unchanged[n]) {
            ids.push_back(static_cast<u16>(FirstDataChunkId + n));
        }
    }
    ids.push_back(MetaChunkId);
    return ids;
}

void ArchiveWriter::MakeAll() {
    for (int n = 0; n < content.division_count; ++n) {
        if (!unchanged[n]) {
            Chunk(static_cast<u16>(FirstDataChunkId + n));
        }
    }
}

std::optional<std::vector<u8>> ArchiveWriter::Chunk(u16 id) {
    if (content.division_count == 0) {
        return std::nullopt;
    }
    if (id == MetaChunkId) {
        MakeAll();
        auto& meta = chunks.back();
        if (!meta) {
            meta = EncryptStream(stream_key, meta_iv, StreamKeyGeneration, Bytes(content));
        }
        return meta;
    }
    const int n = id - FirstDataChunkId;
    if (id < FirstDataChunkId || n >= content.division_count || unchanged[n]) {
        return std::nullopt;
    }
    auto& chunk = chunks[n];
    if (!chunk) {
        const auto [at, length] = slices[n];
        const std::span<const u8> plain{source.stream.data() + at, length};
        Key128 tag{};
        chunk = EncryptStream(stream_key, ivs[n], StreamKeyGeneration, RawDeflate(plain), &tag);
        const auto verified =
            NativeChunkHashes(source.stream, source.extra_data, source.integrity_seed, at, length);
        content.chunk_hashes[n] = verified ? verified->full : Hash(plain);
        if (verified) {
            std::copy(verified->halves.begin(), verified->halves.end(),
                      content.short_hashes.begin() + n * 8);
        }
        content.chunk_sizes[n] = static_cast<s64>(chunk->size());
        content.chunk_macs[n] = tag;
    }
    return chunk;
}

void ArchiveWriter::SetAad(const std::array<u8, 0x20>& aad) {
    content.aad = aad;
}

const std::array<u8, 0x20>& ArchiveWriter::Aad() const {
    return content.aad;
}

Key128 ArchiveWriter::InitialDataMac() {
    MakeAll();
    return Cmac(mac_key, Bytes(content));
}

const InitialDataContent& ArchiveWriter::Content() {
    MakeAll();
    return content;
}

ArchiveReader::Error ArchiveReader::Open(std::span<const u8> meta, const Key128& stream_key_,
                                         const Key128& mac_key,
                                         const std::optional<Key128>& expected_mac) {
    stream_key = stream_key_;
    const auto plain_meta = DecryptStream(stream_key, meta);
    if (!plain_meta || plain_meta->size() != sizeof(content)) {
        return Error::Meta;
    }
    std::memcpy(&content, plain_meta->data(), sizeof(content));
    if (content.magic != InitialDataContent{}.magic || content.version != 2 ||
        content.division_count < 1 || content.division_count > MaxDivisionCount) {
        return Error::Meta;
    }
    if (expected_mac && Cmac(mac_key, Bytes(content)) != *expected_mac) {
        return Error::Mac;
    }
    plain.assign(content.division_count, std::nullopt);
    return Error::None;
}

const InitialDataContent& ArchiveReader::Content() const {
    return content;
}

std::vector<u16> ArchiveReader::ChunkIds() const {
    std::vector<u16> ids;
    for (int n = 0; n < content.division_count; ++n) {
        ids.push_back(static_cast<u16>(FirstDataChunkId + n));
    }
    return ids;
}

ArchiveReader::Error ArchiveReader::Push(u16 id, std::span<const u8> bytes) {
    const int n = id - FirstDataChunkId;
    if (id < FirstDataChunkId || n >= content.division_count || plain[n]) {
        return Error::UnknownId;
    }
    if (static_cast<s64>(bytes.size()) != content.chunk_sizes[n] ||
        bytes.size() < sizeof(AesGcmStreamHeader) + StreamTagSize) {
        return Error::Chunk;
    }
    Key128 tag{};
    std::memcpy(tag.data(), bytes.data() + bytes.size() - StreamTagSize, tag.size());
    if (tag != content.chunk_macs[n]) {
        return Error::Chunk;
    }
    const auto packed = DecryptStream(stream_key, bytes);
    if (!packed) {
        return Error::Chunk;
    }
    auto inflated = RawInflate(*packed, 1ULL << 32);
    if (!inflated) {
        return Error::Chunk;
    }
    plain[n] = std::move(*inflated);
    if (std::all_of(plain.begin(), plain.end(), [](const auto& p) { return p.has_value(); }) &&
        !Complete()) {
        plain[n].reset();
        return Error::Chunk;
    }
    return Error::None;
}

bool ArchiveReader::Complete() const {
    if (plain.empty() ||
        !std::all_of(plain.begin(), plain.end(), [](const auto& p) { return p.has_value(); })) {
        return false;
    }
    bool legacy = true;
    for (std::size_t n = 0; n < plain.size(); ++n) {
        if (Hash(*plain[n]) != content.chunk_hashes[n]) {
            legacy = false;
            break;
        }
    }
    if (legacy)
        return true;
    const auto stream = Stream();
    std::size_t at = 0;
    for (std::size_t n = 0; n < plain.size(); ++n) {
        const std::size_t length = plain[n]->size();
        const auto verified =
            NativeChunkHashes(stream, content.extra_data, content.hash_salt, at, length);
        if (!verified || verified->full != content.chunk_hashes[n] ||
            !std::equal(verified->halves.begin(), verified->halves.end(),
                        content.short_hashes.begin() + n * 8)) {
            return false;
        }
        at += length;
    }
    return true;
}

std::vector<u8> ArchiveReader::Stream() const {
    std::vector<u8> out;
    for (const auto& piece : plain) {
        if (piece) {
            out.insert(out.end(), piece->begin(), piece->end());
        }
    }
    return out;
}

} // namespace Service::FileSystem::SaveTransfer
