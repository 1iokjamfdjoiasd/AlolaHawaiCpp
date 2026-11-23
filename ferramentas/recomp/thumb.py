"""Tradutor Thumb (Thumb-1 do ARMv6, o do ARM11 do 3DS) -> C, no mesmo formato do recomp2.

Cada função Thumb vira ft_XXXXXXXX(Cpu*). O PC do Thumb lê o endereço + 4; as leituras
relativas ao PC usam Align(PC, 4). Os endereços em que o C devolve o controle (s->miss)
levam o bit 0 ligado (= continuar em Thumb).
"""
import struct

from recomp import COND_C, Unsupported

CALL_T = '@@CALLT_{:08X}@@'   # call to a Thumb function
CALL_A = '@@CALL_{:08X}@@'    # call to an ARM function (resolved like recomp2)


def nzf(x):
    return f's->n = {x} >> 31; s->z = {x} == 0;'


def add_flags(a, b, cin, sub):
    """linhas C de uma soma/subtração com flags (res fica em 'res')"""
    if sub:
        c_in = '1' if cin == '0' else cin
        return [f'{{ uint32_t a = {a}, b = {b}; uint64_t t = (uint64_t)a + (uint32_t)~b + {c_in}; uint32_t res = (uint32_t)t;',
                '  ' + nzf('res') + ' s->c = (uint32_t)(t >> 32); s->v = ((a ^ b) & (a ^ res)) >> 31;']
    return [f'{{ uint32_t a = {a}, b = {b}; uint64_t t = (uint64_t)a + b + {cin}; uint32_t res = (uint32_t)t;',
            '  ' + nzf('res') + ' s->c = (uint32_t)(t >> 32); s->v = (~(a ^ b) & (a ^ res)) >> 31;']


def shift_imm(kind, rd, rm, imm):
    """lsl/lsr/asr #imm com flags (formato 1)"""
    if kind == 0:   # lsl
        if imm == 0:
            return [f'r[{rd}] = r[{rm}]; ' + nzf(f'r[{rd}]')]
        return [f'{{ uint32_t m = r[{rm}]; s->c = (m >> {32 - imm}) & 1; r[{rd}] = m << {imm}; ' + nzf(f'r[{rd}]') + ' }']
    if kind == 1:   # lsr (#0 = #32)
        if imm == 0:
            return [f'{{ uint32_t m = r[{rm}]; s->c = m >> 31; r[{rd}] = 0; ' + nzf(f'r[{rd}]') + ' }']
        return [f'{{ uint32_t m = r[{rm}]; s->c = (m >> {imm - 1}) & 1; r[{rd}] = m >> {imm}; ' + nzf(f'r[{rd}]') + ' }']
    if imm == 0:    # asr #32
        return [f'{{ uint32_t m = r[{rm}]; s->c = m >> 31; r[{rd}] = (uint32_t)((int32_t)m >> 31); ' + nzf(f'r[{rd}]') + ' }']
    return [f'{{ uint32_t m = r[{rm}]; s->c = (m >> {imm - 1}) & 1; r[{rd}] = (uint32_t)((int32_t)m >> {imm}); ' + nzf(f'r[{rd}]') + ' }']


def shift_reg(kind, rd, rs):
    """lsl/lsr/asr/ror por registrador (formato 4), com flags"""
    pre = f'{{ uint32_t m = r[{rd}], sh = r[{rs}] & 0xFF;'
    if kind == 'lsl':
        body = ' if (sh) { s->c = sh <= 32 ? (m >> (32 - sh)) & 1 : 0; m = sh < 32 ? m << sh : 0; }'
    elif kind == 'lsr':
        body = ' if (sh) { s->c = sh <= 32 ? (m >> (sh - 1)) & 1 : 0; m = sh < 32 ? m >> sh : 0; }'
    elif kind == 'asr':
        body = ' if (sh) { s->c = sh < 32 ? (m >> (sh - 1)) & 1 : m >> 31; m = sh < 32 ? (uint32_t)((int32_t)m >> sh) : (uint32_t)((int32_t)m >> 31); }'
    else:
        body = ' if (sh) { uint32_t k = sh & 31; if (k) m = (m >> k) | (m << (32 - k)); s->c = m >> 31; }'
    return [pre + body + f' r[{rd}] = m; ' + nzf('m') + ' }']


