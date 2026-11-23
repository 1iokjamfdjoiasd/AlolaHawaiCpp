// Interpolação dentro do jogo (alternativa 3). Plano: estudo/plano_interpolacao.md
//
// Etapa A (análise): PORTE_INTERP_GRAVAR=<s> grava os 3 quadros novos seguintes em <dados>/interp_quadro_<n>.bin
//   (programa, malha, textura, 96 uniforms por desenho; ferramenta: job tmp interp_ver.py).
// Etapa C (prova): PORTE_INTERP_TESTE=1 refaz cada quadro 3D igual logo antes do display transfer (imagem idêntica);
//   =2 refaz com a câmera afastada (a imagem muda: prova que refez).
// Etapa D: PORTE_INTERP=1 -> 60 FPS. Guarda o quadro atual (N) e o anterior (N-1) como listas de operações
//   (desenhos com registradores/uniforms/shader/tabelas/atributos fixos, limpezas, cópias, display transfer final).
//   No primeiro VBlank depois de N (jogo ocioso, esperando o VBlank), refaz N com as uniforms misturadas entre N-1 e N
//   (t = 0,5) e manda para o framebuffer que está na tela; depois do SwapBuffers refaz o quadro real para restaurar.
#include "interp_rec.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_external.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "core/frontend/framebuffer_layout.h"
#include "common/settings.h"
#include "common/hash.h"
#include "common/porte_counters.h"
#include "video_core/rasterizer_cache/pixel_format.h"
#include <SDL.h>
#include <algorithm>
namespace PorteLog { void ReplayHook(float t, double ms, int mixed, int jumped); }   // (main.cpp: advanced log)

std::string PorteDataDir();   // main.cpp
namespace Vulkan
{
void PorteExtraPresent(VideoCore::RendererBase* r);
int PorteFreeFrames();   // vk_present_window.cpp: frames of the main window free right now
// texture lookups noted by the first redraw of a sequence and reused by the next ones (vk_rasterizer.cpp, PORTE);
// only with PORTE_INTERP_MEMO_TESTE (off by default) - see estudo/estudo_desempenho_interpolacao.md
extern int g_porte_memo_mode;
// "gravar e repetir" (porte_repeat.h): record the first redraw of a sequence, repeat it with other uniforms
void PorteCaptureBegin(VideoCore::RasterizerInterface* r);
bool PorteCaptureEnd(VideoCore::RasterizerInterface* r);
bool PorteRepeat(VideoCore::RasterizerInterface* r, const Pica::Uniforms* const* per_draw, std::size_t count);
void PorteSetDrawIdx(int index);
extern u32 g_porte_memo_idx;
extern u64 g_porte_memo_hit, g_porte_memo_miss, g_porte_memo_stale;
}   // renderer_vulkan.cpp / vk_rasterizer.cpp (PORTE)
namespace VideoCore
{
extern RasterizerInterface* g_porte_rasterizer_override;   // gpu.cpp (ganchos PORTE)
extern void (*g_porte_before_swap)();
extern void (*g_porte_after_swap)();
}

namespace InterpRec
{

// ---------------------------------------------------------------- etapa A: gravação p/ análise
#pragma pack(push, 1)
struct DrawRec
{
    u32 index;       // ordem no quadro
    u32 accel;       // 1 = shader na GPU, 0 = CPU
    u64 prog;        // hash do programa do vertex shader
    u64 swz;         // hash do swizzle
    u32 mesh;        // endereço dos vértices + deslocamento (identidade da malha)
    u32 tex0;        // textura 0
    u32 nverts;
    u32 color;       // buffer de cor de destino
    float f[96][4];  // uniforms float (c0..c95)
};
#pragma pack(pop)

static std::vector<DrawRec> sDraws;
static u32 sIndex = 0;
static double sStartAt = -1;
static int sSaved = 0;
static auto sT0 = std::chrono::steady_clock::now();

static u32 MeshId(const Pica::RegsInternal& regs)
{
    const auto& pl = regs.pipeline;
    return pl.vertex_attributes.GetPhysicalBaseAddress() + (u32)pl.vertex_attributes.attribute_loaders[0].data_offset
           + (u32)pl.index_array.offset * 31u;
}

static void RecordForFile(bool accel)
{
    auto& pica = Core::System::GetInstance().GPU().PicaCore();
    const auto& regs = pica.regs.internal;
    DrawRec d{};
    d.index = sIndex++;
    d.accel = accel ? 1 : 0;
    d.prog = pica.vs_setup.GetProgramCodeHash();
    d.swz = pica.vs_setup.GetSwizzleDataHash();
    d.mesh = MeshId(regs);
    d.tex0 = regs.texturing.main_config.texture0_enable ? regs.texturing.texture0.GetPhysicalAddress() : 0;
    d.nverts = regs.pipeline.num_vertices;
    d.color = regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress();
    for (int i = 0; i < 96; i++)
        for (int k = 0; k < 4; k++) d.f[i][k] = pica.vs_setup.uniforms.f[i][k].ToFloat32();
    sDraws.push_back(d);
}

// ---------------------------------------------------------------- listas de operações (etapas C e D)
// tables that live outside the registers (lighting / fog / procedural texture): the game changes them
// between draws, so each draw keeps the version it was drawn with
struct TableSnap
{
    decltype(Pica::PicaCore::Lighting::luts) light;
    decltype(Pica::PicaCore::Fog::lut) fog;
    decltype(Pica::PicaCore::ProcTex::noise_table) pnoise, pcolor, palpha;
    decltype(Pica::PicaCore::ProcTex::color_table) pct;
    decltype(Pica::PicaCore::ProcTex::color_diff_table) pcd;
};
static bool SameTables(const TableSnap& t, const Pica::PicaCore& p)
{
    return memcmp(&t.light, &p.lighting.luts, sizeof(t.light)) == 0 && memcmp(&t.fog, &p.fog.lut, sizeof(t.fog)) == 0 &&
           memcmp(&t.pnoise, &p.proctex.noise_table, sizeof(t.pnoise)) == 0 &&
           memcmp(&t.pcolor, &p.proctex.color_map_table, sizeof(t.pcolor)) == 0 &&
           memcmp(&t.palpha, &p.proctex.alpha_map_table, sizeof(t.palpha)) == 0 &&
           memcmp(&t.pct, &p.proctex.color_table, sizeof(t.pct)) == 0 &&
           memcmp(&t.pcd, &p.proctex.color_diff_table, sizeof(t.pcd)) == 0;
}
static void TakeTables(TableSnap& t, const Pica::PicaCore& p)
{
    t.light = p.lighting.luts;
    t.fog = p.fog.lut;
    t.pnoise = p.proctex.noise_table;
    t.pcolor = p.proctex.color_map_table;
    t.palpha = p.proctex.alpha_map_table;
    t.pct = p.proctex.color_table;
    t.pcd = p.proctex.color_diff_table;
}
static void PutTables(const TableSnap& t, Pica::PicaCore& p)
{
    p.lighting.luts = t.light;
    p.fog.lut = t.fog;
    p.proctex.noise_table = t.pnoise;
    p.proctex.color_map_table = t.pcolor;
    p.proctex.alpha_map_table = t.palpha;
    p.proctex.color_table = t.pct;
    p.proctex.color_diff_table = t.pcd;
    p.lighting.lut_dirty = Pica::PicaCore::Lighting::LutAllDirty;
    p.fog.lut_dirty = true;
    p.proctex.table_dirty = Pica::PicaCore::ProcTex::TableAllDirty;
}

// (22/11) the vertex / index memory a draw reads: the redraws read it from the game's memory AT REDRAW TIME - when the
// game has already rewritten it (text that changed: the play time on the save screen; particles; anything built by the
// CPU every frame) the in-between picture drew the new geometry with the old state: garbled digits, lines across.
struct MemRange { u32 addr, len; };
static int DrawRanges(const Pica::RegsInternal& regs, bool indexed, MemRange out[13])
{
    auto& mem = Core::System::GetInstance().Memory();
    const auto& va = regs.pipeline.vertex_attributes;
    const PAddr base = va.GetPhysicalBaseAddress();
    const u32 n = regs.pipeline.num_vertices;
    if (!n) return 0;
    int k = 0;
    u32 vmin, vmax;
    if (indexed)
    {
        const PAddr ia = base + regs.pipeline.index_array.offset;
        const bool wide = regs.pipeline.index_array.format != 0;
        const u8* p = mem.GetPhysicalPointer(ia);
        if (!p) return 0;
        out[k++] = { ia, n * (wide ? 2u : 1u) };
        vmin = 0xFFFF; vmax = 0;
        if (wide) { const u16* q = (const u16*)p; for (u32 i = 0; i < n; i++) { vmin = std::min<u32>(vmin, q[i]); vmax = std::max<u32>(vmax, q[i]); } }
        else for (u32 i = 0; i < n; i++) { vmin = std::min<u32>(vmin, p[i]); vmax = std::max<u32>(vmax, p[i]); }
        if (vmax < vmin) return 0;
    }
    else { vmin = regs.pipeline.vertex_offset; vmax = vmin + n - 1; }
    for (int i = 0; i < 12; i++)
    {
        const auto& ld = va.attribute_loaders[i];
        if (ld.component_count == 0 || ld.byte_count == 0) continue;
        out[k++] = { base + (u32)ld.data_offset + vmin * (u32)ld.byte_count, (vmax - vmin + 1) * (u32)ld.byte_count };
    }
    return k;
}
static u64 HashRanges(const MemRange* r, int n)
{
    auto& mem = Core::System::GetInstance().Memory();
    u64 h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < n; i++)
    {
        const u8* p = mem.GetPhysicalPointer(r[i].addr);
        if (!p || !mem.GetPhysicalPointer(r[i].addr + r[i].len - 1)) { h = h * 31 + 1; continue; }
        h = (h ^ Common::ComputeHash64(p, r[i].len)) * 0x100000001B3ull;
    }
    return h;
}
// the textures it samples, read from the game's memory (a glyph cache the CPU rewrites, for one)
static int TexRanges(const Pica::RegsInternal& regs, MemRange out[3])
{
    int k = 0;
    for (const auto& t : regs.texturing.GetTextures())
    {
        if (!t.enabled) continue;
        const u32 bpp = VideoCore::GetFormatBpp(VideoCore::PixelFormatFromTextureFormat(t.format));
        const u32 len = (u32)t.config.width * (u32)t.config.height * bpp / 8;
        if (len) out[k++] = { t.config.GetPhysicalAddress(), len };
    }
    return k;
}
static u64 sMemChangedPairs = 0, sMemChangedDraws = 0, sTexChangedDraws = 0;
// (22/11) measured: it never fired (the in-between pictures are made right when N is complete, before the game writes
// again) - off by default (diagnosis: PORTE_INTERP_MEMCHECK=1)
static const bool sMemCheck = getenv("PORTE_INTERP_MEMCHECK") && atoi(getenv("PORTE_INTERP_MEMCHECK")) != 0;
static u64 sHudKept = 0;   // 2D draws not mixed (something other than the position changed)

struct OpDraw
{
    MemRange mr[13]; int nmr = 0; u64 memHash = 0;
    MemRange tr[3]; int ntr = 0; u64 texHash = 0;   // the textures   // (22/11) what it reads from the game's memory, and its hash
    Pica::RegsInternal regs;
    Pica::Uniforms uni;
    u64 prog = 0, swz = 0;
    u32 entry = 0;
    bool indexed = false;
    size_t tables = 0;                  // index in FrameList::snaps
    Pica::AttributeBuffer fixedAttr;    // fixed vertex attributes (bone of rigid meshes, constant colour)
    u32 mesh = 0;                       // identity for pairing with the previous picture
    bool hud = false;
    bool lyt = false;                   // (22/11) a menu / layout piece (its quad mesh is shared by many small draws)
    float uf[96][4];                    // the uniforms as floats (converted once, when recorded)
    float sig[3];                       // where it is placed (sums of the matrix rows): to tell identical objects apart
};
enum OpType { OP_DRAW, OP_FILL, OP_TEXCOPY, OP_DT, OP_SWDRAW };
// (22/11) a draw the Azahar did on the CPU (geometry shader: the particles of a Z move's arrow volley): its finished
// triangles and its registers are kept and sent again as they are in the redraws - before, the whole picture was
// "unsafe" and those parts of the battle stayed at 30 FPS. The triangles keep the current picture's positions
struct SwDraw
{
    Pica::RegsInternal regs;
    size_t tables = 0;
    std::vector<Pica::OutputVertex> verts;
};
struct Op
{
    OpType type;
    size_t draw = 0;   // index in FrameList::draws
    Pica::MemoryFillConfig fill{};
    Pica::DisplayTransferConfig copy{};
};

// one picture's worth of operations (pools are kept between pictures: no allocation after warm-up)
struct FrameList
{
    std::vector<Op> ops;
    std::vector<std::unique_ptr<OpDraw>> draws;
    size_t drawsUsed = 0;
    std::vector<std::unique_ptr<TableSnap>> snaps;
    size_t snapsUsed = 0;
    std::vector<std::unique_ptr<SwDraw>> sws;   // (22/11) draws done on the CPU (OP_SWDRAW)
    size_t swsUsed = 0;
    Pica::DisplayTransferConfig dt{};   // the display transfer that ended it (render target -> framebuffer)
    bool valid = false;
    bool unsafe = false;   // something in it cannot be redrawn faithfully (a draw went to the CPU, geometry shader)
    u64 gen = 0;           // which picture this is (changes every time the list is reused)
    // pairing with the previous picture, worked out once per pair of pictures (not once per in-between picture)
    const struct FrameList* pairedWith = nullptr;
    u64 pairedWithGen = 0, pairedGen = 0;
    std::vector<const OpDraw*> pairP;          // per draw: its previous version (null = none)
    std::vector<u8> pairState;                 // 0 = not mixed (none / HUD), 1 = mixed, 2 = jumped (shown as it is)
    std::vector<std::vector<u16>> pairDiff;    // per draw: the uniform components (r * 4 + k) that change
    std::vector<std::vector<struct RotGroup>> pairRot;   // per draw: the rotation matrices that change (mixed by slerp)
    std::vector<std::vector<u16>> pairIdx;     // (22/11) per draw: whole-number values that change (indices): always the current value
    void Clear() { ops.clear(); drawsUsed = 0; snapsUsed = 0; swsUsed = 0; valid = false; unsafe = false; }
};
static u64 sGenCounter = 0;
static FrameList sLists[3];
static FrameList* sBuild = &sLists[0];   // being recorded
static FrameList* sCur = &sLists[1];     // N (last whole 3D picture)
static FrameList* sPrev = &sLists[2];    // N-1
static std::vector<u8> sAvMask;          // (22/11, AutoCheck) per draw of N in this pair: 1 = changes at once in the middle, 2 = new draw hidden before the middle
static bool sAvAnySkip = false;
static unsigned long long sReplaySkips = 0;   // (22/11) draws our redraws skipped (shader compiling); the rest were the game's

struct ProgCopy { Pica::ProgramCode code; u32 size; };
struct SwzCopy { Pica::SwizzleData data; u32 size; };
static std::map<u64, std::unique_ptr<ProgCopy>> sProgs;
static std::map<u64, std::unique_ptr<SwzCopy>> sSwzs;

static int sTest = 0;     // PORTE_INTERP_TESTE
static int sInterp = 0;   // PORTE_INTERP
static bool sReplaying = false;
static u64 sReplays = 0, sReplayedDraws = 0, sSkippedSw = 0, sRefused = 0, sSkippedSeg = 0, sMixed = 0, sUnpaired = 0;
static u64 sExtraShown = 0, sNotIdle = 0, sNoAddr = 0;
static double sReplayMs = 0;
static bool sNewPicture = false;            // a whole 3D picture arrived since the last VBlank
static FrameList* sRestore = nullptr;       // after the swap: redraw this one exactly
static bool sExtraFlag = false;             // the swap that just happened showed an in-between picture
static bool sExtraShownThis = false;
static void BeforeSwapMix();
// above 60 (PORTE_INTERP_FPS): pictures with a time to be shown (same clock as the presenter)
static double NowS() { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }
static int sTargetFps = 60;
static u64 sSkippedFree = 0;   // in-between pictures skipped: the presenter had no free frame (the game never waits)
static double sPeriod = 1.0 / 30, sLastPicTime = 0;   // real time between game pictures (for the times)
static double sPeriodEmu = 1.0 / 30;                   // emulated time between them (VBlanks / 60: for K)
static u64 sVbCount = 0, sLastPicVb = 0;
static double sShowAtPending = 0, sTimedUntil = 0;
static int sLastK = 0;
static double sSeqEnd = 0;          // where the current sequence of timed pictures ends
static bool sAutoBudget = true;      // the menu's "Ajuste automático do FPS"
static double sBudget = 0;           // pictures per second actually generated (<= target; lowered if the game falls behind)
static u64 sStill = 0;               // game pictures where nothing moved (nothing generated)
static double sInnerMs = 0;
static double sPresentMs = 0;
static bool sCapOk = false;                     // the 1st picture of this sequence was recorded and can be repeated
static u64 sRepeats = 0, sRepeatStopped = 0, sCaptures = 0, sCaptureBad = 0;
static double sRepeatMs = 0;
static std::vector<Pica::Uniforms> sRepUni;
static std::vector<const Pica::Uniforms*> sRepPtr;         // time spent handing the extra pictures to the presenter
static u64 sPresents = 0;          // time spent inside the Azahar rasterizer while redrawing
static bool sProf = false;           // PORTE_INTERP_PERFIL: time of each step of the Azahar draw
static bool sNoMemo = false;         // PORTE_INTERP_SEM_MEMO: every redraw looks the textures up again (test)

static bool IsHud(const Pica::Uniforms& u)
{
    // HUD 2D: projeção ortográfica (c89 = 0 0 0 1); a 3D tem c89 = 0 0 -1 0
    return u.f[89][3].ToFloat32() > 0.5f;
}

static void RecordDraw(bool indexed)
{
    auto& pica = Core::System::GetInstance().GPU().PicaCore();
    auto& vs = pica.vs_setup;
    FrameList& L = *sBuild;
    if (L.drawsUsed == L.draws.size()) L.draws.push_back(std::make_unique<OpDraw>());
    OpDraw& d = *L.draws[L.drawsUsed];
    d.regs = pica.regs.internal;
    d.uni = vs.uniforms;
    d.prog = vs.GetProgramCodeHash();
    d.swz = vs.GetSwizzleDataHash();
    d.entry = vs.entry_point;
    d.indexed = indexed;
    d.fixedAttr = pica.input_default_attributes;
    d.mesh = MeshId(d.regs);
    d.nmr = d.ntr = 0;
    if (sMemCheck)
    {
        d.nmr = DrawRanges(d.regs, indexed, d.mr);
        d.memHash = HashRanges(d.mr, d.nmr);
        d.ntr = TexRanges(d.regs, d.tr);
        d.texHash = HashRanges(d.tr, d.ntr);
    }
    d.hud = IsHud(d.uni);
    for (int r = 0; r < 96; r++)
        for (int k = 0; k < 4; k++) d.uf[r][k] = d.uni.f[r][k].ToFloat32();
    {
        const bool gfl2 = d.uf[89][2] == -1.0f && d.uf[89][3] == 0.0f;
        // gfl2: where the placing matrix (c10-c12) puts it - the other registers may hold values left over from
        // earlier draws (other objects), which made identical objects pair with the wrong one
        if (gfl2) { d.sig[0] = d.uf[10][3]; d.sig[1] = d.uf[11][3]; d.sig[2] = d.uf[12][3]; }
        else
        {
            d.sig[0] = d.sig[1] = d.sig[2] = 0;
            for (int r = 0; r < 96; r++) { d.sig[0] += d.uf[r][3]; d.sig[1] += d.uf[r][0]; d.sig[2] += d.uf[r][1]; }
        }
    }
    if (L.snapsUsed == 0 || !SameTables(*L.snaps[L.snapsUsed - 1], pica))
    {
        if (L.snapsUsed == L.snaps.size()) L.snaps.push_back(std::make_unique<TableSnap>());
        TakeTables(*L.snaps[L.snapsUsed++], pica);
    }
    d.tables = L.snapsUsed - 1;
    if (!sProgs.count(d.prog))
        sProgs[d.prog] = std::make_unique<ProgCopy>(ProgCopy{ vs.GetProgramCode(), vs.GetBiggestProgramSize() });
    if (!sSwzs.count(d.swz))
        sSwzs[d.swz] = std::make_unique<SwzCopy>(SwzCopy{ vs.GetSwizzleData(), vs.GetBiggestSwizzleSize() });
    Op op{ OP_DRAW };
    op.draw = L.drawsUsed++;
    L.ops.push_back(op);
}

