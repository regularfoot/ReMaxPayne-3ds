"""Convert the actor skins named by the campaign into native skeletal meshes."""
import argparse
import gzip
import json
import mmap
from pathlib import Path
import re
from dade.maxpayne import ras
from native_texture import texture
from pack_player import pack_player


def pack(disc,source,out):
    out=out/'cast';out.mkdir(exist_ok=True,parents=True)
    skins=set()
    for path in source.glob('*.json.gz'):
        skins.update(x['character_name'].lower() for x in json.load(gzip.open(path))['characters'])
    with (disc/'english/x_data.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        lookup={e.path.lower():e for e in ras.read_directory(data).entries}
        def read(path):return ras.member_bytes(data,lookup[path.lower()])
        errors=[]
        for skin in sorted(skins):
            if skin=='rat':
                # Its pigrat skeleton is independent of the human bone palette.
                continue
            dest=out/(skin+'.actor')
            if dest.is_file():continue
            config=read('data/database/skins/'+skin+'.txt').decode('latin1')
            filenames=re.findall(r'[^\s;="]+\.kfs',config,re.I)
            folder='data/database/skins/'+skin+'/'
            matches=[k[:-4] for k in lookup if k.startswith(folder) and k.endswith('.skd')]
            if not matches:
                # Some character types refer to a shared skin folder.
                matches=[k[:-4] for k in lookup if k.endswith('.skd') and
                         any(Path(n.replace(chr(92),'/')).stem.lower()==Path(k).stem for n in filenames)]
            if not matches:
                errors.append((skin,'no matching skin'));continue
            src=Path(matches[0]);temp=dest.with_suffix('.partial')
            try:
                pack_player(read,texture,temp,enemy=True,skin_model=(str(src.parent)+'/',src.name))
                temp.replace(dest)
            except (KeyError,ValueError,IndexError) as error:
                errors.append((skin,str(error)));temp.unlink(missing_ok=True)
        if errors:raise ValueError(errors)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for a in ('disc','source','out'):p.add_argument(a,type=Path)
    a=p.parse_args();pack(a.disc,a.source,a.out)
