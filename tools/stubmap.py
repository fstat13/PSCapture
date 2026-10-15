#!/usr/bin/env python3
"""Stampa la mappa completa stub PLT -> nome import, per un .sprx."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import callsites, sprx
db = sprx.load_nid_db()
r = callsites.parse_full(sys.argv[1])
stub2nid, calls, got2nid = callsites.scan(r)
for a, nid in sorted(stub2nid.items()):
    print('0x%-6x %-14s %s' % (a, nid, db.get(nid, '?')))