// a list worth keeping: starts by clearing its target and has 3D (the bottom screen / 2D are drawn over
// what is already there and are not interpolated)
static bool WholePicture(const FrameList& L)
{
    bool cleared = false, has3d = false;
    for (const Op& op : L.ops) { if (op.type == OP_FILL) { cleared = true; break; } if (op.type == OP_DRAW) break; }
    for (const Op& op : L.ops)
        if (op.type == OP_DRAW && !L.draws[op.draw]->hud) { has3d = true; break; }
    return cleared && has3d;
}

// ---------------------------------------------------------------- mixing rules (etapa E)
// identity of a draw: same shader, same mesh, same vertex count, same texture
static u64 DrawKey(const OpDraw& d)
{
    const auto& tx = d.regs.texturing;
    const u64 tex0 = tx.main_config.texture0_enable ? tx.texture0.GetPhysicalAddress() : 0;
    // (20/11 noite) + where it is drawn (color buffer, depth buffer): a model drawn twice in one picture (the screen
    // and an effect texture / shadow, seen from another place - the bones come already in view space) was paired
    // across the two passes, and the in-between pictures showed the character from the other viewpoint (the bag
    // close-up of the opening)
    const auto& fb = d.regs.framebuffer.framebuffer;
    const u64 target = ((u64)fb.GetColorBufferPhysicalAddress() << 32) ^ (u64)fb.GetDepthBufferPhysicalAddress();
    return (d.prog * 0x9E3779B97F4A7C15ull) ^ ((u64)d.mesh << 21) ^ ((u64)d.regs.pipeline.num_vertices << 3) ^ (tex0 * 0x100000001B3ull) ^
           (target * 0xC2B2AE3D27D4EB4Full);
}

static constexpr float kRotJump = 0.6f;    // (rows that are not a rotation matrix) ~35 degrees in one game frame = a cut
static u64 sCutWild = 0;   // draws shown as they are: a bone turned more than 45 degrees with the camera still
static bool sCamStill = false;   // the camera (c90-c92) turned less than ~2.5 degrees between W and L
static constexpr float kMoveJump = 40.0f;  // translation in one game frame (view space): a teleport or a camera cut
static constexpr float kRotCut = 0.5f;     // rotation matrices: quaternion dot < cos(60) = turned more than 120 degrees = a cut
// a whole draw turning more than 90 degrees in one game frame (the character turning around): near 180 degrees each
// bone takes its own shortest way - some turn one way, some the other, and halfway the arms open ("T pose").
// When most of its turning matrices turn that much, the draw is shown as it is now (quaternion dot < cos(45))
static constexpr float kTurnAround = 0.7071f;
static u64 sTurnArounds = 0;
static u64 sIndexStepped = 0;
static u64 sParticles = 0;   // (22/11) particle draws not mixed
static u64 sSwReplayed = 0;   // (22/11) CPU draws sent again in the redraws
static u64 sOtherProg = 0;   // (22/11) draws paired with another shader program: current values only   // (22/11) whole-number values that change (indices): not mixed
static u64 sFlipMixed = 0;   // (22/11) bones that flipped > 120 degrees alone and were mixed (the bag)
static u64 sTexScroll = 0;   // texture rows mixed as a scroll
static u64 sTexSnaps = 0, sJumps = 0, sUnsafe = 0, sRotMixed = 0;
static u64 sCutRot = 0, sCutMove = 0, sCutScale = 0, sCutRow = 0;   // why draws were shown as they are (diagnostics)

// the gfl2 model shaders keep the projection in c86-c89 (c89 = 0 0 -1 0) and the texture matrices in c1-c9
// (22/11) some menu / layout draws carry the same c89 mark as the 3D models (the save screen's 3D-looking card): they
// were mixed as models - their tables of letter positions with the 3D rules (jumps of 24-30 units unseen) - garbled
// numbers. A layout piece is told apart by its mesh: the same small quad mesh drawn by many draws in one picture.
static bool Gfl2Layout(const OpDraw& d) { return !d.lyt && d.uf[89][2] == -1.0f && d.uf[89][3] == 0.0f; }
static void MarkLayout(struct FrameList& L);

