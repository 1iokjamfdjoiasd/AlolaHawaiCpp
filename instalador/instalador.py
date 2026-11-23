r"""Instalador do Pokémon Alola PC (Sun, Moon, Ultra Sun, Ultra Moon; teste de recompilação).

Como o KarPur: o porte não traz nada do jogo. Na instalação, a partir da ROM da própria
pessoa (uma vez por jogo):
  1. confere a ROM (um dos quatro jogos de Alola, descriptografada);
  2. copia a parte do jogo que roda (o programa + os arquivos, ~3,5 GB) para jogo\<jogo>.cxi;
  3. traduz o código do jogo para C (ferramentas\extrair.py + recomp\gerar.py) e compila a DLL nativa neste PC;
  4. coloca o exe do porte, a lista de funções desligadas do jogo e um atalho "Jogar Pokémon <jogo>".
Vários jogos podem ficar na mesma pasta (um exe só; cada um com o seu cxi, a sua DLL e o seu atalho).
Depois disso a ROM não é mais necessária.

uso: Instalar.bat (janela)   ou   python -I instalador.py --auto <rom.3ds> <pasta>
"""
import concurrent.futures
import os
import queue
import shutil
import struct
import subprocess
import sys
import threading
import time
import traceback

# Instalador.exe (PyInstaller, pacote para outras pessoas): RAIZ = a pasta do exe, com bin\, instalador\extras\
# e compilador\ ao lado (sem Python nem MSYS2 no PC); as ferramentas de tradução vão dentro do próprio exe
CONGELADO = getattr(sys, 'frozen', False)
if CONGELADO:
    RAIZ = os.path.dirname(os.path.abspath(sys.executable))
    AQUI = os.path.join(RAIZ, 'instalador')
else:
    AQUI = os.path.dirname(os.path.abspath(__file__))
    RAIZ = os.path.dirname(AQUI)                  # PORTE OFICIAL DO SUN & MOON
sys.path.insert(0, os.path.join(RAIZ, 'ferramentas', 'recomp'))

# os mesmos nomes do exe (porte_recomp/jogos.h): cxi instalado, DLL nativa, sufixo da lista de funções desligadas
GAMES = {
    0x00040000001B5100: {'nome': 'Pokémon Ultra Moon', 'cxi': 'momiji.cxi', 'dll': 'momiji_nativo.dll', 'suf': '', 'extras': 'extras_ultramoon.txt'},
    0x00040000001B5000: {'nome': 'Pokémon Ultra Sun', 'cxi': 'ultrasun.cxi', 'dll': 'nativo_ultrasun.dll', 'suf': '_ultrasun', 'extras': 'extras_ultrasun.txt'},
    0x0004000000164800: {'nome': 'Pokémon Sun', 'cxi': 'sun.cxi', 'dll': 'nativo_sun.dll', 'suf': '_sun', 'extras': 'extras_sunmoon.txt'},
    0x0004000000175E00: {'nome': 'Pokémon Moon', 'cxi': 'moon.cxi', 'dll': 'nativo_moon.dll', 'suf': '_moon', 'extras': 'extras_sunmoon.txt'},
}
MU = 0x200
PRECISA_GB = 6.0                                  # jogo 3,5 GB + compilação (apagada no fim)


# (23/11) English by default; Portuguese only when the game's menu was set to it (idioma=0 in the game's config.ini,
# shared by the four games). INSTALADOR_LANG=pt|en forces one (test aid)
def _idioma_en():
    forcado = os.environ.get('INSTALADOR_LANG')
    if forcado:
        return forcado != 'pt'
    try:
        with open(os.path.join(os.environ.get('APPDATA', ''), 'Pokemon Ultra Moon', 'config.ini'), encoding='utf-8',
                  errors='replace') as f:
            for l in f:
                if l.startswith('idioma='):
                    return l.strip()[7:] != '0'
    except OSError:
        pass
    return True


EN = _idioma_en()


def T(pt, en):
    return en if EN else pt
EXE = 'PokemonUltraMoon-Recomp.exe'
CFLAGS = ['-O1', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off', '-frounding-math',
          '-fno-math-errno', '-msse2', '-w']


