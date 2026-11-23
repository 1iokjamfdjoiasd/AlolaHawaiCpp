// Pokémon X PC (teste de porte) - a small frontend around the Azahar core (GPLv2+).
// One SDL window with both 3DS screens, the player's own decrypted ROM, keyboard,
// game controller and mouse touch, and an in-game menu (F1 / Esc) with the
// Azahar graphics settings, screen layout, sound, speed, controls, save states,
// the game's own progress (Saves tab), themes and Portuguese / English.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <deque>
#include <condition_variable>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <mutex>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <SDL.h>
#include <SDL_syswm.h>

#include "audio_core/sink_details.h"
#include "audio_core/input_details.h"
#include "audio_core/dsp_interface.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/custom_textures/custom_tex_manager.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "core/savestate.h"
#include "core/core_timing.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/apt/applet_manager.h"
#include "core/hle/service/hid/hid.h"
#include "audio_core/dsp_interface.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "core/perf_stats.h"
#include "core/memory.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "font_atlas.h"
#include "geocap.h"
#include "interp_rec.h"
#include "porte_log.h"
#include "discord_rpc.h"   // (22/11) Sistema > Discord   // (22/11) Sistema > Log avançado
#include "common/porte_counters.h"
#include "porte_bridge.h"

// The same program builds Pokémon X and Pokémon Y (target PokemonYVulkan defines PORTE_GAME_Y):
// only the name, the data folder, the menu's default colours and the expected ROM change.
#if defined(PORTE_GAME_UM)
// Ultra Moon: the basic menu is on (everything the code already had); the geometric frame generator
// of X / Y stays off -- the in-game interpolation (interp_rec.cpp, Gráficos > Fluidez) replaces it
#define PORTE_SEM_GERADOR 1
// Pokémon Ultra Moon (7th generation). With PORTE_RECOMP_ON (target PokemonUltraMoonRecomp, in
// PORTE OFICIAL DO SUN & MOON) the game's code translated to C (momiji_nativo.dll) also runs
// natively inside the core; the normal target is the same as X / Y
#define PORTE_GAME_LETTER "Ultra Moon"
#define PORTE_GAME_WLETTER L"Ultra Moon"
static const unsigned kGameTitleLow = 0x001B5100;   // Pokémon Ultra Moon
static const int kDefaultTheme = 6;                  // the "Ultra Moon" theme
#ifdef PORTE_RECOMP_ON
#include "recomp_host.h"
#endif
#include "jogos.h"   // the four Alola games in one exe (Ultra Moon, Ultra Sun, Sun, Moon)
#elif defined(PORTE_GAME_Y)
#define PORTE_GAME_LETTER "Y"
#define PORTE_GAME_WLETTER L"Y"
static const unsigned kGameTitleLow = 0x00055E00;   // Pokémon Y
static const int kDefaultTheme = 1;                 // the red "Y" theme
#else
#define PORTE_GAME_LETTER "X"
#define PORTE_GAME_WLETTER L"X"
static const unsigned kGameTitleLow = 0x00055D00;   // Pokémon X
static const int kDefaultTheme = 0;                 // the blue "X" theme
#endif
static const char* kGameName = "Pokémon " PORTE_GAME_LETTER;
#if defined(PORTE_RECOMP_ON)
static const char* kVersion = "Pokémon Ultra Moon PC 0.1 (test: recompiled code)";
#elif defined(PORTE_GAME_UM)
static const char* kVersion = "Pokémon Ultra Moon PC 0.1 Vulkan (teste)";
#else
static const char* kVersion = "Pokémon " PORTE_GAME_LETTER " PC 0.3 Vulkan (teste)";
#endif

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// file streams need a path object for non-ASCII names on Windows
static std::filesystem::path P(const std::string& s) { return std::filesystem::path(Widen(s)); }

static std::string gDataDir;   // ends with a backslash
static std::string DataDir()
{
    if (const char* over = getenv("PORTE_DATA")) return std::string(over) + "\\";   // testing aid
    PWSTR roaming = nullptr;
    std::wstring base = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) base = roaming;
    CoTaskMemFree(roaming);
    std::wstring full = base + L"\\Pokemon " PORTE_GAME_WLETTER L"\\";
    CreateDirectoryW(full.c_str(), nullptr);
    return Narrow(full);
}

static void OpenPath(const std::string& p) { ShellExecuteW(nullptr, L"open", Widen(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ---------------------------------------------------------------------------
// settings of this frontend (config.ini, key=value) + the Azahar ones we expose
// ---------------------------------------------------------------------------
enum Btn { B_A, B_B, B_X, B_Y, B_Up, B_Down, B_Left, B_Right, B_L, B_R, B_ZL, B_ZR, B_Start, B_Select, B_Home,
           B_CUp, B_CDown, B_CLeft, B_CRight, B_CSlow, B_Count };
// configurable shortcuts (Controles > Atalhos); Esc and F1 always open the menu
// (22/11) the rewind stays in the code but OFF; 1 = back in the menu / shortcuts / config
#ifndef PORTE_REWIND_HABILITADO
#define PORTE_REWIND_HABILITADO 0
#endif
enum Hot { H_Save, H_Load, H_Layout, H_Swap, H_Fps, H_Full, H_Shot, H_Turbo, H_Photo, H_Rewind, H_Count };
static const int kDefaultHot[H_Count] = { SDL_SCANCODE_F5, SDL_SCANCODE_F8, SDL_SCANCODE_F6, SDL_SCANCODE_F7,
                                          SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12, SDL_SCANCODE_TAB, SDL_SCANCODE_F9,
                                          SDL_SCANCODE_BACKSPACE };
static const char* kBtnName[B_Count] = { "A", "B", "X", "Y", "↑", "↓", "←", "→", "L", "R", "ZL", "ZR", "Start", "Select", "Home",
                                         "Circle Pad ↑", "Circle Pad ↓", "Circle Pad ←", "Circle Pad →", "Circle Pad devagar" };
static const int kDefaultKeys[B_Count] = {
    SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_1, SDL_SCANCODE_2,
    SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_H,
    SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_LSHIFT };
// game controller: which SDL controller button each 3DS button (A .. Home) is on.
// kPadLT / kPadRT = the triggers, -1 = none. SDL names buttons by position (A = bottom).
constexpr int kPadLT = 0x40, kPadRT = 0x41, kPadBtns = B_Home + 1;
static const int kDefaultPad[kPadBtns] = {
    SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_X,   // 3DS positions
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kPadLT, kPadRT,
    SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK, -1 };
static bool ValidPadCode(int c) { return c == -1 || c == kPadLT || c == kPadRT || (c >= 0 && c < SDL_CONTROLLER_BUTTON_MAX); }

struct Config
{
    int lang = -1;           // 0 = Português, 1 = English, -1 = follow Windows
    int theme = kDefaultTheme;
    int glass = 2;           // how much of the game shows behind the menu: 0..3
    bool showFps = false;
    bool turboToggle = false;
    bool frameGen = false;   // in-between frames when the game draws at 30 FPS
    int fgRate = 0;          // how many frames a second the generator shows (0 = the monitor's rate)
    bool widescreen = false; // real 16:9: more of the scene on the sides (the game draws a wider view)
    int interpFps = 200;     // Ultra Moon: interpolation inside the game -- 0 = off (original 30), 60..240, -1 = the monitor's rate
    bool vsync = true;       // wait for the monitor when showing a frame (no tearing line)
    bool pauseInMenu = false; // the game keeps running behind the menu unless this is on
    bool pauseUnfocused = false; // pause the game while another program has the focus
    bool muteUnfocused = false;  // no sound while another program has the focus
    bool muteTurbo = false;      // no sound while fast-forwarding
    int fsMode = 0;              // full screen: 0 = borderless (desktop), 1 = exclusive
    int deadzone = 45;           // controller stick: % of the way before the Circle Pad moves
    int priority = 0;            // process priority: 0 normal, 1 above normal, 2 high
    int brightness = 100;        // picture brightness, % (50..150)
    int gamma = 100;             // picture gamma x100 (50..250; 100 = unchanged)
    int saturation = 100;        // picture saturation, % (0..200)
    int sharpness = 0;           // picture sharpening, % (0..100)
    bool fxaa = false;           // FXAA on the game picture
    bool autoBudget = true;      // interpolation: lower the extra pictures when the game falls behind
    bool autoSaveExit = false;   // save state "automático" (slot 11) when the game is closed
    bool autoLoadStart = false;  // ... and load it when the game opens
    bool saveBackup = true;      // copy the game's save to backups\<game> when the game opens
    int uiScale = 100;           // menu size, %
    int accent = 0;              // menu accent colour (0 = the theme's)
    bool menuAnim = true;        // menu animations
    bool shotHiRes = false;      // screenshots: the top screen at the internal resolution
    bool padTouch = true;        // the PS4 / PS5 controller's touchpad touches the bottom screen
    int dynScreen = 0;           // Tela dinâmica: 0 off, 1 battles with the top screen large, 2 battles with the top screen only
    bool discord = true;         // Sistema > Discord: shows the game / scene on the Discord profile
    std::string discordId;       // config.ini "discord_id" (Application ID of the Discord developer portal)
    bool logAdv = false;         // Sistema > Log avançado (vale ao reiniciar)
    bool rewindOn = false;       // rewind: states kept in memory every rewindEvery seconds (off by default: each point stops the game ~0.2 s)
    int rewindEvery = 5;         // seconds between rewind points (2 / 5 / 10)
    int rewindKeep = 60;         // seconds kept (30 / 60 / 120)
    bool turboBattle = false;    // battles fast-forwarded by themselves
    bool turboOnlyBattle = false;// Tab (fast-forward) only works in battles
    int hot[H_Count];            // shortcuts
    int turboRes = 2;        // internal resolution while fast-forwarding (0 = unchanged): high resolutions cap the speed
    int stateSlot = 1;
    int winW = 800, winH = 960;
    bool fullscreen = false;
    int bgColor = 0;
    // two monitors: the other 3DS screen in a second window (shown at 60 FPS, real frames)
    bool dualWindow = false;
    int layoutBeforeDual = 0;                                // the layout to go back to
    int win2x = -100000, win2y = 0, win2w = 0, win2h = 0;    // where the second window was
    int keys[B_Count];
    int pad[kPadBtns];
    int padNames = 0;        // how controller buttons are shown: 0 = like the connected one, 1 Xbox, 2 PlayStation, 3 Nintendo
    Config()
    {
        std::copy(std::begin(kDefaultKeys), std::end(kDefaultKeys), keys);
        std::copy(std::begin(kDefaultPad), std::end(kDefaultPad), pad);
        std::copy(std::begin(kDefaultHot), std::end(kDefaultHot), hot);
    }
};
static Config gCfg;

static bool EN() { return gCfg.lang == 1; }
static std::string BtnName(int b) { return b == B_Count - 1 && EN() ? std::string("Circle Pad slow") : std::string(kBtnName[b]); }
static float gMuteSaved = -1;      // the player's volume while a "mute" option silences the game (-1 = not muted)
static int gResBeforeTurbo = -1;
static int gLayoutKeep = -1;       // the player's layout while a temporary one is shown (modo foto / tela dinâmica)   // the player's resolution while fast-forward lowers it (-1 = not lowered)
static std::string T(const char* pt, const char* en) { return EN() ? en : pt; }

// Azahar settings stored as numbers: name, read, write
struct NumSetting { const char* key; std::function<int()> get; std::function<void(int)> set; };
static std::vector<NumSetting> AzaharSettings()
{
    using namespace Settings;
    auto& v = values;
    return {
        { "resolucao", [&] { return gResBeforeTurbo >= 0 ? gResBeforeTurbo : (int)v.resolution_factor.GetValue(); }, [&](int x) { v.resolution_factor.SetValue(std::clamp(x, 0, 10)); } },
        { "filtro_texturas", [&] { return (int)v.texture_filter.GetValue(); }, [&](int x) { v.texture_filter.SetValue((TextureFilter)std::clamp(x, 0, 5)); } },
        { "amostragem", [&] { return (int)v.texture_sampling.GetValue(); }, [&](int x) { v.texture_sampling.SetValue((TextureSampling)std::clamp(x, 0, 2)); } },
        { "filtro_linear", [&] { return (int)v.filter_mode.GetValue(); }, [&](int x) { v.filter_mode.SetValue(x != 0); } },
        { "mult_precisa", [&] { return (int)v.shaders_accurate_mul.GetValue(); }, [&](int x) { v.shaders_accurate_mul.SetValue(x != 0); } },
        { "cache_shaders", [&] { return (int)v.use_disk_shader_cache.GetValue(); }, [&](int x) { v.use_disk_shader_cache.SetValue(x != 0); } },
        { "shaders_async", [&] { return (int)v.async_shader_compilation.GetValue(); }, [&](int x) { v.async_shader_compilation.SetValue(x != 0); } },
        { "pular_repetidos", [&] { return (int)v.use_skip_duplicate_frames.GetValue(); }, [&](int x) { v.use_skip_duplicate_frames.SetValue(x != 0); } },
        { "escala_inteira", [&] { return (int)v.use_integer_scaling.GetValue(); }, [&](int x) { v.use_integer_scaling.SetValue(x != 0); } },
        { "modo_3d", [&] { return (int)v.render_3d.GetValue(); }, [&](int x) { v.render_3d.SetValue((StereoRenderOption)std::clamp(x, 0, 5)); } },
        { "intensidade_3d", [&] { return (int)v.factor_3d.GetValue(); }, [&](int x) { v.factor_3d.SetValue((u32)std::clamp(x, 0, 100)); } },
        { "texturas_custom", [&] { return (int)v.custom_textures.GetValue(); }, [&](int x) { v.custom_textures.SetValue(x != 0); } },
        { "precarregar_texturas", [&] { return (int)v.preload_textures.GetValue(); }, [&](int x) { v.preload_textures.SetValue(x != 0); } },
        // (with two windows the file keeps the layout to go back to, never "separate windows")
        { "layout", [&] { return gCfg.dualWindow ? gCfg.layoutBeforeDual : gLayoutKeep >= 0 ? gLayoutKeep : (int)v.layout_option.GetValue(); },
          [&](int x) { v.layout_option.SetValue(x == (int)LayoutOption::SeparateWindows ? LayoutOption::Default : (LayoutOption)x); } },
        { "tela_cima_x", [&] { return (int)v.custom_top_x.GetValue(); }, [&](int x) { v.custom_top_x.SetValue((u16)std::clamp(x, 0, 16000)); } },
        { "tela_cima_y", [&] { return (int)v.custom_top_y.GetValue(); }, [&](int x) { v.custom_top_y.SetValue((u16)std::clamp(x, 0, 16000)); } },
        { "tela_cima_largura", [&] { return (int)v.custom_top_width.GetValue(); }, [&](int x) { v.custom_top_width.SetValue((u16)std::clamp(x, 40, 16000)); } },
        { "tela_cima_altura", [&] { return (int)v.custom_top_height.GetValue(); }, [&](int x) { v.custom_top_height.SetValue((u16)std::clamp(x, 24, 16000)); } },
        { "tela_baixo_x", [&] { return (int)v.custom_bottom_x.GetValue(); }, [&](int x) { v.custom_bottom_x.SetValue((u16)std::clamp(x, 0, 16000)); } },
        { "tela_baixo_y", [&] { return (int)v.custom_bottom_y.GetValue(); }, [&](int x) { v.custom_bottom_y.SetValue((u16)std::clamp(x, 0, 16000)); } },
        { "tela_baixo_largura", [&] { return (int)v.custom_bottom_width.GetValue(); }, [&](int x) { v.custom_bottom_width.SetValue((u16)std::clamp(x, 32, 16000)); } },
        { "tela_baixo_altura", [&] { return (int)v.custom_bottom_height.GetValue(); }, [&](int x) { v.custom_bottom_height.SetValue((u16)std::clamp(x, 24, 16000)); } },
        { "tela_baixo_opacidade", [&] { return (int)v.custom_second_layer_opacity.GetValue(); }, [&](int x) { v.custom_second_layer_opacity.SetValue((u16)std::clamp(x, 10, 100)); } },
        { "inverter_telas", [&] { return (int)v.swap_screen.GetValue(); }, [&](int x) { v.swap_screen.SetValue(x != 0); } },
        { "tela_em_pe", [&] { return (int)v.upright_screen.GetValue(); }, [&](int x) { v.upright_screen.SetValue(x != 0); } },
        { "proporcao", [&] { return (int)v.aspect_ratio.GetValue(); }, [&](int x) { v.aspect_ratio.SetValue((AspectRatio)std::clamp(x, 0, 5)); } },
        { "tela_grande_x10", [&] { return (int)std::lround(v.large_screen_proportion.GetValue() * 10); }, [&](int x) { v.large_screen_proportion.SetValue(std::clamp(x, 10, 160) / 10.f); } },
        { "tela_pequena", [&] { return (int)v.small_screen_position.GetValue(); }, [&](int x) { v.small_screen_position.SetValue((SmallScreenPosition)std::clamp(x, 0, 7)); } },
        { "espaco_telas", [&] { return v.screen_gap.GetValue(); }, [&](int x) { v.screen_gap.SetValue(std::clamp(x, 0, 200)); } },
        { "volume", [&] { return (int)std::lround((gMuteSaved >= 0 ? gMuteSaved : v.volume.GetValue()) * 100); }, [&](int x) { v.volume.SetValue(std::clamp(x, 0, 100) / 100.f); } },
        { "emulacao_som", [&] { return (int)v.audio_emulation.GetValue(); }, [&](int x) { v.audio_emulation.SetValue((AudioEmulation)std::clamp(x, 0, 2)); } },
        { "esticar_som", [&] { return (int)v.enable_audio_stretching.GetValue(); }, [&](int x) { v.enable_audio_stretching.SetValue(x != 0); } },
        { "som_tempo_real", [&] { return (int)v.enable_realtime_audio.GetValue(); }, [&](int x) { v.enable_realtime_audio.SetValue(x != 0); } },
        { "limite_velocidade", [&] { return (int)std::lround(v.frame_limit.GetValue()); }, [&](int x) { v.frame_limit.SetValue(std::clamp(x, 0, 1000)); } },
        { "velocidade_turbo", [&] { return (int)std::lround(v.turbo_limit.GetValue()); }, [&](int x) { v.turbo_limit.SetValue(std::clamp(x, 0, 1000)); } },
        { "clock_cpu", [&] { return v.cpu_clock_percentage.GetValue(); }, [&](int x) { v.cpu_clock_percentage.SetValue(std::clamp(x, 25, 400)); } },
        { "new_3ds", [&] { return (int)v.is_new_3ds.GetValue(); }, [&](int x) { v.is_new_3ds.SetValue(x != 0); } },
    };
}

static std::string ConfigPath() { return gDataDir + "config.ini"; }

static void LoadConfig()
{
    // defaults that differ from Azahar's
    Settings::values.resolution_factor.SetValue(0);   // automatic: only what the window needs
    Settings::values.async_shader_compilation.SetValue(true);   // no stutter the first time an effect shows up
    Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::Vulkan);
    gCfg.fgRate = 200;   // the frame generator's default: 200 FPS
    gCfg.frameGen = true;

    std::ifstream f(P(ConfigPath()));
    std::map<std::string, std::string> kv;
    for (std::string line; std::getline(f, line);)
    {
        auto eq = line.find('=');
        if (eq != std::string::npos && line[0] != '#') kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    auto num = [&](const char* k, int def) { auto it = kv.find(k); return it == kv.end() ? def : atoi(it->second.c_str()); };
    gCfg.lang = num("idioma", gCfg.lang);
    gCfg.theme = num("tema", gCfg.theme);
    gCfg.glass = std::clamp(num("transparencia_menu", gCfg.glass), 0, 3);
    gCfg.showFps = num("mostrar_fps", gCfg.showFps) != 0;
    gCfg.turboToggle = num("turbo_alterna", gCfg.turboToggle) != 0;
    gCfg.frameGen = num("gerar_quadros", gCfg.frameGen) != 0;
    gCfg.fgRate = std::clamp(num("fps_gerador", gCfg.fgRate), 0, 500);
    gCfg.interpFps = std::clamp(num("interpolacao_fps", gCfg.interpFps), -1, 400);
    gCfg.widescreen = num("tela_16_9", gCfg.widescreen) != 0;
    gCfg.vsync = num("vsync", gCfg.vsync) != 0;
    gCfg.pauseInMenu = num("pausar_no_menu", gCfg.pauseInMenu) != 0;
    gCfg.pauseUnfocused = num("pausar_sem_foco", gCfg.pauseUnfocused) != 0;
    gCfg.muteUnfocused = num("mudo_sem_foco", gCfg.muteUnfocused) != 0;
    gCfg.muteTurbo = num("mudo_ao_acelerar", gCfg.muteTurbo) != 0;
    gCfg.fsMode = std::clamp(num("modo_tela_cheia", gCfg.fsMode), 0, 1);
    gCfg.deadzone = std::clamp(num("zona_morta", gCfg.deadzone), 10, 90);
    gCfg.priority = std::clamp(num("prioridade", gCfg.priority), 0, 2);
    gCfg.brightness = std::clamp(num("brilho", gCfg.brightness), 50, 150);
    gCfg.gamma = std::clamp(num("gama", gCfg.gamma), 50, 250);
    gCfg.saturation = std::clamp(num("saturacao", gCfg.saturation), 0, 200);
    gCfg.sharpness = std::clamp(num("nitidez", gCfg.sharpness), 0, 100);
    gCfg.fxaa = num("fxaa", gCfg.fxaa) != 0;
    gCfg.autoBudget = num("orcamento_auto", gCfg.autoBudget) != 0;
    gCfg.autoSaveExit = num("salvar_ao_sair", gCfg.autoSaveExit) != 0;
    gCfg.autoLoadStart = num("continuar_ao_abrir", gCfg.autoLoadStart) != 0;
    gCfg.saveBackup = num("backup_save", gCfg.saveBackup) != 0;
    gCfg.uiScale = std::clamp(num("escala_menu", gCfg.uiScale), 70, 150);
    gCfg.accent = std::clamp(num("cor_destaque", gCfg.accent), 0, 7);
    gCfg.menuAnim = num("animacoes_menu", gCfg.menuAnim) != 0;
    gCfg.shotHiRes = num("captura_alta", gCfg.shotHiRes) != 0;
    gCfg.padTouch = num("touchpad_toque", gCfg.padTouch) != 0;
    gCfg.dynScreen = std::clamp(num("tela_dinamica", gCfg.dynScreen), 0, 2);
    gCfg.rewindOn = num("rewind", gCfg.rewindOn) != 0;
    gCfg.logAdv = num("log_avancado", gCfg.logAdv) != 0;
    gCfg.discord = num("discord", gCfg.discord) != 0;
    if (auto it = kv.find("discord_id"); it != kv.end()) gCfg.discordId = it->second;
#if !PORTE_REWIND_HABILITADO
    gCfg.rewindOn = false;   // (disabled in the code: a "rewind=1" left in config.ini does nothing)
#endif
    gCfg.rewindEvery = std::clamp(num("rewind_intervalo", gCfg.rewindEvery), 2, 10);
    gCfg.rewindKeep = std::clamp(num("rewind_segundos", gCfg.rewindKeep), 20, 120);
    gCfg.turboBattle = num("acelerar_batalhas", gCfg.turboBattle) != 0;
    gCfg.turboOnlyBattle = num("acelerar_so_batalha", gCfg.turboOnlyBattle) != 0;
    Settings::values.input_type.SetValue((AudioCore::InputType)std::clamp(num("microfone", (int)Settings::values.input_type.GetValue()), 0, 4));
    if (auto it = kv.find("microfone_aparelho"); it != kv.end() && !it->second.empty()) Settings::values.input_device.SetValue(it->second);
    for (int i = 0; i < H_Count; i++) gCfg.hot[i] = num(("atalho_" + std::to_string(i)).c_str(), gCfg.hot[i]);
    if (auto it = kv.find("saida_som"); it != kv.end() && !it->second.empty()) Settings::values.output_device.SetValue(it->second);   // Som > Saída de som
    gCfg.turboRes = std::clamp(num("resolucao_acelerar", gCfg.turboRes), 0, 4);
    gCfg.stateSlot = std::clamp(num("slot_estado", gCfg.stateSlot), 1, 10);
    gCfg.winW = std::max(320, num("janela_w", gCfg.winW));
    gCfg.winH = std::max(240, num("janela_h", gCfg.winH));
    gCfg.fullscreen = num("tela_cheia", gCfg.fullscreen) != 0;
    gCfg.bgColor = num("cor_fundo", gCfg.bgColor);
    gCfg.dualWindow = num("duas_janelas", gCfg.dualWindow) != 0;
    gCfg.win2x = num("janela2_x", gCfg.win2x);
    gCfg.win2y = num("janela2_y", gCfg.win2y);
    gCfg.win2w = num("janela2_w", gCfg.win2w);
    gCfg.win2h = num("janela2_h", gCfg.win2h);
    for (int i = 0; i < B_Count; i++) gCfg.keys[i] = num(("tecla_" + std::to_string(i)).c_str(), gCfg.keys[i]);
    for (int i = 0; i < kPadBtns; i++)
    {
        const int c = num(("controle_" + std::to_string(i)).c_str(), gCfg.pad[i]);
        if (ValidPadCode(c)) gCfg.pad[i] = c;
    }
    gCfg.padNames = std::clamp(num("nomes_botoes", gCfg.padNames), 0, 3);
    for (auto& s : AzaharSettings())
        if (kv.count(s.key)) s.set(atoi(kv[s.key].c_str()));
}

static void SaveConfig()
{
    if (getenv("PORTE_HIDDEN")) return;   // background tests never write the settings (they mute the sound)
    std::ofstream f(P(ConfigPath()));
    f << "# " << kVersion << "\n"
      << "idioma=" << gCfg.lang << "\n" << "tema=" << gCfg.theme << "\n" << "transparencia_menu=" << gCfg.glass << "\n"
      << "mostrar_fps=" << gCfg.showFps << "\n" << "turbo_alterna=" << gCfg.turboToggle << "\n"
      << "gerar_quadros=" << gCfg.frameGen << "\n" << "fps_gerador=" << gCfg.fgRate << "\n" << "interpolacao_fps=" << gCfg.interpFps << "\n" << "tela_16_9=" << gCfg.widescreen << "\n" << "vsync=" << gCfg.vsync << "\n" << "pausar_no_menu=" << gCfg.pauseInMenu << "\n" << "resolucao_acelerar=" << gCfg.turboRes << "\n"
      << "slot_estado=" << gCfg.stateSlot << "\n" << "janela_w=" << gCfg.winW << "\n" << "janela_h=" << gCfg.winH << "\n"
      << "tela_cheia=" << gCfg.fullscreen << "\n" << "cor_fundo=" << gCfg.bgColor << "\n"
      << "duas_janelas=" << gCfg.dualWindow << "\n" << "janela2_x=" << gCfg.win2x << "\n" << "janela2_y=" << gCfg.win2y << "\n"
      << "janela2_w=" << gCfg.win2w << "\n" << "janela2_h=" << gCfg.win2h << "\n"
      << "pausar_sem_foco=" << gCfg.pauseUnfocused << "\n" << "mudo_sem_foco=" << gCfg.muteUnfocused << "\n"
      << "mudo_ao_acelerar=" << gCfg.muteTurbo << "\n" << "modo_tela_cheia=" << gCfg.fsMode << "\n"
      << "zona_morta=" << gCfg.deadzone << "\n" << "prioridade=" << gCfg.priority << "\n"
      << "brilho=" << gCfg.brightness << "\n" << "gama=" << gCfg.gamma << "\n"
      << "saturacao=" << gCfg.saturation << "\n" << "nitidez=" << gCfg.sharpness << "\n" << "fxaa=" << gCfg.fxaa << "\n"
      << "orcamento_auto=" << gCfg.autoBudget << "\n" << "salvar_ao_sair=" << gCfg.autoSaveExit << "\n"
      << "continuar_ao_abrir=" << gCfg.autoLoadStart << "\n" << "backup_save=" << gCfg.saveBackup << "\n"
      << "escala_menu=" << gCfg.uiScale << "\n" << "cor_destaque=" << gCfg.accent << "\n" << "animacoes_menu=" << gCfg.menuAnim << "\n"
      << "captura_alta=" << gCfg.shotHiRes << "\n";
    for (int i = 0; i < H_Count; i++) f << "atalho_" << i << "=" << gCfg.hot[i] << "\n";
    f << "saida_som=" << Settings::values.output_device.GetValue() << "\n";
    f << "log_avancado=" << gCfg.logAdv << "\n";
    f << "discord=" << gCfg.discord << "\n" << "discord_id=" << gCfg.discordId << "\n";
    f << "rewind=" << gCfg.rewindOn << "\n" << "rewind_intervalo=" << gCfg.rewindEvery << "\n" << "rewind_segundos=" << gCfg.rewindKeep << "\n";
    f << "tela_dinamica=" << gCfg.dynScreen << "\n" << "acelerar_batalhas=" << gCfg.turboBattle << "\n" << "acelerar_so_batalha=" << gCfg.turboOnlyBattle << "\n";
    f << "touchpad_toque=" << gCfg.padTouch << "\n" << "microfone=" << (int)Settings::values.input_type.GetValue() << "\n"
      << "microfone_aparelho=" << Settings::values.input_device.GetValue() << "\n";
    for (int i = 0; i < B_Count; i++) f << "tecla_" << i << "=" << gCfg.keys[i] << "\n";
    for (int i = 0; i < kPadBtns; i++) f << "controle_" << i << "=" << gCfg.pad[i] << "\n";
    f << "nomes_botoes=" << gCfg.padNames << "\n";
    for (auto& s : AzaharSettings()) f << s.key << "=" << s.get() << "\n";
    f.close();
    PorteLog::ConfigChanged(Widen(ConfigPath()));   // (log avançado: what changed)
}

// keyboard -> 3DS buttons (codes are SDL scancodes; the controller presses the same codes)
static void ApplyKeys()
{
    using namespace Settings::NativeButton;
    auto key = [](int b) { return InputCommon::GenerateKeyboardParam(gCfg.keys[b]); };
    auto& p = Settings::values.current_input_profile;
    p.buttons[A] = key(B_A);         p.buttons[B] = key(B_B);
    p.buttons[X] = key(B_X);         p.buttons[Y] = key(B_Y);
    p.buttons[Up] = key(B_Up);       p.buttons[Down] = key(B_Down);
    p.buttons[Left] = key(B_Left);   p.buttons[Right] = key(B_Right);
    p.buttons[L] = key(B_L);         p.buttons[R] = key(B_R);
    p.buttons[ZL] = key(B_ZL);       p.buttons[ZR] = key(B_ZR);
    p.buttons[Start] = key(B_Start); p.buttons[Select] = key(B_Select);
    p.buttons[Home] = key(B_Home);
    p.touch_device = "engine:emu_window";   // mouse on the bottom screen (empty = no touch at all)
    p.analogs[Settings::NativeAnalog::CirclePad] = InputCommon::GenerateAnalogParamFromKeys(
        gCfg.keys[B_CUp], gCfg.keys[B_CDown], gCfg.keys[B_CLeft], gCfg.keys[B_CRight], gCfg.keys[B_CSlow], 0.5f);
}

static const float kBgColors[][3] = { { 0, 0, 0 }, { 0.08f, 0.08f, 0.10f }, { 0.05f, 0.09f, 0.18f }, { 0.16f, 0.05f, 0.07f } };
static void ApplyBgColor()
{
    int i = std::clamp(gCfg.bgColor, 0, (int)std::size(kBgColors) - 1);
    Settings::values.bg_red.SetValue(kBgColors[i][0]);
    Settings::values.bg_green.SetValue(kBgColors[i][1]);
    Settings::values.bg_blue.SetValue(kBgColors[i][2]);
}

// ---------------------------------------------------------------------------
// the window the core draws into
// ---------------------------------------------------------------------------

// Vulkan: the core's renderer creates its surface on this window (its Win32 handle) and
// the presenter (vk_present_window.cpp) shows every frame. Nothing is drawn here.
class SdlWindow : public Frontend::EmuWindow
{
public:
    SDL_Window* win = nullptr;
    HWND hwnd = nullptr;

    // secondary = the second window (two monitors): the core draws the other screen there
    explicit SdlWindow(bool secondary = false) : Frontend::EmuWindow(secondary) {}

    bool Create(int w, int h, bool hidden, const char* title = kGameName, int x = SDL_WINDOWPOS_CENTERED, int y = SDL_WINDOWPOS_CENTERED)
    {
        win = SDL_CreateWindow(title, x, y, w, h,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (hidden ? SDL_WINDOW_HIDDEN : 0));
        if (!win) return false;
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        if (!SDL_GetWindowWMInfo(win, &wm)) return false;
        hwnd = wm.info.win.window;
        window_info.type = Frontend::WindowSystemType::Windows;
        window_info.render_surface = hwnd;
        window_info.render_surface_scale = 1.0f;
        Resized();
        return true;
    }
    void Resized()
    {
        int w, h;
        DrawableSize(w, h);
        UpdateCurrentFramebufferLayout(w, h);
    }
    // the client area in pixels (the process is DPI aware)
    void DrawableSize(int& w, int& h)
    {
        RECT r{};
        GetClientRect(hwnd, &r);
        w = std::max(1, (int)(r.right - r.left));
        h = std::max(1, (int)(r.bottom - r.top));
    }
    // window pixels -> drawable pixels (high DPI)
    void ToDrawable(int x, int y, float& ox, float& oy)
    {
        int ww, wh, dw, dh;
        SDL_GetWindowSize(win, &ww, &wh);
        DrawableSize(dw, dh);
        ox = (float)x * dw / std::max(1, ww);
        oy = (float)y * dh / std::max(1, wh);
    }

    void SwapBuffers() override {}   // the presenter shows the frames
    void PollEvents() override {}
    void MakeCurrent() override {}
    void DoneCurrent() override {}
};

// ---------------------------------------------------------------------------
// drawing the menu (a list of quads the presenter draws with Vulkan)
// ---------------------------------------------------------------------------
struct Rect
{
    float x, y, w, h;
    bool Contains(float px, float py) const { return w > 0 && px >= x && px < x + w && py >= y && py < y + h; }
};
struct Color { float r, g, b, a; };
static Color WithAlpha(Color c, float a) { c.a = a; return c; }

struct Theme { const char* name; Color accent, bg, panel, card, text, dim; };
static const Theme kThemes[] = {
    { "X",       { 0.35f, 0.62f, 1.00f, 1 }, { 0.02f, 0.03f, 0.07f, 1 }, { 0.05f, 0.08f, 0.16f, 1 }, { 0.03f, 0.06f, 0.17f, 1 }, { 0.95f, 0.96f, 0.98f, 1 }, { 0.62f, 0.66f, 0.74f, 1 } },
    { "Y",       { 1.00f, 0.38f, 0.40f, 1 }, { 0.06f, 0.02f, 0.03f, 1 }, { 0.14f, 0.05f, 0.07f, 1 }, { 0.18f, 0.03f, 0.05f, 1 }, { 0.97f, 0.95f, 0.95f, 1 }, { 0.72f, 0.64f, 0.66f, 1 } },
    { "Kalos",   { 0.30f, 0.85f, 0.75f, 1 }, { 0.02f, 0.05f, 0.05f, 1 }, { 0.05f, 0.11f, 0.11f, 1 }, { 0.02f, 0.13f, 0.12f, 1 }, { 0.94f, 0.98f, 0.97f, 1 }, { 0.60f, 0.72f, 0.70f, 1 } },
    { "Gold",    { 0.98f, 0.80f, 0.30f, 1 }, { 0.04f, 0.035f, 0.03f, 1 }, { 0.10f, 0.085f, 0.06f, 1 }, { 0.12f, 0.08f, 0.02f, 1 }, { 0.97f, 0.96f, 0.93f, 1 }, { 0.72f, 0.69f, 0.62f, 1 } },
    { "Black",   { 0.78f, 0.80f, 0.84f, 1 }, { 0.008f, 0.008f, 0.01f, 1 }, { 0.055f, 0.055f, 0.065f, 1 }, { 0.04f, 0.04f, 0.05f, 1 }, { 0.95f, 0.95f, 0.96f, 1 }, { 0.60f, 0.61f, 0.65f, 1 } },
    { "White",   { 0.20f, 0.23f, 0.30f, 1 }, { 0.90f, 0.91f, 0.93f, 1 }, { 0.97f, 0.97f, 0.98f, 1 }, { 0.97f, 0.97f, 0.98f, 1 }, { 0.10f, 0.11f, 0.14f, 1 }, { 0.40f, 0.42f, 0.48f, 1 } },
    { "Ultra Moon", { 0.66f, 0.48f, 1.00f, 1 }, { 0.03f, 0.02f, 0.07f, 1 }, { 0.08f, 0.06f, 0.17f, 1 }, { 0.07f, 0.04f, 0.18f, 1 }, { 0.96f, 0.95f, 0.99f, 1 }, { 0.68f, 0.64f, 0.78f, 1 } },
};
// Aparência > Cor de destaque: 0 = the theme's own, then fixed colours
static const Color kAccents[] = { { 0, 0, 0, 1 }, { 0.35f, 0.62f, 1.00f, 1 }, { 0.66f, 0.48f, 1.00f, 1 }, { 0.30f, 0.85f, 0.55f, 1 },
                                  { 1.00f, 0.40f, 0.40f, 1 }, { 0.98f, 0.78f, 0.28f, 1 }, { 1.00f, 0.50f, 0.78f, 1 }, { 0.30f, 0.85f, 0.85f, 1 } };
static const Theme& Th()
{
    static Theme t;
    t = kThemes[std::clamp(gCfg.theme, 0, (int)std::size(kThemes) - 1)];
    if (gCfg.accent > 0 && gCfg.accent < (int)std::size(kAccents)) t.accent = kAccents[gCfg.accent];
    return t;
}

// The menu is drawn by the presenter (Vulkan, framegen_vk.cpp); here it only becomes a
// list of vertices (pixels, y down), built on the presenter thread under gUiMutex.
struct Ui
{
    int glyph[256];
    float w = 1, h = 1;
    std::vector<Porte::UiVert>* out = nullptr;
    float rw = 0, rh = 0, radius = 0;   // the rounded rectangle being drawn
    float shape = 0;                    // the controller symbol being drawn (mode 3)
    float ox = 0, oy = 0, alpha = 1;    // animations: everything drawn is moved by (ox, oy) and faded by alpha
};
static Ui gUi;

static bool InitUi()
{
    std::fill(std::begin(gUi.glyph), std::end(gUi.glyph), -1);
    for (int i = 0; i < (int)std::size(kFontGlyphs); i++)
        if (kFontGlyphs[i].cp < 256) gUi.glyph[kFontGlyphs[i].cp] = i;
    Porte::gAtlas = kFontAtlas;
    Porte::gAtlasW = kFontAtlasW;
    Porte::gAtlasH = kFontAtlasH;
    return true;
}

static void BeginUi(int w, int h, std::vector<Porte::UiVert>& out)
{
    gUi.w = (float)w;
    gUi.h = (float)h;
    gUi.out = &out;
}

static void Quad(const Rect& r0, int mode, Color c, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1)
{
    if (r0.w <= 0 || r0.h <= 0 || !gUi.out) return;
    const Rect r { r0.x + gUi.ox, r0.y + gUi.oy, r0.w, r0.h };
    c.a *= gUi.alpha;
    const float sw = mode == 2 ? gUi.rw : r.w, sh = mode == 2 ? gUi.rh : r.h;
    const float rad = mode == 2 ? gUi.radius : (mode == 3 || mode == 4) ? gUi.shape : 0.0f;
    auto v = [&](float ax, float ay) {
        return Porte::UiVert{ r.x + ax * r.w, r.y + ay * r.h, u0 + (u1 - u0) * ax, v0 + (v1 - v0) * ay, ax * r.w, ay * r.h,
                              sw, sh, c.r, c.g, c.b, c.a, (float)mode, rad };
    };
    const Porte::UiVert a = v(0, 0), b = v(0, 1), cc = v(1, 1), d = v(1, 0);
    gUi.out->insert(gUi.out->end(), { a, b, cc, a, cc, d });
}
static void FillRect(const Rect& r, Color c) { Quad(r, 0, c); }
static void RoundRect(const Rect& r, float radius, Color c)
{
    gUi.rw = r.w;
    gUi.rh = r.h;
    gUi.radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    Quad(r, 2, c);
}

static std::vector<u32> DecodeUtf8(const std::string& s)
{
    std::vector<u32> out;
    for (size_t i = 0; i < s.size();)
    {
        u8 c = s[i];
        u32 cp = '?';
        int n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); n = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); n = 3; }
        else if ((c >> 3) == 30) n = 4;
        // arrows have no glyph in the atlas: draw them as Latin-1 lookalikes
        if (cp == 0x2190) cp = '<';
        else if (cp == 0x2192) cp = '>';
        else if (cp == 0x2191) cp = '^';
        else if (cp == 0x2193) cp = 'v';
        else if (cp == 0x2026) cp = '.';
        out.push_back(cp);
        i += n;
    }
    return out;
}
static const FontGlyph* Glyph(u32 cp)
{
    int i = cp < 256 ? gUi.glyph[cp] : -1;
    if (i < 0) i = gUi.glyph['?'];
    return &kFontGlyphs[i];
}
// Controller buttons drawn as icons inside any text: PadIcon(code) gives a private-use
// character (U+E000 + code) that Text / TextWidth turn into the icon (defined below).
static bool IsPadIcon(u32 cp) { return cp >= 0xE000 && cp < 0xE100; }
constexpr int kIconTrophy = 0xF0, kIconLocked = 0xF1;   // not controller buttons: the achievement cups
constexpr int kIconAch = 0xA0, kIconAchLocked = 0xC0;     // + achievement number: its emblem (or picture)
static void AchIconDraw(int i, bool locked, float x, float y, float s, float alpha);
static float PadIconWidth(int code, float size);
static void PadIconDraw(int code, float x, float y, float size, float alpha);
static float TextWidth(const std::string& s, float size)
{
    float k = size / kFontSize, w = 0;
    for (u32 cp : DecodeUtf8(s)) w += IsPadIcon(cp) ? PadIconWidth(cp - 0xE000, size) : Glyph(cp)->adv * k;
    return w;
}
// top of the text at y; align 0 = left, 1 = centre, 2 = right of x
static void Text(const std::string& s, float x, float y, float size, Color c, int align = 0)
{
    float k = size / kFontSize;
    if (align == 1) x -= TextWidth(s, size) / 2;
    else if (align == 2) x -= TextWidth(s, size);
    for (u32 cp : DecodeUtf8(s))
    {
        if (IsPadIcon(cp))
        {
            PadIconDraw(cp - 0xE000, x, y, size, c.a);
            x += PadIconWidth(cp - 0xE000, size);
            continue;
        }
        const FontGlyph* g = Glyph(cp);
        if (cp != ' ')
            Quad({ x + g->ox * k, y - 2 * k, g->w * k, kFontCellH * k }, 1, c, g->x / (float)kFontAtlasW,
                 g->y / (float)kFontAtlasH, (g->x + g->w) / (float)kFontAtlasW, (g->y + kFontCellH) / (float)kFontAtlasH);
        x += g->adv * k;
    }
}
static float Fit(const std::string& s, float size, float maxW)
{
    float w = TextWidth(s, size);
    return w > maxW ? size * maxW / w : size;
}

