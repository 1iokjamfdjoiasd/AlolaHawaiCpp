"""Compara o RomFS de duas ROMs de 3DS descriptografadas (.3ds): quais arquivos têm conteúdo diferente.
As ROMs são só lidas.

uso: python -I comparar_romfs.py <base.3ds> <outra.3ds> <saída.txt>
saída: uma linha por arquivo diferente ou só na outra: "<tamanho>  <caminho>"; no fim, o total.
"""
import hashlib
import struct
import sys

MU = 0x200


def romfs_files(path):
    """[(caminho, deslocamento absoluto no arquivo, tamanho)] do RomFS da partição 0"""
    rom = open(path, 'rb')
    h = rom.read(0x200)
    assert h[0x100:0x104] == b'NCSD', 'não é .3ds (NCSD)'
    part0 = struct.unpack_from('<I', h, 0x120)[0] * MU
    rom.seek(part0)
    ncch = rom.read(0x200)
    assert ncch[0x100:0x104] == b'NCCH'
    romfs_off = struct.unpack_from('<I', ncch, 0x1B0)[0] * MU + part0
    rom.seek(romfs_off)
    iv = rom.read(0x60)
    assert iv[0:4] == b'IVFC'
    master = struct.unpack_from('<I', iv, 0x08)[0]
    blk = 1 << struct.unpack_from('<I', iv, 0x4C)[0]
    l3 = romfs_off + ((0x60 + master + blk - 1) // blk) * blk
    rom.seek(l3)
    (hl, dh_off, dh_len, dm_off, dm_len, fh_off, fh_len, fm_off, fm_len, fd_off) = struct.unpack_from('<10I', rom.read(0x28), 0)
    rom.seek(l3 + dm_off)
    dmeta = rom.read(dm_len)
    rom.seek(l3 + fm_off)
    fmeta = rom.read(fm_len)
    files = []

    def walk_dir(off, here):
        _p, _s, child, first_file, _h, nlen = struct.unpack_from('<6I', dmeta, off)
        nm = dmeta[off + 0x18:off + 0x18 + nlen].decode('utf-16le')
        here = here + '/' + nm if off else ''
        f = first_file
        while f != 0xFFFFFFFF:
            _pp, fsib, doff, dsize, _hh, fnl = struct.unpack_from('<IIQQII', fmeta, f)
            files.append((here + '/' + fmeta[f + 0x20:f + 0x20 + fnl].decode('utf-16le'), l3 + fd_off + doff, dsize))
            f = fsib
        c = child
        while c != 0xFFFFFFFF:
            walk_dir(c, here)
            c = struct.unpack_from('<I', dmeta, c + 4)[0]

    walk_dir(0, '')
    rom.close()
    return files


def hashes(path, files):
    out = {}
    with open(path, 'rb') as rom:
        for p, off, size in files:
            rom.seek(off)
            h = hashlib.sha1()
            left = size
            while left > 0:
                b = rom.read(min(left, 1 << 22))
                if not b:
                    break
                h.update(b)
                left -= len(b)
            out[p] = (size, h.hexdigest())
    return out


def main():
    base, other, out = sys.argv[1], sys.argv[2], sys.argv[3]
    fa, fb = romfs_files(base), romfs_files(other)
    ha, hb = hashes(base, fa), hashes(other, fb)
    diff = [(hb[p][0], p) for p in sorted(hb) if p not in ha or ha[p][1] != hb[p][1]]
    total_b = sum(s for s, _ in hb.values())
    total_d = sum(s for s, _ in diff)
    with open(out, 'w', encoding='utf-8') as f:
        for s, p in diff:
            f.write(f'{s:12d}  {p}\n')
        f.write(f'# {len(diff)} de {len(hb)} arquivos diferentes: {total_d / 2**20:.1f} MB de {total_b / 2**20:.1f} MB\n')
    print(f'{len(diff)} de {len(hb)} arquivos diferentes: {total_d / 2**20:.1f} MB de {total_b / 2**20:.1f} MB')


if __name__ == '__main__':
    main()