class Erro(Exception):
    pass


def conferir_rom(path):
    with open(path, 'rb') as f:
        h = f.read(0x200)
        if len(h) < 0x200 or h[0x100:0x104] != b'NCSD':
            raise Erro(T('Esse arquivo não é uma ROM de 3DS (.3ds).', 'That file is not a 3DS ROM (.3ds).'))
        off, size = struct.unpack_from('<II', h, 0x120)
        f.seek(off * MU)
        n = f.read(0x200)
        if n[0x100:0x104] != b'NCCH':
            raise Erro(T('A ROM parece danificada (a parte principal não foi encontrada).', 'The ROM looks damaged (its main part was not found).'))
        pid = struct.unpack_from('<Q', n, 0x118)[0]
        if pid not in GAMES:
            raise Erro(T(f'Essa ROM não é Pokémon Sun, Moon, Ultra Sun ou Ultra Moon (código do jogo {pid:016X}).', f'That ROM is not Pokémon Sun, Moon, Ultra Sun or Ultra Moon (game code {pid:016X}).'))
        if not n[0x18F] & 4:
            raise Erro(T('Essa ROM está criptografada. Use a sua cópia descriptografada (extraída do seu console).', 'That ROM is encrypted. Use your decrypted copy (dumped from your console).'))
        return dict(GAMES[pid], offset=off * MU, tamanho=size * MU, program_id=pid)


def achar_gcc():
    # the one that comes with the package first (the same gcc 16.2 the port was tested with)
    for c in (os.path.join(RAIZ, 'compilador', 'bin', 'gcc.exe'), shutil.which('gcc'),
              r'C:\msys64\ucrt64\bin\gcc.exe', r'C:\msys64\mingw64\bin\gcc.exe'):
        if c and os.path.exists(c):
            return c
    return None