// ---------------------------------------------------------------------------
// controller buttons: names and icons in the style of the controller
// ---------------------------------------------------------------------------
// type of the connected controller (SDL_GameControllerType), set by the main thread
static std::atomic<int> gPadType{ SDL_CONTROLLER_TYPE_UNKNOWN };

// 1 Xbox, 2 PlayStation, 3 Nintendo
static int PadStyle()
{
    if (gCfg.padNames > 0) return gCfg.padNames;
    switch (gPadType.load())
    {
    case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4: case SDL_CONTROLLER_TYPE_PS5: return 2;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return 3;
    default: return 1;
    }
}
static const char* PadStyleName(int st) { static const char* n[] = { "", "Xbox", "PlayStation", "Nintendo" }; return n[std::clamp(st, 1, 3)]; }

static bool PadIsFace(int c) { return c >= SDL_CONTROLLER_BUTTON_A && c <= SDL_CONTROLLER_BUTTON_Y; }
static bool PadIsDpad(int c) { return c >= SDL_CONTROLLER_BUTTON_DPAD_UP && c <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; }
static bool PadIsStick(int c) { return c == SDL_CONTROLLER_BUTTON_LEFTSTICK || c == SDL_CONTROLLER_BUTTON_RIGHTSTICK; }

// the short label printed on the button
static std::string PadShort(int c)
{
    const int st = PadStyle();
    auto by = [st](const char* xbox, const char* ps, const char* nin) { return std::string(st == 2 ? ps : st == 3 ? nin : xbox); };
    switch (c)
    {
    case SDL_CONTROLLER_BUTTON_A: return by("A", "X", "B");   // bottom
    case SDL_CONTROLLER_BUTTON_B: return by("B", "O", "A");   // right
    case SDL_CONTROLLER_BUTTON_X: return by("X", "[]", "Y");  // left
    case SDL_CONTROLLER_BUTTON_Y: return by("Y", "^", "X");   // top
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return by("LB", "L1", "L");
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return by("RB", "R1", "R");
    case kPadLT: return by("LT", "L2", "ZL");
    case kPadRT: return by("RT", "R2", "ZR");
    case SDL_CONTROLLER_BUTTON_BACK: return by("View", "Create", "-");
    case SDL_CONTROLLER_BUTTON_START: return by("Menu", "Options", "+");
    case SDL_CONTROLLER_BUTTON_GUIDE: return by("Guide", "PS", "Home");
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return by("LS", "L3", "LS");
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return by("RS", "R3", "RS");
    case SDL_CONTROLLER_BUTTON_MISC1: return by("Share", "Mic", "Capture");
    default: return std::to_string(c);
    }
}
// the name written in the menu
static std::string PadName(int c)
{
    const int st = PadStyle();
    switch (c)
    {
    case -1: return T("nenhum", "none");
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return T("Direcional cima", "D-pad up");
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return T("Direcional baixo", "D-pad down");
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return T("Direcional esquerda", "D-pad left");
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return T("Direcional direita", "D-pad right");
    case SDL_CONTROLLER_BUTTON_A: if (st == 2) return T("Cruz", "Cross"); break;
    case SDL_CONTROLLER_BUTTON_B: if (st == 2) return T("Círculo", "Circle"); break;
    case SDL_CONTROLLER_BUTTON_X: if (st == 2) return T("Quadrado", "Square"); break;
    case SDL_CONTROLLER_BUTTON_Y: if (st == 2) return T("Triângulo", "Triangle"); break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return T("Analógico esq. (", "Left stick (") + PadShort(c) + ")";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return T("Analógico dir. (", "Right stick (") + PadShort(c) + ")";
    default: break;
    }
    return PadShort(c);
}
// a character that Text draws as the icon of the button
static std::string PadIcon(int c)
{
    if (c < 0) return "";
    const u32 cp = 0xE000 + (u32)c;
    return { char(0xE0 | (cp >> 12)), char(0x80 | ((cp >> 6) & 0x3F)), char(0x80 | (cp & 0x3F)) };
}
// icon followed by the name
static std::string PadLabel(int c)
{
    if (c < 0) return PadName(c);
    const std::string name = PadName(c);
    return name == PadShort(c) ? PadIcon(c) : PadIcon(c) + " " + name;   // no "L1 L1"
}

// Nintendo - and +: round buttons
static bool PadRound(int c) { return PadStyle() == 3 && (c == SDL_CONTROLLER_BUTTON_BACK || c == SDL_CONTROLLER_BUTTON_START); }
static float PadIconWidth(int c, float size)
{
    const float s = size * 1.2f, gap = size * 0.12f;
    if (c == kIconTrophy || c == kIconLocked) return s + 2 * gap;
    if (c >= kIconAch && c < kIconAchLocked + 0x20) return s + 2 * gap;
    if (PadIsFace(c) || PadIsDpad(c) || PadIsStick(c) || PadRound(c)) return s + 2 * gap;
    return std::max(s * 1.5f, TextWidth(PadShort(c), size * 0.58f) + size * 0.8f) + 2 * gap;
}

static void PadIconDraw(int c, float x, float y, float size, float alpha)
{
    const int st = PadStyle();
    const float s = size * 1.2f, gap = size * 0.12f;
    const float cy = y + size * 0.54f;   // middle of the letters of the line
    if (c >= kIconAch && c < kIconAchLocked + 0x20)
    {
        const bool locked = c >= kIconAchLocked;
        AchIconDraw(c - (locked ? kIconAchLocked : kIconAch), locked, x + gap, cy - s / 2, s, alpha);
        return;
    }
    if (c == kIconTrophy || c == kIconLocked)
    {
        // a cup: bowl, handles, stem and base (gold when unlocked, grey when not)
        const Color gold = c == kIconTrophy ? Color { 0.98f, 0.77f, 0.20f, alpha } : Color { 0.50f, 0.52f, 0.58f, alpha };
        const Color dark = c == kIconTrophy ? Color { 0.78f, 0.55f, 0.08f, alpha } : Color { 0.36f, 0.38f, 0.43f, alpha };
        const float x0 = x + gap, y0 = cy - s / 2;
        RoundRect({ x0 + s * 0.06f, y0 + s * 0.10f, s * 0.24f, s * 0.30f }, s * 0.12f, dark);   // handles
        RoundRect({ x0 + s * 0.70f, y0 + s * 0.10f, s * 0.24f, s * 0.30f }, s * 0.12f, dark);
        RoundRect({ x0 + s * 0.20f, y0 + s * 0.02f, s * 0.60f, s * 0.52f }, s * 0.22f, gold);   // bowl
        FillRect({ x0 + s * 0.20f, y0 + s * 0.02f, s * 0.60f, s * 0.16f }, gold);
        FillRect({ x0 + s * 0.44f, y0 + s * 0.52f, s * 0.12f, s * 0.20f }, dark);               // stem
        RoundRect({ x0 + s * 0.26f, y0 + s * 0.72f, s * 0.48f, s * 0.20f }, s * 0.06f, gold);   // base
        return;
    }
    const Color rim { 0.58f, 0.60f, 0.66f, alpha }, body { 0.12f, 0.13f, 0.16f, alpha }, white { 0.96f, 0.96f, 0.97f, alpha };
    const float e = std::max(1.0f, s * 0.07f);   // rim width
    auto button = [&](const Rect& r, float radius) {
        RoundRect(r, radius, rim);
        RoundRect({ r.x + e, r.y + e, r.w - 2 * e, r.h - 2 * e }, radius - e, body);
    };
    const Rect r { x + gap, cy - s / 2, s, s };
    if (PadIsFace(c))
    {
        button(r, s / 2);
        const int i = c - SDL_CONTROLLER_BUTTON_A;   // A bottom, B right, X left, Y top
        if (st == 2)
        {
            static const Color ps[4] = { { 0.49f, 0.67f, 0.98f, 1 }, { 1.0f, 0.42f, 0.45f, 1 }, { 0.93f, 0.55f, 0.85f, 1 }, { 0.25f, 0.85f, 0.70f, 1 } };
            static const int shape[4] = { 0, 1, 2, 3 };   // cross, circle, square, triangle
            const float in = s * 0.19f;
            gUi.shape = (float)shape[i];
            Quad({ r.x + in, r.y + in, s - 2 * in, s - 2 * in }, 3, WithAlpha(ps[i], alpha));
        }
        else
        {
            static const Color xb[4] = { { 0.42f, 0.80f, 0.27f, 1 }, { 0.93f, 0.30f, 0.28f, 1 }, { 0.25f, 0.55f, 0.98f, 1 }, { 0.98f, 0.78f, 0.18f, 1 } };
            const float ls = size * 0.74f;
            Text(PadShort(c), r.x + s / 2, cy - ls * 0.54f, ls, st == 1 ? WithAlpha(xb[i], alpha) : white, 1);
        }
        return;
    }
    if (PadIsDpad(c))
    {
        const float a = s * 0.36f, cx = r.x + s / 2;
        RoundRect({ cx - a / 2, r.y, a, s }, a * 0.25f, rim);
        RoundRect({ r.x, cy - a / 2, s, a }, a * 0.25f, rim);
        const float in = e;
        const float half = s / 2 - a * 0.1f;
        Rect arm;
        switch (c)
        {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: arm = { cx - a / 2 + in, r.y + in, a - 2 * in, half - in }; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: arm = { cx - a / 2 + in, r.y + s - half, a - 2 * in, half - in }; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: arm = { r.x + in, cy - a / 2 + in, half - in, a - 2 * in }; break;
        default: arm = { r.x + s - half, cy - a / 2 + in, half - in, a - 2 * in }; break;
        }
        RoundRect({ cx - a / 2 + in, r.y + in, a - 2 * in, s - 2 * in }, a * 0.2f, body);
        RoundRect({ r.x + in, cy - a / 2 + in, s - 2 * in, a - 2 * in }, a * 0.2f, body);
        RoundRect(arm, a * 0.2f, white);
        return;
    }
    if (PadIsStick(c))
    {
        button(r, s / 2);
        const float ls = size * 0.62f;
        Text(c == SDL_CONTROLLER_BUTTON_LEFTSTICK ? "L" : "R", r.x + s / 2, cy - ls * 0.54f, ls, white, 1);
        return;
    }
    // shoulders, triggers, Start / Back / Guide: a pill with the label
    const bool round = PadRound(c);
    const float w = PadIconWidth(c, size) - 2 * gap, h = round ? w : s * 0.86f;
    const Rect p { x + gap, cy - h / 2, w, h };
    const bool trigger = c == kPadLT || c == kPadRT;
    button(p, trigger ? h * 0.32f : h / 2);
    const float ls = size * (round ? 0.8f : 0.58f);
    Text(PadShort(c), p.x + w / 2, cy - ls * 0.54f, ls, white, 1);
}

// ---------------------------------------------------------------------------
// the game's own save (Saves tab): Sun / Moon (file of 0x6BE00 bytes) and Ultra Sun / Ultra Moon (0x6CC00).
// Block offsets from the save's own block table (footer "FEEB" + (u32 length, u16 id, u16 checksum) entries, blocks
// aligned to 0x200), checked on saves of the four games:
//   MyStatus: +0 TID, +4 game (30 Sun, 31 Moon, 32 Ultra Sun, 33 Ultra Moon), +0x38 name (UTF-16, 13)
//   Zukan: +0x88 caught bits (802 / 807 species)   Misc: +4 money, +8 bits 4..18 = the 15 trainer passport stamps
//   PlayTime: u16 hours, u8 minutes, u8 seconds
// ---------------------------------------------------------------------------
struct SaveLayout { u32 my, zukan, misc, playtime; int species; };
static const SaveLayout kLayoutSM { 0x1200, 0x2A00, 0x4000, 0x40C00, 802 };
static const SaveLayout kLayoutUSUM { 0x1400, 0x2C00, 0x4400, 0x41000, 807 };
static const SaveLayout* LayoutOfFile(size_t size) { return size == 0x6BE00 ? &kLayoutSM : size == 0x6CC00 ? &kLayoutUSUM : nullptr; }
// passport stamps (Misc + 8, bits 4..18): 0 official trainer, 1-4 the trials of Melemele / Akala / Ula'ula / Poni,
// 5 island challenge (champion), 6-9 the island Pokédexes, 10 the Alola Pokédex, 11-13 Battle Tree 50 wins in a row
// (single / double / multi), 14 Poké Finder
static int PassportStamps(u32 raw) { return (int)((raw >> 4) & 0x7FFF); }
static int CountBits(const u8* p, int n) { int c = 0; for (int i = 0; i < n; i++) c += (p[i / 8] >> (i % 8)) & 1; return c; }
static std::string Utf16Name(const u8* p, int maxChars)
{
    std::string out;
    for (int i = 0; i < maxChars; i++)
    {
        const u16 c = p[i * 2] | (p[i * 2 + 1] << 8);
        if (c == 0) break;
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

struct GameSaveInfo { bool ok = false; std::string name; int game = 0, hours = 0, minutes = 0, caught = 0, species = 0, stamps = 0; u32 money = 0; };
static u64 gProgramId = 0;
static Uint64 gSessionStart = 0;   // when the game started running (Jogo > Tempo nesta sessão)

static std::string GameSavePath()
{
    char p[256];
    snprintf(p, sizeof(p), "Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/%08x/%08x/data/00000001/main",
             (unsigned)(gProgramId >> 32), (unsigned)(gProgramId & 0xFFFFFFFF));
    return FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir) + p;
}

static GameSaveInfo ReadGameSave()
{
    static GameSaveInfo cached;
    static long long cachedTime = -2;
    static std::string cachedPath;
    struct _stat64 st;
    const std::string path = GameSavePath();
    if (_wstat64(Widen(path).c_str(), &st) != 0) { cachedTime = -1; return {}; }
    if (st.st_mtime == cachedTime && path == cachedPath) return cached;
    cachedTime = st.st_mtime;
    cachedPath = path;
    cached = {};
    std::ifstream f(P(path), std::ios::binary);
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    const SaveLayout* L = LayoutOfFile(d.size());
    if (!L) return cached;
    cached.name = Utf16Name(&d[L->my + 0x38], 13);
    cached.game = d[L->my + 4];
    cached.hours = d[L->playtime] | (d[L->playtime + 1] << 8);
    cached.minutes = d[L->playtime + 2];
    cached.species = L->species;
    cached.caught = CountBits(&d[L->zukan + 0x88], L->species);
    cached.money = d[L->misc + 4] | (d[L->misc + 5] << 8) | (d[L->misc + 6] << 16) | ((u32)d[L->misc + 7] << 24);
    cached.stamps = PassportStamps(d[L->misc + 8] | (d[L->misc + 9] << 8) | (d[L->misc + 10] << 16) | ((u32)d[L->misc + 11] << 24));
    cached.ok = !cached.name.empty() && cached.minutes < 60 && cached.money <= 9999999 && cached.game >= 30 && cached.game <= 33;
    return cached;
}
// Saves > backups: a copy of the game's save file in <data>\backups\<game>\main_<date>.bin (the 10 newest are kept;
// nothing is written when the save did not change since the newest copy)
static std::string SaveBackupDir() { return gDataDir + "backups\\" + (Jogos::gAtual ? std::string(Jogos::gAtual->nome) : std::string("jogo")) + "\\"; }
static bool BackupGameSave()
{
    std::ifstream f(P(GameSavePath()), std::ios::binary);
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 0x1000) return false;
    namespace fs = std::filesystem;
    const fs::path dir(Widen(SaveBackupDir()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<fs::path> old;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().filename().wstring().rfind(L"main_", 0) == 0) old.push_back(e.path());
    std::sort(old.begin(), old.end());
    if (!old.empty())
    {
        std::ifstream g(old.back(), std::ios::binary);
        std::vector<u8> last((std::istreambuf_iterator<char>(g)), {});
        if (last == d) return false;   // (unchanged)
    }
    char name[64];
    const time_t t = time(nullptr);
    strftime(name, sizeof(name), "main_%Y%m%d_%H%M%S.bin", localtime(&t));
    std::ofstream o(dir / name, std::ios::binary);
    o.write((const char*)d.data(), (std::streamsize)d.size());
    o.close();
    old.push_back(dir / name);
    while (old.size() > 10) { fs::remove(old.front(), ec); old.erase(old.begin()); }
    fprintf(stderr, "save: backup %s\n", name);
    return true;
}
// Jogo > Mods: the Azahar loads ONE mod per game, from <load>\mods\<title id>\ (LayeredFS: its romfs\ replaces the
// game's files). The library <data>\mods\<game>\<name>\ keeps the others; turning one on moves it into place (the one
// that was there goes back to the library). Code mods (exefs) do not work here: the game's code is recompiled.
namespace fs_m = std::filesystem;
static fs_m::path ModActiveDir()
{
    char b[32];
    snprintf(b, sizeof(b), "%016llX", (unsigned long long)gProgramId);
    return fs_m::path(Widen(FileUtil::GetUserPath(FileUtil::UserPath::LoadDir) + "mods/" + b));
}
static fs_m::path ModLibDir() { return fs_m::path(Widen(gDataDir + "mods\\" + (Jogos::gAtual ? std::string(Jogos::gAtual->nome) : std::string("jogo")))); }
static std::string ModActiveName()
{
    std::error_code ec;
    const fs_m::path a = ModActiveDir();
    if (!fs_m::exists(a, ec)) return "";
    std::ifstream f(a / "porte_mod_nome.txt");
    std::string n;
    std::getline(f, n);
    return n.empty() ? T("(instalado à mão)", "(installed by hand)") : n;
}
static bool ModHasCode(const fs_m::path& dir)
{
    std::error_code ec;
    return fs_m::exists(dir / "exefs", ec) || fs_m::exists(dir / "code.ips", ec) || fs_m::exists(dir / "code.bin", ec);
}
static std::vector<std::string> ModList()
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs_m::directory_iterator(ModLibDir(), ec))
        if (e.is_directory()) out.push_back(Narrow(e.path().filename().wstring()));
    std::sort(out.begin(), out.end());
    return out;
}
// turns on the library's mod 'name' ("" = none: the original game); true when something changed
static bool ModSet(const std::string& name)
{
    std::error_code ec;
    const fs_m::path act = ModActiveDir(), lib = ModLibDir();
    fs_m::create_directories(lib, ec);
    if (fs_m::exists(act, ec))
    {
        std::string old;
        { std::ifstream f(act / "porte_mod_nome.txt"); std::getline(f, old); }
        if (old.empty()) { char b[48]; const time_t t = time(nullptr); strftime(b, sizeof(b), "mod_antigo_%Y%m%d_%H%M%S", localtime(&t)); old = b; }
        fs_m::path back = lib / fs_m::path(Widen(old));
        if (fs_m::exists(back, ec)) back += L"_2";
        fs_m::rename(act, back, ec);
        if (ec) { fprintf(stderr, "mods: não deu para tirar o mod ativo (%s)\n", ec.message().c_str()); return false; }
    }
    if (!name.empty())
    {
        fs_m::create_directories(act.parent_path(), ec);
        fs_m::rename(lib / fs_m::path(Widen(name)), act, ec);
        if (ec) { fprintf(stderr, "mods: não deu para ligar %s (%s)\n", name.c_str(), ec.message().c_str()); return false; }
        std::ofstream(act / "porte_mod_nome.txt") << name;
    }
    fprintf(stderr, "mods: ativo agora = %s\n", name.empty() ? "(nenhum)" : name.c_str());
    return true;
}
static void ModReadme()
{
    std::error_code ec;
    fs_m::create_directories(ModLibDir(), ec);
    const fs_m::path r = ModLibDir() / "LEIA-ME.txt";
    if (fs_m::exists(r, ec)) return;
    std::ofstream f(r);
    f << "Mods / Mods\n\nCada mod fica numa pasta aqui dentro, com a pasta romfs\\ do mod (texturas, modelos, textos).\n"
         "Ligue no menu do jogo (Esc > Jogo > Mods) e reinicie o jogo. Só um mod fica ativo por vez.\n"
         "Mods de código (pasta exefs\\ ou code.ips) NÃO funcionam: o código do jogo é recompilado.\n\n"
         "Each mod is a folder in here, with the mod's romfs\\ folder (textures, models, text).\n"
         "Turn it on in the game menu (Esc > Game > Mods) and restart the game. One mod at a time.\n"
         "Code mods (exefs\\ folder or code.ips) do NOT work: the game's code is recompiled.\n";
}
// Saves > Batalhas: battles counted since the counter started (the Battle module coming in), one file per game
static std::string StatsPath() { return gDataDir + "estatisticas" + (Jogos::gAtual ? Narrow(Jogos::gAtual->sufixo) : std::string()) + ".txt"; }
static long long gBattles = -1;   // (-1 = not read yet)
static void StatsLoad()
{
    gBattles = 0;
    std::ifstream f(P(StatsPath()));
    for (std::string l; std::getline(f, l);)
        if (l.rfind("batalhas=", 0) == 0) gBattles = atoll(l.c_str() + 9);
}
static void StatsSave()
{
    if (getenv("PORTE_HIDDEN") && !getenv("PORTE_ACHSAVE")) return;   // background tests keep the file as it is
    std::ofstream f(P(StatsPath()));
    f << "# contadores do porte\nbatalhas=" << gBattles << "\n";
}
static std::string GameNameOf(int game)
{
    switch (game) { case 30: return "Sun"; case 31: return "Moon"; case 32: return "Ultra Sun"; case 33: return "Ultra Moon"; default: return "?"; }
}

// ---------------------------------------------------------------------------
// menu
// ---------------------------------------------------------------------------
struct MenuItem
{
    std::string label, value;
    std::function<void(int)> change;   // left / right
    std::function<void()> activate;    // Enter / click
    bool restart = false;              // takes effect when the game restarts
    std::string hint;
    bool header = false;               // a section title (selectable: Enter / click folds it)
    std::string sub;                   // short note under the name (accent colour): "Restart required"...
    std::string def;                   // the default value, shown in the description pane
    float bar = -1;                    // 0..1: a numeric setting (shown with < > and a bar); -1 = a list of choices
    bool collapsed = false;            // (section titles) folded
    std::vector<std::string> opts;     // the choices of a list setting (shown in a dropdown); empty = none
    int optIdx = -1;                   // which one is chosen now
};
static double NowSec();   // (defined with the presentation code)
static void OptsArr(MenuItem& m, const char* const* names, int n, int idx)
{
    m.opts.assign(names, names + n);
    m.optIdx = std::clamp(idx, 0, n - 1);
}
static MenuItem Header(const std::string& title)
{
    MenuItem m;
    m.label = title;
    m.header = true;
    return m;
}
// the next selectable item from 'from' going 'dir' (+1 / -1), wrapping around
static int NextSelectable(const std::vector<MenuItem>& items, int from, int dir)
{
    const int n = (int)items.size();
    if (n == 0) return 0;
    int i = from;
    for (int k = 0; k < n; k++)
    {
        i = ((i + dir) % n + n) % n;
        if (!items[i].header || items[i].activate) return i;
    }
    return from;
}
static int FirstSelectable(const std::vector<MenuItem>& items)
{
    for (int i = 0; i < (int)items.size(); i++) if (!items[i].header || items[i].activate) return i;
    return 0;
}

enum Page { P_Game, P_Saves, P_Trophies, P_Graphics, P_Screen, P_Sound, P_System, P_Controls, P_Look, P_About, P_Count };

struct Menu
{
    bool open = false;
    int page = P_Game, sel = 0, scroll = 0;
    int waitKey = -1;   // remapping: which button waits for a key
    int waitPad = -1;   // remapping: which button waits for a controller button
    int waitHot = -1;   // remapping: which shortcut waits for a key
    std::vector<Rect> tabRects, itemRects, valueRects;
    std::vector<Rect> leftRects, rightRects;   // the < > buttons of numeric settings
    Rect btnRects[2] {};                       // Continue, Quit game
    float mx = -1, my = -1;                    // mouse (window pixels) for the hover look
    int hover = -1;
    int popup = -1, popSel = 0, popScroll = 0; // the open dropdown (index in the visible list), its highlighted choice
    std::vector<Rect> popRects;                // its choices on screen
    Rect popBox {};
    double openT = 0, tabT = 0, popT = 0, lastT = 0;   // animations: menu opened, tab changed, dropdown opened
    int tabDir = 0;
    float selX = 0, selY = 0, selW = 0, selH = 0;      // the selection highlight (it glides to the selected row)
    bool selInit = false;
};
static Menu gMenu;

// The menu, toasts and FPS text are changed by the main thread and drawn by the
// presentation thread: both hold this lock while they use them.
static std::recursive_mutex gUiMutex;
static std::string gToast;
static Uint64 gToastUntil = 0;
static void Toast(const std::string& s)
{
    std::lock_guard<std::recursive_mutex> lk(gUiMutex);
    gToast = s;
    gToastUntil = SDL_GetTicks64() + 2500;
}

static std::string PageName(int p)
{
    static const char* pt[] = { "Jogo", "Saves", "Conquistas", "Gráficos", "Tela", "Som", "Sistema", "Controles", "Aparência", "Sobre" };
    static const char* en[] = { "Game", "Saves", "Achievements", "Graphics", "Screen", "Sound", "System", "Controls", "Look", "About" };
    return EN() ? en[p] : pt[p];
}

static std::string HotName(int h) { return SDL_GetScancodeName((SDL_Scancode)gCfg.hot[h]); }
static std::string WithKey(const std::string& s, int h) { return s + " (" + HotName(h) + ")"; }
static std::string OnOff(bool b) { return b ? T("Ligado", "On") : T("Desligado", "Off"); }
static std::function<void(int)> Toggle(const std::function<bool()>& get, const std::function<void(bool)>& set)
{
    return [get, set](int) { set(!get()); };
}
template <class E>
static std::function<void(int)> Cycle(Settings::SwitchableSetting<E>& s, int count, int first = 0)
{
    return [&s, count, first](int d) {
        int v = ((int)s.GetValue() - first + d + count) % count + first;
        s.SetValue((E)v);
    };
}

// ---------------------------------------------------------------------------
// achievements, read live from the game's memory: while it runs, the Alola games keep the save blocks in RAM (the
// save file is a copy written when the player saves), so the passport stamps and the Pokédex can be read every half
// second.
// ---------------------------------------------------------------------------
struct Achievement
{
    const char* id;
    const char* pt; const char* en;
    const char* descPt; const char* descEn;
    int kind, arg;   // 0 = passport stamp arg (emblem: island number arg), 1 = stamp arg (star), 2 = stamp arg (crown),
                     // 3 = Pokémon caught >= arg
};
static const Achievement kAch[] = {
    { "melemele", "Provações de Melemele", "Melemele trials", "Complete as provações de Melemele e vença o kahuna Hala",
      "Complete the Melemele trials and beat Kahuna Hala", 0, 1 },
    { "akala", "Provações de Akala", "Akala trials", "Complete as provações de Akala e vença a kahuna Olivia",
      "Complete the Akala trials and beat Kahuna Olivia", 0, 2 },
    { "ulaula", "Provações de Ula'ula", "Ula'ula trials", "Complete as provações de Ula'ula e vença o kahuna Nanu",
      "Complete the Ula'ula trials and beat Kahuna Nanu", 0, 3 },
    { "poni", "Provações de Poni", "Poni trials", "Complete as provações de Poni e vença a kahuna Hapu",
      "Complete the Poni trials and beat Kahuna Hapu", 0, 4 },
    { "campeao", "Campeão de Alola", "Alola Champion", "Complete o desafio das ilhas e vença a Liga Pokémon",
      "Complete the island challenge and win the Pokémon League", 2, 5 },
    { "dex_alola", "Pokédex de Alola completa", "Alola Pokédex complete", "Ganhe o selo da Pokédex de Alola no Passaporte de Treinador",
      "Earn the Alola Pokédex stamp in the Trainer Passport", 1, 10 },
    { "arvore", "Mestre da Árvore de Batalha", "Battle Tree master", "Vença 50 batalhas simples seguidas na Árvore de Batalha",
      "Win 50 single battles in a row at the Battle Tree", 1, 11 },
    { "pokedex10", "Primeiros passos", "First steps", "Capture 10 Pokémon diferentes", "Catch 10 different Pokémon", 3, 10 },
    { "pokedex50", "Colecionador", "Collector", "Capture 50 Pokémon diferentes", "Catch 50 different Pokémon", 3, 50 },
    { "pokedex100", "Pesquisador", "Researcher", "Capture 100 Pokémon diferentes", "Catch 100 different Pokémon", 3, 100 },
    { "pokedex200", "Professor Pokémon", "Pokémon Professor", "Capture 200 Pokémon diferentes", "Catch 200 different Pokémon", 3, 200 },
    { "pokedex400", "Lenda de Alola", "Alola Legend", "Capture 400 Pokémon diferentes", "Catch 400 different Pokémon", 3, 400 },
};
constexpr int kAchCount = (int)std::size(kAch);
static const int kAchTier[kAchCount] = { 0, 0, 1, 1,   // trials: Melemele, Akala bronze; Ula'ula, Poni silver
                                         2, 2, 2,      // champion, Alola Pokédex, Battle Tree
                                         0, 0, 1, 1, 2 };   // Pokédex 10, 50 bronze, 100, 200 silver, 400 gold
static Color AchTierColor(int i, bool dark = false)
{
    static const Color c[3][2] = { { { 0.85f, 0.55f, 0.30f, 1 }, { 0.56f, 0.33f, 0.15f, 1 } },     // bronze
                                   { { 0.86f, 0.88f, 0.92f, 1 }, { 0.55f, 0.58f, 0.65f, 1 } },     // silver
                                   { { 0.98f, 0.77f, 0.20f, 1 }, { 0.76f, 0.53f, 0.08f, 1 } } };   // gold
    return c[std::clamp(kAchTier[std::clamp(i, 0, kAchCount - 1)], 0, 2)][dark ? 1 : 0];
}
static std::string AchTierName(int i)
{
    switch (kAchTier[std::clamp(i, 0, kAchCount - 1)])
    {
    case 0: return T("Bronze", "Bronze");
    case 1: return T("Prata", "Silver");
    default: return T("Ouro", "Gold");
    }
}

// where the blocks are in the game's memory: the trainer block (MyStatus) is looked for in FCRAM (physical memory)
// and the others sit at fixed distances from it (measured with PORTE_DUMPRAMAT on Ultra Moon and Sun: in memory the
// blocks follow each other with small headers, not at the file's offsets). The two families differ only by the
// UnionPokemon block (0x104 / 0x30C bytes) between the Pokédex and Misc.
struct AlolaMem { u32 zukanRel, miscRel; int species; };
static const AlolaMem kMemSM { 0x14F0, 0x27EC, 802 };
static const AlolaMem kMemUSUM { 0x14F0, 0x29F4, 807 };
static const AlolaMem* MemOfTitle(u32 title)
{
    if (title == 0x00164800 || title == 0x00175E00) return &kMemSM;     // Sun, Moon
    if (title == 0x001B5000 || title == 0x001B5100) return &kMemUSUM;   // Ultra Sun, Ultra Moon
    return nullptr;
}
static const u32 kAchKnownMy[] = { 0x3012818, 0x30D67D0 };   // where it was: Ultra Moon, Sun (tried before a scan)

struct AchState
{
    std::map<std::string, long long> got;   // id -> when it was unlocked (time_t)
    bool loaded = false;
    bool live = false;                      // reading the game's memory right now
    u32 my = ~0u;                           // trainer block: offset in FCRAM (~0 = not found yet)
    int stamps = -1, caught = -1;
    std::vector<int> queue;                 // unlocked, waiting to be shown
    int showing = -1;                       // the one on screen (kAchCount = "several at once")
    int several = 0;
    Uint64 showSince = 0;
    Uint64 nextRead = 0, nextScan = 0;
};
static AchState gAch;   // the lists are read by the presenter thread: gUiMutex

static std::string AchPath() { return gDataDir + "conquistas" + (Jogos::gAtual ? Narrow(Jogos::gAtual->sufixo) : std::string()) + ".txt"; }
static std::string AchPicDir() { return gDataDir + "conquistas/"; }

// pictures chosen by the player: conquistas/<id>.png, fitted into 128 x 128 cells
static std::vector<u8> gAchPics;
static bool gAchHasPic[kAchCount] = {};
static void AchLoadPics()
{
    CreateDirectoryW(Widen(AchPicDir()).c_str(), nullptr);
    {
        const std::string readme = AchPicDir() + "LEIA-ME.txt";
        if (!FileUtil::Exists(readme))
        {
            std::ofstream f(P(readme));
            f << "Imagens das conquistas / Achievement pictures\n\n"
                 "Coloque aqui uma imagem PNG com o nome da conquista para usar no lugar do emblema.\n"
                 "Put a PNG named after the achievement here to use it instead of the emblem.\n"
                 "(vale ao abrir o jogo de novo / takes effect the next time the game opens)\n\n";
            for (int i = 0; i < kAchCount; i++) f << kAch[i].id << ".png   " << kAch[i].pt << " / " << kAch[i].en << "\n";
        }
    }
    const int W = Porte::kPicsW, C = Porte::kPicCell, per = W / C;
    gAchPics.assign((size_t)W * W * 4, 0);
    Frontend::ImageInterface img;
    for (int i = 0; i < kAchCount && i < per * per; i++)
    {
        std::ifstream f(P(AchPicDir() + kAch[i].id + ".png"), std::ios::binary);
        if (!f) continue;
        std::vector<u8> png((std::istreambuf_iterator<char>(f)), {}), px;
        u32 w = 0, h = 0;
        if (png.empty() || !img.DecodePNG(px, w, h, png) || !w || !h) { fprintf(stderr, "conquistas: imagem invalida: %s.png\n", kAch[i].id); continue; }
        // fit (keep the shape), centred; box filter when shrinking
        const float sc = std::min((float)C / w, (float)C / h);
        const int dw = std::max(1, (int)(w * sc)), dh = std::max(1, (int)(h * sc)), ox = (C - dw) / 2, oy = (C - dh) / 2;
        const int cx = (i % per) * C, cy = (i / per) * C;
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++)
            {
                const u32 x0 = (u32)(x / sc), x1 = std::max(x0 + 1, std::min(w, (u32)((x + 1) / sc)));
                const u32 y0 = (u32)(y / sc), y1 = std::max(y0 + 1, std::min(h, (u32)((y + 1) / sc)));
                u32 acc[4] = {}, n = 0;
                for (u32 yy = y0; yy < y1 && yy < h; yy++)
                    for (u32 xx = x0; xx < x1 && xx < w; xx++, n++)
                        for (int k = 0; k < 4; k++) acc[k] += px[((size_t)yy * w + xx) * 4 + k];
                u8* d = &gAchPics[((size_t)(cy + oy + y) * W + cx + ox + x) * 4];
                for (int k = 0; k < 4; k++) d[k] = n ? (u8)(acc[k] / n) : 0;
            }
        gAchHasPic[i] = true;
        fprintf(stderr, "conquistas: imagem de %s (%ux%u)\n", kAch[i].id, w, h);
    }
    Porte::gPics = gAchPics.data();
}

