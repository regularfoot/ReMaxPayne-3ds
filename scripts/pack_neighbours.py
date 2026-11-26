"""Link real PAL room meshes to their first-hop portal polygons for native rendering."""
from io import BytesIO
from pathlib import Path
import re
import struct
from tempfile import TemporaryDirectory

import numpy as np
from PIL import Image
from dade.maxpayne.model import read_model
from max_payne_sdk.max_kf2 import MaxKF2Reader
from max_payne_sdk.max_type import parseType
from native_texture import TEXEL_BYTES


def portals(data):
    result={}
    for tag in re.finditer(rb'::\w+::Exit\d+(?=[\x11\x14].\x16)',data,re.I|re.S):
        stream=BytesIO(data)
        stream.seek(tag.end())
        polygon=np.array([parseType(stream) for _ in range(parseType(stream))],dtype=float)
        normal,transform,source,dest,name=[parseType(stream) for _ in range(5)]
        result.setdefault(source,[]).append((polygon,normal,transform[3],dest,name.split('::')[1]))
    return result


def worldsphere(archive,read,name):
    folder='data/database/worldspheres/'
    buf=read(archive,folder+'bg_'+name+'.kf2')
    model=read_model(buf)
    with TemporaryDirectory() as foldername:
        kf=Path(foldername)/'sky.kf2'
        kf.write_bytes(buf)
        nodes=MaxKF2Reader().parse(kf).meshes
    result=[]
    for mesh,node in zip(model.meshes,nodes,strict=True):
        transform=np.asarray(node.node.object_to_parent_transform)
        pos=np.asarray(node.geometry.vertices)@transform[:3]+transform[3]
        for mat,material in enumerate(mesh.materials):
            image=Image.open(BytesIO(read(archive,folder+'textures/'+model.materials[material]))).convert('RGBA')
            verts=bytearray()
            for face in mesh.faces:
                if face.material!=mat: continue
                for i,j in zip(face.positions,face.coords,strict=True):
                    verts.extend(struct.pack('<7f',*pos[i],*mesh.coords[j],.5,.5))
            result.append((material,image,verts))
    return result


def read_graphics(path):
    data=path.read_bytes()
    _,nt,nb,nv,nc=struct.unpack_from('<4s4I',data)
    at=424;textures=[]
    for _ in range(nt):
        w,h,fmt,m,_,_=struct.unpack_from('<HHBBBB',data,at)
        end=at+8+sum((w>>n)*(h>>n)*TEXEL_BYTES[fmt] for n in range(m+1))
        textures.append(data[at:end]);at=end
    batches=list(struct.iter_unpack('<4I',data[at:at+16*nb]));at+=16*nb
    vertices=np.frombuffer(data,dtype='<f4',count=nv*7,offset=at).reshape(-1,7)
    at+=nv*28
    if len(data)!=at+40*nc:raise ValueError('Invalid room payload')
    return data[:424],textures,batches,vertices,data[at:]


def link_room(raw,neighbours,ids,first,path):
    header,textures,batches,vertices,collision=read_graphics(raw)
    vertices=[vertices.tobytes()]
    lookup={t:i for i,t in enumerate(textures)}
    info=[]
    for slot,(polygon,normal,offset,dest,_) in enumerate(neighbours):
        _,images,groups,mesh,_=read_graphics(raw.parent/f'{dest}.room')
        group_base=8+slot*8
        mask=[]
        for j in range(1,len(polygon)-1):
            for k in (0,j,j+1):mask.append((*polygon[k],0,0,0,0))
        # White texture 0 also makes the aperture mask independent of alpha cutouts.
        batches.append((0,0,len(mask),group_base+5))
        vertices.append(np.asarray(mask,dtype='<f4').tobytes())
        remap={}
        start=0
        for tex,light,count,state in groups:
            if state<5:
                for idx in (tex,light):
                    if idx not in remap:
                        img=images[idx]
                        if img not in lookup:
                            lookup[img]=len(textures);textures.append(img)
                        remap[idx]=lookup[img]
                local=mesh[start:start+count].copy()
                local[:,:3]-=offset
                batches.append((remap[tex],remap[light],count,group_base+state))
                vertices.append(local.tobytes())
            start+=count
        center=(polygon.max(axis=0)+polygon.min(axis=0))*.5
        dest_id=first+ids.index(dest) if dest in ids else -1
        info.append(struct.pack('<6fi',*center,*normal,dest_id))
    header=bytearray(header)
    struct.pack_into('<4s3I',header,0,b'MPR7',len(textures),len(batches),sum(len(v)//28 for v in vertices))
    with path.open('wb') as out:
        out.write(header);out.writelines(textures)
        out.writelines(struct.pack('<4I',*b) for b in batches)
        out.writelines(vertices);out.write(collision)
        out.write(struct.pack('<I',len(info)));out.writelines(info)
    print(f'{path.stem}: {len(neighbours)} adjacent rooms, {len(textures)} shared textures, '
          f'{len(batches)} batches, {sum(map(len,vertices))//84} triangles, {path.stat().st_size} bytes',flush=True)
