#include <cstdlib>
#include <map>
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <memory>
#include "core/arm/dyncom/arm_dyncom.h"
#include "core/arm/dyncom/arm_dyncom_interpreter.h"
#include "core/arm/dyncom/arm_dyncom_trans.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "recomp_host.h"

namespace Core {

ARM_DynCom::ARM_DynCom(Core::System& system_, Memory::MemorySystem& memory,
                       PrivilegeMode initial_mode, u32 id,
                       std::shared_ptr<Core::Timing::Timer> timer)
    : ARM_Interface(id, timer), system(system_) {
    state = std::make_unique<ARMul_State>(system, memory, initial_mode);
}

ARM_DynCom::~ARM_DynCom() {}

void ARM_DynCom::Run() {
    if (break_flag) [[unlikely]] {
        return;
    }
    ExecuteInstructions(std::max<s64>(timer->GetDowncount(), 0));
}

void ARM_DynCom::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }
    ExecuteInstructions(1);
}

// (porte) how often the interpreter's block cache is thrown away, and by whom (Recomp::Tick prints it)
extern "C" unsigned long long g_dyncomClears[3];   // 0 Clear, 1 InvalidateCacheRange, 2 SetPageTable
extern "C" unsigned long long g_dyncomBlocks;      // blocks translated
unsigned long long g_dyncomClears[3] = {};
unsigned long long g_dyncomBlocks = 0;

void ARM_DynCom::ClearInstructionCache() {
    g_dyncomClears[0]++;
    state->instruction_cache.clear();
    trans_cache_buf_top = 0;
}

void ARM_DynCom::InvalidateCacheRange(u32, std::size_t) {
    g_dyncomClears[1]++;
    ClearInstructionCache();
}

void ARM_DynCom::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    // (porte, 20/11) the kernel calls this on EVERY thread switch, also when the page table is the same one (only
    // the game's process runs here): the whole block cache was thrown away ~30000 times a second and ~67000 blocks
    // re-translated a second (and each failed decode of data written twice to the log). Only a really different
    // table (another process) clears it now
    static std::map<const ARM_DynCom*, const Memory::PageTable*> last;
    const Memory::PageTable*& prev = last[this];
    static const bool old = getenv("PORTE_DYNCOM_LIMPA") != nullptr;   // (test) the old way: clear on every switch
    if (prev == page_table.get() && !old) return;
    prev = page_table.get();
    g_dyncomClears[2]++;
    ClearInstructionCache();
}

std::shared_ptr<Memory::PageTable> ARM_DynCom::GetPageTable() const {
    return nullptr;
}

void ARM_DynCom::SetPC(u32 pc) {
    state->Reg[15] = pc;
}

u32 ARM_DynCom::GetPC() const {
    return state->Reg[15];
}

u32 ARM_DynCom::GetReg(int index) const {
    return state->Reg[index];
}

void ARM_DynCom::SetReg(int index, u32 value) {
    state->Reg[index] = value;
}

u32 ARM_DynCom::GetVFPReg(int index) const {
    return state->ExtReg[index];
}

void ARM_DynCom::SetVFPReg(int index, u32 value) {
    state->ExtReg[index] = value;
}

u32 ARM_DynCom::GetVFPSystemReg(VFPSystemRegister reg) const {
    return state->VFP[reg];
}

void ARM_DynCom::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    state->VFP[reg] = value;
}

u32 ARM_DynCom::GetCPSR() const {
    return state->Cpsr;
}

void ARM_DynCom::SetCPSR(u32 cpsr) {
    state->Cpsr = cpsr;
}

u32 ARM_DynCom::GetCP15Register(CP15Register reg) const {
    return state->CP15[reg];
}

void ARM_DynCom::SetCP15Register(CP15Register reg, u32 value) {
    state->CP15[reg] = value;
}

void ARM_DynCom::ExecuteInstructions(u64 num_instructions) {
    state->NumInstrsToExecute = num_instructions;
    const u32 ticks_executed = InterpreterMainLoop(state.get());
    Recomp::CountJit(ticks_executed);   // (what was not native, for the counter)
    if (timer) {
        timer->AddTicks(ticks_executed);
    }
    state->ServeBreak();
}

void ARM_DynCom::SaveContext(ThreadContext& ctx) {
    ctx.cpu_registers = state->Reg;
    ctx.cpsr = state->Cpsr;
    ctx.fpu_registers = state->ExtReg;
    ctx.fpscr = state->VFP[VFP_FPSCR];
    ctx.fpexc = state->VFP[VFP_FPEXC];
}

void ARM_DynCom::LoadContext(const ThreadContext& ctx) {
    state->Reg = ctx.cpu_registers;
    state->Cpsr = ctx.cpsr;
    state->ExtReg = ctx.fpu_registers;
    state->VFP[VFP_FPSCR] = ctx.fpscr;
    state->VFP[VFP_FPEXC] = ctx.fpexc;
}

void ARM_DynCom::PrepareReschedule() {
    Recomp::NoteReschedule();
    state->NumInstrsToExecute = 0;
}

} // namespace Core