// gym badge colours (the type of each gym, in order)
static const Color kAchGym[8] = {
    { 0.60f, 0.76f, 0.20f, 1 },   // bug
    { 0.72f, 0.58f, 0.36f, 1 },   // rock
    { 0.86f, 0.36f, 0.24f, 1 },   // fighting
    { 0.30f, 0.74f, 0.36f, 1 },   // grass
    { 0.98f, 0.84f, 0.18f, 1 },   // electric
    { 0.96f, 0.56f, 0.82f, 1 },   // fairy
    { 0.90f, 0.36f, 0.62f, 1 },   // psychic
    { 0.56f, 0.86f, 0.96f, 1 },   // ice
};

// the emblem of an achievement in a square (x, y, s); grey when locked
static void AchIconDraw(int i, bool locked, float x, float y, float s, float alpha)
{
    if (i < 0 || i >= kAchCount) return;
    if (gAchHasPic[i])
    {
        const int per = Porte::kPicsW / Porte::kPicCell;
        const float c = (float)Porte::kPicCell / Porte::kPicsW;
        const float u0 = (i % per) * c, v0 = (i / per) * c;
        gUi.shape = locked ? 1.f : 0.f;
        Quad({ x, y, s, s }, 4, { 1, 1, 1, alpha }, u0, v0, u0 + c, v0 + c);
        return;
    }
    const Achievement& a = kAch[i];
    auto grey = [&](Color c) {
        if (!locked) return Color { c.r, c.g, c.b, alpha };
        const float l = (c.r * 0.3f + c.g * 0.59f + c.b * 0.11f) * 0.55f;
        return Color { l, l, l + 0.02f, alpha };
    };
    const Color main = grey(AchTierColor(i)), dark = grey(AchTierColor(i, true));
    const Color mark = grey({ AchTierColor(i, true).r * 0.55f, AchTierColor(i, true).g * 0.55f, AchTierColor(i, true).b * 0.55f, 1 });
    // the cup: handles, bowl, stem, base
    RoundRect({ x + s * 0.04f, y + s * 0.08f, s * 0.26f, s * 0.32f }, s * 0.13f, dark);
    RoundRect({ x + s * 0.70f, y + s * 0.08f, s * 0.26f, s * 0.32f }, s * 0.13f, dark);
    RoundRect({ x + s * 0.17f, y, s * 0.66f, s * 0.58f }, s * 0.25f, main);
    FillRect({ x + s * 0.17f, y, s * 0.66f, s * 0.18f }, main);
    FillRect({ x + s * 0.24f, y + s * 0.05f, s * 0.08f, s * 0.36f }, Color { 1, 1, 1, (locked ? 0.10f : 0.35f) * alpha });   // shine
    FillRect({ x + s * 0.43f, y + s * 0.56f, s * 0.14f, s * 0.18f }, dark);
    RoundRect({ x + s * 0.22f, y + s * 0.74f, s * 0.56f, s * 0.22f }, s * 0.06f, main);
    FillRect({ x + s * 0.22f, y + s * 0.86f, s * 0.56f, s * 0.04f }, dark);
    // what it is, on the bowl: gym number, star (all badges), crown (champion), Pokédex count
    const float bx = x + s * 0.30f, by = y + s * 0.07f, bw = s * 0.40f;
    auto shape = [&](int id, const Rect& r, Color c) { gUi.shape = (float)id; Quad(r, 3, c); };
    if (a.kind == 0 || a.kind == 3)
    {
        const std::string n = std::to_string(a.arg);
        const float ls = Fit(n, s * 0.40f, bw);
        Text(n, x + s / 2, y + s * 0.27f - ls * 0.54f, ls, mark, 1);
    }
    else if (a.kind == 1) shape(4, { bx, by, bw, bw }, mark);
    else
    {
        const float cw = bw, cx = bx, cy = by + bw * 0.62f;
        for (int k = 0; k < 3; k++)
        {
            const float tw = cw * 0.46f, tx = cx + k * (cw - tw) / 2, th = k == 1 ? bw * 0.5f : bw * 0.4f;
            shape(5, { tx, cy - th + bw * 0.06f, tw, th }, mark);
        }
        FillRect({ cx, cy, cw, bw * 0.16f }, mark);
    }
}

static std::string AchName(int i) { return EN() ? kAch[i].en : kAch[i].pt; }
static std::string AchDesc(int i) { return EN() ? kAch[i].descEn : kAch[i].descPt; }

static void AchLoad()
{
    std::lock_guard<std::recursive_mutex> lk(gUiMutex);
    gAch.loaded = true;
    std::ifstream f(P(AchPath()));
    for (std::string line; std::getline(f, line);)
        if (auto eq = line.find('='); eq != std::string::npos && line[0] != '#')
            gAch.got[line.substr(0, eq)] = atoll(line.c_str() + eq + 1);
}
static void AchSave()
{
    if (getenv("PORTE_HIDDEN") && !getenv("PORTE_ACHSAVE")) return;   // background tests keep the file as it is
    std::ofstream f(P(AchPath()));
    f << "# conquistas desbloqueadas: id=data (segundos desde 1970)\n";
    for (auto& [id, t] : gAch.got) f << id << "=" << t << "\n";
}

// is there a trainer block at FCRAM offset 'off'? (game 30..33, a name, money that makes sense; same trainer ID and
// name as the save file when there is one)
static bool AchCheckTrainer(const u8* fc, size_t fcSize, u32 off, const AlolaMem& M, const std::vector<u8>& file)
{
    if ((size_t)off + M.miscRel + 0x10 > fcSize || (size_t)off + M.zukanRel + 0x88 + 0x80 > fcSize) return false;
    const u8* b = fc + off;
    if (b[4] < 30 || b[4] > 33) return false;
    const u16 c0 = b[0x38] | (b[0x39] << 8);
    if (c0 == 0 || c0 == 0xFFFF) return false;
    const u8* m = b + M.miscRel;
    const u32 money = m[4] | (m[5] << 8) | (m[6] << 16) | ((u32)m[7] << 24);
    if (money > 9999999) return false;
    if (const SaveLayout* L = LayoutOfFile(file.size()))
        return memcmp(b, &file[L->my], 4) == 0 && memcmp(b + 0x38, &file[L->my + 0x38], 26) == 0;
    return true;
}

static void AchUnlock(const std::vector<int>& now, bool quiet)
{
    std::lock_guard<std::recursive_mutex> lk(gUiMutex);
    const long long t = (long long)std::time(nullptr);
    for (int i : now) gAch.got[kAch[i].id] = t;
    AchSave();
    if (quiet) { gAch.several = (int)now.size(); gAch.queue.push_back(kAchCount); }   // one card: "N achievements"
    else gAch.queue.insert(gAch.queue.end(), now.begin(), now.end());
    for (int i : now) fprintf(stderr, "conquista: %s (t=%.1f)\n", kAch[i].id, SDL_GetTicks64() / 1000.0);
}

