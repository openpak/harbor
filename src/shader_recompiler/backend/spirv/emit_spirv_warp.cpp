// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
Id SubgroupScope(EmitContext& ctx) {
    return ctx.Const(static_cast<u32>(spv::Scope::Subgroup));
}

bool StageSupportsSubgroups(EmitContext& ctx) {
    return ctx.profile.SupportsSubgroupStage(ctx.stage);
}

Id GetThreadId(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    return ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
}

Id WarpExtract(EmitContext& ctx, Id value) {
    const Id thread_id{GetThreadId(ctx)};
    const Id local_index{ctx.OpShiftRightArithmetic(ctx.U32[1], thread_id, ctx.Const(5U))};
    if (ctx.profile.has_broken_spirv_subgroup_mask_vector_extract_dynamic) {
        const Id c0_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(0U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 0U), ctx.Const(0U))};
        const Id c1_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(1U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 1U), ctx.Const(0U))};
        const Id c2_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(2U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 2U), ctx.Const(0U))};
        const Id c3_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(3U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 3U), ctx.Const(0U))};
        const Id c0_or_c1{ctx.OpBitwiseOr(ctx.U32[1], c0_sel, c1_sel)};
        const Id c2_or_c3{ctx.OpBitwiseOr(ctx.U32[1], c2_sel, c3_sel)};
        const Id c0_or_c1_or_c2_or_c3{ctx.OpBitwiseOr(ctx.U32[1], c0_or_c1, c2_or_c3)};
        return c0_or_c1_or_c2_or_c3;
    } else {
        return ctx.OpVectorExtractDynamic(ctx.U32[1], value, local_index);
    }
}

Id LoadMask(EmitContext& ctx, Id mask) {
    const Id value{ctx.OpLoad(ctx.U32[4], mask)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpCompositeExtract(ctx.U32[1], value, 0U);
    }
    return WarpExtract(ctx, value);
}

void SetInBoundsFlag(IR::Inst* inst, Id result) {
    IR::Inst* const in_bounds{inst->GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)};
    if (!in_bounds) {
        return;
    }
    in_bounds->SetDefinition(result);
    in_bounds->Invalidate();
}

Id ComputeMinThreadId(EmitContext& ctx, Id thread_id, Id segmentation_mask) {
    return ctx.OpBitwiseAnd(ctx.U32[1], thread_id, segmentation_mask);
}

Id ComputeMaxThreadId(EmitContext& ctx, Id min_thread_id, Id clamp, Id not_seg_mask) {
    return ctx.OpBitwiseOr(ctx.U32[1], min_thread_id,
                           ctx.OpBitwiseAnd(ctx.U32[1], clamp, not_seg_mask));
}

Id GetMaxThreadId(EmitContext& ctx, Id thread_id, Id clamp, Id segmentation_mask) {
    const Id not_seg_mask{ctx.OpNot(ctx.U32[1], segmentation_mask)};
    const Id min_thread_id{ComputeMinThreadId(ctx, thread_id, segmentation_mask)};
    return ComputeMaxThreadId(ctx, min_thread_id, clamp, not_seg_mask);
}

Id SelectValue(EmitContext& ctx, Id in_range, Id value, Id src_thread_id) {
    if (!StageSupportsSubgroups(ctx)) {
        return value;
    }
    return ctx.OpSelect(
        ctx.U32[1], in_range,
        ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, src_thread_id), value);
}

// Harbor: quad shuffles in fragment shaders are lowered with fine derivatives instead of subgroup
// operations. GPUs without a native shuffle (Adreno 6xx before the a660: Turnip's ir3 turns the
// shuffle into a loop over the lanes it reads) can spin forever when part of the 2x2 quad is
// helper pixels, which hung the GPU (VK_ERROR_DEVICE_LOST) in Dinkum. Derivatives are defined
// over exactly that quad, helper pixels included, on every GPU, so this is used everywhere.

