"""Tradutor ARM (ARM11 / ARMv6K + VFPv2) -> C, versão 2.

Novidades em relação à versão 1:
- chamadas (BL) entre funções traduzidas, tail calls (B para outra função);
- saltos indiretos (BLX/BX reg, LDR PC, MOV/ADD PC): tabelas de switch resolvidas na
  tradução; o resto vai por dispatch(endereço) -> função nativa (ou "precisa do JIT");
- ponto flutuante VFPv2 com as regras do 3DS (FPSCR 0x03C00000: flush-to-zero, NaN
  padrão, arredondamento em direção ao zero);
- SIMD do ARMv6 (uadd8, sel, uqsub8, ...), extensões, saturação, empacotamento;
- multiplicações longas, UMAAL, multiplicações DSP (SMULxy, SMLAxy, ...), QADD;
- SWP, LDREX/STREX, MRS/MSR (flags), NOP, barreiras e TLS (mrc p15 c13).

uso: python -I recomp2.py <estudo/extraido> <saída>
"""
import json
import os
import re
import struct
import sys

import capstone

sys.path.insert(0, os.path.dirname(__file__))
import recomp as R1  # noqa: E402
from recomp import AD, COND_C, R, Unsupported, ror, shifter, nz  # noqa: E402

MD = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
CORE = {'r0': 0, 'r1': 1, 'r2': 2, 'r3': 3, 'r4': 4, 'r5': 5, 'r6': 6, 'r7': 7, 'r8': 8, 'r9': 9,
        'r10': 10, 'r11': 11, 'r12': 12, 'sb': 9, 'sl': 10, 'fp': 11, 'ip': 12, 'sp': 13, 'lr': 14, 'pc': 15}

