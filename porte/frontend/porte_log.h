#pragma once
// Log avançado (22/11/2026): Sistema > Log avançado (config "log_avancado") ou PORTE_LOG=1.
// Tudo vai para <dados>\log\porte_<data>_<hora>.txt (guarda os 8 mais novos):
//  - tudo o que o porte escreve no stderr (cena, foco, tela cheia, interpolação, quedas...), que sem o log se perde
//  - cabeçalho: versão, exe, Windows, CPU, RAM, placas de vídeo, monitores, variáveis PORTE_*, config.ini inteiro
//  - uma linha por segundo: FPS da tela e do jogo, tempo entre imagens (média/mín/máx/p99, quantas passaram de 20/33/50 ms),
//    CPU do processo, RAM (em uso / privada / do sistema), pipelines compilados, superfícies criadas/recicladas
//  - ENGASGOS na hora em que acontecem (tela sem imagem nova > 50 ms; núcleo demorando > 60 ms)
//  - módulos de outros programas injetados no jogo (RTSS/Afterburner, OBS, Overwolf, Discord, Steam, Medal, ReShade...)
//  - teclas, controles, janela, opções trocadas no menu (o que mudou no config.ini)
//  - F12: marca no log + os últimos ~3 s de imagens (tempos) e de reexecuções da interpolação
//  - queda: código, módulo+deslocamento, pilha de chamadas e um minidump (log\queda_*.dmp) para abrir no Visual Studio/WinDbg
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace PorteLog
{
inline std::atomic<bool> on{false};
inline std::string dir;            // <dados>\log\ (UTF-8)
inline std::wstring wdir;
inline double t0 = 0;
inline double Now()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
inline double T() { return Now() - t0; }

// counters other files bump (interp_rec.cpp, the cache, the pipeline cache read them through porte_counters.h)
struct RingItem { double t; float a, b; int c, d; char kind; };
template <int N> struct Ring
{
    std::mutex m; RingItem it[N]{}; int head = 0, count = 0;
    void Push(const RingItem& x) { std::lock_guard<std::mutex> l(m); it[head] = x; head = (head + 1) % N; if (count < N) count++; }
    std::vector<RingItem> Last(int n) { std::lock_guard<std::mutex> l(m); std::vector<RingItem> v; n = std::min(n, count);
        for (int i = n; i > 0; i--) v.push_back(it[(head - i + N) % N]); return v; }
};
inline Ring<1024> presRing;     // 'P' real / 'G' in-between picture: a = ms since the previous one
inline Ring<512> replayRing;    // 'R': a = t, b = ms, c = mixed draws, d = jumped draws

// presentation timing (presenter thread)
struct PresStats { std::mutex m; int n = 0, gen = 0, o20 = 0, o33 = 0, o50 = 0, o100 = 0; double sum = 0, mn = 1e9, mx = 0; std::vector<float> all; };
inline PresStats ps;
inline double lastPresent = 0;
inline std::atomic<bool> holdFlag{false};   // game paused (menu / no focus / photo): a long gap is no hitch

inline void Line(const char* fmt, ...)
{
    if (!on) return;
    char b[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    fprintf(stderr, "[%9.3f] %s\n", T(), b);
}

inline void Present(bool generated)
{
    const double now = Now();
    if (!on) { lastPresent = now; return; }
    if (lastPresent > 0)
    {
        const double ms = (now - lastPresent) * 1000.0;
        {
            std::lock_guard<std::mutex> l(ps.m);
            ps.n++; ps.gen += generated; ps.sum += ms; ps.mn = std::min(ps.mn, ms); ps.mx = std::max(ps.mx, ms);
            ps.o20 += ms > 20; ps.o33 += ms > 33.4; ps.o50 += ms > 50; ps.o100 += ms > 100;
            if (ps.all.size() < 4096) ps.all.push_back((float)ms);
        }
        presRing.Push({ T(), (float)ms, 0, generated, 0, generated ? 'G' : 'P' });
        static int perSec = 0; static double secStart = 0;
        if (now - secStart > 1) { secStart = now; perSec = 0; }
        if (ms >= 50 && T() > 8 && !holdFlag && perSec++ < 10) Line("ENGASGO na tela: %.0f ms sem imagem nova", ms);
    }
    lastPresent = now;
}

inline void Replay(float t, double ms, int mixed, int jumped)
{
    if (on) replayRing.Push({ T(), t, (float)ms, mixed, jumped, 'R' });
}

inline void CoreSlow(double ms)
{
    if (on && ms > 60 && T() > 8 && !holdFlag) Line("ENGASGO no núcleo: um passo do jogo levou %.0f ms", ms);
}

inline std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr); return s;
}

