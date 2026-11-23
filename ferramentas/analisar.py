"""Estudo do código da Ultra Moon para uma recompilação estática (ARM11 -> C).
Lê o que extrair.py gravou e escreve estudo/relatorio_etapa1.md + estudo/numeros.json.

uso: python -I analisar.py <pasta estudo/extraido> <pasta estudo>
"""
import collections
import json
import os
import re
import struct
import sys

import capstone

COND = r'(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?'
RE_BL = re.compile(r'^bl' + COND + r'$')
RE_BLX = re.compile(r'^blx' + COND + r'$')
RE_BX = re.compile(r'^bx' + COND + r'$')
REGS = {'r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12',
        'sb', 'sl', 'fp', 'ip', 'sp', 'lr'}


def scan(code: bytes, base: int, text_lo: int, text_hi: int, thumb=False):
    """Varredura linear. Devolve contadores e o conjunto de destinos de chamadas diretas."""
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
    md.skipdata = True
    c = collections.Counter()
    arm_targets, thumb_targets = set(), set()
    svcs = collections.Counter()
    for _addr, size, mn, op in md.disasm_lite(code, base):
        c['instr'] += 1
        if mn == '.byte' or mn == '.long' or mn == '.short':
            c['nao_decodificado'] += 1
            continue
        if mn.startswith('v') or mn.startswith('fmx') or mn.startswith('fld') or mn.startswith('fst'):
            c['vfp'] += 1
        if RE_BL.match(mn) and op.startswith('#'):
            t = int(op[1:], 16)
            if text_lo <= t < text_hi:
                (thumb_targets if thumb else arm_targets).add(t)
            c['bl_direto'] += 1
        elif RE_BLX.match(mn):
            if op.startswith('#'):
                t = int(op[1:], 16)
                (arm_targets if thumb else thumb_targets).add(t)   # blx imm troca de modo
                c['blx_direto_troca_modo'] += 1
            elif op in REGS and op != 'lr':
                c['indireto_blx_reg'] += 1          # chamada por ponteiro (funções virtuais, callbacks)
        elif RE_BX.match(mn):
            if op == 'lr':
                c['retorno_bx_lr'] += 1
            else:
                c['indireto_bx_reg'] += 1           # salto por registrador (tail call / ponteiro)
        elif (mn.startswith('pop') or mn.startswith('ldm')) and 'pc' in op:
            c['retorno_pop_pc'] += 1
        elif mn.startswith('ldr') and op.startswith('pc,'):
            if '[pc, r' in op and 'lsl #2' in op:
                c['tabela_switch'] += 1             # ldrls pc, [pc, rN, lsl #2]: tabela de switch (resolvível)
            else:
                c['indireto_ldr_pc'] += 1
        elif mn.startswith('add') and op.startswith('pc,'):
            c['tabela_switch_add'] += 1
        elif mn.startswith('mov') and op.startswith('pc,'):
            if op == 'pc, lr':
                c['retorno_mov_pc_lr'] += 1
            else:
                c['indireto_mov_pc'] += 1
        elif mn.startswith('svc') or mn.startswith('swi'):
            svcs[op] += 1
            c['svc'] += 1
    return c, arm_targets, thumb_targets, svcs


def code_pointers(blob: bytes, lo: int, hi: int):
    """Palavras de 4 bytes nos dados que apontam para dentro do código (tabelas de funções)."""
    n = 0
    runs = 0
    run = 0
    for i in range(0, len(blob) - 3, 4):
        v = struct.unpack_from('<I', blob, i)[0]
        if lo <= v < hi:
            n += 1
            run += 1
        else:
            if run >= 3:
                runs += 1   # 3+ seguidos: provavelmente uma vtable
            run = 0
    return n, runs


