// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/filesystem/fsp/save_transfer/transfer_archive.h"

#include <algorithm>
#include <cstring>

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

std::array<u8, 0x20> MakeAad(u64 application_id, u64 archive_id) {
    std::array<u8, 0x20> aad{};
    std::memcpy(aad.data(), &application_id, 8);
    std::memcpy(aad.data() + 8, &archive_id, 8);
    return aad;
}

ArchiveWriter::ArchiveWriter(ArchiveSource source_, const Key128& stream_key_,
                             const Key128& mac_key_)
    : source{std::move(source_)}, stream_key{stream_key_}, mac_key{mac_key_} {
    content.extra_data = source.extra_data;
    content.hash_salt = source.integrity_seed;
    content.division_alignment = DivisionAlignment;
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
    return true;
}

int ArchiveWriter::DivisionCount() const {
    return content.division_count;
}

std::vector<u16> ArchiveWriter::ChunkIds() const {
    std::vector<u16> ids;
    for (int n = 0; n < content.division_count; ++n) {
        ids.push_back(static_cast<u16>(FirstDataChunkId + n));
    }
    ids.push_back(MetaChunkId);
    return ids;
}

void ArchiveWriter::MakeAll() {
    for (int n = 0; n < content.division_count; ++n) {
        Chunk(static_cast<u16>(FirstDataChunkId + n));
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
    if (id < FirstDataChunkId || n >= content.division_count) {
        return std::nullopt;
    }
    auto& chunk = chunks[n];
    if (!chunk) {
        const auto [at, length] = slices[n];
        const std::span<const u8> plain{source.stream.data() + at, length};
        Key128 tag{};
        chunk = EncryptStream(stream_key, ivs[n], StreamKeyGeneration, RawDeflate(plain), &tag);
        content.chunk_hashes[n] = Hash(plain);
        content.chunk_sizes[n] = static_cast<s64>(chunk->size());
        content.chunk_macs[n] = tag;
        content.total_chunk_size += static_cast<s64>(chunk->size());
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
    if (!inflated || Hash(*inflated) != content.chunk_hashes[n]) {
        return Error::Chunk;
    }
    plain[n] = std::move(*inflated);
    return Error::None;
}

bool ArchiveReader::Complete() const {
    return !plain.empty() &&
           std::all_of(plain.begin(), plain.end(), [](const auto& p) { return p.has_value(); });
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