// main thread, every pass of the loop
static void AchTick(Core::System& system)
{
    const Uint64 now = SDL_GetTicks64();
    if (now < gAch.nextRead) return;
    gAch.nextRead = now + 500;
    const AlolaMem* M = MemOfTitle((u32)(gProgramId & 0xFFFFFFFF));
    if (!system.IsPoweredOn() || !M) { gAch.live = false; return; }
    if (!gAch.loaded) AchLoad();
    auto& mem = system.Memory();
    const u8* fc = mem.GetFCRAMPointer(0);
    const size_t fcSize = Settings::values.is_new_3ds.GetValue() ? Memory::FCRAM_N3DS_SIZE : Memory::FCRAM_SIZE;
    if (!fc) return;

    std::vector<u8> file;
    {
        std::ifstream f(P(GameSavePath()), std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (gAch.my == ~0u || !AchCheckTrainer(fc, fcSize, gAch.my, *M, file))
    {
        gAch.live = false;
        gAch.my = ~0u;
        for (u32 k : kAchKnownMy)
            if (AchCheckTrainer(fc, fcSize, k, *M, file)) { gAch.my = k; break; }
        const SaveLayout* L = LayoutOfFile(file.size());
        if (gAch.my == ~0u && L && now >= gAch.nextScan)
        {
            // not where it was: look for the save file's trainer (ID + name) in all of FCRAM (~30 ms, at most every 10 s)
            gAch.nextScan = now + 10000;
            u32 key;
            memcpy(&key, &file[L->my], 4);
            for (size_t o = 0; o + 0x100 < fcSize && gAch.my == ~0u; o += 4)
            {
                u32 v;
                memcpy(&v, fc + o, 4);
                if (v == key && AchCheckTrainer(fc, fcSize, (u32)o, *M, file)) gAch.my = (u32)o;
            }
            if (gAch.my != ~0u) fprintf(stderr, "conquistas: treinador achado na FCRAM +%07X\n", (unsigned)gAch.my);
        }
        if (gAch.my == ~0u) return;
    }

    u8* misc = mem.GetFCRAMPointer(gAch.my + M->miscRel);
    // testing aid: PORTE_ACHTEST=A,B takes the champion stamp out of the game's memory at A seconds and gives it back
    // at B (the card must show up only then: the reading is live)
    if (const char* at = getenv("PORTE_ACHTEST"))
    {
        static int step = 0;
        const double t = now / 1000.0, a = atof(at), b = strchr(at, ',') ? atof(strchr(at, ',') + 1) : a + 4;
        if (step == 0 && t >= a) { misc[9] &= ~0x02; step = 1; fprintf(stderr, "teste: selo de campeão tirado da memoria (t=%.1f)\n", t); }
        if (step == 1 && t >= b) { misc[9] |= 0x02; step = 2; fprintf(stderr, "teste: selo de campeão devolvido (t=%.1f)\n", t); }
    }
    const int stamps = PassportStamps(misc[8] | (misc[9] << 8) | (misc[10] << 16) | ((u32)misc[11] << 24));
    const int caught = CountBits(fc + gAch.my + M->zukanRel + 0x88, M->species);
    {
        std::lock_guard<std::recursive_mutex> lk(gUiMutex);
        gAch.live = true;
        gAch.stamps = stamps;
        gAch.caught = caught;
    }

    std::vector<int> earned;
    for (int i = 0; i < kAchCount; i++)
    {
        const Achievement& a = kAch[i];
        const bool ok = a.kind == 3 ? caught >= a.arg : ((stamps >> a.arg) & 1) != 0;
        if (ok && !gAch.got.count(a.id)) earned.push_back(i);
    }
    if (earned.empty()) return;
    // the first reading of a game already in progress: one card, not a dozen
    const bool quiet = earned.size() > 3;
    AchUnlock(earned, quiet);
}

// the progress shown for a locked achievement
static std::string AchProgress(int i)
{
    const Achievement& a = kAch[i];
    if (!gAch.live) return "";
    if (a.kind == 3) return std::to_string(std::min(gAch.caught, a.arg)) + "/" + std::to_string(a.arg);
    if (a.kind == 2) return std::to_string(__builtin_popcount((gAch.stamps >> 1) & 0xF)) + T("/4 ilhas", "/4 islands");
    return "";
}

struct App;   // forward
static void ApplyLive();
static void SaveState(int slot);
static void LoadState(int slot);
static std::vector<MenuItem> BuildItems(int page);

static int MonitorHz();   // the monitor refresh rate (defined with the presentation code)

// ---------------------------------------------------------------------------
// the running game
// ---------------------------------------------------------------------------
static SdlWindow gWindow;
static SdlWindow gWindow2{ true };   // two monitors: always created (hidden), the core gets it at start
static bool gHiddenRun = false;      // background tests: the second window is never shown
static bool gRunning = true;
static std::atomic<bool> gTurbo{false};
static std::string gFpsText;

static void SetFullscreen(bool on)
{
    gCfg.fullscreen = on;
    // exclusive: the monitor keeps its own resolution (SDL would pick the window's size otherwise)
    if (on && gCfg.fsMode == 1)
    {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(std::max(0, SDL_GetWindowDisplayIndex(gWindow.win)), &dm) == 0) SDL_SetWindowDisplayMode(gWindow.win, &dm);
    }
    const int rc = SDL_SetWindowFullscreen(gWindow.win, on ? (gCfg.fsMode == 1 ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP) : 0);
    if (getenv("PORTE_FS_LOG"))   // testing aid: what the window and the monitor really are now
    {
        SDL_PumpEvents();
        const Uint32 fl = SDL_GetWindowFlags(gWindow.win);
        int w = 0, h = 0;
        SDL_GetWindowSize(gWindow.win, &w, &h);
        SDL_DisplayMode cur {};
        SDL_GetCurrentDisplayMode(std::max(0, SDL_GetWindowDisplayIndex(gWindow.win)), &cur);
        fprintf(stderr, "telacheia: pedido=%s modo=%s rc=%d -> %s, janela %dx%d, monitor %dx%d@%dHz\n", on ? "ligar" : "desligar",
                gCfg.fsMode == 1 ? "exclusiva" : "sem_bordas", rc,
                (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP ? "SEM_BORDAS" : (fl & SDL_WINDOW_FULLSCREEN) ? "EXCLUSIVA" : "JANELA",
                w, h, cur.w, cur.h, cur.refresh_rate);
    }
}

// ---- two monitors: the other 3DS screen in a window of its own --------------------------
static bool IsFull(SDL_Window* w) { return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0; }   // borderless or exclusive

// another program has the focus: pause and / or silence the game (Sistema / Som options); fast-forward can mute too
static bool gUnfocusedPause = false;
static void UpdateFocusEffects()
{
    bool focused = gHiddenRun || SDL_GetKeyboardFocus() != nullptr;   // (either of our windows)
    static const double semFoco = getenv("PORTE_SEMFOCO_AT") ? atof(getenv("PORTE_SEMFOCO_AT")) : -1;   // testing aid
    if (semFoco >= 0 && SDL_GetTicks64() >= (Uint64)(semFoco * 1000)) focused = false;
    gUnfocusedPause = gCfg.pauseUnfocused && !focused;
    static bool lastPause = false, lastFocus = true;
    if (gHiddenRun && (lastPause != gUnfocusedPause || lastFocus != focused))
        fprintf(stderr, "foco: %s, pausado=%d\n", focused ? "sim" : "nao", (int)gUnfocusedPause);
    lastPause = gUnfocusedPause; lastFocus = focused;
    const bool mute = gUnfocusedPause || (gCfg.muteUnfocused && !focused) || (gCfg.muteTurbo && gTurbo);
    auto& vol = Settings::values.volume;
    if (mute && gMuteSaved < 0) { gMuteSaved = vol.GetValue(); vol.SetValue(0.f); }
    else if (!mute && gMuteSaved >= 0) { vol.SetValue(gMuteSaved); gMuteSaved = -1; }
}
// Sistema > Prioridade do processo
static void ApplyPriority()
{
    static int last = -1;
    if (gCfg.priority == last) return;
    last = gCfg.priority;
    SetPriorityClass(GetCurrentProcess(), last == 2 ? HIGH_PRIORITY_CLASS : last == 1 ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
}
static bool Window2Shown() { return gWindow2.win && (SDL_GetWindowFlags(gWindow2.win) & SDL_WINDOW_SHOWN); }

static void RememberWindow2()
{
    if (!Window2Shown() || IsFull(gWindow2.win)) return;
    SDL_GetWindowPosition(gWindow2.win, &gCfg.win2x, &gCfg.win2y);
    SDL_GetWindowSize(gWindow2.win, &gCfg.win2w, &gCfg.win2h);
}

// Creates the second window (hidden): where it was last time, else in the middle of the
// second monitor (or of the only one), 4:3 like the bottom screen.
static bool CreateWindow2()
{
    int w = gCfg.win2w, h = gCfg.win2h, x = gCfg.win2x, y = gCfg.win2y;
    if (w < 320 || h < 240)
    {
        h = std::clamp(gCfg.winH / 2, 480, 900);
        w = h * 4 / 3;
    }
    SDL_Rect d;
    const int display = SDL_GetNumVideoDisplays() > 1 ? 1 : 0;
    bool onScreen = false;   // the saved place still on a monitor (one may have been unplugged)
    for (int i = 0; i < SDL_GetNumVideoDisplays(); i++)
        if (SDL_GetDisplayBounds(i, &d) == 0 && x + w / 2 >= d.x && x + w / 2 < d.x + d.w && y + 20 >= d.y && y + 20 < d.y + d.h) onScreen = true;
    if (x == -100000 || !onScreen)
    {
        if (SDL_GetDisplayUsableBounds(display, &d) == 0) { x = d.x + (d.w - w) / 2; y = d.y + (d.h - h) / 2; }
        else { x = SDL_WINDOWPOS_CENTERED; y = SDL_WINDOWPOS_CENTERED; }
    }
    const std::string title = std::string(kGameName) + (EN() ? " - second screen" : " - segunda tela");
    if (!gWindow2.Create(w, h, true, title.c_str(), x, y)) return false;
    SDL_SetWindowMinimumSize(gWindow2.win, 320, 240);
    return true;
}

// Shows / hides the second window and switches the core to "separate windows" (and back).
static void SyncDual()
{
    auto& lo = Settings::values.layout_option;
    if (gCfg.dualWindow)
    {
        if (lo.GetValue() != Settings::LayoutOption::SeparateWindows)
        {
            gCfg.layoutBeforeDual = (int)lo.GetValue();
            lo.SetValue(Settings::LayoutOption::SeparateWindows);
        }
        if (gWindow2.win && !gHiddenRun && !Window2Shown()) SDL_ShowWindow(gWindow2.win);
    }
    else
    {
        if (lo.GetValue() == Settings::LayoutOption::SeparateWindows) lo.SetValue((Settings::LayoutOption)gCfg.layoutBeforeDual);
        if (Window2Shown())
        {
            RememberWindow2();
            if (IsFull(gWindow2.win)) SDL_SetWindowFullscreen(gWindow2.win, 0);
            SDL_HideWindow(gWindow2.win);
        }
    }
    if (gWindow2.win) gWindow2.Resized();
}

// F11: the window the keyboard is on (with two windows, each goes full screen on its own monitor)
static void ToggleFullscreenFocused()
{
    if (Window2Shown() && SDL_GetKeyboardFocus() == gWindow2.win)
    {
        const bool on = IsFull(gWindow2.win);
        if (!on) RememberWindow2();
        SDL_SetWindowFullscreen(gWindow2.win, on ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }
    SetFullscreen(!gCfg.fullscreen);
}

static bool gInBattle = false;   // the Battle module is loaded (scene detector, every pass)
static void ApplyTurbo()
{
    const bool on = (gTurbo && (!gCfg.turboOnlyBattle || gInBattle)) || (gCfg.turboBattle && gInBattle);
    Settings::is_temporary_frame_limit = on;
    Settings::temporary_frame_limit = Settings::values.turbo_limit.GetValue();
    // Drawing at a high resolution is what limits fast-forward (measured: about 150 %
    // at 3x, 260 % at 2x, 350 % at 1x), so it draws at gCfg.turboRes while it is on.
    auto& res = Settings::values.resolution_factor;
    if (on && gCfg.turboRes > 0 && gResBeforeTurbo < 0)
    {
        const int now = (int)res.GetValue();
        const int effective = now > 0 ? now : (int)std::max(1u, gWindow.GetFramebufferLayout().GetScalingRatio());
        if (effective > gCfg.turboRes)
        {
            gResBeforeTurbo = now;
            res.SetValue(gCfg.turboRes);
        }
    }
    else if (!on && gResBeforeTurbo >= 0)
    {
        res.SetValue(gResBeforeTurbo);
        gResBeforeTurbo = -1;
    }
}

// Settings that take effect at once. Azahar's System::ApplySettings() is NOT used
// here: it recreates the audio device every time (the sound cuts out and the game
// stalls for a moment) and reloads camera, microphone and plugins. Only the part
// each setting needs is applied, and only when it changed.
// real 16:9 on / off: the projection factor for the renderer and the top screen's width for the layouts
namespace VideoCore { extern float g_porte_wide_proj; extern float g_porte_brightness, g_porte_inv_gamma, g_porte_saturation, g_porte_sharp; }
static void ApplyWide()
{
    VideoCore::g_porte_wide_proj = gCfg.widescreen && !getenv("PORTE_16X9_SEMPROJ") ? 400.0f / (240.0f * 16.0f / 9.0f) : 1.0f;   // (testing aid: wide layout only)
    Layout::g_porte_top_wide = gCfg.widescreen ? (240.0f * 16.0f / 9.0f) / 400.0f : 1.0f;
}
static void ApplyLive()
{
    ApplyWide();
    VideoCore::g_porte_brightness = gCfg.brightness / 100.f;   // Gráficos > Brilho / Gama (the present shader)
    VideoCore::g_porte_inv_gamma = 100.f / gCfg.gamma;
    VideoCore::g_porte_saturation = gCfg.saturation / 100.f;   // Gráficos > Saturação / Nitidez / FXAA (the present shader)
    VideoCore::g_porte_sharp = gCfg.sharpness / 100.f * 0.6f + (gCfg.fxaa ? 10.f : 0.f);
#ifdef PORTE_SEM_GERADOR
    InterpRec::SetAutoBudget(gCfg.autoBudget);
#endif
    ApplyPriority();
    SyncDual();
    ApplyBgColor();
    ApplyKeys();
    auto& system = Core::System::GetInstance();
    if (system.IsPoweredOn())
    {
        auto& rs = system.GPU().Renderer().Settings();
        static float lastBg[3] = { -1, -1, -1 };
        const float bg[3] = { Settings::values.bg_red.GetValue(), Settings::values.bg_green.GetValue(), Settings::values.bg_blue.GetValue() };
        if (!std::equal(bg, bg + 3, lastBg)) { std::copy(bg, bg + 3, lastBg); rs.bg_color_update_requested = true; }
        system.GPU().Renderer().UpdateCurrentFramebufferLayout();
        static int lastClock = -1;
        if (Settings::values.cpu_clock_percentage.GetValue() != lastClock)
        {
            lastClock = Settings::values.cpu_clock_percentage.GetValue();
            system.CoreTiming().UpdateClockSpeed(lastClock);
        }
        system.DSP().EnableStretching(Settings::values.enable_audio_stretching.GetValue());
        // controls: reload only when a key changed
        static std::string lastKeys;
        std::string keys;
        for (const auto& k : Settings::values.current_input_profile.buttons) keys += k + "|";
        for (const auto& k : Settings::values.current_input_profile.analogs) keys += k + "|";
        keys += Settings::values.current_input_profile.touch_device;
        if (keys != lastKeys)
        {
            lastKeys = keys;
            if (auto hid = Service::HID::GetModule(system)) hid->ReloadInputDevices();
            if (auto apt = Service::APT::GetModule(system)) apt->GetAppletManager()->ReloadInputDevices();
        }
    }
    gWindow.Resized();
    ApplyTurbo();
    SaveConfig();
}

static void SaveState(int slot)
{
    Core::System::GetInstance().SendSignal(Core::System::Signal::Save, (u32)slot);
    Toast(T("Estado salvo no slot ", "State saved to slot ") + std::to_string(slot));
}
static void LoadState(int slot)
{
    auto info = Core::GetSaveStateInfo(gProgramId, 0, (u32)slot);
    if (info.time == 0) { Toast(T("O slot ", "Slot ") + std::to_string(slot) + T(" está vazio", " is empty")); return; }
    Core::System::GetInstance().SendSignal(Core::System::Signal::Load, (u32)slot);
    Toast(T("Estado carregado do slot ", "State loaded from slot ") + std::to_string(slot));
}
static std::string SlotText(int slot)
{
    auto info = Core::GetSaveStateInfo(gProgramId, 0, (u32)slot);
    if (info.time == 0) return T("vazio", "empty");
    time_t t = (time_t)info.time;
    char b[32];
    strftime(b, sizeof(b), "%d/%m %H:%M", localtime(&t));
    return b;
}

// screenshot: the core fills a BGRA buffer (bottom-up), which is a 32-bit BMP as is
static std::vector<u8> gShot;
static u32 gShotW = 0, gShotH = 0;
static std::atomic<bool> gShotReady{false};
static bool gShotPending = false;
static std::string gShotFile;
static void RequestShot(const std::string& file)
{
    if (gShotPending) return;
    PorteLog::Mark("captura " + file);
    auto layout = gWindow.GetFramebufferLayout();
    if (gCfg.shotHiRes)
    {
        // Jogo > Capturas em alta resolução: only the top screen, at the internal resolution (16:9 when it is on)
        const u32 r = Settings::values.resolution_factor.GetValue() ? Settings::values.resolution_factor.GetValue() : std::max(1u, layout.GetScalingRatio());
        const u32 w = (u32)std::lround(400.0 * Layout::g_porte_top_wide * r), h = 240 * r;
        layout = Layout::SingleFrameLayout(w, h, false, false);
    }
    gShotW = layout.width;
    gShotH = layout.height;
    gShot.assign((size_t)gShotW * gShotH * 4, 0);
    gShotPending = true;
    gShotReady = false;
    gShotFile = file;
    Core::System::GetInstance().GPU().Renderer().RequestScreenshot(gShot.data(), [](bool) { gShotReady = true; }, layout);
}
static void FinishShot()
{
    if (!gShotPending || !gShotReady) return;
    gShotPending = false;
    FILE* f = _wfopen(Widen(gShotFile).c_str(), L"wb");
    if (!f) return;
    const u32 size = (u32)gShot.size();
    u8 h[54] = { 'B', 'M' };
    auto put32 = [&](int at, u32 v) { memcpy(h + at, &v, 4); };
    put32(2, 54 + size); put32(10, 54); put32(14, 40); put32(18, gShotW); put32(22, gShotH);
    h[26] = 1; h[28] = 32; put32(34, size);
    fwrite(h, 1, 54, f);
    // (the Vulkan renderer hands the rows top-down; a BMP is bottom-up: written from the last row - the pictures came
    // out upside down before)
    const size_t row = (size_t)gShotW * 4;
    for (u32 y = gShotH; y-- > 0;) fwrite(gShot.data() + y * row, 1, row, f);
    fclose(f);
    if (!getenv("PORTE_HIDDEN")) Toast(T("Captura salva em ", "Screenshot saved to ") + gShotFile.substr(gShotFile.find_last_of('\\') + 1));
}
// ---------------------------------------------------------------------------
// Modo foto (Jogo > Modo foto, F9): the game pauses and its last picture is redrawn by the interpolation's re-execution
// with a free camera (interp_rec: SetPhoto / PhotoFrame). WASD move, Space / Ctrl up / down (Shift: faster), arrows or the
// right mouse button turn, the wheel zooms, H shows / hides the interface, Backspace back to the game's camera, F12 takes
// the picture, F9 or Esc leave. Only the top screen is shown meanwhile. Needs the interpolation (it records the picture).
// ---------------------------------------------------------------------------
struct PhotoState
{
    bool on = false;
    float yaw = 0, pitch = 0, zoom = 1;
    float pos[3] = { 0, 0, 0 };   // where the camera moved, in the game camera's own axes
    bool hideHud = true;
    int layoutBefore = -1;
    double hintUntil = 0, lastT = 0;
    bool dragging = false;
};
static PhotoState gPhoto;
static double NowSec();
static void PhotoApply()
{
    // view' = R^T (view - pos): the camera turned by yaw (around its up axis) and pitch (around its side axis), moved by pos
    const float cy = std::cos(gPhoto.yaw), sy = std::sin(gPhoto.yaw), cp = std::cos(gPhoto.pitch), sp = std::sin(gPhoto.pitch);
    // R = Ry(yaw) * Rx(pitch); its transpose undoes it
    const float R[3][3] = { { cy, sy * sp, sy * cp }, { 0, cp, -sp }, { -sy, cy * sp, cy * cp } };
    InterpRec::Photo ph;
    ph.on = gPhoto.on;
    ph.hideHud = gPhoto.hideHud;
    ph.zoom = gPhoto.zoom;
    for (int i = 0; i < 3; i++)
    {
        float t = 0;
        for (int k = 0; k < 3; k++)
        {
            ph.view[i][k] = R[k][i];
            t += R[k][i] * gPhoto.pos[k];
        }
        ph.view[i][3] = -t;
    }
    InterpRec::SetPhoto(ph);
}
static void PhotoReset() { gPhoto.yaw = gPhoto.pitch = 0; gPhoto.zoom = 1; gPhoto.pos[0] = gPhoto.pos[1] = gPhoto.pos[2] = 0; }
static void ApplyLive();
static void PhotoPad(float dt, float speed, float turn, float mv[3]);   // (the controller: defined after gPad)
static void StartPhoto()
{
    if (gPhoto.on) return;
    if (!InterpRec::PhotoAvailable())
    {
        Toast(T("Modo foto: ligue a interpolação (Gráficos > Fluidez) e reinicie o jogo", "Photo mode: turn on the interpolation (Graphics > Smoothness) and restart the game"));
        return;
    }
    gPhoto.on = true;
    PhotoReset();
    gPhoto.hintUntil = NowSec() + 6.0;
    gPhoto.lastT = NowSec();
    auto& lo = Settings::values.layout_option;
    gPhoto.layoutBefore = (int)lo.GetValue();
    if (gLayoutKeep < 0) gLayoutKeep = gPhoto.layoutBefore;   // (config.ini keeps the player's layout)
    if (!gCfg.dualWindow) { lo.SetValue(Settings::LayoutOption::SingleScreen); ApplyLive(); }
    InputCommon::GetKeyboard()->ReleaseAllKeys();
    PhotoApply();
    fprintf(stderr, "modo foto: ligado\n");
}
static void StopPhoto()
{
    if (!gPhoto.on) return;
    gPhoto.on = false;
    PhotoApply();          // (photo off)
    InterpRec::PhotoFrame();   // the game's own picture back on the screen
    if (gPhoto.layoutBefore >= 0 && !gCfg.dualWindow) { Settings::values.layout_option.SetValue((Settings::LayoutOption)gPhoto.layoutBefore); ApplyLive(); }
    gLayoutKeep = -1;
    gPhoto.layoutBefore = -1;
    InputCommon::GetKeyboard()->ReleaseAllKeys();
    fprintf(stderr, "modo foto: desligado\n");
}
// every pass of the main loop while it is on: the held keys move the camera, then the picture is redrawn
static void PhotoTick()
{
    const double now = NowSec();
    const float dt = (float)std::clamp(now - gPhoto.lastT, 0.0, 0.1);
    gPhoto.lastT = now;
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    const bool fast = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
    const float speed = (fast ? 600.f : 150.f) * dt / gPhoto.zoom;   // (game units per second)
    const float turn = 1.2f * dt / gPhoto.zoom;
    if (k[SDL_SCANCODE_LEFT]) gPhoto.yaw += turn;
    if (k[SDL_SCANCODE_RIGHT]) gPhoto.yaw -= turn;
    if (k[SDL_SCANCODE_UP]) gPhoto.pitch += turn;
    if (k[SDL_SCANCODE_DOWN]) gPhoto.pitch -= turn;
    gPhoto.pitch = std::clamp(gPhoto.pitch, -1.5f, 1.5f);
    float mv[3] = { 0, 0, 0 };   // in the turned camera's axes: x right, y up, z back
    if (k[SDL_SCANCODE_W]) mv[2] -= speed;
    if (k[SDL_SCANCODE_S]) mv[2] += speed;
    if (k[SDL_SCANCODE_A]) mv[0] -= speed;
    if (k[SDL_SCANCODE_D]) mv[0] += speed;
    if (k[SDL_SCANCODE_SPACE]) mv[1] += speed;
    if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]) mv[1] -= speed;
    PhotoPad(dt, speed, turn, mv);
    if (!gPhoto.on) return;   // (B on the controller left the photo mode)
    if (mv[0] || mv[1] || mv[2])
    {
        const float cy = std::cos(gPhoto.yaw), sy = std::sin(gPhoto.yaw), cp = std::cos(gPhoto.pitch), sp = std::sin(gPhoto.pitch);
        const float R[3][3] = { { cy, sy * sp, sy * cp }, { 0, cp, -sp }, { -sy, cy * sp, cy * cp } };
        for (int i = 0; i < 3; i++) gPhoto.pos[i] += R[i][0] * mv[0] + R[i][1] * mv[1] + R[i][2] * mv[2];
    }
    PhotoApply();
    InterpRec::PhotoFrame();
}
// the photo mode's own keys and mouse (true: used here, not by the game / menu)
static bool PhotoEvent(const SDL_Event& e)
{
    if (!gPhoto.on) return false;
    switch (e.type)
    {
    case SDL_KEYDOWN:
        switch (e.key.keysym.scancode)
        {
        case SDL_SCANCODE_ESCAPE: StopPhoto(); return true;
        case SDL_SCANCODE_H: if (!e.key.repeat) gPhoto.hideHud = !gPhoto.hideHud; return true;
        case SDL_SCANCODE_BACKSPACE: PhotoReset(); return true;
        case SDL_SCANCODE_W: case SDL_SCANCODE_A: case SDL_SCANCODE_S: case SDL_SCANCODE_D: case SDL_SCANCODE_SPACE:
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT:
        case SDL_SCANCODE_UP: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_LEFT: case SDL_SCANCODE_RIGHT:
            return true;   // (read every pass in PhotoTick)
        default: return false;   // shortcuts (F12 picture, F9 leave...) go on
        }
    case SDL_KEYUP: return true;
    case SDL_MOUSEWHEEL: gPhoto.zoom = std::clamp(gPhoto.zoom * (e.wheel.y > 0 ? 1.1f : 1 / 1.1f), 0.5f, 6.0f); return true;
    case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_RIGHT) gPhoto.dragging = true; return true;
    case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_RIGHT) gPhoto.dragging = false; return true;
    case SDL_MOUSEMOTION:
        if (gPhoto.dragging)
        {
            gPhoto.yaw -= e.motion.xrel * 0.004f / gPhoto.zoom;
            gPhoto.pitch = std::clamp(gPhoto.pitch - e.motion.yrel * 0.004f / gPhoto.zoom, -1.5f, 1.5f);
        }
        return true;
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// Tela > Personalizar posição das telas: the custom layout edited with the mouse (drag = move, wheel = size, the
// shape stays 5:3 / 4:3), Enter saves, Esc puts back what it was. It starts from the layout on the screen.
// ---------------------------------------------------------------------------
struct LayoutEdit
{
    bool on = false;
    int drag = -1;              // 0 = top screen, 1 = bottom, -1 = none
    float grabX = 0, grabY = 0; // where it was grabbed, inside the screen
    u16 saved[8] = {};          // the custom rectangles before (Esc)
    int layoutBefore = 0;
};
static LayoutEdit gEdit;
static void EditGet(u16 r[8])
{
    auto& v = Settings::values;
    r[0] = v.custom_top_x.GetValue(); r[1] = v.custom_top_y.GetValue(); r[2] = v.custom_top_width.GetValue(); r[3] = v.custom_top_height.GetValue();
    r[4] = v.custom_bottom_x.GetValue(); r[5] = v.custom_bottom_y.GetValue(); r[6] = v.custom_bottom_width.GetValue(); r[7] = v.custom_bottom_height.GetValue();
}
static void EditSet(const u16 r[8])
{
    auto& v = Settings::values;
    v.custom_top_x.SetValue(r[0]); v.custom_top_y.SetValue(r[1]); v.custom_top_width.SetValue(r[2]); v.custom_top_height.SetValue(r[3]);
    v.custom_bottom_x.SetValue(r[4]); v.custom_bottom_y.SetValue(r[5]); v.custom_bottom_width.SetValue(r[6]); v.custom_bottom_height.SetValue(r[7]);
}
static void StartLayoutEdit()
{
    if (gCfg.dualWindow) { Toast(T("Com duas janelas cada tela fica na sua", "With two windows each screen has its own")); return; }
    auto& lo = Settings::values.layout_option;
    gEdit.layoutBefore = (int)lo.GetValue();
    EditGet(gEdit.saved);
    if (lo.GetValue() != Settings::LayoutOption::CustomLayout)
    {
        // start from what is on the screen now
        const auto& L = gWindow.GetFramebufferLayout();
        const auto& t = L.top_screen;
        const auto& b = L.bottom_screen;
        u16 r[8] = { (u16)t.left, (u16)t.top, (u16)std::max(40u, t.right - t.left), (u16)std::max(24u, t.bottom - t.top),
                     (u16)b.left, (u16)b.top, (u16)std::max(32u, b.right - b.left), (u16)std::max(24u, b.bottom - b.top) };
        if (L.bottom_screen.right <= L.bottom_screen.left)   // (only one screen shown: the bottom one gets a corner)
        {
            r[6] = (u16)(L.width / 4); r[7] = (u16)(r[6] * 3 / 4); r[4] = (u16)(L.width - r[6] - 10); r[5] = (u16)(L.height - r[7] - 10);
        }
        EditSet(r);
        lo.SetValue(Settings::LayoutOption::CustomLayout);
        ApplyLive();
    }
    gEdit.on = true;
    gEdit.drag = -1;
}
static void StopLayoutEdit(bool keep)
{
    if (!gEdit.on) return;
    gEdit.on = false;
    gEdit.drag = -1;
    if (!keep)
    {
        EditSet(gEdit.saved);
        Settings::values.layout_option.SetValue((Settings::LayoutOption)gEdit.layoutBefore);
    }
    ApplyLive();   // (saves config.ini)
    Toast(keep ? T("Posição das telas salva (Tela > Disposição: Personalizada)", "Screens' places saved (Screen > Layout: Custom)") : T("Posição das telas cancelada", "Screens' places cancelled"));
}
// its mouse and keys (true: used here)
static bool LayoutEditEvent(const SDL_Event& e)
{
    if (!gEdit.on) return false;
    if (e.type == SDL_KEYDOWN)
    {
        if (e.key.keysym.scancode == SDL_SCANCODE_RETURN || e.key.keysym.scancode == SDL_SCANCODE_KP_ENTER) { StopLayoutEdit(true); return true; }
        if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) { StopLayoutEdit(false); return true; }
        return false;
    }
    if (e.type != SDL_MOUSEBUTTONDOWN && e.type != SDL_MOUSEBUTTONUP && e.type != SDL_MOUSEMOTION && e.type != SDL_MOUSEWHEEL) return false;
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    float x = 0, y = 0;
    gWindow.ToDrawable(mx, my, x, y);
    u16 r[8];
    EditGet(r);
    auto inside = [&](int k) { return x >= r[k * 4] && x < r[k * 4] + r[k * 4 + 2] && y >= r[k * 4 + 1] && y < r[k * 4 + 1] + r[k * 4 + 3]; };
    const int under = inside(1) ? 1 : inside(0) ? 0 : -1;   // (the bottom screen is drawn over the top one)
    const auto& L = gWindow.GetFramebufferLayout();
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && under >= 0)
    {
        gEdit.drag = under;
        gEdit.grabX = x - r[under * 4];
        gEdit.grabY = y - r[under * 4 + 1];
    }
    else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) gEdit.drag = -1;
    else if (e.type == SDL_MOUSEMOTION && gEdit.drag >= 0)
    {
        const int k = gEdit.drag;
        r[k * 4] = (u16)std::clamp((int)(x - gEdit.grabX), 0, std::max(0, (int)L.width - 20));
        r[k * 4 + 1] = (u16)std::clamp((int)(y - gEdit.grabY), 0, std::max(0, (int)L.height - 20));
        EditSet(r);
        ApplyLive();
    }
    else if (e.type == SDL_MOUSEWHEEL && under >= 0)
    {
        // the size around the screen's centre; the shape stays (top 400:240 or 16:9, bottom 320:240)
        const int k = under;
        const float f = e.wheel.y > 0 ? 1.08f : 1 / 1.08f;
        const float cx = r[k * 4] + r[k * 4 + 2] / 2.f, cy = r[k * 4 + 1] + r[k * 4 + 3] / 2.f;
        const float ratio = (float)r[k * 4 + 3] / r[k * 4 + 2];
        const float w = std::clamp(r[k * 4 + 2] * f, k == 0 ? 120.f : 96.f, (float)L.width * 2);
        const float h = w * ratio;
        r[k * 4 + 2] = (u16)w;
        r[k * 4 + 3] = (u16)h;
        r[k * 4] = (u16)std::max(0.f, cx - w / 2);
        r[k * 4 + 1] = (u16)std::max(0.f, cy - h / 2);
        EditSet(r);
        ApplyLive();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rewind (Sistema > Voltar no tempo, Backspace): every rewindEvery seconds the core makes a state at its safe point
// (System::porte_rewind_save) and hands it raw; a worker thread compresses it (zstd 1) into a ring of rewindKeep
// seconds. The key loads the newest point (pressed again / held: older ones); the newer points are dropped.
// ---------------------------------------------------------------------------
// (22/11) the rewind stays in the code but OFF (the user's choice: each point pauses the game ~0.2 s - FPS dips every few
// seconds). 1 = back in the menu / shortcuts / config. The core's safe-point hooks cost nothing while nobody asks.
struct RewindPoint { std::vector<u8> data; double t; };
struct Rewind
{
    std::mutex m;
    std::condition_variable cv;
    std::deque<RewindPoint> ring;   // oldest first
    std::vector<u8> raw;            // waiting for the worker
    std::vector<u8> spare;          // a raw buffer the worker is done with: the core writes the next point into it
    double rawT = 0;
    bool hasRaw = false, quit = false;
    std::thread worker;
    double nextAt = 0, lastPress = 0;
    int back = 0;                   // how many points back the last presses went
    double rawMsSum = 0; int rawCount = 0;   // (how long the game stood still for each point: the log)
};
static Rewind gRw;
static void RewindWorker()
{
    for (;;)
    {
        std::vector<u8> raw;
        double t;
        {
            std::unique_lock lk(gRw.m);
            gRw.cv.wait(lk, [] { return gRw.quit || gRw.hasRaw; });
            if (gRw.quit) return;
            raw.swap(gRw.raw);
            t = gRw.rawT;
            gRw.hasRaw = false;
        }
        std::vector<u8> full;
        try { full = Core::System::GetInstance().PorteFinishState(raw, 1); }
        catch (const std::exception& e) { fprintf(stderr, "rewind: falha ao comprimir (%s)\n", e.what()); continue; }
        catch (...) { fprintf(stderr, "rewind: falha ao comprimir\n"); continue; }
        raw.clear();   // (keeps its memory: handed back to the core for the next point)
        std::lock_guard lk(gRw.m);
        gRw.spare = std::move(raw);
        gRw.ring.push_back({ std::move(full), t });
        const size_t keep = (size_t)std::max(2, gCfg.rewindKeep / std::max(1, gCfg.rewindEvery));
        while (gRw.ring.size() > keep) gRw.ring.pop_front();
    }
}
static void RewindStart(Core::System& system)
{
    if (!PORTE_REWIND_HABILITADO) return;
    system.porte_rewind_sink = [](std::vector<u8>&& raw) {
        std::lock_guard lk(gRw.m);
        gRw.raw = std::move(raw);   // (a point the worker had not taken yet is replaced: the newer one wins)
        gRw.rawT = NowSec();
        gRw.hasRaw = true;
        gRw.cv.notify_one();
    };
    if (!gRw.worker.joinable()) gRw.worker = std::thread(RewindWorker);
}
static void RewindStop(Core::System& system)
{
    system.porte_rewind_sink = nullptr;
    { std::lock_guard lk(gRw.m); gRw.quit = true; }
    gRw.cv.notify_all();
    if (gRw.worker.joinable()) gRw.worker.join();
}
// main loop, while the game runs: time for a new point?
static void RewindTick(Core::System& system, bool running)
{
    if (!gCfg.rewindOn || !running) return;
    const double now = NowSec();
    if (gRw.nextAt <= 0) gRw.nextAt = now + gCfg.rewindEvery;
    if (now < gRw.nextAt || system.porte_rewind_save || system.porte_rewind_load) return;
    gRw.nextAt = now + gCfg.rewindEvery;
    {
        std::lock_guard lk(gRw.m);
        if (!gRw.spare.empty() || gRw.spare.capacity()) system.porte_rewind_spare = std::move(gRw.spare);
        gRw.spare = {};
    }
    system.porte_rewind_save = true;
}
// the key: back to the newest point; pressed again within 2 s (or held), one point further back each time
static void RewindPress(bool repeat)
{
    auto& system = Core::System::GetInstance();
    const double now = NowSec();
    if (repeat && now - gRw.lastPress < 0.4) return;
    if (!gCfg.rewindOn) { Toast(T("Rewind desligado (Sistema > Voltar no tempo)", "Rewind is off (System > Rewind)")); return; }
    std::lock_guard lk(gRw.m);
    if (gRw.ring.empty()) { Toast(T("Ainda não há para onde voltar", "Nothing to go back to yet")); return; }
    gRw.back = now - gRw.lastPress < 2.0 ? gRw.back + 1 : 0;
    gRw.lastPress = now;
    // pressed again: the newest point was loaded a moment ago - one before it
    RewindPoint pt = std::move(gRw.ring.back());
    gRw.ring.pop_back();
    if (gRw.back > 0 && !gRw.ring.empty()) { pt = std::move(gRw.ring.back()); gRw.ring.pop_back(); }
    const int secs = (int)std::lround(now - pt.t);
    system.porte_rewind_buf = pt.data;   // (a copy stays as the newest point: pressing again goes before it)
    system.porte_rewind_load = true;
    gRw.ring.push_back(std::move(pt));
    gRw.nextAt = now + gCfg.rewindEvery;
    Toast(T("Voltou ", "Went back ") + std::to_string(secs) + T(" s", " s"));
    fprintf(stderr, "rewind: voltou %d s (%zu pontos guardados)\n", secs, gRw.ring.size());
}

static std::string ShotPath()
{
    std::string dir = gDataDir + "capturas\\";
    CreateDirectoryW(Widen(dir).c_str(), nullptr);
    char b[64];
    time_t t = time(nullptr);
    strftime(b, sizeof(b), "_%Y%m%d_%H%M%S.bmp", localtime(&t));
    // (named after the running game: pokemon_ultramoon_..., pokemon_sun_...)
    std::string game = Jogos::gAtual ? std::string(Jogos::gAtual->nome) : std::string("alola");
    std::string slug;
    for (char c : game) if (c != ' ') slug += (char)tolower((unsigned char)c);
    return dir + "pokemon_" + slug + b;
}

// ---------------------------------------------------------------------------
// menu pages
// ---------------------------------------------------------------------------
// Gráficos > Fluidez: the menu's interpolation choice to interp_rec (it takes it at the next renderer:
// game start, restart, state load). Background tests stay at the original 30 unless PORTE_INTERP says so
static void ApplyInterp()
{
#ifdef PORTE_SEM_GERADOR
    if (getenv("PORTE_HIDDEN") && !getenv("PORTE_INTERP_MENU")) return;
    InterpRec::Configure(gCfg.interpFps < 0 ? std::max(60, MonitorHz()) : gCfg.interpFps);
#endif
}

static std::vector<MenuItem> BuildItems(int page)
{
    using namespace Settings;
    auto& v = values;
    std::vector<MenuItem> it;
    auto& system = Core::System::GetInstance();
    switch (page)
    {
    case P_Game:
        it.push_back({ T("Continuar", "Resume"), "", nullptr, [] { gMenu.open = false; } });
        it.push_back({ T("Slot do estado", "State slot"), std::to_string(gCfg.stateSlot) + "  (" + SlotText(gCfg.stateSlot) + ")",
                       [](int d) { gCfg.stateSlot = (gCfg.stateSlot - 1 + d + 10) % 10 + 1; SaveConfig(); }, nullptr }); { std::vector<std::string> o; for (int k = 1; k <= 10; k++) o.push_back(std::to_string(k)); it.back().opts = o; it.back().optIdx = gCfg.stateSlot - 1; }
        it.push_back({ WithKey(T("Salvar estado", "Save state"), H_Save), "", nullptr, [] { SaveState(gCfg.stateSlot); gMenu.open = false; } });
        it.push_back({ WithKey(T("Carregar estado", "Load state"), H_Load), "", nullptr, [] { LoadState(gCfg.stateSlot); gMenu.open = false; } });
        it.push_back({ WithKey(T("Captura de tela", "Screenshot"), H_Shot), "", nullptr, [] { gMenu.open = false; RequestShot(ShotPath()); } });
        it.push_back({ WithKey(T("Modo foto", "Photo mode"), H_Photo), "", nullptr, [] { gMenu.open = false; StartPhoto(); }, false,
                       T("Pausa o jogo e solta a câmera: WASD anda, setas ou botão direito do mouse giram, roda dá zoom, H mostra a interface, F12 fotografa. Precisa da interpolação ligada",
                         "Pauses the game and frees the camera: WASD moves, arrows or the right mouse button turn, the wheel zooms, H shows the interface, F12 takes the picture. Needs the interpolation on") });
        it.push_back({ T("Capturas em alta resolução", "High resolution screenshots"), OnOff(gCfg.shotHiRes), [](int) { gCfg.shotHiRes = !gCfg.shotHiRes; }, nullptr, false,
                       T("Só a tela de cima, na resolução interna (ex.: 3x = 1200x720; com 16:9, 1280x720). Desligado = a janela como está",
                         "Only the top screen, at the internal resolution (e.g. 3x = 1200x720; with 16:9, 1280x720). Off = the window as it is") });
        {
            const Uint64 secs = gSessionStart ? (SDL_GetTicks64() - gSessionStart) / 1000 : 0;
            char b[32];
            snprintf(b, sizeof(b), "%lluh %02llumin", (unsigned long long)(secs / 3600), (unsigned long long)(secs / 60 % 60));
            it.push_back({ T("Tempo nesta sessão", "Time this session"), b, nullptr, nullptr, false,
                           T("Desde que o jogo foi aberto agora (o tempo total do save está na aba Saves)", "Since the game was opened this time (the save's total time is on the Saves tab)") });
        }
        it.push_back({ WithKey(T("Tela cheia", "Fullscreen"), H_Full), OnOff(gCfg.fullscreen), [](int) { SetFullscreen(!gCfg.fullscreen); SaveConfig(); }, nullptr });
        it.push_back({ T("Reiniciar o jogo", "Restart the game"), "", nullptr, [] {
            Core::System::GetInstance().SendSignal(Core::System::Signal::Reset); gMenu.open = false; Toast(T("Reiniciando…", "Restarting…")); } });
        it.push_back({ T("Sair", "Quit"), "", nullptr, [] { gRunning = false; } });
        {
            it.push_back(Header(T("Mods (valem ao reiniciar o jogo)", "Mods (take effect when the game restarts)")));
            const std::string active = ModActiveName();
            const std::vector<std::string> mods = ModList();   // (one list: begin / end of two different temporaries crashed)
            it.push_back({ T("Jogo original (sem mod)", "Original game (no mod)"), active.empty() ? T("Ativo", "Active") : "", nullptr,
                           [] { if (ModSet("")) Toast(T("Sem mod: reinicie o jogo (Jogo > Reiniciar)", "No mod: restart the game (Game > Restart)")); } });
            if (!active.empty() && std::find(mods.begin(), mods.end(), active) == mods.end())
                it.push_back({ active, T("Ativo", "Active"), nullptr, nullptr, false, T("O mod que está em uso agora", "The mod in use now") });
            for (const std::string& m : mods)
            {
                const bool code = ModHasCode(ModLibDir() / fs_m::path(Widen(m)));
                it.push_back({ m, code ? T("tem código: não funciona", "has code: will not work") : T("Ligar", "Turn on"), nullptr,
                               [m] { if (ModSet(m)) Toast(T("Mod ligado: reinicie o jogo (Jogo > Reiniciar)", "Mod on: restart the game (Game > Restart)")); }, false,
                               T("Enter liga este mod (o que estava ativo volta para a biblioteca)", "Enter turns this mod on (the active one goes back to the library)") });
            }
            it.push_back({ T("Abrir a pasta de mods", "Open the mods folder"), "", nullptr, [] { ModReadme(); OpenPath(Narrow(ModLibDir().wstring())); }, false,
                           T("Uma pasta por mod, com a pasta romfs do mod dentro. Mods de código (exefs) não funcionam aqui", "One folder per mod, with the mod's romfs folder inside. Code mods (exefs) do not work here") });
        }
        break;

    case P_Saves:
    {
        // the game's own save (what the game wrote the last time the player saved)
        const GameSaveInfo g = ReadGameSave();
        it.push_back(Header(T("Seu save", "Your save")));
        if (!g.ok)
            it.push_back({ T("Nenhum save do jogo ainda", "No game save yet"), "", nullptr, nullptr, false,
                           T("Aparece aqui depois que você salvar dentro do jogo (menu X > Salvar)", "Shows up here once you save in the game (X menu > Save)") });
        else
        {
            const std::string note = T("Do arquivo de save: muda quando você salva dentro do jogo", "From the save file: changes when you save in the game");
            // (a save made in another of the four games, e.g. Ultra Sun's in Ultra Moon, says where it came from)
            const bool other = Jogos::gAtual && GameNameOf(g.game) != Jogos::gAtual->nome;
            it.push_back({ T("Treinador", "Trainer"), other ? g.name + T("  (save vindo do ", "  (save from ") + GameNameOf(g.game) + ")" : g.name,
                           nullptr, nullptr, false, note });
            char tm[32];
            snprintf(tm, sizeof(tm), "%dh %02dmin", g.hours, g.minutes);
            it.push_back({ T("Tempo de jogo", "Play time"), tm, nullptr, nullptr, false, note });
            it.push_back({ T("Pokédex (capturados)", "Pokédex (caught)"), std::to_string(g.caught) + " / " + std::to_string(g.species), nullptr, nullptr, false, note });
            const int islands = __builtin_popcount((g.stamps >> 1) & 0xF);
            it.push_back({ T("Desafio das ilhas", "Island challenge"),
                           (g.stamps >> 5 & 1) ? T("Campeão de Alola!", "Alola Champion!") : std::to_string(islands) + T(" de 4 ilhas", " of 4 islands"),
                           nullptr, nullptr, false, T("Pelos selos do Passaporte de Treinador", "From the Trainer Passport stamps") });
            std::string money = std::to_string(g.money);
            for (int k = (int)money.size() - 3; k > 0; k -= 3) money.insert(k, ".");
            it.push_back({ T("Dinheiro", "Money"), "$ " + money, nullptr, nullptr, false, note });
            it.push_back({ T("Selos do passaporte", "Passport stamps"), std::to_string(__builtin_popcount(g.stamps)) + " / 15", nullptr, nullptr, false, note });
        }
        if (gBattles < 0 && Jogos::gAtual) StatsLoad();
        it.push_back({ T("Batalhas contadas", "Battles counted"), std::to_string(std::max(0LL, gBattles)), nullptr, nullptr, false,
                       T("Toda batalha (selvagem ou com treinador) desde que o contador começou neste PC", "Every battle (wild or trainer) since the counter started on this PC") });
        it.push_back({ T("Backup automático ao abrir", "Automatic backup when opening"), OnOff(gCfg.saveBackup), [](int) { gCfg.saveBackup = !gCfg.saveBackup; }, nullptr, false,
                       T("Cada vez que o jogo abre, uma cópia do save vai para a pasta backups (as 10 mais novas; só quando o save mudou)",
                         "Each time the game opens, a copy of the save goes to the backups folder (the 10 newest; only when the save changed)") });
        it.push_back({ T("Fazer backup do save agora", "Back up the save now"), "", nullptr, [] {
            Toast(BackupGameSave() ? T("Backup feito", "Backup done") : T("Nada novo: o último backup já é igual ao save", "Nothing new: the last backup equals the save")); } });
        it.push_back({ T("Abrir a pasta de backups", "Open the backups folder"), "", nullptr, [] {
            CreateDirectoryW(Widen(gDataDir + "backups").c_str(), nullptr); CreateDirectoryW(Widen(SaveBackupDir()).c_str(), nullptr); OpenPath(SaveBackupDir()); } });
        it.push_back(Header(T("Estados salvos", "Save states")));
        it.push_back({ T("Estado automático (ao sair)", "Automatic state (on exit)"), SlotText(11), nullptr,
                       [] { LoadState(11); gMenu.open = false; }, false, T("Enter carrega. Ligue em Sistema > Salvar estado ao sair", "Enter loads it. Turn on System > Save state on exit") });
        for (int s = 1; s <= 10; s++)
            it.push_back({ T("Estado ", "State ") + std::to_string(s), SlotText(s), nullptr,
                           [s] { gCfg.stateSlot = s; LoadState(s); gMenu.open = false; },
                           false, T("Enter carrega este estado", "Enter loads this state") });
        it.push_back({ T("Abrir a pasta do save do jogo", "Open the game's save folder"), "", nullptr, [] {
            std::string p = GameSavePath();
            OpenPath(p.substr(0, p.find_last_of('/'))); } });
        break;
    }

    case P_Graphics:
    {
        // --- smoothness ---
        it.push_back(Header(T("Fluidez (FPS)", "Smoothness (FPS)")));
#ifdef PORTE_SEM_GERADOR
        {
            // our interpolation: the game stays at 30, the in-between pictures are drawn by mixing its matrices
            // (a value not in the list - set with the fine adjustment below - shows as an extra last entry)
            static const int rates[] = { 0, 60, 90, 120, 144, 165, 200, 240, 300, -1 };
            static constexpr int NR = (int)std::size(rates);
            static auto curIdx = [] { for (int k = 0; k < NR; k++) if (gCfg.interpFps == rates[k]) return k; return NR; };
            static auto rateName = [](int r) {
                return r == 0 ? T("Desligada (30 original)", "Off (original 30)")
                     : r < 0 ? T("Monitor (", "Monitor (") + std::to_string(MonitorHz()) + " Hz)"
                             : std::to_string(r) + " FPS"; };
            const int cur = curIdx();
            const std::string val = cur == NR ? T("Personalizado (", "Custom (") + std::to_string(gCfg.interpFps) + " FPS)" : rateName(gCfg.interpFps);
            it.push_back({ T("Interpolação", "Interpolation"), val,
                           [](int d) {   // (reads the current value each step: a dropdown calls this several times)
                               const int i = curIdx(), n = i == NR ? NR + 1 : NR;
                               gCfg.interpFps = rates[(i + d + n) % n];
                               ApplyInterp(); Toast(T("Vale ao reiniciar o jogo ou carregar um estado", "Takes effect on restart or state load")); },
                           nullptr, true,
                           T("Quadros extras entre as imagens do jogo (ele continua a 30). 200 = o nosso padrão. Vale ao reiniciar ou ao carregar um estado",
                             "Extra frames between the game's pictures (it stays at 30). 200 = our default. Takes effect on restart or state load") });
            { std::vector<std::string> o; for (int rr : rates) o.push_back(rateName(rr)); if (cur == NR) o.push_back(val); it.back().opts = o; it.back().optIdx = cur; }
            it.back().def = "200 FPS";
            const int fine = gCfg.interpFps > 0 ? gCfg.interpFps : gCfg.interpFps < 0 ? std::max(60, MonitorHz()) : 0;
            it.push_back({ T("FPS personalizado", "Custom FPS"), fine ? std::to_string(fine) + " FPS" : T("(interpolação desligada)", "(interpolation off)"),
                           [](int d) {
                               int v = gCfg.interpFps > 0 ? gCfg.interpFps : gCfg.interpFps < 0 ? std::max(60, MonitorHz()) : 55;
                               gCfg.interpFps = std::clamp((v + d * 5) / 5 * 5, 60, 400);
                               ApplyInterp(); Toast(T("Vale ao reiniciar o jogo ou carregar um estado", "Takes effect on restart or state load")); },
                           nullptr, true,
                           T("Ajuste fino de 5 em 5 FPS (60 a 400), para casar com o seu monitor (ex.: 75, 100, 180). Vale ao reiniciar ou ao carregar um estado",
                             "Fine adjustment in steps of 5 FPS (60 to 400), to match your monitor (e.g. 75, 100, 180). Takes effect on restart or state load") });
            it.back().bar = fine ? (fine - 60) / 340.f : 0.f;
            it.push_back({ T("Ajuste automático do FPS", "Automatic FPS adjustment"), OnOff(gCfg.autoBudget), [](int) { gCfg.autoBudget = !gCfg.autoBudget; }, nullptr, false,
                           T("Ligado: quando o PC não dá conta, a interpolação gera menos quadros para o jogo não ficar lento. Desligado: sempre o FPS escolhido",
                             "On: when the PC cannot keep up, the interpolation makes fewer frames so the game does not slow down. Off: always the chosen FPS") });
            it.back().def = T("Ligado", "On");
        }
#else
        it.push_back({ T("Gerar quadros", "Frame generation"), OnOff(gCfg.frameGen),
                       [](int) { gCfg.frameGen = !gCfg.frameGen; }, nullptr, false,
                       T("Cria quadros entre as imagens (cada ponto anda como os triângulos do jogo); +33 ms de atraso",
                         "Builds frames between the images (each point moves as the game's triangles did); +33 ms delay") });
        {
            static const int rates[] = { 0, 60, 90, 120, 144, 165, 200, 240 };
            int cur = 0;
            for (int i = 0; i < 8; i++) if (gCfg.fgRate == rates[i]) cur = i;
            std::string val = gCfg.fgRate == 0 ? T("Monitor (", "Monitor (") + std::to_string(MonitorHz()) + " Hz)"
                                               : std::to_string(gCfg.fgRate) + " FPS";
            it.push_back({ T("FPS máximo do gerador", "Generator max FPS"), val,
                           [cur](int d) { gCfg.fgRate = rates[(cur + d + 8) % 8]; }, nullptr, false,
                           T("Quantos quadros por segundo mostrar no total (reais + gerados)",
                             "How many frames a second to show in total (real + generated)") });
        }
#endif
        it.push_back({ T("VSync (sem linha cortando a imagem)", "VSync (no tearing line)"), OnOff(gCfg.vsync),
                       [](int) { gCfg.vsync = !gCfg.vsync; }, nullptr, false,
                       T("Espera o monitor antes de trocar a imagem: tira a linha horizontal; acima do Hz do monitor não passa",
                         "Waits for the monitor before showing a frame: no horizontal line; never above the monitor's Hz") });
        it.push_back({ T("Pular quadros repetidos", "Skip duplicate frames"), OnOff(v.use_skip_duplicate_frames.GetValue()),
                       Toggle([&] { return v.use_skip_duplicate_frames.GetValue(); }, [&](bool b) { v.use_skip_duplicate_frames.SetValue(b); }) });

        // --- image ---
        it.push_back(Header(T("Imagem", "Image")));
        {
            const u32 f = v.resolution_factor.GetValue();
            const u32 autoF = std::max(1u, gWindow.GetFramebufferLayout().GetScalingRatio());
            static const char* resName[] = { "", "1x (240p)", "2x (480p)", "3x (720p)", "4x (960p)", "5x (1200p, ~1080p)", "6x (1440p)",
                                             "7x (1680p)", "8x (1920p)", "9x (2160p, 4K)", "10x (2400p)" };
            std::vector<std::string> ro { T("Automática (", "Automatic (") + std::to_string(autoF) + "x)" };
            for (int k = 1; k <= 10; k++) ro.push_back(resName[k]);
            it.push_back({ T("Resolução interna", "Internal resolution"), ro[std::min(10u, f)],
                           [&](int d) { v.resolution_factor.SetValue(std::clamp((int)v.resolution_factor.GetValue() + d, 0, 10)); }, nullptr,
                           false, T("Automática = a nitidez que a janela mostra. Acima disso só pesa (6x numa janela de 1080p fica lento)",
                                    "Automatic = the sharpness the window shows. More only costs speed (6x in a 1080p window is slow)") });
            it.back().opts = ro; it.back().optIdx = (int)std::min(10u, f); it.back().def = T("Automática", "Automatic");
        }
        it.push_back({ T("Brilho", "Brightness"), std::to_string(gCfg.brightness) + "%",
                       [](int d) { gCfg.brightness = std::clamp(gCfg.brightness + d * 5, 50, 150); }, nullptr, false,
                       T("Clareia ou escurece a imagem do jogo (o menu não muda)", "Makes the game's picture lighter or darker (the menu stays the same)") });
        it.back().bar = (gCfg.brightness - 50) / 100.f; it.back().def = "100%";
        {
            char g[16];
            snprintf(g, sizeof(g), "%.2f", gCfg.gamma / 100.0);
            it.push_back({ T("Gama", "Gamma"), g,
                           [](int d) { gCfg.gamma = std::clamp(gCfg.gamma + d * 5, 50, 250); }, nullptr, false,
                           T("Acima de 1,00 clareia os tons escuros sem estourar os claros (bom para cavernas e noite); abaixo, mais contraste",
                             "Above 1.00 lifts the dark tones without blowing out the bright ones (good for caves and night); below, more contrast") });
            it.back().bar = (gCfg.gamma - 50) / 200.f; it.back().def = "1.00";
        }
        it.push_back({ T("Saturação", "Saturation"), std::to_string(gCfg.saturation) + "%", [](int d) { gCfg.saturation = std::clamp(gCfg.saturation + d * 10, 0, 200); }, nullptr, false,
                       T("Cores mais vivas (acima de 100%) ou mais apagadas; 0% = preto e branco", "More vivid colours (above 100%) or duller; 0% = black and white") });
        it.back().bar = gCfg.saturation / 200.f; it.back().def = "100%";
        it.push_back({ T("Nitidez", "Sharpness"), std::to_string(gCfg.sharpness) + "%", [](int d) { gCfg.sharpness = std::clamp(gCfg.sharpness + d * 10, 0, 100); }, nullptr, false,
                       T("Realça os contornos da imagem do jogo (bom com resolução baixa ou filtro suave)", "Sharpens the edges of the game picture (good with a low resolution or a smooth filter)") });
        it.back().bar = gCfg.sharpness / 100.f; it.back().def = "0%";
        it.push_back({ T("FXAA (suavizar serrilhado)", "FXAA (smooth jaggies)"), OnOff(gCfg.fxaa), [](int) { gCfg.fxaa = !gCfg.fxaa; }, nullptr, false,
                       T("Suaviza os degraus nas bordas dos objetos. Mais útil em resolução baixa (1x-2x); quase não pesa", "Smooths the steps along the edges of objects. Most useful at a low resolution (1x-2x); almost free") });
        it.back().def = T("Desligado", "Off");
        it.push_back({ T("Suavizar imagem ao esticar", "Smooth when stretching"), OnOff(v.filter_mode.GetValue()),
                       Toggle([&] { return v.filter_mode.GetValue(); }, [&](bool b) { v.filter_mode.SetValue(b); }) });
        it.push_back({ T("Escala inteira", "Integer scaling"), OnOff(v.use_integer_scaling.GetValue()),
                       Toggle([&] { return v.use_integer_scaling.GetValue(); }, [&](bool b) { v.use_integer_scaling.SetValue(b); }) });

        // --- textures ---
        it.push_back(Header(T("Texturas", "Textures")));
        static const char* tf[] = { "Nenhum", "Anime4K", "Bicúbico", "ScaleForce", "xBRZ", "MMPX" };
        static const char* tfEn[] = { "None", "Anime4K", "Bicubic", "ScaleForce", "xBRZ", "MMPX" };
        it.push_back({ T("Filtro de texturas", "Texture filter"), (EN() ? tfEn : tf)[(int)v.texture_filter.GetValue()], Cycle(v.texture_filter, 6) }); OptsArr(it.back(), EN() ? tfEn : tf, 6, (int)v.texture_filter.GetValue());
        static const char* ts[] = { "Do jogo", "Pixelado", "Suave" };
        static const char* tsEn[] = { "Game controlled", "Nearest", "Linear" };
        it.push_back({ T("Amostragem de texturas", "Texture sampling"), (EN() ? tsEn : ts)[(int)v.texture_sampling.GetValue()], Cycle(v.texture_sampling, 3) }); OptsArr(it.back(), EN() ? tsEn : ts, 3, (int)v.texture_sampling.GetValue());
        it.push_back({ T("Texturas personalizadas", "Custom textures"), OnOff(v.custom_textures.GetValue()),
                       Toggle([&] { return v.custom_textures.GetValue(); }, [&](bool b) { v.custom_textures.SetValue(b); }), nullptr, true,
                       T("Pacotes de texturas na pasta load\\textures", "Texture packs in the load\\textures folder") });
        it.push_back({ T("Pré-carregar texturas", "Preload textures"), OnOff(v.preload_textures.GetValue()),
                       Toggle([&] { return v.preload_textures.GetValue(); }, [&](bool b) { v.preload_textures.SetValue(b); }), nullptr, true });

        // --- stereoscopic 3D ---
        it.push_back(Header(T("3D estereoscópico", "Stereoscopic 3D")));
        static const char* s3[] = { "Desligado", "Lado a lado", "Lado a lado inteiro", "Anaglifo", "Entrelaçado", "Entrelaçado invertido" };
        static const char* s3En[] = { "Off", "Side by side", "Side by side (full)", "Anaglyph", "Interlaced", "Reverse interlaced" };
        it.push_back({ T("Modo", "Mode"), (EN() ? s3En : s3)[std::min(5, (int)v.render_3d.GetValue())], Cycle(v.render_3d, 6), nullptr, false,
                       T("Com o 3D ligado o gerador de quadros pausa (mostra só os quadros do jogo)",
                         "With 3D on the frame generator pauses (only the game's own frames)") }); OptsArr(it.back(), EN() ? s3En : s3, 6, std::min(5, (int)v.render_3d.GetValue()));
        it.push_back({ T("Intensidade", "Depth"), std::to_string(v.factor_3d.GetValue()) + "%",
                       [&](int d) { v.factor_3d.SetValue((u32)std::clamp((int)v.factor_3d.GetValue() + d * 10, 0, 100)); } }); it.back().bar = v.factor_3d.GetValue() / 100.f;

        // --- shaders (advanced) ---
        it.push_back(Header(T("Shaders (avançado)", "Shaders (advanced)")));
        it.push_back({ T("Multiplicação precisa", "Accurate multiplication"), OnOff(v.shaders_accurate_mul.GetValue()),
                       Toggle([&] { return v.shaders_accurate_mul.GetValue(); }, [&](bool b) { v.shaders_accurate_mul.SetValue(b); }), nullptr, true });
        it.push_back({ T("Cache de shaders no disco", "Disk shader cache"), OnOff(v.use_disk_shader_cache.GetValue()),
                       Toggle([&] { return v.use_disk_shader_cache.GetValue(); }, [&](bool b) { v.use_disk_shader_cache.SetValue(b); }), nullptr, true });
        it.push_back({ T("Compilar shaders em paralelo", "Async shader compilation"), OnOff(v.async_shader_compilation.GetValue()),
                       Toggle([&] { return v.async_shader_compilation.GetValue(); }, [&](bool b) { v.async_shader_compilation.SetValue(b); }), nullptr, true,
                       T("Menos travadas na primeira vez que um efeito aparece", "Fewer stutters the first time an effect shows up") });
        break;
    }

    case P_Screen:
    {
        static const LayoutOption lo[] = { LayoutOption::Default, LayoutOption::SingleScreen, LayoutOption::LargeScreen,
                                           LayoutOption::SideScreen, LayoutOption::HybridScreen, LayoutOption::CustomLayout };
        static const char* loPt[] = { "Uma em cima da outra", "Só uma tela", "Tela grande + pequena", "Lado a lado", "Híbrido", "Personalizada" };
        static const char* loEn[] = { "Stacked", "Single screen", "Large + small", "Side by side", "Hybrid", "Custom" };
        it.push_back({ T("Duas janelas (dois monitores)", "Two windows (two monitors)"), OnOff(gCfg.dualWindow),
                       [](int) { gCfg.dualWindow = !gCfg.dualWindow; }, nullptr, false,
                       T("A outra tela numa janela própria: arraste para o outro monitor. Ela mostra 60 FPS para não pesar na principal. F11 deixa em tela cheia a janela selecionada",
                         "The other screen in a window of its own: drag it to the other monitor. It shows 60 FPS so the main one stays fast. F11 makes the focused window full screen") });
        if (gCfg.dualWindow)
            it.push_back({ WithKey(T("Disposição das telas", "Screen layout"), H_Layout), T("Uma tela por janela", "One screen per window"), nullptr });
        else
        {
            int cur = 0;
            for (int i = 0; i < 6; i++) if (v.layout_option.GetValue() == lo[i]) cur = i;
            it.push_back({ WithKey(T("Disposição das telas", "Screen layout"), H_Layout), (EN() ? loEn : loPt)[cur],
                           [&](int d) { int c = 0; for (int i = 0; i < 6; i++) if (v.layout_option.GetValue() == lo[i]) c = i;
                                        v.layout_option.SetValue(lo[(c + d + 6) % 6]); } }); OptsArr(it.back(), EN() ? loEn : loPt, 6, cur);
            it.push_back({ T("Personalizar posição das telas", "Customize the screens' places"), "", nullptr, [] { gMenu.open = false; StartLayoutEdit(); }, false,
                           T("Arraste as telas com o mouse e use a roda para mudar o tamanho; Enter salva, Esc cancela. A tela de baixo pode ficar por cima da de cima (flutuante)",
                             "Drag the screens with the mouse and use the wheel to resize; Enter saves, Esc cancels. The bottom screen can sit over the top one (floating)") });
            it.push_back({ T("Opacidade da tela de baixo", "Bottom screen opacity"), std::to_string(v.custom_second_layer_opacity.GetValue()) + "%",
                           [&](int d) { v.custom_second_layer_opacity.SetValue((u16)std::clamp((int)v.custom_second_layer_opacity.GetValue() + d * 10, 10, 100)); }, nullptr, false,
                           T("Na disposição personalizada, quando a tela de baixo fica por cima da de cima", "In the custom layout, when the bottom screen sits over the top one") });
            it.back().bar = v.custom_second_layer_opacity.GetValue() / 100.f; it.back().def = "100%";
        }
        {
            static const char* dp[] = { "Desligada", "Batalha: tela de cima grande", "Batalha: só a tela de cima" };
            static const char* de[] = { "Off", "Battle: top screen large", "Battle: top screen only" };
            it.push_back({ T("Tela dinâmica", "Dynamic screen"), (EN() ? de : dp)[gCfg.dynScreen], [](int d) { gCfg.dynScreen = (gCfg.dynScreen + d + 3) % 3; }, nullptr, false,
                           T("Nas batalhas a disposição muda sozinha e volta para a sua quando a batalha acaba", "In battles the layout changes by itself and goes back to yours when the battle ends") });
            OptsArr(it.back(), EN() ? de : dp, 3, gCfg.dynScreen); it.back().def = EN() ? de[0] : dp[0];
        }
        it.push_back({ T("16:9 de verdade (mais cenário)", "Real 16:9 (more of the scene)"), OnOff(gCfg.widescreen),
                       [](int) { gCfg.widescreen = !gCfg.widescreen; }, nullptr, false,
                       T("A tela de cima vira 16:9 mostrando MAIS do cenário nos lados (o jogo desenha uma visão mais larga), sem esticar. A interface (textos, menus do jogo) pode ficar um pouco mais larga.",
                         "The top screen becomes 16:9 showing MORE of the scene on the sides (the game draws a wider view), not stretched. The game's interface (text, menus) may look slightly wider.") });
        it.back().def = T("Desligado", "Off");
        it.push_back({ WithKey(T("Inverter telas", "Swap screens"), H_Swap), OnOff(v.swap_screen.GetValue()),
                       Toggle([&] { return v.swap_screen.GetValue(); }, [&](bool b) { v.swap_screen.SetValue(b); }) });
        it.push_back({ T("Telas em pé (giradas)", "Upright screens"), OnOff(v.upright_screen.GetValue()),
                       Toggle([&] { return v.upright_screen.GetValue(); }, [&](bool b) { v.upright_screen.SetValue(b); }), nullptr, false,
                       T("Com as telas em pé o gerador de quadros pausa", "With upright screens the frame generator pauses") });
        static const char* ar[] = { "Original", "16:9", "4:3", "21:9", "16:10", "Esticar" };
        static const char* arEn[] = { "Original", "16:9", "4:3", "21:9", "16:10", "Stretch" };
        it.push_back({ T("Proporção (uma tela)", "Aspect ratio (single)"), (EN() ? arEn : ar)[(int)v.aspect_ratio.GetValue()], Cycle(v.aspect_ratio, 6) }); OptsArr(it.back(), EN() ? arEn : ar, 6, (int)v.aspect_ratio.GetValue());
        char prop[16];
        snprintf(prop, sizeof(prop), "%.1fx", v.large_screen_proportion.GetValue());
        it.push_back({ T("Tamanho da tela grande", "Large screen size"), prop,
                       [&](int d) { v.large_screen_proportion.SetValue(std::clamp(v.large_screen_proportion.GetValue() + d * 0.5f, 1.f, 16.f)); } }); it.back().bar = (v.large_screen_proportion.GetValue() - 1.f) / 15.f;
        static const char* sp[] = { "Direita, em cima", "Direita, no meio", "Direita, embaixo", "Esquerda, em cima", "Esquerda, no meio",
                                    "Esquerda, embaixo", "Acima da grande", "Abaixo da grande" };
        static const char* spEn[] = { "Top right", "Middle right", "Bottom right", "Top left", "Middle left", "Bottom left", "Above large", "Below large" };
        it.push_back({ T("Posição da tela pequena", "Small screen position"), (EN() ? spEn : sp)[(int)v.small_screen_position.GetValue()],
                       Cycle(v.small_screen_position, 8) }); OptsArr(it.back(), EN() ? spEn : sp, 8, (int)v.small_screen_position.GetValue());
        it.push_back({ T("Espaço entre as telas", "Gap between screens"), std::to_string(v.screen_gap.GetValue()) + " px",
                       [&](int d) { v.screen_gap.SetValue(std::clamp(v.screen_gap.GetValue() + d * 10, 0, 200)); } }); it.back().bar = v.screen_gap.GetValue() / 200.f; it.back().def = "0 px";
        static const char* bg[] = { "Preto", "Cinza escuro", "Azul noite", "Vinho" };
        static const char* bgEn[] = { "Black", "Dark gray", "Night blue", "Wine" };
        it.push_back({ T("Cor do fundo", "Background color"), (EN() ? bgEn : bg)[std::clamp(gCfg.bgColor, 0, 3)],
                       [](int d) { gCfg.bgColor = (gCfg.bgColor + d + 4) % 4; } }); OptsArr(it.back(), EN() ? bgEn : bg, 4, std::clamp(gCfg.bgColor, 0, 3));
        it.push_back({ WithKey(T("Mostrar FPS", "Show FPS"), H_Fps), OnOff(gCfg.showFps), [](int) { gCfg.showFps = !gCfg.showFps; } });
        it.push_back({ WithKey(T("Tela cheia", "Fullscreen"), H_Full), OnOff(gCfg.fullscreen), [](int) { SetFullscreen(!gCfg.fullscreen); } });
        {
            const std::vector<std::string> fm { T("Sem bordas (recomendado)", "Borderless (recommended)"), T("Exclusiva", "Exclusive") };
            it.push_back({ T("Modo da tela cheia", "Fullscreen mode"), fm[gCfg.fsMode],
                           [](int) { gCfg.fsMode ^= 1; if (gCfg.fullscreen) SetFullscreen(true); }, nullptr, false,
                           T("Sem bordas: Alt+Tab instantâneo. Exclusiva: o jogo fica com o monitor só para ele (pode diminuir o atraso em alguns PCs); a tela pisca ao entrar e sair",
                             "Borderless: instant Alt+Tab. Exclusive: the game has the monitor to itself (may lower the delay on some PCs); the screen blinks when entering and leaving") });
            it.back().opts = fm; it.back().optIdx = gCfg.fsMode; it.back().def = fm[0];
        }
        break;
    }

    case P_Sound:
    {
        it.push_back({ T("Volume (+ / -)", "Volume (+ / -)"), std::to_string((int)std::lround(v.volume.GetValue() * 100)) + "%",
                       [&](int d) { v.volume.SetValue(std::clamp(v.volume.GetValue() + d * 0.05f, 0.f, 1.f)); } }); it.back().bar = std::clamp(v.volume.GetValue(), 0.f, 1.f); it.back().def = "100%";
        it.push_back({ T("Silenciar sem foco", "Mute when in the background"), OnOff(gCfg.muteUnfocused),
                       [](int) { gCfg.muteUnfocused = !gCfg.muteUnfocused; }, nullptr, false,
                       T("Sem som enquanto você usa outro programa (o jogo continua rodando)",
                         "No sound while you use another program (the game keeps running)") }); it.back().def = T("Desligado", "Off");
        it.push_back({ WithKey(T("Silenciar ao acelerar", "Mute while fast-forwarding"), H_Turbo), OnOff(gCfg.muteTurbo),
                       [](int) { gCfg.muteTurbo = !gCfg.muteTurbo; }, nullptr, false,
                       T("Sem som acelerado enquanto o Tab está valendo", "No sped-up sound while Tab is on") }); it.back().def = T("Desligado", "Off");
        {
            // the sink's devices (Settings::values.output_device: "Auto" = the system's default)
            static std::vector<std::string> devs;
            static Uint64 listedAt = 0;
            if (devs.empty() || SDL_GetTicks64() - listedAt > 5000)
            {
                listedAt = SDL_GetTicks64();
                const auto& sd = AudioCore::GetSinkDetails(v.output_type.GetValue());
                devs = sd.list_devices ? sd.list_devices() : std::vector<std::string>{};
                if (devs.size() > 12) devs.resize(12);
            }
            std::vector<std::string> o { T("Padrão do sistema", "System default") };
            int cur = 0;
            for (size_t k = 0; k < devs.size(); k++) { o.push_back(devs[k]); if (devs[k] == v.output_device.GetValue()) cur = (int)k + 1; }
            it.push_back({ T("Saída de som", "Audio output"), o[cur],
                           [](int d) {
                               auto& vv = Settings::values;
                               int c = 0;
                               for (size_t k = 0; k < devs.size(); k++) if (devs[k] == vv.output_device.GetValue()) c = (int)k + 1;
                               const int n = (int)devs.size() + 1;
                               c = (c + d + n) % n;
                               vv.output_device.SetValue(c == 0 ? std::string("Auto") : devs[c - 1]);
                               auto& sys = Core::System::GetInstance();
                               if (sys.IsPoweredOn()) sys.DSP().SetSink(vv.output_type.GetValue(), vv.output_device.GetValue());
                           }, nullptr, false, T("Onde o som do jogo toca (fones, caixas, TV...). Troca na hora", "Where the game sound plays (headphones, speakers, TV...). Changes at once") });
            it.back().opts = o; it.back().optIdx = cur; it.back().def = o[0];
        }
        {
            // microphone (the game asks for it in a few places): off, static noise or a real microphone (+ which one)
            static const char* mp[] = { "Automático", "Desligado", "Ruído (simulado)", "Microfone real" };
            static const char* me[] = { "Automatic", "Off", "Noise (simulated)", "Real microphone" };
            const int cur = std::min(3, (int)v.input_type.GetValue());
            it.push_back({ T("Microfone", "Microphone"), (EN() ? me : mp)[cur], [](int d) {
                int c = std::min(3, (int)Settings::values.input_type.GetValue());
                c = (c + d + 4) % 4;
                Settings::values.input_type.SetValue((AudioCore::InputType)c); }, nullptr, true,
                T("Os jogos de Alola quase não usam. Real = o microfone do PC (escolha abaixo)", "The Alola games hardly use it. Real = the PC's microphone (choose below)") });
            OptsArr(it.back(), EN() ? me : mp, 4, cur); it.back().def = EN() ? me[0] : mp[0];
            if (cur == 3)
            {
                static std::vector<std::string> mics;
                static Uint64 listedAt = 0;
                if (mics.empty() || SDL_GetTicks64() - listedAt > 5000)
                {
                    listedAt = SDL_GetTicks64();
                    const auto& idt = AudioCore::GetInputDetails(AudioCore::InputType::Cubeb);
                    mics = idt.list_devices ? idt.list_devices() : std::vector<std::string>{};
                    if (mics.size() > 12) mics.resize(12);
                }
                std::vector<std::string> o { T("Padrão do sistema", "System default") };
                int c = 0;
                for (size_t k = 0; k < mics.size(); k++) { o.push_back(mics[k]); if (mics[k] == v.input_device.GetValue()) c = (int)k + 1; }
                it.push_back({ T("Aparelho do microfone", "Microphone device"), o[c], [](int d) {
                    int c2 = 0;
                    for (size_t k = 0; k < mics.size(); k++) if (mics[k] == Settings::values.input_device.GetValue()) c2 = (int)k + 1;
                    const int n = (int)mics.size() + 1;
                    c2 = (c2 + d + n) % n;
                    Settings::values.input_device.SetValue(c2 == 0 ? std::string("Auto") : mics[c2 - 1]); }, nullptr, true });
                it.back().opts = o; it.back().optIdx = c;
            }
        }
        it.push_back({ T("Esticar o som (sem estalos)", "Audio stretching"), OnOff(v.enable_audio_stretching.GetValue()),
                       Toggle([&] { return v.enable_audio_stretching.GetValue(); }, [&](bool b) { v.enable_audio_stretching.SetValue(b); }) });
        it.push_back({ T("Som em tempo real", "Realtime audio"), OnOff(v.enable_realtime_audio.GetValue()),
                       Toggle([&] { return v.enable_realtime_audio.GetValue(); }, [&](bool b) { v.enable_realtime_audio.SetValue(b); }), nullptr, false,
                       T("Acompanha a velocidade quando o jogo fica lento", "Follows the speed when the game slows down") });
        static const char* ae[] = { "HLE (rápido)", "LLE (preciso)", "LLE em várias threads" };
        static const char* aeEn[] = { "HLE (fast)", "LLE (accurate)", "LLE multithreaded" };
        it.push_back({ T("Emulação do som", "Audio emulation"), (EN() ? aeEn : ae)[(int)v.audio_emulation.GetValue()], Cycle(v.audio_emulation, 3), nullptr, true }); OptsArr(it.back(), EN() ? aeEn : ae, 3, (int)v.audio_emulation.GetValue());
        break;
    }

    case P_System:
    {
        auto pct = [](double x) { return x <= 0 ? T("Sem limite", "Unlimited") : std::to_string((int)std::lround(x)) + "%"; };
        it.push_back({ T("Limite de velocidade", "Speed limit"), pct(v.frame_limit.GetValue()),
                       [&](int d) { double x = v.frame_limit.GetValue() + d * 25; v.frame_limit.SetValue(x > 500 ? 0 : x < 0 ? 500 : x); } }); it.back().bar = v.frame_limit.GetValue() <= 0 ? 1.f : (float)std::min(1.0, v.frame_limit.GetValue() / 500.0); it.back().def = "100%";
        it.push_back({ WithKey(T("Velocidade do acelerar", "Fast-forward speed"), H_Turbo), pct(v.turbo_limit.GetValue()),
                       [&](int d) { double x = v.turbo_limit.GetValue() + d * 50; v.turbo_limit.SetValue(x > 1000 ? 0 : x < 0 ? 1000 : x); } }); it.back().bar = v.turbo_limit.GetValue() <= 0 ? 1.f : (float)std::min(1.0, v.turbo_limit.GetValue() / 1000.0);
        {
            static const char* rp[] = { "Mesma", "1x", "2x", "3x", "4x" };
            static const char* re[] = { "Same", "1x", "2x", "3x", "4x" };
            it.push_back({ T("Resolução ao acelerar", "Resolution while fast-forwarding"), (EN() ? re : rp)[gCfg.turboRes],
                           [](int d) { gCfg.turboRes = (gCfg.turboRes + d + 5) % 5; }, nullptr, false,
                           T("A resolução alta é o que limita o acelerar: 2x chega a ~260%, 1x a ~350%",
                             "High resolution is what limits fast-forward: 2x reaches ~260%, 1x ~350%") }); OptsArr(it.back(), EN() ? re : rp, 5, gCfg.turboRes);
        }
        it.push_back({ WithKey(T("Acelerar", "Fast-forward"), H_Turbo), gCfg.turboToggle ? T("Liga / desliga", "Toggles") : T("Enquanto segura", "While held"),
                       [](int) { gCfg.turboToggle = !gCfg.turboToggle; } }); it.back().opts = { T("Enquanto segura", "While held"), T("Liga / desliga", "Toggles") }; it.back().optIdx = gCfg.turboToggle ? 1 : 0;
        it.push_back({ T("Acelerar batalhas sozinho", "Fast-forward battles by themselves"), OnOff(gCfg.turboBattle), [](int) { gCfg.turboBattle = !gCfg.turboBattle; }, nullptr, false,
                       T("Durante as batalhas o jogo corre na velocidade do acelerar, sem apertar nada", "During battles the game runs at the fast-forward speed, without pressing anything") });
        it.back().def = T("Desligado", "Off");
        it.push_back({ WithKey(T("Acelerar só em batalha", "Fast-forward only in battles"), H_Turbo), OnOff(gCfg.turboOnlyBattle), [](int) { gCfg.turboOnlyBattle = !gCfg.turboOnlyBattle; }, nullptr, false,
                       T("A tecla de acelerar só funciona durante as batalhas", "The fast-forward key works only during battles") });
        it.back().def = T("Desligado", "Off");
        it.push_back({ T("Clock da CPU emulada", "Emulated CPU clock"), std::to_string(v.cpu_clock_percentage.GetValue()) + "%",
                       [&](int d) { v.cpu_clock_percentage.SetValue(std::clamp(v.cpu_clock_percentage.GetValue() + d * 25, 25, 400)); }, nullptr, false,
                       T("Acima de 100% tira lentidão do jogo; pode causar erros", "Above 100% removes in-game slowdown; may cause glitches") }); it.back().bar = (v.cpu_clock_percentage.GetValue() - 25) / 375.f; it.back().def = "100%";
        it.push_back({ T("Pausar com o menu aberto", "Pause while the menu is open"), OnOff(gCfg.pauseInMenu),
                       [](int) { gCfg.pauseInMenu = !gCfg.pauseInMenu; }, nullptr, false,
                       T("Desligado: o jogo continua atrás do menu e cada mudança (som, imagem) aparece na hora",
                         "Off: the game keeps running behind the menu and every change (sound, image) shows at once") });
        if (PORTE_REWIND_HABILITADO) {
        it.push_back({ WithKey(T("Voltar no tempo (rewind)", "Rewind"), H_Rewind), OnOff(gCfg.rewindOn), [](int) { gCfg.rewindOn = !gCfg.rewindOn; }, nullptr, false,
                       T("Guarda pontos na memória enquanto você joga; a tecla volta para o último (aperte de novo para voltar mais). Cada ponto pausa o jogo por ~0,2 s e usa ~25 MB",
                         "Keeps points in memory while you play; the key goes back to the last one (press again to go further). Each point pauses the game for ~0.2 s and uses ~25 MB") });
        it.back().def = T("Desligado", "Off");
        {
            static const int ev[] = { 2, 5, 10 };
            int c = 1;
            for (int k = 0; k < 3; k++) if (gCfg.rewindEvery == ev[k]) c = k;
            std::vector<std::string> o { T("a cada 2 s", "every 2 s"), T("a cada 5 s", "every 5 s"), T("a cada 10 s", "every 10 s") };
            it.push_back({ T("Pontos do rewind", "Rewind points"), o[c], [](int d) { int c2 = 1; for (int k = 0; k < 3; k++) if (gCfg.rewindEvery == ev[k]) c2 = k; gCfg.rewindEvery = ev[std::clamp(c2 + d, 0, 2)]; }, nullptr, false,
                           T("Mais perto = volta com mais precisão, mas cada ponto faz o jogo parar por um instante", "Closer = goes back more precisely, but each point stops the game for an instant") });
            it.back().opts = o; it.back().optIdx = c; it.back().def = o[1];
            static const int kp[] = { 30, 60, 120 };
            int c3 = 1;
            for (int k = 0; k < 3; k++) if (gCfg.rewindKeep == kp[k]) c3 = k;
            std::vector<std::string> o2 { T("30 s para trás", "30 s back"), T("1 min para trás", "1 min back"), T("2 min para trás", "2 min back") };
            it.push_back({ T("Quanto guardar", "How much to keep"), o2[c3], [](int d) { int c4 = 1; for (int k = 0; k < 3; k++) if (gCfg.rewindKeep == kp[k]) c4 = k; gCfg.rewindKeep = kp[std::clamp(c4 + d, 0, 2)]; }, nullptr, false,
                           T("Memória: (segundos / intervalo) x ~25 MB (1 min a cada 5 s = ~300 MB)", "Memory: (seconds / interval) x ~25 MB (1 min every 5 s = ~300 MB)") });
            it.back().opts = o2; it.back().optIdx = c3; it.back().def = o2[1];
        }
        }   // (PORTE_REWIND_HABILITADO)
        it.push_back({ T("Salvar estado ao sair", "Save state on exit"), OnOff(gCfg.autoSaveExit), [](int) { gCfg.autoSaveExit = !gCfg.autoSaveExit; }, nullptr, false,
                       T("Ao fechar o jogo, um estado vai para o slot \"automático\" (aba Saves). Não substitui salvar dentro do jogo",
                         "When the game closes, a state goes to the \"automatic\" slot (Saves tab). It does not replace saving in the game") }); it.back().def = T("Desligado", "Off");
        it.push_back({ T("Continuar de onde parou ao abrir", "Continue where you left off"), OnOff(gCfg.autoLoadStart), [](int) { gCfg.autoLoadStart = !gCfg.autoLoadStart; }, nullptr, false,
                       T("Ao abrir, carrega o estado automático alguns segundos depois do jogo começar", "When opening, loads the automatic state a few seconds after the game starts") }); it.back().def = T("Desligado", "Off");
        it.push_back({ T("Pausar sem foco", "Pause when in the background"), OnOff(gCfg.pauseUnfocused),
                       [](int) { gCfg.pauseUnfocused = !gCfg.pauseUnfocused; }, nullptr, false,
                       T("O jogo para (e fica mudo) enquanto você usa outro programa e volta quando você clica na janela dele",
                         "The game stops (and goes silent) while you use another program and resumes when you click its window") }); it.back().def = T("Desligado", "Off");
        {
            const std::vector<std::string> pr { T("Normal", "Normal"), T("Acima do normal", "Above normal"), T("Alta", "High") };
            it.push_back({ T("Prioridade do processo", "Process priority"), pr[gCfg.priority],
                           [](int d) { gCfg.priority = (gCfg.priority + d + 3) % 3; }, nullptr, false,
                           T("Mais alta ajuda quando o PC está ocupado com outras coisas (navegador, Discord); outros programas podem ficar um pouco mais lentos",
                             "Higher helps when the PC is busy with other things (browser, Discord); other programs may get a little slower") });
            it.back().opts = pr; it.back().optIdx = gCfg.priority; it.back().def = pr[0];
        }
        {
            using DS = DiscordRpc::Status;
            const DS st = DiscordRpc::status;
            const std::string why = !gCfg.discord ? T("Desligado", "Off")
                                  : st == DS::Connected ? T("Conectado: o seu perfil mostra o jogo e a cena (explorando / em batalha)", "Connected: your profile shows the game and the scene (exploring / in a battle)")
                                  : st == DS::NoId ? T("Falta o ID do aplicativo do Discord (discord_id no config.ini)", "Missing the Discord application ID (discord_id in config.ini)")
                                  : st == DS::BadId ? T("O Discord recusou o ID do aplicativo (discord_id no config.ini)", "Discord refused the application ID (discord_id in config.ini)")
                                  : st == DS::NoDiscord ? T("O Discord não está aberto (tenta de novo a cada 15 s)", "Discord is not open (tries again every 15 s)")
                                  : T("Conectando...", "Connecting...");
            it.push_back({ T("Discord (mostrar o que você está jogando)", "Discord (show what you are playing)"), OnOff(gCfg.discord),
                           [](int) {
                               gCfg.discord = !gCfg.discord;
                               if (gCfg.discord && !gHiddenRun) DiscordRpc::Start(gCfg.discordId.empty() ? DiscordRpc::kDiscordIdPadrao : gCfg.discordId, kGameName);
                               else DiscordRpc::Stop();
                           }, nullptr, false, why });
            it.back().def = T("Ligado", "On");
        }
        it.push_back({ T("Log avançado (para relatar problemas)", "Advanced log (to report problems)"), OnOff(gCfg.logAdv),
                       [](int) { gCfg.logAdv = !gCfg.logAdv; Toast(T("Vale ao abrir o jogo de novo", "Takes effect the next time the game opens")); }, nullptr, false,
                       T("Grava tudo num arquivo da pasta log: FPS e engasgos segundo a segundo, quedas (com minidump), programas que entram no jogo, teclas e opções trocadas. F12 marca o momento. Vale ao reabrir",
                         "Writes everything to a file in the log folder: FPS and hitches every second, crashes (with a minidump), programs that hook into the game, keys and changed options. F12 marks the moment. Takes effect on reopening") });
        it.back().def = T("Desligado", "Off");
        it.push_back({ T("Abrir a pasta de logs", "Open the logs folder"), "", nullptr, [] {
            CreateDirectoryW(Widen(gDataDir + "log").c_str(), nullptr); OpenPath(gDataDir + "log"); } });
        it.push_back({ T("Modo New 3DS", "New 3DS mode"), OnOff(v.is_new_3ds.GetValue()),
                       Toggle([&] { return v.is_new_3ds.GetValue(); }, [&](bool b) { v.is_new_3ds.SetValue(b); }), nullptr, true });
        break;
    }

    case P_Trophies:
    {
        int n = 0;
        for (int i = 0; i < kAchCount; i++) n += gAch.got.count(kAch[i].id) ? 1 : 0;
        const std::string how = gAch.live ? T("Lidas ao vivo da memória do jogo: aparecem na hora em que acontecem.",
                                              "Read live from the game's memory: they show up the moment they happen.")
                                          : T("Esperando o jogo carregar o save para ler o progresso.",
                                              "Waiting for the game to load the save to read the progress.");
        it.push_back({ T("Desbloqueadas", "Unlocked"), std::to_string(n) + " / " + std::to_string(kAchCount), nullptr, nullptr, false, how });
        for (int i = 0; i < kAchCount; i++)
        {
            auto g = gAch.got.find(kAch[i].id);
            std::string val;
            if (g != gAch.got.end())
            {
                const time_t t = (time_t)g->second;
                tm lt {};
                localtime_s(&lt, &t);
                char d[32];
                strftime(d, sizeof(d), EN() ? "%Y-%m-%d" : "%d/%m/%Y", &lt);
                val = d;
            }
            else
            {
                val = AchProgress(i);
                if (val.empty()) val = T("bloqueada", "locked");
            }
            const bool has = g != gAch.got.end();
            it.push_back({ PadIcon((has ? kIconAch : kIconAchLocked) + i) + " " + AchName(i), val, nullptr,
                           has ? std::function<void()>([] {}) : nullptr, false, AchTierName(i) + "  ·  " + AchDesc(i) });   // (unlocked: drawn bright)
        }
        break;
    }

    case P_Controls:
    {
        const std::string menuHint = PadIcon(SDL_CONTROLLER_BUTTON_GUIDE) + T(" ou ", " or ") + PadIcon(SDL_CONTROLLER_BUTTON_RIGHTSTICK) +
                                     T(" abrem o menu  ·  ", " open the menu  ·  ") + PadIcon(SDL_CONTROLLER_BUTTON_LEFTSTICK) + T(" = Circle Pad", " = Circle Pad");
        it.push_back(Header(T("Controle", "Controller")));
        it.push_back({ T("Conectado", "Connected"), [] {
            for (int i = 0; i < SDL_NumJoysticks(); i++)
                if (SDL_IsGameController(i)) return std::string(SDL_GameControllerNameForIndex(i));
            return T("nenhum", "none"); }(), nullptr, nullptr, false, menuHint });
        {
            std::string val = gCfg.padNames == 0 ? T("Automático (", "Automatic (") + PadStyleName(PadStyle()) + ")" : PadStyleName(gCfg.padNames);
            it.push_back({ T("Ícones dos botões", "Button icons"), val, [](int d) { gCfg.padNames = (gCfg.padNames + d + 4) % 4; }, nullptr, false,
                           T("Como os botões aparecem: do controle conectado (Automático), Xbox, PlayStation ou Nintendo. Só muda o desenho, não o que cada botão faz.",
                             "How the buttons are shown: like the connected controller (Automatic), Xbox, PlayStation or Nintendo. Only the drawing changes, not what each button does.") }); it.back().opts = { T("Automático", "Automatic"), "Xbox", "PlayStation", "Nintendo" }; it.back().optIdx = gCfg.padNames;
        }
        it.push_back({ T("Touchpad como toque (PS4 / PS5)", "Touchpad as touch (PS4 / PS5)"), OnOff(gCfg.padTouch), [](int) { gCfg.padTouch = !gCfg.padTouch; }, nullptr, false,
                       T("O touchpad do DualShock 4 / DualSense toca a tela de baixo: a posição do dedo no touchpad = o ponto na tela",
                         "The DualShock 4 / DualSense touchpad touches the bottom screen: the finger's place on the pad = the point on the screen") });
        it.back().def = T("Ligado", "On");
        it.push_back({ T("Zona morta do analógico", "Stick dead zone"), std::to_string(gCfg.deadzone) + "%",
                       [](int d) { gCfg.deadzone = std::clamp(gCfg.deadzone + d * 5, 10, 90); }, nullptr, false,
                       T("Quanto o analógico precisa inclinar para o personagem andar. Menor = responde mais cedo; maior = ignora controle gasto (drift)",
                         "How far the stick must tilt before the character walks. Lower = reacts sooner; higher = ignores a worn stick (drift)") });
        it.back().bar = (gCfg.deadzone - 10) / 80.f; it.back().def = "45%";
        for (int b = 0; b < kPadBtns; b++)
        {
            std::string val = gMenu.waitPad == b ? T("aperte um botão…", "press a button…") : PadLabel(gCfg.pad[b]);
            static const char* dirPt[] = { "Direcional cima", "Direcional baixo", "Direcional esquerda", "Direcional direita" };
            static const char* dirEn[] = { "D-pad up", "D-pad down", "D-pad left", "D-pad right" };
            const std::string name = b >= B_Up && b <= B_Right ? (EN() ? dirEn : dirPt)[b - B_Up] : BtnName(b);
            it.push_back({ "3DS " + name, val, nullptr, [b] { gMenu.waitPad = b; }, false,
                           T("Enter e depois o botão do controle  ·  ", "Enter, then the controller button  ·  ") + PadIcon(SDL_CONTROLLER_BUTTON_GUIDE) +
                               T(" ou Esc cancela", " or Esc cancels") });
        }
        it.push_back({ "3DS Circle Pad", PadLabel(SDL_CONTROLLER_BUTTON_LEFTSTICK), nullptr, nullptr, false, menuHint });
        it.push_back({ T("Restaurar botões do controle", "Restore default controller buttons"), "", nullptr, [] {
            std::copy(std::begin(kDefaultPad), std::end(kDefaultPad), gCfg.pad); Toast(T("Botões do controle restaurados", "Controller buttons restored")); } });

        it.push_back(Header(T("Atalhos", "Shortcuts")));
        {
            static const char* hp[H_Count] = { "Salvar estado", "Carregar estado", "Disposição das telas", "Inverter telas", "Mostrar FPS", "Tela cheia", "Captura de tela", "Acelerar", "Modo foto", "Voltar no tempo (rewind)" };
            static const char* he[H_Count] = { "Save state", "Load state", "Screen layout", "Swap screens", "Show FPS", "Fullscreen", "Screenshot", "Fast-forward", "Photo mode", "Rewind" };
            for (int h = 0; h < H_Count; h++)
                if (h != H_Rewind || PORTE_REWIND_HABILITADO)
                it.push_back({ EN() ? he[h] : hp[h], gMenu.waitHot == h ? T("aperte uma tecla…", "press a key…") : HotName(h), nullptr, [h] { gMenu.waitHot = h; }, false,
                               T("Enter e depois a tecla (Esc cancela). Esc e F1 sempre abrem o menu", "Enter, then the key (Esc cancels). Esc and F1 always open the menu") });
            it.push_back({ T("Restaurar atalhos padrão", "Restore default shortcuts"), "", nullptr, [] {
                std::copy(std::begin(kDefaultHot), std::end(kDefaultHot), gCfg.hot); SaveConfig(); Toast(T("Atalhos restaurados", "Shortcuts restored")); } });
        }
        it.push_back(Header(T("Teclado", "Keyboard")));
        for (int b = 0; b < B_Count; b++)
        {
            std::string val = gMenu.waitKey == b ? T("aperte uma tecla…", "press a key…") : SDL_GetScancodeName((SDL_Scancode)gCfg.keys[b]);
            it.push_back({ BtnName(b), val, nullptr, [b] { gMenu.waitKey = b; } });
        }
        it.push_back({ T("Restaurar teclas padrão", "Restore default keys"), "", nullptr, [] {
            std::copy(std::begin(kDefaultKeys), std::end(kDefaultKeys), gCfg.keys); ApplyLive(); } });
        break;
    }

    case P_Look:
        it.push_back({ T("Tema", "Theme"), Th().name, [](int d) { gCfg.theme = (gCfg.theme + d + (int)std::size(kThemes)) % (int)std::size(kThemes); } }); { std::vector<std::string> o; for (const Theme& t : kThemes) o.push_back(t.name); it.back().opts = o; it.back().optIdx = gCfg.theme; }
        {
            static const char* gl[] = { "Nada", "Pouco", "Médio", "Bastante" };
            static const char* glEn[] = { "None", "A little", "Medium", "A lot" };
            it.push_back({ T("Jogo atrás do menu", "Game behind the menu"), (EN() ? glEn : gl)[gCfg.glass],
                           [](int d) { gCfg.glass = std::clamp(gCfg.glass + d, 0, 3); } }); OptsArr(it.back(), EN() ? glEn : gl, 4, gCfg.glass);
        }
        {
            static const int sc[] = { 80, 90, 100, 115, 130 };
            int cur = 2;
            for (int k = 0; k < 5; k++) if (gCfg.uiScale == sc[k]) cur = k;
            std::vector<std::string> o;
            for (int k : sc) o.push_back(std::to_string(k) + "%");
            it.push_back({ T("Tamanho do menu", "Menu size"), o[cur], [](int d) {
                int c = 2; for (int k = 0; k < 5; k++) if (gCfg.uiScale == sc[k]) c = k;
                gCfg.uiScale = sc[std::clamp(c + d, 0, 4)]; }, nullptr, false, T("Textos e botões do menu maiores ou menores", "Bigger or smaller menu text and buttons") });
            it.back().opts = o; it.back().optIdx = cur; it.back().def = "100%";
        }
        {
            static const char* cp[] = { "Do tema", "Azul", "Roxo", "Verde", "Vermelho", "Dourado", "Rosa", "Ciano" };
            static const char* ce[] = { "Theme's", "Blue", "Purple", "Green", "Red", "Gold", "Pink", "Cyan" };
            it.push_back({ T("Cor de destaque", "Accent colour"), (EN() ? ce : cp)[gCfg.accent], [](int d) { gCfg.accent = (gCfg.accent + d + 8) % 8; } });
            OptsArr(it.back(), EN() ? ce : cp, 8, gCfg.accent); it.back().def = EN() ? ce[0] : cp[0];
        }
        it.push_back({ T("Animações do menu", "Menu animations"), OnOff(gCfg.menuAnim), [](int) { gCfg.menuAnim = !gCfg.menuAnim; } });
        it.back().def = T("Ligado", "On");
        it.push_back({ T("Idioma / Language", "Idioma / Language"), EN() ? "English" : "Português", [](int) { gCfg.lang = EN() ? 0 : 1; } }); it.back().opts = { "Português", "English" }; it.back().optIdx = EN() ? 1 : 0;
        break;

    case P_About:
        it.push_back({ kVersion, "", nullptr, nullptr });
        it.push_back({ T("Núcleo: Azahar 2126.2 (GPLv2+)", "Core: Azahar 2126.2 (GPLv2+)"), "", nullptr, [] { OpenPath("https://azahar-emu.org"); } });
        it.push_back({ T("Fonte do menu: DejaVu Sans", "Menu font: DejaVu Sans"), "", nullptr, nullptr });
        it.push_back({ T("Trapaças (Gateway): abrir o arquivo", "Cheats (Gateway): open the file"), "", nullptr, [] {
            char n[32];
            snprintf(n, sizeof(n), "%016llX.txt", (unsigned long long)gProgramId);
            std::string p = FileUtil::GetUserPath(FileUtil::UserPath::CheatsDir) + n;
            if (!FileUtil::Exists(p)) { std::ofstream f(P(p)); f << "# [Nome da trapaça]\n# código\n"; }
            OpenPath(p); }, false, T("Valem ao reiniciar o jogo", "Take effect when the game restarts") });
        it.push_back({ T("Abrir a pasta de dados", "Open the data folder"), "", nullptr, [] { OpenPath(gDataDir); } });
        it.push_back({ T("Pokémon é marca da Nintendo, Game Freak e The Pokémon Company.", "Pokémon is a trademark of Nintendo, Game Freak and The Pokémon Company."), "", nullptr, nullptr });
        it.push_back({ T("Projeto não oficial. Não inclui o jogo.", "Unofficial project. The game is not included."), "", nullptr, nullptr });
        break;
    }
    return it;
}

static void MenuChange(MenuItem& m, int d)
{
    if (m.change) { m.change(d); ApplyLive(); }
}
static void MenuActivate(MenuItem& m)
{
    if (m.activate) m.activate();
    else if (m.change) { m.change(1); ApplyLive(); }
}

// what the menu shows of a page: the items with the folded sections left out; section titles fold on Enter / click
static std::set<std::string> gCollapsed;   // "page|title" of the folded sections
static std::vector<MenuItem> VisibleItems(int page)
{
    auto all = BuildItems(page);
    std::vector<MenuItem> out;
    bool skip = false;
    for (MenuItem& m : all)
    {
        if (m.header)
        {
            const std::string key = std::to_string(page) + "|" + m.label;
            m.collapsed = gCollapsed.count(key) != 0;
            skip = m.collapsed;
            m.activate = [key] { if (gCollapsed.count(key)) gCollapsed.erase(key); else gCollapsed.insert(key); };
            out.push_back(std::move(m));
            continue;
        }
        if (m.restart && m.sub.empty()) m.sub = T("Vale ao reiniciar o jogo", "Restart required");
        if (m.opts.empty() && m.change && m.bar < 0 && (m.value == OnOff(true) || m.value == OnOff(false)))
        {
            m.opts = { OnOff(false), OnOff(true) };
            m.optIdx = m.value == OnOff(true) ? 1 : 0;
        }
        if (!skip) out.push_back(std::move(m));
    }
    return out;
}

// choosing in a dropdown: as many steps as needed, through the setting's own change() (cyclic or clamped alike)
static void ApplyOpt(MenuItem& m, int target)
{
    if (m.opts.empty() || !m.change || target == m.optIdx) return;
    const int d = target > m.optIdx ? 1 : -1;
    for (int k = m.optIdx; k != target; k += d) m.change(d);
    ApplyLive();
}
static void OpenPopup(int i, const MenuItem& m)
{
    gMenu.popup = i;
    gMenu.popSel = std::max(0, m.optIdx);
    gMenu.popScroll = 0;
    gMenu.popT = NowSec();
}
static void SetPage(int p)
{
    if (p == gMenu.page) return;
    gMenu.tabDir = p > gMenu.page ? 1 : -1;
    gMenu.page = p;
    gMenu.sel = gMenu.scroll = 0;
    gMenu.popup = -1;
    gMenu.tabT = NowSec();
    gMenu.selInit = false;
}
static float EaseOut(float t) { t = std::clamp(t, 0.f, 1.f); return 1 - (1 - t) * (1 - t) * (1 - t); }
static Color Mix(Color a, Color b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t }; }
// a small solid triangle (the font has no arrows): dir 0 down, 1 left, 2 right, 3 up; s = its size in pixels
static void Tri(float cx, float cy, float s, int dir, Color c)
{
    const int n = std::max(3, (int)(s * 0.6f));
    for (int k = 0; k < n; k++)
    {
        const float f = (k + 0.5f) / n, len = s * (1.0f - f);   // the base first, the tip last
        if (dir == 0) FillRect({ cx - len / 2, cy - s * 0.3f + k, len, 1.01f }, c);
        else if (dir == 3) FillRect({ cx - len / 2, cy + s * 0.3f - k, len, 1.01f }, c);
        else if (dir == 2) FillRect({ cx - s * 0.3f + k, cy - len / 2, 1.01f, len }, c);
        else FillRect({ cx + s * 0.3f - k, cy - len / 2, 1.01f, len }, c);
    }
}
// greedy word wrap
static std::vector<std::string> Wrap(const std::string& s, float size, float maxW)
{
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&] {
        if (word.empty()) return;
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextWidth(tryLine, size) > maxW) { lines.push_back(line); line = word; }
        else line = tryLine;
        word.clear();
    };
    for (char ch : s)
    {
        if (ch == ' ') flush();
        else if (ch == '\n') { flush(); lines.push_back(line); line.clear(); }
        else word += ch;
    }
    flush();
    if (!line.empty()) lines.push_back(line);
    return lines;
}
// an outlined box: border colour, then the fill inset by the border width
static void Box(const Rect& r, float radius, Color fill, Color border, float bw)
{
    RoundRect(r, radius, border);
    RoundRect({ r.x + bw, r.y + bw, r.w - 2 * bw, r.h - 2 * bw }, std::max(0.f, radius - bw), fill);
}

