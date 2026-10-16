// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <iostream>
#include <optional>
#include <stdexcept>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TestEnvironment final : public Shader::Environment {
public:
    u32 base = 0x337f8884;
    u32 storage_reads{};
    u32 ReadCbufValue(u32 index, u32 offset) override {
        Check(index == 0, "unexpected constant buffer");
        return offset == 0x510 ? base : offset == 0x514 ? 5 : 3;
    }
    u32 ReadStorageBufferValue(u32 index, u32 offset, u32 byte_offset) override {
        Check(index == 0 && offset == 0x510, "incorrect storage buffer source");
        Check(byte_offset == 0 || byte_offset == 16 || byte_offset == 32,
              "incorrect texture member offset");
        ++storage_reads;
        return 3;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        Check(handle == 3, "buffer pointer mistaken for a texture handle");
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        Check(handle == 3, "incorrect texture format handle");
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        Check(handle == 3, "incorrect integer-format handle");
        return false;
    }
    u64 ReadInstruction(u32) override {
        return 0;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

u32 Evaluate(const IR::Value& value, TestEnvironment& env, u32 input) {
    if (value.IsImmediate()) {
        return value.U32();
    }
    const auto* inst = value.InstRecursive();
    const auto arg = [&](size_t n) { return Evaluate(inst->Arg(n), env, input); };
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetAttributeU32:
        return input;
    case IR::Opcode::GetCbufU32:
        return env.ReadCbufValue(arg(0), arg(1));
    case IR::Opcode::IAdd32:
        return arg(0) + arg(1);
    case IR::Opcode::ISub32:
        return arg(0) - arg(1);
    case IR::Opcode::IMul32:
        return arg(0) * arg(1);
    case IR::Opcode::UDiv32:
        return arg(0) / arg(1);
    case IR::Opcode::BitwiseAnd32:
        return arg(0) & arg(1);
    case IR::Opcode::ShiftLeftLogical32:
        return arg(0) << arg(1);
    case IR::Opcode::ShiftRightLogical32:
        return arg(0) >> arg(1);
    case IR::Opcode::UMin32:
        return std::min(arg(0), arg(1));
    case IR::Opcode::BitFieldUExtract:
        return (arg(0) >> arg(1)) & ((1U << arg(2)) - 1);
    default:
        throw std::runtime_error("unexpected array-index opcode");
    }
}

struct Fixture {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    IR::Program program;
    TestEnvironment env;
    Shader::HostTranslateInfo host;

    Fixture() {
        program.post_order_blocks.push_back(&block);
        program.info.storage_buffers_descriptors.push_back({0, 0x510, 1, false});
        host.min_ssbo_alignment = 64;
        host.max_per_stage_descriptor_samplers = 128;
        host.max_per_stage_descriptor_sampled_images = 128;
        host.max_descriptor_set_sampled_images = 128;
        host.max_per_stage_resources = 128;
        host.ApplyDescriptorLimitPolicy();
    }

    IR::Inst* Sample(const IR::Value& handle) {
        IR::TextureInstInfo info{};
        info.type.Assign(Shader::TextureType::Color2D);
        return ir
            .ImageSampleImplicitLod(handle, ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f)),
                                    {}, {}, {}, info)
            .Inst();
    }

    IR::U32 StorageHandle(u32 member, bool dynamic = true, u32 stride = 48) {
        const auto base = ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x510U));
        IR::U32 offset = ir.Imm32(member);
        if (dynamic) {
            // Maxwell XMAD computes the low and high halves of index * stride.
            const auto input = ir.GetAttributeU32(IR::Attribute::Generic0X);
            const auto low =
                ir.IMul(ir.BitFieldExtract(input, ir.Imm32(0U), ir.Imm32(16U)), ir.Imm32(stride));
            const auto high = ir.ShiftLeftLogical(
                ir.IMul(ir.BitFieldExtract(input, ir.Imm32(16U), ir.Imm32(16U)), ir.Imm32(stride)),
                ir.Imm32(16U));
            offset = ir.IAdd(ir.IAdd(low, high), offset);
        }
        offset = ir.ISub(ir.IAdd(base, offset), ir.BitwiseAnd(base, ir.Imm32(~63U)));
        block.AppendNewInst(IR::Opcode::LoadStorage32, {ir.Imm32(0U), offset});
        return IR::U32{IR::Value{&block.back()}};
    }

    void Run() {
        Shader::Optimization::TexturePass(env, program, host);
    }
};

void DynamicStorageTexture(u32 member, u32 sampler_limit = 128) {
    Fixture f;
    f.host.max_per_stage_descriptor_samplers = sampler_limit;
    const auto* sample = f.Sample(f.StorageHandle(member));
    f.Run();
    const auto& descriptors = f.program.info.texture_descriptors;
    Check(descriptors.size() == 1, "missing texture array");
    const auto& desc = descriptors.front();
    Check(desc.cbuf_index == 0 && desc.cbuf_offset == 0x510, "wrong pointer source");
    Check(desc.storage_stride == 48 && desc.storage_offset == member, "wrong record layout");
    Check(desc.count == std::min(126U, sampler_limit), "host descriptor budget ignored");
    for (const u32 base : {0x337f8880U, 0x337f8884U, 0x337f88bcU}) {
        f.env.base = base;
        Check(Evaluate(sample->Arg(0), f.env, 0) == 0, "incorrect first array index");
        Check(Evaluate(sample->Arg(0), f.env, 5) == 5, "alignment or record stride error");
        Check(Evaluate(sample->Arg(0), f.env, 100000) == desc.count - 1, "array index not bounded");
    }
    Check(f.env.storage_reads != 0, "storage handle was not read");
}

void StaticStorageTexture() {
    Fixture f;
    const auto* sample = f.Sample(f.StorageHandle(16, false));
    f.Run();
    const auto& desc = f.program.info.texture_descriptors.front();
    Check(desc.count == 1 && desc.storage_offset == 16, "incorrect static storage texture");
    Check(sample->Arg(0).IsEmpty(), "static storage texture uses dynamic indexing");
}

void ConstantBufferTexture() {
    Fixture f;
    f.Sample(f.ir.GetCbuf(f.ir.Imm32(0U), f.ir.Imm32(0x100U)));
    f.Run();
    const auto& desc = f.program.info.texture_descriptors.front();
    Check(desc.count == 1 && desc.storage_stride == 0, "direct texture binding changed");
    Check(f.env.storage_reads == 0, "direct texture unexpectedly dereferenced");
}

void UnsupportedStorageTexture() {
    Fixture f;
    f.Sample(f.StorageHandle(0, true, 1));
    bool rejected = false;
    try {
        f.Run();
    } catch (const Shader::NotImplementedException&) {
        rejected = true;
    }
    Check(rejected, "unsupported memory load followed into its pointer");
}
} // namespace

int main() {
    try {
        DynamicStorageTexture(0);
        DynamicStorageTexture(16);
        DynamicStorageTexture(32);
        DynamicStorageTexture(32, 32);
        StaticStorageTexture();
        ConstantBufferTexture();
        UnsupportedStorageTexture();
        std::cout << "Storage texture regression checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