bool LowerFragmentQuad(EmitContext& ctx) {
    return ctx.stage == Stage::Fragment && ctx.profile.support_derivative_control &&
           ctx.input_position.value != 0;
}

/// Whether a shuffle never leaves the 2x2 quad: lane bits 2-4 of the source come from the
/// invocation's own lane (an immediate segmentation mask covering them).
bool ShuffleStaysInQuad(EmitContext& ctx, IR::Inst* inst) {
    if (!LowerFragmentQuad(ctx)) {
        return false;
    }
    const IR::Value segmentation_mask{inst->Arg(3)};
    return segmentation_mask.IsImmediate() && (segmentation_mask.U32() & 0x1cu) == 0x1cu;
}

/// The pixel's position in its quad as a lane index: bit 0 = x, bit 1 = y (the guest's and
/// Vulkan's quad order).
Id QuadPosition(EmitContext& ctx) {
    const Id frag_coord{ctx.OpLoad(ctx.F32[4], ctx.input_position)};
    const Id x{ctx.OpConvertFToU(ctx.U32[1], ctx.OpCompositeExtract(ctx.F32[1], frag_coord, 0U))};
    const Id y{ctx.OpConvertFToU(ctx.U32[1], ctx.OpCompositeExtract(ctx.F32[1], frag_coord, 1U))};
    const Id one{ctx.Const(1U)};
    return ctx.OpBitwiseOr(ctx.U32[1], ctx.OpBitwiseAnd(ctx.U32[1], x, one),
                           ctx.OpShiftLeftLogical(ctx.U32[1], ctx.OpBitwiseAnd(ctx.U32[1], y, one),
                                                  one));
}

/// `value` as held by quad lane `target` (0-3), read through fine derivatives. Bit-exact: each
/// 16-bit half travels as a float holding an integer below 2^16, where every sum and difference
/// below is exact, and the result is rounded back.
Id QuadRead(EmitContext& ctx, Id value, Id target, Id position) {
    const Id one{ctx.Const(1U)};
    const Id own_x{ctx.OpConvertUToF(ctx.F32[1], ctx.OpBitwiseAnd(ctx.U32[1], position, one))};
    const Id own_y{ctx.OpConvertUToF(ctx.F32[1], ctx.OpShiftRightLogical(ctx.U32[1], position, one))};
    const Id target_x{ctx.OpConvertUToF(ctx.F32[1], ctx.OpBitwiseAnd(ctx.U32[1], target, one))};
    const Id target_y{ctx.OpConvertUToF(
        ctx.F32[1], ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpShiftRightLogical(ctx.U32[1], target, one), one))};
    const Id step_x{ctx.OpFSub(ctx.F32[1], target_x, own_x)}; // -1, 0 or 1
    const Id step_y{ctx.OpFSub(ctx.F32[1], target_y, own_y)};

    const auto read_half{[&](Id half) {
        const Id dx{ctx.OpDPdxFine(ctx.F32[1], half)};   // right - left, in this row
        const Id dy{ctx.OpDPdyFine(ctx.F32[1], half)};   // bottom - top, in this column
        const Id dxy{ctx.OpDPdyFine(ctx.F32[1], dx)};    // the other column's dy minus this one's
        const Id dy_target{ctx.OpFAdd(ctx.F32[1], dy, ctx.OpFMul(ctx.F32[1], step_x, dxy))};
        Id result{ctx.OpFAdd(ctx.F32[1], half, ctx.OpFMul(ctx.F32[1], step_x, dx))};
        result = ctx.OpFAdd(ctx.F32[1], result, ctx.OpFMul(ctx.F32[1], step_y, dy_target));
        result = ctx.OpFClamp(ctx.F32[1], ctx.OpRoundEven(ctx.F32[1], result), ctx.Const(0.0f),
                              ctx.Const(65535.0f));
        return ctx.OpConvertFToU(ctx.U32[1], result);
    }};
    const Id low{read_half(
        ctx.OpConvertUToF(ctx.F32[1], ctx.OpBitwiseAnd(ctx.U32[1], value, ctx.Const(0xffffU))))};
    const Id high{read_half(
        ctx.OpConvertUToF(ctx.F32[1], ctx.OpShiftRightLogical(ctx.U32[1], value, ctx.Const(16U))))};
    return ctx.OpBitwiseOr(ctx.U32[1], low, ctx.OpShiftLeftLogical(ctx.U32[1], high, ctx.Const(16U)));
}