// ---- rotation matrices (gfl2: model / bone matrices in c10-c84 as groups of 3 rows, the camera in c90-c92):
// mixing their numbers one by one shrinks and bends what turns (a limb turning 60 degrees is 13% shorter halfway)
// - they are mixed as rotations instead (quaternion slerp), with the scale and the translation mixed linearly
struct RotGroup
{
    u8 r;              // first register (rows r, r+1, r+2)
    float q0[4], q1[4];
    float s0, s1;
    float t0[3], t1[3];
    bool step = false;   // (22/11) this bone changes at once in the middle (a violent move of this bone only)
    bool rotCut = false;   // (22/11) ...because it turned more than 120 degrees (may be a local flip: see RotGroups)
    bool moveCut = false;  // ...because it moved too far
    bool radiusOnly = false;   // ...only the screw radius guard said "teleport" (the slide itself was small)
    float moveRatio = 0;       // raw move / limit (to tell a whole-model teleport from one swinging part)
};
// a rotation times a uniform scale? -> its quaternion and scale
static bool ToRot(const float (*m)[4], float q[4], float& s)
{
    float n[3];
    for (int i = 0; i < 3; i++)
    {
        n[i] = std::sqrt(m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2]);
        if (!std::isfinite(n[i]) || !std::isfinite(m[i][3])) return false;
    }
    s = (n[0] + n[1] + n[2]) / 3.0f;
    if (!(s > 1e-6f)) return false;
    for (int i = 0; i < 3; i++)
        if (std::fabs(n[i] - s) > 0.02f * s) return false;
    const float ss = s * s;
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (std::fabs(m[i][0] * m[j][0] + m[i][1] * m[j][1] + m[i][2] * m[j][2]) > 0.02f * ss) return false;
    float R[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) R[i][j] = m[i][j] / s;
    const float det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
                      R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
    if (det < 0.9f) return false;   // a mirror (or not a rotation)
    float x, y, z, w;
    const float tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0)
    {
        const float S = std::sqrt(tr + 1.0f) * 2.0f;
        w = 0.25f * S; x = (R[2][1] - R[1][2]) / S; y = (R[0][2] - R[2][0]) / S; z = (R[1][0] - R[0][1]) / S;
    }
    else if (R[0][0] > R[1][1] && R[0][0] > R[2][2])
    {
        const float S = std::sqrt(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2.0f;
        w = (R[2][1] - R[1][2]) / S; x = 0.25f * S; y = (R[0][1] + R[1][0]) / S; z = (R[0][2] + R[2][0]) / S;
    }
    else if (R[1][1] > R[2][2])
    {
        const float S = std::sqrt(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2.0f;
        w = (R[0][2] - R[2][0]) / S; x = (R[0][1] + R[1][0]) / S; y = 0.25f * S; z = (R[1][2] + R[2][1]) / S;
    }
    else
    {
        const float S = std::sqrt(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2.0f;
        w = (R[1][0] - R[0][1]) / S; x = (R[0][2] + R[2][0]) / S; y = (R[1][2] + R[2][1]) / S; z = 0.25f * S;
    }
    const float qn = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(qn > 0.5f)) return false;
    q[0] = x / qn; q[1] = y / qn; q[2] = z / qn; q[3] = w / qn;
    return true;
}
// the mixed matrix at t -> three rows
static void MixRot(const RotGroup& g, float t, float out[3][4])
{
    const float d = g.q0[0] * g.q1[0] + g.q0[1] * g.q1[1] + g.q0[2] * g.q1[2] + g.q0[3] * g.q1[3];   // >= 0 (q1 flipped)
    float a = 1.0f - t, b = t;
    if (d < 0.9995f)
    {
        const float th = std::acos(std::min(d, 1.0f)), sn = std::sin(th);
        a = std::sin((1.0f - t) * th) / sn;
        b = std::sin(t * th) / sn;
    }
    float x = a * g.q0[0] + b * g.q1[0], y = a * g.q0[1] + b * g.q1[1], z = a * g.q0[2] + b * g.q1[2], w = a * g.q0[3] + b * g.q1[3];
    const float qn = std::sqrt(x * x + y * y + z * z + w * w);
    x /= qn; y /= qn; z /= qn; w /= qn;
    const float s = g.s0 + (g.s1 - g.s0) * t;
    const float R[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                            { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                            { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++) out[i][j] = s * R[i][j];
        out[i][3] = g.t0[i] + (g.t1[i] - g.t0[i]) * t;
    }
    // (22/11) SCREW MOTION for the translation: a skinned bone's matrix carries the model's bind pose, so it turns about the
    // MODEL's origin (Lillie's feet), not about the joint. Turning about that origin while the translation goes in a straight
    // line sent the points of a bone that turns a lot (the bag: 60-90 degrees a game frame) along a huge arc - the bag flew
    // over her shoulder, big in front of the camera, in pictures where neither real one shows it. A rigid move from pose 0
    // to pose 1 is a turn about one axis plus a slide along it (Chasles): the points now follow THAT (about the strap for the
    // bag, about the orbit's centre for a camera baked into the bones). t = 0 and t = 1 are exactly as before; small turns
    // (walking) barely change. PORTE_INTERP_SEMPARAFUSO=1: the old straight line
    static const bool noScrew = getenv("PORTE_INTERP_SEMPARAFUSO") != nullptr;
    if (noScrew || d >= 0.99991f) return;   // (< ~1.5 degrees: the straight line is the same)
    // relative turn qd = q1 * conj(q0): axis u, angle th
    const float* p = g.q0; const float* q = g.q1;
    const float cx = -p[0], cy = -p[1], cz = -p[2], cw = p[3];
    const float dx = q[3] * cx + q[0] * cw + q[1] * cz - q[2] * cy;
    const float dy = q[3] * cy - q[0] * cz + q[1] * cw + q[2] * cx;
    const float dz = q[3] * cz + q[0] * cy - q[1] * cx + q[2] * cw;
    const float dw = q[3] * cw - q[0] * cx - q[1] * cy - q[2] * cz;
    const float vn = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(vn > 1e-6f)) return;
    const float u[3] = { dx / vn, dy / vn, dz / vn };
    const float th = 2.0f * std::atan2(vn, dw);
    auto rotv = [&](float ang, const float v[3], float o[3]) {   // Rodrigues: v turned by ang about u
        const float c = std::cos(ang), sn = std::sin(ang), ud = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
        const float cr[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        for (int i = 0; i < 3; i++) o[i] = v[i] * c + cr[i] * sn + u[i] * ud * (1 - c);
    };
    float rt0[3];
    rotv(th, g.t0, rt0);
    float td[3], tp[3];
    for (int i = 0; i < 3; i++) td[i] = g.t1[i] - rt0[i];
    const float along = u[0] * td[0] + u[1] * td[1] + u[2] * td[2];
    for (int i = 0; i < 3; i++) tp[i] = td[i] - along * u[i];
    const float ct = std::cos(th * 0.5f) / std::sin(th * 0.5f);
    const float uxt[3] = { u[1] * tp[2] - u[2] * tp[1], u[2] * tp[0] - u[0] * tp[2], u[0] * tp[1] - u[1] * tp[0] };
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = 0.5f * (tp[i] + ct * uxt[i]);   // a point on the axis: (I - Rd) c = tp
    // T(t) = Rd(t) (t0 - c) + c + t * along * u
    float rel[3], rr[3];
    for (int i = 0; i < 3; i++) rel[i] = g.t0[i] - c[i];
    rotv(th * t, rel, rr);
    float T[3];
    for (int i = 0; i < 3; i++) T[i] = rr[i] + c[i] + t * along * u[i];
    if (!std::isfinite(T[0]) || !std::isfinite(T[1]) || !std::isfinite(T[2])) return;
    for (int i = 0; i < 3; i++) out[i][3] = T[i];
}
// (22/11) how far a bone moves that its turn does NOT explain: the slide along the screw axis (see MixRot). A skinned bone
// turning 60-90 degrees about the model's origin changes its translation by a lot without going anywhere (the bag) -
// judged by the raw translation it was a "teleport" and stepped in the middle: the bag at 30 FPS. -1 = no real turn
static float ScrewSlide(const RotGroup& g, float far, float* noGuard = nullptr)
{
    const float* p = g.q0; const float* q = g.q1;
    const float cx = -p[0], cy = -p[1], cz = -p[2], cw = p[3];
    const float dx = q[3] * cx + q[0] * cw + q[1] * cz - q[2] * cy;
    const float dy = q[3] * cy - q[0] * cz + q[1] * cw + q[2] * cx;
    const float dz = q[3] * cz + q[0] * cy - q[1] * cx + q[2] * cw;
    const float dw = q[3] * cw - q[0] * cx - q[1] * cy - q[2] * cz;
    const float vn = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float th = 2.0f * std::atan2(vn, std::fabs(dw));
    if (!(vn > 1e-6f) || th < 0.026f) return -1.0f;   // (< ~1.5 degrees: the raw translation is the measure; a bone far from the
    // model's origin (lever ~15000 units: Lillie's bag bone c13) moves 1100-3400 units turning only 5-12 degrees)
    const float u[3] = { dx / vn, dy / vn, dz / vn };
    // along = u . (t1 - Rd t0) = u . t1 - u . t0 (a turn about u keeps the part along u)
    const float along = u[0] * (g.t1[0] - g.t0[0]) + u[1] * (g.t1[1] - g.t0[1]) + u[2] * (g.t1[2] - g.t0[2]);
    // a teleport that also turns: the axis that would explain it is absurdly far (radius > 1.5x the distance to the camera)
    float ch = 0;
    for (int i = 0; i < 3; i++) { const float v = g.t1[i] - g.t0[i] - along * u[i]; ch += v * v; }   // (|tp|: the turn keeps |t0|, close enough for a guard)
    const float radius = std::sqrt(ch) / (2.0f * std::sin(th * 0.5f));
    if (noGuard) *noGuard = std::fabs(along);
    // (continuous turns up to 25 degrees a frame - a camera orbiting a close-up, baked into the bones - can have their axis
    // several times farther than the camera distance; bigger turns keep the tight guard)
    if (radius > (th < 0.44f ? 6.0f : 1.5f) * far + kMoveJump) return 1e9f;
    return std::fabs(along);
}
// the rotation groups of a gfl2 draw that change between the two versions; false = a cut (shown as it is)
static bool RotGroups(const OpDraw& p, const OpDraw& d, std::vector<RotGroup>& out, u8 isRot[96], bool* violent = nullptr)
{
    out.clear();
    memset(isRot, 0, 96);
    if (!Gfl2Layout(d)) return true;
    static const bool linear = getenv("PORTE_INTERP_LINEAR") != nullptr;   // (test) the old way: number by number
    if (linear) return true;
    int turned = 0, big = 0;   // matrices that turn, and those that turn more than 90 degrees
    auto one = [&](int r) -> bool {
        if (memcmp(p.uf[r], d.uf[r], sizeof(float) * 12) == 0) return true;   // unchanged
        RotGroup g{};
        if (!ToRot(&p.uf[r], g.q0, g.s0) || !ToRot(&d.uf[r], g.q1, g.s1)) return true;   // not a rotation: number by number
        float dot = g.q0[0] * g.q1[0] + g.q0[1] * g.q1[1] + g.q0[2] * g.q1[2] + g.q0[3] * g.q1[3];
        if (dot < 0) { for (float& c : g.q1) c = -c; dot = -dot; }
        auto snap = [&]() { isRot[r] = isRot[r + 1] = isRot[r + 2] = 1; return true; };   // keeps the current value
        turned++;
        if (dot < kTurnAround) big++;
        // (21/11) a violent move of one bone changes the pose of the WHOLE draw at once (state 3: its bones step from the
        // old pose to the new one at the middle of the interval) - freezing only that bone at the new value mixed the
        // others around it: the opening's swinging bag came out bigger and out of place, neither old nor new
        bool v = false;
        // (22/11 night) Lillie's bag flips ~180 degrees in 1-2 game frames at every running step (the game itself does it:
        // seen with the interpolation off). Mixed, it took a wrong way; snapped, it lagged; the whole model stepping made
        // all of Lillie lag. Now a violent bone of a big draw changes at once IN THE MIDDLE, alone (bone step); the other
        // bones keep mixing. Small draws (the beach shadows) still step whole (violent pointer given).
        // (diagnosis) PORTE_INTERP_DIAGANG=<from s>,<to s>: every bone that turns more than 30 degrees in one game frame
        {
            static double gA = -1, gB = -1;
            if (gA < 0) { gA = 0; if (const char* e = getenv("PORTE_INTERP_DIAGANG")) sscanf(e, "%lf,%lf", &gA, &gB); }
            if (gB > 0 && dot < 0.966f)
            {
                const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count();
                if (el >= gA && el <= gB)
                {
                    float dt = 0;
                    for (int i = 0; i < 3; i++) dt += std::fabs(d.uf[r + i][3] - p.uf[r + i][3]);
                    fprintf(stderr, "ANG t=%.3f mesh %08X nv %u c%d gira %.0f graus mov %.1f escala %.3f>%.3f cam_parada %d\n", el, d.mesh,
                            (u32)d.regs.pipeline.num_vertices, r, 2.0 * std::acos(std::min(dot, 1.0f)) * 57.2958, dt, g.s0, g.s1, (int)sCamStill);
                }
            }
        }
        bool boneStep = false;
        if (dot < kRotCut) { sCutRot++; if (violent) v = true; else { boneStep = true; g.rotCut = true; } }   // turned more than 120 degrees in one game frame
        // (20/11 noite, safety net) with the camera still, a bone turning more than 45 degrees in one game frame is a
        // violent move (the opening's swinging bag: 37-71 degrees a frame) - the in-between poses invented for it
        // were wrong; the draw is shown as the game drew it (30 per second, like the original) instead
        // (22/11 night, measured on the running close) the camera is baked into the bone matrices in cutscenes: the camera
        // registers stay still while it orbits Lillie, so "camera still" was wrong and the whole body (34-62 degrees a frame,
        // every bone together) was treated as violent -> Lillie at 30 FPS on fast turns, the bag half stepping / half mixing.
        // No bone turned more than 92 degrees in the whole close (only camera cuts: 170). Rule off; PORTE_INTERP_REGRA60=1 = old
        static const bool rule60 = getenv("PORTE_INTERP_REGRA60") != nullptr;
        if (rule60 && !v && !boneStep && sCamStill && dot < 0.866f) { sCutWild++; if (violent) v = true; else boneStep = true; }   // cos(30): the half-angle of 60 degrees
        for (int i = 0; i < 3; i++)
        {
            g.t0[i] = p.uf[r + i][3];
            g.t1[i] = d.uf[r + i][3];
        }
        // (how far it is from the camera: when the camera turns, far things move a lot)
        const float far = std::max(std::sqrt(g.t0[0] * g.t0[0] + g.t0[1] * g.t0[1] + g.t0[2] * g.t0[2]),
                                   std::sqrt(g.t1[0] * g.t1[0] + g.t1[1] * g.t1[1] + g.t1[2] * g.t1[2]));
        // (20/11 noite) 50% of the distance is room for the camera TURNING (far things then move a lot in view space);
        // with the camera still (less than ~2.5 degrees this frame) nothing real moves more than 10% of its
        // distance in one game frame - the opening's bag parts jumped 3394 units (camera: 13) and swept the screen
        const float farRoom = sCamStill ? 0.05f : 0.5f;
        { float mvr = 0; for (int i = 0; i < 3; i++) mvr = std::max(mvr, std::fabs(g.t1[i] - g.t0[i])); g.moveRatio = mvr / (kMoveJump + farRoom * far); }
        // (22/11) a bone that turns more than ~1.5 degrees: only the move its turn does not explain counts (ScrewSlide)
        static const bool noScrewJ = getenv("PORTE_INTERP_SEMPARAFUSO") != nullptr;
        float slideNoGuard = -1.0f;
        const float slide = (noScrewJ || violent) ? -1.0f : ScrewSlide(g, far, &slideNoGuard);   // (small draws that may step whole - the beach shadows - keep the raw test)
        if (slide >= 0 && slide <= kMoveJump + farRoom * far) {}   // (explained by the turn: no teleport)
        else
        for (int i = 0; i < 3 && !v; i++)
        {
            if (slide >= 0 || std::fabs(g.t1[i] - g.t0[i]) > kMoveJump + farRoom * far)
            {
                static int left = getenv("PORTE_INTERP_DIAG") ? 60 : 0;
                if (left > 0)
                {
                    left--;
                    fprintf(stderr, "interp: grupo c%d malha %08X vértices %u: (%.1f %.1f %.1f) -> (%.1f %.1f %.1f) fica no valor novo\n", r, d.mesh, (u32)d.regs.pipeline.num_vertices,
                            p.uf[r][3], p.uf[r + 1][3], p.uf[r + 2][3], d.uf[r][3], d.uf[r + 1][3], d.uf[r + 2][3]);
                }
                sCutMove++;
                if (violent) { v = true; break; }
                boneStep = true;
                g.moveCut = true;
                if (slide >= 1e8f && slideNoGuard >= 0 && slideNoGuard <= kMoveJump + farRoom * far) g.radiusOnly = true;
                break;
            }
        }
        if (v) *violent = true;
        if (std::fabs(g.s1 - g.s0) > 0.5f * std::max(g.s0, g.s1)) { sCutScale++; return snap(); }   // scale popped (appear / vanish)
        // (diagnosis) PORTE_INTERP_DIAGPASSO=<from s>,<to s>: why a bone steps (angle, screw slide, raw move, distance)
        if (boneStep)
        {
            static double pA = -1, pB = -1;
            if (pA < 0) { pA = 0; if (const char* e = getenv("PORTE_INTERP_DIAGPASSO")) sscanf(e, "%lf,%lf", &pA, &pB); }
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count();
            if (pB > 0 && el >= pA && el <= pB)
            {
                float mv = 0;
                for (int i = 0; i < 3; i++) mv = std::max(mv, std::fabs(g.t1[i] - g.t0[i]));
                fprintf(stderr, "PASSO t=%.3f mesh %08X nv %u c%d gira %.0f desliza %.1f mov %.1f limite %.1f dist %.1f cam_parada %d escala %.3f>%.3f\n", el, d.mesh,
                        (u32)d.regs.pipeline.num_vertices, r, 2.0 * std::acos(std::min(dot, 1.0f)) * 57.2958, slide, mv, kMoveJump + farRoom * far, far, (int)sCamStill, g.s0, g.s1);
            }
        }
        g.step = boneStep;
        g.r = (u8)r;
        out.push_back(g);
        isRot[r] = isRot[r + 1] = isRot[r + 2] = 1;
        return true;
    };
    for (int r = 10; r + 2 <= 84; r += 3)
        if (!one(r)) return false;
    if (!one(90)) return false;
    // (22/11) a bone that turned more than 120 degrees while the draw's TYPICAL bone (median) turned little is a local flip -
    // Lillie's bag swinging round (c13/c16: 140-150 degrees, the body a few) - not a camera cut (then every bone turns
    // together, the camera is baked into them). It is mixed the short way (with the screw) instead of changing at once in
    // the middle: stepped, the bag flipped at every game frame (the bag "at 30 FPS" on the bridge run).
    // PORTE_INTERP_VIRADA=0: the old step
    static const bool flipMix = !(getenv("PORTE_INTERP_VIRADA") && atoi(getenv("PORTE_INTERP_VIRADA")) == 0);
    if (flipMix && !violent)
    {
        bool anyCut = false;
        for (const RotGroup& g : out) anyCut |= g.rotCut;
        if (anyCut)
        {
            std::vector<float> ang;
            for (const RotGroup& g : out)
                if (g.r < 86)
                {
                    const float dd = std::min(1.0f, std::fabs(g.q0[0] * g.q1[0] + g.q0[1] * g.q1[1] + g.q0[2] * g.q1[2] + g.q0[3] * g.q1[3]));
                    ang.push_back(2.0f * std::acos(dd));
                }
            if (ang.size() >= 3)
            {
                std::nth_element(ang.begin(), ang.begin() + ang.size() / 2, ang.end());
                if (ang[ang.size() / 2] < 1.047f)   // (median < 60 degrees)
                    for (RotGroup& g : out)
                        if (g.rotCut && (!g.moveCut || g.radiusOnly) && g.r < 86) { g.step = false; g.rotCut = false; sFlipMixed++; }
            }
        }
    }
    // (22/11) the same for the screw radius guard: in a close-up (camera very near) a swinging part's turn axis can lie
    // farther than the guard allows; if the draw's TYPICAL bone neither jumped nor turned much, it is one part swinging
    // (the bag on the bridge run: c13/c16/c19 turning 9 degrees, 1600 units), not the model teleporting -> mixed
    if (flipMix && !violent)
    {
        bool anyR = false;
        for (const RotGroup& g : out) anyR |= g.radiusOnly && g.step;
        if (anyR)
        {
            std::vector<float> ratio, ang;
            for (const RotGroup& g : out)
                if (g.r < 86)
                {
                    ratio.push_back(g.moveRatio);
                    const float dd = std::min(1.0f, std::fabs(g.q0[0] * g.q1[0] + g.q0[1] * g.q1[1] + g.q0[2] * g.q1[2] + g.q0[3] * g.q1[3]));
                    ang.push_back(2.0f * std::acos(dd));
                }
            if (ratio.size() >= 3)
            {
                std::nth_element(ratio.begin(), ratio.begin() + ratio.size() / 2, ratio.end());
                std::nth_element(ang.begin(), ang.begin() + ang.size() / 2, ang.end());
                if (ratio[ratio.size() / 2] < 1.0f && ang[ang.size() / 2] < 1.047f)
                    for (RotGroup& g : out)
                        if (g.radiusOnly && g.step) { g.step = false; sFlipMixed++; }
            }
        }
    }
    // (20/11: "most bones turn > 90 degrees -> whole draw as it is now" was REMOVED at the user's request - it seemed
    // to cause texture glitches on 180-degree turns; turned/big are only counted)
    (void)turned; (void)big;
    return true;
}

// a row (not part of a rotation) that jumped between the two versions keeps its current value - only that row
// (often a register the draw does not even use, left over from an earlier draw)
static bool RowJumped(const OpDraw& p, const OpDraw& d, int r)
{
    for (int k = 0; k < 4; k++)
    {
        const float a = p.uf[r][k], b = d.uf[r][k];
        if (a == b) continue;
        if (!std::isfinite(a) || !std::isfinite(b)) return true;
        const float diff = std::fabs(b - a);
        const float far = std::max(std::fabs(a), std::fabs(b));
        // small values (matrix rotation part) jump by angle; big ones (translations, 2D positions in pixels) by distance
        const bool small = k < 3 && std::fabs(a) <= 1.5f && std::fabs(b) <= 1.5f;
        if (small ? diff > kRotJump : diff > kMoveJump + 0.5f * far)
        {
            static int left = getenv("PORTE_INTERP_DIAG") ? 40 : 0;
            if (left > 0) { left--; fprintf(stderr, "interp: linha c%d.%d %.4f -> %.4f fica no valor novo\n", r, k, a, b); }
            return true;
        }
    }
    return false;
}

// how far apart two versions are (positions of everything the draw is placed by): picks, among identical
// objects (same kind of NPC, tree, grass...), the one that really is the same object - the game may draw
// them in a different order from one picture to the next (sorted by distance)
static float Distance(const OpDraw& p, const OpDraw& d)
{
    // (20/11 noite) gfl2: all the placing / bone matrices (c10-c85), not only the first translation - a model drawn
    // twice with the same mesh (Lillie and her mirrored reflection on the corridor floor, opening) often has the same
    // c10-c12 in both; the pair was then decided by the draw order, and when the game sorted them the other way the
    // bones of the mirrored copy were mixed in (the bag swung to the other side in the in-between pictures)
    if (Gfl2Layout(p) && Gfl2Layout(d))
    {
        float s = 0;
        for (int r = 10; r < 86; r++)
            for (int k = 0; k < 4; k++)
            {
                const float a = p.uf[r][k], b = d.uf[r][k];
                if (std::isfinite(a) && std::isfinite(b)) s += std::fabs(a - b) * (k == 3 ? 0.01f : 1.0f);   // (translations are large numbers)
            }
        return s;
    }
    return std::fabs(p.sig[0] - d.sig[0]) + std::fabs(p.sig[1] - d.sig[1]) + std::fabs(p.sig[2] - d.sig[2]);
}

static void CheckCut(FrameList& L);

// pairs each draw of L with its previous version in W, once per pair of pictures
static void EnsurePairs(FrameList& L, const FrameList& W)
{
    if (L.pairedWith == &W && L.pairedWithGen == W.gen && L.pairedGen == L.gen) return;
    L.pairedWith = &W;
    L.pairedWithGen = W.gen;
    L.pairedGen = L.gen;
    const size_t n = L.drawsUsed;
    L.pairP.assign(n, nullptr);
    L.pairState.assign(n, 0);
    if (L.pairDiff.size() < n) L.pairDiff.resize(n);
    if (L.pairRot.size() < n) L.pairRot.resize(n);
    if (L.pairIdx.size() < n) L.pairIdx.resize(n);
    for (size_t i = 0; i < n; i++) { L.pairDiff[i].clear(); L.pairRot[i].clear(); L.pairIdx[i].clear(); }

    // how much the camera turned between the two pictures (gfl2: c90-c92, the same for every 3D draw)
    sCamStill = false;
    for (size_t i = 0; i < L.drawsUsed && i < W.drawsUsed; i++)
    {
        const OpDraw& a = *L.draws[i];
        if (a.hud || !Gfl2Layout(a)) continue;
        const OpDraw* b = nullptr;
        for (size_t j = 0; j < W.drawsUsed && !b; j++) if (!W.draws[j]->hud && Gfl2Layout(*W.draws[j])) b = W.draws[j].get();
        if (!b) break;
        float tr = 0;
        for (int r = 90; r <= 92; r++) tr += a.uf[r][0] * b->uf[r][0] + a.uf[r][1] * b->uf[r][1] + a.uf[r][2] * b->uf[r][2];
        sCamStill = std::isfinite(tr) && tr > 2.997f;   // 1 + 2 cos(2.5 degrees)
        break;
    }
    struct Cand { const OpDraw* d; bool used; };
    static std::unordered_map<u64, std::vector<Cand>> byKey;
    for (auto& [k, v] : byKey) v.clear();
    for (size_t i = 0; i < W.drawsUsed; i++) byKey[DrawKey(*W.draws[i])].push_back({ W.draws[i].get(), false });
    // 2D (HUD) draws are mixed only when they are the ONLY one of their kind in both pictures: a sprite that
    // follows the world (the yellow "A" over a door) moves with the 3D at the in-between pictures; text glyphs and
    // other repeated pieces are not mixed (they could slide into each other's place)
    static std::unordered_map<u64, int> hudCount;
    hudCount.clear();
    for (size_t i = 0; i < n; i++)
        if (L.draws[i]->hud) hudCount[DrawKey(*L.draws[i])]++;

    // (20/11 noite) pairs chosen for the whole group of draws that share a key, closest pair first - picking in draw
    // order let a hidden copy of a model (the same mesh drawn again: a reflection, a shadow) take the pair of the
    // visible one, and the visible model got the other copy's bones (the opening: Lillie's bag on the wrong side)
    static std::vector<const OpDraw*> chosen;
    chosen.assign(n, nullptr);
    {
        static std::unordered_map<u64, std::vector<size_t>> mine;
        for (auto& [k, v] : mine) v.clear();
        for (size_t i = 0; i < n; i++) mine[DrawKey(*L.draws[i])].push_back(i);
        struct PairD { float dist; size_t i; size_t c; };
        static std::vector<PairD> all;
        for (auto& [k, idx] : mine)
        {
            if (idx.empty()) continue;
            auto it = byKey.find(k);
            if (it == byKey.end() || it->second.empty()) continue;
            std::vector<Cand>& cands = it->second;
            if (idx.size() == 1 && cands.size() == 1) { chosen[idx[0]] = cands[0].d; cands[0].used = true; continue; }
            all.clear();
            for (size_t i : idx)
                for (size_t c = 0; c < cands.size(); c++) all.push_back({ Distance(*cands[c].d, *L.draws[i]), i, c });
            std::sort(all.begin(), all.end(), [](const PairD& x, const PairD& y) { return x.dist < y.dist; });
            for (const PairD& pd : all)
                if (!chosen[pd.i] && !cands[pd.c].used) { chosen[pd.i] = cands[pd.c].d; cands[pd.c].used = true; }
        }
    }
    // (diagnosis) PORTE_INTERP_DIAGPEQ=<from s>,<to s>: every small 3D draw (<= 16 vertices), how many of its kind there are
    // in L and in W, and its pair
    {
        static double eA = -1, eB = -1;
        if (eA < 0) { eA = 0; if (const char* e = getenv("PORTE_INTERP_DIAGPEQ")) sscanf(e, "%lf,%lf", &eA, &eB); }
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count();
        if (eB > 0 && el >= eA && el <= eB)
        {
            static std::unordered_map<u64, int> inL;
            inL.clear();
            for (size_t i = 0; i < n; i++) inL[DrawKey(*L.draws[i])]++;
            for (size_t i = 0; i < n; i++)
            {
                const OpDraw& d = *L.draws[i];
                if (d.regs.pipeline.num_vertices > 16 || !Gfl2Layout(d) || d.mesh == 0x25560CB0u) continue;
                auto it = byKey.find(DrawKey(d));
                const int nW = it == byKey.end() ? 0 : (int)it->second.size();
                char x[64] = " sem par";
                if (chosen[i]) snprintf(x, sizeof(x), " <- (%.0f %.0f %.0f)", chosen[i]->uf[10][3], chosen[i]->uf[11][3], chosen[i]->uf[12][3]);
                fprintf(stderr, "PEQ t=%.2f g%u d%zu mesh %08X nv %u tex %08X L%d W%d (%.0f %.0f %.0f)%s\n", el, (u32)L.gen, i, d.mesh,
                        (u32)d.regs.pipeline.num_vertices, d.regs.texturing.texture0.GetPhysicalAddress(), inL[DrawKey(d)], nW, d.uf[10][3], d.uf[11][3], d.uf[12][3], x);
            }
        }
    }
    // (22/11) the pose step for the WHOLE model: the draws of one character (body, outline, Lillie's bag...) share bone
    // matrices - identical 3-row groups in the same picture. If any of them has a violent bone, all of them step together
    // (state 3). Stepping one draw while the others of the same model kept mixing drew lines across / a ghost outline;
    // not stepping at all left the bag behind at 30 per second and broken on fast turns.
    static std::vector<int> grp;
    static std::vector<u8> grpViolent;
    grp.assign(n, -1);
    static const bool noGroup = getenv("PORTE_INTERP_GRUPO") == nullptr;   // (off: all of Lillie lagged; =1 turns it on, test)
    if (!noGroup)
    {
        std::vector<int> parent(n);
        for (size_t i = 0; i < n; i++) parent[i] = (int)i;
        auto find = [&](int x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
        static std::unordered_map<u64, int> owner;
        owner.clear();
        for (size_t i = 0; i < n; i++)
        {
            const OpDraw& d = *L.draws[i];
            if (d.hud || !Gfl2Layout(d)) continue;
            for (int r = 10; r + 2 <= 84; r += 3)
            {
                // a real bone: a finite matrix with a translation (leftover zero / identity rows are not used to join)
                bool ok = true; float tsum = 0;
                for (int k = 0; k < 3 && ok; k++)
                    for (int c = 0; c < 4; c++) if (!std::isfinite(d.uf[r + k][c])) { ok = false; break; }
                if (!ok) continue;
                for (int k = 0; k < 3; k++) tsum += std::fabs(d.uf[r + k][3]);
                if (tsum < 1.0f) continue;
                const u64 h = Common::ComputeHash64(&d.uf[r][0], sizeof(float) * 12);
                auto [it2, fresh] = owner.emplace(h, (int)i);
                if (!fresh) { const int a = find((int)i), b = find(it2->second); if (a != b) parent[a] = b; }
            }
        }
        grpViolent.assign(n, 0);
        for (size_t i = 0; i < n; i++)
        {
            grp[i] = find((int)i);
            if (!chosen[i] || !Gfl2Layout(*L.draws[i]) || !Gfl2Layout(*chosen[i])) continue;
            std::vector<RotGroup> tmp; u8 tr[96]; bool v = false;
            RotGroups(*chosen[i], *L.draws[i], tmp, tr, &v);
            if (v) grpViolent[grp[i]] = 1;
        }
    }
    for (size_t i = 0; i < n; i++)
    {
        const OpDraw& d = *L.draws[i];
        auto it = byKey.find(DrawKey(d));
        if (it == byKey.end()) continue;
        if (d.hud && (hudCount[DrawKey(d)] != 1 || it->second.size() != 1)) continue;
        if (!chosen[i]) continue;
        const OpDraw& p = *chosen[i];
        L.pairP[i] = &p;
        u8 isRot[96];
        bool violent = false;
        // (22/11) the 21/11 "pose step" (state 3: a draw with a violent bone stepped from the old pose to the new one in the
        // middle) is OFF: the other draws of the same model kept being mixed, so in the in-between pictures the parts no
        // longer matched - long black lines across the screen and a ghost outline of the player when turning (the user's
        // video; reproduced and measured: 5 pictures with lines in 120 -> 0). Violent bones snap again (as before 21/11).
        // (tests) PORTE_INTERP_VAR=0: the pose step back; 2: a violent draw is shown as it is
        static const int var = getenv("PORTE_INTERP_VAR") ? atoi(getenv("PORTE_INTERP_VAR")) : 1;
        // (22/11, beach) ...but a SMALL draw (<= 16 vertices: the blob shadows of the girl and Comfey - one quad whose "bones"
        // are the shadows) keeps the pose step: there a bone that changes at once is the game giving that shadow slot to
        // another one, and snapping only that bone made the shadow travel from the other one ("the shadow teleports";
        // tracker on the user's beach state: 0-4 per run -> 0). A small draw has no other draws of the same model to
        // fall out of step with (the cause of the lines on the big ones)
        static const u32 stepMax = getenv("PORTE_INTERP_PASSOMAX") ? (u32)atoi(getenv("PORTE_INTERP_PASSOMAX")) : 16;   // (test)
        const bool groupStep = !noGroup && grp[i] >= 0 && grpViolent[grp[i]];
        const bool step = var != 1 || d.regs.pipeline.num_vertices <= stepMax || groupStep;
        if (!RotGroups(p, d, L.pairRot[i], isRot, step ? &violent : nullptr)) { L.pairState[i] = 2; L.pairRot[i].clear(); continue; }
        // (22/11) a small draw that moves MANY bones is a piece of a bigger skinned model (Lillie's red strap: 12 vertices,
        // 20 bones), not a loose object like the beach shadows: stepping it whole in the middle made it pop in and out at
        // every game frame (the bag "at 30 FPS"). It is mixed like the rest of the body. PORTE_INTERP_PEQOSSOS=<n> (default 8)
        static const size_t smallBones = getenv("PORTE_INTERP_PEQOSSOS") ? (size_t)atoi(getenv("PORTE_INTERP_PEQOSSOS")) : 8;
        if (step && violent && var == 1 && !groupStep && L.pairRot[i].size() > smallBones)
        {
            violent = false;
            if (!RotGroups(p, d, L.pairRot[i], isRot, nullptr)) { L.pairState[i] = 2; L.pairRot[i].clear(); continue; }
        }
        // (diagnosis) PORTE_INTERP_DIAGVIOL=<from s>,<to s>: draws with a violent bone (what the pose step would act on)
        {
            static double vA = -1, vB = -1;
            if (vA < 0) { vA = 0; if (const char* e = getenv("PORTE_INTERP_DIAGVIOL")) sscanf(e, "%lf,%lf", &vA, &vB); }
            if (vB > 0)
            {
                const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count();
                bool v2 = violent;
                if (!step && el >= vA && el <= vB) { std::vector<RotGroup> tmp; u8 tr[96]; RotGroups(p, d, tmp, tr, &v2); }
                if (v2 && el >= vA && el <= vB)
                    fprintf(stderr, "VIOL t=%.2f g%u d%zu mesh %08X nv %u prog %llX tex %08X passo %d\n", el, (u32)L.gen, i, d.mesh,
                            (u32)d.regs.pipeline.num_vertices, (unsigned long long)d.prog, d.regs.texturing.texture0.GetPhysicalAddress(), (int)step);
            }
        }
        if (violent && var == 2) { L.pairState[i] = 2; L.pairRot[i].clear(); continue; }
        L.pairState[i] = (violent || (groupStep && Gfl2Layout(d))) ? 3 : 1;
        static const bool soCam = getenv("PORTE_INTERP_SOCAM") != nullptr;   // (test) mix only the camera / projection
        if (soCam)
        {
            L.pairRot[i].erase(std::remove_if(L.pairRot[i].begin(), L.pairRot[i].end(), [](const RotGroup& g) { return g.r < 86; }), L.pairRot[i].end());
            for (int r = 0; r < 86; r++) isRot[r] = 1;   // (rows below 86: kept as they are now)
        }
        const bool gfl2 = Gfl2Layout(d);
        int dgFrozen = 0; std::string dgFrozenRows;
        for (int r = 0; r < 96; r++)
        {
            if (isRot[r]) continue;   // mixed as a rotation
            if (gfl2 && r >= 10 && r < 93 && RowJumped(p, d, r) && !(violent && r < 86)) { sCutRow++; dgFrozen++; dgFrozenRows += "c" + std::to_string(r) + " "; continue; }
            if (!gfl2 && RowJumped(p, d, r)) { sCutRow++; continue; }
            // texture matrices are never mixed: a swap (eyes, mouth, animated patterns) of any size would show
            // the texture between two cells; they keep the current value (scrolling then moves at 30 per second)
            static const bool mixTex = getenv("PORTE_INTERP_MIXTEX") != nullptr;   // (test)
            if (gfl2 && r >= 1 && r <= 9 && !mixTex)
            {
                if (memcmp(p.uf[r], d.uf[r], sizeof(d.uf[r])) == 0) continue;
                // (20/11 noite) a SCROLL (water, clouds, effects: only the offset moves, by a little) is mixed - it moved at
                // 30 per second while the rest ran at 200; a swap of cells (eyes, mouth: a jump of 1/8, 1/4..) is not
                const bool scroll = p.uf[r][0] == d.uf[r][0] && p.uf[r][1] == d.uf[r][1] && p.uf[r][2] == d.uf[r][2] &&
                                    std::isfinite(p.uf[r][3]) && std::isfinite(d.uf[r][3]) && std::fabs(d.uf[r][3] - p.uf[r][3]) < 0.02f;
                static const bool noScroll = getenv("PORTE_INTERP_SEMROLAGEM") != nullptr;   // (test) the old rule
                if (!scroll || noScroll) { sTexSnaps++; continue; }
                sTexScroll++;
                L.pairDiff[i].push_back((u16)(r * 4 + 3));
                continue;
            }
            // (22/11) a value that is a whole number in both pictures and changes is an INDEX (which light table, which
            // entry): mixed, 5 -> 6 gave 5.999 and the shader took table 5 until the very end (the Charizard flight's
            // flash: the character black in every in-between picture). Not mixed: in a 3D draw it changes in the middle
            // ("valor antigo até o meio"). PORTE_INTERP_MISTURAINDICE=1: the old way
            static const bool mixIndex = getenv("PORTE_INTERP_MISTURAINDICE") != nullptr;
            for (int k = 0; k < 4; k++)
                if (p.uf[r][k] != d.uf[r][k] && std::isfinite(p.uf[r][k]) && std::isfinite(d.uf[r][k]))
                {
                    if (!mixIndex && gfl2 && p.uf[r][k] == std::floor(p.uf[r][k]) && d.uf[r][k] == std::floor(d.uf[r][k]) &&
                        std::fabs(p.uf[r][k]) <= 1024.0f && std::fabs(d.uf[r][k]) <= 1024.0f)
                    { sIndexStepped++; L.pairIdx[i].push_back((u16)(r * 4 + k)); continue; }
                    L.pairDiff[i].push_back((u16)(r * 4 + k));
                }
        }
        // (diagnosis) PORTE_INTERP_DIAGOSSO=<from s>,<to s>: per 3D draw - bones mixed as rotations, stepped, rows frozen
        {
            static double oA = -1, oB = -1;
            if (oA < 0) { oA = 0; if (const char* e = getenv("PORTE_INTERP_DIAGOSSO")) sscanf(e, "%lf,%lf", &oA, &oB); }
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count();
            if (oB > 0 && el >= oA && el <= oB && gfl2)
            {
                int nst = 0; for (const RotGroup& g : L.pairRot[i]) nst += g.step;
                int lin = 0; for (const u16 c : L.pairDiff[i]) if ((c >> 2) >= 10 && (c >> 2) < 86 && (c & 3) != 3) lin++;
                fprintf(stderr, "OSSO t=%.3f g%u mesh %08X nv %u estado %d giram %zu passo %d linhas_lineares %d congeladas %d %s\n", el, (u32)L.gen, d.mesh,
                        (u32)d.regs.pipeline.num_vertices, (int)L.pairState[i], L.pairRot[i].size(), nst, lin, dgFrozen, dgFrozenRows.substr(0, 120).c_str());
            }
        }
        // (diagnosis) PORTE_INTERP_DIAGTXT=<from s>,<to s>: every mixed draw with rows below 86 that change
        {
            static double dA = -1, dB = -1;
            if (dA < 0) { dA = 0; if (const char* e = getenv("PORTE_INTERP_DIAGTXT")) sscanf(e, "%lf,%lf", &dA, &dB); }
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count();
            if (dB > 0 && el >= dA && el <= dB && !L.pairRot[i].empty() && L.pairState[i] != 2)
            {
                std::string t;
                for (const RotGroup& g : L.pairRot[i])
                    if (g.r < 86) { char x[96]; snprintf(x, sizeof(x), " g%d t(%.3g %.3g %.3g)>(%.3g %.3g %.3g) s%.3g>%.3g", g.r, g.t0[0], g.t0[1], g.t0[2], g.t1[0], g.t1[1], g.t1[2], g.s0, g.s1); t += x; }
                if (!t.empty()) fprintf(stderr, "DIAGROT t=%.2f g%u d%zu st%d hud=%d gfl2=%d mesh %08X nv %u prog %llX tex %08X:%s\n", el, (u32)L.gen, i, (int)L.pairState[i], (int)d.hud, (int)gfl2,
                                         d.mesh, (u32)d.regs.pipeline.num_vertices, (unsigned long long)d.prog, d.regs.texturing.texture0.GetPhysicalAddress(), t.substr(0, 400).c_str());
            }
            if (dB > 0 && el >= dA && el <= dB && !L.pairDiff[i].empty() && L.pairState[i] == 1)
            {
                std::string t;
                int shown = 0;
                for (const u16 c : L.pairDiff[i])
                    if ((c >> 2) < 86 && shown++ < 10) { char x[64]; snprintf(x, sizeof(x), " c%d.%d %.4g>%.4g", c >> 2, c & 3, p.uf[c >> 2][c & 3], d.uf[c >> 2][c & 3]); t += x; }
                if (shown) fprintf(stderr, "DIAGTXT t=%.2f g%u d%zu hud=%d gfl2=%d mesh %08X nv %u prog %llX tex %08X diffs %zu rot %zu:%s\n", el, (u32)L.gen, i, (int)d.hud, (int)gfl2, d.mesh,
                                     (u32)d.regs.pipeline.num_vertices, (unsigned long long)d.prog, d.regs.texturing.texture0.GetPhysicalAddress(), L.pairDiff[i].size(), L.pairRot[i].size(), t.c_str());
            }
        }
        // (22/11) menus / text / 2D (not gfl2) are NOT mixed: the game's layout keeps tables in the uniforms (the letters of a
        // text: positions c32..c57, an index c9.3, texture coordinates of each letter in c9) - mixing them, even by 0.01,
        // drew pieces of the wrong letters and lines across (the save screen's numbers while it loads; measured with
        // lote/digitos.py: 2-4 garbled pictures every time, 0 with this). Only a 2D sprite whose position alone changed
        // (the translation column: the "A" over a door, which follows the 3D world) is still mixed.
        // PORTE_INTERP_2DANTIGO=1: the old rule (test)
        static const bool old2d = getenv("PORTE_INTERP_2DANTIGO") != nullptr;
        if (!gfl2 && !old2d && L.pairState[i] == 1)
        {
            bool onlyPos = d.hud;
            for (const u16 c : L.pairDiff[i])
                if ((c & 3) != 3) { onlyPos = false; break; }
            if (!onlyPos) { L.pairDiff[i].clear(); L.pairState[i] = 2; sHudKept++; }
        }
    }
    // (22/11) PARTICLES: many identical small draws (same mesh, texture and shader; 24-64 vertices) in one picture - the
    // Festival Plaza's teleport swirls, effect sparks. Each one keeps its place / texture cell in the uniforms and they all
    // look alike, so a particle was paired with ANOTHER one of the next picture and the mix drew streaks where there are
    // none. Not mixed: each changes in the middle (30 per second, always in a real place). The beach shadows (6-12
    // vertices) are not touched. PORTE_INTERP_PARTICULAS=0: the old way; PORTE_INTERP_PARTMIN=<n> copies (default 4)
    {
        static const bool partOn = !(getenv("PORTE_INTERP_PARTICULAS") && atoi(getenv("PORTE_INTERP_PARTICULAS")) == 0);
        static const int partMin = getenv("PORTE_INTERP_PARTMIN") ? atoi(getenv("PORTE_INTERP_PARTMIN")) : 4;
        if (partOn)
        {
            std::unordered_map<u64, int> cnt;
            for (size_t i = 0; i < L.drawsUsed; i++)
            {
                const OpDraw& d = *L.draws[i];
                const u32 nv = d.regs.pipeline.num_vertices;
                if (nv >= 24 && nv <= 64 && !d.hud) cnt[DrawKey(d)]++;
            }
            for (size_t i = 0; i < L.drawsUsed && i < L.pairState.size(); i++)
            {
                const OpDraw& d = *L.draws[i];
                const u32 nv = d.regs.pipeline.num_vertices;
                if (nv < 24 || nv > 64 || d.hud || (L.pairState[i] != 1 && L.pairState[i] != 3)) continue;
                const auto it = cnt.find(DrawKey(d));
                if (it == cnt.end() || it->second < partMin) continue;
                L.pairState[i] = 2; L.pairRot[i].clear(); L.pairDiff[i].clear();
                sParticles++;
            }
        }
    }
    CheckCut(L);
    // (diagnosis) PORTE_INTERP_DUMPU=a,b: for the pictures a..b, the first 3D draws and their pairs
    static int dumpA = -1, dumpB = -1;
    if (dumpA < 0)
    {
        dumpA = 0;
        if (const char* e = getenv("PORTE_INTERP_DUMPU")) { dumpA = atoi(e); if (const char* c = strchr(e, ',')) dumpB = atoi(c + 1); }
    }
    if (dumpA > 0 && (int)L.gen >= dumpA && (int)L.gen <= dumpB)
    {
        int shown = 0;
        for (size_t i = 0; i < L.drawsUsed; i++)
        {
            const OpDraw& d = *L.draws[i];
            if (d.hud) continue;
            // summary of every 3D draw: rotation groups, the biggest turn (smallest quaternion dot), biggest move
            float minDot = 1.0f, maxMove = 0.0f;
            for (const RotGroup& g : L.pairRot[i])
            {
                minDot = std::min(minDot, std::fabs(g.q0[0] * g.q1[0] + g.q0[1] * g.q1[1] + g.q0[2] * g.q1[2] + g.q0[3] * g.q1[3]));
                for (int k = 0; k < 3; k++) maxMove = std::max(maxMove, std::fabs(g.t1[k] - g.t0[k]));
            }
            fprintf(stderr, "RESUMO g%u d%zu mesh %08X v%u est %d grupos %zu diffs %zu menor_dot %.3f maior_mov %.1f sig %.1f %.1f %.1f\n", (u32)L.gen, i, d.mesh,
                    (u32)d.regs.pipeline.num_vertices, (int)L.pairState[i], L.pairRot[i].size(), L.pairDiff[i].size(), minDot, maxMove, d.sig[0], d.sig[1], d.sig[2]);
            if (shown >= 3) continue;
            shown++;
            const OpDraw* q = L.pairP[i];
            fprintf(stderr, "DUMP g%u d%zu prog %llX mesh %08X v%u gfl2 %d par %s estado %d\n", L.gen, i, (unsigned long long)d.prog, d.mesh,
                    (u32)d.regs.pipeline.num_vertices, (int)Gfl2Layout(d), q ? "sim" : "não", (int)L.pairState[i]);
            for (int r : { 86, 87, 88, 89, 90, 91, 92, 10, 11, 12 })
                fprintf(stderr, "   c%d %9.4f %9.4f %9.4f %9.4f | ant %9.4f %9.4f %9.4f %9.4f\n", r, d.uf[r][0], d.uf[r][1], d.uf[r][2], d.uf[r][3],
                        q ? q->uf[r][0] : 0.f, q ? q->uf[r][1] : 0.f, q ? q->uf[r][2] : 0.f, q ? q->uf[r][3] : 0.f);
        }
    }
}

// a camera cut: the typical (median) turn of the placing matrix (c10-c12) of all the 3D draws is over 30 degrees
// in one game frame -> nothing is mixed (the in-between pictures would sweep through the cut)
static u64 sCamCuts = 0, sCutZoom = 0, sCutJump = 0, sCutCamRot = 0, sCutCamJump = 0;   // (cuts by zoom / by the camera jumping)
static void MarkLayout(FrameList& L)
{
    static const bool off = getenv("PORTE_INTERP_SEMLYT") != nullptr;   // (test) the old classification
    // many small draws of one mesh with at least 3 different textures (each letter / panel its own): layout. Grass or
    // trees drawn many times repeat the SAME texture and stay 3D
    static std::unordered_map<u32, std::vector<u32>> texOf;
    for (auto& [m, v] : texOf) v.clear();
    for (size_t i = 0; i < L.drawsUsed; i++)
    {
        const OpDraw& d = *L.draws[i];
        if (d.regs.pipeline.num_vertices > 96) continue;
        auto& v = texOf[d.mesh];
        const u32 tex = d.regs.texturing.texture0.GetPhysicalAddress();
        if (v.size() < 64) v.push_back(tex);
    }
    static std::unordered_map<u32, bool> isLyt;
    isLyt.clear();
    for (auto& [m, v] : texOf)
    {
        if (v.size() < 6) continue;
        std::sort(v.begin(), v.end());
        isLyt[m] = std::unique(v.begin(), v.end()) - v.begin() >= 3;
    }
    for (size_t i = 0; i < L.drawsUsed; i++)
    {
        OpDraw& d = *L.draws[i];
        auto it = isLyt.find(d.mesh);
        d.lyt = !off && d.regs.pipeline.num_vertices <= 96 && it != isLyt.end() && it->second;
    }
}

static void CheckCut(FrameList& L)
{
    static std::vector<float> dots;
    dots.clear();
    for (size_t i = 0; i < L.drawsUsed; i++)
    {
        if (L.pairState[i] == 0) continue;
        const OpDraw& d = *L.draws[i];
        if (!Gfl2Layout(d)) continue;
        float dot = 1.0f;
        if (L.pairState[i] == 2) dot = 0.0f;
        else
            for (const RotGroup& g : L.pairRot[i])
                if (g.r == 10)
                    dot = g.q0[0] * g.q1[0] + g.q0[1] * g.q1[1] + g.q0[2] * g.q1[2] + g.q0[3] * g.q1[3];
        dots.push_back(dot);
    }
    if (dots.size() < 4) return;
    std::nth_element(dots.begin(), dots.begin() + dots.size() / 2, dots.end());
    bool cut = dots[dots.size() / 2] < 0.966f;   // cos(15): the half-angle of 30 degrees
    // (20/11 noite) cutscene cuts that keep the direction: the camera jumps to another place or another zoom (the
    // opening: wide shot of the corridor -> close-up of Lillie). Turning alone did not see them, and the in-between
    // pictures zoomed and slid from one shot to the other ("tremedeira" in the opening and the 3D cutscenes).
    // Also a cut: the projection's scale (c86 x, c87 y) changing more than 8% in one game frame, or the placing
    // translation (c10-c12, view space) jumping more than 35% of its distance - medians over the 3D draws
    if (!cut)
    {
        static std::vector<float> zooms, moves;
        zooms.clear();
        moves.clear();
        for (size_t i = 0; i < L.drawsUsed; i++)
        {
            if (L.pairState[i] == 0 || !L.pairP[i]) continue;
            const OpDraw& d = *L.draws[i];
            const OpDraw& p = *L.pairP[i];
            if (!Gfl2Layout(d) || !Gfl2Layout(p)) continue;
            // (the projection may come turned - the 3DS screen is sideways: c86 = (0, sy, ..), c87 = (sx, 0, ..) -
            // the length of each row is the scale either way)
            const float zx0 = std::hypot(p.uf[86][0], p.uf[86][1]), zx1 = std::hypot(d.uf[86][0], d.uf[86][1]);
            const float zy0 = std::hypot(p.uf[87][0], p.uf[87][1]), zy1 = std::hypot(d.uf[87][0], d.uf[87][1]);
            if (zx0 > 1e-6f && zx1 > 1e-6f && zy0 > 1e-6f && zy1 > 1e-6f)
                zooms.push_back(std::max(std::fabs(std::log(zx1 / zx0)), std::fabs(std::log(zy1 / zy0))));
            float d0 = 0, dd = 0;
            for (int k = 0; k < 3; k++)
            {
                const float a0 = p.uf[10 + k][3], a1 = d.uf[10 + k][3];
                if (!std::isfinite(a0) || !std::isfinite(a1)) { d0 = -1; break; }
                d0 += a0 * a0;
                dd += (a1 - a0) * (a1 - a0);
            }
            if (d0 >= 0) moves.push_back(std::sqrt(dd) / std::max(std::sqrt(d0), 1.0f));
        }
        auto median = [](std::vector<float>& v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
        if (zooms.size() >= 4 && median(zooms) > 0.077f) { cut = true; sCutZoom++; }   // ln(1.08)
        else if (moves.size() >= 4 && median(moves) > 0.35f) { cut = true; sCutJump++; }
    }
    // the camera itself (gfl2: c90-c92 = rotation | translation, the same for every draw): a cut turns it more
    // than ~30 degrees, or moves it far more than it has been moving (the opening: 16 units a frame, the cuts
    // 670 / 6174 / 11667 - the scene's own matrices are identity, the old checks could not see them)
    {
        static float camSpeed = 0;   // typical movement of the camera per game frame (smoothed)
        static float lastJump = 0;   // the camera movement of the frame before
        const OpDraw* cd = nullptr;
        const OpDraw* cp = nullptr;
        for (size_t i = 0; i < L.drawsUsed && !cd; i++)
            if (L.pairState[i] != 0 && L.pairP[i] && Gfl2Layout(*L.draws[i]) && Gfl2Layout(*L.pairP[i])) { cd = L.draws[i].get(); cp = L.pairP[i]; }
        if (cd)
        {
            float tr = 0, jump2 = 0;
            bool ok = true;
            for (int r = 90; r <= 92; r++)
            {
                for (int k = 0; k < 4; k++) if (!std::isfinite(cd->uf[r][k]) || !std::isfinite(cp->uf[r][k])) ok = false;
                tr += cd->uf[r][0] * cp->uf[r][0] + cd->uf[r][1] * cp->uf[r][1] + cd->uf[r][2] * cp->uf[r][2];
                jump2 += (cd->uf[r][3] - cp->uf[r][3]) * (cd->uf[r][3] - cp->uf[r][3]);
            }
            if (ok)
            {
                const float jump = std::sqrt(jump2);
                if (!cut && tr < 2.73f) { cut = true; sCutCamRot++; }                                   // 1 + 2 cos(30)
                // (a jump like the one of the frame before is the camera moving fast, not a cut)
                else if (!cut && jump > 100.0f && jump > 8.0f * std::max(camSpeed, 2.0f) && !(lastJump > 0.4f * jump && lastJump < 2.5f * jump))
                { cut = true; sCutCamJump++; }
                if (!cut) camSpeed = camSpeed * 0.8f + jump * 0.2f;
                lastJump = jump;
            }
        }
    }
    if (!cut) return;
    sCamCuts++;
    for (size_t i = 0; i < L.drawsUsed; i++)
        if (L.pairState[i] == 1 || L.pairState[i] == 3) { L.pairState[i] = 2; L.pairRot[i].clear(); L.pairDiff[i].clear(); }
}

// the uniforms draw i of L gets at t (mixed with its previous version in "with"; with == nullptr: as recorded)
static void MixUniforms(const FrameList& L, const FrameList* with, float t, size_t i, Pica::Uniforms& out, bool count)
{
    const OpDraw& d = *L.draws[i];
    out = d.uni;
    if (!with || i >= L.pairState.size()) return;
    const u8 st = L.pairState[i];
    // (22/11) paired with a draw of ANOTHER shader program (the Charizard flight's flash: the character's light effect
    // switches program): the same uniform registers mean other things there - never mix / step with it, current values only.
    // PORTE_INTERP_OUTROPROG=1: the old way
    static const bool otherProgOk = getenv("PORTE_INTERP_OUTROPROG") != nullptr;
    if (!otherProgOk && L.pairP[i] && L.pairP[i]->prog != d.prog) { if (count) sOtherProg++; return; }
    // (22/11) the self-check's ladder (AutoCheck): 1 = this draw changes at once in the middle
    const u8 av = (&L == sCur && i < sAvMask.size()) ? sAvMask[i] : 0;
    // (22/11) what is NOT mixed in a paired 3D draw (a bone that jumped, a frozen row, a draw shown as it is, a cut) keeps
    // the OLD value until the middle and then the new one - before it jumped to the new value at once, so t = 0 never
    // matched the real N-1 there and the self-check dropped the whole picture to 30 (the explosion, Z moves). It changes
    // just as abruptly (half a period later), and next to bones that are mixed the tear is half as big. Menus / 2D keep
    // the new value (their pair may be another letter). PORTE_INTERP_PASSOBASE=0: the old way
    static const bool stepBase = !(getenv("PORTE_INTERP_PASSOBASE") && atoi(getenv("PORTE_INTERP_PASSOBASE")) == 0);
    if (stepBase && t < 0.5f && (st == 1 || st == 2 || st == 3) && L.pairP[i] && Gfl2Layout(d) && Gfl2Layout(*L.pairP[i]))
    {
        out = L.pairP[i]->uni;
        // indices go with the tables / registers, which are the current picture's: always the current value (an old
        // index with the new light tables made the character black in the first half of the flash)
        if (i < L.pairIdx.size())
            for (const u16 c : L.pairIdx[i]) out.f[c >> 2][c & 3] = d.uni.f[c >> 2][c & 3];
        if (st == 2) return;
    }
    if (av == 1 && st == 2 && L.pairP[i]) { if (t < 0.5f) out = L.pairP[i]->uni; return; }
    if (st == 1 || st == 3)
    {
        const OpDraw& p = *L.pairP[i];
        const float step = t < 0.5f ? 0.0f : 1.0f;   // (state 3: the pose changes at once, in the middle)
        const bool stp = st == 3 || av == 1;
        for (const u16 c : L.pairDiff[i])
        {
            const int r = c >> 2, k = c & 3;
            const float tt = stp && r < 86 ? step : t;
            out.f[r][k] = Pica::f24::FromFloat32(p.uf[r][k] + (d.uf[r][k] - p.uf[r][k]) * tt);
        }
        for (const RotGroup& g : L.pairRot[i])
        {
            float m[3][4];
            MixRot(g, (stp && g.r < 86) || g.step ? step : t, m);
            for (int a = 0; a < 3; a++)
                for (int k = 0; k < 4; k++) out.f[g.r + a][k] = Pica::f24::FromFloat32(m[a][k]);
        }
        if (count) { sRotMixed += L.pairRot[i].size(); sMixed++; }
    }
    else if (count && st == 2) sJumps++;
    else if (count && !d.hud) sUnpaired++;
}

// marks dirty only the registers that differ (the rasterizer re-syncs just those): much cheaper than all
static void MarkChanged(Pica::PicaCore& pica, const Pica::RegsInternal& from, const Pica::RegsInternal& to)
{
    for (u32 i = 0; i < Pica::RegsInternal::NUM_REGS; i++)
        if (from.reg_array[i] != to.reg_array[i]) pica.dirty_regs.Set(i);
}

// redraws L; with "with", the uniforms of each paired draw are mixed: with + (L - with) * t
// memo: 0 = normal, 1 = first redraw of this picture (notes the texture lookups), 2 = reuses them
// Modo foto (Jogo > Modo foto): the last picture is redrawn with a free camera - the game's camera (c90-c92) of every
// gfl2 draw is multiplied by sPhoto.view, the projection's x / y rows (c86 / c87) by sPhoto.zoom, and the interface draws
// can be left out
static Photo sPhoto;
static void PhotoUniforms(const OpDraw& d, Pica::Uniforms& u)
{
    if (!Gfl2Layout(d)) return;
    float V[3][4], N[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) V[i][j] = u.f[90 + i][j].ToFloat32();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
        {
            float v = 0;
            for (int k = 0; k < 3; k++) v += sPhoto.view[i][k] * V[k][j];
            N[i][j] = v + (j == 3 ? sPhoto.view[i][3] : 0.0f);
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) u.f[90 + i][j] = Pica::f24::FromFloat32(N[i][j]);
    if (sPhoto.zoom != 1.0f)
        for (int r = 86; r <= 87; r++)
            for (int k = 0; k < 4; k++) u.f[r][k] = Pica::f24::FromFloat32(u.f[r][k].ToFloat32() * sPhoto.zoom);
}

static void Replay(VideoCore::RasterizerInterface* inner, FrameList& L, const FrameList* with, float t, int memo = 0)
{
    const unsigned long long porteSkips0 = Common::g_porte_async_skips;   // (22/11) skips made by OUR redraws (not the game's)
    const auto t0 = std::chrono::steady_clock::now();
    const u64 porteMixed0 = sMixed, porteJumps0 = sJumps;
    auto& pica = Core::System::GetInstance().GPU().PicaCore();
    auto& vs = pica.vs_setup;
    // estado atual (o do jogo): volta no fim
    static Pica::RegsInternal saveRegs;
    static Pica::Uniforms saveUni;
    static Pica::ProgramCode saveProg;
    static Pica::SwizzleData saveSwz;
    static TableSnap saveTables;
    saveRegs = pica.regs.internal;
    saveUni = vs.uniforms;
    saveProg = vs.GetProgramCode();
    saveSwz = vs.GetSwizzleData();
    const u32 saveProgSize = vs.GetBiggestProgramSize(), saveSwzSize = vs.GetBiggestSwizzleSize();
    const u64 saveProgHash = vs.GetProgramCodeHash(), saveSwzHash = vs.GetSwizzleDataHash();
    const u32 saveEntry = vs.entry_point;
    const Pica::AttributeBuffer saveFixed = pica.input_default_attributes;
    TakeTables(saveTables, pica);
    size_t applied = (size_t)-1;

    if (with) EnsurePairs(L, *with);

    const Pica::RegsInternal* last = &saveRegs;   // what the rasterizer last saw
    sReplaying = true;
    Vulkan::g_porte_memo_mode = sNoMemo ? 0 : memo;
    for (const Op& op : L.ops)
    {
        switch (op.type)
        {
        case OP_FILL: inner->AccelerateFill(op.fill); break;
        case OP_TEXCOPY: inner->AccelerateTextureCopy(op.copy); break;
        case OP_DT: inner->AccelerateDisplayTransfer(op.copy); break;   // intermediate copy (effects)
        case OP_SWDRAW:   // (22/11) a CPU draw: its registers and finished triangles, as they were
        {
            const SwDraw& w = *L.sws[op.draw];
            pica.regs.internal = w.regs;
            if (w.tables != applied) { PutTables(*L.snaps[w.tables], pica); applied = w.tables; }
            MarkChanged(pica, *last, w.regs);
            last = &w.regs;
            for (size_t k = 0; k + 2 < w.verts.size(); k += 3) inner->AddTriangle(w.verts[k], w.verts[k + 1], w.verts[k + 2]);
            inner->DrawTriangles();
            sSwReplayed++;
            break;
        }
        case OP_DRAW:
        {
            const OpDraw& d = *L.draws[op.draw];
            if (sPhoto.on && sPhoto.hideHud && d.hud) break;   // (modo foto: no interface)
            if (with && t < 0.5f && &L == sCur && op.draw < sAvMask.size() && sAvMask[op.draw] == 2) break;   // (lista automática: a new draw appears in the middle)
            pica.regs.internal = d.regs;
            MixUniforms(L, with, t, op.draw, vs.uniforms, true);
            if (sPhoto.on) PhotoUniforms(d, vs.uniforms);
            if (sTest == 2 && !d.hud)
                for (int r = 86; r <= 87; r++)   // afasta a câmera: encolhe x/y da projeção
                    for (int k = 0; k < 4; k++)
                        vs.uniforms.f[r][k] = Pica::f24::FromFloat32(d.uni.f[r][k].ToFloat32() * 0.6f);
            if (d.tables != applied) { PutTables(*L.snaps[d.tables], pica); applied = d.tables; }
            pica.input_default_attributes = d.fixedAttr;
            if (vs.GetProgramCodeHash() != d.prog)
            {
                const ProgCopy& pc = *sProgs[d.prog];
                vs.UpdateProgramCode(pc.code, pc.size);
            }
            if (vs.GetSwizzleDataHash() != d.swz)
            {
                const SwzCopy& sc = *sSwzs[d.swz];
                vs.UpdateSwizzleData(sc.data, sc.size);
            }
            vs.entry_point = d.entry;
            vs.uniforms_dirty = true;
            MarkChanged(pica, *last, d.regs);   // only what differs from the previous draw is synced again
            last = &d.regs;
            {
                const auto ti = std::chrono::steady_clock::now();
                Vulkan::g_porte_memo_idx = (u32)op.draw;
                Vulkan::PorteSetDrawIdx((int)op.draw);
                if (!inner->AccelerateDrawBatch(d.indexed)) sRefused++;
                sInnerMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ti).count();
            }
            sReplayedDraws++;
            break;
        }
        }
    }
    sReplaying = false;
    sReplaySkips += Common::g_porte_async_skips - porteSkips0;
    Vulkan::g_porte_memo_mode = 0;
    Vulkan::PorteSetDrawIdx(-1);

    pica.regs.internal = saveRegs;
    vs.uniforms = saveUni;
    if (vs.GetProgramCodeHash() != saveProgHash) vs.UpdateProgramCode(saveProg, saveProgSize);
    if (vs.GetSwizzleDataHash() != saveSwzHash) vs.UpdateSwizzleData(saveSwz, saveSwzSize);
    vs.entry_point = saveEntry;
    vs.uniforms_dirty = true;
    if (applied != (size_t)-1) PutTables(saveTables, pica);
    pica.input_default_attributes = saveFixed;
    MarkChanged(pica, *last, saveRegs);
    sReplays++;
    const double porteMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    sReplayMs += porteMs;
    PorteLog::ReplayHook(t, porteMs, (int)(sMixed - porteMixed0), (int)(sJumps - porteJumps0));
}

class Recorder : public VideoCore::RasterizerInterface
{
public:
    VideoCore::RasterizerInterface* innerPtr = nullptr;
    bool inTriangles = false;

    // the renderer is recreated when a state is loaded: always forward to the current one
    VideoCore::RasterizerInterface* Inner()
    {
        VideoCore::RasterizerInterface* real = Core::System::GetInstance().GPU().Renderer().Rasterizer();
        static unsigned long long lastGen = Core::g_porte_load_gen;   // (a state load: a new renderer, maybe at the old address)
        if (real != innerPtr || Core::g_porte_load_gen != lastGen)
        {
            lastGen = Core::g_porte_load_gen;
            innerPtr = real;
            for (auto& L : sLists) L.Clear();
            sNewPicture = false;
            sRestore = nullptr;
            sDraws.clear();
            sIndex = 0;
        }
        return innerPtr;
    }

    void AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1, const Pica::OutputVertex& v2) override
    {
        inTriangles = true;
        if (!sReplaying && (sTest || sInterp) && swPending.size() < 3 * 65536) { swPending.push_back(v0); swPending.push_back(v1); swPending.push_back(v2); }
        Inner()->AddTriangle(v0, v1, v2);
    }
    void DrawTriangles() override
    {
        if (inTriangles && !sReplaying)
        {
            if (sStartAt >= 0) RecordForFile(false);
            sSkippedSw++;   // desenho pela CPU
            static const bool swOff = getenv("PORTE_INTERP_SEMCPU") != nullptr;   // (test) the old way: the picture is unsafe
            if ((sTest || sInterp) && !swOff && !swPending.empty())
            {
                // kept as finished triangles, redrawn as they are (see SwDraw)
                auto& pica = Core::System::GetInstance().GPU().PicaCore();
                FrameList& L = *sBuild;
                if (L.swsUsed == L.sws.size()) L.sws.push_back(std::make_unique<SwDraw>());
                SwDraw& w = *L.sws[L.swsUsed];
                w.regs = pica.regs.internal;
                w.verts.swap(swPending);
                if (L.snapsUsed == 0 || !SameTables(*L.snaps[L.snapsUsed - 1], pica))
                {
                    if (L.snapsUsed == L.snaps.size()) L.snaps.push_back(std::make_unique<TableSnap>());
                    TakeTables(*L.snaps[L.snapsUsed++], pica);
                }
                w.tables = L.snapsUsed - 1;
                Op op{ OP_SWDRAW };
                op.draw = L.swsUsed++;
                L.ops.push_back(op);
            }
            else sBuild->unsafe = true;   // the in-between picture would miss it (flicker): not mixed
        }
        swPending.clear();
        inTriangles = false;
        Inner()->DrawTriangles();
    }
    std::vector<Pica::OutputVertex> swPending;   // (22/11) triangles of the CPU draw being made
    bool AccelerateDrawBatch(bool is_indexed) override
    {
        VideoCore::RasterizerInterface* in = Inner();
        const bool keep = !sReplaying && (sTest || sInterp);
        if (!sReplaying)
        {
            if (sStartAt >= 0) RecordForFile(true);
            if (keep) RecordDraw(is_indexed);
        }
        const bool ok = in->AccelerateDrawBatch(is_indexed);
        static const bool swOff = getenv("PORTE_INTERP_SEMCPU") != nullptr;
        if (!ok && keep && !sBuild->ops.empty() && sBuild->ops.back().type == OP_DRAW)
        {
            sBuild->ops.pop_back();   // o Azahar recusou (vai pela CPU): fora da lista - its triangles come as OP_SWDRAW
            sBuild->drawsUsed--;
            if (swOff) sBuild->unsafe = true;
        }
        // a geometry shader draw the GPU took: its state (the geometry shader) is not recorded
        if (ok && keep && Core::System::GetInstance().GPU().PicaCore().regs.internal.pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes) sBuild->unsafe = true;
        return ok;
    }
    bool AccelerateFill(const Pica::MemoryFillConfig& c) override
    {
        VideoCore::RasterizerInterface* in = Inner();
        if ((sTest || sInterp) && !sReplaying) { Op op{ OP_FILL }; op.fill = c; sBuild->ops.push_back(op); }
        return in->AccelerateFill(c);
    }
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& c) override
    {
        VideoCore::RasterizerInterface* in = Inner();
        if ((sTest || sInterp) && !sReplaying) { Op op{ OP_TEXCOPY }; op.copy = c; sBuild->ops.push_back(op); }
        return in->AccelerateTextureCopy(c);
    }
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& c) override
    {
        VideoCore::RasterizerInterface* in = Inner();
        static int logLeft = getenv("PORTE_INTERP_LOGDT") ? atoi(getenv("PORTE_INTERP_LOGDT")) : 0;
        if (logLeft > 0 && !sReplaying)
        {
            logLeft--;
            int nd = 0, n3 = 0, nf = 0, nc = 0;
            u32 color = 0;
            for (const Op& op : sBuild->ops)
            {
                if (op.type == OP_DRAW) { nd++; if (!sBuild->draws[op.draw]->hud) n3++; color = sBuild->draws[op.draw]->regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress(); }
                else if (op.type == OP_FILL) nf++;
                else nc++;
            }
            const auto& fb = Core::System::GetInstance().GPU().PicaCore().regs.framebuffer_config;
            fprintf(stderr, "DT %08X(%ux%u) -> %08X(%ux%u) | trecho: %d desenhos (%d 3D), %d limpezas, %d cópias, alvo %08X | tela cima %08X/%08X ativo %u, baixo %08X/%08X ativo %u\n",
                    c.GetPhysicalInputAddress(), (u32)c.input_width, (u32)c.input_height, c.GetPhysicalOutputAddress(),
                    (u32)c.output_width, (u32)c.output_height, nd, n3, nf, nc, color, (u32)fb[0].address_left1, (u32)fb[0].address_left2,
                    (u32)fb[0].active_fb, (u32)fb[1].address_left1, (u32)fb[1].address_left2, (u32)fb[1].active_fb);
        }
        if ((sTest || sInterp) && !sReplaying)
        {
            // a picture ends when it is copied to a screen (LCD framebuffer); copies to anywhere else are
            // steps inside it (effect textures) and are redone with it
            const u32 out = c.GetPhysicalOutputAddress();
            const auto& fbc = Core::System::GetInstance().GPU().PicaCore().regs.framebuffer_config;
            const bool topLcd = out == fbc[0].address_left1 || out == fbc[0].address_left2;
            const bool botLcd = out == fbc[1].address_left1 || out == fbc[1].address_left2;
            if (!topLcd && !botLcd)
            {
                Op op{ OP_DT };
                op.copy = c;
                sBuild->ops.push_back(op);
                return in->AccelerateDisplayTransfer(c);
            }
            if (topLcd && WholePicture(*sBuild))
            {
                if (sTest) Replay(in, *sBuild, nullptr, 1.0f);   // etapa C: o render target sai da reexecução
                if (sInterp)
                {
                    // keep it: N-1 <- N <- this one
                    sBuild->dt = c;
                    sBuild->valid = true;
                    sBuild->gen = ++sGenCounter;
                    MarkLayout(*sBuild);
                    FrameList* old = sPrev;
                    sPrev = sCur;
                    sCur = sBuild;
                    sBuild = old;
                    sNewPicture = true;
                }
            }
            else sSkippedSeg++;
            sBuild->Clear();
        }
        return in->AccelerateDisplayTransfer(c);
    }
    void FlushAll() override { Inner()->FlushAll(); }
    void FlushRegion(PAddr a, u32 s) override { Inner()->FlushRegion(a, s); }
    void InvalidateRegion(PAddr a, u32 s) override { Inner()->InvalidateRegion(a, s); }
    void FlushAndInvalidateRegion(PAddr a, u32 s) override { Inner()->FlushAndInvalidateRegion(a, s); }
    void ClearAll(bool flush) override { Inner()->ClearAll(flush); }
    void LoadDefaultDiskResources(const std::atomic_bool& stop, const VideoCore::DiskResourceLoadCallback& cb) override
    {
        Inner()->LoadDefaultDiskResources(stop, cb);
    }
    void SwitchDiskResources(u64 title_id) override { Inner()->SwitchDiskResources(title_id); }
};

