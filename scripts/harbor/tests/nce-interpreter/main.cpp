// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cstdio>
#include "core/arm/nce/interpreter_visitor.h"
int main() {
 Core::Memory::Memory memory;
 HarborTestContext context{};
 fpsimd_context simd{};
 context.pc=0x100;
 context.regs[0]=0x1122334455667788ULL;
 context.regs[1]=4092;
 const u32 str=0xf9000020; // str x0,[x1], crossing a page boundary
 memory.WriteBlock(context.pc,&str,4);memory.writes=0;
 auto next=Core::MatchAndExecuteOneInstruction(memory,&context,&simd);
 assert(next && *next==0x104 && memory.writes==1);
 u64 stored{};memory.ReadBlock(4092,&stored,8);assert(stored==context.regs[0]);
 const u32 stp=0xad000440; // stp q0,q1,[x2]
 context.regs[2]=4080;simd.vregs[0]={0x1234,0x5678};simd.vregs[1]={0x9abc,0xdef0};
 memory.WriteBlock(context.pc,&stp,4);memory.writes=0;
 next=Core::MatchAndExecuteOneInstruction(memory,&context,&simd);
 assert(next && memory.writes==2);
 u128 vectors[2]{};memory.ReadBlock(4080,vectors,32);
 assert(vectors[0]==simd.vregs[0] && vectors[1]==simd.vregs[1]);
 const u32 ldr=0xf9400023; // ldr x3,[x1]
 memory.WriteBlock(context.pc,&ldr,4);
 u64 expected{};memory.ReadBlock(context.regs[1],&expected,8);
 next=Core::MatchAndExecuteOneInstruction(memory,&context,&simd);
 assert(next && context.regs[3]==expected);
 const u32 nop=0xd503201f;
 memory.WriteBlock(context.pc,&nop,4);memory.writes=0;
 next=Core::MatchAndExecuteOneInstruction(memory,&context,&simd);
 assert(!next && memory.writes==0);
 puts("NCE interpreter: scalar/page-crossing, SIMD pair, load, unsupported instruction checks passed");
}
