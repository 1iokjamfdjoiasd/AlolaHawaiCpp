"""Gera o código nativo do jogo inteiro: programa principal (ARM + Thumb) e os módulos .cro.

uso: python -I gerar.py <estudo/extraido> <saída> [--sem-modulos]

<extraido> precisa de exheader.bin, code.bin e modulos/*.cro (o que extrair.py grava).
Saída: funcoes_XX.c, tabela.c, runtime.h, runtime.c (teste), runtime_host.c (jogo), traducao.json.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import recomp2  # noqa: E402
import thumb  # noqa: E402
from recomp import Unsupported  # noqa: E402

# --generoso: take every candidate (pointers, constants, addresses inside known code); wrong ones are
# found and switched off by the live check (PORTE_CONFERIR) -> recomp_desligar.txt
GENEROSO = '--generoso' in sys.argv

# --extras <file>: entries the game was seen to need (recomp_faltam.txt written by the exe with
# PORTE_RECOMP_MISS=1): "address" (main program) or "Module+offset"
EXTRAS_MAIN, EXTRAS_MOD = set(), {}
if '--extras' in sys.argv:
    for _l in open(sys.argv[sys.argv.index('--extras') + 1], encoding='utf-8', errors='replace'):
        _l = _l.strip()
        if not _l or _l.startswith('#'):
            continue
        if '+' in _l:
            _n, _o = _l.rsplit('+', 1)
            EXTRAS_MOD.setdefault(_n, set()).add(int(_o, 16))
        else:
            EXTRAS_MAIN.add(int(_l, 16))


def bl_targets(code, lo, hi, base):
    """destinos de BL (ARM) dentro de [lo, hi)"""
    out = set()
    for o in range(lo - base, hi - base - 3, 4):
        w = struct.unpack_from('<I', code, o)[0]
        if (w & 0x0F000000) == 0x0B000000 and (w >> 28) != 0xF:
            off = w & 0xFFFFFF
            if off & 0x800000:
                off -= 1 << 24
            t = (base + o + 8 + off * 4) & 0xFFFFFFFF
            if lo <= t < hi:
                out.add(t)
    return out


def blx_thumb_targets(code, lo, hi, base):
    out = set()
    for o in range(lo - base, hi - base - 3, 4):
        w = struct.unpack_from('<I', code, o)[0]
        if (w & 0xFE000000) == 0xFA000000:
            off = w & 0xFFFFFF
            if off & 0x800000:
                off -= 1 << 24
            t = (base + o + 8 + off * 4 + ((w >> 23) & 2)) & 0xFFFFFFFF
            if lo <= t < hi:
                out.add(t)
    return out


def is_return_or_branch(w):
    if (w & 0x0FFFFFFF) == 0x012FFF1E:   # bx lr
        return True
    if (w & 0x0FFF8000) == 0x08BD8000:   # pop {..., pc}
        return True
    if (w & 0x0F000000) == 0x0A000000 and (w >> 28) == 0xE:   # b (always)
        return True
    if w == 0xE1A0F00E:                  # mov pc, lr
        return True
    return False


def looks_like_start(code, base, a):
    """um ponteiro nos dados aponta para o começo de uma função?"""
    o = a - base
    if o < 4 or o + 4 > len(code):
        return False
    w = struct.unpack_from('<I', code, o)[0]
    prev = struct.unpack_from('<I', code, o - 4)[0]
    if (w & 0x0FFF4000) == 0x092D4000:   # push {..., lr}
        return True
    return is_return_or_branch(prev) and (w >> 28) == 0xE


def code_pointers(blob, lo, hi):
    out = set()
    for i in range(0, len(blob) - 3, 4):
        v = struct.unpack_from('<I', blob, i)[0]
        if lo <= v < hi and not v & 3:
            out.add(v)
    return out


def cro_parse(path):
    d = open(path, 'rb').read()
    if len(d) < 0x138 or d[0x80:0x84] != b'CRO0':
        return None
    u = lambda o: struct.unpack_from('<I', d, o)[0]
    name_off, name_sz = u(0xC0), u(0xC4)
    name = d[name_off:name_off + name_sz].rstrip(b'\0').decode(errors='replace')
    segs = []
    so, sc = u(0xC8), u(0xCC)
    for k in range(sc):
        off, size, typ = struct.unpack_from('<III', d, so + 12 * k)
        segs.append((off, size, typ))

    def tag(t):   # segment tag -> file offset (or None)
        if t == 0xFFFFFFFF:
            return None
        si, so_ = t & 0xF, t >> 4
        if si >= len(segs):
            return None
        return segs[si][0] + so_, segs[si][2]

    code_segs = [(o, o + s) for o, s, t in segs if t == 0 and s]
    # absolute relocations: word at target <- address of (symbol segment + addend)
    reloc = {}
    po, pc_ = u(0x128), u(0x12C)
    for k in range(pc_):
        tg, ty, sym, _u1, _u2, add = struct.unpack_from('<IBBBBI', d, po + 12 * k)
        if ty != 2:
            continue
        at = tag(tg)
        if not at or sym >= len(segs):
            continue
        reloc[at[0]] = (segs[sym][0] + add, segs[sym][2])
    starts = set()
    for lo, hi in code_segs:
        starts |= bl_targets(d, lo, hi, 0)
    lits = set()
    for lo, hi in code_segs:
        lits |= pc_literals(d[lo:hi], lo, hi)
    for _w, (v, typ) in reloc.items():   # function pointers / vtables (never a constant some code loads)
        if typ == 0 and any(lo <= v < hi for lo, hi in code_segs) and not v & 3 and v not in lits                 and v + 4 <= len(d) and (struct.unpack_from('<I', d, v)[0] >> 28) == 0xE:
            starts.add(v)
    for off_hdr in (0xA0, 0xA4, 0xA8):  # onLoad, onExit, onUnresolved
        t = tag(u(off_hdr))
        if t and t[1] == 0:
            starts.add(t[0])
    eo, ec = u(0xD0), u(0xD4)           # named exports: name offset, segment tag
    for k in range(ec):
        t = tag(struct.unpack_from('<I', d, eo + 8 * k + 4)[0])
        if t and t[1] == 0:
            starts.add(t[0])
    io_, ic = u(0xD8), u(0xDC)          # indexed exports: segment tag
    for k in range(ic):
        t = tag(struct.unpack_from('<I', d, io_ + 4 * k)[0])
        if t and t[1] == 0:
            starts.add(t[0])
    code_reloc = {w: v for w, (v, typ) in reloc.items() if typ == 0}
    starts |= {a for a in EXTRAS_MOD.get(name, ()) if not a & 3 and any(lo <= a < hi for lo, hi in code_segs)}
    # never patch data: the first instruction unconditional, not a constant the code loads
    starts = {a for a in starts if a + 4 <= len(d) and (struct.unpack_from('<I', d, a)[0] >> 28) == 0xE and a not in lits}
    return {'nome': name, 'dados': d, 'segs': code_segs, 'starts': starts, 'reloc': code_reloc, 'tamanho': len(d)}



def pc_literals(code, lo, hi):
    """words some instruction reads as data, relative to the PC (literal pools)"""
    out = set()
    for o in range(0, hi - lo - 3, 4):
        w = struct.unpack_from('<I', code, o)[0]
        pc = lo + o + 8
        if (w & 0x0E5F0000) == 0x041F0000:            # ldr / ldrb rX, [pc, #+-imm]
            t = pc + (w & 0xFFF) if w & (1 << 23) else pc - (w & 0xFFF)
            out.add(t & ~3)
        elif (w & 0x0E5F0090) == 0x005F0090 and (w & 0x60):   # ldrh / ldrsb / ldrsh / ldrd [pc, #imm]
            imm = ((w >> 4) & 0xF0) | (w & 0xF)
            t = pc + imm if w & (1 << 23) else pc - imm
            out.add(t & ~3)
            if not (w & (1 << 20)) and ((w >> 5) & 3) == 2:   # ldrd: two words
                out.add((t & ~3) + 4)
        elif (w & 0x0F3F0E00) == 0x0D1F0A00:          # vldr sN / dN, [pc, #imm]
            imm = (w & 0xFF) * 4
            t = (pc & ~3) + imm if w & (1 << 23) else (pc & ~3) - imm
            out.add(t)
            if (w >> 8) & 1:   # vldr dN (double): two words
                out.add(t + 4)
    return out


def pc_literals_at(code, lo, addrs):
    """the words that the instructions at 'addrs' load relative to the PC (constants)"""
    out = set()
    for a in addrs:
        o = a - lo
        if o < 0 or o + 4 > len(code):
            continue
        w = struct.unpack_from('<I', code, o)[0]
        pc = a + 8
        if (w & 0x0E5F0000) == 0x041F0000:
            t = pc + (w & 0xFFF) if w & (1 << 23) else pc - (w & 0xFFF)
            out.add(t & ~3)
        elif (w & 0x0E5F0090) == 0x005F0090 and (w & 0x60):
            imm = ((w >> 4) & 0xF0) | (w & 0xF)
            t = pc + imm if w & (1 << 23) else pc - imm
            out.add(t & ~3)
            if not (w & (1 << 20)) and ((w >> 5) & 3) == 2:   # ldrd: two words
                out.add((t & ~3) + 4)
        elif (w & 0x0F3F0E00) == 0x0D1F0A00:
            imm = (w & 0xFF) * 4
            t = (pc & ~3) + imm if w & (1 << 23) else (pc & ~3) - imm
            out.add(t)
            if (w >> 8) & 1:   # vldr dN (double): two words
                out.add(t + 4)
    return out


def static_exports(path):
    """code addresses the main program exports to the modules (static.crs)"""
    out = set()
    if not os.path.exists(path):
        return out
    d = open(path, 'rb').read()
    u = lambda o: struct.unpack_from('<I', d, o)[0]
    so, sc = u(0xC8), u(0xCC)
    segs = [struct.unpack_from('<III', d, so + 12 * k) for k in range(sc)]

    def tag(t):
        if t == 0xFFFFFFFF or (t & 0xF) >= len(segs):
            return None
        return segs[t & 0xF][0] + (t >> 4), segs[t & 0xF][2]
    eo, ec = u(0xD0), u(0xD4)
    for k in range(ec):
        t = tag(struct.unpack_from('<I', d, eo + 8 * k + 4)[0])
        if t and t[1] == 0:
            out.add(t[0])
    io_, ic = u(0xD8), u(0xDC)
    for k in range(ic):
        t = tag(u(io_ + 4 * k))
        if t and t[1] == 0:
            out.add(t[0])
    return out

def main():
    src, out = sys.argv[1], sys.argv[2]
    with_mods = '--sem-modulos' not in sys.argv
    os.makedirs(out, exist_ok=True)
    exh = open(os.path.join(src, 'exheader.bin'), 'rb').read()
    text_addr, _, text_size = struct.unpack_from('<III', exh, 0x10)
    data_addr, _, data_size = struct.unpack_from('<III', exh, 0x30)
    full = open(os.path.join(src, 'code.bin'), 'rb').read()
    code = full[:text_size]
    lo, hi = text_addr, text_addr + text_size

    # ---- main program, ARM
    starts = bl_targets(code, lo, hi, lo)
    ptrs = code_pointers(full[(text_size + 0xFFF) & ~0xFFF:], lo, hi)
    thumb_starts = blx_thumb_targets(code, lo, hi, lo)
    funcs, reasons = [], {}

    # ---- main program, Thumb first (and the Thumb functions they call): where Thumb code is
    t_done, todo, t_cover = set(), set(t & ~1 for t in thumb_starts), set()
    while todo:
        st = todo.pop()
        if st in t_done:
            continue
        try:
            name, csrc, n, calls_t, _calls_a = thumb.translate_thumb_function(code, lo, st, set(t & ~1 for t in thumb_starts) | t_done | todo)
            funcs.append(('T', None, st, name, csrc, n))
            t_done.add(st)
            todo |= {c for c in calls_t if c not in t_done}
            labels = [int(x, 16) for x in re.findall(r' L_([0-9A-F]{8}):', csrc)]
            if labels:
                t_cover |= set(range(min(labels) & ~3, max(labels) + 4, 4))
        except Unsupported as e:
            reasons['Thumb: ' + str(e)] = reasons.get('Thumb: ' + str(e), 0) + 1
            t_done.add(st)

    # ---- main program, ARM, in two passes. 1: the functions called by BL -> which addresses are
    # surely code; only their instructions say which words are constants (literal pools)
    starts_bl = starts - {t & ~1 for t in thumb_starts} - t_cover
    code_addrs = set()
    for st in sorted(starts_bl):
        try:
            _n, csrc, _c, _calls = recomp2.translate_function(code, lo, st, starts_bl)
            code_addrs |= {int(x, 16) for x in re.findall(r' L_([0-9A-F]{8}):', csrc)}
        except Unsupported:
            pass
    literals = pc_literals_at(code, lo, code_addrs)
    # 2: more functions: pointers in the data, pointers kept as constants in the code
    # ("ldr r1, =function"), functions the main program exports to the modules (static.crs)
    lit_vals = set()
    for L in literals:
        if lo <= L < hi - 3:
            v = struct.unpack_from('<I', code, L - lo)[0]
            if lo <= v < hi and not v & 3:
                lit_vals.add(v)
    exports = static_exports(os.path.join(src, 'modulos', 'static.crs'))
    extra = set()
    for p_ in (ptrs | lit_vals | exports):
        if p_ in starts_bl or p_ in literals or p_ in t_cover or p_ & 3 or not (lo <= p_ < hi):
            continue
        w = struct.unpack_from('<I', code, p_ - lo)[0]
        if (w >> 28) != 0xE:
            continue
        if p_ in exports or (GENEROSO and (p_ in lit_vals or p_ in code_addrs)):
            extra.add(p_)
            continue
        # a constant or a pointer that lands in the middle of known code (the previous instruction
        # runs on into it) is a number, not a function: entering there natively broke the game
        # (0x004010C0, faseA2) -- so the same test as for the pointers in the data
        prev = struct.unpack_from('<I', code, p_ - lo - 4)[0]
        uncond = GENEROSO or (prev >> 28) == 0xE   # "bxeq lr" / "popne {pc}" run on into the next word
        if ((w & 0x0FFF4000) == 0x092D4000 or (uncond and is_return_or_branch(prev)) or prev in (0, 0xE1A00000, 0xE320F000)
                or (uncond and (prev & 0x0FFFFFF0) == 0x012FFF10) or (p_ - 4) in literals):
            extra.add(p_)
    starts = starts_bl | extra | {a_ for a_ in EXTRAS_MAIN if lo <= a_ < hi and not a_ & 3 and a_ not in t_cover}
    # putting the svc on a word that is data breaks the game; leaving a function out only keeps it
    # in the JIT. So: first instruction unconditional, and never a constant that the translated code
    # loads -- repeated until nothing changes (new functions bring new constants)
    starts = {a_ for a_ in starts if (struct.unpack_from('<I', code, a_ - lo)[0] >> 28) == 0xE}
    for _round in range(6):
        trans, labels = {}, set()
        for st in sorted(starts):
            try:
                name, csrc, n, _calls = recomp2.translate_function(code, lo, st, starts)
                trans[st] = (name, csrc, n)
                labels |= {int(x, 16) for x in re.findall(r' L_([0-9A-F]{8}):', csrc)}
            except Unsupported as e:
                if _round == 0:
                    reasons['ARM: ' + str(e)] = reasons.get('ARM: ' + str(e), 0) + 1
        bad = starts & pc_literals_at(code, lo, labels)
        if not bad:
            break
        reasons['ARM: era uma constante'] = reasons.get('ARM: era uma constante', 0) + len(bad)
        starts -= bad
    for st in sorted(trans):
        name, csrc, n = trans[st]
        funcs.append(('A', None, st, name, csrc, n))

    # ---- modules
    mods = []
    if with_mods:
        mdir = os.path.join(src, 'modulos')
        for f in sorted(os.listdir(mdir)):
            if not f.lower().endswith('.cro'):   # static.crs describes the main program itself
                continue
            m = cro_parse(os.path.join(mdir, f))
            if m and m['segs']:
                mods.append(m)
    for k, m in enumerate(mods):
        mlo = min(a for a, _ in m['segs'])
        mhi = max(b for _, b in m['segs'])
        m['traduzidas'] = []
        for st in sorted(m['starts']):
            try:
                name, csrc, n, _calls = recomp2.translate_function(m['dados'], 0, st, m['starts'], mod=k,
                                                                    reloc=m['reloc'], lo=mlo, hi=mhi)
                funcs.append(('M', k, st, name, csrc, n))
                m['traduzidas'].append(st)
            except Unsupported as e:
                reasons['módulo: ' + str(e)] = reasons.get('módulo: ' + str(e), 0) + 1

    done_a = {st for kind, _, st, *_ in funcs if kind == 'A'}
    done_t = {st for kind, _, st, *_ in funcs if kind == 'T'}
    done_m = {(k, st) for kind, k, st, *_ in funcs if kind == 'M'}

    def fix_calls(csrc):
        csrc = re.sub(r'@@CALLT_([0-9A-F]{8})@@',
                      lambda g: f'ft_{g.group(1)}(s)' if int(g.group(1), 16) in done_t else f'dispatch(s, 0x{int(g.group(1), 16) | 1:08X}u)', csrc)
        csrc = re.sub(r'@@CALLM(\d{3})_([0-9A-F]{8})@@',
                      lambda g: (f'm{g.group(1)}_{int(g.group(2), 16):06X}(s)' if (int(g.group(1)), int(g.group(2), 16)) in done_m
                                 else f'dispatch(s, recomp_modbase[{int(g.group(1))}] + 0x{int(g.group(2), 16):X}u)'), csrc)
        csrc = re.sub(r'@@CALL_([0-9A-F]{8})@@',
                      lambda g: f'f_{g.group(1)}(s)' if int(g.group(1), 16) in done_a else f'dispatch(s, 0x{g.group(1)}u)', csrc)
        return csrc

    # fase C: the instruction counter stays exact (one increment per instruction), but the check
    # against the time limit only where the code can come back (the entry and every goto target:
    # loops); straight code needs no check, and the exit code at each instruction bloated the DLL
    def fix(csrc):
        csrc = fix_calls(csrc)
        targets = set(re.findall(r'goto L_([0-9A-F]{8})', csrc))
        return re.sub(r'( L_([0-9A-F]{8}): )if \(\+\+s->icount > s->ilimit\) \{ s->fault = 2; s->miss = [^;]+; return; \}',
                      lambda m: m.group(0) if m.group(2) in targets else m.group(1) + '++s->icount;', csrc)

    # incremental build: a file is written only when its text changed (the compile script then
    # rebuilds only the .o older than their .c), and each file declares only the functions it uses
    def write_if_changed(path, text):
        try:
            if open(path, encoding='utf-8', errors='surrogateescape').read() == text:
                return False
        except OSError:
            pass
        open(path, 'w', encoding='utf-8', errors='surrogateescape').write(text)
        return True

    hdr = recomp2.RUNTIME_H + '\nextern uint32_t recomp_modbase[256];\n'
    write_if_changed(os.path.join(out, 'runtime.h'), hdr)
    write_if_changed(os.path.join(out, 'runtime.c'), recomp2.RUNTIME_C)
    write_if_changed(os.path.join(out, 'runtime_host.c'), recomp2.RUNTIME_HOST_C)
    protos = ''.join(f'void {name}(Cpu* s);\n' for _k, _m, _st, name, *_ in funcs)
    all_names = {name for _k, _m, _st, name, *_ in funcs}
    files, per, changed = [], 500, 0   # (500: ~0,6 GB de pico por gcc, em vez de 1,5)
    for i in range(0, len(funcs), per):
        fn = f'funcoes_{i // per:02d}.c'
        files.append(fn)
        body = ''.join(fix(csrc) + '\n\n' for *_x, csrc, _n in funcs[i:i + per])
        used = sorted(set(re.findall(r'\b(m?\d*_?f_[0-9A-Fa-f_]+|f_[0-9A-F]+|[A-Za-z_][A-Za-z0-9_]*)\(s\)', body)) & all_names)
        own = [name for _k, _m, _st, name, *_ in funcs[i:i + per]]
        decl = ''.join(f'void {n}(Cpu* s);\n' for n in sorted(set(used) | set(own)))
        changed += write_if_changed(os.path.join(out, fn), '#include "runtime.h"\n' + decl + body)
    print(f'arquivos de funções: {len(files)}, mudaram: {changed}', file=sys.stderr)

    A = sorted((st, name) for kind, _k, st, name, *_ in funcs if kind == 'A')
    Tt = sorted((st, name) for kind, _k, st, name, *_ in funcs if kind == 'T')
    with open(os.path.join(out, 'tabela.c'), 'w') as f:
        f.write('#include "runtime.h"\n' + protos)
        f.write(f'__declspec(dllexport) const int recomp_count = {len(A)};\n')
        f.write('__declspec(dllexport) const uint32_t recomp_addr[] = {' + ', '.join(f'0x{a:08X}u' for a, _ in A) + '};\n')
        f.write('__declspec(dllexport) void (*const recomp_fn[])(Cpu*) = {' + ', '.join(n for _, n in A) + '};\n')
        f.write(f'__declspec(dllexport) const uint8_t recomp_complete[{max(1, len(A))}] = {{0}};\n')
        f.write(f'__declspec(dllexport) const int recomp_t_count = {len(Tt)};\n')
        f.write('__declspec(dllexport) const uint32_t recomp_t_addr[] = {' + (', '.join(f'0x{a:08X}u' for a, _ in Tt) or '0') + '};\n')
        f.write('__declspec(dllexport) void (*const recomp_t_fn[])(Cpu*) = {' + (', '.join(n for _, n in Tt) or '0') + '};\n')
        f.write(f'__declspec(dllexport) const uint32_t recomp_code_lo = 0x{lo:08X}u, recomp_code_hi = 0x{hi:08X}u;\n')
        # modules
        f.write(f'__declspec(dllexport) const int recomp_mod_count = {len(mods)};\n')
        f.write('__declspec(dllexport) uint32_t recomp_modbase[256];\n')
        f.write('static uint32_t mod_size[256];\n')
        for k, m in enumerate(mods):
            lst = sorted(m['traduzidas'])
            f.write(f'static const uint32_t mod{k}_off[] = {{' + (', '.join(f'0x{o:X}u' for o in lst) or '0') + '};\n')
            f.write(f'static void (*const mod{k}_fn[])(Cpu*) = {{' + (', '.join(f'm{k:03d}_{o:06X}' for o in lst) or '0') + '};\n')
            # the module's own first word of each function (instructions are not relocated): lets the exe
            # put the code back right even when the memory already has our svc (after a state load)
            f.write(f'static const uint32_t mod{k}_orig[] = {{' + (', '.join('0x%08Xu' % struct.unpack_from('<I', m['dados'], o)[0] for o in lst) or '0') + '};\n')
        f.write('__declspec(dllexport) const char* const recomp_mod_name[] = {' + ', '.join(json.dumps(m['nome']) for m in mods) + '};\n')
        f.write('__declspec(dllexport) const uint32_t recomp_mod_filesize[] = {' + ', '.join(f'{m["tamanho"]}u' for m in mods) + '};\n')
        f.write('__declspec(dllexport) const int recomp_mod_nfn[] = {' + ', '.join(str(len(m['traduzidas'])) for m in mods) + '};\n')
        f.write('__declspec(dllexport) const uint32_t* const recomp_mod_off[] = {' + ', '.join(f'mod{k}_off' for k in range(len(mods))) + '};\n')
        f.write('__declspec(dllexport) const uint32_t* const recomp_mod_orig[] = {' + ', '.join(f'mod{k}_orig' for k in range(len(mods))) + '};\n')
        f.write('__declspec(dllexport) void (*const* const recomp_mod_fn[])(Cpu*) = {' + ', '.join(f'mod{k}_fn' for k in range(len(mods))) + '};\n')
        f.write(r'''
static int loaded[64], nloaded;
__declspec(dllexport) void recomp_mod_load(int k, uint32_t base, uint32_t size)
{
    recomp_modbase[k] = base; mod_size[k] = size;
    for (int i = 0; i < nloaded; i++) if (loaded[i] == k) return;
    if (nloaded < 64) loaded[nloaded++] = k;
}
__declspec(dllexport) void recomp_mod_unload(int k)
{
    mod_size[k] = 0;
    for (int i = 0; i < nloaded; i++) if (loaded[i] == k) { loaded[i] = loaded[--nloaded]; return; }
}
static int find(const uint32_t* a, int n, uint32_t t)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (a[mid] == t) return mid; if (a[mid] < t) lo = mid + 1; else hi = mid - 1; }
    return -1;
}
void dispatch(Cpu* s, uint32_t t)
{
    int i;
    if (t & 1) {   /* Thumb */
        i = find(recomp_t_addr, recomp_t_count, t & ~1u);
        if (i >= 0) { recomp_t_fn[i](s); return; }
        s->fault = 5; s->miss = t; return;
    }
    if (t >= recomp_code_lo && t < recomp_code_hi) {
        i = find(recomp_addr, recomp_count, t);
        if (i >= 0) { recomp_fn[i](s); return; }
        s->fault = 4; s->miss = t; return;
    }
    for (int j = 0; j < nloaded; j++) {
        int k = loaded[j];
        if (t - recomp_modbase[k] < mod_size[k]) {
            i = find(recomp_mod_off[k], recomp_mod_nfn[k], t - recomp_modbase[k]);
            if (i >= 0) { recomp_mod_fn[k][i](s); return; }
            break;
        }
    }
    s->fault = 4; s->miss = t;   /* not translated: in the hybrid port this goes to the JIT */
}
''')
    summary = {
        'principal_arm': {'traduzidas': len(A), 'candidatas': len(starts), 'por_ponteiro': len(extra)},
        'principal_thumb': {'traduzidas': len(Tt), 'candidatas': len(t_done)},
        'modulos': {'quantos': len(mods), 'traduzidas': sum(len(m['traduzidas']) for m in mods),
                    'candidatas': sum(len(m['starts']) for m in mods)},
        'instrucoes_traduzidas': sum(n for *_x, n in funcs),
        'arquivos_c': len(files),
        'recusadas_por_motivo': dict(sorted(reasons.items(), key=lambda x: -x[1])),
        'por_modulo': {m['nome']: [len(m['traduzidas']), len(m['starts'])] for m in mods},
    }
    json.dump(summary, open(os.path.join(out, 'traducao.json'), 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    print(json.dumps({k: v for k, v in summary.items() if k != 'por_modulo'}, indent=1, ensure_ascii=True))


if __name__ == '__main__':
    main()