def instalar(rom, dest, log, progresso):
    """log(texto), progresso(etapa 0..1 geral, texto da etapa)"""
    t0 = time.time()
    info = conferir_rom(rom)
    log(f'ROM: {info["nome"]} ({info["tamanho"] / 2**30:.2f} GB ' + T('da parte principal)', 'main part)'))
    os.makedirs(dest, exist_ok=True)
    livre = shutil.disk_usage(dest).free / 2**30
    if livre < PRECISA_GB:
        raise Erro(T(f'Espaço livre insuficiente: {livre:.1f} GB (precisa de {PRECISA_GB:.0f} GB).', f'Not enough free space: {livre:.1f} GB (needs {PRECISA_GB:.0f} GB).'))
    # (20/11) the game / launcher of this folder open: their files cannot be replaced - say so before copying 3 GB
    # (it copied everything, failed with a technical error and left the 3 GB copy behind)
    for f in (os.path.join(dest, EXE), os.path.join(dest, info['dll']), os.path.join(dest, 'Pokemon Alola PC.exe'),
              os.path.join(dest, 'jogo', info['cxi'])):
        if os.path.exists(f):
            try:
                open(f, 'r+b').close()
            except PermissionError:
                raise Erro(T('O jogo ou o launcher desta pasta está aberto. Feche os dois e clique em Instalar de novo.', 'The game or the launcher in this folder is open. Close both and click Install again.'))
    gcc = achar_gcc()
    if not gcc:
        raise Erro(T('Não achei o compilador (pasta "compilador" ao lado do instalador). Baixe o pacote de novo '
                   'e extraia tudo antes de abrir o Instalador.exe.',
                   'Could not find the compiler (the "compilador" folder next to the installer). Download the package again '
                   'and extract everything before opening Instalador.exe.'))
    log(T('Compilador: ', 'Compiler: ') + gcc)
    try:
        import capstone  # noqa: F401
    except ImportError:
        log(T('Instalando o capstone (desmontador ARM, usado na tradução)...', 'Installing capstone (ARM disassembler, used by the translation)...'))
        subprocess.check_call([sys.executable, '-m', 'pip', 'install', '--user', 'capstone'])

    # 1. the game: the main partition of the ROM, as a 3DS installs it (an NCCH / .cxi)
    jogo = os.path.join(dest, 'jogo')
    os.makedirs(jogo, exist_ok=True)
    cxi = os.path.join(jogo, info['cxi'])
    total = info['tamanho']
    with open(rom, 'rb') as src, open(cxi + '.parcial', 'wb') as out:
        src.seek(info['offset'])
        done = 0
        while done < total:
            b = src.read(min(16 << 20, total - done))
            if not b:
                raise Erro(T('A ROM terminou antes do esperado.', 'The ROM ended earlier than expected.'))
            out.write(b)
            done += len(b)
            progresso(0.40 * done / total, T(f'Copiando o jogo: {done / 2**30:.2f} de {total / 2**30:.2f} GB', f'Copying the game: {done / 2**30:.2f} of {total / 2**30:.2f} GB'))
    os.replace(cxi + '.parcial', cxi)
    log(T('Jogo copiado.', 'Game copied.'))

    py = [sys.executable, '-I']

    def ferramenta(nome, *args):   # extrair / gerar: inside the exe when packaged, the .py files otherwise
        if CONGELADO:
            return [sys.executable, '--ferramenta', nome] + list(args)
        script = os.path.join(RAIZ, 'ferramentas', 'extrair.py') if nome == 'extrair' else \
            os.path.join(RAIZ, 'ferramentas', 'recomp', nome + '.py')
        return py + [script] + list(args)
    if os.path.basename(sys.executable).lower() == 'pythonw.exe':   # (the window runs in pythonw: use python.exe)
        alt = os.path.join(os.path.dirname(sys.executable), 'python.exe')
        if os.path.exists(alt):
            py = [alt, '-I']
    sem_janela = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

    # 2. what the translator needs: the exheader, the code and the modules (.cro) - read from the ROM
    comp = os.path.join(dest, 'compilacao')
    ext = os.path.join(comp, 'extraido')
    progresso(0.41, T('Lendo o código do jogo...', 'Reading the game code...'))
    r = subprocess.run(ferramenta('extrair', rom, ext), capture_output=True, text=True,
                       creationflags=sem_janela)
    if r.returncode:
        raise Erro(T('A leitura do código falhou:', 'Reading the code failed:') + f'\n{(r.stdout + r.stderr)[-2000:]}')

    # 3. translate (ARM -> C), the whole game: main program + modules
    progresso(0.43, T('Traduzindo o código do jogo para C (cerca de 1 minuto)...', 'Translating the game code to C (about 1 minute)...'))
    out_c = os.path.join(comp, 'c')
    # extra entry points found by playing (instalador\extras): what takes the game from ~96% to 98-99% native
    extras = os.path.join(AQUI, 'extras', info['extras'])
    opc = ['--generoso'] + (['--extras', extras] if os.path.exists(extras) else [])
    r = subprocess.run(ferramenta('gerar', ext, out_c, *opc),
                       capture_output=True, text=True, creationflags=sem_janela)
    if r.returncode:
        raise Erro(T('A tradução falhou:', 'The translation failed:') + f'\n{(r.stdout + r.stderr)[-2000:]}')
    fontes = sorted(f for f in os.listdir(out_c) if f.endswith('.c') and (f.startswith('funcoes_') or f in ('tabela.c', 'runtime_host.c')))
    log(T(f'Tradução pronta: {len(fontes)} arquivos C.', f'Translation done: {len(fontes)} C files.'))

    # 4. compile (in parallel) and link the native DLL of this game
    env = dict(os.environ)
    env['PATH'] = os.path.dirname(gcc) + os.pathsep + env.get('PATH', '')
    feitos = [0]
    lock = threading.Lock()

    def compilar(fn):
        r = subprocess.run([gcc] + CFLAGS + ['-c', fn, '-o', fn + '.o'], cwd=out_c, env=env,
                           capture_output=True, text=True, creationflags=sem_janela)
        if r.returncode:
            raise Erro(T(f'O compilador falhou em {fn}:', f'The compiler failed on {fn}:') + f'\n{r.stderr[-2000:]}')
        with lock:
            feitos[0] += 1
            progresso(0.46 + 0.46 * feitos[0] / len(fontes), T(f'Compilando: {feitos[0]} de {len(fontes)} partes', f'Compiling: {feitos[0]} of {len(fontes)} parts'))

    workers = max(1, min(len(fontes), 8, (os.cpu_count() or 4) - 1))   # (at most 8: the PC stays usable)
    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
        list(ex.map(compilar, fontes))
    progresso(0.93, T('Ligando a biblioteca nativa...', 'Linking the native library...'))
    objs = [f + '.o' for f in fontes]
    r = subprocess.run([gcc, '-shared', '-s', '-o', os.path.join(dest, info['dll'])] + objs,
                       cwd=out_c, env=env, capture_output=True, text=True, creationflags=sem_janela)
    if r.returncode:
        raise Erro(T('A ligação falhou:', 'Linking failed:') + f'\n{r.stderr[-2000:]}')
    log(T('Biblioteca nativa compilada: ', 'Native library compiled: ') + info['dll'])

    # 5. the program (shared by the four games), this game's list, its shortcut; clean up
    progresso(0.97, T('Copiando o programa...', 'Copying the program...'))
    src_exe = os.path.join(RAIZ, 'bin', EXE)
    if not os.path.exists(src_exe):
        raise Erro(T(f'O programa {EXE} não está em {os.path.join(RAIZ, "bin")}.', f'The program {EXE} is not in {os.path.join(RAIZ, "bin")}.'))
    shutil.copy2(src_exe, os.path.join(dest, EXE))
    launcher = os.path.join(RAIZ, 'bin', 'Pokemon Alola PC.exe')   # the launcher: the four games in one window
    if os.path.exists(launcher):
        shutil.copy2(launcher, os.path.join(dest, 'Pokemon Alola PC.exe'))
    # (22/11) the game's button icons in the controller's style (Xbox / PlayStation), used only with a controller connected
    icones = os.path.join(RAIZ, 'bin', 'icones_controle')
    if os.path.isdir(icones):
        shutil.copytree(icones, os.path.join(dest, 'icones_controle'), dirs_exist_ok=True)
    lista = f'recomp_desligar{info["suf"]}.txt'   # functions that stay emulated (found by the live check)
    if os.path.exists(os.path.join(RAIZ, 'bin', lista)):
        shutil.copy2(os.path.join(RAIZ, 'bin', lista), os.path.join(dest, lista))
    nome_curto = info['nome'].replace('Pokémon ', '')
    # (20/11) the games are opened by the launcher only (Pokemon Alola PC.exe / the desktop shortcut): no .bat per game;
    # one left by an older install goes away
    try:
        os.remove(os.path.join(dest, f'Jogar Pokemon {nome_curto}.bat'))
    except OSError:
        pass
    if CONGELADO:   # the launcher's "Instalar jogo..." opens this Instalador.exe again
        with open(os.path.join(dest, 'instalador_origem.txt'), 'w', encoding='utf-8') as f:
            f.write(sys.executable)
    # (21/11) the desktop shortcut is an extra: a failure there (PowerShell blocked / not found) only warns - it used to
    # fail the whole installation ("Não achei o arquivo: None") and the clean-up removed part of what was done
    try:
        atalho_area_de_trabalho(dest, log)
    except Exception as e:
        log(T(f'Aviso: não consegui criar o atalho na Área de Trabalho ({e}). Abra o jogo pelo "Pokemon Alola PC.exe" na pasta instalada.',
               f'Warning: could not create the desktop shortcut ({e}). Open the game with "Pokemon Alola PC.exe" in the install folder.'))
    shutil.rmtree(comp, ignore_errors=True)
    with open(os.path.join(dest, 'instalacao.txt'), 'a', encoding='utf-8') as f:
        f.write(f'{info["nome"]} PC (recompilation test): installed on {time.strftime("%d/%m/%Y %H:%M")}\n'
                f'  jogo: jogo\\{info["cxi"]} ({total / 2**30:.2f} GB) | nativo: {info["dll"]} | open with the launcher (Pokemon Alola PC.exe)\n')
    progresso(1.0, T('Pronto!', 'Done!'))
    log(T(f'Instalação concluída em {(time.time() - t0) / 60:.1f} min. Para jogar: abra o "Pokemon Alola PC" (atalho na Área de Trabalho) e escolha {info["nome"]}.',
           f'Installation finished in {(time.time() - t0) / 60:.1f} min. To play: open "Pokemon Alola PC" (desktop shortcut) and choose {info["nome"]}.'))