// one line per second (main thread); extra = what the frontend knows (scene, menu, turbo...)
inline void Second(int shown, int fps, int speed, const std::string& extra, unsigned long long pipes, double pipeMs,
                   unsigned long long surfNew, unsigned long long surfRec, unsigned long long surfRecBig)
{
    if (!on) return;
    int n, gen, o20, o33, o50, o100; double avg, mn, mx, p99 = 0;
    {
        std::lock_guard<std::mutex> l(ps.m);
        n = ps.n; gen = ps.gen; o20 = ps.o20; o33 = ps.o33; o50 = ps.o50; o100 = ps.o100;
        avg = n ? ps.sum / n : 0; mn = n ? ps.mn : 0; mx = ps.mx;
        if (!ps.all.empty())
        {
            const size_t k = std::min(ps.all.size() - 1, (size_t)(ps.all.size() * 0.99));
            std::nth_element(ps.all.begin(), ps.all.begin() + k, ps.all.end());
            p99 = ps.all[k];
        }
        ps.n = ps.gen = ps.o20 = ps.o33 = ps.o50 = ps.o100 = 0; ps.sum = 0; ps.mn = 1e9; ps.mx = 0; ps.all.clear();
    }
    // CPU of the process (all threads, % of the whole machine) and memory
    static ULONGLONG lastK = 0, lastU = 0; static double lastT = 0;
    FILETIME c, e, k, u;
    double cpu = 0;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
    {
        const ULONGLONG K = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime, U = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
        const double now = Now();
        static const DWORD ncpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (lastT > 0) cpu = (double)((K - lastK) + (U - lastU)) / 1e7 / (now - lastT) / ncpu * 100.0;
        lastK = K; lastU = U; lastT = now;
    }
    PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb = sizeof(pm);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
    Line("seg: tela %d/s (%d do gerador) jogo %d %d%% | entre imagens ms méd %.1f mín %.1f máx %.1f p99 %.1f | >20ms %d >33 %d >50 %d >100 %d | "
         "CPU %.0f%% | RAM %zu MB (privada %zu) sistema %lu%% | pipelines %llu (%.0f ms) | superfícies novas %llu recicladas %llu (maiores %llu) | %s",
         shown, gen, fps, speed, avg, mn, mx, p99, o20, o33, o50, o100, cpu, (size_t)(pm.WorkingSetSize >> 20), (size_t)(pm.PrivateUsage >> 20),
         ms.dwMemoryLoad, pipes, pipeMs, surfNew, surfRec, surfRecBig, extra.c_str());
}

// programs that put their code inside the game (overlays, recorders, injectors)
inline void Modules(const char* when)
{
    if (!on) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    std::string third, known;
    int total = 0;
    static const char* kKnown[] = { "rtss", "afterburner", "obs", "graphics-hook", "overwolf", "ow-", "discord", "steam", "gameoverlay",
                                    "medal", "reshade", "nvspcap", "eos", "fraps", "bandicam", "action", "xsplit", "dxtory", "specialk", "vkbasalt", "layer" };
    if (Module32FirstW(snap, &me))
        do
        {
            total++;
            std::string path = Narrow(me.szExePath), low = path;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return (char)tolower(ch); });
            const bool win = low.find("\\windows\\") != std::string::npos;
            const std::string name = Narrow(me.szModule);
            std::string lname = name; std::transform(lname.begin(), lname.end(), lname.begin(), [](unsigned char ch) { return (char)tolower(ch); });
            for (const char* kk : kKnown) if (lname.find(kk) != std::string::npos) { known += " " + name; break; }
            if (!win && low.find("pokemon") == std::string::npos && low.find("porte") == std::string::npos) third += "\n      " + path;
        } while (Module32NextW(snap, &me));
    CloseHandle(snap);
    Line("módulos (%s): %d carregados; de outros programas / ganchos:%s%s", when, total, known.empty() ? " (nenhum conhecido)" : known.c_str(),
         third.empty() ? "" : ("\n    fora do Windows:" + third).c_str());
}