CALL = '@@CALL_{:08X}@@'   # replaced at the end: direct call or dispatch
CONDS = {'eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'cs', 'cc'}
# longest first, so "vnmls" is not read as "vnm" + "ls"
VFP_NAMES = sorted(['vldr', 'vstr', 'vpush', 'vpop', 'vldmia', 'vstmia', 'vldmdb', 'vstmdb', 'vmov', 'vmrs', 'vmsr',
                    'vadd', 'vsub', 'vmul', 'vdiv', 'vnmul', 'vmla', 'vmls', 'vnmla', 'vnmls', 'vneg', 'vabs',
                    'vsqrt', 'vcmp', 'vcmpe', 'vcvt', 'vcvtr'], key=len, reverse=True)


class Ctx:
    """What one function needs while it is translated."""

    def __init__(self, code, base, start, starts, mod=None, reloc=None, lo=None, hi=None):
        self.code, self.base, self.start, self.starts = code, base, start, starts
        self.calls = set()
        self.mod = mod                  # None = main program, else the module number
        self.reloc = reloc or {}        # module: word offset -> code offset it points to (absolute relocation)
        self.lo = base if lo is None else lo
        self.hi = base + len(code) if hi is None else hi

    def call(self, t):
        self.calls.add(t)
        return CALL.format(t) if self.mod is None else f'@@CALLM{self.mod:03d}_{t:08X}@@'

    def word(self, a):
        o = a - self.base
        if o < 0 or o + 4 > len(self.code):
            raise Unsupported('fora do código')
        return struct.unpack_from('<I', self.code, o)[0]


# ---------------------------------------------------------------- jumps
def jump_any(ctx, expr, targets=()):
    """Salto para um endereço calculado: rótulo da própria função, retorno ou outra função."""
    if expr == 'r[14]':
        # "bx lr" is a return: to anything else than our entry lr it is the continuation of some
        # caller (0x0013328C: a loop that sets lr itself), which must not be called -> the JIT
        return ['{ uint32_t jt = r[14]; if (jt == entry_lr) return; s->fault = 4; s->miss = jt; return; }']
    lines = [f'{{ uint32_t jt = {expr};']
    if targets:
        lines.append('  switch (jt - MB) {' if R1.ADDR_REL else '  switch (jt) {')
        for t in sorted(set(targets)):
            lines.append(f'  case 0x{t:08X}u: goto L_{t:08X};')
        lines.append('  default: break; }')
    lines.append('  if (jt == entry_lr) return;')
    lines.append('  { uint32_t tl = r[14]; dispatch(s, jt); if (!s->fault && tl != entry_lr) { s->fault = 4; s->miss = tl; } } return; }')
    return lines


def table_size(ctx, pc, reg):
    """Tamanho de uma tabela de switch: procura 'cmp reg, #n' logo antes."""
    for back in range(1, 7):
        w = ctx.word(pc - 4 * back)
        if (w & 0x0FF0F000) == 0x03500000 and ((w >> 16) & 0xF) == reg:   # cmp reg, #imm
            rot = ((w >> 8) & 0xF) * 2
            n = ror(w & 0xFF, rot)
            # the jump's condition says which cases use the table: LS (<= n) -> n + 1 entries,
            # LO / CC (< n) -> n entries
            jc = ctx.word(pc) >> 28
            if jc == 0x9:
                return n + 1
            if jc == 0x3:
                return n
            raise Unsupported('tabela de switch com condição estranha')
        if (w >> 25) & 7 == 5:
            break
    raise Unsupported('tabela de switch sem tamanho')


# ---------------------------------------------------------------- VFP (with capstone)
def vreg(name):
    if name in CORE:   # first: "sb" / "sl" / "sp" are core registers, not s-registers
        return 'r', CORE[name]
    if name[0] == 's':
        return 's', int(name[1:])
    if name[0] == 'd':
        return 'd', int(name[1:])
    if name in CORE:
        return 'r', CORE[name]
    raise Unsupported(f'registrador {name}')


def vget(kind, n):
    return {'s': f'F32(s->s[{n}])', 'd': f'F64(s, {n})', 'r': f'r[{n}]'}[kind]


def vset(kind, n, expr):
    return {'s': f's->s[{n}] = B32({expr});', 'd': f'SETD(s, {n}, B64({expr}));'}[kind]


def vmem_addr(op, pc):
    m = re.match(r'\[(\w+)(?:, #(-?0x[0-9a-f]+|-?\d+))?\]', op)
    if not m:
        raise Unsupported('endereço vfp')
    base = CORE[m.group(1)]
    off = int(m.group(2), 0) if m.group(2) else 0
    b = AD((pc + 8) & ~3) if base == 15 else f'r[{base}]'
    return f'({b} + (uint32_t){off})'


def regs_list(txt):
    out = []
    for part in txt.strip('{} ').split(','):
        part = part.strip()
        if '-' in part:
            a, b = part.split('-')
            k, x = vreg(a.strip())
            _, y = vreg(b.strip())
            out += [(k, i) for i in range(x, y + 1)]
        else:
            out.append(vreg(part))
    return out


def translate_vfp(w, pc):
    ins = list(MD.disasm(struct.pack('<I', w), pc))
    if not ins:
        raise Unsupported('vfp indecifrável')
    mn, op = ins[0].mnemonic, ins[0].op_str
    parts = mn.split('.')
    head, types = parts[0], parts[1:]
    # take the condition off the name ("vaddeq" -> "vadd"), but never part of a real name ("vmls")
    name = head
    for known in VFP_NAMES:
        if head == known:
            name = known
            break
        if head.startswith(known) and head[len(known):] in CONDS:
            name = known
            break
    ops = [x.strip() for x in re.split(r',(?![^\[{]*[\]}])', op)]

    if name in ('vldr', 'vstr'):
        k, n = vreg(ops[0])
        ad = vmem_addr(ops[1], pc)
        if name == 'vldr':
            if k == 's':
                return [f's->s[{n}] = rd32(s, {ad});']
            return [f'{{ uint32_t ad = {ad}; s->s[{2 * n}] = rd32(s, ad); s->s[{2 * n + 1}] = rd32(s, ad + 4); }}']
        if k == 's':
            return [f'wr32(s, {ad}, s->s[{n}]);']
        return [f'{{ uint32_t ad = {ad}; wr32(s, ad, s->s[{2 * n}]); wr32(s, ad + 4, s->s[{2 * n + 1}]); }}']

    if name in ('vpush', 'vpop', 'vldmia', 'vstmia', 'vldmdb', 'vstmdb'):
        if name in ('vpush', 'vpop'):
            rn, wb, lst = 13, True, regs_list(op)
            ia = name == 'vpop'
            load = name == 'vpop'
        else:
            rn_s = ops[0].rstrip('!')
            rn, wb = CORE[rn_s], ops[0].endswith('!')
            lst = regs_list(','.join(ops[1:]))
            ia = name.endswith('ia')
            load = name.startswith('vld')
        words = []
        for k, n in lst:
            words += [n] if k == 's' else [2 * n, 2 * n + 1]
        size = 4 * len(words)
        lines = [f'{{ uint32_t ad = r[{rn}]{"" if ia else f" - {size}u"};']
        for i, sn in enumerate(words):
            lines.append(f'  s->s[{sn}] = rd32(s, ad + {4 * i}u);' if load else f'  wr32(s, ad + {4 * i}u, s->s[{sn}]);')
        if wb:
            lines.append(f'  r[{rn}] = r[{rn}] {"+" if ia else "-"} {size}u;')
        lines.append('}')
        return lines

    if name == 'vmov':
        ks = [vreg(o) if not o.startswith('#') else ('i', 0) for o in ops]
        if any(k == 'i' for k, _ in ks):
            raise Unsupported('vmov imediato')
        if len(ops) == 2:
            (k0, a), (k1, b) = ks
            if k0 == 's' and k1 == 'r':
                return [f's->s[{a}] = r[{b}];']
            if k0 == 'r' and k1 == 's':
                return [f'r[{a}] = s->s[{b}];']
            if k0 == 's' and k1 == 's':
                return [f's->s[{a}] = s->s[{b}];']
            if k0 == 'd' and k1 == 'd':
                return [f'{{ uint32_t lo = s->s[{2 * b}], hi = s->s[{2 * b + 1}]; s->s[{2 * a}] = lo; s->s[{2 * a + 1}] = hi; }}']
        if len(ops) == 3:
            (k0, a), (k1, b), (k2, c) = ks
            if k0 == 'd' and k1 == 'r' and k2 == 'r':
                return [f's->s[{2 * a}] = r[{b}]; s->s[{2 * a + 1}] = r[{c}];']
            if k0 == 'r' and k1 == 'r' and k2 == 'd':
                return [f'{{ uint32_t lo = s->s[{2 * c}], hi = s->s[{2 * c + 1}]; r[{a}] = lo; r[{b}] = hi; }}']
        if len(ops) == 4:
            (k0, a), (k1, b), (k2, c), (k3, d) = ks
            if k0 == 's' and k2 == 'r':
                return [f's->s[{a}] = r[{c}]; s->s[{b}] = r[{d}];']
            if k0 == 'r' and k2 == 's':
                return [f'{{ uint32_t x = s->s[{c}], y = s->s[{d}]; r[{a}] = x; r[{b}] = y; }}']
        raise Unsupported(f'vmov {op}')

    if name == 'vmrs':
        if ops[0].lower() == 'apsr_nzcv':
            return ['s->n = s->fpscr >> 31; s->z = (s->fpscr >> 30) & 1; s->c = (s->fpscr >> 29) & 1; s->v = (s->fpscr >> 28) & 1;']
        k, n = vreg(ops[0])
        return [f'r[{n}] = s->fpscr;']
    if name == 'vmsr':
        k, n = vreg(ops[1])
        return [f's->fpscr = r[{n}]; fpscr_apply(s);']

    t = types[0] if types else ''
    if t not in ('f32', 'f64') and name != 'vcvt' and name != 'vcvtr':
        raise Unsupported(f'vfp {mn}')
    K = 's' if t == 'f32' else 'd'

    if name in ('vadd', 'vsub', 'vmul', 'vdiv', 'vnmul', 'vmla', 'vmls', 'vnmla', 'vnmls'):
        if len(ops) == 2:
            ops = [ops[0]] + ops
        (_, d), (_, a), (_, b) = (vreg(o) for o in ops)
        A, B, Dv = vget(K, a), vget(K, b), vget(K, d)
        T = 'float' if K == 's' else 'double'
        e = {'vadd': f'{A} + {B}', 'vsub': f'{A} - {B}', 'vmul': f'{A} * {B}', 'vdiv': f'{A} / {B}',
             'vnmul': f'-({A} * {B})'}.get(name)
        if e:
            return ['{ ' + vset(K, d, f'({T})({e})') + ' }']
        # multiply-accumulate: the product is rounded first (no fused multiply-add)
        acc = {'vmla': 'acc + p', 'vmls': 'acc - p', 'vnmla': '-acc - p', 'vnmls': '-acc + p'}[name]
        return [f'{{ volatile {T} p = {A} * {B}; {T} acc = {Dv}; p = CANON{K}(p); ' + vset(K, d, f'({T})({acc})') + ' }']
    if name in ('vneg', 'vabs'):
        (_, d), (_, a) = (vreg(o) for o in ops)
        if K == 's':
            return [f's->s[{d}] = s->s[{a}] {"^ 0x80000000u" if name == "vneg" else "& 0x7FFFFFFFu"};']
        return [f'{{ uint32_t lo = s->s[{2 * a}], hi = s->s[{2 * a + 1}] {"^ 0x80000000u" if name == "vneg" else "& 0x7FFFFFFFu"}; '
                f's->s[{2 * d}] = lo; s->s[{2 * d + 1}] = hi; }}']
    if name == 'vsqrt':
        (_, d), (_, a) = (vreg(o) for o in ops)
        return ['{ ' + vset(K, d, f'{"sqrtf" if K == "s" else "sqrt"}({vget(K, a)})') + ' }']
    if name in ('vcmp', 'vcmpe'):
        _, a = vreg(ops[0])
        b = '0.0' if ops[1].startswith('#') else vget(K, vreg(ops[1])[1])
        return [f'VCMP(s, {vget(K, a)}, {b});']
    if name in ('vcvt', 'vcvtr'):
        if len(types) != 2 or len(ops) != 2:
            raise Unsupported('vcvt ponto fixo')
        to, fr = types
        (kd, d), (ka, a) = (vreg(o) for o in ops)
        src = {'f32': f'F32(s->s[{a}])', 'f64': f'F64(s, {a})', 's32': f'(int32_t)s->s[{a}]', 'u32': f's->s[{a}]'}[fr]
        if to == 'f32':
            return [f'{{ s->s[{d}] = B32((float)({src})); }}']
        if to == 'f64':
            return [f'{{ SETD(s, {d}, B64((double)({src}))); }}']
        if to == 's32':
            return [f's->s[{d}] = F2S32((double){src});']
        if to == 'u32':
            return [f's->s[{d}] = F2U32((double){src});']
    raise Unsupported(f'vfp {mn}')


# ---------------------------------------------------------------- one instruction
def translate(ctx, w, pc):
    cond = w >> 28
    nxt = pc + 4
    if cond == 0xF:
        if (w & 0xFD70F000) == 0xF550F000:   # PLD
            return ['/* pld */'], [nxt]
        if (w & 0xFE000000) == 0xFA000000:   # BLX imm: call into Thumb
            off = w & 0xFFFFFF
            if off & 0x800000:
                off -= 1 << 24
            t = (pc + 8 + off * 4 + ((w >> 23) & 2)) & 0xFFFFFFFF
            return [f'r[14] = 0x{nxt:08X}u; @@CALLT_{t:08X}@@; if (s->fault) return;'], [nxt]
        if w in (0xF57FF01F,) or (w & 0xFFFFFFF0) == 0xF57FF050 or (w & 0xFFFFFFF0) == 0xF57FF040 or (w & 0xFFFFFFF0) == 0xF57FF060:
            return ['/* barreira */'], [nxt]
        if (w & 0xFFF10020) == 0xF1000000:   # CPS
            raise Unsupported('cps')
        raise Unsupported('instrução incondicional')
    body, succ = translate_body(ctx, w, pc)
    if cond == 0xE:
        return body, succ
    s = list(succ)
    if nxt not in s:
        s.append(nxt)
    return [f'if ({COND_C[cond]}) {{'] + ['  ' + b for b in body] + ['}'], s


def translate_body(ctx, w, pc):
    nxt = pc + 4
    op = (w >> 25) & 7

    # ---- branch / call
    if op == 5:
        off = w & 0xFFFFFF
        if off & 0x800000:
            off -= 1 << 24
        t = (pc + 8 + off * 4) & 0xFFFFFFFF
        if w & (1 << 24):
            return [f'r[14] = {AD(nxt)}; {ctx.call(t)}; if (s->fault) return;'], [nxt]
        if t != ctx.start and t in ctx.starts:
            return [f'{{ uint32_t tl = r[14]; {ctx.call(t)}; if (!s->fault && tl != entry_lr) {{ s->fault = 4; s->miss = tl; }} }} return;   /* tail call */'], ['ret']
        return [f'goto L_{t:08X};'], [t]

    # ---- bx / blx register
    if (w & 0x0FFFFFF0) == 0x012FFF10:
        m = w & 0xF
        # (bx lr included: lr may have been changed on the way -- popped, set by hand -- and a
        # return that is not to our entry lr must not look like one)
        return jump_any(ctx, f'r[{m}]'), ['ret']
    if (w & 0x0FFFFFF0) == 0x012FFF30:
        m = w & 0xF
        return [f'{{ uint32_t t = r[{m}]; r[14] = {AD(nxt)}; dispatch(s, t); if (s->fault) return; }}'], [nxt]

    # ---- hints: nop, yield, wfe, wfi, sev
    if (w & 0x0FFFFF00) == 0x0320F000:
        return ['/* nop */'], [nxt]

    # ---- clz
    if (w & 0x0FFF0FF0) == 0x016F0F10:
        rd, rm = (w >> 12) & 0xF, w & 0xF
        return [f'r[{rd}] = r[{rm}] ? (uint32_t)__builtin_clz(r[{rm}]) : 32u;'], [nxt]

    # ---- mrs / msr (flags only: user mode)
    if (w & 0x0FBF0FFF) == 0x010F0000:
        rd = (w >> 12) & 0xF
        return [f'r[{rd}] = (s->n << 31) | (s->z << 30) | (s->c << 29) | (s->v << 28) | (s->q << 27) | (s->ge << 16) | 0x10u;'], [nxt]
    if (w & 0x0FB0FFF0) == 0x0120F000 or (w & 0x0FB0F000) == 0x0320F000:
        if (w >> 22) & 1:
            raise Unsupported('msr spsr')
        mask = (w >> 16) & 0xF
        if w & (1 << 25):
            rot = ((w >> 8) & 0xF) * 2
            val = f'0x{ror(w & 0xFF, rot):08X}u'
        else:
            val = f'r[{w & 0xF}]'
        lines = [f'{{ uint32_t v = {val};']
        if mask & 8:
            lines.append('  s->n = v >> 31; s->z = (v >> 30) & 1; s->c = (v >> 29) & 1; s->v = (v >> 28) & 1; s->q = (v >> 27) & 1;')
        if mask & 4:
            lines.append('  s->ge = (v >> 16) & 0xF;')
        lines.append('}')
        return lines, [nxt]

    # ---- DSP: qadd/qsub/qdadd/qdsub
    if (w & 0x0F900FF0) == 0x01000050:
        k = (w >> 21) & 3
        rn, rd, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF
        return [f'r[{rd}] = QOP(s, {k}, r[{rm}], r[{rn}]);'], [nxt]
    # ---- DSP multiplies (smulxy, smlaxy, smulwy, smlawy, smlalxy)
    if (w & 0x0F900090) == 0x01000080:
        k = (w >> 21) & 3
        rd, rn, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        x, y = (w >> 5) & 1, (w >> 6) & 1
        a = f'(int32_t)(int16_t)(r[{rm}] >> {16 * x})'
        b = f'(int32_t)(int16_t)(r[{rs}] >> {16 * y})'
        if k == 0:   # smlaxy
            return [f'{{ int64_t t = (int64_t)({a} * {b}) + (int32_t)r[{rn}]; r[{rd}] = (uint32_t)t; if (t != (int32_t)t) s->q = 1; }}'], [nxt]
        if k == 1:   # smlawy / smulwy
            bw = f'(int64_t)(int16_t)(r[{rs}] >> {16 * y})'
            if x:   # smulwy
                return [f'r[{rd}] = (uint32_t)(int32_t)(((int64_t)(int32_t)r[{rm}] * {bw}) >> 16);'], [nxt]
            return [f'{{ int64_t t = (((int64_t)(int32_t)r[{rm}] * {bw}) >> 16) + (int32_t)r[{rn}]; r[{rd}] = (uint32_t)t; if (t != (int32_t)t) s->q = 1; }}'], [nxt]
        if k == 2:   # smlalxy: rd = hi, rn = lo
            return [f'{{ int64_t t = (int64_t)((((uint64_t)r[{rd}] << 32) | r[{rn}])) + (int64_t)({a} * {b}); r[{rn}] = (uint32_t)t; r[{rd}] = (uint32_t)((uint64_t)t >> 32); }}'], [nxt]
        return [f'r[{rd}] = (uint32_t)({a} * {b});'], [nxt]

    # ---- multiply (long first: its bits overlap the plain multiply)
    if (w & 0x0F8000F0) == 0x00800090:
        sgn, a, sflag = (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        hi, lo, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        e = (f'(uint64_t)((int64_t)(int32_t)r[{rm}] * (int64_t)(int32_t)r[{rs}])' if sgn else f'(uint64_t)r[{rm}] * r[{rs}]')
        if a:
            e += f' + (((uint64_t)r[{hi}] << 32) | r[{lo}])'
        out = [f'{{ uint64_t t = {e}; r[{lo}] = (uint32_t)t; r[{hi}] = (uint32_t)(t >> 32);']
        if sflag:
            out.append(f'  s->n = r[{hi}] >> 31; s->z = t == 0;')
        out.append('}')
        return out, [nxt]
    if (w & 0x0FF000F0) == 0x00400090:   # umaal
        hi, lo, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        return [f'{{ uint64_t t = (uint64_t)r[{rm}] * r[{rs}] + r[{hi}] + r[{lo}]; r[{lo}] = (uint32_t)t; r[{hi}] = (uint32_t)(t >> 32); }}'], [nxt]
    if (w & 0x0FC000F0) == 0x00000090:
        a, sflag = (w >> 21) & 1, (w >> 20) & 1
        rd, rn, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        e = f'r[{rm}] * r[{rs}]' + (f' + r[{rn}]' if a else '')
        out = [f'r[{rd}] = {e};']
        if sflag:
            out.append(nz(f'r[{rd}]'))
        return out, [nxt]

    # ---- swp / swpb
    if (w & 0x0FB00FF0) == 0x01000090:
        b = (w >> 22) & 1
        rn, rd, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF
        if b:
            return [f'{{ uint32_t ad = r[{rn}], v = rd8(s, ad); wr8(s, ad, r[{rm}]); r[{rd}] = v; }}'], [nxt]
        return [f'{{ uint32_t ad = r[{rn}], v = rd32(s, ad); wr32(s, ad, r[{rm}]); r[{rd}] = v; }}'], [nxt]
    # ---- ldrex / strex (and the b / h / d forms)
    if (w & 0x0F800FFF) == 0x01800F9F and (w >> 20) & 1:
        k = (w >> 21) & 3
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        rdfn = {0: 'rd32', 2: 'rd8', 3: 'rd16'}.get(k)
        if k == 1:
            return [f'{{ uint32_t ad = r[{rn}]; r[{rd}] = rd32(s, ad); r[{rd + 1}] = rd32(s, ad + 4); s->excl = 1; s->excl_addr = ad; }}'], [nxt]
        return [f'{{ uint32_t ad = r[{rn}]; r[{rd}] = {rdfn}(s, ad); s->excl = 1; s->excl_addr = ad; }}'], [nxt]
    if (w & 0x0F800FF0) == 0x01800F90 and not (w >> 20) & 1:
        k = (w >> 21) & 3
        rn, rd, rt = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF
        st = {0: f'wr32(s, ad, r[{rt}]);', 2: f'wr8(s, ad, r[{rt}]);', 3: f'wr16(s, ad, r[{rt}]);',
              1: f'wr32(s, ad, r[{rt}]); wr32(s, ad + 4, r[{rt + 1}]);'}[k]
        return [f'{{ uint32_t ad = r[{rn}]; if (s->excl && s->excl_addr == ad) {{ {st} r[{rd}] = 0; }} else r[{rd}] = 1; s->excl = 0; }}'], [nxt]

    # ---- halfword / signed / doubleword
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
        from recomp import mem_access
        return mem_access(pc, nxt, rn, rd, p, u, wb, off, {
            (1, 1): ('rd16', 'h'), (1, 2): ('rd8s', 'sb'), (1, 3): ('rd16s', 'sh'),
            (0, 1): ('wr16', 'h'), (0, 2): ('ldrd', 'd'), (0, 3): ('strd', 'd')}[(l, sh)])

    # ---- data processing
    if op in (0, 1):
        opc, sflag = (w >> 21) & 0xF, (w >> 20) & 1
        if 8 <= opc <= 11 and not sflag:
            raise Unsupported('mrs/msr/misc')
        if op == 0 and (w & 0x90) == 0x90:
            raise Unsupported('espaço de extensão')
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        if rd == 15 and not sflag and opc in (4, 13) and not (op == 0 and (w & 0x10)):
            # add pc, pc, rN, lsl #2 (switch table) or mov pc, rN
            if opc == 4 and rn == 15 and op == 0 and ((w >> 5) & 3) == 0 and ((w >> 7) & 0x1F) == 2:
                m = w & 0xF
                n = table_size(ctx, pc, m)
                targets = [pc + 8 + 4 * k for k in range(n)]
                return jump_any(ctx, f'{AD(pc + 8)} + (r[{m}] << 2)', targets), targets
            if opc == 13 and op == 0 and (w & 0xFF0) == 0:
                m = w & 0xF
                if m == 14:
                    return ['return;'], ['ret']
                return jump_any(ctx, f'r[{m}]'), ['ret']
            raise Unsupported('escrita no pc')
        if rd == 15:
            raise Unsupported('escrita no pc')
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
            a, b, cin, sub = {2: (vn, v2, '0', True), 3: (v2, vn, '0', True), 4: (vn, v2, '0', False),
                              5: (vn, v2, 's->c', False), 6: (vn, v2, 's->c', True), 7: (v2, vn, 's->c', True),
                              10: (vn, v2, '0', True), 11: (vn, v2, '0', False)}[opc]
            lines.append(f'{{ uint32_t a = {a}, b = {b};')
            if sub:
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
        if res:
            lines.append(f'  r[{rd}] = res;')
        lines.append('}')
        if pre:
            lines.append('}')
        return lines, [nxt]

    # ---- ARMv6 media
    if op == 3 and (w & 0x10):
        return translate_media(w, pc), [nxt]

    # ---- load / store word or byte
    if op in (2, 3):
        p, u, b, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        if not p and wb:
            raise Unsupported('ldrt/strt')
        if op == 2:
            offv = w & 0xFFF
            off = f'{offv}u'
        else:
            if (w & 0xF) == 15:
                raise Unsupported('pc como índice')
            v2, _, pre = shifter(w & ~(1 << 25), pc, False)
            if pre:
                raise Unsupported('shift por registrador no endereço')
            off = v2
        if l and rd == 15:
            if b:
                raise Unsupported('ldrb pc')
            # ldr pc: a return (pop), a switch table, or a jump through memory
            if rn == 13 and not p and u and op == 2 and offv == 4:
                return [f'{{ uint32_t t = rd32(s, r[13]); r[13] += 4u; if (s->fault) return; if (t != entry_lr) {{ s->fault = 4; s->miss = t; }} return; }}'], ['ret']
            if rn == 15 and p and u and op == 3 and ((w >> 5) & 3) == 0 and ((w >> 7) & 0x1F) == 2 and not wb:
                m = w & 0xF
                n = table_size(ctx, pc, m)
                targets = []
                for k in range(n):
                    wa = pc + 8 + 4 * k
                    if ctx.mod is None:
                        targets.append(ctx.word(wa))
                    elif wa in ctx.reloc:
                        targets.append(ctx.reloc[wa])   # the table word is relocated when the module loads
                    else:
                        raise Unsupported('tabela de switch sem relocação')
                return jump_any(ctx, f'rd32(s, {AD(pc + 8)} + (r[{m}] << 2))', targets), targets
            if wb or not p:
                raise Unsupported('ldr pc com escrita')
            sign = '+' if u else '-'
            return jump_any(ctx, f'rd32(s, {R(rn, pc)} {sign} {off})'), ['ret']
        from recomp import mem_access
        kind = {(1, 0): ('rd32', 'w'), (1, 1): ('rd8', 'b'), (0, 0): ('wr32', 'w'), (0, 1): ('wr8', 'b')}[(l, b)]
        return mem_access(pc, nxt, rn, rd, p, u, wb, off, kind)

    # ---- ldm / stm
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
            first = f'r[{rn}] - {4 * n}u' if p else f'r[{rn}] - {4 * n - 4}u'
        lines = [f'{{ uint32_t ad = {first}, base = r[{rn}]; uint32_t npc = 0; (void)npc;']
        succ = [pc + 4]
        for i in lst:
            if l:
                if i == 15:
                    lines.append('  npc = rd32(s, ad); ad += 4;')
                    succ = ['ret']
                else:
                    lines.append(f'  r[{i}] = rd32(s, ad); ad += 4;')
            else:
                v = AD(pc + 12) if i == 15 else ('base' if i == rn else f'r[{i}]')
                lines.append(f'  wr32(s, ad, {v}); ad += 4;')
        if wb and not (l and rn in lst):
            lines.append(f'  r[{rn}] = base {"+" if u else "-"} {4 * n}u;')
        if succ == ['ret']:
            lines.append('  if (s->fault) return; if (npc != entry_lr) { s->fault = 4; s->miss = npc; } return;')
        lines.append('}')
        return lines, succ

    if op == 6:
        cp = (w >> 8) & 0xF
        if cp in (10, 11):
            return translate_vfp(w, pc), [nxt]
        raise Unsupported('coprocessador (ldc/stc)')
    if op == 7:
        if (w >> 24) & 1:
            return [f'if (!host_svc(s, 0x{w & 0xFFFFFF:06X}u, {AD(nxt)})) {{ if (!s->fault) {{ s->fault = 6; s->miss = {AD(pc)}; }} return; }}   /* svc: o sistema faz; senão o JIT */'], [nxt]
        cp = (w >> 8) & 0xF
        if cp in (10, 11):
            return translate_vfp(w, pc), [nxt]
        if cp == 15 and (w & 0x10):
            l = (w >> 20) & 1
            opc1, crn, rd, opc2, crm = (w >> 21) & 7, (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 5) & 7, w & 0xF
            if l and (opc1, crn, opc2, crm) == (0, 13, 3, 0):
                return [f'r[{rd}] = s->tls;'], [nxt]
            if not l and crn == 7:
                return ['/* barreira (cp15 c7) */'], [nxt]
        raise Unsupported('coprocessador')
    raise Unsupported(f'desconhecida {w:08X}')


def translate_media(w, pc):
    rd = (w >> 12) & 0xF
    rn = (w >> 16) & 0xF
    rm = w & 0xF
    # parallel add / subtract
    if (w & 0x0F800010) == 0x06000010 and (w & 0xF00) == 0xF00:
        op1, op2 = (w >> 20) & 7, (w >> 5) & 7
        if op1 in (1, 2, 3, 5, 6, 7) and op2 in (0, 1, 2, 3, 4, 7):
            return [f'r[{rd}] = PAR(s, {op1}, {op2}, r[{rn}], r[{rm}]);']
        raise Unsupported('paralelo')
    # sel
    if (w & 0x0FF00FF0) == 0x06800FB0:
        return [f'r[{rd}] = SEL(s, r[{rn}], r[{rm}]);']
    # extend (with add), 8/16 and the dual 8 -> 16 forms
    if (w & 0x0F8003F0) == 0x06800070:
        sub = (w >> 20) & 7
        rot = ((w >> 10) & 3) * 8
        src = f'(r[{rm}] >> {rot} | r[{rm}] << {32 - rot})' if rot else f'r[{rm}]'
        if sub in (0, 4):   # sxtb16 / uxtb16
            sgn = sub == 0
            lo = f'(uint32_t)(uint16_t)(int16_t)(int8_t)(x)' if sgn else '(x & 0xFFu)'
            hi = f'(uint32_t)(uint16_t)(int16_t)(int8_t)(x >> 16)' if sgn else '((x >> 16) & 0xFFu)'
            if rn == 15:
                return [f'{{ uint32_t x = {src}; r[{rd}] = ({lo}) | (({hi}) << 16); }}']
            return [f'{{ uint32_t x = {src}, y = r[{rn}]; r[{rd}] = ((y + {lo}) & 0xFFFFu) | (((y >> 16) + {hi}) << 16); }}']
        ext = {2: f'(uint32_t)(int32_t)(int8_t){src}', 3: f'(uint32_t)(int32_t)(int16_t){src}',
               6: f'(uint32_t)(uint8_t){src}', 7: f'(uint32_t)(uint16_t){src}'}.get(sub)
        if not ext:
            raise Unsupported('extensão')
        add = '' if rn == 15 else f'r[{rn}] + '
        return [f'r[{rd}] = {add}{ext};']
    # rev, rev16, revsh
    if (w & 0x0FFF0FF0) == 0x06BF0F30:
        return [f'r[{rd}] = __builtin_bswap32(r[{rm}]);']
    if (w & 0x0FFF0FF0) == 0x06BF0FB0:
        return [f'r[{rd}] = ((r[{rm}] & 0x00FF00FFu) << 8) | ((r[{rm}] >> 8) & 0x00FF00FFu);']
    if (w & 0x0FFF0FF0) == 0x06FF0FB0:
        return [f'r[{rd}] = (uint32_t)(int32_t)(int16_t)(((r[{rm}] & 0xFF) << 8) | ((r[{rm}] >> 8) & 0xFF));']
    # pkhbt / pkhtb
    if (w & 0x0FF00030) == 0x06800010:
        sh = (w >> 7) & 0x1F
        if (w >> 6) & 1:   # pkhtb: asr
            e = f'(uint32_t)((int32_t)r[{rm}] >> {sh if sh else 31})'
            return [f'r[{rd}] = (r[{rn}] & 0xFFFF0000u) | ({e} & 0xFFFFu);']
        return [f'r[{rd}] = (r[{rn}] & 0xFFFFu) | ((r[{rm}] << {sh}) & 0xFFFF0000u);']
    # ssat / usat
    if (w & 0x0FA00030) == 0x06A00010:
        u = (w >> 22) & 1
        sat = (w >> 16) & 0x1F
        sh = (w >> 7) & 0x1F
        asr = (w >> 6) & 1
        if asr:
            v = f'(int64_t)((int32_t)r[{rm}] >> {sh if sh else 31})'
        else:
            v = f'(int64_t)(int32_t)(r[{rm}] << {sh})'
        if u:
            return [f'{{ int64_t v = {v}, mx = (1ll << {sat}) - 1; if (v < 0) {{ v = 0; s->q = 1; }} else if (v > mx) {{ v = mx; s->q = 1; }} r[{rd}] = (uint32_t)v; }}']
        return [f'{{ int64_t v = {v}, mx = (1ll << {sat}) - 1, mn = -(1ll << {sat}); if (v < mn) {{ v = mn; s->q = 1; }} else if (v > mx) {{ v = mx; s->q = 1; }} r[{rd}] = (uint32_t)v; }}']
    # usad8 / usada8
    if (w & 0x0FF000F0) == 0x07800010:
        rs = (w >> 8) & 0xF
        e = ' + '.join(f'(uint32_t)abs((int)((r[{rm}] >> {8 * k}) & 0xFF) - (int)((r[{rs}] >> {8 * k}) & 0xFF))' for k in range(4))
        acc = '' if rd == 15 else f'r[{rd}] + '
        return [f'r[{rn}] = {acc}{e};']
    # smmul / smmla (rounding variants too)
    if (w & 0x0FF000D0) == 0x07500010:
        ra, rs = (w >> 12) & 0xF, (w >> 8) & 0xF
        rnd = '0x80000000ll' if (w >> 5) & 1 else '0'
        acc = '' if ra == 15 else f'((int64_t)r[{ra}] << 32) + '
        return [f'r[{rn}] = (uint32_t)((uint64_t)({acc}(int64_t)(int32_t)r[{w & 0xF}] * (int32_t)r[{rs}] + {rnd}) >> 32);']
    raise Unsupported('mídia ARMv6')


# ---------------------------------------------------------------- a whole function
def translate_function(code, base, start, starts, max_insns=20000, mod=None, reloc=None, lo=None, hi=None):
    ctx = Ctx(code, base, start, starts, mod, reloc, lo, hi)
    R1.ADDR_REL = mod is not None
    work, out = [start], {}
    while work:
        a = work.pop()
        if a in out:
            continue
        if a != start and a in starts:
            # runs straight into the next function: the same as calling it with the same lr
            out[a] = [f'{{ uint32_t tl = r[14]; s->icount--; {ctx.call(a)}; if (!s->fault && tl != entry_lr) {{ s->fault = 4; s->miss = tl; }} }} return;   /* continua em outra função */']
            continue
        stub = [f's->fault = 3; s->miss = {AD(a)}; return;   /* o JIT faz esta instrução */']
        try:
            w = ctx.word(a)
            lines, succ = translate(ctx, w, a)
            if any(t != 'ret' and not (ctx.lo <= t < ctx.hi) for t in succ):
                raise Unsupported('desvio para fora')
            if False and a == start and any('s->fault = 6' in ln for ln in lines):   # (svc direto: now they can run natively)
                # starts with a system call (an svc wrapper): the C code would hand back to the JIT on
                # its first instruction, which is our patched svc -> calls us again, forever
                raise Unsupported('começa com chamada ao sistema')
        except Unsupported:
            # only this point goes back to the JIT (data after a call that never returns, a rare
            # instruction...); not the first one: it is replaced by the svc that calls us
            if a == start:
                raise
            lines, succ = stub, []
            ctx.stubs = getattr(ctx, 'stubs', 0) + 1
        out[a] = lines
        if len(out) > max_insns:
            raise Unsupported('grande demais')
        for t in succ:
            if t != 'ret':
                work.append(t)
    name = f'f_{start:08X}' if mod is None else f'm{mod:03d}_{start:06X}'
    c = [f'void {name}(Cpu* s) {{', '  uint32_t* r = s->r; const uint32_t entry_lr = r[14]; (void)entry_lr;']
    if mod is not None:
        c.append(f'  const uint32_t MB = recomp_modbase[{mod}];   /* where the module is loaded now */')
    c.append(f'  goto L_{start:08X};')
    addrs = sorted(out)
    for i, a in enumerate(addrs):
        c.append(f' L_{a:08X}: if (++s->icount > s->ilimit) {{ s->fault = 2; s->miss = {AD(a)}; return; }}')
        c += ['  ' + x for x in out[a]]
        nx = addrs[i + 1] if i + 1 < len(addrs) else None
        if nx != a + 4:
            c.append(f'  goto L_{a + 4:08X};' if (a + 4) in out else f'  s->fault = 3; s->miss = {AD(a + 4)}; return;')
    c.append('}')
    R1.ADDR_REL = False
    return name, '\n'.join(c), len(out), ctx.calls


RUNTIME_H = r'''
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
typedef struct {
    uint32_t r[16]; uint32_t n, z, c, v, q, ge; uint32_t fault, miss;
    uint32_t tls, excl, excl_addr, fpscr; uint32_t s[32];
    uint32_t icount, ilimit;   // instructions run (for the emulated clock) / where to stop and hand back to the JIT
} Cpu;
uint8_t rd8(Cpu* s, uint32_t a);
uint16_t rd16(Cpu* s, uint32_t a);
uint32_t rd32(Cpu* s, uint32_t a);
void wr8(Cpu* s, uint32_t a, uint32_t v);
void wr16(Cpu* s, uint32_t a, uint32_t v);
void wr32(Cpu* s, uint32_t a, uint32_t v);
void dispatch(Cpu* s, uint32_t target);
uint32_t PAR(Cpu* s, int op1, int op2, uint32_t a, uint32_t b);
uint32_t SEL(Cpu* s, uint32_t a, uint32_t b);
uint32_t QOP(Cpu* s, int k, uint32_t a, uint32_t b);
void fpscr_apply(Cpu* s);
// system call run by the exe: 1 = done, go on; 0 = not done (hand back on the svc) or done but a thread
// switch is due (the exe set fault/miss to the next instruction)
int host_svc(Cpu* s, uint32_t imm, uint32_t next);
// floating point as on the 3DS: default NaN, flush-to-zero (host DAZ/FTZ), round toward zero (host MXCSR)
static inline float F32(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t B32(float f) { uint32_t b; memcpy(&b, &f, 4); if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)) b = 0x7FC00000u; return b; }
static inline double F64(Cpu* s, int d) { uint64_t b = (uint64_t)s->s[2 * d] | ((uint64_t)s->s[2 * d + 1] << 32); double f; memcpy(&f, &b, 8); return f; }
static inline uint64_t B64(double f) { uint64_t b; memcpy(&b, &f, 8); if ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (b & 0xFFFFFFFFFFFFFull)) b = 0x7FF8000000000000ull; return b; }
static inline void SETD(Cpu* s, int d, uint64_t b) { s->s[2 * d] = (uint32_t)b; s->s[2 * d + 1] = (uint32_t)(b >> 32); }
static inline float CANONs(float f) { return F32(B32(f)); }
static inline double CANONd(double f) { uint64_t b = B64(f); double g; memcpy(&g, &b, 8); return g; }
static inline void VCMP(Cpu* s, double a, double b) {
    uint32_t f = (a != a || b != b) ? 3u : a == b ? 6u : a < b ? 8u : 2u;
    s->fpscr = (s->fpscr & 0x0FFFFFFFu) | (f << 28);
}
static inline uint32_t F2S32(double x) { if (x != x) return 0; if (x >= 2147483648.0) return 0x7FFFFFFFu; if (x <= -2147483649.0) return 0x80000000u; return (uint32_t)(int32_t)x; }
static inline uint32_t F2U32(double x) { if (x != x || x <= -1.0) return 0; if (x >= 4294967296.0) return 0xFFFFFFFFu; return (uint32_t)x; }
'''

RUNTIME_C = r'''
#include "runtime.h"
#include <immintrin.h>
typedef struct { uint32_t base, len; uint8_t* p; int writable; } Win;
static Win win[6];
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

// ARMv6 parallel add / subtract (op1: 1 S, 2 Q, 3 SH, 5 U, 6 UQ, 7 UH; op2: 0 add16, 1 asx, 2 sax, 3 sub16, 4 add8, 7 sub8)
uint32_t PAR(Cpu* s, int op1, int op2, uint32_t a, uint32_t b)
{
    int sgn = op1 < 4, kind = op1 & 3;   // kind 1 plain (GE), 2 saturating, 3 halving
    uint32_t res = 0, ge = 0;
    if (op2 == 4 || op2 == 7) {
        for (int i = 0; i < 4; i++) {
            int x = sgn ? (int8_t)(a >> 8 * i) : (uint8_t)(a >> 8 * i), y = sgn ? (int8_t)(b >> 8 * i) : (uint8_t)(b >> 8 * i);
            int r = op2 == 4 ? x + y : x - y;
            if (kind == 1) { if (sgn ? r >= 0 : (op2 == 4 ? r >= 0x100 : r >= 0)) ge |= 1u << i; }
            else if (kind == 2) { int lo = sgn ? -128 : 0, hi = sgn ? 127 : 255; r = r < lo ? lo : r > hi ? hi : r; }
            else r >>= 1;
            res |= (uint32_t)(r & 0xFF) << 8 * i;
        }
        if (kind == 1) s->ge = ge;
        return res;
    }
    for (int i = 0; i < 2; i++) {
        int xa = sgn ? (int16_t)(a >> 16 * i) : (uint16_t)(a >> 16 * i);
        int other = (op2 == 1 || op2 == 2) ? 1 - i : i;
        int yb = sgn ? (int16_t)(b >> 16 * other) : (uint16_t)(b >> 16 * other);
        int add = op2 == 0 || (op2 == 1 && i == 1) || (op2 == 2 && i == 0);
        int r = add ? xa + yb : xa - yb;
        if (kind == 1) { if (sgn ? r >= 0 : (add ? r >= 0x10000 : r >= 0)) ge |= 3u << 2 * i; }
        else if (kind == 2) { int lo = sgn ? -32768 : 0, hi = sgn ? 32767 : 65535; r = r < lo ? lo : r > hi ? hi : r; }
        else r >>= 1;
        res |= (uint32_t)(r & 0xFFFF) << 16 * i;
    }
    if (kind == 1) s->ge = ge;
    return res;
}
uint32_t SEL(Cpu* s, uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) r |= ((s->ge >> i & 1) ? a : b) & (0xFFu << 8 * i);
    return r;
}
static int32_t sat32(Cpu* s, int64_t v) { if (v > 0x7FFFFFFF) { s->q = 1; return 0x7FFFFFFF; } if (v < -0x80000000ll) { s->q = 1; return (int32_t)0x80000000u; } return (int32_t)v; }
uint32_t QOP(Cpu* s, int k, uint32_t a, uint32_t b)   // 0 qadd, 1 qsub, 2 qdadd, 3 qdsub: a = Rm, b = Rn
{
    int64_t y = (int32_t)b;
    if (k & 2) y = sat32(s, 2 * y);
    return (uint32_t)sat32(s, (k & 1) ? (int64_t)(int32_t)a - y : (int64_t)(int32_t)a + y);
}

// the host floating point set like the 3DS from the game's FPSCR: rounding (RMode, bits 22-23),
// flush-to-zero (FZ, bit 24) -> MXCSR; every exception masked
void fpscr_apply(Cpu* s)
{
    static const unsigned rc[4] = { 0x0000u, 0x4000u, 0x2000u, 0x6000u };   // ARM RN, RP, RM, RZ -> MXCSR RC
    unsigned m = 0x1F80u | rc[(s->fpscr >> 22) & 3];
    if (s->fpscr & (1u << 24)) m |= 0x8000u | 0x40u;   // FTZ + DAZ
    _mm_setcsr(m);
}
__declspec(dllexport) void call_native(void (*fn)(Cpu*), Cpu* s)
{
    unsigned int old = _mm_getcsr();
    fpscr_apply(s);
    fn(s);
    _mm_setcsr(old);
}
'''


# the runtime used inside the game: memory goes through the emulator (functions given by the exe)
RUNTIME_C = RUNTIME_C + '\nint host_svc(Cpu* s, uint32_t imm, uint32_t next) { (void)s; (void)imm; (void)next; return 0; }\n'
RUNTIME_HOST_C = RUNTIME_C.split('// ARMv6 parallel')[0].split('typedef struct { uint32_t base')[0] + r'''
typedef uint32_t (*RdFn)(uint32_t);
typedef void (*WrFn)(uint32_t, uint32_t);
static RdFn h_rd8, h_rd16, h_rd32;
static WrFn h_wr8, h_wr16, h_wr32;
typedef int (*SvcFn)(Cpu*, uint32_t, uint32_t);
static SvcFn h_svc;
__declspec(dllexport) void recomp_set_svc(SvcFn f) { h_svc = f; }
int host_svc(Cpu* s, uint32_t imm, uint32_t next) { return h_svc ? h_svc(s, imm, next) : 0; }
__declspec(dllexport) void recomp_set_host(RdFn r8, RdFn r16, RdFn r32, WrFn w8, WrFn w16, WrFn w32)
{ h_rd8 = r8; h_rd16 = r16; h_rd32 = r32; h_wr8 = w8; h_wr16 = w16; h_wr32 = w32; }
uint8_t rd8(Cpu* s, uint32_t a) { (void)s; return (uint8_t)h_rd8(a); }
uint16_t rd16(Cpu* s, uint32_t a) { (void)s; return (uint16_t)h_rd16(a); }
uint32_t rd32(Cpu* s, uint32_t a) { (void)s; return h_rd32(a); }
void wr8(Cpu* s, uint32_t a, uint32_t v) { (void)s; h_wr8(a, v); }
void wr16(Cpu* s, uint32_t a, uint32_t v) { (void)s; h_wr16(a, v); }
void wr32(Cpu* s, uint32_t a, uint32_t v) { (void)s; h_wr32(a, v); }
''' + '// ARMv6 parallel' + RUNTIME_C.split('// ARMv6 parallel')[1]


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    exh = open(os.path.join(src, 'exheader.bin'), 'rb').read()
    text_addr, _, text_size = struct.unpack_from('<III', exh, 0x10)
    code = open(os.path.join(src, 'code.bin'), 'rb').read()[:text_size]
    base = text_addr
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
    funcs, reasons, all_calls = [], {}, {}
    for st in sorted(starts):
        try:
            name, csrc, n, calls = translate_function(code, base, st, starts)
            funcs.append((st, name, csrc, n))
            all_calls[st] = calls
        except Unsupported as e:
            k = str(e)
            reasons[k] = reasons.get(k, 0) + 1
    done = {st for st, *_ in funcs}

    def fix(csrc):
        return re.sub(r'@@CALL_([0-9A-F]{8})@@',
                      lambda m: f'f_{m.group(1)}(s)' if int(m.group(1), 16) in done else f'dispatch(s, 0x{m.group(1)}u)', csrc)

    # functions whose whole call tree is translated (no dispatch needed for direct calls)
    complete = set()
    changed = True
    cand = {st for st in done if all(c in done for c in all_calls[st])}
    while changed:
        changed = False
        for st in list(cand):
            if not all(c in cand for c in all_calls[st]):
                cand.discard(st)
                changed = True
    complete = cand

    open(os.path.join(out, 'runtime.h'), 'w').write(RUNTIME_H)
    open(os.path.join(out, 'runtime.c'), 'w').write(RUNTIME_C)
    open(os.path.join(out, 'runtime_host.c'), 'w').write(RUNTIME_HOST_C)
    # several .c files so gcc can work in parallel
    per = 1500
    files = []
    for k in range(0, len(funcs), per):
        fnm = f'funcoes_{k // per:02d}.c'
        files.append(fnm)
        with open(os.path.join(out, fnm), 'w') as f:
            f.write('#include "runtime.h"\n')
            for st, *_ in funcs:
                f.write(f'void f_{st:08X}(Cpu* s);\n')
            for st, name, csrc, n in funcs[k:k + per]:
                f.write(fix(csrc) + '\n\n')
    with open(os.path.join(out, 'tabela.c'), 'w') as f:
        f.write('#include "runtime.h"\n')
        for st, *_ in funcs:
            f.write(f'void f_{st:08X}(Cpu* s);\n')
        f.write(f'__declspec(dllexport) const int recomp_count = {len(funcs)};\n')
        f.write('__declspec(dllexport) const uint32_t recomp_addr[] = {' + ', '.join(f'0x{st:08X}u' for st, *_ in funcs) + '};\n')
        f.write('__declspec(dllexport) void (*const recomp_fn[])(Cpu*) = {' + ', '.join(n for _, n, *_ in funcs) + '};\n')
        f.write('__declspec(dllexport) const uint8_t recomp_complete[] = {' + ', '.join('1' if st in complete else '0' for st, *_ in funcs) + '};\n')
        f.write(r'''
RUNTIME_HOST_C = RUNTIME_HOST_C.replace('
int host_svc(Cpu* s, uint32_t imm, uint32_t next) { (void)s; (void)imm; (void)next; return 0; }
', '
')
void dispatch(Cpu* s, uint32_t t)
{
    if (t & 1) { s->fault = 5; s->miss = t; return; }   /* Thumb: not translated yet */
    int lo = 0, hi = recomp_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (recomp_addr[mid] == t) { recomp_fn[mid](s); return; }
        if (recomp_addr[mid] < t) lo = mid + 1; else hi = mid - 1;
    }
    s->fault = 4; s->miss = t;   /* not translated: in the hybrid port this goes to the JIT */
}
''')
    summary = {'funcoes_candidatas': len(starts), 'traduzidas': len(funcs), 'arvore_completa': len(complete),
               'instrucoes_traduzidas': sum(n for *_, n in funcs), 'arquivos_c': files,
               'recusadas_por_motivo': dict(sorted(reasons.items(), key=lambda x: -x[1]))}
    json.dump(summary, open(os.path.join(out, 'traducao.json'), 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    print(json.dumps({k: v for k, v in summary.items() if k != 'arquivos_c'}, indent=1, ensure_ascii=True))


if __name__ == '__main__':
    main()

# the game runtime has the real host_svc: no test stub in it
RUNTIME_HOST_C = RUNTIME_HOST_C.replace('int host_svc(Cpu* s, uint32_t imm, uint32_t next) { (void)s; (void)imm; (void)next; return 0; }', '')


# ---------------------------------------------------------------- fase C: memória rápida
import re as _re
_PROTOS = """uint8_t rd8(Cpu* s, uint32_t a);
uint16_t rd16(Cpu* s, uint32_t a);
uint32_t rd32(Cpu* s, uint32_t a);
void wr8(Cpu* s, uint32_t a, uint32_t v);
void wr16(Cpu* s, uint32_t a, uint32_t v);
void wr32(Cpu* s, uint32_t a, uint32_t v);"""
_FAST = """uint8_t rd8_s(Cpu* s, uint32_t a);
uint16_t rd16_s(Cpu* s, uint32_t a);
uint32_t rd32_s(Cpu* s, uint32_t a);
void wr8_s(Cpu* s, uint32_t a, uint32_t v);
void wr16_s(Cpu* s, uint32_t a, uint32_t v);
void wr32_s(Cpu* s, uint32_t a, uint32_t v);
// the emulator's page table (one pointer per 4 KB page, 0 = not plain memory): set by the exe
extern uint8_t** recomp_pages;
static inline uint8_t* PG(uint32_t a, uint32_t n) {
    uint8_t** t = recomp_pages;
    if (!t) return 0;
    uint8_t* p = t[a >> 12];
    if (!p || (a & 0xFFFu) > 0x1000u - n) return 0;
    return p + (a & 0xFFFu);
}
static inline uint8_t rd8(Cpu* s, uint32_t a) { uint8_t* p = PG(a, 1); return p ? *p : rd8_s(s, a); }
static inline uint16_t rd16(Cpu* s, uint32_t a) { uint8_t* p = PG(a, 2); if (p) { uint16_t v; memcpy(&v, p, 2); return v; } return rd16_s(s, a); }
static inline uint32_t rd32(Cpu* s, uint32_t a) { uint8_t* p = PG(a, 4); if (p) { uint32_t v; memcpy(&v, p, 4); return v; } return rd32_s(s, a); }
static inline void wr8(Cpu* s, uint32_t a, uint32_t v) { uint8_t* p = PG(a, 1); if (p) *p = (uint8_t)v; else wr8_s(s, a, v); }
static inline void wr16(Cpu* s, uint32_t a, uint32_t v) { uint8_t* p = PG(a, 2); if (p) { uint16_t w = (uint16_t)v; memcpy(p, &w, 2); } else wr16_s(s, a, v); }
static inline void wr32(Cpu* s, uint32_t a, uint32_t v) { uint8_t* p = PG(a, 4); if (p) memcpy(p, &v, 4); else wr32_s(s, a, v); }"""
assert RUNTIME_H.count(_PROTOS) == 1
RUNTIME_H = RUNTIME_H.replace(_PROTOS, _FAST)
_DEF = _re.compile(r'^(uint8_t|uint16_t|uint32_t|void) (rd|wr)(8|16|32)\(Cpu\* s,', _re.M)
assert len(_DEF.findall(RUNTIME_C)) == 6 and len(_DEF.findall(RUNTIME_HOST_C)) == 6
RUNTIME_C = _DEF.sub(r'\1 \2\3_s(Cpu* s,', RUNTIME_C) + '\nuint8_t** recomp_pages = 0;\n'
RUNTIME_HOST_C = _DEF.sub(r'\1 \2\3_s(Cpu* s,', RUNTIME_HOST_C) + '\n__declspec(dllexport) uint8_t** recomp_pages = 0;\n'
