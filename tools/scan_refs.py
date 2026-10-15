#!/usr/bin/env python3
# -*- coding: latin-1 -*-
# Scanner grezzo di riferimenti in un ELF64 LE x86-64: trova chi materializza un
# indirizzo (stringa/rodata/vtable) via:
#   - lea reg,[rip+disp32]   (48/4C 8d, o 8d senza REX)
#   - mov reg,[rip+disp32]   (48 8b / 48 8d)
#   - valore assoluto u64 nei dati (tabelle/vtable)
#
#   python -I scan_refs.py <elf> <hexaddr> [<hexaddr> ...]
#
import struct
import sys


def load(path):
    data = open(path, 'rb').read()
    e_phoff = struct.unpack_from('<Q', data, 0x20)[0]
    e_phentsize = struct.unpack_from('<H', data, 0x36)[0]
    e_phnum = struct.unpack_from('<H', data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags = struct.unpack_from('<II', data, off)
        if p_type != 1:
            continue
        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from('<QQQQQQ', data, off + 8)
        segs.append([p_vaddr, p_offset, p_filesz, p_flags])
    return data, segs


def scan(elf, targets):
    data, segs = load(elf)
    tset = set(targets)

    # pass 1: valori assoluti u64 nei dati (vtable/tabelle con reloc pre-applicate)
    abs_hits = set()
    for vaddr, off, sz, fl in segs:
        if fl & 1:  # solo segmenti dati
            continue
        end = min(off + sz, len(data))
        for i in range(off, end - 8, 8):
            v64 = struct.unpack_from('<Q', data, i)[0]
            if v64 in tset:
                print('ABS64 @ %x = %x' % (vaddr + (i - off), v64))
                abs_hits.add(vaddr + (i - off))

    # pass 2: istruzioni x86-64 con displacement RIP-relativo
    hits = set()
    for vaddr, off, sz, fl in segs:
        if not (fl & 1):
            continue
        end = min(off + sz, len(data))
        buf = data[off:end]
        base = vaddr
        n = len(buf)
        for i in range(n - 8):
            # candidati: [REX] [8d|8b] modrm(rip)
            j = i
            rex = 0
            if buf[j] in (0x48, 0x4c, 0x49, 0x4d, 0x44, 0x41, 0x40, 0x42, 0x43, 0x45, 0x46, 0x47):
                rex = buf[j]
                j += 1
                if j >= n:
                    break
            op = buf[j]
            if op not in (0x8d, 0x8b, 0x89):
                continue
            if j + 6 > n:
                continue
            modrm = buf[j + 1]
            if (modrm & 0xC7) != 0x05:  # mod=00, rm=101 -> RIP+disp32
                continue
            # con 0x41/0x44 ecc. (REX.R/X/B) e' comunque RIP-rel se mod=00 rm=101
            disp = struct.unpack_from('<i', buf, j + 2)[0]
            ilen = (j - i) + 1 + 1 + 4
            tgt = base + i + ilen + disp
            if tgt in tset:
                reg = (modrm >> 3) & 7
                if rex & 4:
                    reg += 8
                kind = {0x8d: 'lea', 0x8b: 'mov-r', 0x89: 'mov-w'}.get(op, hex(op))
                print('CODE %s @ %x (r%d) -> %x' % (kind, base + i, reg, tgt))
                hits.add(base + i)
    print('abs hits: %d  code hits: %d' % (len(abs_hits), len(hits)))
    return hits


if __name__ == '__main__':
    elf = sys.argv[1]
    tgts = [int(x, 16) for x in sys.argv[2:]]
    print('targets: %s' % [hex(t) for t in tgts])
    scan(elf, tgts)
