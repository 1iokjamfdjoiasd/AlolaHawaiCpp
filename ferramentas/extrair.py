"""Extrai de uma ROM de 3DS descriptografada (.3ds / NCSD) só o que o estudo precisa:
o exheader, o code.bin (descompactado), a lista de arquivos do RomFS e os módulos
.cro / .crs / .crr. A ROM é só lida; nada é gravado nela.

uso: python -I extrair.py <rom.3ds> <pasta de saída>
"""
import os
import struct
import sys

MU = 0x200  # "media unit"


def lzss_decompress(comp: bytes) -> bytes:
    """Compressão "de trás para frente" do code.bin (igual ao LZSS_Decompress do Citra/Azahar)."""
    size = len(comp)
    extra = struct.unpack_from('<I', comp, size - 4)[0]
    top_bottom = struct.unpack_from('<I', comp, size - 8)[0]
    out_size = size + extra
    out = bytearray(out_size)
    out[:size] = comp
    o = out_size
    index = size - ((top_bottom >> 24) & 0xFF)
    stop = size - (top_bottom & 0xFFFFFF)
    while index > stop:
        index -= 1
        control = comp[index]
        for _ in range(8):
            if index <= stop or index <= 0 or o <= 0:
                break
            if control & 0x80:
                index -= 2
                seg = comp[index] | (comp[index + 1] << 8)
                seg_size = ((seg >> 12) & 15) + 3
                seg_off = (seg & 0x0FFF) + 2
                for _ in range(seg_size):
                    src = o + seg_off   # read before moving back (as the original does)
                    o -= 1
                    out[o] = out[src]
            else:
                o -= 1
                index -= 1
                out[o] = comp[index]
            control = (control << 1) & 0xFF
    return bytes(out)


def main():
    rom_path, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    rom = open(rom_path, 'rb')

    hdr = rom.read(0x200)
    assert hdr[0x100:0x104] == b'NCSD', 'não é uma ROM .3ds (NCSD)'
    part0 = struct.unpack_from('<I', hdr, 0x120)[0] * MU
    rom.seek(part0)
    ncch = rom.read(0x200)
    assert ncch[0x100:0x104] == b'NCCH'
    assert ncch[0x18F] & 4, 'a ROM está criptografada: precisa da versão descriptografada'
    program_id = struct.unpack_from('<Q', ncch, 0x118)[0]
    report = [f'program_id={program_id:016X}', f'product={ncch[0x150:0x160].rstrip(bytes(1)).decode()}']

    # exheader (system control info)
    ex_size = struct.unpack_from('<I', ncch, 0x180)[0]
    rom.seek(part0 + 0x200)
    exh = rom.read(0x800)
    open(os.path.join(out_dir, 'exheader.bin'), 'wb').write(exh)
    name = exh[0:8].rstrip(b'\0').decode(errors='replace')
    flags = exh[0x0D]
    text = struct.unpack_from('<III', exh, 0x10)
    ro = struct.unpack_from('<III', exh, 0x20)
    data = struct.unpack_from('<III', exh, 0x30)
    bss = struct.unpack_from('<I', exh, 0x3C)[0]
    report += [f'exheader_size=0x{ex_size:X} nome={name} code_comprimido={flags & 1}',
               f'.text endereço=0x{text[0]:08X} páginas={text[1]} tamanho=0x{text[2]:X}',
               f'.rodata endereço=0x{ro[0]:08X} páginas={ro[1]} tamanho=0x{ro[2]:X}',
               f'.data endereço=0x{data[0]:08X} páginas={data[1]} tamanho=0x{data[2]:X}',
               f'.bss tamanho=0x{bss:X}']

    # ExeFS: .code (code.bin), icon, banner
    exefs_off = struct.unpack_from('<I', ncch, 0x1A0)[0] * MU + part0
    rom.seek(exefs_off)
    eh = rom.read(0x200)
    for i in range(10):
        n = eh[i * 16:i * 16 + 8].rstrip(b'\0').decode()
        off, sz = struct.unpack_from('<II', eh, i * 16 + 8)
        if not n:
            continue
        rom.seek(exefs_off + 0x200 + off)
        blob = rom.read(sz)
        report.append(f'exefs {n}: 0x{sz:X} bytes')
        if n == '.code':
            open(os.path.join(out_dir, 'code_comprimido.bin'), 'wb').write(blob)
            code = lzss_decompress(blob) if flags & 1 else blob
            open(os.path.join(out_dir, 'code.bin'), 'wb').write(code)
            report.append(f'code.bin descompactado: 0x{len(code):X} bytes')

    # RomFS: IVFC level 3
    romfs_off = struct.unpack_from('<I', ncch, 0x1B0)[0] * MU + part0
    rom.seek(romfs_off)
    iv = rom.read(0x60)
    assert iv[0:4] == b'IVFC'
    master = struct.unpack_from('<I', iv, 0x08)[0]
    l3_log2 = struct.unpack_from('<I', iv, 0x4C)[0]
    blk = 1 << l3_log2
    l3 = romfs_off + ((0x60 + master + blk - 1) // blk) * blk
    rom.seek(l3)
    l3h = rom.read(0x28)
    (hl, dh_off, dh_len, dm_off, dm_len, fh_off, fh_len, fm_off, fm_len, fd_off) = struct.unpack_from('<10I', l3h, 0)
    rom.seek(l3 + dm_off)
    dmeta = rom.read(dm_len)
    rom.seek(l3 + fm_off)
    fmeta = rom.read(fm_len)

    files = []

    def walk_dir(off, path):
        parent, sibling, child, first_file, _hash, nlen = struct.unpack_from('<6I', dmeta, off)
        nm = dmeta[off + 0x18:off + 0x18 + nlen].decode('utf-16le')
        here = path + '/' + nm if off else ''
        f = first_file
        while f != 0xFFFFFFFF:
            _p, fsib, doff, dsize, _h, fnl = struct.unpack_from('<IIQQII', fmeta, f)
            fname = fmeta[f + 0x20:f + 0x20 + fnl].decode('utf-16le')
            files.append((here + '/' + fname, doff, dsize))
            f = fsib
        c = child
        while c != 0xFFFFFFFF:
            walk_dir(c, here)
            c = struct.unpack_from('<I', dmeta, c + 4)[0]

    walk_dir(0, '')
    total = sum(s for _, _, s in files)
    with open(os.path.join(out_dir, 'romfs_lista.txt'), 'w', encoding='utf-8') as fl:
        for p, o, s in files:
            fl.write(f'{s:12d}  {p}\n')
    report.append(f'romfs: {len(files)} arquivos, {total / 2**20:.0f} MB')

    mods = [x for x in files if x[0].lower().endswith(('.cro', '.crs', '.crr'))]
    mdir = os.path.join(out_dir, 'modulos')
    os.makedirs(mdir, exist_ok=True)
    for p, o, s in mods:
        rom.seek(l3 + fd_off + o)
        dst = os.path.join(mdir, p.lstrip('/').replace('/', '__'))
        open(dst, 'wb').write(rom.read(s))
    report.append(f'módulos extraídos: {len(mods)} ({sum(s for _, _, s in mods) / 2**20:.1f} MB)')

    open(os.path.join(out_dir, 'resumo.txt'), 'w', encoding='utf-8').write('\n'.join(report) + '\n')
    print('\n'.join(report))


if __name__ == '__main__':
    main()
