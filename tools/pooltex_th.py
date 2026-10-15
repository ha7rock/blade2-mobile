import sys, os, re, struct
def gob_off(x,y): return (x%64//32)*256 + (y%8//2)*64 + (x%32//16)*32 + (y%2)*16 + x%16
def bl(x,y,rb,rows,th):
    bh=1<<th; gx=(rb+63)//64; block=512*bh
    return (y//(8*bh))*gx*block + (x//64)*block + ((y%(8*bh))//8)*512 + gob_off(x,y)
def deswz(pool,rb,rows,th):
    out=bytearray(rb*rows)
    for y in range(rows):
        for x in range(0,rb,16):
            o=bl(x,y,rb,rows,th); out[y*rb+x:y*rb+x+16]=pool[o:o+16]
    return out
def c565(v): return ((v>>11)&31, (v>>5)&63, v&31)
def score(lin,rb,rows,bpb,bw):
    # per-block/pixel feature vector, then total variation between right/down neighbours
    if bw==4:
        n=rb//bpb; co = 8 if bpb==16 else 0
        feat=[[c565(struct.unpack_from('<H',lin,y*rb+x*bpb+co)[0]) for x in range(n)] for y in range(rows)]
    else:
        n=rb//bpb; feat=[[tuple(lin[y*rb+x*bpb:y*rb+x*bpb+min(bpb,3)]) for x in range(n)] for y in range(rows)]
    s=0
    for y in range(rows):
        for x in range(n):
            a=feat[y][x]
            if x+1<n: b=feat[y][x+1]; s+=sum(abs(a[i]-b[i]) for i in range(len(a)))
            if y+1<rows: b=feat[y+1][x]; s+=sum(abs(a[i]-b[i]) for i in range(len(a)))
    return s
res=[]
for fn in sorted(os.listdir(sys.argv[1])):
    m=re.match(r'(\d+)_(\d+)x(\d+)_l(\d+)_f(0x[0-9a-f]+)_fl(0x[0-9a-f]+)_b(\d+)_(\d+)_(\d+)_th(\d+)\.bin',fn)
    if not m: continue
    ser,w,h,lv,fmt,fl,bpb,bw,bh,tha=m.groups(); w,h,lv,bpb,bw,bh,tha=map(int,(w,h,lv,bpb,bw,bh,tha))
    if bpb not in (4,8,16) or (bw==4 and bpb==16 and fmt in ('0x4b','0x4c','0x4d','0x4e','0x4f')): continue
    rb=(w+bw-1)//bw*bpb; rows=(h+bh-1)//bh
    if rows<=8: continue
    pool=open(os.path.join(sys.argv[1],fn),'rb').read()
    sc={}
    for th in range(0,5):
        if (8<<th) > rows*2 and th>0: break
        try: sc[th]=score(deswz(pool,rb,rows,th),rb,rows,bpb,bw)
        except Exception as e: sc[th]=None
    best=min((v,k) for k,v in sc.items() if v is not None)[1]
    gobs=(rows+7)//8
    print(f'{w}x{h} lv{lv} fmt{fmt} bpb{bpb} rows{rows} gobs{gobs} auto th{tha} best th{best} scores {sc}')