/// A quad-confined shuffle through QuadRead: `src_of` maps the quad position to the source lane;
/// in_range follows the guest's clamp exactly as the subgroup path does.
template <typename SourceLane, typename InRange>
Id QuadShuffle(EmitContext& ctx, IR::Inst* inst, Id value, SourceLane&& src_of,
               InRange&& in_range_of) {
    const Id position{QuadPosition(ctx)};
    const Id src_thread_id{src_of(position)};
    const Id in_range{in_range_of(position, src_thread_id)};
    SetInBoundsFlag(inst, in_range);
    const Id target{ctx.OpBitwiseAnd(ctx.U32[1], src_thread_id, ctx.Const(3U))};
    return ctx.OpSelect(ctx.U32[1], in_range, QuadRead(ctx, value, target, position), value);
}

Id AddPartitionBase(EmitContext& ctx, Id thread_id) {
    const Id partition_idx{ctx.OpShiftRightLogical(ctx.U32[1], GetThreadId(ctx), ctx.Const(5u))};
    const Id partition_base{ctx.OpShiftLeftLogical(ctx.U32[1], partition_idx, ctx.Const(5u))};
    return ctx.OpIAdd(ctx.U32[1], thread_id, partition_base);
}
} // Anonymous namespace

Id EmitLaneId(EmitContext& ctx) {
    const Id id{GetThreadId(ctx)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return id;
    }
    return ctx.OpBitwiseAnd(ctx.U32[1], id, ctx.Const(31U));
}

Id EmitVoteAll(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return pred;
    }
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpGroupNonUniformAll(ctx.U1, SubgroupScope(ctx), pred);
    }
    const Id mask_ballot{
        ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), ctx.true_value)};
    const Id active_mask{WarpExtract(ctx, mask_ballot)};
    const Id ballot{
        WarpExtract(ctx, ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred))};
    const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], ballot, active_mask)};
    return ctx.OpIEqual(ctx.U1, lhs, active_mask);
}

Id EmitVoteAny(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return pred;
    }
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpGroupNonUniformAny(ctx.U1, SubgroupScope(ctx), pred);
    }
    const Id mask_ballot{
        ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), ctx.true_value)};
    const Id active_mask{WarpExtract(ctx, mask_ballot)};
    const Id ballot{
        WarpExtract(ctx, ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred))};
    const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], ballot, active_mask)};
    return ctx.OpINotEqual(ctx.U1, lhs, ctx.u32_zero_value);
}

Id EmitVoteEqual(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.true_value;
    }
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpGroupNonUniformAllEqual(ctx.U1, SubgroupScope(ctx), pred);
    }
    const Id mask_ballot{
        ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), ctx.true_value)};
    const Id active_mask{WarpExtract(ctx, mask_ballot)};
    const Id ballot{
        WarpExtract(ctx, ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred))};
    const Id lhs{ctx.OpBitwiseXor(ctx.U32[1], ballot, active_mask)};
    return ctx.OpLogicalOr(ctx.U1, ctx.OpIEqual(ctx.U1, lhs, ctx.u32_zero_value),
                           ctx.OpIEqual(ctx.U1, lhs, active_mask));
}

Id EmitSubgroupBallot(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.OpSelect(ctx.U32[1], pred, ctx.Const(1u), ctx.u32_zero_value);
    }
    const Id ballot{ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpCompositeExtract(ctx.U32[1], ballot, 0U);
    }
    return WarpExtract(ctx, ballot);
}

Id EmitSubgroupEqMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.Const(1u);
    }
    return LoadMask(ctx, ctx.subgroup_mask_eq);
}

