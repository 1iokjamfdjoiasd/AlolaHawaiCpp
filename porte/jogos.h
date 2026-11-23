// Os quatro jogos de Alola num exe só: qual jogo é (pelo ID do título) e os arquivos de cada um.
// O Ultra Moon mantém os nomes de antes (momiji_nativo.dll, recomp_desligar.txt, jogo\momiji.cxi).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace Jogos
{

struct Jogo
{
    uint32_t tidLow;         // parte baixa do ID do título (0004000000xxxxxx)
    const char* nome;        // "Ultra Moon"
    const wchar_t* sufixo;   // nos arquivos: L"" (Ultra Moon) ou L"_ultrasun"...
    const wchar_t* dll;      // código nativo (gerado pelo instalador / ferramentas\recomp)
    const wchar_t* cxi;      // instalado ao lado do exe: jogo\<cxi>
};

inline const Jogo kJogos[] = {
    { 0x001B5100, "Ultra Moon", L"",          L"momiji_nativo.dll",   L"momiji.cxi" },
    { 0x001B5000, "Ultra Sun",  L"_ultrasun", L"nativo_ultrasun.dll", L"ultrasun.cxi" },
    { 0x00164800, "Sun",        L"_sun",      L"nativo_sun.dll",      L"sun.cxi" },
    { 0x00175E00, "Moon",       L"_moon",     L"nativo_moon.dll",     L"moon.cxi" },
};

inline const Jogo* Por(uint64_t titleId)
{
    for (const Jogo& j : kJogos)
        if (j.tidLow == (uint32_t)(titleId & 0xFFFFFFFF) && (titleId >> 32) == 0x00040000) return &j;
    return nullptr;
}

// o jogo desta execução (o Ultra Moon até se saber qual é)
inline const Jogo* gAtual = &kJogos[0];

// o ID do título lido do cabeçalho de uma ROM .3ds (NCSD, partição 0) ou .cxi (NCCH); 0 = não deu
inline uint64_t TitleDoArquivo(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return 0;
    unsigned char h[0x200] = {};
    uint64_t tid = 0;
    if (fread(h, 1, sizeof(h), f) == sizeof(h))
    {
        long ncch = -1;
        if (h[0x100] == 'N' && h[0x101] == 'C' && h[0x102] == 'S' && h[0x103] == 'D')
            ncch = (long)((uint32_t)h[0x120] | (uint32_t)h[0x121] << 8 | (uint32_t)h[0x122] << 16 | (uint32_t)h[0x123] << 24) * 0x200;
        else if (h[0x100] == 'N' && h[0x101] == 'C' && h[0x102] == 'C' && h[0x103] == 'H')
            ncch = 0;
        if (ncch > 0)
        {
            if (fseek(f, ncch, SEEK_SET) != 0 || fread(h, 1, sizeof(h), f) != sizeof(h)) ncch = -1;
        }
        if (ncch >= 0 && h[0x100] == 'N' && h[0x101] == 'C' && h[0x102] == 'C' && h[0x103] == 'H')
            for (int i = 7; i >= 0; i--) tid = (tid << 8) | h[0x118 + i];
    }
    fclose(f);
    return tid;
}

}   // namespace Jogos
