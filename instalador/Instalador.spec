# -*- mode: python ; coding: utf-8 -*-
import os
from PyInstaller.utils.hooks import collect_all

# paths relative to the repository (build from instalador/: python -m PyInstaller --noconfirm Instalador.spec)
RAIZ = os.path.abspath(os.path.join(SPECPATH, '..'))

datas = []
binaries = []
hiddenimports = ['extrair', 'gerar', 'recomp2', 'thumb', 'recomp']
tmp_ret = collect_all('capstone')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]


a = Analysis(
    [os.path.join(RAIZ, 'instalador', 'instalador.py')],
    pathex=[os.path.join(RAIZ, 'ferramentas'), os.path.join(RAIZ, 'ferramentas', 'recomp')],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='Instalador',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=[os.path.join(RAIZ, 'porte', 'PokemonUM.ico')],
)