inline void Mark(const std::string& what)
{
    if (!on) return;
    Line("MARCA (%s)", what.c_str());
    auto pr = presRing.Last(400);
    std::string s = "    últimas imagens (ms desde a anterior; * = do gerador de quadros):";
    for (size_t i = 0; i < pr.size(); i++)
    {
        if (i % 25 == 0) s += "\n     ";
        char b[24]; snprintf(b, sizeof(b), " %.1f%s", pr[i].a, pr[i].c ? "*" : ""); s += b;
    }
    fprintf(stderr, "%s\n", s.c_str());
    auto rr = replayRing.Last(120);
    s = "    últimas reexecuções da interpolação (t mistura / ms / desenhos misturados / com salto):";
    for (size_t i = 0; i < rr.size(); i++)
    {
        if (i % 6 == 0) s += "\n     ";
        char b[64]; snprintf(b, sizeof(b), "  [%.3f] t%.2f %.2fms m%d s%d", rr[i].t, rr[i].a, rr[i].b, rr[i].c, rr[i].d); s += b;
    }
    fprintf(stderr, "%s\n", s.c_str());
}

// what changed in config.ini since the last look
inline void ConfigChanged(const std::wstring& path)
{
    if (!on) return;
    static std::vector<std::string> last;
    std::ifstream f{ std::filesystem::path(path) };
    std::vector<std::string> now; std::string l;
    while (std::getline(f, l)) if (!l.empty() && l[0] != '#') now.push_back(l);
    if (!last.empty())
    {
        std::string ch;
        for (const auto& x : now) if (std::find(last.begin(), last.end(), x) == last.end()) ch += " " + x;
        if (!ch.empty()) Line("opções trocadas:%s", ch.c_str());
    }
    last = now;
}