static Recorder sRecorder;

// ---------------------------------------------------------------- etapa D: o quadro do meio no VBlank
static u32 TopScreenAddress()
{
    const auto& fb = Core::System::GetInstance().GPU().PicaCore().regs.framebuffer_config[0];
    return fb.active_fb == 0 ? fb.address_left1 : fb.address_left2;
}

// PORTE_INTERP_FOTOS=<s>,<n>: from <s> seconds on, a picture of each of the next <n> VBlanks
// (<dados>/vb_<k>.raw: w, h, RGBA) - to check the in-between pictures one by one
static std::vector<u8> sShotBuf;
static int sShotsLeft = -1, sShotK = 0;
static bool sShotAsked = false, sShotDone = false;
static double sShotAt = 0;
static void ShotBefore()
{
    if (sShotsLeft < 0)
    {
        sShotsLeft = 0;
        if (const char* f = getenv("PORTE_INTERP_FOTOS"))
        {
            sShotAt = atof(f);
            if (const char* c = strchr(f, ',')) sShotsLeft = atoi(c + 1);
        }
    }
    if (sShotsLeft <= 0) return;
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count() < sShotAt) return;
    const auto layout = Layout::DefaultFrameLayout(400, 480, false, false);
    sShotBuf.assign(400 * 480 * 4, 0);
    sShotDone = false;
    sShotAsked = true;
    Core::System::GetInstance().GPU().Renderer().RequestScreenshot(sShotBuf.data(), [](bool) { sShotDone = true; }, layout);
}
static void ShotAfter()
{
    if (!sShotAsked) return;
    sShotAsked = false;
    sShotsLeft--;
    const std::string p = PorteDataDir() + "vb_" + std::to_string(++sShotK) + (sExtraShownThis ? "_meio" : "_real") + ".raw";
    if (FILE* f = fopen(p.c_str(), "wb"))
    {
        const u32 wh[2] = { 400, 480 };
        fwrite(wh, 4, 2, f);
        fwrite(sShotBuf.data(), 1, sShotBuf.size(), f);
        fclose(f);
    }
    if (!sShotDone) fprintf(stderr, "interp: foto %d sem resposta do renderizador na hora\n", sShotK);
}

