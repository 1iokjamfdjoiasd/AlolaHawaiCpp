"""Acha, sem recompilar, qual função nova derruba o jogo: busca binária pela lista de desligar.

uso: python -I bissetar_lista.py <tabela.c nova> <tabela.c base> <pasta bin> <dados de teste> <pasta de trabalho> [segundos]

As funções da tabela nova que não estão na base são as suspeitas. Em cada passo, liga só metade
delas (a outra metade vai para bin\\recomp_desligar.txt, junto com a lista base) e roda o jogo
oculto alguns segundos: se cair, a culpada está na metade ligada. No fim deixa a culpada na lista.
"""
import os
import re
import shutil
import subprocess
import sys


def entries(tab):
    t = open(tab, encoding='utf-8', errors='replace').read()
    i = t.index('recomp_addr[]')
    main = {'%08X' % int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{8})u', t[i:t.index('};', i)])}
    names = re.findall(r'"([^"]+)"', t[t.index('recomp_mod_name[]'):].split('};', 1)[0])
    mods = set()
    for k, n in enumerate(names):
        m = re.search(r'static const uint32_t mod%d_off\[\] = \{([^}]*)\}' % k, t)
        if m:
            mods |= {'%s+%X' % (n, int(x, 16)) for x in re.findall(r'0x([0-9A-Fa-f]+)u', m.group(1))}
    return main | mods


def run(binp, dados, trab, seg, n):
    d = os.path.join(trab, 'bis_dados')
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(dados, d)
    env = dict(os.environ, PORTE_RECOMP='1', PORTE_HIDDEN='1', PORTE_DATA=d, PORTE_SECONDS=str(seg), PORTE_PATHAT='3',
               PORTE_PATH='W9000,A400,W4000,A400,W4000,A400,W4000,A400,W6000,L1500,W500,R1500')
    log = os.path.join(trab, 'bis_%02d.log' % n)
    with open(log, 'wb') as f:
        try:
            subprocess.run([os.path.join(binp, 'PokemonUltraMoon-Recomp.exe')], cwd=binp, env=env, stdout=f, stderr=subprocess.STDOUT,
                           timeout=seg + 40)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill', '/F', '/IM', 'PokemonUltraMoon-Recomp.exe'], capture_output=True)
            return True   # hung: as bad as a crash
    # it also counts as bad when the main loop stopped early (a hang that ended by itself)
    txt = open(log, 'rb').read()
    if b't=%ds' % (seg - 3) not in txt and b't=%ds' % (seg - 2) not in txt and b't=%ds' % (seg - 1) not in txt:
        return True
    return 'A emulação parou'.encode('utf-8') in open(log, 'rb').read()


def main():
    nova, base, binp, dados, trab = sys.argv[1:6]
    seg = int(sys.argv[6]) if len(sys.argv) > 6 else 20
    os.makedirs(trab, exist_ok=True)
    lista = os.path.join(binp, 'recomp_desligar.txt')
    keep = [l.strip() for l in open(lista, encoding='utf-8') if l.strip()]
    sus = sorted(entries(nova) - entries(base) - set(keep))
    print(len(sus), 'suspeitas')

    def with_on(on):
        off = set(sus) - set(on)
        open(lista, 'w', encoding='utf-8').write(''.join(x + '\n' for x in sorted(set(keep) | off)))

    with_on([])
    n = 0
    if run(binp, dados, trab, seg, n):
        print('cai mesmo com todas as suspeitas desligadas: a culpa não está nelas')
        open(lista, 'w', encoding='utf-8').write(''.join(x + '\n' for x in keep))
        return
    with_on(sus)
    n += 1
    if not run(binp, dados, trab, seg, n):
        print('com todas ligadas também não caiu (o erro não se repetiu)')
        open(lista, 'w', encoding='utf-8').write(''.join(x + '\n' for x in keep))
        return
    cand = sus
    while len(cand) > 1:
        half = cand[:len(cand) // 2]
        with_on(half)
        n += 1
        caiu = run(binp, dados, trab, seg, n)
        print('  ligadas %d de %d -> %s' % (len(half), len(cand), 'CAIU' if caiu else 'ok'))
        cand = half if caiu else cand[len(cand) // 2:]
    print('culpada:', cand[0])
    open(lista, 'w', encoding='utf-8').write(''.join(x + '\n' for x in sorted(set(keep) | {cand[0]})))


main()
