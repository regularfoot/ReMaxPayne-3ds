"""Convert original KF2 inventory and pickup meshes into native draw batches."""
import argparse
from collections import defaultdict
import gzip
from io import BytesIO
import json
import mmap
from pathlib import Path
import posixpath
import re
import struct
from tempfile import TemporaryDirectory

import numpy as np
from PIL import Image
from dade.maxpayne import ras
from dade.maxpayne.model import read_model
from max_payne_sdk.max_kf2 import MaxKF2Reader
from native_texture import texture
from pack_player import affine
from weapon_motion import pack_weapon_motion
from xbox_disc import archive_root

WEAPONS = ('beretta','deserteagle','pumpshotgun','sawedshotgun','jackhammer','ingram',
           'mp5','sniper','baseballbat','leadpipe','grenade','molotov','m79')


def pack(disc,source,out):
    names=set()
    for path in source.glob('*.json.gz'):
        names.update(item['item_name'].lower() for item in json.load(gzip.open(path))['items'])
    with TemporaryDirectory() as tmp,(archive_root(disc)/'x_data.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as buf:
        lookup={e.path.lower():e for e in ras.read_directory(buf).entries}
        # Enemies can spawn items that are never placed in a campaign map.
        names.update(m[1] for path in lookup if (m:=re.fullmatch(r'data/database/level_items/(\w+)\.txt',path)))
        def read(path):return ras.member_bytes(buf,lookup[path])
        def export(kind,name,model,config=None,base=None):
            data=read(model);local=Path(tmp)/Path(model).name;local.write_bytes(data)
            doc=MaxKF2Reader().parse(local);mesh=read_model(data)
            transforms={};batches=defaultdict(list);images={}
            for geometry,src in zip(mesh.meshes,doc.meshes,strict=True):
                node=src.node;matrix=affine(node.object_to_parent_transform)
                if node.parent_name:matrix=matrix@transforms[node.parent_name]
                transforms[node.name]=matrix
                points=np.array([(*v,1) for v in src.geometry.vertices])@matrix
                for face in geometry.faces:
                    material=geometry.materials[face.material]
                    for i,uv in zip(face.positions,face.coords,strict=True):
                        batches[material].append((*points[i,:3],*geometry.coords[uv]))
            for mat in batches:
                bitmap=mesh.materials[mat].lower()
                candidates=[str(Path(model).parent/'textures'/bitmap),'data/database/level_items/sharedtextures/'+bitmap,
                            'data/database/weapons/sharedtextures/'+bitmap]
                tex=next((k for k in candidates if k in lookup),None)
                if not tex:
                    matches=[k for k in lookup if k.endswith('/'+bitmap)]
                    if not matches:raise ValueError((model,bitmap))
                    tex=matches[0]
                images[mat]=texture(Image.open(BytesIO(read(tex))))
            dest=out/kind/(name+'.mesh');dest.parent.mkdir(parents=True,exist_ok=True)
            temp=dest.with_suffix('.partial')
            with temp.open('wb') as file:
                file.write(struct.pack('<4sII',b'OBJ1',len(batches),sum(map(len,batches.values()))))
                for key,vertices in batches.items():
                    file.write(struct.pack('<I',len(vertices)));file.write(images[key])
                for vertices in batches.values():file.write(np.array(vertices,dtype='<f4').tobytes())
            temp.replace(dest)
            if kind=="weapons":pack_weapon_motion(read,config,base,doc,mesh,tmp,dest.with_suffix(".motion"))
        for kind,item_names,folder in [('items',names,'level_items'),('weapons',WEAPONS,'weapons'),
                                      ('debris',('debris_blue_sign','debris_wood_crate','wood_crate'),'projectiles'),
                                      ('cases',('case_9mm','case_50cal','case_12ga','case_300cal','case_40mm'),'projectiles')]:
            for name in sorted(item_names):
                base='data/database/'+folder+'/'+name
                body=re.sub(r'//[^\r\n]*','',read(base+'.txt').decode('latin1'))
                ref=re.search(r'\bExportData\s*=\s*([^;]+);',body,re.I)[1].strip().strip('"').replace('\\','/').lower()
                export(kind,name,posixpath.normpath(base+'/'+ref),body,base)
        export('cast','rat','data/database/skins/rat/rat_l0.kfs')
        from pack_rat_motion import pack_rat
        pack_rat(read,tmp,out/'cast/rat.anim')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for arg in ('disc','source','out'):parser.add_argument(arg,type=Path)
    args=parser.parse_args();pack(args.disc,args.source,args.out)