// draws the menu over the game (the layout: title bar, tabs, the list on the left with folding sections, the
// description of the selected setting on the right, Continue / Quit at the bottom)
static void DrawMenu(int w, int h)
{
    const Theme& th = Th();
    static const float glassAlpha[] = { 1.0f, 0.90f, 0.78f, 0.62f };
    const double nowT = NowSec();
    const float dtA = gMenu.lastT > 0 ? (float)std::min(0.1, nowT - gMenu.lastT) : 1.f;
    gMenu.lastT = nowT;
    // opening: the whole menu fades in and rises a little (0.22 s)
    static const double testSlow = getenv("PORTE_MENU_LENTO") ? std::max(1.0, atof(getenv("PORTE_MENU_LENTO"))) : 1.0;   // (test) slower animations
    const double slow = gCfg.menuAnim ? testSlow : 1e-4;   // Aparência > Animações do menu (off: everything at once)
    const float eOpen = gMenu.openT > 0 ? EaseOut((float)((nowT - gMenu.openT) / (0.22 * slow))) : 1.f;
    gUi.alpha = eOpen;
    gUi.ox = 0;
    gUi.oy = (1 - eOpen) * std::min(w, h) * 0.03f;
    FillRect({ 0, -gUi.oy, (float)w, (float)h }, WithAlpha(th.bg, glassAlpha[gCfg.glass]));

    const float u = std::clamp(std::clamp(std::min(h / 27.f, w / 36.f), 9.f, 34.f) * gCfg.uiScale / 100.f, 7.f, 44.f);   // base unit (Aparência > Tamanho do menu)
    const float bw = std::max(1.f, u * 0.07f);                              // border width
    auto items = VisibleItems(gMenu.page);
    if (gMenu.sel >= (int)items.size()) gMenu.sel = std::max(0, (int)items.size() - 1);
    if (!items.empty() && items[gMenu.sel].header && !items[gMenu.sel].activate) gMenu.sel = NextSelectable(items, gMenu.sel, 1);
    const bool dark = th.bg.r + th.bg.g + th.bg.b < 1.5f;
    const Color rowC = Mix(th.card, th.bg, 0.35f), btnC = Mix(th.card, th.text, dark ? 0.10f : 0.06f);
    const Color lineC = WithAlpha(th.text, 0.14f);
    auto hov = [&](const Rect& r) { return r.Contains(gMenu.mx, gMenu.my); };

    // the whole frame
    const float m = u * 0.7f;
    Rect P { m, m, w - 2 * m, h - 2 * m };
    RoundRect(P, u * 0.6f, WithAlpha(th.panel, 0.97f));

    // title bar: emblem, "POKÉMON <GAME> · PC RECOMP", version; an accent line under it
    Rect H { P.x + u * 0.6f, P.y + u * 0.5f, P.w - u * 1.2f, u * 2.4f };
    RoundRect(H, u * 0.45f, Mix(th.bg, th.panel, 0.4f));
    FillRect({ H.x + u * 0.2f, H.y + H.h - u * 0.13f, H.w - u * 0.4f, u * 0.13f }, th.accent);
    {
        const float cx = H.x + u * 1.45f, cy = H.y + H.h * 0.47f, r = u * 0.82f;
        std::string nome = Jogos::gAtual ? Jogos::gAtual->nome : "Ultra Moon";
        const bool sol = nome.find("Sun") != std::string::npos;
        if (sol)
        {
            RoundRect({ cx - r * 0.62f, cy - r * 0.62f, r * 1.24f, r * 1.24f }, r, th.accent);           // the sun: a disc and a ring
            Box({ cx - r, cy - r, r * 2, r * 2 }, r, WithAlpha(th.accent, 0.0f), WithAlpha(th.accent, 0.55f), bw * 1.5f);
        }
        else
        {
            RoundRect({ cx - r, cy - r, r * 2, r * 2 }, r, th.accent);                                      // the moon: a crescent
            RoundRect({ cx - r * 0.55f, cy - r * 1.12f, r * 2, r * 2 }, r, Mix(th.bg, th.panel, 0.4f));
        }
        std::string up = nome;
        for (char& ch : up) ch = (char)toupper((unsigned char)ch);
        const std::string title = "POKÉMON " + up + "  ·  PC RECOMP";
        const float ts = u * 1.02f;
        Text(title, H.x + u * 2.75f, H.y + (H.h - ts) / 2 - u * 0.1f, Fit(title, ts, H.w * 0.62f), th.accent);
        const std::string ver = "Alola PC Recomp 0.2";
        Text(ver, H.x + H.w - u * 0.7f, H.y + (H.h - u * 0.72f) / 2 - u * 0.05f, u * 0.72f, th.dim, 2);
    }

    // tabs (the font shrinks when they do not fit in one row)
    gMenu.tabRects.assign(P_Count, Rect { 0, 0, 0, 0 });
    const float tabY = H.y + H.h + u * 0.55f, tabH = u * 1.75f;
    float tabFont = u * 0.8f, pad = u * 1.1f, gap = u * 0.25f;
    {
        float total = 0;
        for (int p = 0; p < P_Count; p++) total += TextWidth(PageName(p), tabFont) + pad * 2 + gap;
        const float avail = P.w - u * 1.2f;
        if (total > avail) { const float k = avail / total; tabFont *= k; pad *= k; gap *= k; }
    }
    float tx = P.x + u * 0.6f;
    for (int p = 0; p < P_Count; p++)
    {
        const std::string name = PageName(p);
        const float tw = TextWidth(name, tabFont) + pad * 2;
        Rect r { tx, tabY, tw, tabH };
        gMenu.tabRects[p] = r;
        const bool cur = p == gMenu.page;
        RoundRect(r, u * 0.4f, cur ? Mix(btnC, th.accent, 0.18f) : hov(r) ? Mix(rowC, th.accent, 0.10f) : rowC);
        FillRect({ r.x, r.y + r.h - u * 0.45f, r.w, u * 0.45f }, cur ? Mix(btnC, th.accent, 0.18f) : hov(r) ? Mix(rowC, th.accent, 0.10f) : rowC);   // (square bottom)
        if (cur) FillRect({ r.x + pad * 0.5f, r.y + r.h - u * 0.16f, r.w - pad, u * 0.12f }, th.accent);
        Text(name, r.x + r.w / 2, r.y + (tabH - tabFont) / 2 - u * 0.08f, tabFont, cur ? th.text : Mix(th.text, th.dim, 0.4f), 1);
        tx += tw + gap;
    }
    FillRect({ P.x + u * 0.3f, tabY + tabH, P.w - u * 0.6f, bw }, lineC);

    // the content: list (left) and description (right) - after a tab change it slides in from that side (0.2 s)
    const float eTab = gMenu.tabT > 0 ? EaseOut((float)((nowT - gMenu.tabT) / (0.2 * slow))) : 1.f;
    const float baseAlpha = gUi.alpha, baseOx = gUi.ox;
    gUi.ox = baseOx + (1 - eTab) * u * 3.0f * gMenu.tabDir;
    gUi.alpha = baseAlpha * (0.25f + 0.75f * eTab);
    const float footH = u * 3.0f;
    const float cTop = tabY + tabH + u * 0.7f, cBot = P.y + P.h - footH;
    Rect L { P.x + u * 0.6f, cTop, (P.w - u * 1.2f) * 0.58f, cBot - cTop };
    Box(L, u * 0.4f, Mix(th.panel, th.bg, 0.25f), lineC, bw);
    const float dx = L.x + L.w + u * 0.9f;
    FillRect({ dx - u * 0.45f, cTop, bw, cBot - cTop }, lineC);
    Rect D { dx, cTop, P.x + P.w - u * 0.6f - dx, cBot - cTop };

    // rows: variable height (with a note: taller); the list scrolls by items
    auto rowHeight = [&](const MenuItem& it) { return it.header ? u * 1.85f : (it.sub.empty() ? u * 2.15f : u * 2.65f) + (it.bar >= 0 ? u * 0.25f : 0); };
    const float innerTop = L.y + u * 0.35f, innerBot = L.y + L.h - u * 0.35f, sbW = u * 0.32f;
    const float rowX = L.x + u * 0.35f, rowW = L.w - u * 0.35f * 2 - sbW - u * 0.3f;
    if (gMenu.sel < gMenu.scroll) gMenu.scroll = gMenu.sel;
    for (;;)   // keep the selected row (and the section title above it) in view
    {
        float y = 0;
        for (int i = gMenu.scroll; i <= gMenu.sel && i < (int)items.size(); i++) y += rowHeight(items[i]) + u * 0.22f;
        if (y <= innerBot - innerTop || gMenu.scroll >= gMenu.sel) break;
        gMenu.scroll++;
    }
    gMenu.scroll = std::clamp(gMenu.scroll, 0, std::max(0, (int)items.size() - 1));
    const size_t n = items.size();
    gMenu.itemRects.assign(n, Rect { 0, 0, 0, 0 });
    gMenu.valueRects.assign(n, Rect { 0, 0, 0, 0 });
    gMenu.leftRects.assign(n, Rect { 0, 0, 0, 0 });
    gMenu.rightRects.assign(n, Rect { 0, 0, 0, 0 });
    float y = innerTop;
    int lastShown = gMenu.scroll - 1;
    for (int i = gMenu.scroll; i < (int)n; i++)
    {
        const MenuItem& it = items[i];
        const float rh = rowHeight(it);
        if (y + rh > innerBot) break;
        lastShown = i;
        Rect r { rowX, y, rowW, rh };
        gMenu.itemRects[i] = r;
        const bool sel = i == gMenu.sel, hv = hov(r);
        if (it.header)
        {
            if (sel)
            {
                if (!gMenu.selInit) { gMenu.selX = r.x; gMenu.selY = r.y; gMenu.selW = r.w; gMenu.selH = r.h; gMenu.selInit = true; }
                const float k = 1 - std::exp(-dtA * 22.f);
                gMenu.selX += (r.x - gMenu.selX) * k; gMenu.selY += (r.y - gMenu.selY) * k;
                gMenu.selW += (r.w - gMenu.selW) * k; gMenu.selH += (r.h - gMenu.selH) * k;
            }
            RoundRect(r, u * 0.3f, sel ? Mix(th.accent, th.panel, 0.55f) : Mix(th.accent, th.panel, hv ? 0.68f : 0.76f));
            Tri(r.x + u * 1.0f, r.y + r.h / 2, u * 0.62f, it.collapsed ? 2 : 0, th.text);
            Text(it.label, r.x + u * 1.9f, r.y + (r.h - u * 0.88f) / 2 - u * 0.08f, u * 0.88f, th.text);
            y += rh + u * 0.22f;
            continue;
        }
        const bool interactive = it.change || it.activate;
        if (sel)
        {
            if (!gMenu.selInit) { gMenu.selX = r.x; gMenu.selY = r.y; gMenu.selW = r.w; gMenu.selH = r.h; gMenu.selInit = true; }
            const float k = 1 - std::exp(-dtA * 22.f);   // glide toward the selected row
            gMenu.selX += (r.x - gMenu.selX) * k; gMenu.selY += (r.y - gMenu.selY) * k;
            gMenu.selW += (r.w - gMenu.selW) * k; gMenu.selH += (r.h - gMenu.selH) * k;
            Box(r, u * 0.3f, Mix(rowC, th.accent, 0.10f), lineC, bw);   // (the gliding outline is drawn after all rows)
        }
        else Box(r, u * 0.3f, hv ? Mix(rowC, th.accent, 0.06f) : rowC, lineC, bw);
        // the value control on the right
        const float vh = u * 1.5f, vw = std::min(rowW * 0.44f, u * 11.f);
        const float barSpace = it.bar >= 0 ? u * 0.25f : 0;
        Rect vb { r.x + r.w - vw - u * 0.45f, r.y + (r.h - barSpace - vh) / 2, vw, vh };
        const float ts = u * 0.82f;
        if (!it.value.empty() && it.change && it.bar >= 0)
        {
            gMenu.valueRects[i] = vb;
            Rect lb { vb.x, vb.y, vh * 1.1f, vh }, rb { vb.x + vb.w - vh * 1.1f, vb.y, vh * 1.1f, vh };
            gMenu.leftRects[i] = lb;
            gMenu.rightRects[i] = rb;
            RoundRect(lb, u * 0.25f, hov(lb) ? Mix(btnC, th.accent, 0.3f) : btnC);
            RoundRect(rb, u * 0.25f, hov(rb) ? Mix(btnC, th.accent, 0.3f) : btnC);
            Tri(lb.x + lb.w / 2, lb.y + lb.h / 2, u * 0.55f, 1, th.text);
            Tri(rb.x + rb.w / 2, rb.y + rb.h / 2, u * 0.55f, 2, th.text);
            Text(it.value, vb.x + vb.w / 2, vb.y + (vh - ts) / 2 - u * 0.06f, Fit(it.value, ts, vb.w - vh * 2.6f), sel ? th.accent : th.text, 1);
            // the bar under the row: how far along the setting is
            const float bx = r.x + u * 0.6f, bxe = vb.x + vb.w, by = r.y + r.h - u * 0.42f;
            FillRect({ bx, by, bxe - bx, std::max(1.f, u * 0.08f) }, WithAlpha(th.text, 0.18f));
            const float f = std::clamp(it.bar, 0.f, 1.f);
            FillRect({ bx, by, (bxe - bx) * f, std::max(1.f, u * 0.08f) }, th.accent);
            RoundRect({ bx + (bxe - bx) * f - u * 0.17f, by - u * 0.13f, u * 0.34f, u * 0.34f }, u * 0.17f, th.accent);
        }
        else if (!it.value.empty() && it.change)
        {
            gMenu.valueRects[i] = vb;
            RoundRect(vb, u * 0.25f, hov(vb) ? Mix(btnC, th.accent, 0.22f) : btnC);
            Text(it.value, vb.x + u * 0.5f, vb.y + (vh - ts) / 2 - u * 0.06f, Fit(it.value, ts, vb.w - u * 1.9f), sel ? th.accent : th.text);
            Tri(vb.x + vb.w - u * 0.75f, vb.y + vh / 2, u * 0.55f, 0, th.text);
        }
        else if (!it.value.empty())
            Text(it.value, r.x + r.w - u * 0.6f, r.y + (r.h - ts) / 2 - u * 0.06f, Fit(it.value, ts, vw), th.dim, 2);
        else if (it.activate)
            Tri(r.x + r.w - u * 0.9f, r.y + r.h / 2, u * 0.55f, 2, sel ? th.accent : th.dim);
        // name (and the note under it)
        const float labelW = (it.value.empty() ? r.w - u * 2.2f : vb.x - r.x - u * 1.0f);
        if (it.sub.empty())
            Text(it.label, r.x + u * 0.6f, r.y + (r.h - barSpace - ts) / 2 - u * 0.08f, Fit(it.label, ts, labelW), interactive ? th.text : th.dim);
        else
        {
            Text(it.label, r.x + u * 0.6f, r.y + u * 0.32f, Fit(it.label, ts, labelW), interactive ? th.text : th.dim);
            Text(it.sub, r.x + u * 0.6f, r.y + u * 0.32f + ts * 1.22f, Fit(it.sub, u * 0.62f, labelW), th.accent);
        }
        y += rh + u * 0.22f;
    }
    // the selection outline, gliding to the selected row - drawn last: only lines, nothing is covered
    if (gMenu.selInit && gMenu.sel < (int)n && gMenu.itemRects[gMenu.sel].w > 0)
    {
        const float t = bw * 1.8f, x0 = gMenu.selX, y0 = gMenu.selY, ww = gMenu.selW, hh = gMenu.selH;
        FillRect({ x0, y0, ww, t }, th.accent);
        FillRect({ x0, y0 + hh - t, ww, t }, th.accent);
        FillRect({ x0, y0, t, hh }, th.accent);
        FillRect({ x0 + ww - t, y0, t, hh }, th.accent);
    }
    // scroll bar
    if (n > 0)
    {
        Rect track { L.x + L.w - sbW - u * 0.3f, innerTop, sbW, innerBot - innerTop };
        RoundRect(track, sbW / 2, WithAlpha(th.text, 0.08f));
        const float shown = (float)std::max(1, lastShown - gMenu.scroll + 1);
        const float th0 = std::clamp(shown / n, 0.08f, 1.f) * track.h;
        const float maxScroll = std::max(1.f, (float)n - shown);
        const float ty0 = track.y + (track.h - th0) * std::clamp(gMenu.scroll / maxScroll, 0.f, 1.f);
        RoundRect({ track.x, ty0, track.w, th0 }, sbW / 2, WithAlpha(th.accent, 0.75f));
        if (lastShown < (int)n - 1) Tri(L.x + L.w / 2, L.y + L.h + u * 0.45f, u * 0.5f, 0, th.accent);
    }

    // description of the selected row
    if (gMenu.sel < (int)n)
    {
        const MenuItem& it = items[gMenu.sel];
        float yy = D.y + u * 0.3f;
        const float hs = u * 0.95f;
        for (const std::string& l : Wrap(it.label, hs, D.w)) { Text(l, D.x, yy, hs, th.accent); yy += hs * 1.3f; }
        yy += u * 0.5f;
        std::string body = it.hint;
        if (it.header) body = it.collapsed ? T("Seção recolhida. Enter (ou clique) mostra as opções dela.", "Folded section. Enter (or a click) shows its settings.")
                                           : T("Enter (ou clique) recolhe esta seção.", "Enter (or a click) folds this section.");
        else if (body.empty()) body = it.change ? T("Use ← → (ou clique) para mudar.", "Use ← → (or click) to change.")
                                                : it.activate ? T("Enter (ou clique) para usar.", "Enter (or click) to use.") : "";
        const float bs = u * 0.76f;
        for (const std::string& l : Wrap(body, bs, D.w)) { if (yy > D.y + D.h - u * 4) break; Text(l, D.x, yy, bs, th.text); yy += bs * 1.38f; }
        if (!it.sub.empty() || !it.def.empty())
        {
            yy += u * 0.5f;
            FillRect({ D.x, yy, D.w, bw }, lineC);
            yy += u * 0.7f;
            if (!it.sub.empty()) { Text(it.sub, D.x, yy, Fit(it.sub, bs, D.w), th.accent); yy += bs * 1.6f; }
            if (!it.def.empty()) Text(T("Padrão: ", "Default: ") + it.def, D.x, yy, bs, th.dim);
        }
    }
    // keys, at the bottom of the description
    {
        const std::string keys = T("Q / E abas  ·  ↑ ↓ escolhem  ·  ← → mudam  ·  Enter confirma  ·  Esc volta ao jogo",
                                   "Q / E tabs  ·  ↑ ↓ choose  ·  ← → change  ·  Enter confirms  ·  Esc back to the game");
        const float ks = u * 0.58f;
        if (TextWidth(keys, ks) <= D.w) Text(keys, D.x, D.y + D.h - u * 0.75f, ks, th.dim);
        else   // (narrow window: two lines at a readable size instead of one tiny line)
        {
            const auto lines = Wrap(keys, ks, D.w);
            float ky = D.y + D.h - u * 0.75f - (lines.size() - 1) * ks * 1.3f;
            for (const std::string& l : lines) { Text(l, D.x, ky, Fit(l, ks, D.w), th.dim); ky += ks * 1.3f; }
        }
    }

    gUi.ox = baseOx;
    gUi.alpha = baseAlpha;
    // bottom: speed on the left, Continue / Quit on the right
    if (!gFpsText.empty()) Text(gFpsText, P.x + u * 0.8f, P.y + P.h - footH / 2 - u * 0.36f, Fit(gFpsText, u * 0.66f, P.w * 0.45f), th.dim);
    const float bH = u * 1.85f, bW = u * 6.6f, by2 = P.y + P.h - footH / 2 - bH / 2;
    const char* labels[2] = { nullptr, nullptr };
    const std::string cont = T("Continuar", "Continue"), quit = T("Sair do jogo", "Quit game");
    Rect quitR { P.x + P.w - u * 0.6f - bW, by2, bW, bH }, contR { quitR.x - u * 0.6f - bW, by2, bW, bH };
    gMenu.btnRects[0] = contR;
    gMenu.btnRects[1] = quitR;
    (void)labels;
    for (int b = 0; b < 2; b++)
    {
        const Rect& r = gMenu.btnRects[b];
        Box(r, u * 0.3f, hov(r) ? Mix(btnC, th.accent, 0.28f) : btnC, hov(r) ? th.accent : lineC, bw);
        const std::string& lbl = b == 0 ? cont : quit;
        Text(lbl, r.x + r.w / 2, r.y + (r.h - u * 0.85f) / 2 - u * 0.08f, Fit(lbl, u * 0.85f, r.w - u), th.text, 1);
    }

    {   // (testing aid) PORTE_MENUPOP=1: the selected row's dropdown opens once
        static bool popTest = getenv("PORTE_MENUPOP") != nullptr;
        if (popTest && gMenu.sel < (int)n && !items[gMenu.sel].opts.empty() && gMenu.valueRects[gMenu.sel].w > 0)
        {
            popTest = false;
            OpenPopup(gMenu.sel, items[gMenu.sel]);
        }
    }
    // the open dropdown: the choices under the value box (above it when there is no room), on top of everything
    gMenu.popRects.clear();
    gMenu.popBox = Rect { 0, 0, 0, 0 };
    if (gMenu.popup >= 0 && gMenu.popup < (int)n && !items[gMenu.popup].opts.empty() && gMenu.valueRects[gMenu.popup].w > 0)
    {
        const MenuItem& it = items[gMenu.popup];
        const Rect& vb = gMenu.valueRects[gMenu.popup];
        const int count = (int)it.opts.size();
        const float oh = u * 1.4f;
        const int maxVis = std::max(3, std::min(count, (int)((h - u * 2) / oh) - 1));
        const int vis = std::min(count, std::min(maxVis, 9));
        gMenu.popSel = std::clamp(gMenu.popSel, 0, count - 1);
        if (gMenu.popSel < gMenu.popScroll) gMenu.popScroll = gMenu.popSel;
        if (gMenu.popSel >= gMenu.popScroll + vis) gMenu.popScroll = gMenu.popSel - vis + 1;
        gMenu.popScroll = std::clamp(gMenu.popScroll, 0, std::max(0, count - vis));
        const float bh = vis * oh + u * 0.4f;
        float by = vb.y + vb.h + u * 0.15f;
        const bool up = by + bh > h - u * 0.5f;
        if (up) by = vb.y - u * 0.15f - bh;
        const float ePop = EaseOut((float)((nowT - gMenu.popT) / (0.14 * slow)));
        Rect box { vb.x, by, vb.w, bh };
        gMenu.popBox = box;
        const float saveA = gUi.alpha, saveOy = gUi.oy;
        gUi.alpha = saveA * ePop;
        gUi.oy = saveOy + (1 - ePop) * u * 0.6f * (up ? 1 : -1);
        RoundRect({ box.x + u * 0.12f, box.y + u * 0.18f, box.w, box.h }, u * 0.3f, WithAlpha({ 0, 0, 0, 1 }, 0.35f));   // shadow
        Box(box, u * 0.3f, Mix(th.panel, th.card, 0.5f), th.accent, bw * 1.4f);
        gMenu.popRects.assign(count, Rect { 0, 0, 0, 0 });
        for (int k = gMenu.popScroll; k < gMenu.popScroll + vis; k++)
        {
            Rect orr { box.x + u * 0.2f, box.y + u * 0.2f + (k - gMenu.popScroll) * oh, box.w - u * 0.4f, oh - u * 0.08f };
            gMenu.popRects[k] = orr;
            const bool cur = k == it.optIdx, hl = k == gMenu.popSel || hov(orr);
            if (hl) RoundRect(orr, u * 0.22f, WithAlpha(th.accent, 0.28f));
            const float fs = u * 0.78f;
            Text(it.opts[k], orr.x + u * 0.5f, orr.y + (orr.h - fs) / 2 - u * 0.06f, Fit(it.opts[k], fs, orr.w - u * 1.6f), cur ? th.accent : th.text);
            if (cur) RoundRect({ orr.x + orr.w - u * 0.7f, orr.y + orr.h / 2 - u * 0.15f, u * 0.3f, u * 0.3f }, u * 0.15f, th.accent);   // the chosen one
        }
        if (gMenu.popScroll > 0) Tri(box.x + box.w / 2, box.y + u * 0.12f, u * 0.4f, 3, th.dim);
        if (gMenu.popScroll + vis < count) Tri(box.x + box.w / 2, box.y + box.h - u * 0.1f, u * 0.4f, 0, th.dim);
        gUi.alpha = saveA;
        gUi.oy = saveOy;
    }
    gUi.ox = gUi.oy = 0;
    gUi.alpha = 1;
}