// PORTE_INTERP_SEQ=<s>,<n>: from <s> seconds on, every timed picture (above 60) is saved as it is sent to the
// presenter: <dados>/seq_<k>.raw = header (w, h, format, stride, gen, kind) + t + showAt + the LCD framebuffer
// bytes (downloaded the normal way: FlushRegion, like a CPU read of VRAM). kind: 1 = mixed, 2 = real
static int sSeqLeft = -1, sSeqK = 0;
static u32 sLastGap = 0;   // VBlanks between the last two game pictures
static double sSeqAt = 0;
static void SeqShot(VideoCore::RasterizerInterface* in, u32 addr, float t, double showAt, u32 kind)
{
    if (sSeqLeft < 0)
    {
        sSeqLeft = 0;
        if (const char* f = getenv("PORTE_INTERP_SEQ"))
        {
            sSeqAt = atof(f);
            if (const char* c = strchr(f, ',')) sSeqLeft = atoi(c + 1);
        }
    }
    if (sSeqLeft <= 0) return;
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count() < sSeqAt) return;
    sSeqLeft--;
    const auto& fb = Core::System::GetInstance().GPU().PicaCore().regs.framebuffer_config[0];
    const u32 w = fb.width, h = fb.height, stride = fb.stride, size = stride * h;
    in->FlushRegion(addr, size);
    const u8* p = Core::System::GetInstance().Memory().GetPhysicalPointer(addr);
    if (!p) return;
    const std::string path = PorteDataDir() + "seq_" + std::to_string(++sSeqK) + ".raw";
    if (FILE* f = fopen(path.c_str(), "wb"))
    {
        const u32 hd[6] = { w, h, (u32)fb.color_format.Value(), stride, (u32)sCur->gen, kind | (sLastGap << 8) };
        fwrite(hd, 4, 6, f);
        fwrite(&t, 4, 1, f);
        fwrite(&showAt, 8, 1, f);
        fwrite(p, 1, size, f);
        fclose(f);
    }
}

