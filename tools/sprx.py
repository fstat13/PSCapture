#!/usr/bin/env python3
"""Parser per .sprx PS4/PS5 (ET_SCE_DYNAMIC): export/import con nomi risolti via aerolib.csv."""
import struct, sys, os

def load_nid_db(p=None):
    p = p or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'aerolib.csv')
    m={}
    with open(p, encoding='utf-8', errors='replace') as f:
        for line in f:
            line=line.strip()
            if not line: continue
            parts=line.split(' ',1)
            if len(parts)==2: m[parts[0]]=parts[1]
    return m

def parse(path):
    d=open(path,'rb').read()
    phoff=struct.unpack_from('<Q',d,32)[0]
    phes=struct.unpack_from('<H',d,54)[0]
    phn=struct.unpack_from('<H',d,56)[0]
    ph=[]
    for i in range(phn):
        o=phoff+i*phes
        t,fl,off,va,pa,fsz,msz,al=struct.unpack_from('<IIQQQQQQ',d,o)
        ph.append((t,off,va,fsz,msz))
    def v2f(v):
        for t,off,va,fsz,msz in ph:
            if t==1 and va<=v<va+msz: return off+(v-va)
    dyn=[p for p in ph if p[0]==2]
    if not dyn: return None
    _,doff,_,dsz,_=dyn[0]
    strtab=symtab=hash_=None; ents=[]
    for i in range(dsz//16):
        t,v=struct.unpack_from('<QQ',d,doff+i*16)
        ents.append((t,v))
        if t==5: strtab=v2f(v)
        if t==6: symtab=v2f(v)
        if t==4: hash_=v2f(v)
    if not strtab or not symtab: return None
    def nm(off):
        e=d.find(b'\x00',strtab+off)
        return d[strtab+off:e].decode('latin1')
    # nomi delle librerie: DT_NEEDED + tag SCE con indice
    needed=[nm(v) for t,v in ents if t==1 and v<0x1000000]
    libs={}
    for t,v in ents:
        if 0x61000000<=t<=0x61ffffff:
            idx=v>>48; off=v&0xffffffff
            if off and off<0x1000000:
                try: s=nm(off)
                except Exception: continue
                if s and idx not in libs: libs[idx]=s
    nchain=struct.unpack_from('<I',d,hash_+4)[0] if hash_ else 0
    syms=[]
    for i in range(nchain):
        o=symtab+i*24
        st_name,st_info,st_other,st_shndx,st_value,st_size=struct.unpack_from('<IBBHQQ',d,o)
        if not st_name: continue
        s=nm(st_name)
        if '#' in s:
            nid,lid,mid=s.split('#',2)
        else:
            nid,lid,mid=s,'',''
        syms.append({'nid':nid,'lid':lid,'mid':mid,'info':st_info,'type':st_info&0xf,
                     'shndx':st_shndx,'value':st_value,'size':st_size})
    return {'data':d,'phdrs':ph,'needed':needed,'libs':libs,'syms':syms,'strtab':strtab}

def report(path, db):
    r=parse(path)
    if not r:
        print('%s: non e un .sprx valido' % path); return
    name=os.path.basename(path)
    print('='*78)
    print('FILE: %s' % name)
    print('  NEEDED: %s' % ', '.join(r['needed']))
    exp=[s for s in r['syms'] if s['shndx']!=0 and s['value']!=0 and s['type']==2]
    obj=[s for s in r['syms'] if s['shndx']!=0 and s['value']!=0 and s['type']==1]
    imp=[s for s in r['syms'] if s['shndx']==0]
    print('  EXPORT funzioni: %d   EXPORT oggetti: %d   IMPORT: %d' % (len(exp),len(obj),len(imp)))
    print('  --- EXPORT (funzioni) ---')
    for s in sorted(exp,key=lambda x:x['value']):
        n=db.get(s['nid'],'??')
        print('     %-12s 0x%-7x  %s' % (s['nid'], s['value'], n))
    if obj:
        print('  --- EXPORT (oggetti) ---')
        for s in sorted(obj,key=lambda x:x['value']):
            print('     %-12s 0x%-7x  %s' % (s['nid'], s['value'], db.get(s['nid'],'??')))
    print('  --- IMPORT ---')
    seen=set()
    for s in sorted(imp,key=lambda x:db.get(x['nid'],'zz')):
        n=db.get(s['nid'],'??')
        if (n,s['lid']) in seen: continue
        seen.add((n,s['lid']))
        print('     %-12s #%s#%s  %s' % (s['nid'], s['lid'], s['mid'], n))
    print()

if __name__=='__main__':
    db=load_nid_db()
    print('DB NID: %d voci' % len(db))
    for p in sys.argv[1:]:
        report(p, db)