static void DrawOverlay(int w, int h)
{
    const Theme& th = Th();
    const float u = std::clamp(std::min(h / 26.f, w / 30.f), 10.f, 40.f);
    if (gEdit.on && !gMenu.open)
    {
        auto& v = Settings::values;
        const float bw = std::max(2.f, u * 0.12f);
        auto outline = [&](float x0, float y0, float ww, float hh, Color c) {
            FillRect({ x0, y0, ww, bw }, c); FillRect({ x0, y0 + hh - bw, ww, bw }, c);
            FillRect({ x0, y0, bw, hh }, c); FillRect({ x0 + ww - bw, y0, bw, hh }, c);
        };
        outline(v.custom_top_x.GetValue(), v.custom_top_y.GetValue(), v.custom_top_width.GetValue(), v.custom_top_height.GetValue(), th.accent);
        outline(v.custom_bottom_x.GetValue(), v.custom_bottom_y.GetValue(), v.custom_bottom_width.GetValue(), v.custom_bottom_height.GetValue(), Color { 1.f, 0.8f, 0.3f, 1 });
        const std::string hint = T("Arraste as telas com o mouse · roda = tamanho · Enter salva · Esc cancela", "Drag the screens with the mouse · wheel = size · Enter saves · Esc cancels");
        float hs = Fit(hint, u * 0.6f, w - u * 2.f), hw = TextWidth(hint, hs);
        RoundRect({ (w - hw) / 2 - u * 0.5f, u * 0.4f, hw + u, hs + u * 0.6f }, u * 0.3f, WithAlpha(th.bg, 0.85f));
        Text(hint, (w - hw) / 2, u * 0.7f, hs, th.text);
        return;
    }
    if (gPhoto.on && !gMenu.open)
    {
        // Modo foto: a small tag, and the keys for the first seconds
        const std::string tag = T("MODO FOTO", "PHOTO MODE");
        float ts = u * 0.6f, tw = TextWidth(tag, ts);
        RoundRect({ u * 0.4f, u * 0.4f, tw + u * 0.8f, ts + u * 0.5f }, u * 0.3f, WithAlpha(th.accent, 0.85f));
        Text(tag, u * 0.8f, u * 0.6f, ts, th.bg);
        if (NowSec() < gPhoto.hintUntil)
        {
            const std::string hint = T("WASD andar · Espaço/Ctrl subir/descer · Shift rápido · setas/botão direito girar · roda zoom · H interface · Backspace câmera do jogo · ",
                                       "WASD move · Space/Ctrl up/down · Shift fast · arrows/right button turn · wheel zoom · H interface · Backspace game camera · ") +
                                     HotName(H_Shot) + T(" foto · ", " picture · ") + HotName(H_Photo) + T("/Esc sair", "/Esc leave");
            float hs = Fit(hint, u * 0.55f, w - u * 2.f), hw = TextWidth(hint, hs);
            RoundRect({ (w - hw) / 2 - u * 0.5f, h - hs - u * 1.2f, hw + u, hs + u * 0.6f }, u * 0.3f, WithAlpha(th.bg, 0.75f));
            Text(hint, (w - hw) / 2, h - hs - u * 0.9f, hs, th.text);
        }
        return;
    }
    if (gCfg.showFps && !gMenu.open && !gFpsText.empty())
    {
        float ts = u * 0.65f, tw = TextWidth(gFpsText, ts);
        RoundRect({ u * 0.4f, u * 0.4f, tw + u * 0.8f, ts + u * 0.5f }, u * 0.3f, WithAlpha(th.bg, 0.7f));
        Text(gFpsText, u * 0.8f, u * 0.6f, ts, th.text);
    }
    if (gTurbo && !gMenu.open)
    {
        std::string s = ">> " + std::to_string((int)Settings::values.turbo_limit.GetValue()) + "%";
        if (Settings::values.turbo_limit.GetValue() <= 0) s = ">> MAX";
        float ts = u * 0.7f, tw = TextWidth(s, ts);
        RoundRect({ w - tw - u * 1.2f, u * 0.4f, tw + u * 0.8f, ts + u * 0.5f }, u * 0.3f, WithAlpha(th.accent, 0.85f));
        Text(s, w - u * 0.8f, u * 0.6f, ts, th.bg, 2);
    }
    // achievement card: slides in at the top right, stays a few seconds (waits while the menu is open)
    {
        const Uint64 now = SDL_GetTicks64();
        constexpr Uint64 kShow = 4500;
        if (gAch.showing >= 0 && now - gAch.showSince > kShow) gAch.showing = -1;
        if (gAch.showing < 0 && !gAch.queue.empty() && !gMenu.open)
        {
            gAch.showing = gAch.queue.front();
            gAch.queue.erase(gAch.queue.begin());
            gAch.showSince = now;
        }
        if (gAch.showing >= 0)
        {
            const float age = (float)(now - gAch.showSince);
            const float in = std::clamp(age / 250.f, 0.f, 1.f), out = std::clamp((kShow - age) / 400.f, 0.f, 1.f);
            const float k = std::min(in, out), ease = 1 - (1 - k) * (1 - k);
            const bool many = gAch.showing >= kAchCount;
            const std::string l1 = many ? T("Conquistas desbloqueadas!", "Achievements unlocked!")
                                        : T("Troféu de ", "") + AchTierName(gAch.showing) + T(" desbloqueado!", " trophy unlocked!");
            const std::string l2 = many ? std::to_string(gAch.several) + T(" do seu progresso no jogo", " from your progress in the game") : AchName(gAch.showing);
            const float s1 = u * 0.62f, s2 = u * 0.95f, icon = u * 1.5f;
            const float tw = std::max(TextWidth(l1, s1), TextWidth(l2, s2));
            const float cw = std::min((float)w - u, tw + icon * 1.6f + u * 1.9f), ch = u * 2.9f;
            const float top = u * 0.6f + (gTurbo ? u * 1.6f : 0.f);   // below the fast-forward mark
            const Rect r { w - cw - u * 0.6f + (1 - ease) * (cw + u), top, cw, ch };
            const Color tier = many ? Color { 0.98f, 0.77f, 0.20f, 1 } : AchTierColor(gAch.showing);
            RoundRect({ r.x - 2, r.y - 2, r.w + 4, r.h + 4 }, u * 0.6f, WithAlpha(tier, 0.85f * k));
            RoundRect(r, u * 0.55f, WithAlpha(th.panel, 0.97f * k));
            const float em = ch - u * 0.9f;   // the emblem: as tall as the card allows
            if (many) Text(PadIcon(kIconTrophy), r.x + u * 0.5f, r.y + (ch - icon) / 2 - icon * 0.05f, icon, WithAlpha(th.text, k));
            else AchIconDraw(gAch.showing, false, r.x + u * 0.55f, r.y + (ch - em) / 2, em, k);
            const float tx = r.x + u * 0.5f + (many ? PadIconWidth(kIconTrophy, icon) : em + u * 0.25f) + u * 0.3f;
            Text(l1, tx, r.y + u * 0.45f, Fit(l1, s1, r.x + r.w - tx - u * 0.5f), WithAlpha(tier, k));
            Text(l2, tx, r.y + u * 1.25f, Fit(l2, s2, r.x + r.w - tx - u * 0.5f), WithAlpha(th.text, k));
        }
    }
    if (!gToast.empty() && SDL_GetTicks64() < gToastUntil)
    {
        float ts = u * 0.75f, tw = TextWidth(gToast, ts);
        Rect r { (w - tw) / 2 - u * 0.8f, h - u * 3.2f, tw + u * 1.6f, ts + u * 0.8f };
        RoundRect(r, u * 0.4f, WithAlpha(th.panel, 0.95f));
        Text(gToast, w / 2.f, r.y + u * 0.35f, ts, th.text, 1);
    }
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------
static void MenuKey(SDL_Scancode sc)
{
    auto items = VisibleItems(gMenu.page);
    if (gMenu.waitPad >= 0) { gMenu.waitPad = -1; return; }   // waiting for a controller button: a key cancels
    if (gMenu.waitKey >= 0)
    {
        if (sc != SDL_SCANCODE_ESCAPE) { gCfg.keys[gMenu.waitKey] = sc; ApplyLive(); }
        gMenu.waitKey = -1;
        return;
    }
    if (gMenu.waitHot >= 0)
    {
        // (Esc cancels; F1 stays the menu key)
        if (sc != SDL_SCANCODE_ESCAPE && sc != SDL_SCANCODE_F1)
        {
            for (int h = 0; h < H_Count; h++) if (h != gMenu.waitHot && gCfg.hot[h] == sc) gCfg.hot[h] = gCfg.hot[gMenu.waitHot];   // (swap)
            gCfg.hot[gMenu.waitHot] = sc;
            SaveConfig();
        }
        gMenu.waitHot = -1;
        return;
    }
    if (gMenu.popup >= 0 && gMenu.popup < (int)items.size() && !items[gMenu.popup].opts.empty())
    {
        MenuItem& pm = items[gMenu.popup];
        const int count = (int)pm.opts.size();
        switch (sc)
        {
        case SDL_SCANCODE_UP: gMenu.popSel = (gMenu.popSel - 1 + count) % count; break;
        case SDL_SCANCODE_DOWN: gMenu.popSel = (gMenu.popSel + 1) % count; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
            ApplyOpt(pm, gMenu.popSel);
            gMenu.popup = -1;
            break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F1: case SDL_SCANCODE_BACKSPACE: gMenu.popup = -1; break;
        default: break;
        }
        return;
    }
    gMenu.popup = -1;
    switch (sc)
    {
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F1: gMenu.open = false; break;
    case SDL_SCANCODE_UP: gMenu.sel = NextSelectable(items, gMenu.sel, -1); break;
    case SDL_SCANCODE_DOWN: gMenu.sel = NextSelectable(items, gMenu.sel, 1); break;
    case SDL_SCANCODE_LEFT: if (gMenu.sel < (int)items.size()) MenuChange(items[gMenu.sel], -1); break;
    case SDL_SCANCODE_RIGHT: if (gMenu.sel < (int)items.size()) MenuChange(items[gMenu.sel], 1); break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
        if (gMenu.sel < (int)items.size())
        {
            if (!items[gMenu.sel].opts.empty() && items[gMenu.sel].change) OpenPopup(gMenu.sel, items[gMenu.sel]);
            else MenuActivate(items[gMenu.sel]);
        }
        break;
    case SDL_SCANCODE_Q: case SDL_SCANCODE_PAGEUP: SetPage((gMenu.page - 1 + P_Count) % P_Count); break;
    case SDL_SCANCODE_E: case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_TAB: SetPage((gMenu.page + 1) % P_Count); break;
    default: break;
    }
}

static void MenuClick(float x, float y, int wheel)
{
    if (gMenu.popup >= 0)
    {
        auto items = VisibleItems(gMenu.page);
        if (gMenu.popup >= (int)items.size()) { gMenu.popup = -1; return; }
        if (wheel)
        {
            const int count = (int)items[gMenu.popup].opts.size();
            gMenu.popSel = std::clamp(gMenu.popSel - wheel, 0, std::max(0, count - 1));
            return;
        }
        for (int k = 0; k < (int)gMenu.popRects.size(); k++)
            if (gMenu.popRects[k].Contains(x, y)) { ApplyOpt(items[gMenu.popup], k); break; }
        gMenu.popup = -1;   // a click anywhere closes it (on a choice: after choosing it)
        return;
    }
    if (wheel)
    {
        auto items = VisibleItems(gMenu.page);
        for (int k = 0; k < std::abs(wheel); k++)
        {
            int next = NextSelectable(items, gMenu.sel, wheel > 0 ? -1 : 1);
            if ((wheel > 0) != (next < gMenu.sel)) break;   // no wrapping with the wheel
            gMenu.sel = next;
        }
        return;
    }
    if (gMenu.btnRects[0].Contains(x, y)) { gMenu.open = false; return; }   // Continue
    if (gMenu.btnRects[1].Contains(x, y)) { gRunning = false; return; }      // Quit game
    for (int p = 0; p < (int)gMenu.tabRects.size(); p++)
        if (gMenu.tabRects[p].Contains(x, y)) { SetPage(p); return; }
    auto items = VisibleItems(gMenu.page);
    for (int i = 0; i < (int)gMenu.itemRects.size() && i < (int)items.size(); i++)
        if (gMenu.itemRects[i].Contains(x, y))
        {
            gMenu.sel = i;
            if (items[i].header) { MenuActivate(items[i]); return; }   // fold / unfold
            if (gMenu.leftRects[i].Contains(x, y)) { MenuChange(items[i], -1); return; }
            if (gMenu.rightRects[i].Contains(x, y)) { MenuChange(items[i], 1); return; }
            if (items[i].change && gMenu.valueRects[i].Contains(x, y))
            {
                if (!items[i].opts.empty()) OpenPopup(i, items[i]);   // a list: the dropdown with its choices
                else MenuChange(items[i], 1);
                return;
            }
            if (items[i].activate) MenuActivate(items[i]);
            return;
        }
}

static void OpenMenu()
{
#ifdef PORTE_SEM_MENU
    return;   // no menu for now
#endif
    gMenu.open = true;
    gMenu.waitKey = -1;
    gMenu.waitHot = -1;
    gMenu.waitPad = -1;
    gMenu.popup = -1;
    gMenu.openT = NowSec();
    gMenu.selInit = false;
    InputCommon::GetKeyboard()->ReleaseAllKeys();
}

// game controller -> the keys bound to the 3DS buttons (so remapping covers both)
static SDL_GameController* gPad = nullptr;
static void StopPhoto();
// Modo foto with a controller: left stick moves, right stick looks, LT / RT down / up, LB / RB zoom, Y interface, B leaves
// (the screenshot is the controller button mapped to it? no: F12 / the keyboard shortcut)
static void PhotoPad(float dt, float speed, float turn, float mv[3])
{
    if (!gPad) return;
    auto ax = [](SDL_GameControllerAxis a) {
        const float v = SDL_GameControllerGetAxis(gPad, a) / 32767.f;
        return std::fabs(v) < 0.18f ? 0.f : v;
    };
    mv[0] += ax(SDL_CONTROLLER_AXIS_LEFTX) * speed;
    mv[2] += ax(SDL_CONTROLLER_AXIS_LEFTY) * speed;
    mv[1] += (ax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) - ax(SDL_CONTROLLER_AXIS_TRIGGERLEFT)) * speed;
    gPhoto.yaw -= ax(SDL_CONTROLLER_AXIS_RIGHTX) * turn * 1.5f;
    gPhoto.pitch = std::clamp(gPhoto.pitch - ax(SDL_CONTROLLER_AXIS_RIGHTY) * turn * 1.5f, -1.5f, 1.5f);
    if (SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) gPhoto.zoom = std::min(6.f, gPhoto.zoom * (1 + dt));
    if (SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) gPhoto.zoom = std::max(0.5f, gPhoto.zoom / (1 + dt));
    static bool yWas = false, bWas = false;
    const bool y = SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_Y), b = SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_B);
    if (y && !yWas) gPhoto.hideHud = !gPhoto.hideHud;
    if (b && !bWas) StopPhoto();
    yWas = y; bWas = b;
}
static bool gPadHeld[B_Count] = {};
static void PadSet(int b, bool down)
{
    if (gPadHeld[b] == down) return;
    gPadHeld[b] = down;
    if (gMenu.open && down) return;
    if (down) InputCommon::GetKeyboard()->PressKey(gCfg.keys[b]);
    else InputCommon::GetKeyboard()->ReleaseKey(gCfg.keys[b]);
}
// remapping: the controller button 'code' goes to the 3DS button waiting for it
// (a 3DS button that had it before gets the old one, so no two share a button)
static void PadAssign(int code)
{
    const int b = gMenu.waitPad;
    gMenu.waitPad = -1;
    if (b < 0 || b >= kPadBtns) return;
    for (int i = 0; i < B_Count; i++) PadSet(i, false);
    for (int i = 0; i < kPadBtns; i++)
        if (i != b && gCfg.pad[i] == code) gCfg.pad[i] = gCfg.pad[b];
    gCfg.pad[b] = code;
    SaveConfig();
}
static void PollPadSticks()
{
    if (!gPad) return;
    const float lx = SDL_GameControllerGetAxis(gPad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f;
    const float ly = SDL_GameControllerGetAxis(gPad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f;
    const float lt = SDL_GameControllerGetAxis(gPad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.f;
    const float rt = SDL_GameControllerGetAxis(gPad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.f;
    static bool ltWas = false, rtWas = false;   // triggers pressed in the last poll (remapping needs a new press)
    const bool ltNow = lt > 0.5f, rtNow = rt > 0.5f;
    const bool ltNew = ltNow && !ltWas, rtNew = rtNow && !rtWas;
    ltWas = ltNow;
    rtWas = rtNow;
    if (gMenu.open)
    {
        if (gMenu.waitPad >= 0 && (ltNew || rtNew)) PadAssign(ltNew ? kPadLT : kPadRT);
        return;
    }
    const float dz = gCfg.deadzone / 100.f;   // Controles > Zona morta
    PadSet(B_CLeft, lx < -dz);
    PadSet(B_CRight, lx > dz);
    PadSet(B_CUp, ly < -dz);
    PadSet(B_CDown, ly > dz);
    for (int b = 0; b < kPadBtns; b++)
        if (gCfg.pad[b] == kPadLT || gCfg.pad[b] == kPadRT) PadSet(b, gCfg.pad[b] == kPadLT ? ltNow : rtNow);
}
// controller button pressed / released in the game: every 3DS button on it
static void PadButton(int code, bool down)
{
    for (int b = 0; b < kPadBtns; b++)
        if (gCfg.pad[b] == code) PadSet(b, down);
}
static void PadMenuButton(int b)
{
    switch (b)
    {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: MenuKey(SDL_SCANCODE_UP); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: MenuKey(SDL_SCANCODE_DOWN); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: MenuKey(SDL_SCANCODE_LEFT); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: MenuKey(SDL_SCANCODE_RIGHT); break;
    case SDL_CONTROLLER_BUTTON_B: MenuKey(SDL_SCANCODE_RETURN); break;   // 3DS A position confirms
    case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_GUIDE: case SDL_CONTROLLER_BUTTON_RIGHTSTICK: gMenu.open = false; break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: MenuKey(SDL_SCANCODE_Q); break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: MenuKey(SDL_SCANCODE_E); break;
    default: break;
    }
}

// keys the game never sees
static bool HotKey(SDL_Scancode sc, bool repeat)
{
    using namespace Settings;
    // configurable shortcuts: the key pressed becomes the default key of its action (the cases below); a default key
    // whose action moved to another key goes to the game
    {
        bool mapped = false;
        for (int h = 0; h < H_Count; h++)
            if (gCfg.hot[h] == sc) { sc = (SDL_Scancode)kDefaultHot[h]; mapped = true; break; }
        if (!mapped)
            for (int h = 0; h < H_Count; h++)
                if (kDefaultHot[h] == sc) return false;
    }
    switch (sc)
    {
    case SDL_SCANCODE_F1:
        if (!repeat && (SDL_GetModState() & KMOD_CTRL))
        {
            // Ctrl+F1: try the achievement card (the next one at each press; nothing is unlocked)
            static int next = 0;
            std::lock_guard<std::recursive_mutex> lk(gUiMutex);
            gAch.queue.push_back(next);
            next = (next + 1) % kAchCount;
            return true;
        }
        if (!repeat) OpenMenu();
        return true;
    case SDL_SCANCODE_ESCAPE: if (!repeat) OpenMenu(); return true;
    case SDL_SCANCODE_F9: if (!repeat) { if (gPhoto.on) StopPhoto(); else StartPhoto(); } return true;   // (Modo foto; configurable)
    case SDL_SCANCODE_BACKSPACE: if (!PORTE_REWIND_HABILITADO) return false; RewindPress(repeat); return true;   // (Voltar no tempo; configurable)
    case SDL_SCANCODE_F5: if (!repeat) SaveState(gCfg.stateSlot); return true;
    case SDL_SCANCODE_F8: if (!repeat) LoadState(gCfg.stateSlot); return true;
    case SDL_SCANCODE_F6:
        if (!repeat && gCfg.dualWindow) Toast(T("Com duas janelas cada tela fica na sua", "With two windows each screen has its own"));
        else if (!repeat)
        {
            static const LayoutOption lo[] = { LayoutOption::Default, LayoutOption::SingleScreen, LayoutOption::LargeScreen,
                                               LayoutOption::SideScreen, LayoutOption::HybridScreen, LayoutOption::CustomLayout };
            int cur = 0;
            for (int i = 0; i < 6; i++) if (values.layout_option.GetValue() == lo[i]) cur = i;
            values.layout_option.SetValue(lo[(cur + 1) % 6]);
            ApplyLive();
        }
        return true;
    case SDL_SCANCODE_F7: if (!repeat) { values.swap_screen.SetValue(!values.swap_screen.GetValue()); ApplyLive(); } return true;
    case SDL_SCANCODE_F10: if (!repeat) { gCfg.showFps = !gCfg.showFps; SaveConfig(); } return true;
    case SDL_SCANCODE_F11: if (!repeat) { ToggleFullscreenFocused(); SaveConfig(); } return true;
    case SDL_SCANCODE_F12: if (!repeat) RequestShot(ShotPath()); return true;
    case SDL_SCANCODE_TAB:
        if (!repeat) { gTurbo = gCfg.turboToggle ? !gTurbo : true; ApplyTurbo(); }
        return true;
    case SDL_SCANCODE_EQUALS: case SDL_SCANCODE_KP_PLUS: case SDL_SCANCODE_MINUS: case SDL_SCANCODE_KP_MINUS:
    {
        float d = (sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_KP_PLUS) ? 0.05f : -0.05f;
        if (gMuteSaved >= 0) gMuteSaved = std::clamp(gMuteSaved + d, 0.f, 1.f);   // (muted: change the volume it goes back to)
        else values.volume.SetValue(std::clamp(values.volume.GetValue() + d, 0.f, 1.f));
        ApplyLive();
        Toast("Volume " + std::to_string((int)std::lround((gMuteSaved >= 0 ? gMuteSaved : values.volume.GetValue()) * 100)) + "%");
        return true;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// frames: the core's Vulkan renderer hands every finished frame to the presenter
// (vk_present_window.cpp, its own thread), which makes the in-between frames and shows
// them at the chosen rate. This side tells it what it needs (OnSwap: new picture?
// triangles?), builds the menu for it (BuildUi) and keeps the speed counters.
// ---------------------------------------------------------------------------
static double NowSec() { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }
static int MonitorHz()
{
    SDL_DisplayMode m;
    int idx = gWindow.win ? SDL_GetWindowDisplayIndex(gWindow.win) : 0;
    if (SDL_GetCurrentDisplayMode(std::max(0, idx), &m) == 0 && m.refresh_rate > 0) return m.refresh_rate;
    return 60;
}
static int FgRate() { return gCfg.fgRate > 0 ? gCfg.fgRate : MonitorHz(); }

// --- shared by the emulation (main) thread and the presenter thread ---
static std::atomic<int> gGenCount{0}, gPresents{0};
static DWORD gMainThreadId = 0;
static std::atomic<DWORD> gPresentThreadId{0};   // for the crash log
// main thread only
#ifdef PORTE_GAME_UM
static double gAccGameFrames = 0;   // (no rounding per frame: it distorted the sum)
#else
static int gAccGameFrames = 0;
#endif
static double gAccSpeed = 0, gAccTime = 0, gLastTickSec = 0;
static u64 gLastPubId = ~0ull;
static double gProfRun = 0, gProfWait = 0;   // seconds per stats period (main thread)
// testing aids
static Uint64 gStart = 0;
static int gShotAt = 0;
static std::atomic<bool> gShotDone{false};

// Main thread, once per emulated frame: the core's speed counters, added up per second.
static double gAccVbl = 0;
static void UpdateStats()
{
    auto& system = Core::System::GetInstance();
    const u64 id = system.GPU().Renderer().GetCurrentFrame();
    if (id == gLastPubId) return;
    gLastPubId = id;
    const double now = NowSec();
    const double dt = gLastTickSec > 0 ? now - gLastTickSec : 0.0;
    gLastTickSec = now;
    const auto st = system.GetAndResetPerfStats();
#ifdef PORTE_GAME_UM
    // right after a state load (or a long pause) the core's clock jumps: not a real reading.
    // Slow frames (loading, heavy scenes: more than 0.25 s apart) count too -- dropping them made
    // the counter show "0 FPS / 0%" exactly when the game was slow
    if (dt > 0 && dt < 5.0 && st.emulation_speed >= 0 && st.emulation_speed < 20)
    {
        gAccGameFrames += st.game_fps * dt;
#else
    // right after a state load (or a long pause) the core's clock jumps: not a real reading
    if (dt > 0 && dt < 0.25 && st.emulation_speed >= 0 && st.emulation_speed < 20)
    {
        gAccGameFrames += (int)std::lround(st.game_fps * dt);
#endif
        gAccSpeed += st.emulation_speed * dt;
        gAccTime += dt;
        gAccVbl += st.system_fps * dt;   // (diagnosis) emulated VBlanks
    }
}

// Emulation (main) thread, from the presenter's PresentWindow::Present: the core just drew
// a frame. Whether the game delivered a new picture in it comes from the core itself (the
// flag behind "skip duplicate frames"); the triangles drawn since the previous new picture
// belong to it.
// (22/11) the game's own button icons (A, B, X, Y, R on the touch screen menus) drawn in the style of the connected
// controller: bin/icones_controle/<xbox|ps>/<texture hash>_<controller button>.png (made by ferramentas/
// gerar_icones_controle.py). Each icon shows the controller button the 3DS button is MAPPED to (Controles). Only with a
// controller connected (Xbox / PlayStation); without one, or a Nintendo one, the 3DS icons stay
static void PadIconsUpdate()
{
    static const struct { const char* hash; int btn; } kIcons[] = {
        { "0E8403331BFE0087", B_A }, { "65C87B93AF6B164B", B_B }, { "B3F60FFA9BF73C9C", B_X },
        { "89AE7C33AE138E83", B_Y }, { "D0917714C6CBEFED", B_R },
        { "A0379692CF9CF7DD", B_A } };   // (the "A" balloon over doors / people)
    auto& sys = Core::System::GetInstance();
    if (!sys.IsPoweredOn()) return;
    static const int forced = getenv("PORTE_ICONES_TESTE") ? atoi(getenv("PORTE_ICONES_TESTE")) : 0;   // (test: no controller needed)
    const int style = forced ? forced : gPad ? PadStyle() : 0;
    const char* dir = style == 1 ? "xbox" : style == 2 ? "ps" : nullptr;
    std::string sig = std::to_string(style);
    for (const auto& ic : kIcons) sig += "," + std::to_string(gCfg.pad[ic.btn]);
    static std::string lastSig;
    static const void* lastMgr = nullptr;
    const void* mgr = &sys.CustomTexManager();
    if (sig == lastSig && mgr == lastMgr) return;
    lastSig = sig;
    lastMgr = mgr;
    std::vector<std::pair<u64, std::string>> list;
    if (dir)
    {
        static const std::wstring base = [] {
            wchar_t exe[MAX_PATH * 2] = L"";
            GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
            std::wstring d = exe;
            return d.substr(0, d.find_last_of(L"\/") + 1) + L"icones_controle\\";
        }();
        for (const auto& ic : kIcons)
        {
            const int code = gCfg.pad[ic.btn];
            if (code < 0) continue;
            const std::wstring f = base + Widen(dir) + L"\\" + Widen(ic.hash) + L"_" + std::to_wstring(code) + L".png";
            if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            list.push_back({ std::strtoull(ic.hash, nullptr, 16), Narrow(f) });
        }
    }
    fprintf(stderr, "ícones de controle: %s (%zu)\n", dir ? dir : "3DS", list.size());
    LOG_INFO(Frontend, "porte: ícones de controle: estilo {} controle {} tipo {} -> {} ({} figuras)", style, gPad ? "sim" : "não", gPadType.load(),
             dir ? dir : "3DS", list.size());
    sys.CustomTexManager().PorteSetIcons(std::move(list));
}

static Porte::FrameMeta OnSwap()
{
    Porte::FrameMeta m;
    PadIconsUpdate();   // (22/11) the game's button icons in the controller's style
    m.changed = Core::PerfStats::game_frames_updated;
    if (InterpRec::TakeExtraFlag()) m.changed = true;   // quadro do meio da interpolação: também é imagem nova
    // interpolação acima de 60: os quadros do meio vêm com hora marcada; enquanto ela está ativa, os quadros
    // normais do VBlank não são mostrados (os mesmos quadros já saem na sequência com hora)
    m.showAt = InterpRec::TakeShowAt();
    if (m.showAt > 0) m.changed = true;
    else if (InterpRec::SuppressNormal()) m.changed = false;
    m.time = NowSec();
#ifdef PORTE_SEM_GERADOR
    m.geo = nullptr;   // no frame generator: the triangles are not captured
    InterpRec::OnFrameEnd(m.changed);   // interpolação no jogo (etapa A: gravador)
    InterpRec::Install();
#else
    m.geo = GeoCap::OnFrameEnd(m.changed);
    GeoCap::Install();
#endif
    // the generator's screen mapping is the plain top screen: with stereoscopic 3D (two
    // pictures of it) or rotated screens it pauses and the game's own frames are shown
    if (Settings::values.render_3d.GetValue() != Settings::StereoRenderOption::Off || Settings::values.upright_screen.GetValue())
        m.geo = nullptr;
    const auto& L = gWindow.GetFramebufferLayout();
    m.top[0] = (float)L.top_screen.left;
    m.top[1] = (float)L.top_screen.top;
    m.top[2] = (float)L.top_screen.right;
    m.top[3] = (float)L.top_screen.bottom;
    return m;
}

// The core's frame limiter waits here (main thread) instead of sleeping by itself.
// (20/11 noite) getenv walks the whole environment block: the main loop asked for ~16 test variables on every turn
// (thousands of times a second) - 23% of the emulation thread's time, measured. Each call site now reads its
// variable once (a static of its own) - the values never change while the game runs
#define GETENV_ONCE(name) ([]() -> const char* { static const char* v = getenv(name); return v; }())

static void LimiterWait(std::chrono::steady_clock::time_point deadline)
{
    using clock = std::chrono::steady_clock;
    UpdateStats();
    struct WaitTimer { double t0; ~WaitTimer() { gProfWait += NowSec() - t0; } } waitTimer{ NowSec() };
    for (;;)
    {
        const auto left = deadline - clock::now();
        if (left <= std::chrono::microseconds(0)) break;
        if (left > std::chrono::microseconds(1500)) std::this_thread::sleep_for(left - std::chrono::microseconds(1000));
        else std::this_thread::yield();
    }
}

// Presenter thread: the menu / overlay of one frame.
static void BuildUi(int w, int h, std::vector<Porte::UiVert>& out)
{
    gPresentThreadId = GetCurrentThreadId();
    std::lock_guard<std::recursive_mutex> lk(gUiMutex);
    BeginUi(w, h, out);
    if (gMenu.open) DrawMenu(w, h);
    DrawOverlay(w, h);
    gUi.out = nullptr;
}
static void OnPresented(bool generated)
{
    PorteLog::Present(generated);
    gPresents++;
    if (generated) gGenCount++;
}
// testing aids: PORTE_SHOTEVERY=N saves the window every N seconds (shot_K.raw),
// PORTE_SHOTAT=N once (shot.raw); raw = int w, int h, BGRA rows from the top
static std::string ShotWanted()
{
    static const int shotEvery = getenv("PORTE_SHOTEVERY") ? atoi(getenv("PORTE_SHOTEVERY")) : 0;
    static int shotK = 0;
    const Uint64 el = SDL_GetTicks64() - gStart;
    if (shotEvery && el >= (Uint64)(shotK + 1) * shotEvery * 1000 && shotK < 60)
    {
        shotK++;
        return gDataDir + "shot_" + std::to_string(shotK) + ".raw";
    }
    // (testing aid) PORTE_SHOTMS=<start s>,<every ms>,<count>: a burst of window pictures (burst_K.raw)
    static double bStart = -1, bEvery = 0; static int bLeft = 0, bK = 0;
    if (bStart < 0) { bStart = 0; if (const char* b = getenv("PORTE_SHOTMS")) { sscanf(b, "%lf,%lf,%d", &bStart, &bEvery, &bLeft); } }
    if (bLeft > 0 && el >= (Uint64)(bStart * 1000 + bK * bEvery)) { bLeft--; bK++; return gDataDir + "burst_" + std::to_string(bK) + ".raw"; }
    if (gShotAt && !gShotDone && el >= (Uint64)gShotAt * 1000)
    {
        gShotDone = true;
        return gDataDir + "shot.raw";
    }
    return {};
}
// what the presenter reads every frame
static void SyncPresenter()
{
#ifdef PORTE_SEM_GERADOR
    Porte::gGenOn = false;
#else
    Porte::gGenOn = gCfg.frameGen && !gTurbo;
#endif
    Porte::gRate = FgRate();
    Porte::gVsync = gCfg.vsync;
    Porte::gMenuOpen = gMenu.open;
    Porte::gHold = (gMenu.open && gCfg.pauseInMenu) || gUnfocusedPause;
    PorteLog::holdFlag = Porte::gHold || gPhoto.on;
}

// ---------------------------------------------------------------------------
static std::string PickRom()
{
    wchar_t buf[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = EN() ? L"3DS ROM (*.3ds;*.cci;*.cxi;*.cia)\0*.3ds;*.cci;*.cxi;*.cia\0All files\0*.*\0"
                           : L"ROM de 3DS (*.3ds;*.cci;*.cxi;*.cia)\0*.3ds;*.cci;*.cxi;*.cia\0Todos os arquivos\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrTitle = EN() ? L"Choose your Pokémon " PORTE_GAME_WLETTER L" ROM (decrypted)" : L"Escolha a sua ROM de Pokémon " PORTE_GAME_WLETTER L" (descriptografada)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? Narrow(buf) : std::string();
}

static void Error(const std::string& msg)
{
    fprintf(stderr, "ERRO: %s\n", msg.c_str());
    if (!getenv("PORTE_HIDDEN")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kGameName, msg.c_str(), nullptr);
}

// the ROM path is remembered; Shift held at start asks again
std::string PorteDataDir() { return gDataDir; }   // (interp_rec.cpp)
static std::string RomPathFile() { return gDataDir + "rom.txt"; }

// When the program crashes, write where (module offset + thread) to stderr, so a
// crash in a background test can be traced to a function (nm on the .exe).
static LONG WINAPI CrashLog(EXCEPTION_POINTERS* e)
{
    const auto at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &mod))
        GetModuleFileNameW(mod, name, MAX_PATH);
    const DWORD tid = GetCurrentThreadId();
    const char* who = tid == gMainThreadId ? "principal" : tid == gPresentThreadId ? "apresentacao" : "outra";
    fprintf(stderr, "FALHA: codigo %08lX em %ls +0x%llx (thread %s)\n", e->ExceptionRecord->ExceptionCode, name,
            (unsigned long long)(at - (uintptr_t)mod), who);
    static int stacks = 0;
    if (PorteLog::on && stacks++ < 4) PorteLog::CrashStack(e);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
namespace PorteLog { void ReplayHook(float t, double ms, int mixed, int jumped) { Replay(t, ms, mixed, jumped); } }
static LONG WINAPI CrashFinal(EXCEPTION_POINTERS* e)
{
    CrashLog(e);
    PorteLog::Line("QUEDA: o jogo vai fechar (código %08lX)", e->ExceptionRecord->ExceptionCode);
    PorteLog::CrashDump(e);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv)
{
    SetUnhandledExceptionFilter(CrashFinal);
    gMainThreadId = GetCurrentThreadId();
    AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS* e) -> LONG {
        if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) CrashLog(e);
        return EXCEPTION_CONTINUE_SEARCH; });
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool hidden = getenv("PORTE_HIDDEN") != nullptr;   // background tests
    const int testSecs = getenv("PORTE_SECONDS") ? atoi(getenv("PORTE_SECONDS")) : 0;

    gDataDir = DataDir();
    FileUtil::SetUserPath(gDataDir);
    Common::Log::Initialize();
    Common::Log::Start();
    LoadConfig();
    // (22/11) Sistema > Log avançado (or PORTE_LOG=1): stderr and everything else to <dados>\log\porte_*.txt
    if ((gCfg.logAdv && !hidden) || getenv("PORTE_LOG"))
        if (PorteLog::Start(Widen(gDataDir))) PorteLog::Header(kVersion, kGameName, Widen(ConfigPath()));
    // (22/11) Discord: background tests only with PORTE_DISCORD_TESTE=<id>
    if (const char* dt = getenv("PORTE_DISCORD_TESTE")) DiscordRpc::Start(dt, kGameName);
    else if (gCfg.discord && !hidden) DiscordRpc::Start(gCfg.discordId.empty() ? DiscordRpc::kDiscordIdPadrao : gCfg.discordId, kGameName);
    if (gCfg.lang < 0) gCfg.lang = 1;   // (23/11) English by default, whatever the Windows language (Português in the menu)
    // (20/11) one copy of the game at a time: two would share the same save folder (a save could be overwritten by
    // the other). Background tests run several at once and are allowed
    if (!hidden || getenv("PORTE_TESTE_TRAVA"))
    {
        CreateMutexW(nullptr, TRUE, L"Local\\PokemonAlolaPC_jogo");
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            Error(T("O jogo já está aberto (veja na barra de tarefas). Use a janela que já está aberta.",
                    "The game is already running (check the taskbar). Use the window that is already open."));
            return 4;
        }
    }
    if (getenv("PORTE_SCALE")) Settings::values.resolution_factor.SetValue(atoi(getenv("PORTE_SCALE")));
    // (testing aid, 22/11) PORTE_API=software|opengl: another renderer, to compare the picture (e.g. the gray skin)
    if (const char* api = getenv("PORTE_API"))
    {
        if (!strcmp(api, "software")) Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::Software);
        else if (!strcmp(api, "opengl")) Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::OpenGL);
    }
    if (const char* hr = getenv("PORTE_HORA"))   // (testing aid, 22/11) fixed 3DS clock (epoch seconds) - same time as a reference
    {
        Settings::values.init_clock.SetValue(Settings::InitClock::FixedTime);
        Settings::values.init_time.SetValue(strtoull(hr, nullptr, 10));
    }
    if (getenv("PORTE_DUMPTEX")) Settings::values.dump_textures.SetValue(true);   // (testing aid, 22/11) textures -> <dados>/dump/textures
    if (const char* sp = getenv("PORTE_SPIRV")) Settings::values.spirv_shader_gen.SetValue(atoi(sp) != 0);   // (testing aid: 0 = GLSL path)
    if (hidden && !getenv("PORTE_NOMUTE")) Settings::values.volume.SetValue(0.f);   // background tests are silent
    // the frame generator needs the game's finished triangles: the core's CPU vertex path hands them over
#ifdef PORTE_SEM_GERADOR
    // without the frame generator the shaders can run on the GPU (on the CPU they were most of the
    // emulation time); PORTE_SWSHADER=1 puts them back on the CPU
    Settings::values.use_hw_shader.SetValue(getenv("PORTE_SWSHADER") == nullptr);
#else
    Settings::values.use_hw_shader.SetValue(getenv("PORTE_HWSHADER") != nullptr);   // (testing aid: compare with the GPU path)
#endif
#ifndef PORTE_SEM_MENU
    if (getenv("PORTE_MENU")) { gMenu.open = true; gMenu.page = atoi(getenv("PORTE_MENU")); }   // testing aid
#endif
    if (getenv("PORTE_MENUSEL")) gMenu.sel = atoi(getenv("PORTE_MENUSEL"));                     // testing aid

    // the ROM / cxi given on the command line, in UTF-8 like every other path here: the narrow argv
    // comes in the ANSI code page, and an accent in the path (a user named "João", a folder "Pokémon")
    // turned into bytes no file has -> "Não consegui carregar a ROM (erro 2)" on other people's PCs
    std::string rom;
    {
        int n = 0;
        if (LPWSTR* wa = CommandLineToArgvW(GetCommandLineW(), &n))
        {
            if (n > 1) rom = Narrow(wa[1]);
            LocalFree(wa);
        }
        else if (argc > 1) rom = argv[1];
    }
#ifdef PORTE_GAME_UM
    // installed by the installer: the game next to the exe (jogo\<cxi>), no ROM needed.
    // Each installed game has its own "Jogar Pokemon <jogo>.bat" that passes its cxi; opened
    // without one, the first installed game of jogos.h (Ultra Moon, Ultra Sun, Sun, Moon) is used.
    if (rom.empty())
    {
        wchar_t exe[MAX_PATH * 2] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        for (const Jogos::Jogo& j : Jogos::kJogos)
        {
            const std::string installed = Narrow(dir + L"jogo\\" + j.cxi);
            if (FileUtil::Exists(installed)) { rom = installed; break; }
        }
    }
#endif
    if (rom.empty())
    {
        std::ifstream f(P(RomPathFile()));
        std::getline(f, rom);
        // (background tests never ask: the person may be typing with Shift held)
        if ((!rom.empty() && !FileUtil::Exists(rom)) || (!hidden && (GetAsyncKeyState(VK_SHIFT) & 0x8000))) rom.clear();
    }
    if (rom.empty() && !hidden)
    {
        rom = PickRom();
        if (!rom.empty()) { std::ofstream f(P(RomPathFile())); f << rom; }
    }
    if (rom.empty()) return 2;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) { Error(std::string("SDL: ") + SDL_GetError()); return 1; }
    for (const auto& m : Service::service_module_map) Settings::values.lle_modules.emplace(m.name, false);
    InputCommon::Init();   // after SDL_Init: it then leaves the event loop to us
    ApplyKeys();
    ApplyBgColor();

    Settings::values.use_vsync.SetValue(gCfg.vsync);
    if (!gWindow.Create(gCfg.winW, gCfg.winH, hidden))
    {
        Error(T("Não consegui criar a janela: ", "Could not create the window: ") + SDL_GetError());
        return 1;
    }
    gHiddenRun = hidden;
    if (getenv("PORTE_DUAL")) gCfg.dualWindow = atoi(getenv("PORTE_DUAL")) != 0;   // testing aid
    if (!CreateWindow2())
    {
        Error(T("Não consegui criar a janela: ", "Could not create the window: ") + SDL_GetError());
        return 1;
    }
    SyncDual();
    InitUi();
    AchLoadPics();
    if (getenv("PORTE_FRAMEGEN")) gCfg.frameGen = atoi(getenv("PORTE_FRAMEGEN")) != 0;   // testing aid
    if (getenv("PORTE_RATE")) gCfg.fgRate = atoi(getenv("PORTE_RATE"));                   // testing aid
    Porte::gHooks.onSwap = OnSwap;
    Porte::gHooks.buildUi = BuildUi;
    Porte::gHooks.onPresented = OnPresented;
    Porte::gHooks.shotWanted = ShotWanted;
    SyncPresenter();
    if (gCfg.fullscreen && !hidden) SetFullscreen(true);

    auto& system = Core::System::GetInstance();
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

#ifdef PORTE_RECOMP_ON
    {
        // MotoResenhaZ (fase B): with the native code next to the exe there is no JIT -- Azahar's
        // interpreter only runs what is not native. PORTE_MOTOR=jit: the hybrid of before
        wchar_t exe[MAX_PATH * 2] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        const char* motor = getenv("PORTE_MOTOR");
        const char* rec = getenv("PORTE_RECOMP");
        // which game (read from the ROM's header): each one has its own native code
        if (const Jogos::Jogo* j = Jogos::Por(Jogos::TitleDoArquivo(Widen(rom)))) Jogos::gAtual = j;
        const bool nativeDll = GetFileAttributesW((dir + Jogos::gAtual->dll).c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool useJit = !nativeDll || (rec && atoi(rec) == 0) || (motor && strcmp(motor, "jit") == 0);
        Settings::values.use_cpu_jit.SetValue(useJit);
        fprintf(stderr, "motor: %s\n", useJit ? "JIT (híbrido)" : "MotoResenhaZ (nativo + interpretador, sem JIT)");
    }
#endif
    ApplyInterp();   // the menu's interpolation (Gráficos > Fluidez), before the first renderer
    if (getenv("PORTE_16X9")) gCfg.widescreen = atoi(getenv("PORTE_16X9")) != 0;   // (testing aid)
    ApplyWide();
    const auto result = system.Load(gWindow, rom, &gWindow2);
    if (result != Core::System::ResultStatus::Success)
    {
        if (result == Core::System::ResultStatus::ErrorLoader_ErrorEncrypted)
            Error(T("Essa ROM está criptografada. Use a sua cópia descriptografada (extraída do seu console).",
                    "This ROM is encrypted. Use your decrypted copy (dumped from your console)."));
        else if (result == Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat)
            Error(T("Formato de ROM não suportado.", "ROM format not supported."));
        else
            Error(T("Não consegui carregar a ROM (erro ", "Could not load the ROM (error ") + std::to_string((int)result) + ").");
        DeleteFileW(Widen(RomPathFile()).c_str());   // ask again next time
        return 3;
    }
    system.GetAppLoader().ReadProgramId(gProgramId);
    fprintf(stderr, "program_id=%016llX\n", (unsigned long long)gProgramId);
#ifdef PORTE_GAME_UM
    // the four Alola games: the window says which one is running
    if (const Jogos::Jogo* j = Jogos::Por(gProgramId))
    {
        Jogos::gAtual = j;
        static const std::string name = std::string("Pokémon ") + j->nome;
        kGameName = name.c_str();
        SDL_SetWindowTitle(gWindow.win, kGameName);
        if (gCfg.saveBackup && (!getenv("PORTE_HIDDEN") || getenv("PORTE_AUTOLOAD_TESTE"))) BackupGameSave();   // Saves > Backup automático
        fprintf(stderr, "jogo: %s\n", kGameName);
    }
    else
        Toast(T("Aviso: essa ROM não parece ser Pokémon Sun, Moon, Ultra Sun ou Ultra Moon",
                "Warning: this ROM does not look like Pokémon Sun, Moon, Ultra Sun or Ultra Moon"));
#else
    if ((gProgramId & 0xFFFFFFFF) != kGameTitleLow)
        Toast(T("Aviso: essa ROM não parece ser Pokémon " PORTE_GAME_LETTER, "Warning: this ROM does not look like Pokémon " PORTE_GAME_LETTER));
#endif
    system.GPU().ApplyPerProgramSettings(gProgramId);
    ApplyLive();
#ifdef PORTE_RECOMP_ON
    {
        wchar_t exe[MAX_PATH * 2] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        if (Recomp::Start(system, dir)) fprintf(stderr, "código nativo ligado\n");
    }
#endif

    gShotAt = getenv("PORTE_SHOTAT") ? atoi(getenv("PORTE_SHOTAT")) : 0;
    const Uint64 start = SDL_GetTicks64();
    gStart = start;
    RewindStart(system);
    gSessionStart = start;
    Uint64 lastStats = start;
    timeBeginPeriod(1);   // 1 ms sleeps, for in-between frames every few ms
    system.frame_limiter.SetWaitCallback(LimiterWait);
    while (gRunning)
    {
        std::unique_lock<std::recursive_mutex> uiLock(gUiMutex);
        // (22/11) a controller that never sent its "added" event to this loop (already plugged in / taken by something
        // else) stayed unopened: no controller as far as the button icons knew. Looked for every half second
        {
            static Uint64 lastPadScan = 0;
            const Uint64 nowPad = SDL_GetTicks64();
            if (!gPad && nowPad - lastPadScan > 500)
            {
                lastPadScan = nowPad;
                for (int i = 0; i < SDL_NumJoysticks(); i++)
                    if (SDL_IsGameController(i) && (gPad = SDL_GameControllerOpen(i)) != nullptr)
                    {
                        gPadType = (int)SDL_GameControllerGetType(gPad);
                        LOG_INFO(Frontend, "porte: controle aberto pela busca: {} (tipo {})", SDL_GameControllerName(gPad) ? SDL_GameControllerName(gPad) : "?", gPadType.load());
                        break;
                    }
            }
        }
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (PorteLog::on)   // (log avançado: keys, controllers, window)
            {
                if (e.type == SDL_KEYDOWN && !e.key.repeat) PorteLog::Line("tecla: %s%s", SDL_GetScancodeName(e.key.keysym.scancode), gMenu.open ? " (menu)" : "");
                else if (e.type == SDL_CONTROLLERDEVICEADDED) PorteLog::Line("controle conectado: %s", SDL_GameControllerNameForIndex(e.cdevice.which) ? SDL_GameControllerNameForIndex(e.cdevice.which) : "?");
                else if (e.type == SDL_CONTROLLERDEVICEREMOVED) PorteLog::Line("controle desconectado");
                else if (e.type == SDL_CONTROLLERBUTTONDOWN) PorteLog::Line("controle: %s", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button));
                else if (e.type == SDL_RENDER_DEVICE_RESET) PorteLog::Line("AVISO: dispositivo de vídeo reiniciado");
                else if (e.type == SDL_WINDOWEVENT)
                {
                    static const char* wn[] = { "?", "mostrada", "escondida", "exposta", "movida", "redimensionada", "tamanho mudou", "minimizada",
                                                "maximizada", "restaurada", "mouse entrou", "mouse saiu", "ganhou foco", "perdeu foco", "fechar", "foco", "teste", "monitor" };
                    const int we = e.window.event;
                    if (we != SDL_WINDOWEVENT_EXPOSED && we != SDL_WINDOWEVENT_ENTER && we != SDL_WINDOWEVENT_LEAVE && we != SDL_WINDOWEVENT_MOVED)
                        PorteLog::Line("janela: %s %d %d", we < 18 ? wn[we] : "?", e.window.data1, e.window.data2);
                }
            }
            if (!gMenu.open && LayoutEditEvent(e)) continue;   // (Personalizar posição das telas)
            if (!gMenu.open && PhotoEvent(e)) continue;   // (modo foto: its keys and mouse)
            // Controles > Touchpad como toque: the controller's touchpad (0..1 on both axes) is the bottom screen
            if ((e.type == SDL_CONTROLLERTOUCHPADDOWN || e.type == SDL_CONTROLLERTOUCHPADMOTION || e.type == SDL_CONTROLLERTOUCHPADUP) && gCfg.padTouch && !gMenu.open)
            {
                if (e.ctouchpad.finger == 0)
                {
                    const auto& r = gWindow.GetFramebufferLayout().bottom_screen;
                    const unsigned fx = r.left + (unsigned)(std::clamp(e.ctouchpad.x, 0.f, 1.f) * (r.right - r.left - 1));
                    const unsigned fy = r.top + (unsigned)(std::clamp(e.ctouchpad.y, 0.f, 1.f) * (r.bottom - r.top - 1));
                    if (e.type == SDL_CONTROLLERTOUCHPADDOWN) gWindow.TouchPressed(fx, fy);
                    else if (e.type == SDL_CONTROLLERTOUCHPADMOTION) gWindow.TouchMoved(fx, fy);
                    else gWindow.TouchReleased();
                }
                continue;
            }
            switch (e.type)
            {
            case SDL_QUIT: gRunning = false; break;
            case SDL_WINDOWEVENT:
                // the X of the window
                if (e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == SDL_GetWindowID(gWindow.win))
                    gRunning = false;
                // closing the second window turns two-window mode off
                else if (e.window.event == SDL_WINDOWEVENT_CLOSE && gWindow2.win && e.window.windowID == SDL_GetWindowID(gWindow2.win))
                {
                    gCfg.dualWindow = false;
                    ApplyLive();
                }
                else if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && gWindow2.win && e.window.windowID == SDL_GetWindowID(gWindow2.win))
                    gWindow2.Resized();
                else if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                {
                    gWindow.Resized();
                    if (!gCfg.fullscreen) SDL_GetWindowSize(gWindow.win, &gCfg.winW, &gCfg.winH);
                }
                else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                {
                    InputCommon::GetKeyboard()->ReleaseAllKeys();
                    if (gTurbo && !gCfg.turboToggle) { gTurbo = false; ApplyTurbo(); }
                }
                break;
            case SDL_KEYDOWN:
                if (gMenu.open) MenuKey(e.key.keysym.scancode);
                else if (!HotKey(e.key.keysym.scancode, e.key.repeat) && !e.key.repeat)
                    InputCommon::GetKeyboard()->PressKey(e.key.keysym.scancode);
                break;
            case SDL_KEYUP:
                if (e.key.keysym.scancode == gCfg.hot[H_Turbo] && !gCfg.turboToggle) { gTurbo = false; ApplyTurbo(); }
                InputCommon::GetKeyboard()->ReleaseKey(e.key.keysym.scancode);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEWHEEL:
            {
                float x = 0, y = 0;
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                const Uint32 wid = e.type == SDL_MOUSEMOTION ? e.motion.windowID : e.type == SDL_MOUSEWHEEL ? e.wheel.windowID : e.button.windowID;
                if (Window2Shown() && wid == SDL_GetWindowID(gWindow2.win))
                {
                    gWindow2.ToDrawable(mx, my, x, y);
                    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) OpenMenu();
                    else if (gMenu.open) break;
                    else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) gWindow2.TouchPressed((unsigned)x, (unsigned)y);
                    else if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK)) gWindow2.TouchMoved((unsigned)x, (unsigned)y);
                    else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) gWindow2.TouchReleased();
                    break;
                }
                gWindow.ToDrawable(mx, my, x, y);
                if (gMenu.open)
                {
                    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) MenuClick(x, y, 0);
                    else if (e.type == SDL_MOUSEWHEEL) MenuClick(x, y, e.wheel.y);
                    else if (e.type == SDL_MOUSEMOTION)
                    {
                        {
                            std::lock_guard<std::recursive_mutex> lk(gUiMutex);
                            gMenu.mx = x;
                            gMenu.my = y;
                        }
                        gMenu.hover = -1;
                        for (int i = 0; i < (int)gMenu.itemRects.size(); i++) if (gMenu.itemRects[i].Contains(x, y)) gMenu.hover = i;
                    }
                    break;
                }
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) gWindow.TouchPressed((unsigned)x, (unsigned)y);
                else if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK)) gWindow.TouchMoved((unsigned)x, (unsigned)y);
                else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) gWindow.TouchReleased();
                else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) OpenMenu();
                break;
            }
            case SDL_CONTROLLERDEVICEADDED:
                if (!gPad)
                {
                    gPad = SDL_GameControllerOpen(e.cdevice.which);
                    if (gPad) gPadType = (int)SDL_GameControllerGetType(gPad);
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (gPad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gPad)) == e.cdevice.which)
                {
                    SDL_GameControllerClose(gPad);
                    gPad = nullptr;
                    gPadType = (int)SDL_CONTROLLER_TYPE_UNKNOWN;
                    for (int b = 0; b < B_Count; b++) PadSet(b, false);
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (gMenu.open && gMenu.waitPad >= 0)
                {
                    if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) gMenu.waitPad = -1;   // cancels
                    else PadAssign(e.cbutton.button);
                }
                else if (gMenu.open) PadMenuButton(e.cbutton.button);
                else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE || e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) OpenMenu();
                else PadButton(e.cbutton.button, true);
                break;
            case SDL_CONTROLLERBUTTONUP:
                PadButton(e.cbutton.button, false);
                break;
            }
        }
        PollPadSticks();
        AchTick(system);
        RewindTick(system, !gPhoto.on && !(gMenu.open && gCfg.pauseInMenu) && !gUnfocusedPause);
        {
            // scene detector: in a battle while the Battle module is loaded (the same in the four games)
            const bool battle = system.IsPoweredOn() && Recomp::ModuleLoaded("Battle");
            if (battle != gInBattle)
            {
                gInBattle = battle;
                if (battle)
                {
                    if (gBattles < 0) StatsLoad();
                    gBattles++;
                    StatsSave();
                }
                fprintf(stderr, "cena: %s (batalhas contadas: %lld)\n", battle ? "batalha" : "fora de batalha", gBattles);
                ApplyTurbo();   // (automatic / only-in-battle fast-forward)
                // Tela > Tela dinâmica: the battle's layout, and the player's back afterwards (never saved: gLayoutKeep)
                if (gCfg.dynScreen > 0 && !gCfg.dualWindow && !gPhoto.on)
                {
                    auto& lo = Settings::values.layout_option;
                    if (battle)
                    {
                        if (gLayoutKeep < 0) gLayoutKeep = (int)lo.GetValue();
                        lo.SetValue(gCfg.dynScreen == 1 ? Settings::LayoutOption::LargeScreen : Settings::LayoutOption::SingleScreen);
                    }
                    else if (gLayoutKeep >= 0)
                    {
                        lo.SetValue((Settings::LayoutOption)gLayoutKeep);
                        gLayoutKeep = -1;
                    }
                    ApplyLive();
                }
            }
        }
        // Sistema > Continuar de onde parou: the "automático" state (slot 11) once the game has started (6 s)
        if (gCfg.autoLoadStart && (!gHiddenRun || getenv("PORTE_AUTOLOAD_TESTE")))
        {
            static bool autoDone = false;
            if (!autoDone && SDL_GetTicks64() - gStart > 6000)
            {
                autoDone = true;
                if (Core::GetSaveStateInfo(gProgramId, 0, 11).time != 0)
                {
                    system.SendSignal(Core::System::Signal::Load, 11);
                    Toast(T("Continuando de onde você parou", "Continuing where you left off"));
                    fprintf(stderr, "estado automático carregado\n");
                }
            }
        }
