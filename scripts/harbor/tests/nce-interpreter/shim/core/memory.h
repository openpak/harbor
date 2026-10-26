// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once
#include <array>
#include <cassert>
#include <cstring>
#include "common/common_types.h"
namespace Core::Memory {
class Memory {
public:
    std::array<u8, 16384> bytes{};
    unsigned reads{}, writes{};
    void WriteBlock(u64 address, const void* data, size_t size) {
        assert(address + size <= bytes.size()); ++writes;
        std::memcpy(bytes.data()+address, data, size);
    }
    void ReadBlock(u64 address, void* data, size_t size) {
        assert(address + size <= bytes.size()); ++reads;
        std::memcpy(data, bytes.data()+address, size);
    }
    u32 Read32(u64 address) { u32 v; ReadBlock(address,&v,sizeof(v)); return v; }
};
}