// (20/11 noite) the game's real speed, measured like the "%" (emulated VBlanks per second of real time, every 0.5 s).
// The budget used the time between game pictures WITHOUT the single hitches - and at 3x resolution the speed was
// lost exactly in those hitches (the game waiting for the GPU behind the extra pictures): it never saw them, the
// game ran at ~96%. Now: below 98% the FPS is cut (20%, down to 90); still slow for 1.5 s at 90 -> the light 60 FPS
// mode (the in-between picture at the VBlank: 99.9% at 3x) for 20 s, doubling at each failed try (up to 2 min);
// with room (above 99.3%) it climbs back, +30 FPS a second
static double sSpeed = 1.0;
static bool sLow60 = false;
static double sHoldUntil = 0, sLowUntil = 0, sLowBack = 8;
static VideoCore::RasterizerInterface* sGraceFor = nullptr;
static double sGraceUntil = 0;   // after a new renderer (start, restart, state load): shaders and loading, not us
static void SpeedWatch()
{
    if (sTargetFps <= 60 || !sInterp) return;
    static double t0 = 0;
    static long long sys0 = 0;
    static int bad = 0, lowBad = 0;
    const double now = NowS();
    // (20/11 noite) the speed from the core's own clock - the same number the "%" on screen shows. Counting VBlank
    // callbacks said ~100% while the core's time (and the screen) said ~87% at 3x with the PC busy: the budget
    // never reacted
    const long long sys = (long long)Core::System::GetInstance().CoreTiming().GetGlobalTimeUs().count();
    if (t0 == 0 || now - t0 > 2.0 || sys < sys0) { t0 = now; sys0 = sys; return; }   // (after a pause / a state load: start again)
    if (now - t0 < 0.5) return;
    sSpeed = (double)(sys - sys0) / 1e6 / (now - t0);
    t0 = now;
    sys0 = sys;
    if (sBudget <= 0) sBudget = sTargetFps;
    if (!sAutoBudget) { sBudget = sTargetFps; sLow60 = false; bad = 0; return; }   // (Gráficos > Ajuste automático do FPS: off)
    if (now < sGraceUntil) { bad = 0; return; }
    // far below (loading an area, a scene change, a stall): slow with or without the extra pictures - not a reason
    // to cut (it put the light mode on for 20-40 s after loadings)
    // (only the first 2 s: a slowness that lasts longer counts like any other)
    static int veryLow = 0;
    if (sSpeed < 0.85 && ++veryLow <= 4) { bad = 0; return; }
    if (sSpeed >= 0.85) veryLow = 0;
    if (sSpeed < 0.98)
    {
        bad++;
        // only a LASTING lag (1.5 s) is acted on: single dips happen with or without the extra pictures, and
        // cutting for them only lowered the FPS (2x: 183 -> 114 FPS for the same speed)
        if (bad < 3) return;
        sHoldUntil = now + 3.0;
        if (sLow60)
        {
            // light mode and still slow: it is not the extra pictures (loading an area, compiling shaders) -
            // no reason to stay at 60: back to 90 and forget this try
            if (++lowBad >= 2) { sLow60 = false; sBudget = 90; sLowBack = 8; lowBad = 0; bad = 0; }
            return;
        }
        if (sBudget > 90.5) { sBudget = std::max(90.0, sBudget * 0.85); bad = 0; }
        else if (bad >= 6)
        {
            sLow60 = true;
            sLowUntil = now + sLowBack;
            sLowBack = std::min(60.0, sLowBack * 2);
            fprintf(stderr, "interp: velocidade %.0f%% mesmo a 90 FPS: modo leve (60) por %.0f s\n", sSpeed * 100, sLowUntil - now);
        }
        return;
    }
    bad = 0;
    lowBad = 0;
    if (sSpeed > 0.993 && now >= sHoldUntil)
    {
        if (sLow60)
        {
            if (now >= sLowUntil) { sLow60 = false; sBudget = 90; sHoldUntil = now + 3.0; }
        }
        else
        {
            sBudget = std::min((double)sTargetFps, sBudget + 30.0);
            if (sBudget >= sTargetFps && now - sHoldUntil > 30) sLowBack = 8;   // (steady at the target: forget the failures)
        }
    }
}

static u64 sAt60 = 0;
static u64 sAsyncDropped = 0;
static u64 sOffscreenDropped = 0;
static u64 sFxDropped = 0;   // (22/11) pairs not mixed: a new model drawn into an off-screen effect buffer   // (22/11) pairs not mixed: a draw into an off-screen buffer the picture never clears   // (22/11) pairs / pictures dropped: a draw skipped while its shader compiled
static void BeforeSwap()
{
    sExtraShownThis = false;
    sVbCount++;
    SpeedWatch();
    // the game already draws one picture per VBlank (battle, opening): nothing to put in between
    static bool lastHadPicture = false;
    const bool hasPicture = sNewPicture;
    if (hasPicture && lastHadPicture)
    {
        sNewPicture = false;
        sAt60++;
    }
    lastHadPicture = hasPicture;
    BeforeSwapMix();
    ShotBefore();
}

// (22/11) AUTOVERIFICAÇÃO (estudo/plano_interpolacao_geral.md, Princípio 1): the in-between picture at t = 0 must be the
// real picture N-1. Every <n> pairs (PORTE_INTERP_AUTOVERIF=<n>, 0 = off; default 1 = every pair) N's list is redrawn with the
// uniforms at t = 0 into the OTHER framebuffer (the one with N-1, that the game overwrites with N+1 anyway) and
// compared, at the native size (240x400, the same at 2x or 5x), with what was there. Too different -> this pair shows
// only the real picture (30 FPS, never a wrong picture) and every pair is checked until two in a row are fine again.
// Each fall goes to the log (stderr -> log avançado): when, how much, where on the screen, the game picture.
static int sAvEvery = -1, sAvCount = 0, sAvGood = 0, sAvBlock = 16;
static double sAvLimit = 0.02;
static bool sAvFalling = false;
static double sAvFallAt = 0;
static u64 sAvChecks = 0, sAvFails = 0, sAvNoOther = 0, sAvPairs30 = 0, sAvFallPairs = 0;
static double sAvMs = 0, sAvWorst = 0;
static std::vector<u8> sAvRef, sAvMix;
static bool AvRead(VideoCore::RasterizerInterface* in, u32 addr, u32 size, std::vector<u8>& out)
{
    in->FlushRegion(addr, size);
    const u8* p = Core::System::GetInstance().Memory().GetPhysicalPointer(addr);
    if (!p) return false;
    out.assign(p, p + size);
    return true;
}
static inline void AvPixel(const u8* p, u32 fmt, int& r, int& g, int& b)
{
    switch (fmt)
    {
    case 0: r = p[3]; g = p[2]; b = p[1]; break;   // RGBA8 (stored ABGR)
    case 1: r = p[2]; g = p[1]; b = p[0]; break;   // RGB8 (stored BGR)
    case 2: { const u32 v = p[0] | (p[1] << 8); r = ((v >> 11) & 31) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3; break; }
    case 3: { const u32 v = p[0] | (p[1] << 8); r = ((v >> 11) & 31) << 3; g = ((v >> 6) & 31) << 3; b = ((v >> 1) & 31) << 3; break; }
    default: { const u32 v = p[0] | (p[1] << 8); r = ((v >> 12) & 15) << 4; g = ((v >> 8) & 15) << 4; b = ((v >> 4) & 15) << 4; break; }
    }
}
// fraction of the 8x8 blocks whose mean difference (per channel) is above sAvBlock; the box of those blocks
static double AvCompare(const u8* a, const u8* b, u32 w, u32 h, u32 stride, u32 fmt, int box[4])
{
    const u32 bpp = fmt == 0 ? 4 : fmt == 1 ? 3 : 2;
    const u32 bw = w / 8, bh = h / 8;
    int bad = 0;
    box[0] = box[1] = 1 << 30; box[2] = box[3] = -1;
    for (u32 by = 0; by < bh; by++)
        for (u32 bx = 0; bx < bw; bx++)
        {
            int sum = 0;
            for (u32 y = by * 8; y < by * 8 + 8; y++)
            {
                const u8* pa = a + y * stride + bx * 8 * bpp;
                const u8* pb = b + y * stride + bx * 8 * bpp;
                for (u32 x = 0; x < 8; x++, pa += bpp, pb += bpp)
                {
                    int r1, g1, b1, r2, g2, b2;
                    AvPixel(pa, fmt, r1, g1, b1);
                    AvPixel(pb, fmt, r2, g2, b2);
                    sum += std::abs(r1 - r2) + std::abs(g1 - g2) + std::abs(b1 - b2);
                }
            }
            if (sum > sAvBlock * 64 * 3)
            {
                bad++;
                box[0] = std::min(box[0], (int)bx * 8); box[1] = std::min(box[1], (int)by * 8);
                box[2] = std::max(box[2], (int)bx * 8 + 8); box[3] = std::max(box[3], (int)by * 8 + 8);
            }
        }
    return bw && bh ? (double)bad / (bw * bh) : 0.0;
}
// true: the in-between pictures of this pair may be made; false: show only the real picture
// (22/11, tarde) ESCADA DE MODOS: a failed check no longer drops the whole picture to 30 at once. Each pair is checked in
// the mode it will be drawn with, trying the gentlest first:
//   0 = everything as usual
//   1 = draws that are NEW in N (no partner in N-1: an effect, particles appearing - the explosion) appear in the middle
//   2 = also every draw that changes steps in the middle (old values, then new); the camera still mixes (the screen still
//       turns smoothly)
//   (none matches) = the real picture only (30 FPS), as before
// A pair starts at the mode of the last one and always tries one step gentler first, so it comes back by itself. A draw
// that is "new" only because it was not paired (it would blink if hidden) fails the check in mode 1 - never used then.
// PORTE_INTERP_ESCADA=0: off (straight to 30 as before)
static int sAvMode = 0, sAvLadder = -1;
static u64 sAvModePairs[3] = { 0, 0, 0 };
static bool AvCandidate(const FrameList& L, size_t i, u8& kind)
{
    if (i >= L.pairState.size()) return false;
    const OpDraw& d = *L.draws[i];
    const u8 st = L.pairState[i];
    if (st == 1 && (!L.pairDiff[i].empty() || !L.pairRot[i].empty())) { kind = 1; return true; }
    if (st == 2 && L.pairP[i]) { kind = 1; return true; }
    if (st == 0 && !d.hud && !L.pairP[i]) { kind = 2; return true; }
    return false;
}
static void AvBuildMode(int mode)
{
    const FrameList& L = *sCur;
    sAvMask.assign(L.drawsUsed, 0);
    sAvAnySkip = false;
    if (mode <= 0) return;
    for (size_t i = 0; i < L.drawsUsed; i++)
    {
        u8 k = 0;
        if (!AvCandidate(L, i, k)) continue;
        if (k == 2) { sAvMask[i] = 2; sAvAnySkip = true; }
        else if (mode >= 2) sAvMask[i] = 1;
    }
}
// one check: N's list at t = 0 (with the current mask) into N-1's framebuffer, compared with the reference
static double AvTest(VideoCore::RasterizerInterface* in, u32 other, u32 w, u32 h, u32 stride, u32 fmt, u32 size, int box[4])
{
    Replay(in, *sCur, sPrev, 0.0f, 0);
    Pica::DisplayTransferConfig dt = sCur->dt;
    dt.output_address = sPrev->dt.output_address;
    sReplaying = true;
    in->AccelerateDisplayTransfer(dt);
    sReplaying = false;
    sAvChecks++;
    box[0] = box[1] = box[2] = box[3] = 0;
    if (!AvRead(in, other, size, sAvMix)) return 0.0;
    return AvCompare(sAvRef.data(), sAvMix.data(), w, h, stride, fmt, box);
}
// true: the in-between pictures of this pair may be made; false: show only the real picture
static bool AutoCheck(VideoCore::RasterizerInterface* in, FrameList* real)
{
    if (sAvEvery < 0)
    {
        // (22/11) OFF by default at the user's request: it dropped effect scenes (explosion, Z moves) to 30 too often;
        // PORTE_INTERP_AUTOVERIF=1 turns it back on (with the ladder of modes)
        sAvEvery = getenv("PORTE_INTERP_AUTOVERIF") ? std::max(0, atoi(getenv("PORTE_INTERP_AUTOVERIF"))) : 0;
        if (const char* v = getenv("PORTE_INTERP_AUTOVERIF_LIMITE")) sAvLimit = atof(v);
        if (const char* v = getenv("PORTE_INTERP_AUTOVERIF_BLOCO")) sAvBlock = std::max(1, atoi(v));
        sAvLadder = !(getenv("PORTE_INTERP_ESCADA") && atoi(getenv("PORTE_INTERP_ESCADA")) == 0);
        fprintf(stderr, "interp: autoverificação %s (1 par em %d, limite %.1f%% dos blocos, bloco %d), escada de modos %s\n", sAvEvery ? "ligada" : "desligada",
                sAvEvery, sAvLimit * 100, sAvBlock, sAvLadder ? "ligada" : "desligada");
    }
    AvBuildMode(sAvLadder ? sAvMode : 0);
    if (!sAvEvery) return true;
    // (every pair by default: 1 in 4 still let 1 bad pair through in the Charizard flight; the game falling behind
    // (budget under the target, light 60 mode) -> at most 1 in 2, the extra pictures come first)
    const int every = (sBudget > 0 && sBudget < sTargetFps - 0.5) || sLow60 ? std::max(sAvEvery, 2) : sAvEvery;
    if (!sAvFalling && sAvMode == 0 && ++sAvCount < every) return true;
    sAvCount = 0;
    // the test picture goes into N-1's framebuffer (what is there is the reference): when the screen shows N it is the
    // hidden one (the game overwrites it with N+1); when the screen still shows N-1 the in-between pictures go there
    // anyway, and if this pair is refused N-1 is redrawn there exactly (below)
    const u32 other = sPrev->dt.GetPhysicalOutputAddress();
    if (other == sCur->dt.GetPhysicalOutputAddress()) { sAvNoOther++; return !sAvFalling; }
    const auto t0 = std::chrono::steady_clock::now();
    const auto& fb = Core::System::GetInstance().GPU().PicaCore().regs.framebuffer_config[0];
    const u32 w = fb.width, h = fb.height, stride = fb.stride, fmt = (u32)fb.color_format.Value(), size = stride * h;
    if (!w || !h || stride < w * 2 || size > 4u << 20) return true;
    if (!AvRead(in, other, size, sAvRef)) return true;
    // the modes to try: one gentler than the last one first, then up the ladder
    int order[4], no = 0;
    const int maxMode = sAvLadder ? 2 : 0;
    const int start = std::min(sAvMode, maxMode);
    if (start > 0) order[no++] = start - 1;
    for (int m = start; m <= maxMode; m++) order[no++] = m;
    int box[4] = { 0, 0, 0, 0 };
    double diff = 1.0, first = -1;
    bool good = false;
    for (int k = 0; k < no && !good; k++)
    {
        AvBuildMode(order[k]);
        int b[4];
        const double dk = AvTest(in, other, w, h, stride, fmt, size, b);
        if (first < 0) { first = dk; memcpy(box, b, sizeof(box)); }
        diff = dk;
        if (dk <= sAvLimit)
        {
            good = true;
            if (order[k] != sAvMode)
            {
                static int said = 0;
                if (said++ < 200) fprintf(stderr, "interp: autoverificação: modo %d -> %d (imagem %llu, %.1f%%)\n", sAvMode, order[k], (unsigned long long)sCur->gen, dk * 100);
            }
            sAvMode = order[k];
        }
    }
    // (diagnosis) PORTE_INTERP_AVDIAG=1: on a failure, which draws must not be mixed for t = 0 to match (search inside
    // this pair, many checks: a hitch - tests only); each culprit is printed with what changes in it
    static const bool avDiag = getenv("PORTE_INTERP_AVDIAG") != nullptr;
    if (!good && avDiag)
    {
        std::vector<size_t> S;
        for (size_t i = 0; i < sCur->drawsUsed; i++) { u8 k = 0; if (AvCandidate(*sCur, i, k)) S.push_back(i); }
        auto testSet = [&](const std::vector<size_t>& set) {
            sAvMask.assign(sCur->drawsUsed, 0);
            sAvAnySkip = false;
            for (size_t i : set) { u8 k = 0; AvCandidate(*sCur, i, k); sAvMask[i] = k; }
            int b[4];
            return AvTest(in, other, w, h, stride, fmt, size, b);
        };
        const double dAll = testSet(S);
        fprintf(stderr, "AVDIAG imagem %llu: %.1f%% normal, %.1f%% com os %zu candidatos sem mistura\n", (unsigned long long)sCur->gen, first * 100, dAll * 100, S.size());
        if (dAll <= sAvLimit)
        {
            int tests = 0;
            for (size_t chunk = std::max<size_t>(1, S.size() / 2); chunk >= 1 && tests < 80; chunk /= 2)
            {
                for (size_t pos = 0; pos < S.size() && tests < 80;)
                {
                    std::vector<size_t> T;
                    for (size_t k = 0; k < S.size(); k++) if (k < pos || k >= pos + chunk) T.push_back(S[k]);
                    tests++;
                    if (!T.empty() && testSet(T) <= sAvLimit) S = T; else pos += chunk;
                }
                if (chunk == 1) break;
            }
            for (size_t i : S)
            {
                const OpDraw& d = *sCur->draws[i];
                std::string rows;
                int shown = 0;
                for (const u16 c : sCur->pairDiff[i])
                    if (shown++ < 24) { char x[48]; snprintf(x, sizeof(x), " c%d.%d %.3g>%.3g", c >> 2, c & 3, sCur->pairP[i] ? sCur->pairP[i]->uf[c >> 2][c & 3] : 0.f, d.uf[c >> 2][c & 3]); rows += x; }
                std::string rot;
                for (const RotGroup& g : sCur->pairRot[i]) { char x[24]; snprintf(x, sizeof(x), " r%d%s", g.r, g.step ? "*" : ""); rot += x; }
                fprintf(stderr, "AVDIAG   culpado d%zu mesh %08X nv %u estado %d gfl2 %d lyt %d hud %d prog %llX tex %08X par %s | linhas:%s | giros:%s\n", i, d.mesh,
                        (u32)d.regs.pipeline.num_vertices, (int)sCur->pairState[i], (int)Gfl2Layout(d), (int)d.lyt, (int)d.hud, (unsigned long long)d.prog,
                        d.regs.texturing.texture0.GetPhysicalAddress(), sCur->pairP[i] ? "sim" : "não", rows.substr(0, 600).c_str(), rot.substr(0, 200).c_str());
                // who draws INTO the texture it reads (an earlier pass of the same picture)?
                const u32 tex = d.regs.texturing.texture0.GetPhysicalAddress();
                for (size_t j = 0; j < sCur->drawsUsed; j++)
                {
                    const OpDraw& e = *sCur->draws[j];
                    const u32 cb = e.regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress();
                    if (cb == tex || (cb <= tex && tex < cb + 0x80000))
                        fprintf(stderr, "AVDIAG     escreve na textura dele: d%zu mesh %08X nv %u estado %d gfl2 %d prog %llX cor %08X\n", j, e.mesh,
                                (u32)e.regs.pipeline.num_vertices, (int)sCur->pairState[j], (int)Gfl2Layout(e), (unsigned long long)e.prog, cb);
                }
                fprintf(stderr, "AVDIAG     ele desenha em %08X (tela: %08X)\n", d.regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress(),
                        sCur->dt.GetPhysicalInputAddress());
            }
        }
    }
    if (!good) { sAvMode = 0; AvBuildMode(0); diff = first; }
    else sAvModePairs[sAvMode]++;
    sAvMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    sAvWorst = std::max(sAvWorst, first);
    if (!good)
    {
        sAvFails++;
        if (!sAvFalling)
        {
            sAvFalling = true;
            sAvFallAt = NowS();
            sAvFallPairs = 0;
            // (the LCD framebuffer is stored turned: x = 0..239 is the screen's height, y = 0..399 its width)
            fprintf(stderr, "interp: AUTOVERIFICAÇÃO: imagem do meio em t=0 difere da real (%.1f%% dos blocos, área tela x %d-%d y %d-%d, "
                            "imagem %llu, %zu desenhos) -> 30 FPS até bater de novo\n",
                    diff * 100, box[1], box[3], (int)w - box[2], (int)w - box[0], (unsigned long long)sCur->gen, sCur->drawsUsed);
        }
        sAvGood = 0;
    }
    else if (sAvFalling)
    {
        sAvFalling = false;
        fprintf(stderr, "interp: autoverificação: voltou a bater depois de %llu pares a 30 (%.1f s)\n", (unsigned long long)sAvFallPairs, NowS() - sAvFallAt);
    }
    if (sAvFalling)
    {
        sAvPairs30++;
        sAvFallPairs++;
        if (real == sPrev)
        {
            Replay(in, *sPrev, nullptr, 1.0f, 0);   // (the screen shows this framebuffer: N-1 exactly again)
            sReplaying = true;
            in->AccelerateDisplayTransfer(sPrev->dt);
            sReplaying = false;
        }
        return false;
    }
    return true;
}