#ifdef PORTE_RECOMP_ON
        Recomp::Tick(system);
#endif
        // testing aids for scripted play (background runs):
        //   PORTE_LOADSTATE=N  load state N after PORTE_LOADAT seconds (default 8: the game must be running)
        //   PORTE_PATH="U2000,A100,W500"  keys after PORTE_PATHAT seconds (default 3):
        //     U D L R A B X Y S(tart) E (select) + milliseconds held; W = wait;
        //     lower case u d l r = Circle Pad
        //   PORTE_SAVEAT=N  save state PORTE_SAVESLOT (default 9) at N seconds
        //   PORTE_FAST=1    no speed limit
        {
            const Uint64 el = SDL_GetTicks64() - start;
            static bool loaded = false, saved = false, fast = false;
            if (!fast && GETENV_ONCE("PORTE_FAST")) { fast = true; Settings::values.frame_limit.SetValue(0); }
            if (const char* ls = GETENV_ONCE("PORTE_LOADSTATE"); ls && !loaded && el >= (Uint64)(GETENV_ONCE("PORTE_LOADAT") ? atof(GETENV_ONCE("PORTE_LOADAT")) * 1000 : 8000))
            {
                loaded = true;
                system.SendSignal(Core::System::Signal::Load, (u32)atoi(ls));
                fprintf(stderr, "teste: carregar estado %s\n", ls);
            }
            if (const char* sa = GETENV_ONCE("PORTE_SAVEAT"); sa && !saved && el >= (Uint64)atoi(sa) * 1000)
            {
                saved = true;
                const int slot = GETENV_ONCE("PORTE_SAVESLOT") ? atoi(GETENV_ONCE("PORTE_SAVESLOT")) : 9;
                system.SendSignal(Core::System::Signal::Save, (u32)slot);
                fprintf(stderr, "teste: salvar estado %d\n", slot);
            }
            {
                static std::vector<double> fsAt;
                static size_t fsN = 0;
                static bool fsRead = false;
                if (!fsRead) { fsRead = true; if (const char* ft = getenv("PORTE_FS_TOGGLE")) for (const char* q = ft; *q;) { fsAt.push_back(atof(q)); while (*q && *q != ',') q++; if (*q) q++; } }
                if (fsN < fsAt.size() && el >= (Uint64)(fsAt[fsN] * 1000)) { fsN++; std::lock_guard<std::recursive_mutex> lk(gUiMutex); SetFullscreen(!gCfg.fullscreen); }
            }
            {   // testing aid: PORTE_POKE="08066628=1.7777778,..." writes those floats into the game's memory every pass
                static std::vector<std::pair<u32, float>> pokes;
                static bool pokeRead = false;
                if (!pokeRead)
                {
                    pokeRead = true;
                    if (const char* pk = getenv("PORTE_POKE"))
                        for (const char* q = pk; *q;)
                        {
                            char* e = nullptr;
                            const u32 a = (u32)strtoul(q, &e, 16);
                            if (!e || *e != '=') break;
                            pokes.push_back({ a, (float)atof(e + 1) });
                            while (*q && *q != ',') q++;
                            if (*q) q++;
                        }
                }
                if (!pokes.empty() && system.IsPoweredOn())
                    if (auto proc = system.Kernel().GetCurrentProcess())
                        for (auto [a, v] : pokes)
                            if (system.Memory().IsValidVirtualAddress(*proc, a)) { u32 bits; memcpy(&bits, &v, 4); system.Memory().Write32(a, bits); }
            }
            // PORTE_MENUOPENAT=s / PORTE_MENUTABAT=s: open the menu / go to the next tab at s seconds (animation test)
            static bool menuOpened = false, menuTabbed = false;
            if (const char* mo = GETENV_ONCE("PORTE_MENUOPENAT"); mo && !menuOpened && el >= (Uint64)(atof(mo) * 1000)) { menuOpened = true; std::lock_guard<std::recursive_mutex> lk(gUiMutex); OpenMenu(); }
            if (const char* mt = GETENV_ONCE("PORTE_MENUTABAT"); mt && !menuTabbed && el >= (Uint64)(atof(mt) * 1000)) { menuTabbed = true; std::lock_guard<std::recursive_mutex> lk(gUiMutex); SetPage((gMenu.page + 1) % P_Count); }
            // PORTE_FOTO_TESTE="t,yaw,pitch,x,y,z,zoom,hud": the photo mode at t seconds with that camera (hud 1 = shown)
            {
                static bool photoDone = false;
                if (const char* ft = GETENV_ONCE("PORTE_FOTO_TESTE"); ft && !photoDone && el >= (Uint64)(atof(ft) * 1000))
                {
                    photoDone = true;
                    float v[8] = { 0, 0, 0, 0, 0, 0, 1, 0 };
                    sscanf(ft, "%f,%f,%f,%f,%f,%f,%f,%f", &v[7], &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
                    std::lock_guard<std::recursive_mutex> lk(gUiMutex);
                    StartPhoto();
                    gPhoto.yaw = v[0]; gPhoto.pitch = v[1]; gPhoto.pos[0] = v[2]; gPhoto.pos[1] = v[3]; gPhoto.pos[2] = v[4];
                    gPhoto.zoom = v[5] > 0 ? v[5] : 1; gPhoto.hideHud = v[6] == 0;
                    fprintf(stderr, "teste: modo foto yaw %.2f pitch %.2f pos %.0f %.0f %.0f zoom %.2f\n", v[0], v[1], v[2], v[3], v[4], gPhoto.zoom);
                }
            }
            // PORTE_MOD_TESTE=name: turns that library mod on (or "-" = none) at 3 s (the mods manager test)
            {
                static bool modDone = false;
                if (const char* mt = GETENV_ONCE("PORTE_MOD_TESTE"); mt && !modDone && el >= 3000 && gProgramId)
                {
                    modDone = true;
                    ModSet(strcmp(mt, "-") == 0 ? std::string() : std::string(mt));
                }
            }
            // PORTE_TELAS_TESTE="t,topX,topY,topW,botX,botY,botW,opacity": the custom layout at t seconds (edit mode + save)
            {
                static bool telasDone = false;
                if (const char* tt = GETENV_ONCE("PORTE_TELAS_TESTE"); tt && !telasDone && el >= (Uint64)(atof(tt) * 1000))
                {
                    telasDone = true;
                    float v8[8] = {};
                    sscanf(tt, "%f,%f,%f,%f,%f,%f,%f,%f", &v8[0], &v8[1], &v8[2], &v8[3], &v8[4], &v8[5], &v8[6], &v8[7]);
                    std::lock_guard<std::recursive_mutex> lk(gUiMutex);
                    StartLayoutEdit();
                    const u16 r[8] = { (u16)v8[1], (u16)v8[2], (u16)v8[3], (u16)(v8[3] * 0.6f), (u16)v8[4], (u16)v8[5], (u16)v8[6], (u16)(v8[6] * 0.75f) };
                    EditSet(r);
                    Settings::values.custom_second_layer_opacity.SetValue((u16)v8[7]);
                    StopLayoutEdit(true);
                    fprintf(stderr, "teste: telas personalizadas\n");
                }
            }
            // PORTE_TECLAAT="s,scancode": press that key at s seconds (shortcut tests)
            {
                // PORTE_TECLAREP="every_s,count": the same key again every every_s seconds, count more times (stress tests)
                static bool keyDone = false;
                static int keyRepLeft = -1;
                static double keyNext = 0;
                if (keyRepLeft < 0) { keyRepLeft = 0; if (const char* kr = GETENV_ONCE("PORTE_TECLAREP")) { keyRepLeft = strchr(kr, ',') ? atoi(strchr(kr, ',') + 1) : 0; } }
                if (const char* ka = GETENV_ONCE("PORTE_TECLAAT"); ka && keyDone && keyRepLeft > 0 && el >= (Uint64)(keyNext * 1000)) { keyDone = false; keyRepLeft--; }
                if (const char* ka = GETENV_ONCE("PORTE_TECLAAT"); ka && !keyDone && el >= (Uint64)(std::max(atof(ka), keyNext) * 1000))
                {
                    keyDone = true;
                    if (const char* kr = GETENV_ONCE("PORTE_TECLAREP")) keyNext = el / 1000.0 + atof(kr);
                    const char* c = strchr(ka, ',');
                    SDL_Event ke {};
                    ke.type = SDL_KEYDOWN;
                    ke.key.keysym.scancode = (SDL_Scancode)(c ? atoi(c + 1) : 0);
                    ke.key.windowID = SDL_GetWindowID(gWindow.win);
                    SDL_PushEvent(&ke);
                    SDL_Event ku = ke;
                    ku.type = SDL_KEYUP;
                    SDL_PushEvent(&ku);
                    fprintf(stderr, "teste: tecla %d\n", (int)ke.key.keysym.scancode);
                }
            }
            // PORTE_CTRLF1AT=N: press Ctrl+F1 at N seconds (the achievement card test)
            static bool ctrlF1 = false;
            if (const char* cf = GETENV_ONCE("PORTE_CTRLF1AT"); cf && !ctrlF1 && el >= (Uint64)(atof(cf) * 1000))
            {
                ctrlF1 = true;
                SDL_SetModState(KMOD_LCTRL);
                SDL_Event ke {};
                ke.type = SDL_KEYDOWN;
                ke.key.keysym.scancode = SDL_SCANCODE_F1;
                ke.key.keysym.sym = SDLK_F1;
                ke.key.keysym.mod = KMOD_LCTRL;
                ke.key.windowID = SDL_GetWindowID(gWindow.win);
                SDL_PushEvent(&ke);
                fprintf(stderr, "teste: Ctrl+F1\n");
            }
            // PORTE_DUMPRAMAT=N: at N seconds write the emulated FCRAM (ram.bin) and the game's
            // heap by virtual address from 0x08000000 (heap.bin, unmapped pages = 0)
            static bool dumped = false;
            // PORTE_DUMPRAMAT=30 or "30,31,32": several dumps (ram_1.bin, ram_2.bin...; the first also ram.bin)
            static int dumpN = 0;
            static std::vector<double> dumpAt;
            if (const char* da = GETENV_ONCE("PORTE_DUMPRAMAT"); da && dumpAt.empty())
                for (const char* q = da; *q;) { dumpAt.push_back(atof(q)); while (*q && *q != ',') q++; if (*q) q++; }
            if (dumpN < (int)dumpAt.size() && el >= (Uint64)(dumpAt[dumpN] * 1000))
            {
                dumpN++;
                dumped = dumpN >= (int)dumpAt.size();
                auto& mem = system.Memory();
                const size_t fc = Settings::values.is_new_3ds.GetValue() ? Memory::FCRAM_N3DS_SIZE : Memory::FCRAM_SIZE;
                if (FILE* f = _wfopen(Widen(gDataDir + "ram_" + std::to_string(dumpN) + ".bin").c_str(), L"wb")) { fwrite(mem.GetFCRAMPointer(0), 1, fc, f); fclose(f); }
                if (dumpN == 1)
                if (FILE* f = _wfopen(Widen(gDataDir + "ram.bin").c_str(), L"wb")) { fwrite(mem.GetFCRAMPointer(0), 1, fc, f); fclose(f); }
                if (auto proc = system.Kernel().GetCurrentProcess())
                    if (FILE* f = _wfopen(Widen(gDataDir + "heap.bin").c_str(), L"wb"))
                    {
                        std::vector<u8> page(Memory::CITRA_PAGE_SIZE);
                        for (VAddr a = Memory::HEAP_VADDR; a < Memory::HEAP_VADDR_END; a += Memory::CITRA_PAGE_SIZE)
                        {
                            if (mem.IsValidVirtualAddress(*proc, a)) mem.ReadBlock(*proc, a, page.data(), page.size());
                            else std::fill(page.begin(), page.end(), 0);
                            fwrite(page.data(), 1, page.size(), f);
                        }
                        fclose(f);
                    }
                fprintf(stderr, "teste: memoria gravada (%zu MB de FCRAM)\n", fc >> 20);
            }
            static std::vector<std::pair<int, Uint64>> path;   // button (-1 = wait), end time (ms after start)
            static bool pathRead = false;
            static int pathHeld = -1;
            if (!pathRead)
            {
                pathRead = true;
                if (const char* ps = GETENV_ONCE("PORTE_PATH"))
                {
                    Uint64 end = (Uint64)(GETENV_ONCE("PORTE_PATHAT") ? atof(GETENV_ONCE("PORTE_PATHAT")) * 1000 : 3000);
                    for (const char* c = ps; *c;)
                    {
                        const char k = *c++;
                        int b = -1;
                        switch (k)
                        {
                        case 'U': b = B_Up; break; case 'D': b = B_Down; break; case 'L': b = B_Left; break; case 'R': b = B_Right; break;
                        case 'u': b = B_CUp; break; case 'd': b = B_CDown; break; case 'l': b = B_CLeft; break; case 'r': b = B_CRight; break;
                        case 'A': b = B_A; break; case 'B': b = B_B; break; case 'X': b = B_X; break; case 'Y': b = B_Y; break;
                        case 'S': b = B_Start; break; case 'E': b = B_Select; break;
                        default: b = -1; break;
                        }
                        end += strtoul(c, (char**)&c, 10);
                        path.push_back({ b, end });
                        if (*c == ',') c++;
                    }
                    fprintf(stderr, "teste: roteiro de %zu passos, termina em %.1f s\n", path.size(), path.empty() ? 0.0 : path.back().second / 1000.0);
                }
            }
            int want = -1;
            Uint64 begin = (Uint64)(GETENV_ONCE("PORTE_PATHAT") ? atof(GETENV_ONCE("PORTE_PATHAT")) * 1000 : 3000);
            for (auto& [b, end] : path)
            {
                if (el < end)
                {
                    // a press is released for the last 60 ms so the same key twice counts twice
                    if (el >= begin && el + 60 < end) want = b;
                    break;
                }
                begin = end;
            }
            // test (PORTE_LATENCIA=<s>): the presenter logs each picture from s-0,3 to s+0,8; the key pressed at s is logged
            static const double latAt = getenv("PORTE_LATENCIA") ? atof(getenv("PORTE_LATENCIA")) : -1;
            if (latAt > 0)
            {
                static bool latKey = false;
                Porte::gLatRec = el + 300 >= (Uint64)(latAt * 1000) && el <= (Uint64)(latAt * 1000) + 800;
                if (!latKey && want >= 0 && want != pathHeld && el + 5 >= (Uint64)(latAt * 1000))
                {
                    latKey = true;
                    fprintf(stderr, "lat: tecla t=%.6f\n", NowSec());
                }
            }
            if (want != pathHeld)
            {
                if (pathHeld >= 0) InputCommon::GetKeyboard()->ReleaseKey(gCfg.keys[pathHeld]);
                if (want >= 0) InputCommon::GetKeyboard()->PressKey(gCfg.keys[want]);
                pathHeld = want;
            }
        }
        // (22/11) testing aid: PORTE_CONTROLE=<folder> = remote control for Claude to play (a copy of the save). Every
        // 100 ms the game reads <folder>/cmd.txt (and deletes it); one command per line:
        //   botao <A|B|X|Y|L|R|S|E|U|D|Lt|Rt|u|d|l|r> <ms>   hold (u d l r = Circle Pad, U D Lt Rt = D-pad; S start E select)
        //   toque <x> <y>          touch the bottom screen (3DS point, 150 ms)
        //   espera <ms>            do nothing for a while
        //   foto <nome>            window picture -> <folder>/<nome>.bmp (after the commands before it)
        //   salvar <slot> | carregar <slot> | sair
        // When the list is done it writes <folder>/pronto.txt (time, scene, FPS). Commands run one after the other.
        if (const char* cdir = GETENV_ONCE("PORTE_CONTROLE"))
        {
            struct Cmd { std::string op; std::string a; int n = 0, m = 0; };
            static std::deque<Cmd> q;
            static Uint64 busyUntil = 0, lastPoll = 0;
            static int held = -1, touchState = 0;
            static bool reportPending = false;
            const Uint64 now = SDL_GetTicks64();
            const std::string dir = std::string(cdir) + "\\";
            if (q.empty() && now - lastPoll >= 100)
            {
                lastPoll = now;
                std::ifstream f(P(dir + "cmd.txt"));
                if (f)
                {
                    for (std::string line; std::getline(f, line);)
                    {
                        std::istringstream ls(line); Cmd c; ls >> c.op;
                        if (c.op == "botao") ls >> c.a >> c.n;
                        else if (c.op == "toque") ls >> c.n >> c.m;
                        else if (c.op == "espera" || c.op == "salvar" || c.op == "carregar") ls >> c.n;
                        else if (c.op == "foto") ls >> c.a;
                        if (!c.op.empty()) q.push_back(c);
                    }
                    f.close();
                    std::error_code ec; std::filesystem::remove(P(dir + "cmd.txt"), ec); std::filesystem::remove(P(dir + "pronto.txt"), ec);
                    reportPending = true;
                }
            }
            if (now >= busyUntil)
            {
                if (held >= 0) { InputCommon::GetKeyboard()->ReleaseKey(gCfg.keys[held]); held = -1; busyUntil = now + 60; }
                else if (touchState == 1) { gWindow.TouchReleased(); touchState = 0; busyUntil = now + 60; }
                else if (!q.empty())
                {
                    Cmd c = q.front(); q.pop_front();
                    static const std::map<std::string, int> btn = { {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"L", B_L}, {"R", B_R},
                        {"S", B_Start}, {"E", B_Select}, {"U", B_Up}, {"D", B_Down}, {"Lt", B_Left}, {"Rt", B_Right},
                        {"u", B_CUp}, {"d", B_CDown}, {"l", B_CLeft}, {"r", B_CRight} };
                    if (c.op == "botao" && btn.count(c.a)) { held = btn.at(c.a); InputCommon::GetKeyboard()->PressKey(gCfg.keys[held]); busyUntil = now + std::max(c.n, 30); }
                    else if (c.op == "toque")
                    {
                        const auto& r = gWindow.GetFramebufferLayout().bottom_screen;
                        gWindow.TouchPressed(r.left + (unsigned)(c.n / 320.f * (r.right - r.left)), r.top + (unsigned)(c.m / 240.f * (r.bottom - r.top)));
                        touchState = 1; busyUntil = now + 150;
                    }
                    else if (c.op == "espera") busyUntil = now + c.n;
                    else if (c.op == "foto") { RequestShot(dir + c.a + ".bmp"); busyUntil = now + 400; }
                    else if (c.op == "salvar") { SaveState(c.n); busyUntil = now + 1500; }
                    else if (c.op == "carregar") { LoadState(c.n); busyUntil = now + 3000; }
                    else if (c.op == "sair") gRunning = false;
                }
                else if (reportPending)
                {
                    reportPending = false;
                    std::ofstream o(P(dir + "pronto.txt"));
                    o << "t=" << (now - start) / 1000.0 << " cena=" << (gInBattle ? "batalha" : "campo") << " tela=" << gFpsText << "\n";
                }
            }
        }
        // testing aid: PORTE_TAP="SEC:X:Y" touches the bottom screen at 3DS point (X, Y) for 150 ms
        if (const char* tp = GETENV_ONCE("PORTE_TAP"))
        {
            static int tapState = 0;
            float ts = 0, tx = 0, ty = 0;
            sscanf(tp, "%f:%f:%f", &ts, &tx, &ty);
            const Uint64 el = SDL_GetTicks64() - start;
            const auto& L = gWindow.GetFramebufferLayout();
            const auto& r = L.bottom_screen;
            if (tapState == 0 && el >= (Uint64)(ts * 1000))
            {
                const unsigned fx = r.left + (unsigned)(tx / 320.f * (r.right - r.left));
                const unsigned fy = r.top + (unsigned)(ty / 240.f * (r.bottom - r.top));
                fprintf(stderr, "teste: toque em %u,%u (tela de baixo %u,%u-%u,%u) -> %d\n", fx, fy, r.left, r.top, r.right, r.bottom,
                        (int)gWindow.TouchPressed(fx, fy));
                tapState = 1;
            }
            else if (tapState == 1 && el >= (Uint64)(ts * 1000 + 150)) { gWindow.TouchReleased(); tapState = 2; }
        }
        // testing aid: PORTE_TESTOPTS=N changes options once a second, 6 times, from N seconds on
        if (const char* to = GETENV_ONCE("PORTE_TESTOPTS"))
        {
            static int done = 0;
            const Uint64 el = SDL_GetTicks64() - start;
            if (done < 6 && el >= (Uint64)(atof(to) * 1000) + (Uint64)done * 1000)
            {
                auto& v = Settings::values;
                switch (done % 4)
                {
                case 0: v.swap_screen.SetValue(!v.swap_screen.GetValue()); break;
                case 1: v.volume.SetValue(v.volume.GetValue() > 0.5f ? 0.3f : 0.8f); break;
                case 2: gCfg.bgColor = (gCfg.bgColor + 1) % 4; break;
                case 3: v.screen_gap.SetValue(v.screen_gap.GetValue() ? 0 : 10); break;
                }
                ApplyLive();
                fprintf(stderr, "teste: opcao trocada %d\n", done + 1);
                done++;
            }
        }
        // testing aid: PORTE_TURBO=SEC holds fast-forward from SEC seconds on
        static bool turboTestDone = false;
        if (const char* tt = GETENV_ONCE("PORTE_TURBO"); tt && !turboTestDone && SDL_GetTicks64() - start >= (Uint64)(atof(tt) * 1000))
        {
            turboTestDone = true;
            gTurbo = true;
            ApplyTurbo();
            fprintf(stderr, "teste: acelerar ligado (%.0f%%), resolucao agora = %u\n", Settings::values.turbo_limit.GetValue(),
                    Settings::values.resolution_factor.GetValue());
        }
        if (const char* to = GETENV_ONCE("PORTE_TURBOOFF"); to && gTurbo && SDL_GetTicks64() - start >= (Uint64)(atof(to) * 1000))
        {
            gTurbo = false;
            ApplyTurbo();
            fprintf(stderr, "teste: acelerar desligado, resolucao de volta = %u\n", Settings::values.resolution_factor.GetValue());
        }
        // testing aid: PORTE_TESTMENU=SEC goes through every option of every tab from SEC on, the
        // way a player does (Right on it, a second later Left back), logging the value before,
        // after and back; then the actions: screenshot, save / load state 7, restart.
        // (Nothing that opens other programs or waits for a real key is pressed.)
        if (const char* tm = GETENV_ONCE("PORTE_TESTMENU"))
        {
            static int page = 0, sel = -1, step = 0, action = 0, options = 0, problems = 0;
            static Uint64 next = 0;
            static std::string label, before, after;
            const Uint64 el = SDL_GetTicks64() - start;
            const auto valueOf = [](int p, int s) {
                auto it = BuildItems(p);
                return s < (int)it.size() ? it[s].value : std::string("?");
            };
            if (step >= 0 && el >= (Uint64)(atof(tm) * 1000) && el >= next)
            {
                if (step == 1)
                {
                    after = valueOf(page, sel);
                    MenuKey(SDL_SCANCODE_LEFT);
                    const std::string back = valueOf(page, sel);
                    const char* note = after == before ? "  <- SEM EFEITO" : back != before ? "  <- NAO VOLTOU" : "";
                    if (*note) problems++;
                    fprintf(stderr, "menu: [%s] %s: %s -> %s -> %s%s\n", PageName(page).c_str(), label.c_str(), before.c_str(), after.c_str(),
                            back.c_str(), note);
                    step = 0;
                    next = el + 500;
                }
                else if (step == 0)
                {
                    // the next option that can be changed
                    bool found = false;
                    while (page < P_Count && !found)
                    {
                        auto items = BuildItems(page);
                        while (++sel < (int)items.size())
                            if (items[sel].change && !items[sel].header) { found = true; label = items[sel].label; break; }
                        if (!found) { page++; sel = -1; }
                    }
                    if (found)
                    {
                        OpenMenu();
                        gMenu.page = page;
                        gMenu.sel = sel;
                        before = valueOf(page, sel);
                        MenuKey(SDL_SCANCODE_RIGHT);
                        options++;
                        step = 1;
                        next = el + 1200;
                    }
                    else
                    {
                        MenuKey(SDL_SCANCODE_ESCAPE);
                        fprintf(stderr, "menu: %d opcoes testadas, %d com problema\n", options, problems);
                        step = 2;
                        next = el + 1000;
                    }
                }
                else if (step == 2)
                {
                    // the actions of the Game tab
                    const std::string shot = gDataDir + "teste_captura.bmp";
                    switch (action++)
                    {
                    case 0:
                        DeleteFileW(Widen(shot).c_str());
                        RequestShot(shot);
                        fprintf(stderr, "menu: [Jogo] Captura de tela pedida\n");
                        next = el + 1500;
                        break;
                    case 1:
                        fprintf(stderr, "menu: [Jogo] Captura de tela: %s\n", FileUtil::Exists(shot) ? "arquivo criado" : "FALHOU (sem arquivo)");
                        gCfg.stateSlot = 7;
                        SaveState(7);
                        fprintf(stderr, "menu: [Jogo] Salvar estado 7 pedido\n");
                        next = el + 3000;
                        break;
                    case 2:
                        fprintf(stderr, "menu: [Jogo] Salvar estado 7: %s\n", Core::GetSaveStateInfo(gProgramId, 0, 7).time ? "salvo" : "FALHOU");
                        LoadState(7);
                        fprintf(stderr, "menu: [Jogo] Carregar estado 7 pedido\n");
                        next = el + 5000;
                        break;
                    case 3:
                        fprintf(stderr, "menu: [Jogo] Reiniciar o jogo pedido\n");
                        Core::System::GetInstance().SendSignal(Core::System::Signal::Reset);
                        next = el + 6000;
                        break;
                    default:
                        fprintf(stderr, "menu: teste completo\n");
                        step = -1;
                        break;
                    }
                }
            }
        }
        const bool menuOpen = gMenu.open;
        UpdateFocusEffects();
        SyncPresenter();
        uiLock.unlock();
        // testing aid: PORTE_AUTOPRESS=N taps A every 2 s from N seconds on
        if (static const int ap = getenv("PORTE_AUTOPRESS") ? atoi(getenv("PORTE_AUTOPRESS")) : 0; ap)
        {
            static bool down = false;
            const Uint64 el = SDL_GetTicks64() - start;
            const double until = GETENV_ONCE("PORTE_AUTOPRESS_UNTIL") ? atof(GETENV_ONCE("PORTE_AUTOPRESS_UNTIL")) : 1e9;
            const bool want = el >= (Uint64)ap * 1000 && el < (Uint64)(until * 1000) && el % 2000 < 120;
            if (want != down)
            {
                down = want;
                if (down) InputCommon::GetKeyboard()->PressKey(gCfg.keys[B_A]);
                else InputCommon::GetKeyboard()->ReleaseKey(gCfg.keys[B_A]);
            }
        }

        // the game runs only while the menu is closed
        if ((!menuOpen || !gCfg.pauseInMenu) && !gUnfocusedPause && !gPhoto.on)
        {
            const double r0 = NowSec();
            const auto status = system.RunLoop();
            gProfRun += NowSec() - r0;
            PorteLog::CoreSlow((NowSec() - r0) * 1000.0);
            if (status == Core::System::ResultStatus::ShutdownRequested) break;
            if (status == Core::System::ResultStatus::ErrorSavestate)
                Toast(T("Não foi possível usar o estado salvo", "Could not use the save state"));
            else if (status != Core::System::ResultStatus::Success)
            {
                Error(T("A emulação parou (erro ", "Emulation stopped (error ") + std::to_string((int)status) + "): " + system.GetStatusDetails());
                break;
            }
        }
        else if (gPhoto.on)
        {
            PhotoTick();      // (the game paused: its last picture with the free camera)
            SDL_Delay(8);
        }
        else SDL_Delay(5);
        UpdateStats();   // when the frame limiter did not wait (fast forward)
        FinishShot();

        const Uint64 now = SDL_GetTicks64();
        // testing aid: PORTE_TESTSTATE=N saves state 1 at N s, loads it at N+5 s,
        // and switches the resolution to 4x at N+8 s
        if (static const int ts = getenv("PORTE_TESTSTATE") ? atoi(getenv("PORTE_TESTSTATE")) : 0; ts)
        {
            std::lock_guard<std::recursive_mutex> lk(gUiMutex);
            static int step = 0;
            const Uint64 el = now - start;
            if (step == 0 && el >= (Uint64)ts * 1000) { step = 1; SaveState(1); fprintf(stderr, "teste: salvar estado 1\n"); }
            if (step == 1 && el >= (Uint64)(ts + 5) * 1000)
            {
                step = 2;
                const auto info = Core::GetSaveStateInfo(gProgramId, 0, 1);
                fprintf(stderr, "teste: estado 1 existe=%d\n", info.time != 0);
                LoadState(1);
            }
            if (step == 2 && el >= (Uint64)(ts + 8) * 1000)
            {
                step = 3;
                Settings::values.resolution_factor.SetValue(4);
                ApplyLive();
                fprintf(stderr, "teste: resolucao 4x\n");
            }
        }
        if (now - lastStats >= 1000)
        {
            // the core's stats are read every frame (see above); add them up per second here
            char b[96];
            const double secs = (now - lastStats) / 1000.0;
            const int gen = (int)std::lround(gGenCount.exchange(0) / secs);
#ifdef PORTE_GAME_UM
            // Ultra Moon: the core's game_fps does not follow how this game swaps its frames (it showed
            // ~8 with 30 on screen): the game's number is the new pictures the presenter really got
            // (the core's "frame changed" flag)
            const int fps = (int)std::lround(Porte::gIngested.exchange(0) / secs);
#else
            const int fps = (int)std::lround(gAccGameFrames / secs);
#endif
            int speed = gAccTime > 0 ? (int)std::lround(gAccSpeed / gAccTime * 100) : 0;
            {
                // (20/11 noite) the speed straight from the core's clock (emulated time / real time over this second).
                // The core's per-frame average read 74-93% with the interpolation on while the game really ran at 100%
                // (this reading, the VBlanks and the game's pictures all said 100%): people saw "slow" where it was not
                static long long sysPrev = 0;
                static double tPrev = 0;
                const long long sysNow = (long long)Core::System::GetInstance().CoreTiming().GetGlobalTimeUs().count();
                const double tNow = NowSec();
                if (sysPrev > 0 && tPrev > 0 && sysNow >= sysPrev && tNow > tPrev)
                {
                    const double sp = (sysNow - sysPrev) / 1e6 / (tNow - tPrev);
                    if (sp >= 0 && sp < 20) speed = (int)std::lround(sp * 100);
                }
                sysPrev = sysNow;
                tPrev = tNow;
            }
            if ((hidden || PorteLog::on) && gAccTime > 0) fprintf(stderr, "stats: tempo emulado %.1f%%, VBlanks %.1f/s (= %.1f%%), janela %.2f s de %.2f s\n",
                                                gAccSpeed / gAccTime * 100, gAccVbl / gAccTime, gAccVbl / gAccTime / 59.83 * 100, gAccTime, secs);
            gAccVbl = 0;
            {
                static unsigned long long vb0 = 0;
                const unsigned long long vb = InterpRec::VblankCount();
                static long long sysP = 0;
                const long long sysN = (long long)Core::System::GetInstance().CoreTiming().GetGlobalTimeUs().count();
                if (hidden && sysP > 0) fprintf(stderr, "stats: relógio do núcleo direto %.1f%%\n", (sysN - sysP) / 1e6 / secs * 100);
                sysP = sysN;
                if (hidden) fprintf(stderr, "stats: VBlanks vistos pela interpolação %.1f/s\n", (vb - vb0) / secs);
                vb0 = vb;
            }
            const int shown = (int)std::lround(gPresents.exchange(0) / secs);
            gAccGameFrames = 0;
            gAccSpeed = gAccTime = 0;
            // what the screen shows in total first, then what the game itself draws
#ifdef PORTE_GAME_UM
            // Ultra Moon: always both -- the screen (pictures sent to the monitor: what tools like MSI
            // Afterburner count) and the game's own frames
            if (true)
#else
            if (gen > 0)
#endif
                snprintf(b, sizeof(b), EN() ? "Screen: %d FPS  ·  game: %d  ·  %d%%" : "Tela: %d FPS  ·  jogo: %d  ·  %d%%", shown, fps, speed);
            else
                snprintf(b, sizeof(b), "%d FPS  ·  %d%%", fps, speed);
            if (const double lim = Settings::values.frame_limit.GetValue(); lim > 0 && std::lround(lim) != 100 && !gTurbo)
            {
                const size_t n = strlen(b);
                snprintf(b + n, sizeof(b) - n, EN() ? "  (limit %d%%)" : "  (limite %d%%)", (int)std::lround(lim));
            }
            {
                std::lock_guard<std::recursive_mutex> lk(gUiMutex);
                if (!gMenu.open) gFpsText = b;
            }
            std::string title = std::string(kGameName) + (gCfg.showFps ? std::string("  -  ") + b : "");
            SDL_SetWindowTitle(gWindow.win, title.c_str());
            if (hidden || PorteLog::on)
                fprintf(stderr, "t=%llus %s  [tela: %d/s; gerados: %d/s; imagens novas: %d/s; acumuladas: %d] [ms/s: loop %.0f espera %.0f]\n",
#ifdef PORTE_GAME_UM
                        (unsigned long long)((now - start) / 1000), b, shown, gen, fps,
#else
                        (unsigned long long)((now - start) / 1000), b, shown, gen, (int)std::lround(Porte::gIngested.exchange(0) / secs),
#endif
                        Porte::gFrameSkip.exchange(0), gProfRun * 1000 / secs, gProfWait * 1000 / secs);
            if (PorteLog::on)
            {
                static unsigned long long pb0 = 0, pu0 = 0, sn0 = 0, sr0 = 0, sb0 = 0;
                const unsigned long long pb = Common::g_porte_pipe_builds, pu = Common::g_porte_pipe_us, sn = Common::g_porte_surf_new,
                                         sr = Common::g_porte_surf_rec, sb = Common::g_porte_surf_rec_big;
                char ex[256];
                snprintf(ex, sizeof(ex), "%s%s%s%s%s | res %ux interp %d | janela %dx%d%s", gInBattle ? "batalha" : "campo", gMenu.open ? " menu" : "",
                         gTurbo ? " ACELERANDO" : "", gUnfocusedPause ? " pausado(sem foco)" : "", gPhoto.on ? " modo-foto" : "",
                         Settings::values.resolution_factor.GetValue(), gCfg.interpFps, gCfg.winW, gCfg.winH,
                         (SDL_GetWindowFlags(gWindow.win) & SDL_WINDOW_FULLSCREEN) ? " tela cheia" : "");
                PorteLog::Second(shown, fps, speed, ex, pb - pb0, (pu - pu0) / 1000.0, sn - sn0, sr - sr0, sb - sb0);
                pb0 = pb; pu0 = pu; sn0 = sn; sr0 = sr; sb0 = sb;
                static int modStep = 0;
                const Uint64 el = now - start;
                if ((modStep == 0 && el > 10000) || (modStep == 1 && el > 120000)) { PorteLog::Modules(modStep ? "2 min" : "10 s"); modStep++; }
            }
            if (DiscordRpc::running)
                DiscordRpc::SetActivity(gInBattle ? T("Em batalha", "In a battle") : T("Explorando Alola", "Exploring Alola"),
                                        gPhoto.on ? T("Modo foto", "Photo mode") : gTurbo ? T("Acelerando", "Fast-forwarding") : "");
            gProfRun = gProfWait = 0;
            lastStats = now;
        }

        if (testSecs && now - start >= (Uint64)testSecs * 1000) gRunning = false;
    }

    // Sistema > Salvar estado ao sair: the "automático" state (slot 11); the core handles the request at the start of
    // a pass of its loop
    if (gCfg.autoSaveExit && system.IsPoweredOn() && (!gHiddenRun || getenv("PORTE_AUTOLOAD_TESTE")))
    {
        system.SendSignal(Core::System::Signal::Save, 11);
        system.RunLoop();
        fprintf(stderr, "estado automático salvo\n");
    }
    RememberWindow2();
    SaveConfig();
    DiscordRpc::Stop();
    RewindStop(system);
    system.frame_limiter.SetWaitCallback(nullptr);
    system.Shutdown();   // the renderer (and with it the presenter thread) goes away here
    InputCommon::Shutdown();
    if (gPad) SDL_GameControllerClose(gPad);
    SDL_DestroyWindow(gWindow.win);
    SDL_Quit();
    // stop the core's log thread now; left to the exit-time destructors it throws
    // (the thread is already gone by then) and the program ends with an error
    Common::Log::Stop();
    return 0;
}
