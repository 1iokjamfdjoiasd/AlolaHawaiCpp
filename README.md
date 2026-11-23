# AlolaHawaiCpp

A static recompilation toolkit and PC frontend for a family of Nintendo 3DS titles released in 2016–2017
(the "Alola" generation: Sun, Moon, Ultra Sun and Ultra Moon, USA versions).

The game's ARM code is translated to C **on your own PC, from your own game dump**, compiled into a native
library, and run inside a modified [Azahar](https://github.com/azahar-emu/azahar) core with a custom frontend.

> **This repository contains no game code, no game assets, no ROMs and no keys.**
> You must own the game and dump it yourself from your own console (decrypted `.3ds`).
> Requests for ROMs, keys or download links will be closed without reply.

## Features

- Native code recompiled from your dump at install time (no JIT), interpreter fallback for the rest
- In-game frame interpolation up to 240 FPS (battles, Z-Moves and cutscenes included)
- Vulkan renderer, true 16:9 layout, upscaling, FXAA
- Full in-game menu (Esc / Guide button): graphics, screen layouts, sound, controls, save states, achievements
- Controller support with Xbox / PlayStation button icons inside the game's own menus
- One launcher for the four supported titles
- English by default (Portuguese available in the menu)

## Using it (Windows 10/11, 64-bit)

1. Download the package from the [Releases](../../releases) page and extract it.
2. Run `Instalador.exe` (the installer), choose **your** decrypted game dump and an install folder, click **Install**.
   The installer extracts the game, translates its code to C and compiles it with the bundled GCC (≈8 minutes, ≈6 GB free space needed during install).
3. Open the launcher (`Pokemon Alola PC.exe` / desktop shortcut) and press **Play**.

Requirements: a GPU with Vulkan support and an up-to-date driver; a CPU with SSE4.2.

## Building from source

Toolchain: MSYS2 UCRT64 (GCC, CMake, Ninja), Python 3 with `capstone` and `pyinstaller`.

```sh
# 1. Azahar at the exact commit the patch was made against
git clone --recursive https://github.com/azahar-emu/azahar.git
cd azahar && git checkout 219d743 && git submodule update --init --recursive
git apply ../azahar-patches/porte.patch
cd ..

# 2. Configure: the frontend is injected through CMAKE_PROJECT_INCLUDE
cmake -S azahar -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_QT=OFF -DENABLE_TESTS=OFF -DENABLE_ROOM=OFF -DENABLE_VULKAN=ON -DENABLE_OPENGL=ON \
      -DCMAKE_PROJECT_INCLUDE="$PWD/porte/um_hook.cmake"
cmake --build build --target PokemonUltraMoonRecomp      # -> bin/PokemonUltraMoon-Recomp.exe

# 3. Launcher
sh porte/launcher/compilar.sh                            # -> bin/Pokemon Alola PC.exe

# 4. Installer (optional)
cd instalador && python -m PyInstaller --noconfirm Instalador.spec
```

The native game library is **not** built here: it is produced by the installer from the user's own dump
(`ferramentas/extrair.py` → `ferramentas/recomp/recomp2.py` → GCC).

## Layout

| Folder | Contents |
|---|---|
| `porte/` | Frontend (menu, interpolation, Vulkan presenter), recompiled-code host, launcher |
| `ferramentas/` | Dump reader, ARM → C translator, controller icon generator |
| `instalador/` | Installer and per-title lists of function entry addresses (addresses only) |
| `azahar-patches/` | Changes to the Azahar core (GPLv2+) |

## Legal

This is an unofficial, non-commercial, fan-made project. It is not affiliated with, endorsed or sponsored by
Nintendo, Game Freak, Creatures or The Pokémon Company. All trademarks belong to their respective owners and are
used only to describe compatibility. No copyrighted game data is distributed: everything game-specific is
generated locally from the user's own legally obtained copy.

Licensed under the **GNU General Public License v2.0 or later** (see `LICENSE`), like the Azahar core it is built on.
Third-party components are listed in `THIRD_PARTY.md`.