// (22/11) a draw into a buffer OFF the screen that the picture never clears / fills / copies into before (the Charizard
// flight's flash: the character drawn into a separate buffer for its glow, then laid over the scene): every redraw adds
// to what the last one left there instead of starting from what the game had - the glow saturated and the character came
// out black in every in-between picture. Such a picture cannot be redrawn faithfully: its pairs show the real pictures
static bool OffscreenUncleared(const FrameList& L)
{
    const u32 screen = L.dt.GetPhysicalInputAddress();
    u32 lo[64], hi[64];
    int n = 0;
    for (const Op& op : L.ops)
    {
        if (op.type == OP_FILL && n < 64) { lo[n] = op.fill.GetStartAddress(); hi[n] = op.fill.GetEndAddress(); n++; }
        else if ((op.type == OP_TEXCOPY || op.type == OP_DT) && n < 64) { lo[n] = op.copy.GetPhysicalOutputAddress(); hi[n] = lo[n] + 1; n++; }
        else if (op.type == OP_DRAW)
        {
            const OpDraw& d = *L.draws[op.draw];
            const u32 cb = d.regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress();
            auto isCleared = [&](u32 addr) {
                for (int k = 0; k < n; k++) if (addr >= lo[k] && addr < std::max(hi[k], lo[k] + 1)) return true;
                return false;
            };
            // the stencil / depth it writes must start clean too: an effect that counts in the stencil (the flash's glow)
            // added up at every redraw - the 1st redraw right, the 2nd lost the glow, the next ones a black character
            const auto& om = d.regs.framebuffer.output_merger;
            const u32 db = d.regs.framebuffer.framebuffer.GetDepthBufferPhysicalAddress();
            if (db && (om.stencil_test.enable || om.depth_write_enable) && !isCleared(db)) return true;
            if (cb == screen || d.hud) continue;
            if (!isCleared(cb)) return true;
        }
    }
    return false;
}

static void BeforeSwapMix()
{
    if (!sNewPicture) return;   // only the first VBlank after a new picture (the second shows the real one)
    sNewPicture = false;
    // how often the game delivers pictures (normally 1/30 s)
    const double now = NowS();
    // emulated time between the two pictures (VBlanks / 60) and the real time it took
    u64 gap = 0;
    if (sLastPicVb > 0)
    {
        gap = sVbCount - sLastPicVb;
        sLastGap = (u32)gap;
        if (gap >= 1 && gap <= 6) sPeriodEmu = sPeriodEmu * 0.8 + (gap / 60.0) * 0.2;
    }
    sLastPicVb = sVbCount;
    if (sLastPicTime > 0)
    {
        const double p = now - sLastPicTime;
        // a single hitch (loading an area, compiling a shader) is not the speed of the game: it does not enter the
        // average (it kept the budget low for seconds); three in a row are (the game really is slow)
        static int hitches = 0;
        const double expect = gap >= 1 && gap <= 6 ? gap / 60.0 : sPeriodEmu;
        const bool hitch = p > expect * 1.5;
        hitches = hitch ? hitches + 1 : 0;
        if (p > 1.0 / 70 && p < 0.2 && (!hitch || hitches >= 3)) sPeriod = sPeriod * 0.8 + p * 0.2;
    }
    sLastPicTime = now;
    sTimedUntil = 0;   // (if nothing is generated below, the normal pictures show)
    // (22/11) the GAME's own picture skipped draws (their shader was still compiling: the first time a scene / effect shows
    // up, Azahar draws it without them - the Charizard flight's first pictures came out black / grey). The redraws, a moment
    // later, already have the shader and drew the whole scene: the screen alternated black and scene at 200 FPS. The pairs
    // with such a picture (this one and the next) show only the real pictures
    {
        static unsigned long long lastGame = 0;
        static int hold = 0;
        const unsigned long long game = Common::g_porte_async_skips - sReplaySkips;
        if (game != lastGame) { lastGame = game; hold = 2; }
        if (hold > 0) { hold--; sAsyncDropped++; return; }
    }
    // (22/11) OFF by default: battles draw into buffers they never clear and EVERY battle pair was dropped (30 FPS);
    // the flight was fixed by the new-effect-pass rule. PORTE_INTERP_FORADATELA=1 turns this check on (test)
    static const bool offOk = !(getenv("PORTE_INTERP_FORADATELA") && atoi(getenv("PORTE_INTERP_FORADATELA")) == 1);
    if (!offOk && sCur->valid && sPrev->valid && (OffscreenUncleared(*sCur) || OffscreenUncleared(*sPrev))) { sOffscreenDropped++; return; }
    if (!sCur->valid || !sPrev->valid) return;
    // pictures that cannot be redrawn faithfully, and stereoscopic 3D (only the left eye would be mixed)
    if (sCur->unsafe || sPrev->unsafe) { sUnsafe++; return; }
    // (22/11) the game already rewrote what a draw of N reads: no in-between pictures for this pair (the real picture
    // shows) - only with PORTE_INTERP_MEMCHECK=1 (diagnosis)
    if (sMemCheck)
    {
        u64 changed = 0;
        for (size_t i = 0; i < sCur->drawsUsed; i++)
        {
            const OpDraw& d = *sCur->draws[i];
            if (d.nmr && HashRanges(d.mr, d.nmr) != d.memHash) changed++;
            else if (d.ntr && HashRanges(d.tr, d.ntr) != d.texHash) { changed++; sTexChangedDraws++; }
        }
        if (changed) { sMemChangedPairs++; sMemChangedDraws += changed; return; }
    }
    if (Settings::values.render_3d.GetValue() != Settings::StereoRenderOption::Off) return;
    // the game must be idle (waiting for the VBlank): nothing of the next picture drawn yet
    bool idle = true;
    for (const Op& op : sBuild->ops)
        if (op.type == OP_DRAW) { idle = false; break; }
    if (!idle) { sNotIdle++; return; }
    // which picture is on the screen now: the framebuffer the LCD reads is where N or N-1 was copied
    const u32 shown = TopScreenAddress();
    FrameList* real = nullptr;
    if (sCur->dt.GetPhysicalOutputAddress() == shown) real = sCur;
    else if (sPrev->dt.GetPhysicalOutputAddress() == shown) real = sPrev;
    if (!real) { sNoAddr++; return; }
    VideoCore::RasterizerInterface* in = sRecorder.Inner();
    if (sTargetFps > 60) SeqShot(in, shown, 1.0f, 0, 3);   // (test: the real picture N, before anything is redrawn)

    // above 60: K pictures per game picture, each with its time (the presenter holds them until then):
    // t = 1/K .. (K-1)/K mixed between N-1 and N, then N itself at the end of the period
    VideoCore::RendererBase* rv = sTargetFps > 60 && !sLow60 ? &Core::System::GetInstance().GPU().Renderer() : nullptr;
    if (rv)
    {
        // a fixed grid of 1/fps: each picture at a grid instant, with the t of that instant; the periods of
        // consecutive game pictures follow each other (N-1 -> N during (T0, T0 + P]): exactly fps on average
        // nothing moves between N-1 and N (standing still, camera still): the in-between pictures would all be
        // the real one - nothing is redrawn, the normal pictures show (saves the whole cost)
        EnsurePairs(*sCur, *sPrev);
        // (22/11) a MODEL (hundreds of vertices) with no partner in N-1, drawn into a buffer off the screen: an effect pass
        // that just started (the Charizard flight's flash: the character drawn again into a glow buffer, with another
        // shader). Its redraws do not come out like the game's (the glow lost, the character black in every in-between
        // picture) - this pair shows the real pictures. PORTE_INTERP_PASSEFX=1: mixes it anyway (test)
        static const bool fxOk = getenv("PORTE_INTERP_PASSEFX") != nullptr;
        if (!fxOk)
        {
            const u32 screen = sCur->dt.GetPhysicalInputAddress();
            for (size_t i = 0; i < sCur->drawsUsed && i < sCur->pairState.size(); i++)
            {
                const OpDraw& d = *sCur->draws[i];
                if (!sCur->pairP[i] && !d.hud && d.regs.pipeline.num_vertices >= 300 && Gfl2Layout(d) &&
                    d.regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress() != screen)
                { sFxDropped++; sSeqEnd = 0; return; }
            }
        }
        bool moves = false;
        for (size_t i = 0; i < sCur->drawsUsed && !moves; i++)
            if ((sCur->pairState[i] == 1 || sCur->pairState[i] == 3) && (!sCur->pairDiff[i].empty() || !sCur->pairRot[i].empty())) moves = true;   // (a jumped draw is shown as it is)
        if (!moves) { sStill++; sSeqEnd = 0; return; }
        if (!AutoCheck(in, real)) { sSeqEnd = 0; return; }   // (22/11: t = 0 is not N-1 -> only the real picture)
        // (test) PORTE_INTERP_FIEL=1: N redrawn as it is (no mixing) and saved (seq kind 6) - is the redraw itself faithful?
        static const bool fiel = getenv("PORTE_INTERP_FIEL") != nullptr;
        if (fiel)
        {
            Replay(in, *sCur, nullptr, 1.0f, 0);
            sReplaying = true;
            in->AccelerateDisplayTransfer(real->dt);
            sReplaying = false;
            SeqShot(in, real->dt.GetPhysicalOutputAddress(), 1.0f, 0, 6);
            // which uniform of which draw is far from the current value at t = 0.999 (should be ~0)?
            for (size_t i = 0; i < sCur->drawsUsed; i++)
            {
                Pica::Uniforms um;
                MixUniforms(*sCur, sPrev, 0.999f, i, um, false);
                const OpDraw& d = *sCur->draws[i];
                for (int r = 0; r < 96; r++)
                    for (int k = 0; k < 4; k++)
                    {
                        const float a = um.f[r][k].ToFloat32(), b = d.uni.f[r][k].ToFloat32();
                        if (std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) > 0.05f * std::max(1.0f, std::fabs(b)))
                            fprintf(stderr, "FIEL g%llu d%zu mesh %08X nv %u st %d prog %llX c%d.%d misturado %.4g atual %.4g antigo %.4g\n", (unsigned long long)sCur->gen, i, d.mesh,
                                    (u32)d.regs.pipeline.num_vertices, (int)sCur->pairState[i], (unsigned long long)d.prog, r, k, a, b,
                                    sCur->pairP[i] ? sCur->pairP[i]->uf[r][k] : 0.0f);
                    }
            }
            // and t = 0.001 mixed: the in-between machinery with (almost) nothing mixed yet (kind 7)
            Replay(in, *sCur, sPrev, 0.999f, 0);
            sReplaying = true;
            in->AccelerateDisplayTransfer(real->dt);
            sReplaying = false;
            SeqShot(in, real->dt.GetPhysicalOutputAddress(), 0.999f, 0, 7);
        }
        // budget: the emulation must stay at full speed; when the game falls behind real time the number of
        // pictures goes down, and back up to the target when there is room again
        const double lag = sPeriodEmu > 0 ? sPeriod / sPeriodEmu : 1.0;
        if (sBudget <= 0) sBudget = sTargetFps;
        // down only when the game stays behind (3 pictures in a row), by 10%; back up fast (a quarter of what is
        // missing per game picture: 60 -> 200 in ~0,3 s) - the pictures are cheap now (gravar e repetir)
        (void)lag;   // (the budget follows the game's real speed: SpeedWatch, every VBlank)
        const double P = sPeriod;
        double T0 = now + 0.004;   // (a little room: the first ones are drawn right now)
        if (sSeqEnd > 0 && std::fabs(T0 - sSeqEnd) < P * 0.5) T0 = sSeqEnd;   // continue where the last one ended
        sSeqEnd = T0 + P;
        // (test) PORTE_INTERP_VARRER=<s>: once, after <s> seconds, this pair is redrawn at t = 0, 0.05 .. 1 and
        // each one saved (seq_*.raw, kind 4) - to see if the mixing itself moves things back and forth
        static double sweepAt = getenv("PORTE_INTERP_VARRER") ? atof(getenv("PORTE_INTERP_VARRER")) : -1;
        if (sweepAt >= 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count() >= sweepAt)
        {
            sweepAt = -1;
            if (sSeqLeft < 0) { sSeqLeft = 0; }
            const bool rep = getenv("PORTE_INTERP_REPETIR") != nullptr;
            const bool two = !sNoMemo || rep;
            sSeqLeft += two ? 42 : 21;
            sSeqAt = 0;
            // (2nd pass, same t: with the reuse on, the reuse; with PORTE_INTERP_REPETIR, record + repeat -> kind 5)
            for (int pass = 0; pass < (two ? 2 : 1); pass++)
                for (int j = 0; j <= 20; j++)
                {
                    const float tt = j / 20.0f;
                    bool done = false;
                    if (rep && pass == 1 && j > 0)
                    {
                        const size_t n = sCur->drawsUsed;
                        if (sRepUni.size() < n) sRepUni.resize(n);
                        sRepPtr.assign(n, nullptr);
                        for (size_t i = 0; i < n; i++) { MixUniforms(*sCur, sPrev, tt, i, sRepUni[i], false); sRepPtr[i] = &sRepUni[i]; }
                        done = Vulkan::PorteRepeat(in, sRepPtr.data(), n);
                        if (!done) fprintf(stderr, "varredura: repetição recusada em t=%.2f\n", tt);
                    }
                    if (!done && rep && pass == 1 && j == 0)
                    {
                        Vulkan::PorteCaptureBegin(in);
                        Replay(in, *sCur, sPrev, tt, 0);
                        fprintf(stderr, "varredura: gravação %s\n", Vulkan::PorteCaptureEnd(in) ? "boa" : "recusada");
                        done = true;
                    }
                    if (!done) Replay(in, *sCur, sPrev, tt, pass == 0 ? 0 : (j == 0 ? 1 : 2));
                    sReplaying = true;
                    in->AccelerateDisplayTransfer(real->dt);
                    sReplaying = false;
                    SeqShot(in, real->dt.GetPhysicalOutputAddress(), tt, 0, pass == 0 ? 4 : 5);
                }
        }
        const double step = 1.0 / sBudget;
        int made = 0, drawn = 0;
        for (double g = std::floor(T0 / step + 1.0) * step; g <= T0 + P + 1e-6 && made < 16; g += step, made++)
        {
            const float t = (float)((g - T0) / P);
            // never make the game wait for a frame: with fewer than 3 free (one stays for the real picture) the
            // in-between picture is skipped; the last one (the game's own picture N) is always shown
            if (t < 0.999f && Vulkan::PorteFreeFrames() < 3) { sSkippedFree++; continue; }
            const int memo = drawn == 0 ? 1 : 2;   // the first one notes the texture lookups, the others reuse them
            // "gravar e repetir" (on; PORTE_INTERP_REPETIR=0 turns it off): the 1st picture of the sequence is recorded; the others
            // send the same commands again with their own uniforms (no work of the Azahar per draw)
            static const bool repeatOn = !(getenv("PORTE_INTERP_REPETIR") && atoi(getenv("PORTE_INTERP_REPETIR")) == 0);   // (on by default; =0 turns it off)
            bool repeated = false;
            if (repeatOn && drawn > 0 && sCapOk && !sAvAnySkip && sCur->swsUsed == 0)
            {
                const auto tr = std::chrono::steady_clock::now();
                const size_t n = sCur->drawsUsed;
                if (sRepUni.size() < n) sRepUni.resize(n);
                sRepPtr.assign(n, nullptr);
                for (size_t i = 0; i < n; i++)
                {
                    MixUniforms(*sCur, t >= 0.999f ? nullptr : sPrev, t, i, sRepUni[i], false);
                    sRepPtr[i] = &sRepUni[i];
                }
                repeated = Vulkan::PorteRepeat(in, sRepPtr.data(), n);
                if (repeated)
                {
                    sRepeats++;
                    sRepeatMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tr).count();
                }
                else { sRepeatStopped++; sCapOk = false; }
            }
            const unsigned long long asyncSkips0 = Common::g_porte_async_skips;
            if (!repeated)
            {
                const bool capture = repeatOn && drawn == 0 && !sAvAnySkip && sCur->swsUsed == 0;
                if (capture) Vulkan::PorteCaptureBegin(in);
                if (t >= 0.999f) Replay(in, *sCur, nullptr, 1.0f, memo);
                else Replay(in, *sCur, sPrev, t, memo);
                if (capture)
                {
                    sCapOk = Vulkan::PorteCaptureEnd(in);
                    if (sCapOk) sCaptures++; else sCaptureBad++;
                }
            }
            // (22/11) a draw of this redraw was skipped because its shader was still compiling (a new effect: the sky, the
            // fade, the flash of the Charizard flight) - the picture would lack it. The first one of the pair: no in-between
            // pictures for this pair (the screen keeps the real one: nothing was copied there yet); a later one: stop here
            if (Common::g_porte_async_skips != asyncSkips0)
            {
                sAsyncDropped++;
                sCapOk = false;
                if (drawn == 0) { sSeqEnd = 0; sTimedUntil = 0; return; }
                break;
            }
            sReplaying = true;
            in->AccelerateDisplayTransfer(real->dt);   // into the framebuffer the screen reads
            sReplaying = false;
            SeqShot(in, real->dt.GetPhysicalOutputAddress(), t, g, t >= 0.999f ? 2 : 1);
            sShowAtPending = g;
            const auto tp = std::chrono::steady_clock::now();
            Vulkan::PorteExtraPresent(rv);
            sPresentMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp).count();
            sPresents++;   // -> presenter, held until its time (OnSwap: TakeShowAt)
            sShowAtPending = 0;
            drawn++;
            if (t < 0.999f) sExtraShown++;
        }
        // (no putting back of the real picture: while this runs the normal VBlank pictures are not shown, and
        // the game draws its next picture in the other framebuffer)
        sTimedUntil = now + 2.5 * P;
        sLastK = made;
        return;
    }

    EnsurePairs(*sCur, *sPrev);
    if (!AutoCheck(in, real)) return;   // (22/11: t = 0 is not N-1 -> only the real picture)
    const unsigned long long asyncSkips0 = Common::g_porte_async_skips;
    Replay(in, *sCur, sPrev, 0.5f, 1);   // halfway between N-1 and N
    if (Common::g_porte_async_skips != asyncSkips0) { sAsyncDropped++; return; }   // (22/11: a shader still compiling - see above)
    sReplaying = true;
    in->AccelerateDisplayTransfer(real->dt);   // into the framebuffer on the screen
    sReplaying = false;
    sRestore = real;
    sExtraFlag = true;
    sExtraShownThis = true;
    sExtraShown++;
}

