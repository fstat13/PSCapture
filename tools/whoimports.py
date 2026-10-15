import sys, glob, os
sys.path.insert(0, os.path.expanduser('~/ps5-research/tools'))
import sprx
TARGETS = {
 'svzPXluOz8U':'sceAvcap2Initialize','gojqghDU+1Y':'sceAvcap2Terminate',
 '4jwUgirLFrA':'sceAvcap2OpenVideo','8CGNCwBsItI':'sceAvcap2Start',
 'pN0hIQZsMzc':'sceAvcap2ReadVideo','jkv9MlN7gvs':'sceAvcap2ReadVideo2',
 'tR7gPe1i8hw':'sceAvcap2Close','YaXDckFygvw':'sceAvcap2RequestKeyFrame',
 'Dht3tcbRoEY':'sceAvcap2QueryVideoEncoderMemorySize','X8c-f8LgzHo':'sceAvcap2MapMemory',
 '2GC2UnBJHIA':'sceAvcap2GetFramePitch','RW88AiwmL+Y':'sceAvcap2GetSupportInformation',
 '-NotZDC2pQc':'sceAvcap2SetVideoEncodeConfig','NeKlrUv+lbY':'sceAvcap2GetNonVclNalUnitStream',
 '0zsTiDhM0nU':'IPMI::Client::create','svzPXluOz8U':'sceAvcap2Initialize',
}
for p in sorted(glob.glob(os.path.join(os.path.expanduser('~/ps5-research/disklibs'),'*.sprx'))):
    try: r = sprx.parse(p)
    except Exception as e: continue
    if not r: continue
    hits=[]
    for s in r['syms']:
        if s['shndx']==0 and s['nid'] in TARGETS:
            hits.append(TARGETS[s['nid']])
    if hits:
        print('%-42s importa: %s' % (os.path.basename(p), ', '.join(sorted(set(hits)))))
