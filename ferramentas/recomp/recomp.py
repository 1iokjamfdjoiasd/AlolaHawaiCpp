"""Tradutor ARM (ARM11 / ARMv6K) -> C, primeira versão (etapa 2, teste).

Traduz as funções "folha" do code.bin (as que não chamam outras funções) para C.
Cada instrução vira um bloco com rótulo; os desvios viram goto. A execução
condicional do ARM vira if (...), e os flags N/Z/C/V são calculados como no
processador. Instruções que esta versão ainda não sabe traduzir fazem a função
ser recusada (no porte híbrido ela continuaria no JIT).

uso: python -I recomp.py <estudo/extraido> <saída: pasta> [máximo de funções]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))


class Unsupported(Exception):
    pass


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


COND_C = {
    0x0: 's->z', 0x1: '!s->z', 0x2: 's->c', 0x3: '!s->c', 0x4: 's->n', 0x5: '!s->n',
    0x6: 's->v', 0x7: '!s->v', 0x8: '(s->c && !s->z)', 0x9: '(!s->c || s->z)',
    0xA: '(s->n == s->v)', 0xB: '(s->n != s->v)', 0xC: '(!s->z && s->n == s->v)',
    0xD: '(s->z || s->n != s->v)',
}


# Addresses in the generated C: absolute (main program) or relative to a module's base
# (a .cro loads where the game puts it): set ADDR_REL while translating a module.
ADDR_REL = False


def AD(x):
    x &= 0xFFFFFFFF
    return f'(MB + 0x{x:X}u)' if ADDR_REL else f'0x{x:08X}u'


def R(n, pc):
    """Valor de um registrador como expressão C (o PC lê o endereço + 8)."""
    return AD(pc + 8) if n == 15 else f'r[{n}]'


class Fn:
    def __init__(self, code, base, start):
        self.code, self.base, self.start = code, base, start
        self.insns = {}   # addr -> word

    def word(self, a):
        o = a - self.base
        if o < 0 or o + 4 > len(self.code):
            raise Unsupported('fora do código')
        return struct.unpack_from('<I', self.code, o)[0]


# ---------------------------------------------------------------- operand 2
def shifter(w, pc, need_carry):
    """Operando 2 de processamento de dados. Devolve (expr valor, expr carry ou None, linhas antes)."""
    pre = []
    if w & (1 << 25):
        rot = ((w >> 8) & 0xF) * 2
        v = ror(w & 0xFF, rot)
        carry = (f'{v >> 31}u' if rot else None)
        return f'0x{v:08X}u', carry, pre
    rm = w & 0xF
    typ = (w >> 5) & 3
    vm = R(rm, pc)
    if w & 0x10:   # shift by register
        if w & 0x80:
            raise Unsupported('não é dp')
        rs = (w >> 8) & 0xF
        if rm == 15 or rs == 15:
            raise Unsupported('pc em shift por registrador')
        pre.append(f'{{ uint32_t m = {vm}, sh = r[{rs}] & 0xFF;')
        if typ == 0:
            pre.append('  op2 = sh == 0 ? m : sh < 32 ? m << sh : 0;')
            pre.append('  c2 = sh == 0 ? s->c : sh <= 32 ? (m >> (32 - sh)) & 1 : 0; }')
        elif typ == 1:
            pre.append('  op2 = sh == 0 ? m : sh < 32 ? m >> sh : 0;')
            pre.append('  c2 = sh == 0 ? s->c : sh <= 32 ? (m >> (sh - 1)) & 1 : 0; }')
        elif typ == 2:
            pre.append('  op2 = sh == 0 ? m : sh < 32 ? (uint32_t)((int32_t)m >> sh) : (uint32_t)((int32_t)m >> 31);')
            pre.append('  c2 = sh == 0 ? s->c : sh < 32 ? (m >> (sh - 1)) & 1 : m >> 31; }')
        else:
            pre.append('  { uint32_t k = sh & 31; op2 = k ? (m >> k) | (m << (32 - k)) : m;')
            pre.append('    c2 = sh == 0 ? s->c : (op2 >> 31); } }')
        return 'op2', 'c2', pre
    amt = (w >> 7) & 0x1F
    if typ == 0:
        if amt == 0:
            return vm, None, pre
        return f'({vm} << {amt})', f'(({vm} >> {32 - amt}) & 1)', pre
    if typ == 1:
        if amt == 0:
            return '0u', f'({vm} >> 31)', pre
        return f'({vm} >> {amt})', f'(({vm} >> {amt - 1}) & 1)', pre
    if typ == 2:
        if amt == 0:
            return f'(uint32_t)((int32_t){vm} >> 31)', f'({vm} >> 31)', pre
        return f'(uint32_t)((int32_t){vm} >> {amt})', f'(({vm} >> {amt - 1}) & 1)', pre
    if amt == 0:   # RRX
        return f'(({vm} >> 1) | (s->c << 31))', f'({vm} & 1)', pre
    return f'(({vm} >> {amt}) | ({vm} << {32 - amt}))', f'(({vm} >> {amt - 1}) & 1)', pre


def nz(x):
    return f's->n = {x} >> 31; s->z = {x} == 0;'


# ---------------------------------------------------------------- one instruction
def translate(w, pc, fn_start):
    """Devolve (linhas C, sucessores) — sucessores: lista de endereços, 'ret' = fim."""
    cond = w >> 28
    nxt = pc + 4
    if cond == 0xF:
        if (w & 0xFD70F000) == 0xF550F000:   # PLD: só uma dica de cache
            return ['/* pld */'], [nxt]
        raise Unsupported('instrução incondicional')
    body, succ = translate_body(w, pc, fn_start)
    if cond == 0xE:
        return body, succ
    # condicional: se falhar, segue para a próxima
    s = list(succ)
    if nxt not in s:
        s.append(nxt)
    return [f'if ({COND_C[cond]}) {{'] + ['  ' + b for b in body] + ['}'], s


def translate_body(w, pc, fn_start):
    nxt = pc + 4
    op = (w >> 25) & 7

    # ---- branch
    if op == 5:
        if w & (1 << 24):
            raise Unsupported('bl (não é folha)')
        off = w & 0xFFFFFF
        if off & 0x800000:
            off -= 1 << 24
        t = (pc + 8 + off * 4) & 0xFFFFFFFF
        return [f'goto L_{t:08X};'], [t]

    # ---- bx / blx reg
    if (w & 0x0FFFFFF0) == 0x012FFF10:
        if (w & 0xF) == 14:
            return ['return;'], ['ret']
        raise Unsupported('bx registrador')
    if (w & 0x0FFFFFF0) == 0x012FFF30:
        raise Unsupported('blx registrador')

    # ---- clz
    if (w & 0x0FFF0FF0) == 0x016F0F10:
        rd, rm = (w >> 12) & 0xF, w & 0xF
        return [f'r[{rd}] = r[{rm}] ? (uint32_t)__builtin_clz(r[{rm}]) : 32u;'], [nxt]

    # ---- multiply
    if (w & 0x0F0000F0) == 0x00000090 and op == 0:
        a, sflag = (w >> 21) & 1, (w >> 20) & 1
        rd, rn, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        if (w >> 22) & 3:
            raise Unsupported('mul desconhecido')
        e = f'r[{rm}] * r[{rs}]' + (f' + r[{rn}]' if a else '')
        out = [f'r[{rd}] = {e};']
        if sflag:
            out.append(nz(f'r[{rd}]'))
        return out, [nxt]
    if (w & 0x0F8000F0) == 0x00800090:
        sgn, a, sflag = (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        hi, lo, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        if sgn:
            e = f'(uint64_t)((int64_t)(int32_t)r[{rm}] * (int64_t)(int32_t)r[{rs}])'
        else:
            e = f'(uint64_t)r[{rm}] * r[{rs}]'
        if a:
            e += f' + (((uint64_t)r[{hi}] << 32) | r[{lo}])'
        out = [f'{{ uint64_t t = {e}; r[{lo}] = (uint32_t)t; r[{hi}] = (uint32_t)(t >> 32);']
        if sflag:
            out.append(f'  s->n = r[{hi}] >> 31; s->z = t == 0;')
        out.append('}')
        return out, [nxt]

    # ---- halfword / signed / doubleword loads and stores
    if op == 0 and (w & 0x90) == 0x90 and (w & 0x60):
        p, u, i, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        sh = (w >> 5) & 3
        if i:
            off = f'{((w >> 4) & 0xF0) | (w & 0xF)}u'
        else:
            if (w & 0xF) == 15:
                raise Unsupported('pc como índice')
            off = f'r[{w & 0xF}]'
        return mem_access(pc, nxt, rn, rd, p, u, wb, off, {
            (1, 1): ('rd16', 'h'), (1, 2): ('rd8s', 'sb'), (1, 3): ('rd16s', 'sh'),
            (0, 1): ('wr16', 'h'), (0, 2): ('ldrd', 'd'), (0, 3): ('strd', 'd')}[(l, sh)])

    # ---- data processing (and the misc space)
    if op in (0, 1):
        opc, sflag = (w >> 21) & 0xF, (w >> 20) & 1
        if 8 <= opc <= 11 and not sflag:
            raise Unsupported('mrs/msr/misc')
        if op == 0 and (w & 0x90) == 0x90:
            raise Unsupported('espaço de extensão')
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        need_c = sflag and opc in (0, 1, 8, 9, 12, 13, 14, 15)
        v2, c2, pre = shifter(w, pc, need_c)
        vn = R(rn, pc) if not (op == 0 and (w & 0x10)) else (R(rn, pc + 4) if rn == 15 else f'r[{rn}]')
        lines = []
        if pre:
            lines.append('{ uint32_t op2, c2;')
            lines += ['  ' + x for x in pre]
        res = None
        if opc in (0, 1, 12, 14, 13, 15, 8, 9):
            ex = {0: f'{vn} & {v2}', 1: f'{vn} ^ {v2}', 12: f'{vn} | {v2}', 14: f'{vn} & ~{v2}',
                  13: f'{v2}', 15: f'~{v2}', 8: f'{vn} & {v2}', 9: f'{vn} ^ {v2}'}[opc]
            lines.append(f'{{ uint32_t res = {ex};')
            if opc not in (8, 9):
                res = 'res'
            if sflag:
                lines.append('  ' + nz('res') + (f' s->c = {c2};' if c2 else ''))
        else:
            # arithmetic: a op b (+ carry)
            a, b, cin, sub = {2: (vn, v2, '0', True), 3: (v2, vn, '0', True), 4: (vn, v2, '0', False),
                              5: (vn, v2, 's->c', False), 6: (vn, v2, 's->c', True), 7: (v2, vn, 's->c', True),
                              10: (vn, v2, '0', True), 11: (vn, v2, '0', False)}[opc]
            lines.append(f'{{ uint32_t a = {a}, b = {b};')
            if sub:
                # a - b - !cin  ==  a + ~b + cin   (cin = 1 for plain SUB/CMP)
                c_in = '1' if cin == '0' else cin
                lines.append(f'  uint64_t t = (uint64_t)a + (uint32_t)~b + {c_in}; uint32_t res = (uint32_t)t;')
                if sflag:
                    lines.append('  ' + nz('res') + ' s->c = (uint32_t)(t >> 32); s->v = ((a ^ b) & (a ^ res)) >> 31;')
            else:
                lines.append(f'  uint64_t t = (uint64_t)a + b + {cin}; uint32_t res = (uint32_t)t;')
                if sflag:
                    lines.append('  ' + nz('res') + ' s->c = (uint32_t)(t >> 32); s->v = (~(a ^ b) & (a ^ res)) >> 31;')
            if opc not in (10, 11):
                res = 'res'
        succ = [nxt]
        if res:
            if rd == 15:
                if sflag:
                    raise Unsupported('movs pc')
                if opc == 13 and (w & 0x02000FFF) == 14 and not (w & 0x10):   # mov pc, lr
                    lines.append('  return;')
                    succ = ['ret']
                else:
                    raise Unsupported('escrita no pc')
            else:
                lines.append(f'  r[{rd}] = res;')
        lines.append('}')
        if pre:
            lines.append('}')
        return lines, succ

    # ---- ARMv6 media: extend (sxtb/uxtb/sxth/uxth, with add) and rev
    if op == 3 and (w & 0x10):
        if (w & 0x0F8003F0) == 0x06800070:
            sub = (w >> 20) & 7
            rn, rd, rot, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, ((w >> 10) & 3) * 8, w & 0xF
            src = f'(r[{rm}] >> {rot} | r[{rm}] << {32 - rot})' if rot else f'r[{rm}]'
            ext = {2: f'(uint32_t)(int32_t)(int8_t){src}', 3: f'(uint32_t)(int32_t)(int16_t){src}',
                   6: f'(uint32_t)(uint8_t){src}', 7: f'(uint32_t)(uint16_t){src}'}.get(sub)
            if not ext:
                raise Unsupported('extensão de 16 bits dupla')
            add = '' if rn == 15 else f'r[{rn}] + '
            return [f'r[{rd}] = {add}{ext};'], [nxt]
        if (w & 0x0FFF0FF0) == 0x06BF0F30:
            rd, rm = (w >> 12) & 0xF, w & 0xF
            return [f'r[{rd}] = __builtin_bswap32(r[{rm}]);'], [nxt]
        raise Unsupported('mídia ARMv6')

    # ---- load / store word or byte
    if op in (2, 3):
        p, u, b, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        if op == 2:
            off = f'{w & 0xFFF}u'
        else:
            if (w & 0xF) == 15:
                raise Unsupported('pc como índice')
            v2, _, pre = shifter(w & ~(1 << 25), pc, False)
            if pre:
                raise Unsupported('shift por registrador no endereço')
            off = v2
        kind = {(1, 0): ('rd32', 'w'), (1, 1): ('rd8', 'b'), (0, 0): ('wr32', 'w'), (0, 1): ('wr8', 'b')}[(l, b)]
        return mem_access(pc, nxt, rn, rd, p, u, wb, off, kind)

    # ---- ldm / stm (push / pop)
    if op == 4:
        p, u, sbit, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, regs = (w >> 16) & 0xF, w & 0xFFFF
        if sbit or rn == 15 or regs == 0:
            raise Unsupported('ldm/stm especial')
        n = bin(regs).count('1')
        lst = [i for i in range(16) if regs >> i & 1]
        if u:
            first = f'r[{rn}] + {4 if p else 0}u'
        else:
            first = f'r[{rn}] - {4 * n if not p else 4 * n}u' if p else f'r[{rn}] - {4 * n - 4}u'
        lines = [f'{{ uint32_t ad = {first}, base = r[{rn}];']
        succ = [pc + 4]
        for i in lst:
            if l:
                if i == 15:
                    lines.append('  { uint32_t npc = rd32(s, ad); ad += 4; (void)npc; ret_pending = 1; }')
                    succ = ['ret']
                else:
                    lines.append(f'  r[{i}] = rd32(s, ad); ad += 4;')
            else:
                v = AD(pc + 12) if i == 15 else ('base' if i == rn else f'r[{i}]')
                lines.append(f'  wr32(s, ad, {v}); ad += 4;')
        if wb and not (l and rn in lst):
            lines.append(f'  r[{rn}] = base {"+" if u else "-"} {4 * n}u;')
        lines.append('}')
        if succ == ['ret']:
            lines.append('if (ret_pending) return;')
        return lines, succ

    if op in (6, 7):
        if (w >> 24) & 0xF == 0xF:
            raise Unsupported('svc')
        raise Unsupported('coprocessador / VFP')
    raise Unsupported(f'desconhecida {w:08X}')


def mem_access(pc, nxt, rn, rd, p, u, wb, off, kind):
    fn, k = kind
    base = R(rn, pc)
    sign = '+' if u else '-'
    addr = f'({base} {sign} {off})' if p else base
    lines = [f'{{ uint32_t ad = {addr};']
    if fn in ('rd32', 'rd8', 'rd16', 'rd8s', 'rd16s'):
        if rd == 15:
            raise Unsupported('ldr pc')
        call = {'rd8s': '(uint32_t)(int32_t)(int8_t)rd8(s, ad)', 'rd16s': '(uint32_t)(int32_t)(int16_t)rd16(s, ad)'}.get(fn, f'{fn}(s, ad)')
        lines.append(f'  uint32_t val = {call};')
    elif fn == 'ldrd':
        if rd & 1 or rd == 14:
            raise Unsupported('ldrd ímpar')
        lines.append('  uint32_t val = rd32(s, ad), val2 = rd32(s, ad + 4);')
    elif fn == 'strd':
        if rd & 1 or rd == 14:
            raise Unsupported('strd ímpar')
        lines.append(f'  wr32(s, ad, r[{rd}]); wr32(s, ad + 4, r[{rd + 1}]);')
    else:
        v = AD(pc + 12) if rd == 15 else f'r[{rd}]'
        lines.append(f'  {fn}(s, ad, {v});')
    if not p:
        if rn == 15:
            raise Unsupported('pós-índice no pc')
        lines.append(f'  r[{rn}] = {base} {sign} {off};')
    elif wb:
        if rn == 15:
            raise Unsupported('writeback no pc')
        lines.append(f'  r[{rn}] = ad;')
    if fn in ('rd32', 'rd8', 'rd16', 'rd8s', 'rd16s'):
        lines.append(f'  r[{rd}] = val;')
    elif fn == 'ldrd':
        lines.append(f'  r[{rd}] = val; r[{rd + 1}] = val2;')
    lines.append('}')
    return lines, [nxt]


# ---------------------------------------------------------------- a whole function
def translate_function(code, base, start, known_starts, max_insns=3000):
    work, seen, out = [start], {}, {}
    while work:
        a = work.pop()
        if a in seen:
            continue
        if a != start and a in known_starts:
            raise Unsupported('cai em outra função (tail call)')
        o = a - base
        if o < 0 or o + 4 > len(code):
            raise Unsupported('fora do código')
        w = struct.unpack_from('<I', code, o)[0]
        lines, succ = translate(w, a, start)
        seen[a] = w
        out[a] = lines
        if len(seen) > max_insns:
            raise Unsupported('grande demais')
        for t in succ:
            if t != 'ret':
                work.append(t)
    name = f'f_{start:08X}'
    c = [f'void {name}(Cpu* s) {{', '  uint32_t* r = s->r; int ret_pending = 0; uint32_t budget = 200000; (void)ret_pending;',
         f'  goto L_{start:08X};   /* the entry: code below it in memory can come first */']
    addrs = sorted(out)
    for i, a in enumerate(addrs):
        c.append(f' L_{a:08X}: if (!--budget) {{ s->fault = 2; return; }}')
        c += ['  ' + x for x in out[a]]
        nx = addrs[i + 1] if i + 1 < len(addrs) else None
        if nx != a + 4:
            # the next instruction in memory is not the next block: jump there or stop
            if (a + 4) in out:
                c.append(f'  goto L_{a + 4:08X};')
            else:
                c.append('  s->fault = 3; return;')
    c.append('}')
    return name, '\n'.join(c), len(out)


RUNTIME_H = r'''
#include <stdint.h>
typedef struct { uint32_t r[16]; uint32_t n, z, c, v; uint32_t fault; } Cpu;
uint8_t rd8(Cpu* s, uint32_t a);
uint16_t rd16(Cpu* s, uint32_t a);
uint32_t rd32(Cpu* s, uint32_t a);
void wr8(Cpu* s, uint32_t a, uint32_t v);
void wr16(Cpu* s, uint32_t a, uint32_t v);
void wr32(Cpu* s, uint32_t a, uint32_t v);
'''

RUNTIME_C = r'''
#include "runtime.h"
// memória do teste: até 4 janelas (endereço do 3DS -> bytes)
typedef struct { uint32_t base, len; uint8_t* p; int writable; } Win;
static Win win[4];
static int nwin;
__declspec(dllexport) void set_window(int i, uint32_t base, uint32_t len, uint8_t* p, int writable)
{ win[i].base = base; win[i].len = len; win[i].p = p; win[i].writable = writable; if (i >= nwin) nwin = i + 1; }
static uint8_t* at(Cpu* s, uint32_t a, uint32_t n, int w)
{
    for (int i = 0; i < nwin; i++)
        if (a - win[i].base < win[i].len && a - win[i].base + n <= win[i].len && (!w || win[i].writable))
            return win[i].p + (a - win[i].base);
    s->fault = 1;
    return 0;
}
uint8_t rd8(Cpu* s, uint32_t a) { uint8_t* p = at(s, a, 1, 0); return p ? p[0] : 0; }
uint16_t rd16(Cpu* s, uint32_t a) { uint8_t* p = at(s, a, 2, 0); return p ? (uint16_t)(p[0] | p[1] << 8) : 0; }
uint32_t rd32(Cpu* s, uint32_t a) { uint8_t* p = at(s, a, 4, 0); return p ? (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) : 0; }
void wr8(Cpu* s, uint32_t a, uint32_t v) { uint8_t* p = at(s, a, 1, 1); if (p) p[0] = (uint8_t)v; }
void wr16(Cpu* s, uint32_t a, uint32_t v) { uint8_t* p = at(s, a, 2, 1); if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); } }
void wr32(Cpu* s, uint32_t a, uint32_t v) { uint8_t* p = at(s, a, 4, 1); if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); } }
'''


def main():
    src, out = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9
    os.makedirs(out, exist_ok=True)
    exh = open(os.path.join(src, 'exheader.bin'), 'rb').read()
    text_addr, _, text_size = struct.unpack_from('<III', exh, 0x10)
    code = open(os.path.join(src, 'code.bin'), 'rb').read()
    base = text_addr

    # function starts = direct BL targets in .text
    starts = set()
    for o in range(0, text_size - 3, 4):
        w = struct.unpack_from('<I', code, o)[0]
        if (w & 0x0F000000) == 0x0B000000 and (w >> 28) != 0xF:
            off = w & 0xFFFFFF
            if off & 0x800000:
                off -= 1 << 24
            t = (base + o + 8 + off * 4) & 0xFFFFFFFF
            if base <= t < base + text_size:
                starts.add(t)
    starts = sorted(starts)

    funcs, reasons = [], {}
    for st in starts:
        try:
            name, csrc, n = translate_function(code, base, st, set(starts))
            funcs.append((st, name, csrc, n))
        except Unsupported as e:
            k = str(e)
            reasons[k] = reasons.get(k, 0) + 1
        if len(funcs) >= limit:
            break

    open(os.path.join(out, 'runtime.h'), 'w').write(RUNTIME_H)
    open(os.path.join(out, 'runtime.c'), 'w').write(RUNTIME_C)
    with open(os.path.join(out, 'funcoes.c'), 'w') as f:
        f.write('#include "runtime.h"\n\n')
        for st, name, csrc, n in funcs:
            f.write(csrc + '\n\n')
        f.write(f'__declspec(dllexport) const int recomp_count = {len(funcs)};\n')
        f.write('__declspec(dllexport) const uint32_t recomp_addr[] = {' + ', '.join(f'0x{st:08X}u' for st, *_ in funcs) + '};\n')
        f.write('__declspec(dllexport) void (*const recomp_fn[])(Cpu*) = {' + ', '.join(n for _, n, *_ in funcs) + '};\n')
    total_insn = sum(n for *_, n in funcs)
    summary = {'funcoes_candidatas': len(starts), 'traduzidas': len(funcs), 'instrucoes_traduzidas': total_insn,
               'recusadas_por_motivo': dict(sorted(reasons.items(), key=lambda x: -x[1]))}
    import json
    json.dump(summary, open(os.path.join(out, 'traducao.json'), 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    print(json.dumps(summary, indent=1, ensure_ascii=True))


if __name__ == '__main__':
    main()
