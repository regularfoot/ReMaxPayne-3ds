"""Map collision faces to PAL footstep materials and bake room light probes and static bounds."""
import argparse
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from functools import lru_cache
from io import BytesIO
from pathlib import Path
import struct
from tempfile import TemporaryDirectory

import numpy as np
from PIL import Image
from campaign_materials import material_rules
from campaign_source import levels, level_archive
from pack_native import member
from max_payne_sdk.max_ldb import MaxLDBReader
from triangulate import triangulate

SURFACES = ('default','carpet','cardboard','couch','gravel','metal','metal_hollow','metal_outside',
            'metal_solid','paper','pipes','pipes_steam','snow','water_bottom','wood','wireframe')

def position(v):
    return (v.x,v.y,v.z)

def light_grid(mesh, corners):
    samples=[]
    @lru_cache(None)
    def image(light):
        return np.asarray(Image.open(BytesIO(light.data)).convert('RGB'))
    for p in mesh.polygons.polygons:
        if not p.lightmap or p.num_vertices<3:continue
        vertices=corners[p.texture_vertex_idx:p.texture_vertex_idx+p.num_vertices]
        pts=[mesh.vertices.vertices[v.vertex_idx] for v in vertices]
        tex=image(p.lightmap);h,w=tex.shape[:2]
        u=sum(v.lightmap_uv.u for v in vertices)/len(vertices);v=sum(v.lightmap_uv.v for v in vertices)/len(vertices)
        rgb=tex[min(h-1,max(0,int(v*h))),min(w-1,max(0,int(u*w)))]
        samples.append((sum(pt.x for pt in pts)/len(pts),sum(pt.z for pt in pts)/len(pts),rgb))
    if not samples:return struct.pack('<4f',0,0,0,0)+bytes([200])*48
    pts=np.asarray([s[:2] for s in samples]);rgb=np.asarray([s[2] for s in samples],dtype=float)
    lo=pts.min(axis=0);size=np.maximum(pts.max(axis=0)-lo,.01);out=bytearray()
    for z in range(4):
        for x in range(4):
            d=np.sum((pts-(lo+size*[x/3,z/3]))**2,axis=1)
            ix=np.argsort(d)[:8];c=np.average(rgb[ix],axis=0,weights=1/(1+d[ix]))
            out.extend(np.clip(48+c*1.62,48,255).astype('u1').tobytes())
    return struct.pack('<4f',*lo,*(3/size))+out

def bake(job):
    arcs,stage,dest=job
    with TemporaryDirectory() as td:
        p=Path(td)/'scene.ldb'
        p.write_bytes(member(level_archive(arcs,stage),'data/database/levels/'+stage['directory']+'/'+stage['level']))
        scene=MaxLDBReader(p).parse()
    static={m.static_mesh_id:m for m in scene.static_meshes.static_meshes}
    mats={k:{p.id:p.material.category_name.lower() for p in m.polygons.polygons} for k,m in static.items()}
    rules=material_rules(arcs)
    codes={name:(rule.channels<<4) | (SURFACES.index(name) if name in SURFACES else 0) for name,rule in rules.items()}
    floors=defaultdict(bytearray)
    for p in scene.bsp.polygons.polygons:
        mat=mats.get(p.static_mesh_id,{}).get(p.polygon_id,'default')
        points=[position(v)for v in scene.bsp.vertices.vertices[p.vertex_idx:p.vertex_idx+p.num_vertices]]
        floors[p.static_mesh_id].extend(bytes([codes[mat]])*len(triangulate(points)))
    data=[];probes=[]
    props=scene.dynamic_meshes.dynamic_meshes
    with dest.with_suffix('.mpc').open('rb') as f:
        magic,n,nt=struct.unpack('<4sII',f.read(12));assert magic==b'MPC1'
        f.seek(36);offsets=struct.unpack('<'+'I'*(n+nt+2),f.read(4*(n+nt+2)))
        for r in scene.rooms.rooms:
            room=r.id;mesh=static[r.static_meshes[0]];probes.append(light_grid(mesh,scene.static_meshes.texture_vertices.texture_vertices))
            f.seek(offsets[room+1]);_,b,s,e,v=struct.unpack('<64s4I',f.read(80))
            for _ in range(e):
                _,_,nv=struct.unpack('<3I',f.read(12));f.seek(60+12*nv,1)
            f.seek(12*b+28*v,1);ids=bytearray();tri=[]
            for _ in range(s):
                prop,count=struct.unpack('<iI',f.read(8));faces=np.frombuffer(f.read(40*count),'<f4').reshape(-1,10)
                tri.append(faces[:,:9].reshape(-1,3,3))
                if prop<0:surface=floors[mesh.static_mesh_id]
                else:
                    obj=props[prop];surface=bytearray()
                    for poly in obj.polygons.polygons:
                        if not (obj.config.dynamic_collisions or obj.config.bullet_collisions):continue
                        verts=scene.dynamic_meshes.texture_vertices.texture_vertices[poly.texture_vertex_idx:poly.texture_vertex_idx+poly.num_vertices]
                        ps=np.array([position(obj.vertices.vertices[t.vertex_idx]) for t in verts])
                        if len(ps)<3:continue
                        xf=np.asarray(obj.properties.object_to_room_transform);ps=ps@xf[:3]+xf[3]
                        indices=triangulate(ps)
                        valid=[np.linalg.norm(np.cross(ps[b]-ps[a],ps[c]-ps[a]))>1e-7 for a,b,c in indices]
                        m=poly.material.category_name.lower();surface.extend(bytes([codes[m]])*sum(valid))
                if len(surface)!=count:raise ValueError((stage['id'],room,prop,count,len(surface)))
                ids.extend(surface)
            blocks=[]
            if tri:
                xyz=np.concatenate(tri)
                for j in range(0,len(xyz),16):
                    chunk=xyz[j:j+16];blocks.append(np.concatenate((chunk.min(axis=(0,1)),chunk.max(axis=(0,1)))).astype('<f4').tobytes())
            data.append(struct.pack('<I',len(ids))+bytes(ids)+b''.join(blocks))
    pos=8+64*n+4*(n+1);offsets=[pos]
    for block in data:offsets.append(offsets[-1]+len(block))
    temp=dest.with_suffix('.surf.tmp')
    temp.write_bytes(struct.pack('<4sI',b'SRF2',n)+b''.join(probes)+struct.pack('<'+'I'*len(offsets),*offsets)+b''.join(data));temp.replace(dest)
    return stage['id']

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('disc',type=Path);ap.add_argument('out',type=Path);ap.add_argument('--jobs',type=int,default=3)
    a=ap.parse_args();arcs=a.disc/'english'
    jobs=[(arcs,s,a.out/(s['id']+'.surf')) for s in levels(arcs)]
    with ProcessPoolExecutor(a.jobs) as pool:
        for name in pool.map(bake,jobs):print(name,flush=True)
