"""Teste da tradução v2: cada função traduzida (C nativo) contra o ARM de referência
(Unicorn, ARM1136 + VFPv2 com o FPSCR do 3DS), nas mesmas entradas aleatórias.

Resultados por execução: igual, diferente, ou incompleta (o código nativo chegou numa
função ainda não traduzida / Thumb / chamada ao sistema — no porte iria para o JIT).

uso: python -I teste2.py <estudo/extraido> <pasta da tradução> [execuções por função]
"""
import ctypes
import json
import os
import random
import struct
import sys
import time

import unicorn
from unicorn import arm_const as A

MAGIC_LR = 0x0DEAD000
HEAP, HEAP_LEN = 0x08000000, 0x100000
STACK, STACK_LEN = 0x0FFE0000, 0x20000
TLS = HEAP + 0xF0000
FPSCR_3DS = 0x03C00000


class Cpu(ctypes.Structure):
    _fields_ = [('r', ctypes.c_uint32 * 16), ('n', ctypes.c_uint32), ('z', ctypes.c_uint32),
                ('c', ctypes.c_uint32), ('v', ctypes.c_uint32), ('q', ctypes.c_uint32), ('ge', ctypes.c_uint32),
                ('fault', ctypes.c_uint32), ('miss', ctypes.c_uint32), ('tls', ctypes.c_uint32),
                ('excl', ctypes.c_uint32), ('excl_addr', ctypes.c_uint32), ('fpscr', ctypes.c_uint32),
                ('s', ctypes.c_uint32 * 32), ('icount', ctypes.c_uint32), ('ilimit', ctypes.c_uint32)]


UC_R = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]
UC_S = [getattr(A, f'UC_ARM_REG_S{i}') for i in range(32)]


def page(n):
    return (n + 0xFFF) & ~0xFFF