inline void Header(const char* version, const char* game, const std::wstring& cfgPath)
{
    if (!on) return;
    time_t tt = time(nullptr); char tb[64]; strftime(tb, sizeof(tb), "%d/%m/%Y %H:%M:%S", localtime(&tt));
    fprintf(stderr, "==================== LOG AVANÇADO - %s - %s ====================\n", game, tb);
    fprintf(stderr, "versão: %s\n", version);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa{}; GetFileAttributesExW(exe, GetFileExInfoStandard, &fa);
    SYSTEMTIME st{}; FILETIME lft; FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lft); FileTimeToSystemTime(&lft, &st);
    fprintf(stderr, "exe: %s (compilado %02d/%02d/%04d %02d:%02d)\n", Narrow(exe).c_str(), st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    // Windows (RtlGetVersion: the real build, not the compatibility one)
    typedef LONG(WINAPI * RGV)(PRTL_OSVERSIONINFOW);
    if (auto f = (RGV)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))
    {
        RTL_OSVERSIONINFOW v{}; v.dwOSVersionInfoSize = sizeof(v); f(&v);
        fprintf(stderr, "Windows: %lu.%lu build %lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }
    int r[4]; char brand[49] = {};
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004) for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(brand + i * 16, r, 16); }
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
    fprintf(stderr, "CPU: %s (%lu núcleos lógicos) | RAM: %llu MB (livre %llu MB)\n", brand, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
            ms.ullTotalPhys >> 20, ms.ullAvailPhys >> 20);
    DISPLAY_DEVICEW dd{}; dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof(dd))
    {
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        const bool act = dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
        if (act && EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
            fprintf(stderr, "vídeo: %s -> %s %lux%lu @ %lu Hz%s\n", Narrow(dd.DeviceString).c_str(), Narrow(dd.DeviceName).c_str(), dm.dmPelsWidth,
                    dm.dmPelsHeight, dm.dmDisplayFrequency, (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (principal)" : "");
    }
    // PORTE_* variables (tests / aids)
    if (LPWCH env = GetEnvironmentStringsW())
    {
        for (LPWCH p = env; *p; p += wcslen(p) + 1)
            if (wcsncmp(p, L"PORTE_", 6) == 0 || wcsncmp(p, L"VK_", 3) == 0) fprintf(stderr, "env: %s\n", Narrow(p).c_str());
        FreeEnvironmentStringsW(env);
    }
    std::ifstream f{ std::filesystem::path(cfgPath) };
    std::stringstream ss; ss << f.rdbuf();
    fprintf(stderr, "---- config.ini ----\n%s\n---- fim do config.ini ----\n", ss.str().c_str());
    ConfigChanged(cfgPath);
    fprintf(stderr, "(o log do núcleo Azahar fica ao lado: log\\azahar_log.txt)\n");
}

// stderr -> the log file; the 8 newest are kept
inline bool Start(const std::wstring& dataDirW)
{
    wdir = dataDirW + L"log\\";
    CreateDirectoryW(wdir.c_str(), nullptr);
    dir = Narrow(wdir);
    std::vector<std::filesystem::path> old;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(wdir, ec))
    {
        const auto n = e.path().filename().wstring();
        if (n.rfind(L"porte_", 0) == 0 && e.path().extension() == L".txt") old.push_back(e.path());
    }
    std::sort(old.begin(), old.end());
    while (old.size() >= 8) { std::filesystem::remove(old.front(), ec); old.erase(old.begin()); }
    time_t tt = time(nullptr); wchar_t nb[64]; wcsftime(nb, 64, L"porte_%Y%m%d_%H%M%S.txt", localtime(&tt));
    const std::wstring file = wdir + nb;
    if (!_wfreopen(file.c_str(), L"w", stderr)) return false;
    setvbuf(stderr, nullptr, _IONBF, 0);   // nothing lost if the game falls
    t0 = Now();
    on = true;
    return true;
}

// a crash: the call stack (x64 unwind from the faulting context) and a minidump
inline void CrashStack(EXCEPTION_POINTERS* e)
{
    CONTEXT ctx = *e->ContextRecord;
    fprintf(stderr, "    pilha:\n");
    for (int i = 0; i < 32 && ctx.Rip; i++)
    {
        HMODULE mod = nullptr; wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ctx.Rip, &mod))
            GetModuleFileNameW(mod, name, MAX_PATH);
        const wchar_t* base = wcsrchr(name, L'\\');
        fprintf(stderr, "      #%02d %s +0x%llx\n", i, Narrow(base ? base + 1 : name).c_str(), (unsigned long long)(ctx.Rip - (DWORD64)mod));
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fe)
        {
            if (!mod) break;   // nowhere known: stop (reading the stack further would guess)
            ctx.Rip = *(DWORD64*)ctx.Rsp; ctx.Rsp += 8;   // a leaf function
            continue;
        }
        PVOID hd = nullptr; DWORD64 ef = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &hd, &ef, nullptr);
    }
}
inline void CrashDump(EXCEPTION_POINTERS* e)
{
    if (!on) return;
    typedef BOOL(WINAPI * MDWD)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    auto f = dbg ? (MDWD)(void*)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
    if (!f) return;
    time_t tt = time(nullptr); wchar_t nb[64]; wcsftime(nb, 64, L"queda_%Y%m%d_%H%M%S.dmp", localtime(&tt));
    const std::wstring file = wdir + nb;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), e, FALSE };
    const BOOL ok = f(GetCurrentProcess(), GetCurrentProcessId(), h, (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei, nullptr, nullptr);
    CloseHandle(h);
    fprintf(stderr, "    minidump: %s (%s)\n", Narrow(file).c_str(), ok ? "gravado" : "falhou");
}
}
