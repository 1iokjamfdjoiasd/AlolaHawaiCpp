// Launcher do Pokémon Alola PC: escolhe um dos quatro jogos de Alola e abre o porte.
// Fica ao lado do PokemonUltraMoon-Recomp.exe (em bin\ ou na pasta instalada). Os jogos instalados
// estão em jogo\<cxi> (nomes de ../jogos.h); um jogo não instalado pode ser aberto pela ROM, e o
// launcher lembra a ROM de cada jogo (launcher.txt ao lado dele).
// Win32 + GDI puro: nada além do Windows (sem Python, sem SDL).
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>

#include "../jogos.h"

namespace
{

const wchar_t* kExe = L"PokemonUltraMoon-Recomp.exe";

struct Card
{
    const Jogos::Jogo* jogo = nullptr;
    const wchar_t* nome = L"";
    const wchar_t* regiao = L"";
    COLORREF cor = 0;
    bool instalado = false;
    bool incompleto = false;   // the game copied but its native code (DLL) missing: the install stopped in the middle
    double gb = 0;
    std::wstring rom;   // the ROM remembered for it (when not installed)
    RECT r {};
};

std::wstring gDir;            // the launcher's folder (= the port's exe and jogo\)
std::wstring gInstaladorExe;  // Instalador.exe of the package this was installed from (instalador_origem.txt)
std::wstring gInstalador;     // instalador\instalador.py, when this is the development folder
Card gCards[4];
int gSel = 0;
int gHover = -1;              // 0..3 cards, 10 + button
std::wstring gStatus;
float gS = 1;                 // DPI scale
HFONT fTitulo, fNome, fTexto, fPeq, fBotao;

enum { BT_JOGAR, BT_ROM, BT_INSTALAR, BT_PASTA, BT_N };
RECT gBt[BT_N];
// (23/11) English by default; Portuguese only when the game's menu was set to it (idioma=0 in the game's config.ini)
bool gEN = true;
const wchar_t* T(const wchar_t* pt, const wchar_t* en) { return gEN ? en : pt; }
const wchar_t* kBtPt[BT_N] = { L"Jogar", L"Abrir ROM…", L"Instalar jogo…", L"Pasta do jogo" };
const wchar_t* kBtEn[BT_N] = { L"Play", L"Open ROM…", L"Install game…", L"Game folder" };

const COLORREF kFundo = RGB(20, 15, 38), kCartao = RGB(34, 27, 60), kCartaoSel = RGB(46, 37, 82),
               kTexto = RGB(242, 238, 252), kFraco = RGB(170, 160, 205), kBotao = RGB(52, 43, 90);

int S(int v) { return (int)(v * gS + 0.5f); }

std::wstring ConfigPath() { return gDir + L"launcher.txt"; }

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
bool Existe(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void Carregar()
{
    std::ifstream f(ConfigPath().c_str());
    std::map<std::string, std::string> kv;
    for (std::string l; std::getline(f, l);)
    {
        const auto eq = l.find('=');
        if (eq != std::string::npos) kv[l.substr(0, eq)] = l.substr(eq + 1);
    }
    for (Card& c : gCards)
    {
        auto it = kv.find("rom" + Utf8(c.jogo->sufixo[0] ? c.jogo->sufixo : L"_ultramoon"));
        if (it != kv.end()) c.rom = Wide(it->second);
    }
    if (kv.count("sel")) gSel = std::clamp(atoi(kv["sel"].c_str()), 0, 3);
}

void Salvar()
{
    std::ofstream f(ConfigPath().c_str());
    if (!f) return;   // (a read-only folder: just not remembered)
    f << "sel=" << gSel << "\n";
    for (const Card& c : gCards)
        if (!c.rom.empty()) f << "rom" << Utf8(c.jogo->sufixo[0] ? c.jogo->sufixo : L"_ultramoon") << "=" << Utf8(c.rom) << "\n";
}

void Atualizar()
{
    for (Card& c : gCards)
    {
        const std::wstring cxi = gDir + L"jogo\\" + c.jogo->cxi;
        WIN32_FILE_ATTRIBUTE_DATA a;
        c.instalado = GetFileAttributesExW(cxi.c_str(), GetFileExInfoStandard, &a) != 0;
        c.gb = c.instalado ? (((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow) / 1073741824.0 : 0;
        c.incompleto = c.instalado && !Existe(gDir + c.jogo->dll);
    }
}

void Status(HWND w, const std::wstring& s)
{
    gStatus = s;
    InvalidateRect(w, nullptr, FALSE);
}

// opens the port with this file (installed cxi or ROM); the launcher closes when it starts
bool Abrir(HWND w, const std::wstring& arquivo)
{
    const std::wstring exe = gDir + kExe;
    if (!Existe(exe))
    {
        Status(w, std::wstring(T(L"Não achei o ", L"Could not find ")) + kExe + T(L" nesta pasta.", L" in this folder."));
        return false;
    }
    std::wstring cmd = L"\"" + exe + L"\" \"" + arquivo + L"\"";
    STARTUPINFOW si { sizeof(si) };
    PROCESS_INFORMATION pi {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, gDir.c_str(), &si, &pi))
    {
        Status(w, std::wstring(T(L"Não consegui abrir o jogo (erro ", L"Could not start the game (error ")) + std::to_wstring(GetLastError()) + L").");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Salvar();
    DestroyWindow(w);
    return true;
}

bool EscolherRom(HWND w, std::wstring& out)
{
    wchar_t buf[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn { sizeof(ofn) };
    ofn.hwndOwner = w;
    ofn.lpstrFilter = T(L"ROM de 3DS (*.3ds;*.cci;*.cxi)\0*.3ds;*.cci;*.cxi\0Todos os arquivos\0*.*\0",
                        L"3DS ROM (*.3ds;*.cci;*.cxi)\0*.3ds;*.cci;*.cxi\0All files\0*.*\0");
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)std::size(buf);
    ofn.lpstrTitle = T(L"Escolha a sua ROM de Pokémon Sun, Moon, Ultra Sun ou Ultra Moon (descriptografada)",
                       L"Choose your Pokémon Sun, Moon, Ultra Sun or Ultra Moon ROM (decrypted)");
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = buf;
    return true;
}

// Abrir ROM: reads which game it is, remembers it for that card and plays
void AbrirRom(HWND w)
{
    std::wstring rom;
    if (!EscolherRom(w, rom)) return;
    const Jogos::Jogo* j = Jogos::Por(Jogos::TitleDoArquivo(rom));
    if (!j)
    {
        Status(w, T(L"Essa ROM não é Pokémon Sun, Moon, Ultra Sun ou Ultra Moon (ou está criptografada).", L"That ROM is not Pokémon Sun, Moon, Ultra Sun or Ultra Moon (or it is encrypted)."));
        return;
    }
    for (int i = 0; i < 4; i++)
        if (gCards[i].jogo == j) { gSel = i; gCards[i].rom = rom; }
    Abrir(w, rom);
}

void Jogar(HWND w)
{
    Card& c = gCards[gSel];
    if (c.instalado) { Abrir(w, gDir + L"jogo\\" + c.jogo->cxi); return; }
    if (!c.rom.empty() && Existe(c.rom)) { Abrir(w, c.rom); return; }
    Status(w, std::wstring(c.nome) + T(L" não está instalado: use \"Instalar jogo…\" ou \"Abrir ROM…\".", L" is not installed: use \"Install game…\" or \"Open ROM…\"."));
}

void Instalar(HWND w)
{
    if (!gInstaladorExe.empty())
    {
        const std::wstring dirExe = gInstaladorExe.substr(0, gInstaladorExe.find_last_of(L"\\/"));
        HINSTANCE r = ShellExecuteW(w, L"open", gInstaladorExe.c_str(), nullptr, dirExe.c_str(), SW_SHOWNORMAL);
        Status(w, (INT_PTR)r <= 32 ? T(L"Não consegui abrir o instalador.", L"Could not open the installer.") : T(L"Instalador aberto: instale na mesma pasta; o jogo novo aparece aqui.", L"Installer opened: install into this same folder; the new game shows up here."));
        return;
    }
    if (gInstalador.empty())
    {
        Status(w, T(L"Não achei o Instalador.exe (abra-o direto da pasta do pacote que você baixou).", L"Could not find Instalador.exe (open it straight from the folder of the package you downloaded)."));
        return;
    }
    const std::wstring dirInst = gInstalador.substr(0, gInstalador.find_last_of(L'\\'));
    HINSTANCE r = ShellExecuteW(w, L"open", L"pythonw", (L"\"" + gInstalador + L"\"").c_str(), dirInst.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) Status(w, T(L"Não consegui abrir o instalador (precisa do Python nesta versão de teste).", L"Could not open the installer (this test version needs Python)."));
    else Status(w, T(L"Instalador aberto. Quando terminar, abra o launcher da pasta onde instalou.", L"Installer opened. When it finishes, open the launcher from the folder you installed to."));
}

void Pasta(HWND w)
{
    ShellExecuteW(w, L"open", gDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Acao(HWND w, int b)
{
    switch (b)
    {
    case BT_JOGAR: Jogar(w); break;
    case BT_ROM: AbrirRom(w); break;
    case BT_INSTALAR: Instalar(w); break;
    case BT_PASTA: Pasta(w); break;
    }
}

// ---------------------------------------------------------------- drawing
void Caixa(HDC dc, const RECT& r, int raio, COLORREF fill, COLORREF borda = CLR_INVALID, int esp = 0)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = borda == CLR_INVALID ? (HPEN)GetStockObject(NULL_PEN) : CreatePen(PS_SOLID, esp, borda);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, raio, raio);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    if (borda != CLR_INVALID) DeleteObject(p);
}

void Texto(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt = DT_LEFT | DT_TOP | DT_SINGLELINE)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), (int)t.size(), &r, fmt | DT_NOPREFIX);
}

COLORREF Mistura(COLORREF a, COLORREF b, float t)
{
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

void Layout(int w, int h)
{
    const int m = S(28), topo = S(96), gap = S(16);
    const int bh = S(46), base = h - m - S(30) - bh;
    const int cw = (w - 2 * m - gap) / 2, ch = (base - topo - gap - S(18)) / 2;
    for (int i = 0; i < 4; i++)
    {
        const int x = m + (i % 2) * (cw + gap), y = topo + (i / 2) * (ch + gap);
        gCards[i].r = { x, y, x + cw, y + ch };
    }
    const int bw0 = S(170), bw = (w - 2 * m - bw0 - 3 * S(12)) / 3;
    int x = w - m;
    gBt[BT_JOGAR] = { x - bw0, base, x, base + bh };
    x -= bw0 + S(12);
    for (int b : { BT_ROM, BT_INSTALAR, BT_PASTA })
    {
        gBt[b] = { x - bw, base, x, base + bh };
        x -= bw + S(12);
    }
}

void Pintar(HWND w, HDC dc0)
{
    RECT cr;
    GetClientRect(w, &cr);
    HDC dc = CreateCompatibleDC(dc0);
    HBITMAP bmp = CreateCompatibleBitmap(dc0, cr.right, cr.bottom);
    HGDIOBJ ob = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    HBRUSH fundo = CreateSolidBrush(kFundo);
    FillRect(dc, &cr, fundo);
    DeleteObject(fundo);
    Layout(cr.right, cr.bottom);

    const Card& sel = gCards[gSel];
    Texto(dc, L"Pokémon Alola PC", { S(28), S(22), cr.right - S(28), S(62) }, fTitulo, kTexto);
    Texto(dc, T(L"Sun · Moon · Ultra Sun · Ultra Moon — código do jogo recompilado para o PC (teste)", L"Sun · Moon · Ultra Sun · Ultra Moon — game code recompiled for PC (test)"),
          { S(28), S(62), cr.right - S(28), S(86) }, fPeq, kFraco);

    for (int i = 0; i < 4; i++)
    {
        const Card& c = gCards[i];
        const bool s = i == gSel, hv = i == gHover;
        Caixa(dc, c.r, S(18), s ? kCartaoSel : hv ? Mistura(kCartao, kCartaoSel, 0.5f) : kCartao, s ? c.cor : CLR_INVALID, S(3));
        // the game's colour: a band on the left
        RECT band { c.r.left + S(14), c.r.top + S(16), c.r.left + S(20), c.r.bottom - S(16) };
        Caixa(dc, band, S(6), c.instalado || !c.rom.empty() ? c.cor : Mistura(c.cor, kCartao, 0.6f));
        const int x = c.r.left + S(36);
        Texto(dc, c.nome, { x, c.r.top + S(16), c.r.right - S(12), c.r.top + S(52) }, fNome, kTexto);
        Texto(dc, c.regiao, { x, c.r.top + S(52), c.r.right - S(12), c.r.top + S(74) }, fPeq, kFraco);
        std::wstring st;
        COLORREF stc = kFraco;
        if (c.instalado && c.incompleto)
        {
            st = T(L"▲ Incompleta: instale de novo", L"▲ Incomplete: install again");
            stc = RGB(255, 190, 90);
        }
        else if (c.instalado)
        {
            wchar_t b[64];
            swprintf(b, 64, T(L"● Instalado  ·  %.2f GB", L"● Installed  ·  %.2f GB"), c.gb);
            st = b;
            if (!gEN) std::replace(st.begin(), st.end(), L'.', L',');   // 3,45 GB
            stc = c.cor;
        }
        else if (!c.rom.empty() && Existe(c.rom))
        {
            st = std::wstring(T(L"● Pela ROM: ", L"● From ROM: ")) + c.rom.substr(c.rom.find_last_of(L"\\/") + 1);
            stc = Mistura(c.cor, kTexto, 0.3f);
        }
        else st = T(L"○ Não instalado", L"○ Not installed");
        Texto(dc, st, { x, c.r.bottom - S(40), c.r.right - S(14), c.r.bottom - S(14) }, fTexto, stc,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    for (int b = 0; b < BT_N; b++)
    {
        const bool hv = gHover == 10 + b;
        const bool pode = b != BT_INSTALAR || !gInstalador.empty() || !gInstaladorExe.empty();
        COLORREF f = b == BT_JOGAR ? sel.cor : kBotao;
        if (hv && pode) f = Mistura(f, RGB(255, 255, 255), 0.15f);
        Caixa(dc, gBt[b], S(14), f);
        Texto(dc, (gEN ? kBtEn : kBtPt)[b], gBt[b], fBotao, b == BT_JOGAR ? RGB(18, 12, 30) : pode ? kTexto : Mistura(kFraco, kBotao, 0.4f),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    std::wstring dica = gStatus.empty() ? std::wstring(T(L"Enter joga  ·  setas escolhem  ·  duplo clique no cartão joga  ·  Esc no jogo abre o menu", L"Enter plays  ·  arrows choose  ·  double-click a card to play  ·  Esc in game opens the menu"))
                                        : gStatus;
    Texto(dc, dica, { S(28), cr.bottom - S(40), cr.right - S(28), cr.bottom - S(14) }, fPeq, gStatus.empty() ? kFraco : kTexto,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    BitBlt(dc0, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
}

int Acerto(int x, int y)
{
    const POINT p { x, y };
    for (int i = 0; i < 4; i++) if (PtInRect(&gCards[i].r, p)) return i;
    for (int b = 0; b < BT_N; b++) if (PtInRect(&gBt[b], p)) return 10 + b;
    return -1;
}

HFONT Fonte(int px, int peso, const wchar_t* nome = L"Segoe UI")
{
    return CreateFontW(-S(px), 0, 0, 0, peso, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, nome);
}

LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        Pintar(w, dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED:
    {
        // dragged to a monitor with another scale: new fonts and the size Windows suggests
        gS = HIWORD(wp) / 96.f;
        for (HFONT* f : { &fTitulo, &fNome, &fTexto, &fPeq, &fBotao }) DeleteObject(*f);
        fTitulo = Fonte(28, FW_SEMIBOLD, L"Segoe UI Semibold");
        fNome = Fonte(22, FW_SEMIBOLD, L"Segoe UI Semibold");
        fTexto = Fonte(15, FW_NORMAL);
        fPeq = Fonte(13, FW_NORMAL);
        fBotao = Fonte(16, FW_SEMIBOLD, L"Segoe UI Semibold");
        const RECT* r = (const RECT*)lp;
        SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    case WM_SIZE: InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO:
    {
        auto* mi = (MINMAXINFO*)lp;
        mi->ptMinTrackSize = { S(720), S(500) };
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        const int h = Acerto(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != gHover)
        {
            gHover = h;
            SetCursor(LoadCursor(nullptr, h >= 0 ? IDC_HAND : IDC_ARROW));
            InvalidateRect(w, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t { sizeof(t), TME_LEAVE, w, 0 };
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: gHover = -1; InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, gHover >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN:
    {
        const int h = Acerto(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h >= 0 && h < 4) { gSel = h; gStatus.clear(); InvalidateRect(w, nullptr, FALSE); }
        else if (h >= 10) Acao(w, h - 10);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
        const int h = Acerto(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h >= 0 && h < 4) { gSel = h; Jogar(w); }
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp)
        {
        case VK_LEFT: case VK_RIGHT: gSel ^= 1; break;
        case VK_UP: case VK_DOWN: gSel ^= 2; break;
        case VK_RETURN: Jogar(w); return 0;
        case VK_ESCAPE: DestroyWindow(w); return 0;
        default: return 0;
        }
        gStatus.clear();
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) { Atualizar(); InvalidateRect(w, nullptr, FALSE); }   // (an install may have finished)
        return 0;
    case WM_DESTROY:
        Salvar();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

}   // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t exe[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    gDir = exe;
    gDir = gDir.substr(0, gDir.find_last_of(L"\\/") + 1);
    // development folder (bin\ inside the port's folder): the installer is next to it
    const std::wstring inst1 = gDir + L"..\\instalador\\instalador.py";
    if (Existe(inst1))
    {
        wchar_t full[MAX_PATH * 2];
        GetFullPathNameW(inst1.c_str(), MAX_PATH * 2, full, nullptr);
        gInstalador = full;
    }

    // installed from a package: the Instalador.exe it came from (the installer writes its path here)
    {
        std::ifstream f((gDir + L"instalador_origem.txt").c_str());
        std::string l;
        if (std::getline(f, l))
        {
            const std::wstring e = Wide(l);
            if (Existe(e)) gInstaladorExe = e;
        }
    }

    // the cards, in the games' order: Sun, Moon, Ultra Sun, Ultra Moon
    const struct { uint32_t tid; const wchar_t* nome; const wchar_t* reg; COLORREF cor; } kOrdem[] = {
        { 0x00164800, L"Pokémon Sun", L"2016  ·  Solgaleo", RGB(255, 160, 50) },
        { 0x00175E00, L"Pokémon Moon", L"2016  ·  Lunala", RGB(105, 140, 255) },
        { 0x001B5000, L"Pokémon Ultra Sun", L"2017  ·  Necrozma Dusk Mane", RGB(255, 105, 70) },
        { 0x001B5100, L"Pokémon Ultra Moon", L"2017  ·  Necrozma Dawn Wings", RGB(170, 125, 255) },
    };
    for (int i = 0; i < 4; i++)
    {
        gCards[i].jogo = Jogos::Por(0x0004000000000000ull | kOrdem[i].tid);
        gCards[i].nome = kOrdem[i].nome;
        gCards[i].regiao = kOrdem[i].reg;
        gCards[i].cor = kOrdem[i].cor;
    }
    // language: the game's own setting (shared by the four games); no file or no "idioma=0" = English
    {
        wchar_t ad[MAX_PATH * 2] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH * 2))
        {
            std::ifstream f((std::wstring(ad) + L"\\Pokemon Ultra Moon\\config.ini").c_str());
            for (std::string l; std::getline(f, l);)
                if (l.rfind("idioma=", 0) == 0) gEN = l.substr(7) != "0";
        }
        if (const char* lg = getenv("LAUNCHER_LANG")) gEN = std::string(lg) != "pt";   // test aid
    }
    Carregar();
    Atualizar();

    WNDCLASSEXW wc { sizeof(wc) };
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PokemonAlolaLauncher";
    RegisterClassExW(&wc);

    HDC sdc = GetDC(nullptr);
    gS = GetDeviceCaps(sdc, LOGPIXELSY) / 96.f;
    ReleaseDC(nullptr, sdc);
    fTitulo = Fonte(28, FW_SEMIBOLD, L"Segoe UI Semibold");
    fNome = Fonte(22, FW_SEMIBOLD, L"Segoe UI Semibold");
    fTexto = Fonte(15, FW_NORMAL);
    fPeq = Fonte(13, FW_NORMAL);
    fBotao = Fonte(16, FW_SEMIBOLD, L"Segoe UI Semibold");

    RECT r { 0, 0, S(820), S(560) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"Pokémon Alola PC", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    // dark title bar (Windows 10 20H1+ / 11)
    BOOL dark = TRUE;
    using DwmSet = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
        if (auto set = (DwmSet)GetProcAddress(dwm, "DwmSetWindowAttribute")) set(w, 20, &dark, sizeof(dark));
    if (const char* foto = getenv("LAUNCHER_FOTO"))   // test aid: draws the (hidden) window into a .bmp and quits
    {
        if (getenv("LAUNCHER_SEL")) gSel = std::clamp(atoi(getenv("LAUNCHER_SEL")), 0, 3);
        if (getenv("LAUNCHER_HOVER")) gHover = atoi(getenv("LAUNCHER_HOVER"));
        RECT cr;
        GetClientRect(w, &cr);
        HDC sdc2 = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(sdc2);
        BITMAPINFO bi {};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), cr.right, -cr.bottom, 1, 32, BI_RGB };
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(sdc2, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ ob = SelectObject(dc, bmp);
        Pintar(w, dc);
        GdiFlush();
        const DWORD n = (DWORD)cr.right * cr.bottom * 4;
        BITMAPFILEHEADER fh { 0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + n), 0, 0,
                              (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) };
        if (FILE* f = fopen(foto, "wb"))
        {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
            fwrite(bits, 1, n, f);
            fclose(f);
        }
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(nullptr, sdc2);
        DestroyWindow(w);
    }
    else
    {
        ShowWindow(w, show);
        UpdateWindow(w);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
