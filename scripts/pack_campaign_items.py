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
from campaign_faces import opposing_faces
from native_texture import RGB565, RGB8, texture
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
            for index,(geometry,src) in enumerate(zip(mesh.meshes,doc.meshes,strict=True)):
                node=src.node;matrix=affine(node.object_to_parent_transform)
                if node.parent_name:matrix=matrix@transforms[node.parent_name]
                transforms[node.name]=matrix
                points=np.array([(*v,1) for v in src.geometry.vertices])@matrix
                faces=defaultdict(list)
                for face in geometry.faces:
                    material=geometry.materials[face.material]
                    faces[material].append([(*points[i,:3],*geometry.coords[uv])
                                            for i,uv in zip(face.positions,face.coords,strict=True)])
                for material,triangles in faces.items():
                    batches[material].append((index,np.asarray(triangles,dtype='<f4')))
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
            runs=[];counts=[];geometry=[]
            for material,(key,parts) in enumerate(batches.items()):
                count=0
                for index,triangles in parts:
                    num=len(triangles)*3
                    # Opposite faces on the same animated node can occupy the
                    # same depth. Cull only proven pairs so thin single faces
                    # still draw from both sides.
                    if kind=='weapons' and images[key][4] in (RGB8,RGB565) and len(triangles)>1:
                        marked=np.zeros(len(triangles),dtype=bool)
                        marked[opposing_faces(triangles[...,:3].astype(np.float64))]=True
                        for cull,group in ((False,triangles[~marked]),(True,triangles[marked])):
                            if len(group):
                                runs.append((index,material,cull,len(group)*3))
                                geometry.append(group.tobytes())
                    else:
                        if kind=='weapons':runs.append((index,material,False,num))
                        geometry.append(triangles.tobytes())
                    count+=num
                counts.append(count)
            with temp.open('wb') as file:
                file.write(struct.pack('<4sII',b'OBJ1',len(batches),sum(counts)))
                for key,count in zip(batches,counts,strict=True):
                    file.write(struct.pack('<I',count));file.write(images[key])
                for vertices in geometry:file.write(vertices)
            temp.replace(dest)
            if kind=="weapons":pack_weapon_motion(read,config,base,doc,runs,tmp,dest.with_suffix(".motion"))
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
