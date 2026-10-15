#!/usr/bin/env python3
"""Stampa le righe di un disassemblato objdump il cui indirizzo cade in [lo,hi)."""
import re, sys
path, lo, hi = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
pat = re.compile(r'^\s*([0-9a-f]+):')
for line in open(path, errors='replace'):
    m = pat.match(line)
    if not m:
        continue
    a = int(m.group(1), 16)
    if lo <= a < hi:
        sys.stdout.write(line)
