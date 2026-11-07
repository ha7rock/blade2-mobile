#!/usr/bin/env python3
"""Read-only audit of uncompressed NSO modules. No keys or game data are exported.
Usage: python3 inspect_nso.py MODULE_DIR D2R_SOURCE OUTPUT_JSON
First decompress modules with hactool -t nso0 --uncompressed=DEST SOURCE.
Coverage is static symbol matching, NOT behavioral compatibility.
"""
import hashlib,json,re,struct,sys
from pathlib import Path

def inspect(path):
    raw=path.read_bytes()
    if raw[:4]!=b'NSO0': raise ValueError(f'{path.name}: not NSO0')
    if struct.unpack_from('<I',raw,12)[0]&7: raise ValueError('Compressed NSO unsupported; decompress first')
    segments=[struct.unpack_from('<III',raw,16+i*16) for i in range(3)]
    size=max(m+n for _,m,n in segments)
    if size>1024**3: raise ValueError('Unreasonable image size')
    img=bytearray(size)
    for off,mem,n in segments:
        if off+n>len(raw): raise ValueError('Truncated segment')
        img[mem:mem+n]=raw[off:off+n]
    mod=struct.unpack_from('<I',img,4)[0]
    if img[mod:mod+4]!=b'MOD0': raise ValueError('Missing MOD0')
    pos=mod+struct.unpack_from('<i',img,mod+4)[0]
    dyn={}
    for _ in range(4096):
        tag,val=struct.unpack_from('<qQ',img,pos);pos+=16
        if not tag: break
        dyn[tag]=val
    else: raise ValueError('Unterminated dynamic table')
    syms,strings=dyn[6],dyn[5]
    stride=dyn.get(11,24)
    if stride!=24 or strings<syms or (strings-syms)%24: raise ValueError('Unsupported symbol layout')
    count=(strings-syms)//24
    if 4 in dyn:
        count=struct.unpack_from('<II',img,dyn[4])[1]
    imports=[];exports=[]
    for i in range(count):
        name,info,other,section,value,n=struct.unpack_from('<IBBHQQ',img,syms+i*24)
        if not name: continue
        a=strings+name
        if a>=len(img): raise ValueError('Symbol name out of range')
        z=img.find(0,a)
        if z<0: raise ValueError('Unterminated symbol')
        text=img[a:z].decode('utf-8',errors='replace')
        if section: exports.append(text)
        else: imports.append(text)
    return {'module':path.name,'sha256':hashlib.sha256(raw).hexdigest(),
        'build_id':raw[64:96].hex(),'text_bytes':segments[0][2],
        'image_bytes_without_bss':size,'bss_bytes':struct.unpack_from('<I',raw,60)[0],
        'imports':sorted(set(imports)),'exports':sorted(set(exports)),
        'engine_marker_UE4_4_17':b'++UE4+Release-4.17' in img}

def main():
    source,reference,dest=map(Path,sys.argv[1:])
    modules=[inspect(source/n) for n in ('rtld','main','subsdk0','sdk') if (source/n).exists()]
    if not modules: raise ValueError('No modules found')
    registered=set()
    for folder in ('host','gfx'):
        for p in (reference/'dm'/folder).rglob('*'):
            if p.suffix in ('.c','.m','.h'):
                registered.update(re.findall(r'\bHLE(?:_FORCE)?\s*\(\s*"([^"]+)"',p.read_text(errors='replace')))
    exports=set().union(*(set(m['exports']) for m in modules))
    for m in modules:
        imported=set(m['imports'])
        m['d2r_hle_name_matches']=sorted(imported&registered)
        m['resolved_by_guest_module']=sorted(imported&exports)
        m['neither_guest_export_nor_literal_hle']=sorted(imported-exports-registered)
    result={'scope':'Static names only; macro-generated HLE registrations may be missed; no runtime compatibility claim.',
            'modules':modules}
    dest.write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n')
    for m in modules:
        print(m['module'],'text_MiB',round(m['text_bytes']/2**20,2),'imports',len(m['imports']),
              'HLE_name_matches',len(m['d2r_hle_name_matches']),
              'unmatched',len(m['neither_guest_export_nor_literal_hle']))
if __name__=='__main__':main()