def cro_info(path: str):
    d = open(path, 'rb').read()
    if len(d) < 0x138 or d[0x80:0x84] not in (b'CRO0', b'CRS0'):
        return None
    u = lambda o: struct.unpack_from('<I', d, o)[0]
    name_off, name_sz = u(0xC0), u(0xC4)
    info = {
        'arquivo': os.path.basename(path),
        'tipo': d[0x80:0x84].decode(),
        'nome': d[name_off:name_off + name_sz].rstrip(b'\0').decode(errors='replace') if name_sz else '',
        'tamanho': len(d),
        'codigo_off': u(0xB0), 'codigo_tam': u(0xB4),
        'dados_tam': u(0xBC),
        'exports_nome': u(0xD4), 'exports_indice': u(0xDC),
        'modulos_importados': u(0xF4),
        'imports_nome': u(0x104), 'imports_indice': u(0x10C), 'imports_anonimos': u(0x114),
        'relocacoes_externas': u(0xFC), 'relocacoes_internas': u(0x12C),
    }
    return info, d


def main():
    src, out = sys.argv[1], sys.argv[2]
    exh = open(os.path.join(src, 'exheader.bin'), 'rb').read()
    text_addr, _, text_size = struct.unpack_from('<III', exh, 0x10)
    ro_addr, ro_pages, ro_size = struct.unpack_from('<III', exh, 0x20)
    data_addr, data_pages, data_size = struct.unpack_from('<III', exh, 0x30)
    code = open(os.path.join(src, 'code.bin'), 'rb').read()
    # code.bin = .text, .rodata, .data, cada um em páginas de 4 KB
    text = code[:text_size]
    ro_off = (text_size + 0xFFF) & ~0xFFF
    rodata = code[ro_off:ro_off + ro_size]
    data_off = ro_off + ((ro_size + 0xFFF) & ~0xFFF)
    data = code[data_off:data_off + data_size]
    lo, hi = text_addr, text_addr + text_size

    print('varrendo o programa principal (ARM)...', flush=True)
    c, arm_t, thumb_t, svcs = scan(text, text_addr, lo, hi)
    p_ro = code_pointers(rodata, lo, hi)
    p_da = code_pointers(data, lo, hi)
    main_res = {
        'texto_bytes': text_size, 'rodata_bytes': ro_size, 'data_bytes': data_size,
        'contagem': dict(c),
        'funcoes_arm_chamadas_diretamente': len(arm_t),
        'funcoes_thumb_alvo_de_blx': len(thumb_t),
        'svc_por_numero': dict(svcs.most_common()),
        'ponteiros_codigo_rodata': p_ro[0], 'vtables_provaveis_rodata': p_ro[1],
        'ponteiros_codigo_data': p_da[0], 'vtables_provaveis_data': p_da[1],
    }

    print('lendo os módulos...', flush=True)
    mods = []
    mdir = os.path.join(src, 'modulos')
    tot = collections.Counter()
    mod_funcs = 0
    for f in sorted(os.listdir(mdir)):
        r = cro_info(os.path.join(mdir, f))
        if not r:
            mods.append({'arquivo': f, 'tipo': 'outro', 'tamanho': os.path.getsize(os.path.join(mdir, f))})
            continue
        info, d = r
        if info['codigo_tam']:
            seg = d[info['codigo_off']:info['codigo_off'] + info['codigo_tam']]
            mc, ma, mt, _ = scan(seg, 0, 0, info['codigo_tam'])
            info['instrucoes'] = mc['instr']
            info['funcoes'] = len(ma) + len(mt)
            info['indiretos'] = mc['indireto_blx_reg'] + mc['indireto_bx_reg'] + mc['indireto_ldr_pc'] + mc['indireto_mov_pc']
            mod_funcs += info['funcoes']
            tot.update(mc)
        mods.append(info)

    res = {'principal': main_res, 'modulos': mods, 'modulos_total': dict(tot), 'modulos_funcoes': mod_funcs}
    json.dump(res, open(os.path.join(out, 'numeros.json'), 'w', encoding='utf-8'), indent=1, ensure_ascii=False)

    # relatório
    mc = main_res['contagem']
    ind_main = mc.get('indireto_blx_reg', 0) + mc.get('indireto_bx_reg', 0) + mc.get('indireto_ldr_pc', 0) + mc.get('indireto_mov_pc', 0)
    ind_mod = sum(tot.get(k, 0) for k in ('indireto_blx_reg', 'indireto_bx_reg', 'indireto_ldr_pc', 'indireto_mov_pc'))
    cros = [m for m in mods if m.get('tipo') in ('CRO0', 'CRS0')]
    L = []
    L.append('# Etapa 1 — estudo da Ultra Moon para recompilação estática\n')
    L.append('## Programa principal (code.bin, "momiji")\n')
    L.append(f'- Código executável: {text_size / 2**20:.2f} MB ({mc["instr"]:,} instruções na varredura linear; {mc.get("nao_decodificado", 0):,} palavras não decodificadas = dados no meio do código)')
    L.append(f'- Funções ARM chamadas diretamente: **{len(arm_t):,}** · funções Thumb (alvo de BLX): **{len(thumb_t):,}**')
    L.append(f'- Chamadas diretas (BL): {mc.get("bl_direto", 0):,}')
    L.append(f'- Retornos (fáceis): bx lr {mc.get("retorno_bx_lr", 0):,} · pop/ldm pc {mc.get("retorno_pop_pc", 0):,} · mov pc, lr {mc.get("retorno_mov_pc_lr", 0):,}')
    L.append(f'- Tabelas de switch (resolvíveis): {mc.get("tabela_switch", 0) + mc.get("tabela_switch_add", 0):,}')
    L.append(f'- **Saltos indiretos de verdade: {ind_main:,}** (blx reg {mc.get("indireto_blx_reg", 0):,} · bx reg {mc.get("indireto_bx_reg", 0):,} · ldr pc {mc.get("indireto_ldr_pc", 0):,} · mov pc {mc.get("indireto_mov_pc", 0):,})')
    L.append(f'- Instruções de ponto flutuante (VFP): {mc.get("vfp", 0):,}')
    L.append(f'- Chamadas ao sistema (SVC): {mc.get("svc", 0):,} · por número: ' + ', '.join(f'{k}×{v}' for k, v in list(svcs.most_common())[:12]))
    L.append(f'- Ponteiros para código nos dados: .rodata {p_ro[0]:,} (≈{p_ro[1]:,} tabelas) · .data {p_da[0]:,} (≈{p_da[1]:,} tabelas)\n')
    L.append(f'## Módulos (.cro / .crs): {len(cros)} de {len(mods)} arquivos\n')
    L.append(f'- Código somado: {sum(m.get("codigo_tam", 0) for m in cros) / 2**20:.2f} MB · funções: **{mod_funcs:,}** · saltos indiretos: **{ind_mod:,}**')
    L.append(f'- Relocações internas: {sum(m.get("relocacoes_internas", 0) for m in cros):,} · externas: {sum(m.get("relocacoes_externas", 0) for m in cros):,}\n')
    L.append('| módulo | código KB | funções | indiretos | exports | imports | relocações |')
    L.append('|---|---:|---:|---:|---:|---:|---:|')
    for m in sorted(cros, key=lambda m: -m.get('codigo_tam', 0))[:40]:
        imp = m['imports_nome'] + m['imports_indice'] + m['imports_anonimos']
        L.append(f'| {m["nome"] or m["arquivo"]} | {m["codigo_tam"] // 1024} | {m.get("funcoes", 0):,} | {m.get("indiretos", 0):,} | '
                 f'{m["exports_nome"] + m["exports_indice"]} | {imp} | {m["relocacoes_internas"] + m["relocacoes_externas"]:,} |')
    open(os.path.join(out, 'relatorio_etapa1.md'), 'w', encoding='utf-8').write('\n'.join(L) + '\n')
    print('\n'.join(L[:16]))


if __name__ == '__main__':
    main()