def jump_any_t(expr, entry_check=True):
    if expr in ('r[14]', 'r[14] | 1u') and entry_check:
        return ['{ uint32_t jt = r[14]; if (jt == entry_lr) return; s->fault = 4; s->miss = jt; return; }']
    lines = [f'{{ uint32_t jt = {expr};']
    if entry_check:
        lines.append('  if (jt == entry_lr) return;')
    lines.append('  { uint32_t tl = r[14]; dispatch(s, jt); if (!s->fault && tl != entry_lr) { s->fault = 4; s->miss = tl; } } return; }')
    return lines


def translate_t(code, base, pc, starts_t):
    """uma instrução Thumb em pc. Devolve (linhas, sucessores, tamanho em bytes)."""
    o = pc - base
    h = struct.unpack_from('<H', code, o)[0]
    nxt = pc + 2
    PCV = pc + 4
    top5 = h >> 11

    # 19: BL / BLX (pair of halfwords)
    if top5 == 0x1E:
        h2 = struct.unpack_from('<H', code, o + 2)[0]
        off = ((h & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
        if off & 0x400000:
            off -= 1 << 23
        ret = pc + 4
        if (h2 >> 11) == 0x1F:      # BL: Thumb -> Thumb
            t = (PCV + off) & 0xFFFFFFFF
            return [f'r[14] = 0x{ret | 1:08X}u; {CALL_T.format(t)}; if (s->fault) return;'], [ret], 4
        if (h2 >> 11) == 0x1D:      # BLX: Thumb -> ARM
            t = ((PCV + off) & ~3) & 0xFFFFFFFF
            return [f'r[14] = 0x{ret | 1:08X}u; {CALL_A.format(t)}; if (s->fault) return;'], [ret], 4
        raise Unsupported('bl thumb estranho')
    if top5 in (0x1D, 0x1F):
        raise Unsupported('metade de bl solta')

    # 1: shift by immediate / 2: add-sub
    if (h >> 13) == 0:
        op = (h >> 11) & 3
        if op != 3:
            imm, rm, rd = (h >> 6) & 0x1F, (h >> 3) & 7, h & 7
            return shift_imm(op, rd, rm, imm), [nxt], 2
        i, sub = (h >> 10) & 1, (h >> 9) & 1
        v = f'{(h >> 6) & 7}u' if i else f'r[{(h >> 6) & 7}]'
        rn, rd = (h >> 3) & 7, h & 7
        return add_flags(f'r[{rn}]', v, '0', sub) + [f'  r[{rd}] = res; }}'], [nxt], 2
    # 3: mov/cmp/add/sub imm8
    if (h >> 13) == 1:
        op, rd, imm = (h >> 11) & 3, (h >> 8) & 7, h & 0xFF
        if op == 0:
            return [f'r[{rd}] = {imm}u; s->n = 0; s->z = {1 if imm == 0 else 0};'], [nxt], 2
        if op == 1:
            return add_flags(f'r[{rd}]', f'{imm}u', '0', True) + ['}'], [nxt], 2
        return add_flags(f'r[{rd}]', f'{imm}u', '0', op == 3) + [f'  r[{rd}] = res; }}'], [nxt], 2
    # 4: ALU
    if (h >> 10) == 0x10:
        op, rs, rd = (h >> 6) & 0xF, (h >> 3) & 7, h & 7
        R, S = f'r[{rd}]', f'r[{rs}]'
        if op in (0, 1, 12, 14, 15, 8):
            e = {0: f'{R} & {S}', 1: f'{R} ^ {S}', 12: f'{R} | {S}', 14: f'{R} & ~{S}', 15: f'~{S}', 8: f'{R} & {S}'}[op]
            out = f'{{ uint32_t res = {e}; ' + nzf('res') + (f' r[{rd}] = res;' if op != 8 else '') + ' }'
            return [out], [nxt], 2
        if op in (2, 3, 4, 7):
            return shift_reg({2: 'lsl', 3: 'lsr', 4: 'asr', 7: 'ror'}[op], rd, rs), [nxt], 2
        if op == 5:   # adc
            return add_flags(R, S, 's->c', False) + [f'  r[{rd}] = res; }}'], [nxt], 2
        if op == 6:   # sbc
            return add_flags(R, S, 's->c', True) + [f'  r[{rd}] = res; }}'], [nxt], 2
        if op == 9:   # neg = 0 - rs
            return add_flags('0u', S, '0', True) + [f'  r[{rd}] = res; }}'], [nxt], 2
        if op == 10:  # cmp
            return add_flags(R, S, '0', True) + ['}'], [nxt], 2
        if op == 11:  # cmn
            return add_flags(R, S, '0', False) + ['}'], [nxt], 2
        if op == 13:  # mul
            return [f'r[{rd}] = r[{rd}] * r[{rs}]; ' + nzf(f'r[{rd}]')], [nxt], 2
    # 5: hi registers / bx / blx
    if (h >> 10) == 0x11:
        op = (h >> 8) & 3
        rm = (h >> 3) & 0xF
        rd = (h & 7) | ((h >> 4) & 8)
        vm = f'0x{PCV:08X}u' if rm == 15 else f'r[{rm}]'
        if op == 3:
            if h & 0x80:   # blx reg
                return [f'{{ uint32_t t = {vm}; r[14] = 0x{nxt | 1:08X}u; dispatch(s, t); if (s->fault) return; }}'], [nxt], 2
            return jump_any_t(vm), ['ret'], 2   # (bx lr too: lr may have changed on the way)
        vd = f'0x{PCV:08X}u' if rd == 15 else f'r[{rd}]'
        if op == 1:   # cmp
            return add_flags(vd, vm, '0', True) + ['}'], [nxt], 2
        if op == 0:   # add (no flags)
            if rd == 15:
                return jump_any_t(f'({vd} + {vm}) & ~1u | 1u'), ['ret'], 2
            return [f'r[{rd}] = {vd} + {vm};'], [nxt], 2
        if rd == 15:   # mov pc, rm
            return jump_any_t(f'{vm} | 1u'), ['ret'], 2   # (mov pc, lr too)
        return [f'r[{rd}] = {vm};'], [nxt], 2
    # 6: ldr pc-relative
    if (h >> 11) == 9:
        rd, imm = (h >> 8) & 7, (h & 0xFF) * 4
        return [f'r[{rd}] = rd32(s, 0x{(PCV & ~3) + imm:08X}u);'], [nxt], 2
    # 7/8: register offset
    if (h >> 12) == 5:
        op, ro, rb, rd = (h >> 9) & 7, (h >> 6) & 7, (h >> 3) & 7, h & 7
        ad = f'r[{rb}] + r[{ro}]'
        return [{0: f'wr32(s, {ad}, r[{rd}]);', 1: f'wr16(s, {ad}, r[{rd}]);', 2: f'wr8(s, {ad}, r[{rd}]);',
                 3: f'r[{rd}] = (uint32_t)(int32_t)(int8_t)rd8(s, {ad});', 4: f'r[{rd}] = rd32(s, {ad});',
                 5: f'r[{rd}] = rd16(s, {ad});', 6: f'r[{rd}] = rd8(s, {ad});',
                 7: f'r[{rd}] = (uint32_t)(int32_t)(int16_t)rd16(s, {ad});'}[op]], [nxt], 2
    # 9: word/byte immediate
    if (h >> 13) == 3:
        b, l = (h >> 12) & 1, (h >> 11) & 1
        imm, rb, rd = (h >> 6) & 0x1F, (h >> 3) & 7, h & 7
        ad = f'r[{rb}] + {imm if b else imm * 4}u'
        if l:
            return [f'r[{rd}] = {"rd8" if b else "rd32"}(s, {ad});'], [nxt], 2
        return [f'{"wr8" if b else "wr32"}(s, {ad}, r[{rd}]);'], [nxt], 2
    # 10: halfword immediate
    if (h >> 12) == 8:
        l, imm, rb, rd = (h >> 11) & 1, ((h >> 6) & 0x1F) * 2, (h >> 3) & 7, h & 7
        if l:
            return [f'r[{rd}] = rd16(s, r[{rb}] + {imm}u);'], [nxt], 2
        return [f'wr16(s, r[{rb}] + {imm}u, r[{rd}]);'], [nxt], 2
    # 11: sp-relative
    if (h >> 12) == 9:
        l, rd, imm = (h >> 11) & 1, (h >> 8) & 7, (h & 0xFF) * 4
        if l:
            return [f'r[{rd}] = rd32(s, r[13] + {imm}u);'], [nxt], 2
        return [f'wr32(s, r[13] + {imm}u, r[{rd}]);'], [nxt], 2
    # 12: add rd, pc/sp, #imm
    if (h >> 12) == 0xA:
        sp, rd, imm = (h >> 11) & 1, (h >> 8) & 7, (h & 0xFF) * 4
        if sp:
            return [f'r[{rd}] = r[13] + {imm}u;'], [nxt], 2
        return [f'r[{rd}] = 0x{(PCV & ~3) + imm:08X}u;'], [nxt], 2
    # 13/14 and the ARMv6 misc space (1011 ....)
    if (h >> 12) == 0xB:
        if (h >> 8) == 0xB0:   # add/sub sp, #imm
            imm = (h & 0x7F) * 4
            return [f'r[13] {"-" if h & 0x80 else "+"}= {imm}u;'], [nxt], 2
        if (h & 0x0600) == 0x0400:   # push / pop
            l, rbit = (h >> 11) & 1, (h >> 8) & 1
            regs = [i for i in range(8) if h >> i & 1]
            if l:
                lines = ['{ uint32_t ad = r[13]; uint32_t npc = 0; (void)npc;']
                for i in regs:
                    lines.append(f'  r[{i}] = rd32(s, ad); ad += 4;')
                if rbit:
                    lines.append('  npc = rd32(s, ad); ad += 4;')
                lines.append('  r[13] = ad;')
                if rbit:
                    lines.append('  if (s->fault) return; if (npc != entry_lr) { s->fault = 4; s->miss = npc; } return;')
                lines.append('}')
                return lines, (['ret'] if rbit else [nxt]), 2
            lst = regs + ([14] if rbit else [])
            n = len(lst)
            lines = [f'{{ uint32_t ad = r[13] - {4 * n}u;']
            for i in lst:
                lines.append(f'  wr32(s, ad, r[{i}]); ad += 4;')
            lines.append(f'  r[13] -= {4 * n}u; }}')
            return lines, [nxt], 2
        if (h >> 8) == 0xB2:   # sxth / sxtb / uxth / uxtb
            op, rm, rd = (h >> 6) & 3, (h >> 3) & 7, h & 7
            e = {0: f'(uint32_t)(int32_t)(int16_t)r[{rm}]', 1: f'(uint32_t)(int32_t)(int8_t)r[{rm}]',
                 2: f'(uint32_t)(uint16_t)r[{rm}]', 3: f'(uint32_t)(uint8_t)r[{rm}]'}[op]
            return [f'r[{rd}] = {e};'], [nxt], 2
        if (h >> 8) == 0xBA:   # rev / rev16 / revsh
            op, rm, rd = (h >> 6) & 3, (h >> 3) & 7, h & 7
            if op == 0:
                return [f'r[{rd}] = __builtin_bswap32(r[{rm}]);'], [nxt], 2
            if op == 1:
                return [f'r[{rd}] = ((r[{rm}] & 0x00FF00FFu) << 8) | ((r[{rm}] >> 8) & 0x00FF00FFu);'], [nxt], 2
            if op == 3:
                return [f'r[{rd}] = (uint32_t)(int32_t)(int16_t)(((r[{rm}] & 0xFF) << 8) | ((r[{rm}] >> 8) & 0xFF));'], [nxt], 2
        if h == 0xBF00:
            return ['/* nop */'], [nxt], 2
        raise Unsupported(f'thumb misc {h:04X}')
    # 15: ldmia / stmia
    if (h >> 12) == 0xC:
        l, rb = (h >> 11) & 1, (h >> 8) & 7
        regs = [i for i in range(8) if h >> i & 1]
        if not regs:
            raise Unsupported('ldm thumb vazio')
        lines = [f'{{ uint32_t ad = r[{rb}];']
        for i in regs:
            lines.append(f'  r[{i}] = rd32(s, ad); ad += 4;' if l else f'  wr32(s, ad, r[{i}]); ad += 4;')
        if not (l and rb in regs):
            lines.append(f'  r[{rb}] = ad;')
        lines.append('}')
        return lines, [nxt], 2
    # 16: conditional branch / svc
    if (h >> 12) == 0xD:
        cond = (h >> 8) & 0xF
        if cond == 0xF:
            return [f'if (!host_svc(s, 0x{h & 0xFF:02X}u, 0x{(pc + 2) | 1:08X}u)) {{ if (!s->fault) {{ s->fault = 6; s->miss = 0x{pc | 1:08X}u; }} return; }}'], [pc + 2], 2
        if cond == 0xE:
            raise Unsupported('thumb udf')
        off = h & 0xFF
        if off & 0x80:
            off -= 0x100
        t = (PCV + off * 2) & 0xFFFFFFFF
        return [f'if ({COND_C[cond]}) goto L_{t:08X};'], [t, nxt], 2
    # 18: unconditional branch
    if (h >> 11) == 0x1C:
        off = h & 0x7FF
        if off & 0x400:
            off -= 0x800
        t = (PCV + off * 2) & 0xFFFFFFFF
        if t != pc and t in starts_t:
            return [f'{CALL_T.format(t)}; return;'], ['ret'], 2
        return [f'goto L_{t:08X};'], [t], 2
    raise Unsupported(f'thumb {h:04X}')


def translate_thumb_function(code, base, start, starts_t, max_insns=4000):
    work, out, calls_t, calls_a = [start], {}, set(), set()
    while work:
        a = work.pop()
        if a in out:
            continue
        if not (base <= a < base + len(code) - 1):
            raise Unsupported('thumb fora do código')
        if a != start and a in starts_t:
            out[a] = ([f'{CALL_T.format(a)}; return;   /* cai em outra função */'], 2)
            calls_t.add(a)
            continue
        lines, succ, size = translate_t(code, base, a, starts_t)
        for ln in lines:
            for tok, dst in (('@@CALLT_', calls_t), ('@@CALL_', calls_a)):
                k = ln.find(tok)
                if k >= 0:
                    dst.add(int(ln[k + len(tok):k + len(tok) + 8], 16))
        out[a] = (lines, size)
        if len(out) > max_insns:
            raise Unsupported('thumb grande demais')
        for t in succ:
            if t != 'ret':
                work.append(t)
    name = f'ft_{start:08X}'
    c = [f'void {name}(Cpu* s) {{', '  uint32_t* r = s->r; const uint32_t entry_lr = r[14]; (void)entry_lr;',
         f'  goto L_{start:08X};']
    addrs = sorted(out)
    for i, a in enumerate(addrs):
        lines, size = out[a]
        c.append(f' L_{a:08X}: if (++s->icount > s->ilimit) {{ s->fault = 2; s->miss = 0x{a | 1:08X}u; return; }}')
        c += ['  ' + x for x in lines]
        nx = addrs[i + 1] if i + 1 < len(addrs) else None
        if nx != a + size:
            c.append(f'  goto L_{a + size:08X};' if (a + size) in out else f'  s->fault = 3; s->miss = 0x{a + size | 1:08X}u; return;')
    c.append('}')
    return name, '\n'.join(c), len(out), calls_t, calls_a
