# Third-party components

| Component | Where | License |
|---|---|---|
| [Azahar](https://github.com/azahar-emu/azahar) (3DS emulator core; this project patches it) | `azahar-patches/` (applied to upstream commit `219d743`) | GPLv2 or later |
| [Unicorn Engine](https://github.com/unicorn-engine/unicorn) headers | `porte/unicorn/` | GPLv2 |
| [DejaVu Sans](https://dejavu-fonts.github.io/) (rasterized into `porte/frontend/font_atlas.h`) | menu font | Bitstream Vera / DejaVu font license (free) |
| [Capstone](https://www.capstone-engine.org/) (used by the translator / installer) | Python dependency | BSD 3-Clause |
| GCC / MinGW-w64 (bundled with the release package to compile on the user's PC) | release package only | GPL (sources at gcc.gnu.org and mingw-w64.org) |
