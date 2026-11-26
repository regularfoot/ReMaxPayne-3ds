"""Pack each campaign map into an independently streamable native 3DS scene."""
from collections import defaultdict
from functools import lru_cache
from io import BytesIO
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse
import math
import struct
import numpy as np
from PIL import Image

from campaign_faces import cull_opposing_faces
from campaign_materials import material_rules
from campaign_source import levels, level_archive
from max_payne_sdk.max_ldb import MaxLDBReader
from native_texture import texture
from pack_native import member
from pack_neighbours import worldsphere
from triangulate import triangulate
from xbox_disc import archive_root


def vec(p):
    return (p.x,p.y,p.z)


def sky_texture(image):
    return texture(image, max_size=128)


def pack_level(level, config, sky, dest, rules, texture_limit=128):
    rooms=level.rooms.rooms
    exits=level.exits.exits
    props=level.dynamic_meshes.dynamic_meshes
    waypoints={w.shared_name.casefold():w for w in level.waypoints.waypoints}
    start=waypoints[config['playerstartingplace'].casefold()].object_properties
    spawn=start.object_to_room_transform
    textures=[]
    index={}
    high_bytes=0
    def store(key,load,lightmap=False):
        nonlocal high_bytes
        if key not in index:
            index[key]=len(textures)
            image=load();limit=128 if lightmap else min(256,texture_limit)
            low=texture(image,lightmap=lightmap,max_size=limit)
            if not lightmap and texture_limit==512 and max(image.size)>256:
                high=texture(image,max_size=512);extra=len(high)-len(low)
                # All room textures stay resident to avoid transition I/O.
                # Bound the added New-3DS base levels per complete stage.
                if 0<extra<=1024*1024-high_bytes:
                    low=high;high_bytes+=extra
            textures.append(low)
        return index[key]
    white=store('white',lambda:Image.new('RGB',(8,8),'white'))
    neutral=store('neutral',lambda:Image.new('RGB',(8,8),(128,128,128)))
    @lru_cache(None)
    def material(mat):
        if not mat.diffuse_texture and not mat.alpha_texture:return white
        def image():
            im=Image.open(BytesIO(mat.diffuse_texture.data)).convert('RGBA') if mat.diffuse_texture else None
            if mat.alpha_texture:
                alpha=Image.open(BytesIO(mat.alpha_texture.data)).convert('L')
                if im is None:im=Image.new('RGBA',alpha.size,'white')
                im.putalpha(alpha.resize(im.size))
            return im
        return store((mat.diffuse_texture,mat.alpha_texture),image)
    sky_batches=[]
    for _,image,data in sky:
        base=len(textures)
        textures.append(sky_texture(image))
        sky_batches.append((base,neutral,-1,data))
    static={m.static_mesh_id:m for m in level.static_meshes.static_meshes}
    collision=defaultdict(bytearray)
    vertices=level.bsp.vertices.vertices
    for p in level.bsp.polygons.polygons:
        v=[vec(a) for a in vertices[p.vertex_idx:p.vertex_idx+p.num_vertices]]
        for a,b,c in triangulate(v):
            collision[p.static_mesh_id].extend(struct.pack('<10f',*v[a],*v[b],*v[c],p.normal.y))
    chunks=[]
    for room in rooms:
        assert room.id==len(chunks) and len(room.static_meshes)==1
        mesh_id=room.static_meshes[0]
        batches=defaultdict(bytearray)
        solids={-1:collision[mesh_id]}
        def searchlight(m,owner,transform):
            # Crossed soft strips match the authored lamp axis and the two ring radii.
            # They contribute light to the scene and have no opaque shell or end cap.
            pts=np.asarray([vec(p) for p in m.vertices.vertices])
            near=pts[pts[:,0]>pts[:,0].max()-.01];far=pts[pts[:,0]<pts[:,0].min()+.01]
            def texture_image():
                x=np.linspace(0,1,64);y=np.linspace(-1,1,32)
                intensity=(1-x[None,:])**1.55*np.maximum(0,1-y[:,None]**2)**2.8
                rgba=np.empty((*intensity.shape,4),dtype='u1')
                rgba[:,:,:3]=(122,139,162)
                rgba[:,:,3]=np.rint(138*intensity)
                return Image.fromarray(rgba)
            idx=store('searchlight-falloff',texture_image,True)
            batch=batches[idx,neutral|0x8000,owner]
            for axis in (1,2):
                corners=[]
                for ring,u in ((near,.008),(far,.992)):
                    low=ring.min(axis=0);high=ring.max(axis=0)
                    for sign,v in ((-1,.016),(1,.984)):
                        p=(high+low)*.5;p[axis]+=sign*(high[axis]-low[axis])*.5
                        p=p@transform[:3]+transform[3]
                        corners.append(struct.pack('<7f',*p,u,v,.5,.5))
                batch.extend(b''.join(corners[i] for i in (0,1,2,1,3,2)))
        def mesh(m,owner):
            pos=np.array([vec(v) for v in m.vertices.vertices],dtype=float).reshape(-1,3)
            prop=owner>=0
            if prop:
                t=np.array(m.properties.object_to_room_transform)
                if m.shared_name.lower().endswith('::powerlamp::halo.do') and not(m.config.dynamic_collisions or m.config.bullet_collisions):
                    searchlight(m,owner,t);return
                pos=pos@t[:3]+t[3]
            corners=m.texture_vertices.texture_vertices
            for face in m.polygons.polygons:
                mat=face.material
                category=mat.category_name.lower()
                rule=rules[category]
                visible=rule.draw
                solid=prop and (m.config.dynamic_collisions or m.config.bullet_collisions)
                if not visible and not solid:continue
                poly=corners[face.texture_vertex_idx:face.texture_vertex_idx+face.num_vertices]
                points=[pos[c.vertex_idx] for c in poly]
                indices=triangulate(points)
                if not indices:continue;
                render_points=points
                if visible:
                    light=face.lightmap
                    lm=store(('lm',light.id),lambda:Image.open(BytesIO(light.data)),True) if light else white
                    base=material(mat);uv=[(c.uv.u,c.uv.v) for c in poly]
                    if 'light_volumetric' in mat.material_name.casefold():
                        # This RGB atlas encodes emitted light. Black texels add nothing.
                        lm=neutral|0x8000
                    elif textures[base][4]==4 or not rule.depth_write:
                        if rule.blend:
                            lm|=0x4000
                            if rule.depth_write:base|=0x4000
                        elif rule.alpha_reference==127:base|=0x8000
                        else:raise ValueError((category,rule.alpha_reference))
                    if category=='graffiti':base|=0x2000
                    batch=batches[base,lm,owner]
                    encoded=[struct.pack('<7f',*p,*tc,c.lightmap_uv.u,c.lightmap_uv.v)
                             for p,tc,c in zip(render_points,uv,poly,strict=True)]
                    triangles=b''.join(encoded[a]+encoded[b]+encoded[c] for a,b,c in indices)
                    batch.extend(triangles)
                    if 'siren02' in mat.material_name.casefold():
                        batches[base,neutral|0xc000,owner].extend(triangles)
                if solid:
                    for ia,ib,ic in indices:
                        a,b,c=points[ia],points[ib],points[ic]
                        u,v=b-a,c-a
                        normal=(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0])
                        length=math.hypot(*normal)
                        if length>1e-7:
                            solids.setdefault(owner,bytearray()).extend(struct.pack('<10f',*a,*b,*c,abs(normal[1])/length))
        mesh(static[mesh_id],-1)
        for i,prop in enumerate(props):
            if prop.properties.room_id==room.id: mesh(prop,i)
        local=[(i,e) for i,e in enumerate(exits) if e.room_id==room.id]
        data=BytesIO()
        name=room.name.removeprefix('::').encode('utf-8')
        if len(name)>=64:raise ValueError('Room name too long')
        batches=cull_opposing_faces([(*key,value) for key,value in batches.items() if value],textures)
        solids={key:value for key,value in solids.items() if value}
        data.write(struct.pack('<64s4I',name,len(batches),len(solids),len(local),sum(len(payload) for *_,payload in batches)//28))
        for i,e in local:
            pts=[vec(v) for v in e.vertices.vertices]
            data.write(struct.pack('<3I15f',i,e.parent_room_id,len(pts),*np.ravel(e.transform),*e.normal))
            data.write(np.asarray(pts,dtype='<f4').tobytes())
        for base,lm,owner,payload in batches:data.write(struct.pack('<HHiI',base,lm,owner,len(payload)//28))
        for *_,payload in batches:data.write(payload)
        for owner,faces in solids.items():
            data.write(struct.pack('<iI',owner,len(faces)//40));data.write(faces)
        chunks.append(data.getvalue())
    skydata=BytesIO()
    skydata.write(struct.pack('<I',len(sky_batches)))
    for base,lm,owner,payload in sky_batches:skydata.write(struct.pack('<HHiI',base,lm,owner,len(payload)//28))
    for *_,payload in sky_batches:skydata.write(payload)
    # Offset tables allow the 3DS to read only visible rooms and their shared textures.
    pieces=[skydata.getvalue(),*chunks,*textures]
    offset=36+4*(len(pieces)+1)
    offsets=[offset]
    for piece in pieces:offsets.append(offsets[-1]+len(piece))
    with dest.open('wb') as out:
        out.write(struct.pack('<4s4I4f',b'MPC1',len(rooms),len(textures),start.room_id,len(props),*spawn[3],math.atan2(spawn[2][0],spawn[2][2])))
        out.write(struct.pack(f'<{len(offsets)}I',*offsets))
        out.writelines(pieces)
    print(config['id'],len(rooms),len(textures),dest.stat().st_size,flush=True)


def write_directed_lights(records,dest):
    # LDB stores full cone angles. Save cosine thresholds so actor lighting only
    # needs a dot product per light during play.
    rows=[]
    for room,transform,values in records:
        r,g,b,_,near,_,inner,outer,radius,_=values
        axis=np.asarray(transform[2],dtype=float)
        axis/=max(1e-8,np.linalg.norm(axis))
        rows.append(struct.pack('<I12f',room,*transform[3],*axis,r/255,g/255,b/255,
                                radius,math.cos(inner/2),math.cos(outer/2)))
    temp=dest.with_suffix('.dlg.partial')
    temp.write_bytes(struct.pack('<4sI',b'DLG1',len(rows))+b''.join(rows))
    temp.replace(dest)


def pack_lights(level, dest):
    write_directed_lights(((l.object_properties.room_id,l.object_properties.object_to_room_transform,
                            [getattr(l,'unk'+str(i)) for i in range(1,11)]) for l in level.dynamic_lights.lights),
                          dest.with_suffix('.dlg'))
    groups=[[] for room in level.rooms.rooms]
    for light in level.pointlights.pointlights:
        props=light.object_properties
        if light.falloff>0 and light.intensity>0:
            groups[props.room_id].append(struct.pack('<7f',*props.object_to_room_transform[3],
                light.falloff,*(v*light.intensity/255 for v in (light.r,light.g,light.b))))
    partial=dest.with_suffix('.lights.partial')
    with partial.open('wb') as f:
        f.write(struct.pack('<4sI',b'MPL1',len(groups)))
        for group in groups:
            f.write(struct.pack('<I',len(group)))
            f.writelines(group)
    partial.replace(dest)


def pack(disc,out,only,force=False,texture_limit=128):
    arcs=archive_root(disc)
    out.mkdir(parents=True,exist_ok=True)
    @lru_cache(None)
    def sky(name):return worldsphere(arcs/'x_data.ras',member,name) if name else []
    for stage in levels(arcs):
        if only and stage['id'] not in only:continue
        dest=out/(stage['id']+'.mpc')
        if not force and stage['id']!='startup_level' and dest.is_file() and dest.with_suffix('.lights').is_file() and dest.with_suffix('.dlg').is_file():continue
        arc=level_archive(arcs,stage)
        with TemporaryDirectory() as scratch:
            local=Path(scratch)/'stage.ldb'
            local.write_bytes(member(arc,'data/database/levels/'+stage['directory']+'/'+stage['level']))
            data=MaxLDBReader(local).parse()
            partial=dest.with_suffix('.partial')
            pack_level(data,stage,sky(stage.get('worldsphere')),partial,material_rules(arcs),texture_limit)
            partial.replace(dest)
            pack_lights(data,dest.with_suffix('.lights'))
            del data


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc',type=Path)
    parser.add_argument('output',type=Path)
    parser.add_argument('--level',action='append')
    parser.add_argument('--force',action='store_true')
    parser.add_argument('--texture-limit',type=int,choices=(128,256,512),default=128)
    a=parser.parse_args()
    pack(a.disc,a.output,a.level,a.force,a.texture_limit)
