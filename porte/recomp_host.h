// Ultra Moon recompiled: the game's own ARM code translated to C (momiji_nativo.dll) runs
// natively inside the Azahar core. The first instruction of every translated function is
// replaced by an "svc" with its number; the JIT stops there and RunNative() runs the C version.
// Whatever the C code cannot do (a function not translated, a system call, the end of the time
// slice) hands the registers back with PC on that instruction and the JIT goes on: the original
// code is still in memory, only the first instructions are patched.
//   svc 0xFE0000 + i           main program function i
//   svc 0x100000 + k*0x10000 + i   function i of module (.cro) k, while it is loaded
#pragma once
#include <cstdint>
#include <string>

namespace Core {
class System;
class ARM_Interface;
}

namespace Recomp {
constexpr uint32_t kSvcBase = 0xFE0000;
constexpr uint32_t kSvcModBase = 0x100000;
constexpr uint32_t kSvcFirst = kSvcModBase;   // every svc number from here up is ours

// after the game is loaded: load the DLL next to the exe and patch the code
bool Start(Core::System& system, const std::wstring& exeDir);
// main thread, every pass: re-patch after a state load, write the counters once a second
void Tick(Core::System& system);
// from the JIT (CallSVC): run one translated function. Returns false if the number is not ours
// (then the normal system call runs). 'ticks' = instructions run, for the emulated clock.
bool RunNative(Core::ARM_Interface& cpu, uint32_t swi, uint64_t& ticks);
// the JIT ran 'ticks' (to tell how much of the game runs natively)
void CountJit(uint64_t ticks);
// the kernel asked for a thread switch (ARM_Dynarmic::PrepareReschedule)
void NoteReschedule();
// PORTE_RECOMP_PERFIL=1: where the JIT was when its time slice ended (a sample of where it spends time)
void SampleJit(uint32_t pc);
// from ldr:ro: a module was loaded at 'base' (its first 'fixedSize' bytes stay mapped) / is about to go
void OnCroLoaded(const std::string& name, uint32_t base, uint32_t fixedSize, uint32_t fileSize);
void OnCroUnload(uint32_t base);
// from ldr:ro: the static.crs is at 'address' (head of the two lists of loaded modules)
void OnCrsLoaded(uint32_t address);
// is the module 'name' (e.g. "Battle") loaded now? (from the registered modules; false when the recompiler is off)
bool ModuleLoaded(const char* name);
std::string StatusText();
bool Active();
} // namespace Recomp
