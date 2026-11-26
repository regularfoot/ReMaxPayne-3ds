"""Compare every shipped room surface, drawing batch and collision channel with the Xbox LDB."""
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from functools import lru_cache
from hashlib import sha256
from io import BytesIO
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from campaign_materials import material_rules
from campaign_source import levels, level_archive
from max_payne_sdk.max_ldb import MaxLDBReader
from native_texture import TEXEL_BYTES, texture
from pack_campaign import sky_texture
from pack_native import member
from pack_neighbours import worldsphere
from triangulate import triangulate


@lru_cache(None)
def sky_batches(archives,name):
    return [(sha256(sky_texture(image)).digest(),len(vertices)//28,image.getchannel('A').getextrema()[0]<255)
            for _,image,vertices in worldsphere(archives/'x_data.ras',member,name)] if name else []


def verify_effect_banks(disc,assets):
    from pack_campaign_faces import pack as pack_faces
    from pack_campaign_particles import pack as pack_particles
    from pack_decals import pack_decals
    from dade.maxpayne import ras
    import mmap
    with TemporaryDirectory() as tmp:
        dest=Path(tmp);(dest/'cast').symlink_to((assets/'cast').resolve(),target_is_directory=True)
        pack_faces(disc,dest)
        pack_particles(disc,dest/'particles.pak')
        with (disc/'english/x_data.ras').open('rb') as file,mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as archive:
            entries={e.path.lower():e for e in ras.read_directory(archive).entries}
            pack_decals(lambda name:ras.member_bytes(archive,entries[name]),dest)
        assert {p.name for p in (dest/'faces').glob('*.face')}=={p.name for p in (assets/'faces').glob('*.face')}
        for path in [dest/'particles.pak',dest/'decals.pak',*(dest/'faces').glob('*.face')]:
            assert path.read_bytes()==(assets/path.relative_to(dest)).read_bytes(),path.name


def xyz(vertex):
    return vertex.x,vertex.y,vertex.z


def fallback_texture(data):
    """Compare retail pixels at the unchanged Old 3DS mip, regardless of New base."""
    w,h,fmt,levels,clamp,reserved=struct.unpack_from('<HHBBBB',data)
    at=8
    while max(w,h)>128:
        assert levels
        at+=w*h*TEXEL_BYTES[fmt]
        w//=2;h//=2;levels-=1
    return struct.pack('<HHBBBB',w,h,fmt,levels,clamp,reserved)+data[at:]


def verify(job):
    archives,stage,assets=job
    name=stage['id']
    with TemporaryDirectory() as tmp:
        original=Path(tmp)/'original.ldb'
        original.write_bytes(member(level_archive(archives,stage),'data/database/levels/'+stage['directory']+'/'+stage['level']))
        level=MaxLDBReader(original).parse()
    rules=material_rules(archives)
    geometry=(assets/(name+'.mpc')).read_bytes()
    surfaces=(assets/(name+'.surf')).read_bytes()
    rooms,images=struct.unpack_from('<2I',geometry,4)
    assert geometry[:4]==b'MPC1' and surfaces[:4]==b'SRF2'
    assert struct.unpack_from('<I',surfaces,4)[0]==rooms==len(level.rooms.rooms)
    offsets=struct.unpack_from(f'<{rooms+images+2}I',geometry,36)
    surface_offsets=struct.unpack_from(f'<{rooms+1}I',surfaces,8+64*rooms)
    stored=[sha256(fallback_texture(geometry[a:b])).digest() for a,b in zip(offsets[rooms+1:],offsets[rooms+2:])]
    skies=sky_batches(archives,stage.get('worldsphere',''))
    count,=struct.unpack_from('<I',geometry,offsets[0])
    assert count==len(skies),(name,'worldsphere')
    for i,(digest,n,_) in enumerate(skies):
        base,_,owner,vertices=struct.unpack_from('<HHiI',geometry,offsets[0]+4+12*i)
        image_at=rooms+1+base
        assert sha256(geometry[offsets[image_at]:offsets[image_at+1]]).digest()==digest and owner==-1 and vertices==n,(name,'sky layer',i)
    @lru_cache(None)
    def diffuse(mat):
        rgb=Image.open(BytesIO(mat.diffuse_texture.data)).convert('RGBA') if mat.diffuse_texture else None
        if mat.alpha_texture:
            mask=Image.open(BytesIO(mat.alpha_texture.data)).convert('L')
            if rgb is None:rgb=Image.new('RGBA',mask.size,'white')
            rgb.putalpha(mask.resize(rgb.size))
        if rgb is None:rgb=Image.new('RGBA',(8,8),'white')
        encoded=texture(rgb)
        return sha256(encoded).digest(),encoded[4]==4
    meshes={m.static_mesh_id:m for m in level.static_meshes.static_meshes}
    props=level.dynamic_meshes.dynamic_meshes
    bsp={}
    for face in level.bsp.polygons.polygons:bsp.setdefault(face.static_mesh_id,[]).append(face)
    totals=Counter(sky_layers=len(skies),alpha_sky_layers=sum(alpha for _,_,alpha in skies))
    for r in level.rooms.rooms:
        at=offsets[r.id+1];_,batches,solids,exits,vertices=struct.unpack_from('<64s4I',geometry,at);at+=80
        for _ in range(exits):
            points=struct.unpack_from('<I',geometry,at+8)[0];at+=72+points*12
        expected=Counter();actual=Counter();channels={}
        static=meshes[r.static_meshes[0]]
        by_id={p.id:p.material for p in static.polygons.polygons}
        channels[-1]=b''.join(bytes([rules[by_id[p.polygon_id].category_name.lower()].channels if p.polygon_id in by_id else rules['default'].channels])*
                             len(triangulate([xyz(v) for v in level.bsp.vertices.vertices[p.vertex_idx:p.vertex_idx+p.num_vertices]]))
                             for p in bsp.get(static.static_mesh_id,[]))
        special=set()
        for owner,mesh in [(-1,static)]+[(i,m) for i,m in enumerate(props) if m.properties.room_id==r.id]:
            solid=owner>=0 and (mesh.config.dynamic_collisions or mesh.config.bullet_collisions)
            if owner>=0 and mesh.shared_name.lower().endswith('::powerlamp::halo.do') and not solid:
                special.add(owner);continue
            solid_channels=bytearray()
            for poly in mesh.polygons.polygons:
                mat=poly.material;rule=rules[mat.category_name.lower()]
                coords=mesh.texture_vertices.texture_vertices[poly.texture_vertex_idx:poly.texture_vertex_idx+poly.num_vertices]
                pts=np.asarray([xyz(mesh.vertices.vertices[c.vertex_idx]) for c in coords]).reshape(-1,3)
                if owner>=0:
                    matrix=np.asarray(mesh.properties.object_to_room_transform)
                    pts=pts@matrix[:3]+matrix[3]
                indices=triangulate(pts);tris=len(indices)
                if solid and tris:
                    triangles=pts[indices]
                    valid=np.linalg.norm(np.cross(triangles[:,1]-triangles[:,0],triangles[:,2]-triangles[:,0]),axis=1)>1e-7
                    solid_channels.extend([rule.channels]*int(valid.sum()))
                if not rule.draw or not tris:continue
                digest,alpha=diffuse(mat);material=mat.material_name.lower()
                mode=2 if 'light_volumetric' in material else int((alpha or not rule.depth_write) and rule.blend)
                flag=(1 if mode==1 and rule.depth_write else 2 if alpha and mode==0 and rule.alpha_reference==127 else 0)
                decal=mat.category_name.lower()=='graffiti'
                expected[owner,digest,mode,flag,decal]+=tris*3
                if 'siren02' in material:expected[owner,digest,3,flag,decal]+=tris*3
                totals['visible_polygons']+=1
                if alpha:totals['alpha_polygons']+=1
            if solid:channels[owner]=bytes(solid_channels)
        for _ in range(batches):
            base,lm,owner,n=struct.unpack_from('<HHiI',geometry,at);at+=12
            if owner in special:
                assert (lm>>14,n)==(2,12),(name,r.id,owner);special.remove(owner)
            else:actual[owner,stored[base&0x1fff],lm>>14,base>>14,bool(base&0x2000)]+=n
        assert not special and actual==expected,(name,r.id,'render batches',actual-expected,expected-actual)
        at+=vertices*28
        ns=struct.unpack_from('<I',surfaces,surface_offsets[r.id])[0]
        packed=surfaces[surface_offsets[r.id]+4:surface_offsets[r.id]+4+ns]
        saved=0
        for _ in range(solids):
            owner,n=struct.unpack_from('<iI',geometry,at);at+=8+40*n
            source=channels.pop(owner);observed=bytes(v>>4 for v in packed[saved:saved+n])
            assert observed==source,(name,r.id,owner,'projectile channels');saved+=n
            totals['small_pass_big_block']+=sum(not v&2 and bool(v&4) for v in source)
        assert not any(channels.values()) and saved==ns and at==offsets[r.id+2],(name,r.id)
        totals['rooms']+=1;totals['collision_triangles']+=ns
    return name,totals


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('disc',type=Path);ap.add_argument('assets',type=Path);ap.add_argument('--jobs',type=int,default=3)
    args=ap.parse_args();arcs=args.disc/'english';total=Counter()
    verify_effect_banks(args.disc,args.assets)
    with ProcessPoolExecutor(args.jobs) as pool:
        for name,stats in pool.map(verify,[(arcs,s,args.assets) for s in levels(arcs)]):
            total.update(stats);print(name,dict(stats),flush=True)
    print('All Xbox rooms verified',dict(total))
