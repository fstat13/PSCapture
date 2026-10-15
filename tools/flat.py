#!/usr/bin/env python3
"""Estrae un segmento PT_LOAD di un .sprx come binario piatto, per objdump."""
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
want = sys.argv[3] if len(sys.argv) > 3 else 'exec'   # exec | all
d=open(src,'rb').read()
phoff=struct.unpack_from('<Q',d,32)[0]; phes=struct.unpack_from('<H',d,54)[0]; phn=struct.unpack_from('<H',d,56)[0]
segs=[]
for i in range(phn):
    o=phoff+i*phes
    t,fl,off,va,pa,fsz,msz,al=struct.unpack_from('<IIQQQQQQ',d,o)
    if t==1: segs.append((fl,off,va,fsz,msz))
segs.sort(key=lambda s:s[2])
if want=='exec':
    sel=[s for s in segs if s[0]&1]
    if not sel: print('nessun segmento eseguibile'); sys.exit(1)
    fl,off,va,fsz,msz=sel[0]
    open(dst,'wb').write(d[off:off+fsz])
    print('ADJUST_VMA=0x%x  SIZE=0x%x' % (va,fsz))
else:
    end=max(s[2]+s[4] for s in segs)
    base=min(s[2] for s in segs)
    buf=bytearray(end-base)
    for fl,off,va,fsz,msz in segs:
        buf[va-base:va-base+fsz]=d[off:off+fsz]
    open(dst,'wb').write(bytes(buf))
    print('ADJUST_VMA=0x%x  SIZE=0x%x' % (base,len(buf)))
