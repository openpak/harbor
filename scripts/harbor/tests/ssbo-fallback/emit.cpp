// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
int main(int argc, char** argv) {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.min_ssbo_alignment = 256;
    Shader::RuntimeInfo runtime{};
    Shader::IR::Program program{};
    program.stage = Shader::Stage::Compute;
    program.info.uses_global_memory = true;
    program.info.uses_int64 = true;
    program.info.used_constant_buffer_types = Shader::IR::Type::U32 | Shader::IR::Type::U32x2;
    program.info.used_storage_buffer_types = Shader::IR::Type::U32 | Shader::IR::Type::U32x2 | Shader::IR::Type::U32x4;
    program.info.constant_buffer_descriptors.push_back({0, 1});
    program.info.storage_buffers_descriptors.push_back({0, 0x100, 1, true});
    program.info.nvn_buffer_used.set(0);
    Shader::Backend::Bindings bindings{};
    Shader::Backend::SPIRV::EmitContext ctx(profile, runtime, program, bindings);
    auto code = ctx.Assemble();
    std::ofstream out(argv[1], std::ios::binary);
    out.write(reinterpret_cast<const char*>(code.data()), code.size()*4);
}
