# AlolaHawaiCpp

A static recompilation toolkit and PC frontend for a family of Nintendo 3DS titles released in 2016–2017
(the "Alola" generation: Sun, Moon, Ultra Sun and Ultra Moon, USA versions).

The game's ARM code is translated to C **on your own PC, from your own game dump**, compiled into a native
library, and run inside a modified [Azahar](https://github.com/azahar-emu/azahar) core with a custom frontend.

> **This repository contains no game code, no game assets, no ROMs and no keys.**
> You must own the game and dump it yourself from your own console (decrypted `.3ds`).
> Requests for ROMs, keys or download links will be closed without reply.

## Goal: a 100% native PC port

The long-term goal is a **complete native PC port that does not depend on an emulator at all**. We are working to
move as much as possible out of the emulator, step by step:

| Part | Today | Goal |
|---|---|---|
| Game CPU code (ARM11) | ✅ Recompiled to native x86-64 C: ~98–99% of executed code runs natively, the rest falls back to an interpreter | 100% native, no interpreter |
| Graphics (PICA200 GPU) | Azahar's Vulkan renderer (with our own frame interpolation, 16:9 and presenter) | Direct native renderer driven by the recompiled code |
| Sound (DSP) | Azahar's DSP emulation | Native audio output |
| 3DS OS services (files, saves, input, time) | Azahar's high-level emulation (HLE) | Thin native replacements |
| Frontend (menu, launcher, controls, installer) | ✅ Our own code | — |

In short: the game logic already runs as native code; the remaining emulator pieces are the hardware and
operating-system layers around it, and replacing them is the main work ahead. Contributions are welcome.

### Roadmap to 100% native

**Phase 0 — Foundation** ✅
- [x] ARM11 → C static recompiler, run at install time from the user's own dump
- [x] Native code for ~98–99% of executed game code (interpreter fallback for the rest)
- [x] All four titles supported (Sun, Moon, Ultra Sun, Ultra Moon)
- [x] Own frontend: menu, launcher, installer, controllers, frame interpolation, photo mode

**Phase 1 — CPU: 100% native game code**
- [ ] Translate the remaining functions that still fall back to the interpreter (indirect jumps, rare instructions)
- [ ] Recompile the code modules the game loads at runtime (CRO) ahead of time
- [ ] Automatic checker that proves no game code runs outside the native library
- [ ] Native 60 FPS game logic in the overworld (not just interpolated frames)

**Phase 2 — Operating system: native services**
- [ ] Files and saves read/written directly on the PC (no virtual SD / NAND)
- [ ] Input, clock and applet services (HID, time, APT) as thin native code
- [ ] Game threads on native threads instead of an emulated kernel scheduler
- [ ] Game memory mapped directly in the PC's address space

**Phase 3 — Audio**
- [ ] Replace DSP emulation with a native mixer playing the game's sound data

**Phase 4 — Graphics**
- [ ] Translate the game's PICA200 command lists straight to Vulkan
- [ ] Compile all game shaders at install time (no first-time stutter or glitch)
- [ ] Remove the emulated GPU / rasterizer

**Phase 5 — Standalone**
- [ ] Drop the Azahar core entirely: one native executable + the recompiled game library
- [ ] Linux build
- [ ] Mod support on top of the native code (textures, models, scripts)

## Beyond the original hardware

We don't just want to match the 3DS — we plan to add **many features the original hardware never supported**.
Some are already here:

- **Photo mode** (Game > Photo mode, or F9): the game pauses and the scene is redrawn with a **free camera** —
  move, turn and zoom anywhere around the frozen moment, hide the interface and take the shot. Impossible on the
  original console: it is made possible by re-running the game's own draw calls from a new viewpoint.
- **High frame rate**: in-game frame interpolation up to 240 FPS, while the game logic keeps its original 30 FPS timing.
- **True widescreen (16:9)** and **higher internal resolutions** instead of the original 400×240.
- **Achievements**, **save states** and an **advanced log** for bug reports.
- **Modern controllers** with Xbox / PlayStation button icons shown inside the game's own menus.

More is planned as the port becomes fully native.

## Features

- Game code recompiled to native from your dump at install time (no JIT; ~98–99% native, interpreter fallback for the rest)
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