def main():
    src, tr = sys.argv[1], sys.argv[2]
    want = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    only = sys.argv[4] if len(sys.argv) > 4 else None   # 'completas' = só árvore de chamadas completa
    exh = open(os.path.join(src, 'exheader.bin'), 'rb').read()
    text_addr, _, text_size = struct.unpack_from('<III', exh, 0x10)
    data_addr, _, data_size = struct.unpack_from('<III', exh, 0x30)
    bss = struct.unpack_from('<I', exh, 0x3C)[0]
    code = open(os.path.join(src, 'code.bin'), 'rb').read()
    ro_img = code[:data_addr - text_addr]
    rw_len = page(data_size + bss)
    rw_img = code[data_addr - text_addr:data_addr - text_addr + data_size].ljust(rw_len, b'\0')

    dll = ctypes.CDLL(os.path.join(tr, 'recomp2.dll'))
    count = ctypes.c_int.in_dll(dll, 'recomp_count').value
    addrs = (ctypes.c_uint32 * count).in_dll(dll, 'recomp_addr')
    fns = (ctypes.c_void_p * count).in_dll(dll, 'recomp_fn')
    complete = (ctypes.c_uint8 * count).in_dll(dll, 'recomp_complete')
    dll.call_native.argtypes = [ctypes.c_void_p, ctypes.POINTER(Cpu)]

    buf_ro = ctypes.create_string_buffer(ro_img, len(ro_img))
    buf_rw = ctypes.create_string_buffer(rw_len)
    buf_heap = ctypes.create_string_buffer(HEAP_LEN)
    buf_stack = ctypes.create_string_buffer(STACK_LEN)
    dll.set_window.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int]
    dll.set_window(0, text_addr, len(ro_img), ctypes.addressof(buf_ro), 0)
    dll.set_window(1, data_addr, rw_len, ctypes.addressof(buf_rw), 1)
    dll.set_window(2, HEAP, HEAP_LEN, ctypes.addressof(buf_heap), 1)
    dll.set_window(3, STACK, STACK_LEN, ctypes.addressof(buf_stack), 1)

    uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)
    try:
        uc.ctl_set_cpu_model(A.UC_CPU_ARM_1136)
    except Exception:
        pass
    uc.mem_map(text_addr, page(len(ro_img)), unicorn.UC_PROT_READ | unicorn.UC_PROT_EXEC)
    uc.mem_write(text_addr, ro_img)
    uc.mem_map(data_addr, rw_len, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
    uc.mem_map(HEAP, HEAP_LEN, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
    uc.mem_map(STACK, STACK_LEN, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
    # VFP on, as the 3DS kernel leaves it for the game
    uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)

    rnd = random.Random(4321)

    def heap_image():
        words = []
        for _ in range(HEAP_LEN // 4):
            k = rnd.random()
            words.append(HEAP + rnd.randrange(0, HEAP_LEN - 0x1000, 4) if k < 0.3 else
                         rnd.randrange(0, 64) if k < 0.5 else
                         struct.unpack('<I', struct.pack('<f', rnd.uniform(-1000, 1000)))[0] if k < 0.65 else rnd.getrandbits(32))
        return struct.pack(f'<{len(words)}I', *words)

    heaps = [heap_image() for _ in range(3)]

    def arg():
        k = rnd.random()
        if k < 0.40:
            return HEAP + rnd.randrange(0, HEAP_LEN - 0x1000, 4)
        if k < 0.55:
            return data_addr + rnd.randrange(0, max(4, data_size - 0x100), 4)
        if k < 0.80:
            return rnd.randrange(0, 64)
        return rnd.getrandbits(32)

    def fval():
        k = rnd.random()
        if k < 0.6:
            return struct.unpack('<I', struct.pack('<f', rnd.uniform(-1000, 1000)))[0]
        if k < 0.7:
            return 0
        return rnd.getrandbits(32)

    res = {'passou': [], 'falhou': [], 'so_incompleta': [], 'sem_entrada_valida': []}
    stats = {'execucoes_iguais': 0, 'execucoes_diferentes': 0, 'execucoes_incompletas': 0}
    faltando = {}
    first_fail = []
    t0 = time.time()
    # 4th argument may also be a file with addresses (hex, one per line): test only those
    only_set = set()
    if only and only != 'completas' and os.path.exists(only):
        only_set = {int(l.strip(), 16) for l in open(only) if l.strip() and '+' not in l}
    for i in range(count):
        if only == 'completas' and not complete[i]:
            continue
        addr = addrs[i]
        if only and only != 'completas' and addr not in only_set:
            continue
        same_n = diff_n = inc_n = 0
        tries = 0
        while same_n + diff_n + inc_n < want and tries < want * 4:
            tries += 1
            heap = heaps[rnd.randrange(3)]
            regs = [arg() for _ in range(4)] + [rnd.getrandbits(32) for _ in range(9)]
            fregs = [fval() for _ in range(32)]
            sp = STACK + STACK_LEN - 0x100
            uc.mem_write(data_addr, rw_img)
            uc.mem_write(HEAP, heap)
            uc.mem_write(STACK, bytes(STACK_LEN))
            for k in range(13):
                uc.reg_write(UC_R[k], regs[k])
            for k in range(32):
                uc.reg_write(UC_S[k], fregs[k])
            uc.reg_write(A.UC_ARM_REG_SP, sp)
            uc.reg_write(A.UC_ARM_REG_LR, MAGIC_LR)
            uc.reg_write(A.UC_ARM_REG_CPSR, 0x10)
            uc.reg_write(A.UC_ARM_REG_FPSCR, FPSCR_3DS)
            uc.reg_write(A.UC_ARM_REG_C13_C0_3, TLS)
            try:
                uc.emu_start(addr, MAGIC_LR, count=400000)
            except unicorn.UcError:
                continue
            if uc.reg_read(A.UC_ARM_REG_PC) != MAGIC_LR:
                continue
            cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
            ref = {
                'r': [uc.reg_read(UC_R[k]) for k in range(14)],
                's': [uc.reg_read(UC_S[k]) for k in range(32)],
                'flags': (cpsr >> 28 & 0xF, cpsr >> 27 & 1, cpsr >> 16 & 0xF),
                'fpscr': uc.reg_read(A.UC_ARM_REG_FPSCR) & 0xF3C00000,
                'rw': bytes(uc.mem_read(data_addr, rw_len)), 'heap': bytes(uc.mem_read(HEAP, HEAP_LEN)),
                'stack': bytes(uc.mem_read(STACK, STACK_LEN)),
            }
            ctypes.memmove(buf_rw, rw_img, rw_len)
            ctypes.memmove(buf_heap, heap, HEAP_LEN)
            ctypes.memset(buf_stack, 0, STACK_LEN)
            cpu = Cpu()
            for k in range(13):
                cpu.r[k] = regs[k]
            for k in range(32):
                cpu.s[k] = fregs[k]
            cpu.r[13], cpu.r[14] = sp, MAGIC_LR
            cpu.fpscr, cpu.tls = FPSCR_3DS, TLS
            cpu.ilimit = 1000000
            dll.call_native(fns[i], ctypes.byref(cpu))
            if cpu.fault in (4, 5, 6):
                inc_n += 1
                key = {4: 'função não traduzida', 5: 'Thumb', 6: 'chamada ao sistema'}[cpu.fault]
                faltando[key] = faltando.get(key, 0) + 1
                continue
            got = {
                'r': [cpu.r[k] for k in range(14)], 's': [cpu.s[k] for k in range(32)],
                'flags': ((cpu.n << 3) | (cpu.z << 2) | (cpu.c << 1) | cpu.v, cpu.q, cpu.ge),
                'fpscr': cpu.fpscr & 0xF3C00000,
                'rw': buf_rw.raw, 'heap': buf_heap.raw, 'stack': buf_stack.raw,
            }
            if cpu.fault == 0 and got == ref:
                same_n += 1
            else:
                diff_n += 1
                if len(first_fail) < 25:
                    d = [f'r{k}: arm={ref["r"][k]:08X} c={got["r"][k]:08X}' for k in range(14) if got['r'][k] != ref['r'][k]]
                    d += [f's{k}: arm={ref["s"][k]:08X} c={got["s"][k]:08X}' for k in range(32) if got['s'][k] != ref['s'][k]]
                    if got['flags'] != ref['flags']:
                        d.append(f'flags arm={ref["flags"]} c={got["flags"]}')
                    if got['fpscr'] != ref['fpscr']:
                        d.append(f'fpscr arm={ref["fpscr"]:08X} c={got["fpscr"]:08X}')
                    d += [k for k in ('rw', 'heap', 'stack') if got[k] != ref[k]]
                    first_fail.append({'funcao': f'{addr:08X}', 'fault_c': cpu.fault, 'diferencas': d[:8]})
        stats['execucoes_iguais'] += same_n
        stats['execucoes_diferentes'] += diff_n
        stats['execucoes_incompletas'] += inc_n
        key = ('falhou' if diff_n else 'passou' if same_n else 'so_incompleta' if inc_n else 'sem_entrada_valida')
        res[key].append(f'{addr:08X}')
        if (i + 1) % 500 == 0:
            print(f'{i + 1}/{count}  ' + ' '.join(f'{k}={len(v)}' for k, v in res.items()) + f'  {time.time() - t0:.0f}s', flush=True)

    summary = {k: len(v) for k, v in res.items()}
    summary.update(stats)
    summary['incompletas_por_motivo'] = faltando
    summary['execucoes_por_funcao'] = want
    summary['segundos'] = round(time.time() - t0)
    summary['primeiras_falhas'] = first_fail
    json.dump({'resumo': summary, 'listas': res}, open(os.path.join(tr, 'resultado_teste2.json'), 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    print(json.dumps({k: v for k, v in summary.items() if k != 'primeiras_falhas'}, ensure_ascii=True))
    for f in first_fail[:12]:
        print(json.dumps(f, ensure_ascii=True))


if __name__ == '__main__':
    main()
