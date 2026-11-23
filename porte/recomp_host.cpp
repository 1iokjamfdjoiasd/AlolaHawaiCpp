// Ultra Moon recompiled: loading momiji_nativo.dll, patching the game code, running the C code.
#include "recomp_host.h"
#include "jogos.h"
#include "core/loader/loader.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "unicorn/unicorn.h"

namespace Recomp {

// must match runtime.h of the generated code
struct Cpu
{
    uint32_t r[16];
    uint32_t n, z, c, v, q, ge;
    uint32_t fault, miss;
    uint32_t tls, excl, excl_addr, fpscr;
    uint32_t s[32];
    uint32_t icount, ilimit;
};
using Fn = void (*)(Cpu*);
using RdFn = uint32_t (*)(uint32_t);
using WrFn = void (*)(uint32_t, uint32_t);

static HMODULE gDll = nullptr;
static Core::System* gSystem = nullptr;
static Memory::MemorySystem* gMem = nullptr;
static bool gActive = false;
static void (*gCallNative)(Fn, Cpu*) = nullptr;
// main program
static int gCount = 0;
static const uint32_t* gAddr = nullptr;
static Fn const* gFn = nullptr;
static std::vector<int> gPatched;
static std::vector<uint32_t> gOrigMain;   // first instruction of each main function, as in the game
// modules
static int gModCount = 0;
static const char* const* gModName = nullptr;
static const int* gModNfn = nullptr;
static const uint32_t* const* gModOff = nullptr;
static Fn const* const* gModFn = nullptr;
static uint32_t* gModBase = nullptr;
static void (*gModLoad)(int, uint32_t, uint32_t) = nullptr;
static void (*gModUnload)(int) = nullptr;
static const uint32_t* const* gModOrig = nullptr;   // original first word of each module function (may be absent)
static uint32_t gCrs = 0;                            // static.crs (lists of loaded modules)
static const void* gLastProcess = nullptr;           // the kernel's process object: a new one after a state load
struct Loaded { int k; uint32_t base, size; std::vector<uint32_t> original; };
static std::vector<Loaded> gLoaded;

// counters
static std::atomic<uint64_t> gCalls{0}, gNativeInsns{0}, gJitTicks{0};
static std::atomic<uint64_t> gBack[8];
static uint64_t gLastCalls = 0, gLastInsns = 0, gLastJit = 0, gLastBack[8] = {};
static std::string gStatus;
// PORTE_RECOMP_MISS=1: which addresses the C code hands back most (to see what is missing)
static bool gMissLog = false;
static std::map<uint32_t, uint64_t> gMiss;
// every place handed back for lack of a translation (not svc), as "address" or "Module+offset":
// written to recomp_faltam.txt for gerar.py --extras (the next build translates them)
static std::set<std::string> gFaltam;
static std::wstring gFaltamPath;
static std::wstring gPerfilPath;
static DWORD gLastMissPrint = 0;
static DWORD gLastPrint = 0;

// ---------------------------------------------------------------------------
// live check (PORTE_CONFERIR=1, testing): every native call also runs in Unicorn (ARM1136 + the
// 3DS's VFP) from the same registers, for the same number of instructions, and the two are
// compared. The native code runs in a sandbox (its writes go to a diary, its reads look at the
// diary first); Unicorn reads and writes the game's memory through the Azahar memory functions,
// and the code it runs comes from clean copies (the original words where our svc are). The game
// always goes on with Unicorn's result, so a wrong translation cannot break it during the test:
// the function is switched off (recomp_desligar.txt) and reported (conferencia.txt).
// Also: if the native code reads one of our svc words as data, the function there is switched off.
// ---------------------------------------------------------------------------
namespace Conf {
static bool on = false;
static int every = 1;            // 1 = every call; N = the first N calls of each function, then 1 in 1000
static bool followNative = false; // PORTE_CONFERIR_SEGUE=nativo: on a difference the game goes on with the native result
static bool followNativeOnce = false;   // (the bisection already put the native result in place)
static bool sandbox = false;
// byte logs kept per 4 KB page: the bytes + a mask of which ones were written
struct Pg { uint8_t d[4096]; uint64_t m[64]; uint8_t w[4096]; };   // w: value written (Unicorn's log)
struct PageLog
{
    std::unordered_map<uint32_t, Pg*> pages;
    std::vector<Pg*> pool;
    uint32_t lastP = ~0u;
    Pg* last = nullptr;
    Pg* Find(uint32_t p)
    {
        if (p == lastP) return last;
        auto it = pages.find(p);
        if (it == pages.end()) return nullptr;
        lastP = p;
        last = it->second;
        return last;
    }
    Pg* Get(uint32_t p)
    {
        if (Pg* g = Find(p)) return g;
        Pg* g;
        if (pool.empty()) g = new Pg;
        else { g = pool.back(); pool.pop_back(); }
        memset(g->m, 0, sizeof(g->m));
        pages[p] = g;
        lastP = p;
        last = g;
        return g;
    }
    void Clear()
    {
        for (auto& [p, g] : pages) pool.push_back(g);
        pages.clear();
        lastP = ~0u;
        last = nullptr;
    }
    static bool Has(const Pg* g, uint32_t o) { return g->m[o >> 6] >> (o & 63) & 1; }
    static void Set(Pg* g, uint32_t o) { g->m[o >> 6] |= 1ull << (o & 63); }
    // every run of written bytes: f(address, bytes, length)
    template <class F> void Runs(F f) const
    {
        for (auto& [p, g] : pages)
        {
            uint32_t o = 0;
            while (o < 4096)
            {
                if (!(o & 63) && !g->m[o >> 6]) { o += 64; continue; }
                if (!Has(g, o)) { o++; continue; }
                uint32_t e = o;
                while (e < 4096 && Has(g, e)) e++;
                f((p << 12) + o, &g->d[o], e - o);
                o = e;
            }
        }
    }
};
static PageLog diary;   // native writes (the values written)
static PageLog undo;    // Unicorn's writes: the mask = which bytes, the data = what was there before
static std::vector<uint8_t> svcBits(0x800000);        // 1 bit per word below 0x10000000: one of our svc
static uint32_t dataHit = 0;
static FILE* report = nullptr;
static std::wstring desligarPath;     // the list read at start (next to the exe)
static std::wstring desligarOutPath;  // where new ones are written (PORTE_CONFERIR_PASTA: one per parallel test)
static std::atomic<uint64_t> nChecked{0}, nDiff{0}, nSkip{0}, nOff{0}, nData{0}, nUnstable{0};
static uint64_t lastChecked = 0;
static std::unordered_map<uint32_t, uint32_t> calls;   // svc number -> times run
static int reported = 0;

static inline void SetSvcBit(uint32_t a, bool v)
{
    if (a >= 0x10000000u) return;
    const uint32_t w = a >> 2;
    if (v) svcBits[w >> 3] |= (uint8_t)(1u << (w & 7));
    else svcBits[w >> 3] &= (uint8_t)~(1u << (w & 7));
}
static inline bool SvcBit(uint32_t a) { return a < 0x10000000u && (svcBits[a >> 5] >> ((a >> 2) & 7) & 1); }
static inline void Watch(uint32_t a, unsigned size)
{
    for (uint32_t w = a & ~3u; w < a + size; w += 4)
        if (SvcBit(w) && !dataHit) dataHit = w;
}
static uint32_t SbRead2(uint32_t a, unsigned size);
static bool logReads;
// every read of the native run (for the report: compared with Unicorn's)
static std::vector<std::pair<uint32_t, uint32_t>> natReads, ucReads;
static uint32_t SbRead(uint32_t a, unsigned size)
{
    const uint32_t r = SbRead2(a, size);
    if (logReads && natReads.size() < 200000) natReads.push_back({ a, r });
    return r;
}
static uint32_t SbRead2(uint32_t a, unsigned size)
{
    const uint32_t p = a >> 12, p2 = (a + size - 1) >> 12;
    Pg* g = diary.Find(p);
    Pg* g2 = p2 != p ? diary.Find(p2) : g;
    if (!g && !g2) return size == 1 ? gMem->Read8(a) : size == 2 ? gMem->Read16(a) : gMem->Read32(a);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
    {
        const uint32_t b = a + i;
        Pg* q = (b >> 12) == p ? g : g2;
        const uint32_t o = b & 0xFFFu;
        v |= (uint32_t)(q && PageLog::Has(q, o) ? q->d[o] : gMem->Read8(b)) << (8 * i);
    }
    return v;
}
static void SbWrite(uint32_t a, uint32_t v, unsigned size)
{
    for (unsigned i = 0; i < size; i++)
    {
        const uint32_t b = a + i;
        Pg* g = diary.Get(b >> 12);
        g->d[b & 0xFFFu] = (uint8_t)(v >> (8 * i));
        PageLog::Set(g, b & 0xFFFu);
    }
}
} // namespace Conf

static uint32_t HRd8(uint32_t a) { if (Conf::on) { Conf::Watch(a, 1); if (Conf::sandbox) return Conf::SbRead(a, 1); } return gMem->Read8(a); }
static uint32_t HRd16(uint32_t a) { if (Conf::on) { Conf::Watch(a, 2); if (Conf::sandbox) return Conf::SbRead(a, 2); } return gMem->Read16(a); }
static uint32_t HRd32(uint32_t a) { if (Conf::on) { Conf::Watch(a, 4); if (Conf::sandbox) return Conf::SbRead(a, 4); } return gMem->Read32(a); }
static void HWr8(uint32_t a, uint32_t v) { if (Conf::sandbox) Conf::SbWrite(a, v, 1); else gMem->Write8(a, (u8)v); }
static void HWr16(uint32_t a, uint32_t v) { if (Conf::sandbox) Conf::SbWrite(a, v, 2); else gMem->Write16(a, (u16)v); }
static void HWr32(uint32_t a, uint32_t v) { if (Conf::sandbox) Conf::SbWrite(a, v, 4); else gMem->Write32(a, v); }

static uint32_t SvcWord(uint32_t imm) { return 0xEF000000u | imm; }
static uint32_t MainSvc(int i) { return kSvcBase + (uint32_t)i; }
static uint32_t ModSvc(int k, int i) { return kSvcModBase + (uint32_t)k * 0x10000u + (uint32_t)i; }

static std::vector<char> gOff;   // main functions switched off at run time (they cannot run natively)

// ---- fase B, peça 1: system calls run straight from the native code (PORTE_SVC_DIRETO=0 turns it off)
static Core::ARM_Interface* gCurCpu = nullptr;
static std::unique_ptr<Kernel::SVCContext> gSvcCtx;
static bool gResched = false, gSvcDireto = true;
static uint32_t gFlushed = 0;   // native instructions of this call already given to the emulated clock
static u8*** gPagesVar = nullptr;   // the DLL's recomp_pages (fase C: its fast memory path reads the page table)
static std::atomic<uint64_t> gSvcDone{0}, gSvcBack{0};
static int HSvc(struct Cpu* s, uint32_t imm, uint32_t next);
// (60 FPS study) the waits seen in the direct svc: key "svc|object|caller" -> count
static bool gEsperas = false;
// 60 FPS (estudo): o objeto gráfico do jogo fica em [0x006A3984]; +8 = pedido de modo (2 = nenhum),
// +9 = modo em uso (0 = 1 VBlank por quadro = 60 FPS, 1 = 2 VBlanks = 30 FPS). Cada cena pede o seu
// modo por SetFrameMode (0x00401354). PORTE_60FPS=1 pede 0 sempre que a cena estiver em 1.
static int g60 = 0;
static uint32_t g60Forced = 0;
static bool g60SceneWants30 = true, g60Seen = false;
static float* g60PassoAnim = nullptr;   // recomp_passo_anim in the DLL
static DWORD g60Desde = 0, g60T0 = 0; // PORTE_60FPS_DESDE=ms: só força depois desse tempo (teste)
static std::map<std::string, uint64_t> gEsperasN;
static std::wstring gEsperasPath;
static DWORD gEsperasT0 = 0, gEsperasLast = 0;
static void PatchMain(Core::System& system)
{
    for (int i : gPatched)
        if (gOff.empty() || !gOff[i]) gMem->Write32(gAddr[i], SvcWord(MainSvc(i)));
    for (u32 c = 0; c < system.GetNumCores(); c++) system.GetCore(c).ClearInstructionCache();
}

// where our svc are (for "read as data" and switching off)
static std::unordered_map<uint32_t, uint32_t> gSvcAt;   // address -> svc number
// functions switched off in recomp_desligar.txt: main program addresses / "Module+offset"
static std::set<uint32_t> gOffMain;
static std::set<std::string> gOffMod;

namespace Conf {
using UcOpen = decltype(&uc_open);
using UcClose = decltype(&uc_close);
using UcCtl = decltype(&uc_ctl);
using UcMapPtr = decltype(&uc_mem_map_ptr);
using UcMmio = decltype(&uc_mmio_map);
using UcRegW = decltype(&uc_reg_write);
using UcRegR = decltype(&uc_reg_read);
using UcStart = decltype(&uc_emu_start);
using UcStop = decltype(&uc_emu_stop);
using UcHook = decltype(&uc_hook_add);
using UcErrStr = decltype(&uc_strerror);
static UcOpen u_open; static UcClose u_close; static UcCtl u_ctl; static UcMapPtr u_map_ptr; static UcMmio u_mmio;
static UcRegW u_regw; static UcRegR u_regr; static UcStart u_start; static UcStop u_stop; static UcHook u_hook;
static UcErrStr u_strerror;
static uc_engine* uc = nullptr;
static bool dirty = true;
static DWORD born = 0;   // pages are mapped by pointer: start again every few seconds (memory gets remapped)
struct Shadow
{
    uint32_t base = 0;                  // module base (0 = main program)
    uint32_t lo = 0, hi = 0;            // pages
    std::vector<uint8_t> buf;
    std::vector<uint32_t> mixed;        // pages shared with data: copied again before each check
    std::vector<std::pair<uint32_t, uint32_t>> fixes;   // (address, original word) of our svc in the mixed pages
    std::vector<std::pair<uint32_t, uint32_t>> orig;    // every (address, original word) of our svc in it
};
static bool needRefresh = false;
static std::vector<Shadow> shadows;
static bool ucFail = false;
static const char* ucWhy = "";
static bool elsewhere = false;   // stopped before an svc / code it cannot run, somewhere else: a valid place to go on
static uint32_t elsewherePc = 0;
static uint32_t hookTarget = 0;
static uint64_t hookMin = 0, hookN = 0;
static uint64_t lastExit = 0;
static uint32_t ring[32];   // the last instructions Unicorn ran (for the report)
static void OnCode(uc_engine* u, uint64_t addr, uint32_t, void*)
{
    ring[hookN & 31] = (uint32_t)addr;
    (void)u;   // (only counts and keeps the last ones: the exit stops it)
    hookN++;
}

static bool LoadUnicorn(const std::wstring& exeDir)
{
    HMODULE h = LoadLibraryW((exeDir + L"unicorn.dll").c_str());
    if (!h) return false;
    u_open = (UcOpen)GetProcAddress(h, "uc_open");
    u_close = (UcClose)GetProcAddress(h, "uc_close");
    u_ctl = (UcCtl)GetProcAddress(h, "uc_ctl");
    u_map_ptr = (UcMapPtr)GetProcAddress(h, "uc_mem_map_ptr");
    u_mmio = (UcMmio)GetProcAddress(h, "uc_mmio_map");
    u_regw = (UcRegW)GetProcAddress(h, "uc_reg_write");
    u_regr = (UcRegR)GetProcAddress(h, "uc_reg_read");
    u_start = (UcStart)GetProcAddress(h, "uc_emu_start");
    u_stop = (UcStop)GetProcAddress(h, "uc_emu_stop");
    u_hook = (UcHook)GetProcAddress(h, "uc_hook_add");
    u_strerror = (UcErrStr)GetProcAddress(h, "uc_strerror");
    return u_open && u_close && u_ctl && u_map_ptr && u_mmio && u_regw && u_regr && u_start && u_stop && u_hook && u_strerror;
}

static uint64_t MmioRd(uc_engine*, uint64_t off, unsigned size, void* user)
{
    const uint32_t a = (uint32_t)(uintptr_t)user + (uint32_t)off;
    uint64_t v = 0;
    switch (size)
    {
    case 1: v = gMem->Read8(a); break;
    case 2: v = gMem->Read16(a); break;
    case 4: v = gMem->Read32(a); break;
    default:
        for (unsigned i = 0; i < size; i++) v |= (uint64_t)gMem->Read8(a + i) << (8 * i);
    }
    return v;
}
static void MmioWr(uc_engine*, uint64_t off, unsigned size, uint64_t v, void* user)
{
    const uint32_t a = (uint32_t)(uintptr_t)user + (uint32_t)off;
    switch (size)
    {
    case 1: gMem->Write8(a, (u8)v); break;
    case 2: gMem->Write16(a, (u16)v); break;
    case 4: gMem->Write32(a, (u32)v); break;
    default:
        for (unsigned i = 0; i < size; i++) gMem->Write8(a + i, (u8)(v >> (8 * i)));
    }
}
static std::set<uint32_t> mapped;   // pages already given to Unicorn (by pointer)
// the ranges given by pointer and where they pointed: the game remaps memory (the linear heap),
// and a range kept with the old pointer made Unicorn read stale memory (false differences)
struct MapRange { uint32_t lo, hi; u8* host; };
static std::vector<MapRange> ranges;
static bool RangesStillValid()
{
    for (auto& r : ranges)
        for (uint32_t a = r.lo; a < r.hi; a += 0x1000)
            if (gMem->GetPointer(a) != r.host + (a - r.lo)) return false;
    return true;
}
static bool OnUnmapped(uc_engine* u, uc_mem_type type, uint64_t addr, int, int64_t, void*)
{
    if (type == UC_MEM_FETCH_UNMAPPED)
    {
        uint32_t cp = 0;
        u_regr(u, UC_ARM_REG_CPSR, &cp);
        elsewhere = true;
        elsewherePc = (uint32_t)addr | ((cp & 0x20) ? 1u : 0u);
        return false;
    }
    const uint32_t page = (uint32_t)addr & ~0xFFFu;
    if (u8* host = gMem->GetPointer(page))
    {
        // one region for the whole run of pages that are next to each other in the host too (every
        // new Unicorn region rebuilds its memory map: thousands of 4 KB ones made it ~10000x slower).
        // Never over a clean copy of code (those are mapped already: the run stops at them)
        auto contiguous = [](uint32_t a, u8* h) { return gMem->GetPointer(a) == h; };
        auto taken = [](uint32_t a) {
            for (auto& sh : shadows)
                if (a >= sh.lo && a < sh.hi) return true;
            return mapped.count(a >> 12) != 0;
        };
        uint32_t lo = page, hi = page + 0x1000;
        while (hi - lo < 0x4000000u && hi && !taken(hi) && contiguous(hi, host + (hi - page))) hi += 0x1000;
        while (hi - lo < 0x4000000u && lo >= 0x1000 && !taken(lo - 0x1000) && contiguous(lo - 0x1000, host - (page - lo) - 0x1000)) lo -= 0x1000;
        if (u_map_ptr(u, lo, hi - lo, UC_PROT_READ | UC_PROT_WRITE, host - (page - lo)) == UC_ERR_OK)
        {
            for (uint32_t a = lo; a < hi; a += 0x1000) mapped.insert(a >> 12);
            ranges.push_back({ lo, hi, host - (page - lo) });
            return true;
        }
        ucFail = true;
        ucWhy = "não consegui mapear a memória";
        return false;
    }
    if (u_mmio(u, page, 0x1000, MmioRd, (void*)(uintptr_t)page, MmioWr, (void*)(uintptr_t)page) != UC_ERR_OK)
    {
        ucFail = true;
        ucWhy = "não consegui mapear a memória";
        return false;
    }
    return true;
}
static bool atSvc = false;   // stopped on the native's last PC, which was an svc (the hook cannot stop before it)
static bool logReads2 = false;   // (unused) PORTE_CONFERIR_LEITURAS=1: keep every read of both sides for the report
static bool OnRead(uc_engine*, uc_mem_type, uint64_t addr, int size, int64_t, void*)
{
    uint32_t v = 0;
    for (int i = 0; i < size && i < 4; i++) v |= (uint32_t)gMem->Read8((uint32_t)addr + i) << (8 * i);
    if (ucReads.size() < 200000) ucReads.push_back({ (uint32_t)addr, v });
    return true;
}
// every write of Unicorn (called before it happens: the old value is still there)
static std::unordered_map<uint32_t, uint32_t> ucWritePc;   // (diagnosis) address -> PC of Unicorn's last write
static bool OnWrite(uc_engine* u, uc_mem_type, uint64_t addr, int size, int64_t value, void*)
{
    {
        uint32_t pc = 0;
        u_regr(u, UC_ARM_REG_PC, &pc);
        for (int i = 0; i < size; i++) ucWritePc[(uint32_t)addr + i] = pc;
    }
    const uint32_t a = (uint32_t)addr;
    for (int i = 0; i < size; i++)
    {
        const uint32_t b = a + (uint32_t)i;
        Pg* g = undo.Get(b >> 12);
        const uint32_t o = b & 0xFFFu;
        if (!PageLog::Has(g, o)) { g->d[o] = gMem->Read8(b); PageLog::Set(g, o); }
        g->w[o] = (uint8_t)((uint64_t)value >> (8 * i));
    }
    return true;
}
static bool OnFetchProt(uc_engine* u, uc_mem_type, uint64_t addr, int, int64_t, void*)
{
    uint32_t cp = 0;
    u_regr(u, UC_ARM_REG_CPSR, &cp);
    elsewhere = true;
    elsewherePc = (uint32_t)addr | ((cp & 0x20) ? 1u : 0u);
    return false;
}
static void OnIntr(uc_engine* u, uint32_t, void*)
{
    uint32_t pc = 0, cp = 0;
    u_regr(u, UC_ARM_REG_PC, &pc);
    u_regr(u, UC_ARM_REG_CPSR, &cp);
    const uint32_t at = pc - ((cp & 0x20) ? 2u : 4u);
    if (at == hookTarget) atSvc = true;   // Unicorn ran the svc only: no system here
    else { elsewhere = true; elsewherePc = at | ((cp & 0x20) ? 1u : 0u); }   // stopped on another svc
    u_stop(u);
}
static bool EnsureUc()
{
    static const DWORD renew = getenv("PORTE_CONFERIR_RENOVA") ? (DWORD)atoi(getenv("PORTE_CONFERIR_RENOVA")) : 20000;
    if (uc && !dirty && GetTickCount() - born < renew && RangesStillValid()) return true;
    if (uc) { u_close(uc); uc = nullptr; }
    mapped.clear();
    ranges.clear();
    if (u_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) { uc = nullptr; return false; }
    u_ctl(uc, UC_CTL_WRITE(UC_CTL_CPU_MODEL, 1), (int)UC_CPU_ARM_11MPCORE);   // the 3DS ARM11 (ARMv6K: ldrexb/h/d, clrex, yield)
    uint32_t v = 0;
    u_regr(uc, UC_ARM_REG_C1_C0_2, &v);
    v |= 0xFu << 20;   // VFP on (cp10 / cp11)
    u_regw(uc, UC_ARM_REG_C1_C0_2, &v);
    v = 0x40000000u;
    u_regw(uc, UC_ARM_REG_FPEXC, &v);
    for (auto& sh : shadows)
        if (u_map_ptr(uc, sh.lo, sh.hi - sh.lo, UC_PROT_READ | UC_PROT_EXEC, sh.buf.data()) != UC_ERR_OK)
            fprintf(stderr, "conferência: cópia limpa %08X-%08X não mapeada\n", (unsigned)sh.lo, (unsigned)sh.hi);
    uc_hook hk;
    u_hook(uc, &hk, UC_HOOK_MEM_UNMAPPED, (void*)OnUnmapped, nullptr, 1, 0);
    u_hook(uc, &hk, UC_HOOK_INTR, (void*)OnIntr, nullptr, 1, 0);
    if (logReads) u_hook(uc, &hk, UC_HOOK_CODE, (void*)OnCode, nullptr, 1, 0);   // (slow: diagnosis only)
    u_ctl(uc, UC_CTL_WRITE(UC_CTL_UC_USE_EXITS, 1), 1);
    lastExit = 0;
    u_hook(uc, &hk, UC_HOOK_MEM_WRITE, (void*)OnWrite, nullptr, 1, 0);
    u_hook(uc, &hk, UC_HOOK_MEM_FETCH_PROT, (void*)OnFetchProt, nullptr, 1, 0);
    if (logReads) u_hook(uc, &hk, UC_HOOK_MEM_READ, (void*)OnRead, nullptr, 1, 0);
    born = GetTickCount();
    dirty = false;
    return true;
}
// a clean copy of [codeLo, codeHi) as it is in memory now, with the given original words put back
static void AddShadow(uint32_t base, uint32_t codeLo, uint32_t codeHi, const std::vector<std::pair<uint32_t, uint32_t>>& orig)
{
    Shadow sh;
    sh.base = base;
    sh.lo = codeLo & ~0xFFFu;
    sh.hi = (codeHi + 0xFFFu) & ~0xFFFu;
    sh.buf.assign(sh.hi - sh.lo, 0);
    for (uint32_t a = sh.lo; a < sh.hi; a += 0x1000)
        if (const u8* p = gMem->GetPointer(a)) memcpy(&sh.buf[a - sh.lo], p, 0x1000);
    if (codeLo & 0xFFFu) sh.mixed.push_back(sh.lo);
    if ((codeHi & 0xFFFu) && (sh.mixed.empty() || sh.mixed[0] != sh.hi - 0x1000)) sh.mixed.push_back(sh.hi - 0x1000);
    for (auto& [a, w] : orig)
    {
        if (a < sh.lo || a + 4 > sh.hi || !w) continue;
        memcpy(&sh.buf[a - sh.lo], &w, 4);
        sh.orig.push_back({ a, w });
        for (uint32_t m : sh.mixed)
            if (a - m < 0x1000) sh.fixes.push_back({ a, w });
    }
    shadows.push_back(std::move(sh));
    dirty = true;
}
static void RemoveShadow(uint32_t base)
{
    for (size_t j = 0; j < shadows.size(); j++)
        if (shadows[j].base == base && base) { shadows.erase(shadows.begin() + j); dirty = true; return; }
}
// copy every clean copy again from memory (the loader writes into code when modules link: the
// stubs of the main program that jump to module functions, literal pools of the modules)
static void RefreshAll()
{
    for (auto& sh : shadows)
    {
        for (uint32_t a = sh.lo; a < sh.hi; a += 0x1000)
            if (const u8* p = gMem->GetPointer(a)) memcpy(&sh.buf[a - sh.lo], p, 0x1000);
        for (auto& [a, w] : sh.orig) memcpy(&sh.buf[a - sh.lo], &w, 4);
    }
    needRefresh = false;
    dirty = true;   // Unicorn keeps translated blocks of the old bytes
}
static void RefreshMixed()
{
    if (needRefresh) RefreshAll();
    for (auto& sh : shadows)
    {
        for (uint32_t m : sh.mixed)
            if (const u8* p = gMem->GetPointer(m)) memcpy(&sh.buf[m - sh.lo], p, 0x1000);
        for (auto& [a, w] : sh.fixes) memcpy(&sh.buf[a - sh.lo], &w, 4);
    }
}
static void InvalidateWritten()
{
    undo.Runs([](uint32_t a, const uint8_t*, uint32_t len) { gMem->RasterizerFlushVirtualRegion(a, len, Memory::FlushMode::Invalidate); });
}
static bool ShouldCheck(uint32_t swi)
{
    const uint32_t c = ++calls[swi];
    return every <= 1 || c <= (uint32_t)every || c % 1000 == 0;
}
} // namespace Conf

static std::string FnName(uint32_t swi, uint32_t entry)
{
    char b[96];
    if (swi >= kSvcBase) snprintf(b, sizeof(b), "%08X", (unsigned)entry);
    else
    {
        const int k = (int)((swi - kSvcModBase) >> 16), i = (int)(swi & 0xFFFF);
        snprintf(b, sizeof(b), "%s+%X (em %08X)", gModName[k], (unsigned)gModOff[k][i], (unsigned)entry);
    }
    return b;
}

// switch one function off now: the game's instruction goes back, the JIT runs it from here on;
// 'keep' = also write it to recomp_desligar.txt (so the next runs leave it off from the start)
static void DisableFn(uint32_t swi, uint32_t entry, const char* why, bool keep)
{
    uint32_t orig = 0;
    if (swi >= kSvcBase)
    {
        const int i = (int)(swi - kSvcBase);
        orig = gOrigMain[i];
        if (gOff.empty()) gOff.assign(gCount, 0);
        if (gOff[i]) return;
        gOff[i] = 1;
    }
    else
    {
        const int k = (int)((swi - kSvcModBase) >> 16), i = (int)(swi & 0xFFFF);
        bool found = false;
        for (auto& L : gLoaded)
            if (L.k == k) { orig = L.original[i]; found = orig != 0; L.original[i] = 0; }
        if (!found) return;
    }
    if (orig)
    {
        gMem->Write32(entry, orig);
        gSystem->InvalidateCacheRange(entry, 4);
    }
    Conf::SetSvcBit(entry, false);
    gSvcAt.erase(entry);
    Conf::nOff++;
    fprintf(stderr, "recomp: função %s desligada (%s)\n", FnName(swi, entry).c_str(), why);
    if (keep && !Conf::desligarOutPath.empty())
    {
        if (FILE* f = _wfopen(Conf::desligarOutPath.c_str(), L"a"))
        {
            if (swi >= kSvcBase) fprintf(f, "%08X\n", (unsigned)entry);
            else
            {
                const int k = (int)((swi - kSvcModBase) >> 16), i = (int)(swi & 0xFFFF);
                fprintf(f, "%s+%X\n", gModName[k], (unsigned)gModOff[k][i]);
            }
            fclose(f);
        }
    }
}

static void LoadOffList()
{
    gOffMain.clear();
    gOffMod.clear();
    FILE* f = _wfopen(Conf::desligarPath.c_str(), L"r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.empty() || l[0] == '#') continue;
        if (l.find('+') != std::string::npos) gOffMod.insert(l);
        else gOffMain.insert((uint32_t)strtoul(l.c_str(), nullptr, 16));
    }
    fclose(f);
    fprintf(stderr, "recomp: recomp_desligar.txt: %zu do programa e %zu de módulos ficam no JIT\n", gOffMain.size(), gOffMod.size());
}

template <class T>
static T Sym(const char* name) { return (T)GetProcAddress(gDll, name); }

bool Start(Core::System& system, const std::wstring& exeDir)
{
    gActive = false;
    gSystem = &system;
    const char* off = getenv("PORTE_RECOMP");
    if (off && atoi(off) == 0)
    {
        fprintf(stderr, "recomp: desligado (PORTE_RECOMP=0), só JIT\n");
        return false;
    }
    // which of the four games this is (the files of each one: jogos.h)
    {
        u64 pid = 0;
        system.GetAppLoader().ReadProgramId(pid);
        if (const Jogos::Jogo* j = Jogos::Por(pid)) Jogos::gAtual = j;
    }
    const Jogos::Jogo& jogo = *Jogos::gAtual;
    fprintf(stderr, "recomp: jogo Pokémon %s, código nativo %ls\n", jogo.nome, jogo.dll);
    if (!gDll) gDll = LoadLibraryW((exeDir + jogo.dll).c_str());
    if (!gDll)
    {
        fprintf(stderr, "recomp: momiji_nativo.dll não encontrada ao lado do exe: só JIT\n");
        return false;
    }
    auto* count = Sym<const int*>("recomp_count");
    gAddr = Sym<const uint32_t*>("recomp_addr");
    gFn = Sym<Fn const*>("recomp_fn");
    gCallNative = Sym<void (*)(Fn, Cpu*)>("call_native");
    auto setHost = Sym<void (*)(RdFn, RdFn, RdFn, WrFn, WrFn, WrFn)>("recomp_set_host");
    if (!count || !gAddr || !gFn || !gCallNative || !setHost)
    {
        fprintf(stderr, "recomp: DLL incompleta: só JIT\n");
        return false;
    }
    gCount = *count;
    // modules (a DLL without them still works: main program only)
    if (auto* mc = Sym<const int*>("recomp_mod_count"))
    {
        gModCount = *mc;
        gModName = Sym<const char* const*>("recomp_mod_name");
        gModNfn = Sym<const int*>("recomp_mod_nfn");
        gModOff = Sym<const uint32_t* const*>("recomp_mod_off");
        gModFn = Sym<Fn const* const*>("recomp_mod_fn");
        gModBase = Sym<uint32_t*>("recomp_modbase");
        gModLoad = Sym<void (*)(int, uint32_t, uint32_t)>("recomp_mod_load");
        gModUnload = Sym<void (*)(int)>("recomp_mod_unload");
        gModOrig = Sym<const uint32_t* const*>("recomp_mod_orig");
        if (!gModName || !gModNfn || !gModOff || !gModFn || !gModBase || !gModLoad || !gModUnload) gModCount = 0;
    }
    if (getenv("PORTE_RECOMP_SEM_MODULOS")) gModCount = 0;
    gMem = &system.Memory();
    gMissLog = getenv("PORTE_RECOMP_MISS") != nullptr;
    const std::wstring suf = jogo.sufixo;   // (Ultra Moon: the names of before)
    Conf::desligarPath = exeDir + L"recomp_desligar" + suf + L".txt";
    std::wstring outDir = exeDir;
    if (const char* pd = getenv("PORTE_CONFERIR_PASTA"))
    {
        wchar_t w[MAX_PATH * 2] = L"";
        MultiByteToWideChar(CP_UTF8, 0, pd, -1, w, MAX_PATH * 2);
        outDir = w;
        if (!outDir.empty() && outDir.back() != L'\\' && outDir.back() != L'/') outDir += L"\\";
        CreateDirectoryW(outDir.c_str(), nullptr);
    }
    Conf::desligarOutPath = outDir + L"recomp_desligar" + suf + L".txt";
    gFaltamPath = outDir + L"recomp_faltam" + suf + L".txt";
    gPerfilPath = outDir + L"recomp_perfil" + suf + L".txt";
    gEsperasPath = outDir + L"recomp_esperas" + suf + L".txt";
    gEsperas = getenv("PORTE_RECOMP_ESPERAS") != nullptr;
    if (const char* v = getenv("PORTE_60FPS"); v && jogo.tidLow == 0x001B5100) g60 = atoi(v);   // (Ultra Moon addresses)
    if (const char* v = getenv("PORTE_60FPS_DESDE")) g60Desde = (DWORD)atoi(v);
    g60T0 = GetTickCount();
    if (!getenv("PORTE_RECOMP_SEM_LISTA")) LoadOffList();
    if (const char* c = getenv("PORTE_CONFERIR"); c && atoi(c) > 0)
    {
        if (Conf::LoadUnicorn(exeDir))
        {
            Conf::on = true;
            Conf::every = atoi(c);
            if (const char* f = getenv("PORTE_CONFERIR_SEGUE")) Conf::followNative = strcmp(f, "nativo") == 0;
            Conf::logReads = getenv("PORTE_CONFERIR_LEITURAS") != nullptr;
            Conf::report = _wfopen((outDir + L"conferencia.txt").c_str(), L"w");
            fprintf(stderr, "conferência: ligada (%s)\n", Conf::every <= 1 ? "todas as chamadas" : "primeiras chamadas de cada função");
        }
        else fprintf(stderr, "conferência: unicorn.dll não encontrada ao lado do exe: desligada\n");
    }
    setHost(HRd8, HRd16, HRd32, HWr8, HWr16, HWr32);
    // (22/11, research) PORTE_GANCHO=1: the generated code calls us at ModelInstanceNode::PreDraw and CTRGL::FlushAllDirtyCommand_
    if (getenv("PORTE_GANCHO"))
        if (auto** hk = Sym<void (**)(Cpu*, uint32_t)>("recomp_hook"))
        {
            *hk = [](Cpu* c, uint32_t at) {
                static int dumps = 0, pds = 0;
                static uint32_t ctrgl = 0;
                if (at == 0x00354150u)
                {
                    ctrgl = c->r[0];
                    if (dumps < 40 && (dumps++ % 4) == 0)
                    {
                        std::string t;
                        for (int k = 0; k < 64; k++) { char x[16]; snprintf(x, sizeof(x), " %08X", HRd32(ctrgl + 4 * k)); t += x; }
                        fprintf(stderr, "CTRGL this %08X:%s\n", ctrgl, t.c_str());
                    }
                }
                else if (pds < 3000)
                {
                    pds++;
                    std::string t;
                    if (ctrgl) for (int k = 0; k < 64; k++) { char x[16]; snprintf(x, sizeof(x), " %08X", HRd32(ctrgl + 4 * k)); t += x; }
                    fprintf(stderr, "PREDRAW %08X modelo %08X ctrgl%s\n", at, c->r[0], t.c_str());
                }
            };
            fprintf(stderr, "recomp: gancho de pesquisa ligado\n");
        }
    if (const char* sd = getenv("PORTE_SVC_DIRETO")) gSvcDireto = atoi(sd) != 0;
    if (auto setSvc = Sym<void (*)(int (*)(Cpu*, uint32_t, uint32_t))>("recomp_set_svc")) setSvc(HSvc);
    else gSvcDireto = false;
    // (fase C: this used to sit between the "if" above and its "else", which then turned the direct
    // svc off in every run)
    gPagesVar = Sym<u8***>("recomp_pages");
    g60PassoAnim = getenv("PORTE_60FPS_SEMPASSO") ? nullptr : Sym<float*>("recomp_passo_anim");
    if (getenv("PORTE_MEMORIA_LENTA")) gPagesVar = nullptr;   // (testing aid: every access through the exe)

    const int maxN = getenv("PORTE_RECOMP_MAX") ? atoi(getenv("PORTE_RECOMP_MAX")) : gCount;
    gPatched.clear();
    gOrigMain.assign(gCount, 0);
    // (22/11) every original word is kept, also past PORTE_RECOMP_MAX: a state saved with all functions native can then
    // be loaded with fewer (the svc words of the others are put back to the original code)
    for (int i = 0; i < gCount; i++)
    {
        const uint32_t w = gMem->Read32(gAddr[i]);
        gOrigMain[i] = (w == SvcWord(MainSvc(i))) ? 0 : w;
        if ((int)gPatched.size() >= maxN || gOffMain.count(gAddr[i])) continue;
        gPatched.push_back(i);
    }
    gSvcAt.clear();
    for (int i : gPatched) { gSvcAt[gAddr[i]] = MainSvc(i); Conf::SetSvcBit(gAddr[i], true); }
    if (Conf::on)
    {
        // clean copy of the main program's code (before the svc go in)
        const auto proc = system.Kernel().GetCurrentProcess();
        const auto& seg = proc->codeset->CodeSegment();
        std::vector<std::pair<uint32_t, uint32_t>> orig;
        for (int i = 0; i < gCount; i++) orig.push_back({ gAddr[i], gOrigMain[i] });
        Conf::shadows.clear();
        Conf::AddShadow(0, seg.addr, seg.addr + seg.size, orig);
        fprintf(stderr, "conferência: cópia limpa do programa %08X-%08X\n", (unsigned)seg.addr, (unsigned)(seg.addr + seg.size));
    }
    PatchMain(system);
    gActive = true;
    int modFns = 0;
    for (int k = 0; k < gModCount; k++) modFns += gModNfn[k];
    fprintf(stderr, "recomp: %d funções nativas no programa principal, %d módulos com %d funções (ligadas quando carregam)\n",
            (int)gPatched.size(), gModCount, modFns);
    return true;
}

bool Active() { return gActive; }

void OnCroLoaded(const std::string& name, uint32_t base, uint32_t fixedSize, uint32_t fileSize)
{
    if (!gActive) return;
    // the module's .text only (segment table, type 0): .rodata gets pointers written into it later
    // (when other modules link to this one), so Unicorn must read it live, not from a copy.
    // Header fields and segment offsets are addresses once ldr:ro has loaded the module.
    uint32_t codeLo = 0, codeSz = 0;
    {
        uint32_t tab = gMem->Read32(base + 0xC8);
        const uint32_t n = gMem->Read32(base + 0xCC);
        if (tab < base) tab += base;
        for (uint32_t j = 0; j < n && j < 8; j++)
        {
            uint32_t off = gMem->Read32(tab + 12 * j), size = gMem->Read32(tab + 12 * j + 4);
            const uint32_t type = gMem->Read32(tab + 12 * j + 8);
            if (type != 0 || !size) continue;
            if (off < base) off += base;
            if (off < base || off + size > base + fixedSize) continue;
            codeLo = off;
            codeSz = size;
        }
        if (!codeSz) fprintf(stderr, "conferência: %s sem segmento de texto legível\n", name.c_str());
    }
    int k = -1;
    for (int i = 0; i < gModCount; i++)
        if (name == gModName[i]) { k = i; break; }
    if (k < 0)
    {
        if (Conf::on && codeSz) Conf::AddShadow(base, codeLo, codeLo + codeSz, {});
        Conf::needRefresh = true;
        fprintf(stderr, "recomp: módulo %s não traduzido: fica no JIT\n", name.c_str());
        return;
    }
    // the original first words: from the DLL when it has them (after a state load the memory may
    // already have our svc), else from memory as the loader left it
    auto origWord = [&](int i) -> uint32_t {
        if (gModOrig) return gModOrig[k][i];
        const uint32_t w = gMem->Read32(base + gModOff[k][i]);
        return w == SvcWord(ModSvc(k, i)) ? 0u : w;
    };
    {
        std::vector<std::pair<uint32_t, uint32_t>> orig;
        if (gModOrig)
            for (int i = 0; i < gModNfn[k]; i++) orig.push_back({ base + gModOff[k][i], gModOrig[k][i] });
        if (Conf::on && codeSz) Conf::AddShadow(base, codeLo, codeLo + codeSz, orig);   // before the svc go in
    }
    if (Conf::on) fprintf(stderr, "conferência: %s texto %08X-%08X\n", name.c_str(), (unsigned)codeLo, (unsigned)(codeLo + codeSz));
    Loaded L{ k, base, fixedSize, {} };
    L.original.resize(gModNfn[k]);
    int patched = 0;
    for (int i = 0; i < gModNfn[k]; i++)
    {
        const uint32_t a = base + gModOff[k][i];
        if (gModOff[k][i] + 4 > fixedSize) { L.original[i] = 0; continue; }
        char key[96];
        snprintf(key, sizeof(key), "%s+%X", name.c_str(), (unsigned)gModOff[k][i]);
        if (gOffMod.count(key))
        {
            L.original[i] = 0;
            const uint32_t o = origWord(i);
            if (o && gMem->Read32(a) != o) gMem->Write32(a, o);   // an svc left by a saved state
            continue;
        }
        L.original[i] = origWord(i);
        if (!L.original[i]) continue;   // unknown original: leave it to the JIT
        gMem->Write32(a, SvcWord(ModSvc(k, i)));
        gSvcAt[a] = ModSvc(k, i);
        Conf::SetSvcBit(a, true);
        if (Conf::on)
            for (auto& sh : Conf::shadows)
                if (sh.base == base)
                {
                    sh.orig.push_back({ a, L.original[i] });
                    for (uint32_t m : sh.mixed)
                        if (a - m < 0x1000) sh.fixes.push_back({ a, L.original[i] });
                }
        patched++;
    }
    gModLoad(k, base, fixedSize);
    gLoaded.push_back(std::move(L));
    Conf::needRefresh = true;
    gSystem->InvalidateCacheRange(base, fixedSize);
    fprintf(stderr, "recomp: módulo %s em %08X: %d funções nativas\n", name.c_str(), (unsigned)base, patched);
}

void OnCrsLoaded(uint32_t address)
{
    gCrs = address;
    fprintf(stderr, "recomp: static.crs em %08X\n", (unsigned)address);
}

static void RescanModules();
// a state load runs Shutdown + Init: memory, kernel and CPUs are new objects. So the memory is
// taken again here (start of every native call: the JIT may call us before the next Tick), and a
// new process object means "state loaded": main program and modules are set up again
static void CheckState(Core::System& system)
{
    gMem = &system.Memory();
    if (gPagesVar)
    {
        // the live check needs every access through the exe (sandbox, "svc read as data")
        const auto pt = gMem->GetCurrentPageTable();
        *gPagesVar = (Conf::on || !pt) ? nullptr : pt->GetPointerArray().data();
    }
    const auto procPtr = system.Kernel().GetCurrentProcess();
    const void* proc = procPtr.get();
    // (21/11) also the core's load counter: the new process can sit at the old one's address (rewind)
    static unsigned long long lastGen = Core::g_porte_load_gen;
    const bool loaded = Core::g_porte_load_gen != lastGen;
    if (proc && gLastProcess && (proc != gLastProcess || loaded))
    {
        lastGen = Core::g_porte_load_gen;
        gLastProcess = proc;
        Conf::dirty = true;   // Unicorn points into the old memory
        gSvcCtx.reset();      // it holds the old kernel
        if (gOff.empty()) gOff.assign(gCount, 0);
        std::vector<char> on(gCount, 0);
        for (int i : gPatched) on[i] = !gOff[i];
        int fixed = 0;
        for (int i = 0; i < gCount; i++)
        {
            const uint32_t want = on[i] ? SvcWord(MainSvc(i)) : gOrigMain[i];
            if (want && gMem->Read32(gAddr[i]) != want) { gMem->Write32(gAddr[i], want); fixed++; }
        }
        RescanModules();
        for (u32 c = 0; c < system.GetNumCores(); c++) system.GetCore(c).ClearInstructionCache();
        fprintf(stderr, "recomp: estado carregado: programa principal acertado (%d palavras)\n", fixed);
    }
    if (proc) gLastProcess = proc;
}

// after a state load: the modules are the state's
static void RescanModules()
{
    // forget the old registration (the memory is the state's now: nothing is written back)
    for (auto& L : gLoaded)
    {
        for (int i = 0; i < gModNfn[L.k]; i++)
        {
            const uint32_t a = L.base + gModOff[L.k][i];
            gSvcAt.erase(a);
            Conf::SetSvcBit(a, false);
        }
        gModUnload(L.k);
    }
    gLoaded.clear();
    if (Conf::on)
    {
        for (size_t j = Conf::shadows.size(); j-- > 0;)
            if (Conf::shadows[j].base) Conf::shadows.erase(Conf::shadows.begin() + j);
        Conf::needRefresh = true;
    }
    if (!gCrs) { fprintf(stderr, "recomp: estado carregado, mas o static.crs ainda não é conhecido\n"); return; }
    // the two lists (auto-linked modules: crs+0x88; the others: crs+0x8C), each one by NextCRO
    int n = 0;
    fprintf(stderr, "recomp: static.crs %08X: magia %08X, listas %08X / %08X\n", (unsigned)gCrs, (unsigned)gMem->Read32(gCrs + 0x80),
            (unsigned)gMem->Read32(gCrs + 0x88), (unsigned)gMem->Read32(gCrs + 0x8C));
    for (uint32_t headField : { 0x88u, 0x8Cu })
    {
        uint32_t cur = gMem->Read32(gCrs + headField);
        for (int guard = 0; cur && guard < 256; guard++)
        {
            const uint32_t magic = gMem->Read32(cur + 0x80);
            const uint32_t nameAddr = gMem->Read32(cur + 0xC0), nameSize = gMem->Read32(cur + 0xC4);
            std::string name;
            for (uint32_t c = 0; c < nameSize && c < 64; c++)
            {
                const char ch = (char)gMem->Read8(nameAddr + c);
                if (!ch) break;
                name += ch;
            }
            bool okName = !name.empty();
            for (char ch : name) okName = okName && ch >= 0x20 && ch < 0x7F;
            if (!okName) { fprintf(stderr, "recomp: módulo em %08X ilegível (magia %08X)", (unsigned)cur, (unsigned)magic); fputc(10, stderr); break; }
            uint32_t fixed = gMem->Read32(cur + 0x98);
            const uint32_t fileSize = gMem->Read32(cur + 0x90);
            if (!fixed || fixed > fileSize) fixed = fileSize;
            OnCroLoaded(name, cur, fixed, fileSize);
            n++;
            cur = gMem->Read32(cur + 0x88);
        }
    }
    fprintf(stderr, "recomp: estado carregado: %d módulos registrados de novo\n", n);
}

bool ModuleLoaded(const char* name)
{
    if (!gModName) return false;
    for (const auto& L : gLoaded)
        if (L.k >= 0 && L.k < gModCount && gModName[L.k] && strcmp(gModName[L.k], name) == 0) return true;
    return false;
}

void OnCroUnload(uint32_t base)
{
    if (Conf::on) { Conf::RemoveShadow(base); Conf::needRefresh = true; }
    for (size_t j = 0; j < gLoaded.size(); j++)
    {
        if (gLoaded[j].base != base) continue;
        Loaded& L = gLoaded[j];
        // put the game's code back as it was (the buffer stays the game's)
        for (int i = 0; i < gModNfn[L.k]; i++)
            if (L.original[i])
            {
                gMem->Write32(base + gModOff[L.k][i], L.original[i]);
                gSvcAt.erase(base + gModOff[L.k][i]);
                Conf::SetSvcBit(base + gModOff[L.k][i], false);
            }
        gModUnload(L.k);
        gSystem->InvalidateCacheRange(base, L.size);
        gLoaded.erase(gLoaded.begin() + j);
        return;
    }
}

void CountJit(uint64_t ticks) { gJitTicks.fetch_add(ticks, std::memory_order_relaxed); }

void NoteReschedule() { gResched = true; }

// the same system call the JIT would make (Kernel::SVCContext), with the native registers; if the
// kernel asks for a thread switch, hand back to the JIT right after the svc (the switch happens there)
static int HSvc(Cpu* s, uint32_t imm, uint32_t next)
{
    if (!gSvcDireto || Conf::sandbox || !gCurCpu || !gSystem) return 0;
    auto& cpu = *gCurCpu;
    for (int k = 0; k < 15; k++) cpu.SetReg(k, s->r[k]);
    cpu.SetPC(next & ~1u);
    // the emulated clock sees the native instructions run so far (GetSystemTick, timeouts)
    cpu.GetTimer().AddTicks(s->icount - gFlushed);
    gFlushed = s->icount;
    if (!gSvcCtx) gSvcCtx = std::make_unique<Kernel::SVCContext>(*gSystem);
    if (gEsperas && imm == 0x0A)
    {
        // SleepThread: r0:r1 = nanoseconds (frame pacing by sleeping?)
        const uint64_t ns = (uint64_t)s->r[0] | ((uint64_t)s->r[1] << 32);
        char key[160];
        snprintf(key, sizeof(key), "svc 0A | lr %08X | dorme %.1f ms", (unsigned)s->r[14], ns / 1e6);
        gEsperasN[key]++;
    }
    if (gEsperas && (imm == 0x24 || imm == 0x25))
    {
        // which object: WaitSynchronization1 r0 = handle; WaitSynchronizationN r1 = list of handles, r2 = how many
        std::string obj = "?";
        if (auto proc = gSystem->Kernel().GetCurrentProcess())
        {
            auto name = [&](uint32_t h) {
                auto o = proc->handle_table.GetGeneric(h);
                return o ? o->GetTypeName() + ":" + o->GetName() : std::string("h") + std::to_string(h);
            };
            if (imm == 0x24) obj = name(s->r[0]);
            else
            {
                obj.clear();
                const uint32_t n = std::min<uint32_t>(s->r[2], 4);
                for (uint32_t i = 0; i < n; i++) obj += (i ? "+" : "") + name(gMem->Read32(s->r[1] + 4 * i));
            }
        }
        char key[300];
        snprintf(key, sizeof(key), "svc %02X | lr %08X | %s", (unsigned)imm, (unsigned)s->r[14], obj.c_str());
        gEsperasN[key]++;
        const DWORD now = GetTickCount();
        if (!gEsperasT0) gEsperasT0 = now;
        if (now - gEsperasLast > 5000 && !gEsperasPath.empty())
        {
            gEsperasLast = now;
            if (FILE* f = _wfopen(gEsperasPath.c_str(), L"w"))
            {
                const double secs = (now - gEsperasT0) / 1000.0;
                std::vector<std::pair<uint64_t, std::string>> v;
                for (auto& [k, c] : gEsperasN) v.push_back({ c, k });
                std::sort(v.rbegin(), v.rend());
                fprintf(f, "# esperas pelo svc direto em %.0f s (por segundo | chamada)\n", secs);
                for (auto& [c, k] : v) fprintf(f, "%7.1f/s  %s\n", c / secs, k.c_str());
                fclose(f);
            }
            gEsperasN.clear();
            gEsperasT0 = now;
        }
    }
    gResched = false;
    gSvcCtx->CallSVC(imm);
    for (int k = 0; k < 15; k++) s->r[k] = cpu.GetReg(k);
    gSvcDone.fetch_add(1, std::memory_order_relaxed);
    if (gResched)
    {
        s->fault = 7;
        s->miss = next;
        gSvcBack.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return 1;
}

static int gPerfil = -1;
static std::unordered_map<uint32_t, uint64_t> gPerfilHits;
static DWORD gPerfilLast = 0;
void SampleJit(uint32_t pc)
{
    if (gPerfil < 0) gPerfil = getenv("PORTE_RECOMP_PERFIL") ? 1 : 0;
    if (!gPerfil || !gActive) return;
    gPerfilHits[pc]++;
    const DWORD now = GetTickCount();
    if (now - gPerfilLast < 10000 || gPerfilPath.empty()) return;
    gPerfilLast = now;
    std::vector<std::pair<uint64_t, uint32_t>> v;
    uint64_t total = 0;
    for (auto& [a, n] : gPerfilHits) { v.push_back({ n, a }); total += n; }
    std::sort(v.rbegin(), v.rend());
    if (FILE* f = _wfopen(gPerfilPath.c_str(), L"w"))
    {
        fprintf(f, "# amostras %llu (PC do JIT no fim de cada fatia)\n", (unsigned long long)total);
        for (size_t i = 0; i < v.size() && i < 400; i++)
        {
            const uint32_t a = v[i].second;
            std::string where = "principal";
            for (auto& L : gLoaded)
                if (a - L.base < L.size) { char b[96]; snprintf(b, sizeof(b), "%s+%X", gModName[L.k], (unsigned)(a - L.base)); where = b; break; }
            fprintf(f, "%08X %s %llu\n", (unsigned)a, where.c_str(), (unsigned long long)v[i].first);
        }
        fclose(f);
    }
}

extern "C" unsigned long long g_dyncomClears[3], g_dyncomBlocks;
void Tick(Core::System& system)
{
    {   // (diagnosis) the interpreter's block cache: thrown away how often, blocks translated
        static DWORD t0 = GetTickCount();
        static unsigned long long c0[3] = {}, b0 = 0;
        static const bool show = getenv("PORTE_HIDDEN") != nullptr;
        if (show && GetTickCount() - t0 >= 1000)
        {
            t0 = GetTickCount();
            fprintf(stderr, "dyncom: limpezas/s Clear %llu (Invalidate %llu, SetPageTable %llu), blocos traduzidos/s %llu\n",
                    g_dyncomClears[0] - c0[0], g_dyncomClears[1] - c0[1], g_dyncomClears[2] - c0[2], g_dyncomBlocks - b0);
            for (int i = 0; i < 3; i++) c0[i] = g_dyncomClears[i];
            b0 = g_dyncomBlocks;
        }
    }
    if (!gActive) return;
    CheckState(system);
    uint32_t gfxMode = 0xFF;
    if (gMem)
    {
        const uint32_t gfx = gMem->Read32(0x006A3984);
        if (gfx && gMem->GetPointer(gfx))
        {
            gfxMode = gMem->Read8(gfx + 9);
            // the scene's own wish: a request in +8 (0/1), or the mode in use when there is none
            const uint32_t req = gMem->Read8(gfx + 8);
            // (a request equal to the one we wrote ourselves is ours, not the scene's)
            static int wrote = -1;
            if (req >= 2) wrote = -1;
            if (req < 2 && (int)req != wrote) g60SceneWants30 = req == 1;
            else if (req >= 2 && (!g60Seen || (g60 == 1 && gfxMode == 1))) g60SceneWants30 = gfxMode == 1;   // (forcing 60, only the scene sets 30)
            g60Seen = true;
            if ((g60 == 1 || g60 == 3) && GetTickCount() - g60T0 >= g60Desde)
            {
                const uint32_t want = g60 == 1 ? 0 : 1;   // 1: força 60; 3: força 30 (teste)
                const uint32_t now = req < 2 ? req : gfxMode;
                if (now != want) { gMem->Write8(gfx + 8, want); wrote = (int)want; g60Forced++; }
            }
            // animations (gfl2 frame += step): half a step while a 30 scene runs at 60
            if (g60PassoAnim) *g60PassoAnim = (g60 == 1 && g60SceneWants30 && GetTickCount() - g60T0 >= g60Desde) ? 0.5f : 1.0f;
        }
    }
    const DWORD now = GetTickCount();
    if (now - gLastPrint < 1000) return;
    gLastPrint = now;
    if (g60) fprintf(stderr, "60fps: modo em uso %u (0=60, 1=30), cena quer %s, forçado %u vezes, passo anim %s\n", gfxMode,
                     g60SceneWants30 ? "30" : "60", g60Forced, g60PassoAnim ? (*g60PassoAnim < 1 ? "0.5" : "1") : "sem DLL nova");
    const uint64_t calls = gCalls.load(), insns = gNativeInsns.load(), jit = gJitTicks.load();
    uint64_t back[8];
    for (int k = 0; k < 8; k++) back[k] = gBack[k].load();
    const double n = double(insns - gLastInsns), j = double(jit - gLastJit);
    char buf[320];
    snprintf(buf, sizeof(buf),
             "nativo: %.0f%% das instruções (%.1f M nativas/s, %.1f M no JIT/s), %llu chamadas/s; devolvidas ao JIT: sem tradução %llu, sistema %llu, Thumb %llu, tempo %llu",
             n + j > 0 ? 100.0 * n / (n + j) : 0.0, n / 1e6, j / 1e6, (unsigned long long)(calls - gLastCalls),
             (unsigned long long)(back[4] - gLastBack[4]), (unsigned long long)(back[6] - gLastBack[6] + back[3] - gLastBack[3]),
             (unsigned long long)(back[5] - gLastBack[5]), (unsigned long long)(back[2] - gLastBack[2]));
    gStatus = buf;
    fprintf(stderr, "%s\n", buf);
    {
        static uint64_t lastDone = 0, lastBack = 0;
        const uint64_t d = gSvcDone.load(), b = gSvcBack.load();
        if (gSvcDireto) fprintf(stderr, "svc direto: %llu/s feitos no nativo (%llu/s pediram troca de thread)\n",
                                (unsigned long long)(d - lastDone), (unsigned long long)(b - lastBack));
        lastDone = d;
        lastBack = b;
    }
    if (Conf::on)
    {
        const uint64_t c = Conf::nChecked.load();
        fprintf(stderr, "conferência: %llu conferidas/s (total %llu), diferenças %llu, desligadas %llu, svc lidos como dado %llu, não conferíveis %llu, instáveis %llu\n",
                (unsigned long long)(c - Conf::lastChecked), (unsigned long long)c, (unsigned long long)Conf::nDiff.load(),
                (unsigned long long)Conf::nOff.load(), (unsigned long long)Conf::nData.load(), (unsigned long long)Conf::nSkip.load(),
                (unsigned long long)Conf::nUnstable.load());
        Conf::lastChecked = c;
        if (Conf::report) fflush(Conf::report);
    }
    if (gMissLog && now - gLastMissPrint > 10000)
    {
        gLastMissPrint = now;
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto& [a, n] : gMiss) v.push_back({ n, a });
        std::sort(v.rbegin(), v.rend());
        fprintf(stderr, "recomp: mais devolvidos ao JIT (10 s):\n");
        for (size_t i = 0; i < v.size() && i < 20; i++)
        {
            const uint32_t a = v[i].second & 0x7FFFFFFF;
            std::string where = "principal";
            for (auto& L : gLoaded)
                if (a - L.base < L.size) { where = std::string(gModName[L.k]) + "+" + std::to_string(a - L.base); break; }
            fprintf(stderr, "  %08X %s %s  %llu\n", (unsigned)a, (v[i].second & 0x80000000u) ? "svc" : "   ", where.c_str(),
                    (unsigned long long)v[i].first);
        }
        gMiss.clear();
        if (!gFaltamPath.empty())
        {
            // add to what earlier runs found
            if (FILE* f = _wfopen(gFaltamPath.c_str(), L"r"))
            {
                char line[128];
                while (fgets(line, sizeof(line), f))
                {
                    std::string l = line;
                    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                    if (!l.empty()) gFaltam.insert(l);
                }
                fclose(f);
            }
            if (FILE* f = _wfopen(gFaltamPath.c_str(), L"w"))
            {
                for (auto& k : gFaltam) fprintf(f, "%s\n", k.c_str());
                fclose(f);
            }
        }
    }
    gLastCalls = calls;
    gLastInsns = insns;
    gLastJit = jit;
    for (int k = 0; k < 8; k++) gLastBack[k] = back[k];
}

std::string StatusText() { return gStatus; }

bool RunNative(Core::ARM_Interface& cpu, uint32_t swi, uint64_t& ticks)
{
    ticks = 0;
    if (!gActive || swi < kSvcFirst) return false;
    CheckState(*gSystem);
    gCurCpu = &cpu;
    gFlushed = 0;
    Fn fn = nullptr;
    uint32_t entry = 0;
    if (swi >= kSvcBase)
    {
        const int i = (int)(swi - kSvcBase);
        if (i >= gCount) return false;
        fn = gFn[i];
        entry = gAddr[i];
    }
    else
    {
        const int k = (int)((swi - kSvcModBase) >> 16), i = (int)(swi & 0xFFFF);
        if (k >= gModCount || i >= gModNfn[k]) return false;
        fn = gModFn[k][i];
        entry = gModBase[k] + gModOff[k][i];
    }
    Cpu s;
    memset(&s, 0, sizeof(s));
    for (int k = 0; k < 15; k++) s.r[k] = cpu.GetReg(k);
    s.r[15] = entry;
    const uint32_t cpsr = cpu.GetCPSR();
    s.n = cpsr >> 31 & 1;
    s.z = cpsr >> 30 & 1;
    s.c = cpsr >> 29 & 1;
    s.v = cpsr >> 28 & 1;
    s.q = cpsr >> 27 & 1;
    s.ge = cpsr >> 16 & 0xF;
    for (int k = 0; k < 32; k++) s.s[k] = cpu.GetVFPReg(k);
    s.fpscr = cpu.GetVFPSystemReg(VFP_FPSCR);
    s.tls = cpu.GetCP15Register(CP15_THREAD_URO);
    s.ilimit = 4000000;
    const uint32_t entryLr = s.r[14];

    const bool check = Conf::on && Conf::ShouldCheck(swi);
    const Cpu in = s;
    if (Conf::on) Conf::dataHit = 0;
    if (check) { Conf::diary.Clear(); Conf::natReads.clear(); Conf::sandbox = true; }
    LARGE_INTEGER tq0, tq1, tq2, tq3, tqf;
    if (check) QueryPerformanceCounter(&tq0);
    gCallNative(fn, &s);
    // (22/11, research) PORTE_PREDRAW=1: count the game's calls to ModelInstanceNode::PreDraw (UM 0x00333758 / 0x004CD49C)
    // that leave the native code (the target has no translated entry -> JIT) and the model pointers (r0) seen
    {
        static const bool pd = getenv("PORTE_PREDRAW") != nullptr;
        if (pd && s.fault == 4 && (s.miss == 0x00333758u || s.miss == 0x004CD49Cu))
        {
            static std::unordered_map<uint32_t, uint32_t> seen;
            static uint64_t n = 0; static DWORD last = 0;
            n++; seen[s.r[0]]++;
            const DWORD now = GetTickCount();
            if (now - last > 1000)
            {
                last = now;
                fprintf(stderr, "predraw: %llu chamadas no último segundo, %zu modelos diferentes (alvo %08X)\n", (unsigned long long)n, seen.size(), s.miss);
                n = 0; seen.clear();
            }
        }
    }
    if (check) QueryPerformanceCounter(&tq1);
    Conf::sandbox = false;
    uint32_t forcePc = 0;
    bool usePc = false;
    if (check)
    {
        const uint32_t natPc = (s.fault == 0) ? entryLr : s.miss;
        const auto proc = gSystem->Kernel().GetCurrentProcess();
        auto commit = [&] { Conf::diary.Runs([&](uint32_t a, const uint8_t* d, uint32_t len) { gMem->WriteBlock(*proc, a, d, len); }); };
        auto restore = [&] { Conf::undo.Runs([&](uint32_t a, const uint8_t* d, uint32_t len) { gMem->WriteBlock(*proc, a, d, len); }); };
        if (s.fault == 2 || (s.fault && (natPc & ~1u) == entry) || !Conf::EnsureUc())
        {
            commit();
            Conf::nSkip++;
        }
        else
        {
            using namespace Conf;
            RefreshMixed();
            undo.Clear();
            ucReads.clear();
            ucWritePc.clear();
            ucFail = false;
            uint32_t v = (cpsr & 0xF80F0000u) | 0x10u;
            u_regw(uc, UC_ARM_REG_CPSR, &v);
            for (int k = 0; k < 13; k++) u_regw(uc, UC_ARM_REG_R0 + k, &in.r[k]);
            u_regw(uc, UC_ARM_REG_SP, &in.r[13]);
            u_regw(uc, UC_ARM_REG_LR, &in.r[14]);
            for (int k = 0; k < 32; k++) u_regw(uc, UC_ARM_REG_S0 + k, &in.s[k]);
            v = in.fpscr;
            u_regw(uc, UC_ARM_REG_FPSCR, &v);
            v = in.tls;
            u_regw(uc, UC_ARM_REG_C13_C0_3, &v);
            // stop by the count, never by an address: Unicorn decides "stop here" when it
            // translates, so blocks kept from an earlier call would ignore a new address. The
            // native count may include the instruction where it handed back (svc, untranslatable)
            // or not (a branch to code without translation): run one less, then one more if needed
            // stop on the native's last PC (first arrival there): an "exit" of Unicorn. Exits are
            // compiled into the translated blocks, so the blocks at the old and the new exit go
            hookTarget = natPc & ~1u;
            hookMin = 0;
            hookN = 0;
            atSvc = false;
            elsewhere = false;
            {
                uint64_t ex = hookTarget;
                if (lastExit && lastExit != ex) u_ctl(uc, UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2), lastExit, lastExit + 4);
                u_ctl(uc, UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2), ex, ex + 4);
                u_ctl(uc, UC_CTL_WRITE(UC_CTL_UC_EXITS, 2), &ex, (size_t)1);
                lastExit = ex;
            }
            QueryPerformanceCounter(&tq2);
            // the native's last PC can be reached earlier, at another depth (a recursive function
            // returning to itself): the stop counts only with the native's final SP too; otherwise
            // step one instruction past it (exit moved away) and go on
            // two steps: first (almost) the native's count with no exit, so that an earlier pass
            // over the last PC (an earlier turn of a loop: same PC, same SP) is not taken; then the
            // exit. The slack covers the native count being a little above now and then
            uc_err e = UC_ERR_OK;
            {
                const uint32_t slack = s.icount / 50 + 4;
                if (s.icount > slack + 1)
                {
                    uint64_t away = 0xFFFFFFF0u, ex = hookTarget;
                    u_ctl(uc, UC_CTL_WRITE(UC_CTL_UC_EXITS, 2), &away, (size_t)1);
                    u_ctl(uc, UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2), ex, ex + 4);
                    e = u_start(uc, entry, 0, 0, (size_t)(s.icount - slack));
                    u_ctl(uc, UC_CTL_WRITE(UC_CTL_UC_EXITS, 2), &ex, (size_t)1);
                    u_ctl(uc, UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2), ex, ex + 4);
                    uint32_t pc = 0, cp = 0;
                    u_regr(uc, UC_ARM_REG_PC, &pc);
                    u_regr(uc, UC_ARM_REG_CPSR, &cp);
                    if (e == UC_ERR_OK && !ucFail && !atSvc && !elsewhere)
                        e = u_start(uc, pc | ((cp & 0x20) ? 1u : 0u), 0, 0, (size_t)s.icount * 2 + 64);
                }
                else e = u_start(uc, entry, 0, 0, (size_t)s.icount * 2 + 64);
            }
            for (int again = 0; again < 256 && e == UC_ERR_OK && !ucFail && !atSvc && !elsewhere; again++)
            {
                uint32_t pc = 0, sp = 0, cp = 0;
                u_regr(uc, UC_ARM_REG_PC, &pc);
                u_regr(uc, UC_ARM_REG_SP, &sp);
                if ((pc & ~1u) != hookTarget || sp == s.r[13]) break;
                u_regr(uc, UC_ARM_REG_CPSR, &cp);
                uint64_t away = 0xFFFFFFF0u, ex = hookTarget;
                u_ctl(uc, UC_CTL_WRITE(UC_CTL_UC_EXITS, 2), &away, (size_t)1);
                u_ctl(uc, UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2), ex, ex + 4);
                e = u_start(uc, pc | ((cp & 0x20) ? 1u : 0u), 0, 0, 1);
                u_ctl(uc, UC_CTL_WRITE(UC_CTL_UC_EXITS, 2), &ex, (size_t)1);
                u_ctl(uc, UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2), ex, ex + 4);
                if (e != UC_ERR_OK || ucFail) break;
                u_regr(uc, UC_ARM_REG_PC, &pc);
                u_regr(uc, UC_ARM_REG_CPSR, &cp);
                e = u_start(uc, pc | ((cp & 0x20) ? 1u : 0u), 0, 0, (size_t)s.icount * 2 + 64);
            }
            QueryPerformanceCounter(&tq3);
            InvalidateWritten();
            Cpu u = in;
            for (int k = 0; k < 13; k++) u_regr(uc, UC_ARM_REG_R0 + k, &u.r[k]);
            u_regr(uc, UC_ARM_REG_SP, &u.r[13]);
            u_regr(uc, UC_ARM_REG_LR, &u.r[14]);
            uint32_t upc = 0, ucpsr = 0;
            u_regr(uc, UC_ARM_REG_PC, &upc);
            u_regr(uc, UC_ARM_REG_CPSR, &ucpsr);
            for (int k = 0; k < 32; k++) u_regr(uc, UC_ARM_REG_S0 + k, &u.s[k]);
            u_regr(uc, UC_ARM_REG_FPSCR, &u.fpscr);
            u.n = ucpsr >> 31 & 1; u.z = ucpsr >> 30 & 1; u.c = ucpsr >> 29 & 1; u.v = ucpsr >> 28 & 1; u.q = ucpsr >> 27 & 1;
            u.ge = ucpsr >> 16 & 0xF;
            if (ucpsr & 0x20) upc |= 1;
            if (atSvc) upc = natPc;   // it stopped right after the svc it was not meant to run
            if (elsewhere) { upc = elsewherePc; e = UC_ERR_OK; }   // compared below: "stopped at ..."
            if (e != UC_ERR_OK || ucFail)
            {
                // Unicorn could not run it: put the memory back as it was, the native result wins
                restore();
                commit();
                nSkip++;
                if (reported < 400 && report)
                {
                    reported++;
                    fprintf(report, "NÃO CONFERÍVEL %s: %s (pc %08X; nativo: saída %u em %08X, %u instruções; Unicorn %llu)\n", FnName(swi, entry).c_str(),
                            ucFail ? ucWhy : u_strerror(e), (unsigned)upc, (unsigned)s.fault, (unsigned)natPc, (unsigned)s.icount, (unsigned long long)hookN);
                    {
                        // the first read where the two saw different values
                        if (!logReads) goto semLeituras;
                        size_t q = 0;
                        while (q < natReads.size() && q < ucReads.size() && natReads[q] == ucReads[q]) q++;
                        if (q < natReads.size() || q < ucReads.size())
                            fprintf(report, "   1ª leitura diferente (nº %zu): nativo %08X=%08X, Unicorn %08X=%08X\n", q,
                                    q < natReads.size() ? (unsigned)natReads[q].first : 0u, q < natReads.size() ? (unsigned)natReads[q].second : 0u,
                                    q < ucReads.size() ? (unsigned)ucReads[q].first : 0u, q < ucReads.size() ? (unsigned)ucReads[q].second : 0u);
                    }
                    semLeituras:
                    fprintf(report, "   últimas do Unicorn:");
                    for (uint64_t q = hookN > 16 ? hookN - 16 : 0; q < hookN; q++) fprintf(report, " %08X", (unsigned)ring[q & 31]);
                    fprintf(report, "\n   registros do Unicorn:");
                    for (int k = 0; k < 15; k++) fprintf(report, " r%d=%08X", k, (unsigned)u.r[k]);
                    fprintf(report, "\n");
                }
            }
            else
            {
                nChecked++;
                // compare: where it stopped, registers, flags, VFP, every byte written
                char why[200] = "";
                if ((upc & ~1u) != (natPc & ~1u))
                    snprintf(why, sizeof(why), "parou em %08X depois de %llu instruções (nativo em %08X)", (unsigned)upc, (unsigned long long)hookN, (unsigned)natPc);
                for (int k = 0; k < 15 && !why[0]; k++)
                    if (u.r[k] != s.r[k]) snprintf(why, sizeof(why), "r%d: nativo %08X, certo %08X", k, (unsigned)s.r[k], (unsigned)u.r[k]);
                if (!why[0] && (u.n != s.n || u.z != s.z || u.c != s.c || u.v != s.v))
                    snprintf(why, sizeof(why), "flags NZCV: nativo %u%u%u%u, certo %u%u%u%u", s.n, s.z, s.c, s.v, u.n, u.z, u.c, u.v);
                if (!why[0] && (u.q != s.q || u.ge != s.ge)) snprintf(why, sizeof(why), "Q/GE: nativo %u/%X, certo %u/%X", s.q, s.ge, u.q, u.ge);
                for (int k = 0; k < 32 && !why[0]; k++)
                    if (u.s[k] != s.s[k]) snprintf(why, sizeof(why), "s%d: nativo %08X, certo %08X", k, (unsigned)s.s[k], (unsigned)u.s[k]);
                if (!why[0] && ((u.fpscr ^ s.fpscr) & 0xF3C00000u)) snprintf(why, sizeof(why), "FPSCR: nativo %08X, certo %08X", (unsigned)s.fpscr, (unsigned)u.fpscr);
                if (!why[0])
                {
                    // every byte Unicorn wrote (its value is in memory now) against the native diary
                    undo.Runs([&](uint32_t a, const uint8_t*, uint32_t len) {
                        for (uint32_t i = 0; i < len && !why[0]; i++)
                        {
                            const uint32_t b = a + i;
                            const uint8_t now = undo.Find(b >> 12)->w[b & 0xFFFu];
                            Pg* g = diary.Find(b >> 12);
                            if (!g || !PageLog::Has(g, b & 0xFFFu)) snprintf(why, sizeof(why), "escrita em %08X só no certo (%02X)", (unsigned)b, now);
                            else if (g->d[b & 0xFFFu] != now)
                                snprintf(why, sizeof(why), "escrita em %08X: nativo %02X, certo %02X", (unsigned)b, g->d[b & 0xFFFu], now);
                        }
                    });
                    if (!why[0])
                        diary.Runs([&](uint32_t a, const uint8_t* d, uint32_t len) {
                            for (uint32_t i = 0; i < len && !why[0]; i++)
                            {
                                Pg* g = undo.Find((a + i) >> 12);
                                if (!g || !PageLog::Has(g, (a + i) & 0xFFFu))
                                    snprintf(why, sizeof(why), "escrita em %08X só no nativo (%02X)", (unsigned)(a + i), d[i]);
                            }
                        });
                }
                {
                    LARGE_INTEGER tq4;
                    QueryPerformanceCounter(&tq4);
                    QueryPerformanceFrequency(&tqf);
                    const double ms = (tq4.QuadPart - tq0.QuadPart) * 1000.0 / tqf.QuadPart;
                    if (ms > 50 && report && reported < 400)
                    {
                        reported++;
                        fprintf(report, "LENTA %s: %.0f ms (nativo %.1f, preparo %.1f, Unicorn %.1f, comparação %.1f) | %u instruções, %zu páginas nativo, %zu Unicorn\n",
                                FnName(swi, entry).c_str(), ms, (tq1.QuadPart - tq0.QuadPart) * 1000.0 / tqf.QuadPart,
                                (tq2.QuadPart - tq1.QuadPart) * 1000.0 / tqf.QuadPart, (tq3.QuadPart - tq2.QuadPart) * 1000.0 / tqf.QuadPart,
                                (tq4.QuadPart - tq3.QuadPart) * 1000.0 / tqf.QuadPart, (unsigned)s.icount, diary.pages.size(), undo.pages.size());
                    }
                }
                if (why[0])
                {
                    restore();   // memory as it was before Unicorn
                    PageLog d1;
                    std::swap(d1.pages, diary.pages);
                    diary.lastP = ~0u; diary.last = nullptr;
                    Cpu again = in;
                    sandbox = true;
                    gCallNative(fn, &again);
                    sandbox = false;
                    bool same = again.fault == s.fault && again.miss == s.miss && again.icount == s.icount &&
                                !memcmp(again.r, s.r, sizeof(s.r)) && !memcmp(again.s, s.s, sizeof(s.s)) &&
                                again.n == s.n && again.z == s.z && again.c == s.c && again.v == s.v;
                    if (same)
                        d1.Runs([&](uint32_t a, const uint8_t* dd, uint32_t len) {
                            for (uint32_t i = 0; i < len && same; i++)
                            {
                                Pg* g = diary.Find((a + i) >> 12);
                                same = g && PageLog::Has(g, (a + i) & 0xFFFu) && g->d[(a + i) & 0xFFFu] == dd[i];
                            }
                        });
                    // put Unicorn's writes back (the game goes on with one result or the other below)
                    undo.Runs([&](uint32_t a, const uint8_t*, uint32_t len) {
                        for (uint32_t i = 0; i < len; i++) gMem->Write8(a + i, undo.Find((a + i) >> 12)->w[(a + i) & 0xFFFu]);
                    });
                    diary.Clear();
                    std::swap(diary.pages, d1.pages);
                    diary.lastP = ~0u; diary.last = nullptr;
                    for (auto& [pg, g] : d1.pages) d1.pool.push_back(g);
                    d1.pages.clear();
                    if (!same)
                    {
                        nUnstable++;
                        if (report && reported < 400)
                        {
                            reported++;
                            fprintf(report, "INSTÁVEL %s: %s (o próprio nativo deu outro resultado na 2a vez: a memória mudou por fora)\n",
                                    FnName(swi, entry).c_str(), why);
                        }
                        why[0] = 0;   // not a translation error: keep the function on (memory has Unicorn's writes)
                        for (int k = 0; k < 15; k++) s.r[k] = u.r[k];
                        for (int k = 0; k < 32; k++) s.s[k] = u.s[k];
                        s.n = u.n; s.z = u.z; s.c = u.c; s.v = u.v; s.q = u.q; s.ge = u.ge; s.fpscr = u.fpscr;
                        forcePc = upc;
                        usePc = true;
                    }
                }
                if (why[0] && getenv("PORTE_CONFERIR_BISSECAO") && report && reported < 400)
                {
                    // run both again from the same input, stopping after N instructions, and halve N:
                    // the native code can only stop at a checked label (block heads), so the answer is
                    // "the block starting at X" (memory restored before every run)
                    uint64_t away = 0xFFFFFFF0u;
                    if (lastExit) u_ctl(uc, UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2), lastExit, lastExit + 4);
                    u_ctl(uc, UC_CTL_WRITE(UC_CTL_UC_EXITS, 2), &away, (size_t)1);
                    lastExit = 0;
                    auto natTo = [&](uint32_t lim, Cpu& o) {
                        restore();
                        diary.Clear();
                        o = in;
                        o.ilimit = lim;
                        sandbox = true;
                        gCallNative(fn, &o);
                        sandbox = false;
                    };
                    auto ucTo = [&](uint32_t n, Cpu& o, uint32_t& opc) {
                        restore();
                        undo.Clear();
                        ucWritePc.clear();
                        o = in;
                        uint32_t v2 = (cpsr & 0xF80F0000u) | 0x10u;
                        u_regw(uc, UC_ARM_REG_CPSR, &v2);
                        for (int k = 0; k < 13; k++) u_regw(uc, UC_ARM_REG_R0 + k, &in.r[k]);
                        u_regw(uc, UC_ARM_REG_SP, &in.r[13]);
                        u_regw(uc, UC_ARM_REG_LR, &in.r[14]);
                        for (int k = 0; k < 32; k++) u_regw(uc, UC_ARM_REG_S0 + k, &in.s[k]);
                        v2 = in.fpscr; u_regw(uc, UC_ARM_REG_FPSCR, &v2);
                        v2 = in.tls; u_regw(uc, UC_ARM_REG_C13_C0_3, &v2);
                        ucFail = false; atSvc = false; elsewhere = false;
                        if (n) u_start(uc, entry, 0, 0, n);
                        for (int k = 0; k < 13; k++) u_regr(uc, UC_ARM_REG_R0 + k, &o.r[k]);
                        u_regr(uc, UC_ARM_REG_SP, &o.r[13]);
                        u_regr(uc, UC_ARM_REG_LR, &o.r[14]);
                        u_regr(uc, UC_ARM_REG_PC, &opc);
                        for (int k = 0; k < 32; k++) u_regr(uc, UC_ARM_REG_S0 + k, &o.s[k]);
                        uint32_t cp = 0;
                        u_regr(uc, UC_ARM_REG_CPSR, &cp);
                        o.n = cp >> 31 & 1; o.z = cp >> 30 & 1; o.c = cp >> 29 & 1; o.v = cp >> 28 & 1;
                    };
                    uint32_t lo = 0, hi = s.icount, firstPc = 0, lastSamePc = entry;
                    std::string detail;
                    for (int it = 0; it < 40 && hi - lo > 1; it++)
                    {
                        const uint32_t mid = lo + (hi - lo) / 2;
                        Cpu o, u2;
                        natTo(mid, o);
                        if (o.fault != 2) { hi = mid; continue; }   // it ended before: no stop point there
                        const uint32_t ran = o.icount - 1;          // the instruction at o.miss was not run
                        uint32_t upc = 0;
                        ucTo(ran, u2, upc);
                        bool same = (upc & ~1u) == (o.miss & ~1u) && u2.n == o.n && u2.z == o.z && u2.c == o.c && u2.v == o.v;
                        int badReg = -1;
                        for (int k = 0; k < 15 && same; k++)
                            if (u2.r[k] != o.r[k]) { same = false; badReg = k; }
                        int badS = -1;
                        for (int k = 0; k < 32 && same; k++)
                            if (u2.s[k] != o.s[k]) { same = false; badS = k; }
                        uint32_t badMem = 0, badMemPc = 0;
                        int nv = -1, uv = -1;
                        if (same)
                        {
                            undo.Runs([&](uint32_t a, const uint8_t*, uint32_t len) {
                                for (uint32_t i = 0; i < len && same; i++)
                                {
                                    const uint32_t b = a + i;
                                    const uint8_t w = undo.Find(b >> 12)->w[b & 0xFFFu];
                                    Pg* g = diary.Find(b >> 12);
                                    if (!g || !PageLog::Has(g, b & 0xFFFu) || g->d[b & 0xFFFu] != w)
                                    {
                                        same = false; badMem = b; uv = w; nv = (g && PageLog::Has(g, b & 0xFFFu)) ? g->d[b & 0xFFFu] : -1;
                                        auto it2 = ucWritePc.find(b);
                                        badMemPc = it2 == ucWritePc.end() ? 0 : it2->second;
                                    }
                                }
                            });
                            if (same)
                                diary.Runs([&](uint32_t a, const uint8_t* dd, uint32_t len) {
                                    for (uint32_t i = 0; i < len && same; i++)
                                    {
                                        Pg* g = undo.Find((a + i) >> 12);
                                        if (!g || !PageLog::Has(g, (a + i) & 0xFFFu)) { same = false; badMem = a + i; nv = dd[i]; uv = -1; }
                                    }
                                });
                        }
                        if (same) { lo = ran; lastSamePc = o.miss; }
                        else
                        {
                            hi = ran;
                            firstPc = o.miss;
                            char b[200];
                            if (badReg >= 0) snprintf(b, sizeof(b), "r%d nativo %08X certo %08X", badReg, (unsigned)o.r[badReg], (unsigned)u2.r[badReg]);
                            else if (badS >= 0) snprintf(b, sizeof(b), "s%d nativo %08X certo %08X", badS, (unsigned)o.s[badS], (unsigned)u2.s[badS]);
                            else if (badMem) snprintf(b, sizeof(b), "memória %08X: nativo %d, certo %d (o Unicorn escreveu em %08X)", (unsigned)badMem, nv, uv,
                                                      (unsigned)badMemPc);
                            else snprintf(b, sizeof(b), "pc/flags: nativo %08X %u%u%u%u, certo %08X %u%u%u%u", (unsigned)o.miss, o.n, o.z, o.c, o.v,
                                          (unsigned)upc, u2.n, u2.z, u2.c, u2.v);
                            detail = b;
                        }
                    }
                    reported++;
                    fprintf(report, "   BISSEÇÃO: tudo igual até o bloco em %08X (%u instruções); diferente ao chegar em %08X: %s\n",
                            (unsigned)lastSamePc, (unsigned)lo, (unsigned)firstPc, detail.c_str());
                    // the memory is left as before the call and the native result goes on (diagnosis only)
                    restore();
                    natTo(4000000, s);
                    sandbox = false;
                    commit();
                    followNativeOnce = true;
                }
                if (why[0])
                {
                    nDiff++;
                    if (report && reported < 400)
                    {
                        reported++;
                        fprintf(report, "DIFERENÇA %s: %s | %u instruções | entrada:", FnName(swi, entry).c_str(), why, (unsigned)s.icount);
                        for (int k = 0; k < 15; k++) fprintf(report, " r%d=%08X", k, (unsigned)in.r[k]);
                        fprintf(report, " flags=%u%u%u%u fpscr=%08X\n", in.n, in.z, in.c, in.v, (unsigned)in.fpscr);
                        {
                            bool done = false;
                            undo.Runs([&](uint32_t a, const uint8_t*, uint32_t len) {
                                for (uint32_t i = 0; i < len && !done; i++)
                                {
                                    const uint32_t b = a + i;
                                    const uint8_t uv = undo.Find(b >> 12)->w[b & 0xFFFu];
                                    Pg* g = diary.Find(b >> 12);
                                    const bool has = g && PageLog::Has(g, b & 0xFFFu);
                                    if (!has || g->d[b & 0xFFFu] != uv)
                                    {
                                        auto w = ucWritePc.find(b);
                                        fprintf(report, "   1a escrita diferente: %08X nativo %s%02X, certo %02X (o Unicorn escreveu em %08X)\n", (unsigned)b,
                                                has ? "" : "(não escreveu) ", has ? g->d[b & 0xFFFu] : 0, uv,
                                                w == ucWritePc.end() ? 0u : (unsigned)w->second);
                                        done = true;
                                    }
                                }
                            });
                            if (!done) fprintf(report, "   escritas iguais (só registradores diferem)\n");
                        }
                        if (logReads)
                        {
                            // by address, not by order (ldrd / ldm read their words in another order):
                            // the first native read whose address Unicorn read with another value, or not at all
                            {
                                std::unordered_map<uint32_t, uint32_t> ucv;
                                for (auto& [ad, v] : ucReads) ucv.emplace(ad, v);
                                for (size_t q2 = 0; q2 < natReads.size(); q2++)
                                {
                                    auto it = ucv.find(natReads[q2].first);
                                    if (it == ucv.end())
                                    {
                                        fprintf(report, "   por endereço: leitura nº %zu do nativo em %08X (=%08X) o Unicorn não fez\n", q2,
                                                (unsigned)natReads[q2].first, (unsigned)natReads[q2].second);
                                        break;
                                    }
                                    if (it->second != natReads[q2].second)
                                    {
                                        auto w = ucWritePc.find(natReads[q2].first);
                                        fprintf(report, "   por endereço: leitura nº %zu em %08X: nativo %08X, Unicorn %08X (o Unicorn escreveu ali em %08X)\n", q2,
                                                (unsigned)natReads[q2].first, (unsigned)natReads[q2].second, (unsigned)it->second,
                                                w == ucWritePc.end() ? 0u : (unsigned)w->second);
                                        break;
                                    }
                                }
                            }
                            size_t q = 0;
                            while (q < natReads.size() && q < ucReads.size() && natReads[q] == ucReads[q]) q++;
                            fprintf(report, "   leituras: nativo %zu, Unicorn %zu; 1ª diferente nº %zu: nativo %08X=%08X, Unicorn %08X=%08X\n",
                                    natReads.size(), ucReads.size(), q,
                                    q < natReads.size() ? (unsigned)natReads[q].first : 0u, q < natReads.size() ? (unsigned)natReads[q].second : 0u,
                                    q < ucReads.size() ? (unsigned)ucReads[q].first : 0u, q < ucReads.size() ? (unsigned)ucReads[q].second : 0u);
                            fprintf(report, "   últimas do Unicorn:");
                            for (uint64_t r = hookN > 24 ? hookN - 24 : 0; r < hookN; r++) fprintf(report, " %08X", (unsigned)ring[r & 31]);
                            fprintf(report, "\n");
                        }
                    }
                    if (followNative || followNativeOnce)
                    {
                        if (!followNativeOnce) { restore(); commit(); }
                        followNativeOnce = false;
                    }
                    else
                    {
                    // the game goes on with the right result (memory already has Unicorn's writes)
                    for (int k = 0; k < 15; k++) s.r[k] = u.r[k];
                    for (int k = 0; k < 32; k++) s.s[k] = u.s[k];
                    s.n = u.n; s.z = u.z; s.c = u.c; s.v = u.v; s.q = u.q; s.ge = u.ge; s.fpscr = u.fpscr;
                    forcePc = upc;
                    usePc = true;
                    }
                    DisableFn(swi, entry, "diferença na conferência", true);
                }
            }
        }
    }
    if (Conf::on && Conf::dataHit)
    {
        // one of our svc was read as data (a constant, a table): that function must stay in the JIT
        const uint32_t at = Conf::dataHit;
        Conf::dataHit = 0;
        auto it = gSvcAt.find(at);
        if (it != gSvcAt.end())
        {
            Conf::nData++;
            if (Conf::report) fprintf(Conf::report, "SVC LIDO COMO DADO em %08X (lido por %s)\n", (unsigned)at, FnName(swi, entry).c_str());
            DisableFn(it->second, at, "svc lido como dado", true);
        }
    }

    for (int k = 0; k < 15; k++) cpu.SetReg(k, s.r[k]);
    for (int k = 0; k < 32; k++) cpu.SetVFPReg(k, s.s[k]);
    cpu.SetVFPSystemReg(VFP_FPSCR, s.fpscr);
    uint32_t nc = (cpsr & ~0xF80F0000u) | (s.n << 31) | (s.z << 30) | (s.c << 29) | (s.v << 28) | (s.q << 27) | ((s.ge & 0xF) << 16);
    uint32_t pc = usePc ? forcePc : (s.fault == 0) ? entryLr : s.miss;
    if (!usePc && s.fault && (pc & ~1u) == entry)
    {
        // handed back on its own first instruction: that is our svc, so it would call us again
        // forever. Switch this function off: the game's instruction goes back, the JIT runs it.
        uint32_t orig = 0;
        if (swi >= kSvcBase)
        {
            const int i = (int)(swi - kSvcBase);
            orig = gOrigMain[i];
            if (gOff.empty()) gOff.assign(gCount, 0);
            gOff[i] = 1;
        }
        else
        {
            const int k = (int)((swi - kSvcModBase) >> 16), i = (int)(swi & 0xFFFF);
            for (auto& L : gLoaded)
                if (L.k == k) { orig = L.original[i]; L.original[i] = 0; }
        }
        if (orig)
        {
            gMem->Write32(entry, orig);
            gSystem->InvalidateCacheRange(entry, 4);
        }
        fprintf(stderr, "recomp: função %08X desligada (não avança no nativo)\n", (unsigned)entry);
    }
    if (pc & 1) { nc |= 0x20; pc &= ~1u; }
    else nc &= ~0x20u;
    cpu.SetCPSR(nc);
    cpu.SetPC(pc);
    ticks = s.icount - gFlushed;   // (what system calls on the way did not give the clock yet)
    gCalls.fetch_add(1, std::memory_order_relaxed);
    gNativeInsns.fetch_add(s.icount, std::memory_order_relaxed);
    if (s.fault && s.fault < 8) gBack[s.fault].fetch_add(1, std::memory_order_relaxed);
    if (gMissLog && (s.fault == 4 || s.fault == 6 || s.fault == 3)) gMiss[s.miss | (s.fault == 6 ? 0x80000000u : 0)]++;
    // where the JIT goes on from here (to translate as an entry next time): the target without a
    // translation (fault 4), the instruction after the svc the JIT runs (fault 6), or the caller's
    // continuation it returns to (normal return / a return to somewhere else)
    uint32_t resume = 0;
    if (s.fault == 4) resume = s.miss;
    else if (s.fault == 6 && !(s.miss & 1)) resume = s.miss + 4;
    else if (s.fault == 0) resume = entryLr;
    if (gMissLog && resume && !(resume & 1))
    {
        const uint32_t a = resume;
        char key[96];
        snprintf(key, sizeof(key), "%08X", (unsigned)a);
        for (auto& L : gLoaded)
            if (a - L.base < L.size) { snprintf(key, sizeof(key), "%s+%X", gModName[L.k], (unsigned)(a - L.base)); break; }
        gFaltam.insert(key);
    }
    return true;
}

} // namespace Recomp