Id EmitSubgroupLtMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    return LoadMask(ctx, ctx.subgroup_mask_lt);
}

Id EmitSubgroupLeMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.Const(1u);
    }
    return LoadMask(ctx, ctx.subgroup_mask_le);
}

Id EmitSubgroupGtMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    return LoadMask(ctx, ctx.subgroup_mask_gt);
}

Id EmitSubgroupGeMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.Const(1u);
    }
    return LoadMask(ctx, ctx.subgroup_mask_ge);
}

Id EmitShuffleIndex(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                    Id segmentation_mask) {
    if (ShuffleStaysInQuad(ctx, inst)) {
        const Id not_seg{ctx.OpNot(ctx.U32[1], segmentation_mask)};
        return QuadShuffle(
            ctx, inst, value,
            [&](Id position) {
                const Id min_id{ComputeMinThreadId(ctx, position, segmentation_mask)};
                return ctx.OpBitwiseOr(ctx.U32[1], ctx.OpBitwiseAnd(ctx.U32[1], index, not_seg),
                                       min_id);
            },
            [&](Id position, Id src) {
                const Id min_id{ComputeMinThreadId(ctx, position, segmentation_mask)};
                return ctx.OpSLessThanEqual(ctx.U1, src,
                                            ComputeMaxThreadId(ctx, min_id, clamp, not_seg));
            });
    }
    const Id not_seg_mask{ctx.OpNot(ctx.U32[1], segmentation_mask)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id min_thread_id{ComputeMinThreadId(ctx, thread_id, segmentation_mask)};
    const Id max_thread_id{ComputeMaxThreadId(ctx, min_thread_id, clamp, not_seg_mask)};

    const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], index, not_seg_mask)};
    Id src_thread_id{ctx.OpBitwiseOr(ctx.U32[1], lhs, min_thread_id)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    if (ctx.profile.warp_size_potentially_larger_than_guest) {
        src_thread_id = AddPartitionBase(ctx, src_thread_id);
    }

    SetInBoundsFlag(inst, in_range);
    return SelectValue(ctx, in_range, value, src_thread_id);
}

Id EmitShuffleUp(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                 Id segmentation_mask) {
    if (ShuffleStaysInQuad(ctx, inst)) {
        return QuadShuffle(
            ctx, inst, value, [&](Id position) { return ctx.OpISub(ctx.U32[1], position, index); },
            [&](Id position, Id src) {
                return ctx.OpSGreaterThanEqual(
                    ctx.U1, src, GetMaxThreadId(ctx, position, clamp, segmentation_mask));
            });
    }
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    Id src_thread_id{ctx.OpISub(ctx.U32[1], thread_id, index)};
    const Id in_range{ctx.OpSGreaterThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    if (ctx.profile.warp_size_potentially_larger_than_guest) {
        src_thread_id = AddPartitionBase(ctx, src_thread_id);
    }

    SetInBoundsFlag(inst, in_range);
    return SelectValue(ctx, in_range, value, src_thread_id);
}

Id EmitShuffleDown(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                   Id segmentation_mask) {
    if (ShuffleStaysInQuad(ctx, inst)) {
        return QuadShuffle(
            ctx, inst, value, [&](Id position) { return ctx.OpIAdd(ctx.U32[1], position, index); },
            [&](Id position, Id src) {
                return ctx.OpSLessThanEqual(
                    ctx.U1, src, GetMaxThreadId(ctx, position, clamp, segmentation_mask));
            });
    }
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    Id src_thread_id{ctx.OpIAdd(ctx.U32[1], thread_id, index)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    if (ctx.profile.warp_size_potentially_larger_than_guest) {
        src_thread_id = AddPartitionBase(ctx, src_thread_id);
    }

    SetInBoundsFlag(inst, in_range);
    return SelectValue(ctx, in_range, value, src_thread_id);
}

Id EmitShuffleButterfly(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                        Id segmentation_mask) {
    if (ShuffleStaysInQuad(ctx, inst)) {
        return QuadShuffle(
            ctx, inst, value,
            [&](Id position) { return ctx.OpBitwiseXor(ctx.U32[1], position, index); },
            [&](Id position, Id src) {
                return ctx.OpSLessThanEqual(
                    ctx.U1, src, GetMaxThreadId(ctx, position, clamp, segmentation_mask));
            });
    }
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    Id src_thread_id{ctx.OpBitwiseXor(ctx.U32[1], thread_id, index)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    if (ctx.profile.warp_size_potentially_larger_than_guest) {
        src_thread_id = AddPartitionBase(ctx, src_thread_id);
    }

    SetInBoundsFlag(inst, in_range);
    return SelectValue(ctx, in_range, value, src_thread_id);
}

Id EmitQuadBroadcast(EmitContext& ctx, Id value, Id lane) {
    if (LowerFragmentQuad(ctx)) {
        const Id target{ctx.OpBitwiseAnd(ctx.U32[1], lane, ctx.Const(3U))};
        return QuadRead(ctx, value, target, QuadPosition(ctx));
    }
    if (ctx.profile.support_quad_shuffles) {
        return ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], SubgroupScope(ctx), value, lane);
    }
    const Id base{ctx.OpBitwiseAnd(ctx.U32[1], GetThreadId(ctx), ctx.Const(~3u))};
    const Id local_lane{ctx.OpBitwiseAnd(ctx.U32[1], lane, ctx.Const(3u))};
    const Id src_thread_id{ctx.OpBitwiseOr(ctx.U32[1], base, local_lane)};
    return ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, src_thread_id);
}

