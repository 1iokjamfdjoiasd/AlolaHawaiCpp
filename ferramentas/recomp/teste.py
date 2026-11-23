"""Compara cada função traduzida (C nativo, DLL) com o ARM de referência (Unicorn, ARM1136)
nas mesmas entradas aleatórias. Escreve <saída>/resultado_teste.json e .md.

uso: python -I teste.py <estudo/extraido> <pasta da tradução> [execuções válidas por função]
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
STACK, STACK_LEN = 0x0FFF0000, 0x10000


class Cpu(ctypes.Structure):
    _fields_ = [('r', ctypes.c_uint32 * 16), ('n', ctypes.c_uint32), ('z', ctypes.c_uint32),
                ('c', ctypes.c_uint32), ('v', ctypes.c_uint32), ('fault', ctypes.c_uint32)]


UC_REGS = [A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3, A.UC_ARM_REG_R4,
           A.UC_ARM_REG_R5, A.UC_ARM_REG_R6, A.UC_ARM_REG_R7, A.UC_ARM_REG_R8, A.UC_ARM_REG_R9,
           A.UC_ARM_REG_R10, A.UC_ARM_REG_R11, A.UC_ARM_REG_R12, A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]


def page(n):
    return (n + 0xFFF) & ~0xFFF


def main():
    src, tr = sys.argv[1], sys.argv[2]
    want = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    exh = open(os.path.join(src, 'exheader.bin'), 'rb').read()
    text_addr, _, text_size = struct.unpack_from('<III', exh, 0x10)
    ro_addr, _, ro_size = struct.unpack_from('<III', exh, 0x20)
    data_addr, _, data_size = struct.unpack_from('<III', exh, 0x30)
    bss = struct.unpack_from('<I', exh, 0x3C)[0]
    code = open(os.path.join(src, 'code.bin'), 'rb').read()
    ro_end = data_addr                          # .text + .rodata: read only
    ro_img = code[:ro_end - text_addr]
    rw_len = page(data_size + bss)
    rw_img = code[data_addr - text_addr:data_addr - text_addr + data_size].ljust(rw_len, b'\0')

    dll = ctypes.CDLL(os.path.join(tr, 'recomp_teste.dll'))
    count = ctypes.c_int.in_dll(dll, 'recomp_count').value
    addrs = (ctypes.c_uint32 * count).in_dll(dll, 'recomp_addr')
    FN = ctypes.CFUNCTYPE(None, ctypes.POINTER(Cpu))
    fns = (ctypes.c_void_p * count).in_dll(dll, 'recomp_fn')

    # C memory windows
    buf_ro = ctypes.create_string_buffer(ro_img, len(ro_img))
    buf_rw = ctypes.create_string_buffer(rw_len)
    buf_heap = ctypes.create_string_buffer(HEAP_LEN)
    buf_stack = ctypes.create_string_buffer(STACK_LEN)
    dll.set_window.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int]
    dll.set_window(0, text_addr, len(ro_img), ctypes.addressof(buf_ro), 0)
    dll.set_window(1, data_addr, rw_len, ctypes.addressof(buf_rw), 1)
    dll.set_window(2, HEAP, HEAP_LEN, ctypes.addressof(buf_heap), 1)
    dll.set_window(3, STACK, STACK_LEN, ctypes.addressof(buf_stack), 1)

    # reference ARM
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

    rnd = random.Random(1234)

    def heap_image():
        words = []
        for _ in range(HEAP_LEN // 4):
            k = rnd.random()
            words.append(HEAP + rnd.randrange(0, HEAP_LEN - 0x1000, 4) if k < 0.3 else
                         rnd.randrange(0, 64) if k < 0.5 else rnd.getrandbits(32))
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

    results = {'passou': [], 'falhou': [], 'sem_entrada_valida': []}
    first_fail = []
    t0 = time.time()
    for i in range(count):
        addr = addrs[i]
        fn = FN(fns[i])
        valid = bad = 0
        tries = 0
        while valid < want and tries < want * 4:
            tries += 1
            heap = heaps[rnd.randrange(3)]
            regs = [arg() for _ in range(4)] + [rnd.getrandbits(32) for _ in range(9)]
            sp = STACK + STACK_LEN - 0x100
            # reference
            uc.mem_write(data_addr, rw_img)
            uc.mem_write(HEAP, heap)
            uc.mem_write(STACK, bytes(STACK_LEN))
            for k in range(13):
                uc.reg_write(UC_REGS[k], regs[k])
            uc.reg_write(A.UC_ARM_REG_SP, sp)
            uc.reg_write(A.UC_ARM_REG_LR, MAGIC_LR)
            uc.reg_write(A.UC_ARM_REG_CPSR, 0x10)   # user mode, flags clear
            try:
                uc.emu_start(addr, MAGIC_LR, count=200000)
            except unicorn.UcError:
                continue
            if uc.reg_read(A.UC_ARM_REG_PC) != MAGIC_LR:
                continue
            ref_regs = [uc.reg_read(UC_REGS[k]) for k in range(14)]
            ref_rw = bytes(uc.mem_read(data_addr, rw_len))
            ref_heap = bytes(uc.mem_read(HEAP, HEAP_LEN))
            ref_stack = bytes(uc.mem_read(STACK, STACK_LEN))
            # native
            ctypes.memmove(buf_rw, rw_img, rw_len)
            ctypes.memmove(buf_heap, heap, HEAP_LEN)
            ctypes.memset(buf_stack, 0, STACK_LEN)
            cpu = Cpu()
            for k in range(13):
                cpu.r[k] = regs[k]
            cpu.r[13], cpu.r[14] = sp, MAGIC_LR
            fn(ctypes.byref(cpu))
            valid += 1
            got = [cpu.r[k] for k in range(14)]
            same = (cpu.fault == 0 and got == ref_regs and buf_rw.raw == ref_rw and
                    buf_heap.raw == ref_heap and buf_stack.raw == ref_stack)
            if not same:
                bad += 1
                if len(first_fail) < 15:
                    diff = [f'r{k}: arm={ref_regs[k]:08X} c={got[k]:08X}' for k in range(14) if got[k] != ref_regs[k]]
                    mem = [n for n, a, b in (('data', buf_rw.raw, ref_rw), ('heap', buf_heap.raw, ref_heap),
                                             ('pilha', buf_stack.raw, ref_stack)) if a != b]
                    first_fail.append({'funcao': f'{addr:08X}', 'fault_c': cpu.fault, 'regs': diff[:6], 'memoria': mem})
        if valid == 0:
            results['sem_entrada_valida'].append(f'{addr:08X}')
        elif bad:
            results['falhou'].append(f'{addr:08X}')
        else:
            results['passou'].append(f'{addr:08X}')
        if (i + 1) % 200 == 0:
            print(f'{i + 1}/{count} funções  passou={len(results["passou"])} falhou={len(results["falhou"])} '
                  f'sem entrada={len(results["sem_entrada_valida"])}  {time.time() - t0:.0f}s', flush=True)

    summary = {k: len(v) for k, v in results.items()}
    summary['execucoes_validas_por_funcao'] = want
    summary['primeiras_falhas'] = first_fail
    summary['segundos'] = round(time.time() - t0)
    json.dump({'resumo': summary, 'listas': results}, open(os.path.join(tr, 'resultado_teste.json'), 'w', encoding='utf-8'), indent=1)
    print(json.dumps({k: v for k, v in summary.items() if k != 'primeiras_falhas'}))
    for f in first_fail[:8]:
        print(f)


if __name__ == '__main__':
    main()