static void AfterSwap()
{
    ShotAfter();
    if (!sRestore) return;
    // put back what the game left there, exactly (the next VBlank shows the real picture)
    VideoCore::RasterizerInterface* in = sRecorder.Inner();
    Replay(in, *sRestore, nullptr, 1.0f, sRestore == sCur ? 2 : 0);
    sReplaying = true;
    in->AccelerateDisplayTransfer(sRestore->dt);
    sReplaying = false;
    sRestore = nullptr;
}

double TakeShowAt()
{
    const double v = sShowAtPending;
    sShowAtPending = 0;
    return v;
}

bool SuppressNormal() { return sTimedUntil > NowS(); }
unsigned long long VblankCount() { return sVbCount; }

bool TakeExtraFlag()
{
    const bool f = sExtraFlag;
    sExtraFlag = false;
    return f;
}

// the menu's choice (Gráficos > Fluidez): 0 = off, 60..400 = interpolate to that many FPS, -1 = not chosen
// (then only the PORTE_INTERP* variables count). Applied when the renderer is (re)created: game start,
// restart, state load -- never in the middle of a running renderer (no Vulkan objects change under it)
static int sCfgFps = -1;
void Configure(int fps) { sCfgFps = fps; }

void SetPhoto(const Photo& p) { sPhoto = p; }

// Modo foto: the picture on the screen is redrawn with the photo camera and shown (the game is paused meanwhile)
bool PhotoFrame()
{
    if (!sRecorder.innerPtr || !sCur->valid) return false;
    VideoCore::RasterizerInterface* in = sRecorder.Inner();
    if (!sCur->valid) return false;   // (a new renderer: nothing recorded yet)
    const u32 shown = TopScreenAddress();
    FrameList* real = sCur;
    if (sCur->dt.GetPhysicalOutputAddress() != shown && sPrev->valid && sPrev->dt.GetPhysicalOutputAddress() == shown) real = sPrev;
    Replay(in, *real, nullptr, 1.0f, 0);
    sReplaying = true;
    in->AccelerateDisplayTransfer(real->dt);
    sReplaying = false;
    sShowAtPending = 0;
    sExtraFlag = true;   // (the presenter shows only "new" pictures: with the game paused nothing else says so)
    Vulkan::PorteExtraPresent(&Core::System::GetInstance().GPU().Renderer());
    return true;
}
bool PhotoAvailable() { return sRecorder.innerPtr && sCur->valid; }
void SetAutoBudget(bool on) { sAutoBudget = on; }

void Install()
{
    static const bool envOn = getenv("PORTE_INTERP_GRAVAR") != nullptr || getenv("PORTE_INTERP_TESTE") != nullptr ||
                              (getenv("PORTE_INTERP") != nullptr && atoi(getenv("PORTE_INTERP")) != 0);   // (=0: off, nothing hooked)
    static const bool envInterp = getenv("PORTE_INTERP") != nullptr;   // (the variables win over the menu)
    auto& gpu = Core::System::GetInstance().GPU();
    VideoCore::RasterizerInterface* const ras = gpu.Renderer().Rasterizer();
    static VideoCore::RasterizerInterface* sAppliedFor = nullptr;
    if (ras && ras != sAppliedFor)
    {
        sAppliedFor = ras;
        if (!envInterp && sCfgFps >= 0)
        {
            sInterp = sCfgFps > 0 ? 1 : 0;
            sTargetFps = std::clamp(sCfgFps > 0 ? sCfgFps : 60, 60, 400);
        }
        if (!sInterp && !sTest)
        {
            // turned off (by the menu) for this new renderer: nothing of ours stays hooked
            if (VideoCore::g_porte_rasterizer_override == &sRecorder) VideoCore::g_porte_rasterizer_override = nullptr;
            if (VideoCore::g_porte_before_swap == BeforeSwap) VideoCore::g_porte_before_swap = nullptr;
            if (VideoCore::g_porte_after_swap == AfterSwap) VideoCore::g_porte_after_swap = nullptr;
        }
    }
    if (!envOn && !sInterp) return;
    if (sStartAt < 0 && getenv("PORTE_INTERP_GRAVAR")) sStartAt = atof(getenv("PORTE_INTERP_GRAVAR"));
    if (getenv("PORTE_INTERP_TESTE")) sTest = atoi(getenv("PORTE_INTERP_TESTE"));
    if (envInterp) sInterp = atoi(getenv("PORTE_INTERP"));
    sProf = getenv("PORTE_INTERP_PERFIL") != nullptr;
    // reuse of texture lookups: OFF (it made the GPU driver reset on the user's PC); only for tests
    sNoMemo = getenv("PORTE_INTERP_MEMO_TESTE") == nullptr;
    if (getenv("PORTE_INTERP_FPS")) sTargetFps = std::clamp(atoi(getenv("PORTE_INTERP_FPS")), 60, 400);
    if (!ras) return;
    sRecorder.Inner();   // renderizador novo (estado carregado, reinício): esquece o gravado
    if (ras != sGraceFor) { sGraceFor = ras; sGraceUntil = NowS() + 6.0; sLow60 = false; sBudget = 0; }
    gpu.PicaCore().BindRasterizer(&sRecorder);
    if (sTest || sInterp) VideoCore::g_porte_rasterizer_override = &sRecorder;   // limpezas e transferências também
    if (sInterp)
    {
        VideoCore::g_porte_before_swap = BeforeSwap;
        VideoCore::g_porte_after_swap = AfterSwap;
    }
}

void OnFrameEnd(bool newImage)
{
    if (!sRecorder.innerPtr) return;
    static auto last = std::chrono::steady_clock::now();
    if ((sTest || sInterp) && std::chrono::steady_clock::now() - last > std::chrono::seconds(1))
    {
        last = std::chrono::steady_clock::now();
        fprintf(stderr,
                "interp: %llu quadros do meio mostrados (jogo ocupado %llu, framebuffer desconhecido %llu); %llu reexecuções "
                "(%.2f ms cada), %llu desenhos misturados, %llu sem par, %llu com salto (não misturados), %llu texturas com salto, "
                "%llu recusados, %llu pela CPU, %llu trechos 2D, %llu VBlanks com o jogo já a 60, %llu quadros inseguros; imagens por quadro=%d (período %.1f ms), %.2f ms por reexecução dentro do Azahar, orçamento %.0f FPS, %llu quadros parados, %llu matrizes giradas (slerp)\n",
                (unsigned long long)sExtraShown, (unsigned long long)sNotIdle, (unsigned long long)sNoAddr,
                (unsigned long long)sReplays, sReplays ? sReplayMs / sReplays : 0.0, (unsigned long long)sMixed,
                (unsigned long long)sUnpaired, (unsigned long long)sJumps, (unsigned long long)sTexSnaps, (unsigned long long)sRefused, (unsigned long long)sSkippedSw,
                (unsigned long long)sSkippedSeg, (unsigned long long)sAt60, (unsigned long long)sUnsafe, sLastK, sPeriod * 1000, sReplays ? sInnerMs / sReplays : 0.0, sBudget, (unsigned long long)sStill, (unsigned long long)sRotMixed);
        sRotMixed = 0;
        if (sTargetFps > 60) fprintf(stderr, "interp: velocidade %.1f%%, orçamento %.0f FPS%s\n", sSpeed * 100, sBudget, sLow60 ? " (modo leve 60)" : "");
        if (sMemChangedPairs) fprintf(stderr, "interp: memória dos vértices mudou antes de redesenhar: %llu pares sem imagens do meio (%llu desenhos, %llu por textura)\n",
                                      (unsigned long long)sMemChangedPairs, (unsigned long long)sMemChangedDraws, (unsigned long long)sTexChangedDraws);
        sMemChangedPairs = sMemChangedDraws = sTexChangedDraws = 0;
        if (!sNoMemo)
        {
            fprintf(stderr, "interp: buscas de textura reaproveitadas %llu, refeitas %llu (sem anotação %llu, superfície mudou %llu)%s", (unsigned long long)Vulkan::g_porte_memo_hit,
                    (unsigned long long)(Vulkan::g_porte_memo_miss + Vulkan::g_porte_memo_stale), (unsigned long long)Vulkan::g_porte_memo_miss, (unsigned long long)Vulkan::g_porte_memo_stale, "\n");
            Vulkan::g_porte_memo_hit = Vulkan::g_porte_memo_miss = Vulkan::g_porte_memo_stale = 0;
        }
        if (sRepeats || sCaptures || sCaptureBad)
            fprintf(stderr, "interp: gravar e repetir: %llu gravações boas, %llu recusadas; %llu repetições (%.3f ms cada), %llu paradas%s",
                    (unsigned long long)sCaptures, (unsigned long long)sCaptureBad, (unsigned long long)sRepeats,
                    sRepeats ? sRepeatMs / sRepeats : 0.0, (unsigned long long)sRepeatStopped, "\n");
        sRepeats = sRepeatStopped = sCaptures = sCaptureBad = 0;
        sRepeatMs = 0;
        if (sPresents) fprintf(stderr, "interp: %.3f ms por apresentação extra (%llu), %llu puladas sem imagem livre\n", sPresentMs / sPresents, (unsigned long long)sPresents, (unsigned long long)sSkippedFree);
        sSkippedFree = 0;
        sPresentMs = 0;
        sPresents = 0;
        if (sCutRot || sCutMove || sCutScale || sCutRow || sCamCuts || sTurnArounds) fprintf(stderr, "interp: motivos de salto: giro %llu, translação %llu, escala %llu, linhas que ficam no valor novo %llu, cortes de câmera %llu (%llu por zoom, %llu por salto, câmera: %llu giro, %llu salto), viradas bruscas %llu\n", (unsigned long long)sCutRot, (unsigned long long)sCutMove, (unsigned long long)sCutScale, (unsigned long long)sCutRow, (unsigned long long)sCamCuts, (unsigned long long)sCutZoom, (unsigned long long)sCutJump, (unsigned long long)sCutCamRot, (unsigned long long)sCutCamJump, (unsigned long long)sTurnArounds);
        sCutZoom = sCutJump = sCutCamRot = sCutCamJump = 0;
        if (sAsyncDropped) fprintf(stderr, "interp: %llu imagens do meio não feitas: shader de um efeito novo ainda compilando\n", (unsigned long long)sAsyncDropped);
        sAsyncDropped = 0;
        if (sOffscreenDropped) fprintf(stderr, "interp: %llu pares sem imagens do meio: desenho em buffer fora da tela sem limpeza (efeito de brilho)\n", (unsigned long long)sOffscreenDropped);
        sOffscreenDropped = 0;
        if (sFxDropped) fprintf(stderr, "interp: %llu pares sem imagens do meio: passe de efeito novo fora da tela\n", (unsigned long long)sFxDropped);
        sFxDropped = 0;
        if (sTexScroll) fprintf(stderr, "interp: %llu linhas de textura rolando misturadas\n", (unsigned long long)sTexScroll);
        sTexScroll = 0;
        if (sCutWild) fprintf(stderr, "interp: %llu desenhos com movimento violento mostrados como estao\n", (unsigned long long)sCutWild);
        sCutWild = 0;
        sCutRot = sCutMove = sCutScale = sCutRow = sCamCuts = sTurnArounds = 0;
        sStill = 0;
        sInnerMs = 0;
        if (sAvChecks || sAvNoOther)
            fprintf(stderr, "interp: autoverificação: %llu conferências (%.2f ms cada), %llu falharam, pior %.1f%%, %llu pares a 30, %llu sem N-1 à mão, pares por modo 0/1/2: %llu/%llu/%llu%s\n",
                    (unsigned long long)sAvChecks, sAvChecks ? sAvMs / sAvChecks : 0.0, (unsigned long long)sAvFails, sAvWorst * 100,
                    (unsigned long long)sAvPairs30, (unsigned long long)sAvNoOther, (unsigned long long)sAvModePairs[0], (unsigned long long)sAvModePairs[1], (unsigned long long)sAvModePairs[2], sAvFalling ? " (em queda)" : "");
        sAvChecks = sAvFails = sAvPairs30 = sAvNoOther = sAvModePairs[0] = sAvModePairs[1] = sAvModePairs[2] = 0;
        sAvMs = sAvWorst = 0;
        sAt60 = 0;
        sUnsafe = 0;
        sReplays = sReplayedDraws = sSkippedSw = sRefused = sSkippedSeg = sMixed = sUnpaired = sJumps = sTexSnaps = 0;
        sExtraShown = sNotIdle = sNoAddr = 0;
        sReplayMs = 0;
    }
    if (!newImage) return;   // os desenhos se somam até o jogo entregar imagem nova
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - sT0).count();
    if (sStartAt >= 0 && el >= sStartAt && sSaved < 3)
    {
        sSaved++;
        const std::string p = PorteDataDir() + "interp_quadro_" + std::to_string(sSaved) + ".bin";
        if (FILE* f = fopen(p.c_str(), "wb"))
        {
            fwrite(sDraws.data(), sizeof(DrawRec), sDraws.size(), f);
            fclose(f);
        }
        fprintf(stderr, "interp: quadro %d gravado (%zu desenhos)\n", sSaved, sDraws.size());
    }
    sDraws.clear();
    sIndex = 0;
}

}