Id EmitQuadSwap(EmitContext& ctx, Id value, Id direction) {
    const Id xor_mask{ctx.OpIAdd(ctx.U32[1], direction, ctx.Const(1u))};
    if (LowerFragmentQuad(ctx)) {
        const Id position{QuadPosition(ctx)};
        const Id target{ctx.OpBitwiseXor(ctx.U32[1], position, xor_mask)};
        return QuadRead(ctx, value, target, position);
    }
    return ctx.OpGroupNonUniformShuffleXor(ctx.U32[1], SubgroupScope(ctx), value, xor_mask);
}

Id EmitFSwizzleAdd(EmitContext& ctx, Id op_a, Id op_b, Id swizzle) {
    const Id three{ctx.Const(3U)};
    // In fragment shaders the pixel's real quad position, as the lowered quad shuffles use.
    Id mask{LowerFragmentQuad(ctx) ? QuadPosition(ctx) : GetThreadId(ctx)};
    mask = ctx.OpBitwiseAnd(ctx.U32[1], mask, three);
    mask = ctx.OpShiftLeftLogical(ctx.U32[1], mask, ctx.Const(1U));
    mask = ctx.OpShiftRightLogical(ctx.U32[1], swizzle, mask);
    mask = ctx.OpBitwiseAnd(ctx.U32[1], mask, three);

    const Id modifier_a{ctx.OpVectorExtractDynamic(ctx.F32[1], ctx.fswzadd_lut_a, mask)};
    const Id modifier_b{ctx.OpVectorExtractDynamic(ctx.F32[1], ctx.fswzadd_lut_b, mask)};

    const Id result_a{ctx.OpFMul(ctx.F32[1], op_a, modifier_a)};
    const Id result_b{ctx.OpFMul(ctx.F32[1], op_b, modifier_b)};
    return ctx.OpFAdd(ctx.F32[1], result_a, result_b);
}

Id EmitDPdxFine(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdxFine(ctx.F32[1], op_a);
}

Id EmitDPdyFine(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdyFine(ctx.F32[1], op_a);
}

Id EmitDPdxCoarse(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdxCoarse(ctx.F32[1], op_a);
}

Id EmitDPdyCoarse(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdyCoarse(ctx.F32[1], op_a);
}

} // namespace Shader::Backend::SPIRV
