#!/usr/bin/env python3
"""Trova i call site di funzioni importate dentro un .sprx PS5.

In un ET_SCE_DYNAMIC le chiamate alle librerie esterne passano dalla PLT, e la
PLT e' anonima (nessun nome, nessuna sezione). Ma ogni stub PLT e' un
`jmp *disp32(%rip)` che punta a uno slot GOT, e quello slot e' descritto da una
rilocazione in .rela.plt il cui indice di simbolo punta al dynsym: li' c'e' il
NID. Quindi:

    rilocazione .rela.plt  ->  (slot GOT -> NID)
    scansione `ff 25`      ->  (stub PLT -> slot GOT)
    scansione `e8`         ->  (call site -> stub PLT)

Da li' si legge come il chiamante riempie le strutture: molto piu' veloce che
ricostruirle a tentativi sulla console.
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sprx as sprxmod


def parse_full(path):
    d = open(path, 'rb').read()
    phoff = struct.unpack_from('<Q', d, 32)[0]
    phes = struct.unpack_from('<H', d, 54)[0]
    phn = struct.unpack_from('<H', d, 56)[0]
    ph = []
    for i in range(phn):
        o = phoff + i * phes
        t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', d, o)
        ph.append((t, fl, off, va, fsz, msz))

    def v2f(v):
        for t, fl, off, va, fsz, msz in ph:
            if t == 1 and va <= v < va + msz:
                delta = v - va
                if delta < fsz:
                    return off + delta
        return None

    dyn = [p for p in ph if p[0] == 2]
    if not dyn:
        return None
    _, _, doff, _, dsz, _ = dyn[0]

    tag = {}
    ents = []
    for i in range(dsz // 16):
        t, v = struct.unpack_from('<QQ', d, doff + i * 16)
        ents.append((t, v))
        tag.setdefault(t, v)

    strtab = v2f(tag.get(5, 0))
    symtab = v2f(tag.get(6, 0))
    hash_ = v2f(tag.get(4, 0))

    def nm(off):
        e = d.find(b'\x00', strtab + off)
        return d[strtab + off:e].decode('latin1')

    nchain = struct.unpack_from('<I', d, hash_ + 4)[0] if hash_ else 0
    symnames = []
    for i in range(nchain):
        o = symtab + i * 24
        st_name = struct.unpack_from('<I', d, o)[0]
        symnames.append(nm(st_name) if st_name else '')

    return {'data': d, 'ph': ph, 'tag': tag, 'ents': ents, 'v2f': v2f,
            'symnames': symnames, 'nchain': nchain}


def plt_map(r):
    """slot GOT -> NID, dall'insieme delle rilocazioni PLT."""
    d, v2f, tag = r['data'], r['v2f'], r['tag']
    jmprel = tag.get(0x17)          # DT_JMPREL
    pltrelsz = tag.get(2) or tag.get(0x2b)  # DT_PLTRELSZ (a volte tag 2)
    out = {}
    if not jmprel or not pltrelsz:
        return out
    off = v2f(jmprel)
    if off is None:
        off = jmprel
    for i in range(pltrelsz // 24):
        o = off + i * 24
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', d, o)
        symidx = r_info >> 32
        if symidx < len(r['symnames']):
            name = r['symnames'][symidx]
            nid = name.split('#')[0] if '#' in name else name
            out[r_offset] = nid
    return out


def scan(r):
    """Ritorna (stub PLT -> NID) e (call site -> stub PLT)."""
    d, v2f, ph = r['data'], r['v2f'], r['ph']
    got2nid = plt_map(r)

    exec_segs = [p for p in ph if p[0] == 1 and (p[1] & 1)]
    stub2nid = {}
    call2stub = []

    for t, fl, off, va, fsz, msz in exec_segs:
        code = d[off:off + fsz]
        # jmp *disp32(%rip)  =  ff 25 xx xx xx xx
        for i in range(len(code) - 6):
            if code[i] == 0xff and code[i + 1] == 0x25:
                disp = struct.unpack_from('<i', code, i + 2)[0]
                a = va + i
                tgt = a + 6 + disp
                if tgt in got2nid:
                    stub2nid[a] = got2nid[tgt]
        # call rel32 = e8 xx xx xx xx
        for i in range(len(code) - 5):
            if code[i] == 0xe8:
                rel = struct.unpack_from('<i', code, i + 1)[0]
                a = va + i
                call2stub.append((a, a + 5 + rel))

    return stub2nid, call2stub, got2nid


def report(path, want_nids, ctx=14):
    r = parse_full(path)
    if not r:
        print('%s: nessun PT_DYNAMIC' % path)
        return
    stub2nid, calls, got2nid = scan(r)
    d, v2f = r['data'], r['v2f']
    name = os.path.basename(path)
    print('=' * 78)
    print('%s   stub PLT risolti: %d   call site totali: %d' %
          (name, len(stub2nid), len(calls)))

    # stub per nome
    print('--- stub PLT per le funzioni cercate ---')
    for a, nid in sorted(stub2nid.items()):
        if nid in want_nids:
            print('   0x%-7x -> %-12s %s' % (a, nid, want_nids[nid]))

    print('--- call site ---')
    for a, tgt in calls:
        nid = stub2nid.get(tgt)
        if not nid or nid not in want_nids:
            continue
        print()
        print('  ### %s  chiamata da 0x%x' % (want_nids[nid], a))
        # decodifica il contesto attorno alla chiamata
        off = v2f(a)
        if off is None:
            continue
        start = max(0, off - ctx * 8)
        lines = []
        i = start
        while i < off + 8:
            b = d[i:i + 1]
            lines.append('%08x  %s' % (i, ' '.join('%02x' % c for c in d[i:i + 8])))
            i += 8
        for l in lines:
            print('     ' + l)


if __name__ == '__main__':
    want = {
        'svzPXluOz8U': 'sceAvcap2Initialize',
        'gojqghDU+1Y': 'sceAvcap2Terminate',
        '4jwUgirLFrA': 'sceAvcap2OpenVideo',
        '8CGNCwBsItI': 'sceAvcap2Start',
        'pN0hIQZsMzc': 'sceAvcap2ReadVideo',
        'jkv9MlN7gvs': 'sceAvcap2ReadVideo2',
        'tR7gPe1i8hw': 'sceAvcap2Close',
        'YaXDckFygvw': 'sceAvcap2RequestKeyFrame',
        'Dht3tcbRoEY': 'sceAvcap2QueryVideoEncoderMemorySize',
        'X8c-f8LgzHo': 'sceAvcap2MapMemory',
        '2GC2UnBJHIA': 'sceAvcap2GetFramePitch',
        'RW88AiwmL+Y': 'sceAvcap2GetSupportInformation',
        '-NotZDC2pQc': 'sceAvcap2SetVideoEncodeConfig',
        'NeKlrUv+lbY': 'sceAvcap2GetNonVclNalUnitStream',
        'CYgd06bO5q0': 'sceAvcap2AllocateVideoEncoderMemory',
        'j7ClJpPRVQI': 'sceAvcap2GetImagePasteMemory',
    }
    for p in sys.argv[1:]:
        report(p, want)