def atalho_area_de_trabalho(dest, log):
    """'Pokemon Alola PC' on the desktop, pointing at the launcher"""
    launcher = os.path.join(dest, 'Pokemon Alola PC.exe')
    if not os.path.exists(launcher) or os.environ.get('INSTALADOR_SEM_ATALHO'):
        return
    ps = ('$d=[Environment]::GetFolderPath("Desktop"); $l=Join-Path $d "Pokemon Alola PC.lnk"; '
          '$s=(New-Object -ComObject WScript.Shell).CreateShortcut($l); $s.TargetPath=$env:ALVO; '
          '$s.WorkingDirectory=$env:PASTA; $s.Save()')
    env = dict(os.environ, ALVO=launcher, PASTA=dest)
    psexe = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')
    if not os.path.exists(psexe):
        psexe = 'powershell'
    r = subprocess.run([psexe, '-NoProfile', '-NonInteractive', '-Command', ps], env=env, capture_output=True,
                       text=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    log(T('Atalho "Pokemon Alola PC" criado na Área de Trabalho.', '"Pokemon Alola PC" shortcut created on the desktop.') if r.returncode == 0
        else T('Não consegui criar o atalho na Área de Trabalho.', 'Could not create the desktop shortcut.'))


def limpar(dest):
    """after a failed install: the half copy and the build folder (1-4 GB) go away"""
    try:
        jogo = os.path.join(dest, 'jogo')
        if os.path.isdir(jogo):
            for f in os.listdir(jogo):
                if f.endswith('.parcial'):
                    os.remove(os.path.join(jogo, f))
        shutil.rmtree(os.path.join(dest, 'compilacao'), ignore_errors=True)
    except OSError:
        pass


def instalar_seguro(rom, dest, log, progresso):
    """instalar() with messages a person understands (no traceback) and the leftovers cleaned on failure"""
    try:
        instalar(rom, dest, log, progresso)
    except Erro:
        limpar(dest)
        raise
    except FileNotFoundError as e:
        limpar(dest)
        raise Erro(T('Não achei o arquivo: ', 'File not found: ') + str(e.filename))
    except PermissionError as e:
        limpar(dest)
        raise Erro(T(f'Sem permissão para usar "{e.filename}". Se o jogo ou o launcher estiver aberto, feche e tente de novo; '
                   'se não, escolha outra pasta (por exemplo, dentro de Documentos).',
                   f'No permission to use "{e.filename}". If the game or the launcher is open, close it and try again; '
                   'otherwise choose another folder (for example, inside Documents).'))
    except OSError as e:
        limpar(dest)
        raise Erro(T('Erro ao ler ou gravar arquivos: ', 'Error reading or writing files: ') + str(e))


# ------------------------------------------------------------------ window
def janela():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title(T('Instalador — Pokémon Alola PC (teste)', 'Installer — Pokémon Alola PC (test)'))
    root.geometry('640x520')
    root.configure(bg='#1b1530')
    try:
        root.iconbitmap(os.path.join(RAIZ, 'porte_recomp', 'PokemonUM.ico'))
    except Exception:
        pass
    st = ttk.Style()
    st.theme_use('clam')
    st.configure('TLabel', background='#1b1530', foreground='#f2eefc', font=('Segoe UI', 10))
    st.configure('T.TLabel', font=('Segoe UI Semibold', 16), foreground='#f5b13d')
    st.configure('D.TLabel', foreground='#b8add6', font=('Segoe UI', 9))
    st.configure('TButton', font=('Segoe UI', 10), padding=6)
    st.configure('Horizontal.TProgressbar', troughcolor='#2b2347', background='#f5b13d', thickness=14)

    q = queue.Queue()
    rom_v = tk.StringVar()
    dest_v = tk.StringVar(value=os.path.join(os.path.expanduser('~'), 'Pokemon Alola PC') if CONGELADO
                          else os.path.join(RAIZ, 'instalado'))
    etapa_v = tk.StringVar(value='')

    ttk.Label(root, text='Pokémon Sun · Moon · Ultra Sun · Ultra Moon PC', style='T.TLabel').pack(anchor='w', padx=20, pady=(18, 0))
    ttk.Label(root, text=T('Instala o jogo a partir da sua ROM (uma vez só) e compila o código nativo neste PC.\n'
                         'Nada do jogo vem com o porte. Depois de instalado, a ROM não é mais necessária.',
                         'Installs the game from your ROM (only once) and compiles the native code on this PC.\n'
                         'Nothing from the game comes with the port. Once installed, the ROM is no longer needed.'),
              style='D.TLabel', justify='left').pack(anchor='w', padx=20, pady=(2, 14))

    def linha(rot, var, cmd):
        fr = tk.Frame(root, bg='#1b1530')
        fr.pack(fill='x', padx=20, pady=4)
        ttk.Label(fr, text=rot, width=18).pack(side='left')
        tk.Entry(fr, textvariable=var, bg='#2b2347', fg='#f2eefc', insertbackground='#f2eefc', relief='flat',
                 font=('Segoe UI', 9)).pack(side='left', fill='x', expand=True, ipady=4)
        ttk.Button(fr, text=T('Escolher...', 'Browse...'), command=cmd).pack(side='left', padx=(6, 0))

    def escolher_rom():
        p = filedialog.askopenfilename(title=T('Escolha a sua ROM de Pokémon Sun, Moon, Ultra Sun ou Ultra Moon (descriptografada)',
                                               'Choose your Pokémon Sun, Moon, Ultra Sun or Ultra Moon ROM (decrypted)'),
                                       filetypes=[(T('ROM de 3DS', '3DS ROM'), '*.3ds *.cci'), (T('Todos os arquivos', 'All files'), '*.*')])
        if p:
            rom_v.set(os.path.normpath(p))

    def escolher_dest():
        p = filedialog.askdirectory(title=T('Onde instalar', 'Where to install'))
        if p:
            p = os.path.normpath(p)
            dest_v.set(p if os.path.basename(p).lower() == 'pokemon alola pc' else os.path.join(p, 'Pokemon Alola PC'))

    linha(T('ROM do jogo', 'Game ROM'), rom_v, escolher_rom)
    linha(T('Instalar em', 'Install to'), dest_v, escolher_dest)
    info = ttk.Label(root, text=T(f'Precisa de cerca de {PRECISA_GB:.0f} GB livres durante a instalação (3,6 GB depois).',
                            f'Needs about {PRECISA_GB:.0f} GB free during the installation (3.6 GB afterwards).'),
                     style='D.TLabel')
    info.pack(anchor='w', padx=20, pady=(4, 10))

    barra = ttk.Progressbar(root, maximum=1000, style='Horizontal.TProgressbar')
    barra.pack(fill='x', padx=20)
    ttk.Label(root, textvariable=etapa_v).pack(anchor='w', padx=20, pady=(6, 4))
    caixa = tk.Text(root, height=9, bg='#120e22', fg='#cfc6ea', relief='flat', font=('Consolas', 9), state='disabled')
    caixa.pack(fill='both', expand=True, padx=20, pady=(0, 10))
    botoes = tk.Frame(root, bg='#1b1530')
    botoes.pack(fill='x', padx=20, pady=(0, 16))
    b_inst = ttk.Button(botoes, text=T('Instalar', 'Install'))
    b_inst.pack(side='right')
    b_jogar = ttk.Button(botoes, text=T('Abrir o jogo', 'Open the game'), state='disabled')
    b_jogar.pack(side='right', padx=8)

    def escrever(t):
        caixa.configure(state='normal')
        caixa.insert('end', t + '\n')
        caixa.see('end')
        caixa.configure(state='disabled')

    def trabalho(rom, dest):
        try:
            instalar_seguro(rom, dest, lambda t: q.put(('log', t)), lambda p, t: q.put(('prog', p, t)))
            q.put(('fim', dest))
        except Erro as e:
            q.put(('erro', str(e)))
        except Exception:
            q.put(('erro', T('Erro inesperado:\n', 'Unexpected error:\n') + traceback.format_exc()[-1500:]))

    def iniciar():
        if not rom_v.get():
            messagebox.showwarning(T('Instalador', 'Installer'), T('Escolha a ROM primeiro.', 'Choose the ROM first.'))
            return
        try:
            conferir_rom(rom_v.get())
        except (Erro, OSError) as e:
            messagebox.showerror(T('Instalador', 'Installer'), str(e))
            return
        b_inst.configure(state='disabled')
        rodando[0] = True
        threading.Thread(target=trabalho, args=(rom_v.get(), dest_v.get()), daemon=True).start()

    def abrir():   # the launcher (the four games); the port's exe if it is missing
        l = os.path.join(dest_v.get(), 'Pokemon Alola PC.exe')
        subprocess.Popen([l if os.path.exists(l) else os.path.join(dest_v.get(), EXE)], cwd=dest_v.get())

    rodando = [False]

    def fechar():
        if rodando[0] and not messagebox.askyesno(T('Instalador', 'Installer'), T('A instalação ainda não terminou. Fechar mesmo assim?\n'
                                                  '(o que já foi copiado é apagado; dá para instalar de novo depois)',
                                                  'The installation has not finished yet. Close anyway?\n'
                                                  '(what was copied so far is deleted; you can install again later)')):
            return
        if rodando[0]:
            limpar(dest_v.get())
        root.destroy()
    root.protocol('WM_DELETE_WINDOW', fechar)
    b_inst.configure(command=iniciar)
    b_jogar.configure(command=abrir)

    def bomba():
        try:
            while True:
                m = q.get_nowait()
                if m[0] == 'log':
                    escrever(m[1])
                elif m[0] == 'prog':
                    barra['value'] = int(m[1] * 1000)
                    etapa_v.set(m[2])
                elif m[0] in ('fim', 'erro'):
                    rodando[0] = False
                if m[0] == 'fim':
                    escrever(T('Pronto! A ROM original já pode ser guardada ou apagada.', 'Done! The original ROM can now be stored away or deleted.'))
                    b_jogar.configure(state='normal')
                elif m[0] == 'erro':
                    escrever(T('ERRO: ', 'ERROR: ') + m[1])
                    etapa_v.set(T('A instalação não terminou.', 'The installation did not finish.'))
                    b_inst.configure(state='normal')
                    messagebox.showerror(T('Instalador', 'Installer'), m[1])
        except queue.Empty:
            pass
        root.after(100, bomba)

    bomba()
    if os.environ.get('INSTALADOR_TESTE'):   # test aid: build the window hidden, then close (nothing shown)
        root.withdraw()
        root.after(500, root.destroy)
    root.mainloop()


def rodar_ferramenta():
    """Instalador.exe --ferramenta <extrair|gerar> args: runs the translation tool packed inside the exe"""
    import runpy
    if sys.stdout is None:   # (window exe: no console)
        sys.stdout = open(os.devnull, 'w')
    if sys.stderr is None:
        sys.stderr = sys.stdout
    nome = sys.argv[2]
    sys.argv = [nome + '.py'] + sys.argv[3:]
    runpy.run_module(nome, run_name='__main__', alter_sys=True)


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--ferramenta':
        rodar_ferramenta()
        sys.exit(0)
    if len(sys.argv) >= 4 and sys.argv[1] == '--auto':
        ult = [0.0]

        def prog(p, t):
            if p - ult[0] >= 0.05 or p >= 1:
                ult[0] = p
                print(f'[{p * 100:5.1f}%] {t}', flush=True)
        try:
            instalar_seguro(sys.argv[2], sys.argv[3], lambda t: print(t, flush=True), prog)
        except Erro as e:
            print('ERRO:', e)
            sys.exit(1)
    else:
        janela()
