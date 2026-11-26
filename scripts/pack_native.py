#!/usr/bin/env python3
"""Build the native menu, characters and Tutorial and Prologue rooms from a local Xbox disc."""
import argparse
from collections import defaultdict
from io import BytesIO
import math
import mmap
from pathlib import Path
from tempfile import TemporaryDirectory
import struct
import subprocess

from PIL import Image, ImageFilter, ImageOps
from dade.maxpayne import ldb, ras

from native_texture import texture


def member(archive, path):
    with archive.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        entry, = [e for e in ras.read_directory(data).entries if e.path.casefold() == path.casefold()]
        return ras.member_bytes(data, entry)


def write_lcd(image, stream):
    import numpy as np
    r,g,b=np.asarray(image,dtype=np.uint16).transpose(2,0,1)
    encoded=((r>>3)<<11)|((g>>2)<<5)|(b>>3)
    stream.write(encoded[::-1].T.astype('<u2',copy=False).tobytes())


def write_screen(data, dest, *, comic=False, preserve_aspect=False, size=(400, 240)):
    import numpy as np
    # gfxGetFramebuffer uses bottom-to-top columns on the rotated top LCD.
    image = Image.open(BytesIO(data)).convert('RGB')
    if comic:
        ink=np.asarray(image).max(axis=2)>9
        rows=np.flatnonzero(ink.sum(axis=1)>=2)
        cols=np.flatnonzero(ink.sum(axis=0)>=2)
        if len(rows) and len(cols):
            image=image.crop((max(0,int(cols[0])-2),max(0,int(rows[0])-2),
                              min(image.width,int(cols[-1])+3),min(image.height,int(rows[-1])+3)))
        if preserve_aspect:
            content=ImageOps.contain(image,size,Image.Resampling.LANCZOS)
            image=Image.new('RGB',size);image.paste(content,((size[0]-content.width)//2,(size[1]-content.height)//2))
        else:
            image=image.resize(size,Image.Resampling.LANCZOS).filter(ImageFilter.UnsharpMask(.55,80,3))
    else:
        # The PAL menu DDS contains baked black bars and isolated compression
        # specks below the artwork. Keep the full horizontal title and face.
        visible=(np.asarray(image).max(axis=2)>10).sum(axis=1)>image.width//100
        rows=np.flatnonzero(visible)
        image=image.crop((0,int(rows[0]),image.width,int(rows[-1])+1)).resize(size,Image.Resampling.LANCZOS)
    with dest.open('wb') as stream:
        write_lcd(image,stream)


def marker(data, name):
    # PAL entities contain two tagged 4x3 matrices followed by their room id.
    key = name.encode() + b'\x0d\x14\x00\x1a'
    if data.count(key) != 1:
        raise ValueError(f'Expected one placed entity {name}')
    at = data.index(key) + len(key)
    if data[at + 48] != 0x1a or data[at + 97] != 0x14:
        raise ValueError(f'Unexpected placement layout for {name}')
    return struct.unpack_from('<12f', data, at), data[at + 98]


def pack_room(data, level, index, dest, sky, *, house=False, extra=None):
    import numpy as np
    from pack_mission import ROOMS, IDS, markers, HOUSE_ROOMS, HOUSE_IDS, house_markers
    rooms, ids = (HOUSE_ROOMS, HOUSE_IDS) if house else (ROOMS, IDS)
    spawn_name = '::'+rooms[index]+'::Jumppoint '+('01' if index==3 else '00') if house and extra is None else ('::startroom::w1' if house else '::StartRoom::Jumppoint 00')
    spawn, _ = marker(data, spawn_name)
    room_name,room_id = extra or (rooms[index],ids[index])
    prefix = '::'+room_name+'::'
    room = level.mesh.meshes[room_id]
    basis = room.transform[:9]
    if any(abs(v - (i % 4 == 0)) > 0.0001 for i, v in enumerate(basis)):
        raise ValueError('Expected the PAL starting room in level axes')
    origin = room.transform[9:]
    images = {t.path.lower(): t for t in level.textures}
    tex_ids, textures = {}, []
    draws = defaultdict(bytearray)
    collision = defaultdict(bytearray)
    group = 0

    def image_id(key, picture, *, lightmap=False):
        if key not in tex_ids:
            tex_ids[key] = len(textures)
            textures.append(texture(picture(), lightmap=lightmap))
        return tex_ids[key]

    white = image_id('white', lambda: Image.new('RGB', (8, 8), 'white'))

    def mesh_parts(mesh, corners, prop, transform=None):
        def convert(point):
            if not prop:
                return point
            t = transform if transform is not None else mesh.transform
            return tuple(t[9+i] - origin[i] + sum(point[k]*t[3*k+i] for k in range(3)) for i in range(3))

        positions = [convert(p) for p in mesh.positions]
        for face in mesh.faces:
            mat = level.materials[face.material]
            if mat.category.endswith('nodraw') or mat.category in ('dummy', 'cameracollision', 'skybox'):
                continue
            name = mat.image.lower()
            def diffuse():
                im = Image.open(BytesIO(images[name].data)).convert('RGBA')
                if mat.alpha:
                    mask = Image.open(BytesIO(images[mat.alpha.lower()].data)).convert('L')
                    im.putalpha(mask.resize(im.size))
                return im
            tex = image_id((name, mat.alpha), diffuse) if name in images else white
            lm = face.lightmap
            light = image_id(('light', lm), lambda: Image.open(BytesIO(level.lightmaps[lm].data)), lightmap=True) if 0 <= lm < len(level.lightmaps) else white
            batch = draws[group, tex, light]
            corners_used = corners[face.first_corner:face.first_corner+face.corner_count]
            for i in range(1, len(corners_used)-1):
                tri = [corners_used[j] for j in (0, i, i+1)]
                for vertex in tri:
                    pos = positions[vertex.position]
                    if not prop and mat.category == 'graffiti':
                        # Painted overlays share the wall plane in the LDB.
                        pos = tuple(v + n * .005 for v, n in zip(pos, face.normal))
                    batch.extend(struct.pack('<7f', *pos, *vertex.uv, *vertex.lightmap_uv))
                if prop:
                    pts = [positions[c.position] for c in tri]
                    a, b, c = pts
                    e = [b[k]-a[k] for k in range(3)]
                    f = [c[k]-a[k] for k in range(3)]
                    n = [e[1]*f[2]-e[2]*f[1], e[2]*f[0]-e[0]*f[2], e[0]*f[1]-e[1]*f[0]]
                    length = math.hypot(*n)
                    # Prop winding varies. Test the top height near the current feet.
                    if length > 1e-7:
                        collision[group].extend(struct.pack('<10f', *a, *b, *c, abs(n[1])/length))

    # The first batch is a camera-centred sky, independent of room and stereo depth.
    neutral=image_id('neutral', lambda: Image.new('RGB',(8,8),(128,128,128)))
    for material,image,verts in sky:
        base=image_id(('worldsphere',material),lambda: image)
        draws[5,base,neutral].extend(verts)

    mesh_parts(room, level.mesh.corners, False)
    # Groups 1/2 are closed/open van or cabinet doors, 3/4 are the locked/open gate.
    for name, prop, clips in zip(level.props.names, level.props.meshes, level.props.animations, strict=True):
        if not name.startswith(prefix) or '_Tip_' in name or 'FSM' in name:
            continue
        if house:
            # Invisible scripted controls and cameras are not solid scenery.
            leaf = name.rsplit('::', 1)[-1].lower()
            if any(term in leaf for term in ('camera', 'controller', 'cam_', 'timer', 'trigger', 'post_process', 'look_at_mover')):
                continue
        van_door=not house and index in (0,2) and name.startswith(prefix+'van01::van_door_')
        cabinet_door=not house and index==3 and name in (prefix+'Medikit::d_door01.DO',prefix+'Medikit::d_door02.DO')
        gate=not house and index==0 and name==prefix+'Door.DO'
        lock=not house and index==0 and name==prefix+'lock.DO'
        group=1 if van_door or cabinet_door else 3 if gate or lock else 0
        mesh_parts(prop, level.props.corners, True)
        if van_door or cabinet_door or gate:
            clip=next(c for c in clips if c.name.lower()==('animation_0' if van_door else 'open'))
            def affine(rows):
                m=np.eye(4)
                m[:,:3]=np.reshape(rows,(4,3))
                return m
            final=affine(clip.end) @ np.linalg.inv(affine(clip.start)) @ affine(prop.transform)
            group+=1
            mesh_parts(prop,level.props.corners,True,final[:,:3].flatten())
    group=0
    for face in level.geometry.polygons:
        if face.mesh_index == level.mesh.keys[room_id]:
            pts = level.geometry.vertices[face.first_vertex:face.first_vertex+face.vertex_count]
            for i in range(1, len(pts)-1):
                collision[group].extend(struct.pack('<10f', *pts[0], *pts[i], *pts[i+1], face.normal[1]))
    # scene.c rejects spatial blocks of 16 triangles before testing individual faces.
    # Morton order keeps those blocks local even when the LDB groups faces by material.
    for key, packed in collision.items():
        faces = np.frombuffer(packed, dtype='<f4').reshape(-1, 10)
        centers = faces[:, :9].reshape(-1, 3, 3).mean(axis=1)
        span = np.maximum(np.ptp(centers, axis=0), .001)
        cell = ((centers - centers.min(axis=0)) * (1023 / span)).astype(np.uint32)
        order = np.zeros(len(faces), dtype=np.uint32)
        for bit in range(10):
            for axis in range(3):
                order |= ((cell[:, axis] >> bit) & 1) << (3 * bit + axis)
        collision[key] = faces[np.argsort(order, kind='stable')].tobytes()
    count = sum(len(d)//28 for d in draws.values())
    if not count or not collision:
        raise ValueError('Empty starting room')
    with dest.open('wb') as out:
        out.write(struct.pack('<4s4I4f', b'MPR6', len(textures), len(draws), count,
                              sum(map(len,collision.values()))//40,*spawn[9:],math.atan2(spawn[6],spawn[8])))
        out.write(bytes(368) if extra else (house_markers if house else markers)(data,marker,index))
        out.write(struct.pack('<5I',*(len(collision[g])//40 for g in range(5))))
        for t in textures:
            out.write(t)
        for (state, base, light), vertices in draws.items():
            out.write(struct.pack('<4I', base, light, len(vertices)//28,state))
        for vertices in draws.values():
            out.write(vertices)
        for g in range(5): out.write(collision[g])
    if not house and index==2:
        from pack_mission import navigation
        navigation(collision[0],dest.with_name('street.nav'))
    print(f'{room_name}: {count//3} triangles, {len(draws)} batches, {len(textures)} textures, {dest.stat().st_size} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=True, parents=True)
    archives = args.disc / 'english'
    from pack_ui import lower_ui
    lower_ui(archives,args.output)
    write_screen(member(archives/'x_english.ras', 'data/menu/bitmaps/menu_bg_mainmenu.dds'), args.output/'menu.565')
    from pack_mission import IDS, HOUSE_IDS
    from pack_neighbours import portals, worldsphere, link_room
    for house,archive,path,sky_name,ids,names in (
        (False,'x_data','front_end/Tutorial','bronx',IDS,('start','alley','street','subway','sidealley','deadend')),
        (True,'x_level1','part1a/P0_L1','sunset',HOUSE_IDS,('home','homephone','homeliving','homestairs'))):
        data=member(archives/(archive+'.ras'),'data/database/levels/'+path+'.ldb')
        level=ldb.read_level(data)
        neighbours=portals(data)
        sky=worldsphere(archives/'x_data.ras',member,sky_name)
        with TemporaryDirectory() as scratch:
            raw=Path(scratch)
            for index,room_id in enumerate(ids):
                pack_room(data,level,index,raw/f'{room_id}.room',sky,house=house)
            if not house:
                (args.output/'street.nav').write_bytes((raw/'street.nav').read_bytes())
            for room_id in ids:
                for _,_,_,dest,room_name in neighbours[room_id]:
                    destfile=raw/f'{dest}.room'
                    if not destfile.exists():
                        pack_room(data,level,-1,destfile,(),house=house,extra=(room_name,dest))
            for room_id,name in zip(ids,names,strict=True):
                link_room(raw/f'{room_id}.room',neighbours[room_id],ids,len(IDS) if house else 0,
                          args.output/(name+'.room'))
    for page in range(3,8):
        stem=f'P1L0a_{page:03d}'
        write_screen(member(archives/'x_english.ras',f'data/database/graphicnovelpages/part1/textures/{stem}.dds'),
                     args.output/f'page{page}.565',comic=True)
        subprocess.run(['ffmpeg','-nostdin','-v','error','-y','-i',archives/f'data/database/sounds/story/part1/{stem}.wav',
                        '-ac','1','-ar','22050','-f','s16le',args.output/f'page{page}.pcm'],check=True)
    from pack_player import pack_player
    pack_player(lambda path: member(archives/'x_data.ras', path), texture, args.output/'max.actor')
    pack_player(lambda path: member(archives/'x_data.ras', path), texture, args.output/'johnny.actor',enemy=True)
    pack_player(lambda path: member(archives/'x_data.ras', path), texture, args.output/'young.actor',young=True)
    for sound, output in [('dynamic/phone/Phone_Ringing_Old1', 'ring.pcm'),
                          ('dynamic/phone/Payphone_Use', 'answer.pcm'),
                          ('weapons/shoot_beretta', 'shot.pcm'),
                          ('weapons/reload_beretta', 'reload.pcm'),
                          ('weapons/shoot_pumpshotgun','pump.pcm'),
                          ('weapons/Reload_Pumpshotgun02','pump_reload.pcm'),
                          ('weapons/shoot_sniper','sniper.pcm'),
                          ('weapons/reload_sniper','sniper_reload.pcm'),
                          ('dynamic/switch/Rotary2','switch.pcm'),
                          ('dynamic/door/Door_Wood_Small_Open','cabinet.pcm'),
                          ('characters/Use_Painkillers','dose.pcm'),
                          ('dynamic/door/Door_Metal_Medium_Open','door.pcm')]:
        wav = archives/'data/database/sounds'/(sound+'.wav')
        subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-y', '-i', wav, '-ac', '1',
                        '-ar', '22050', '-f', 's16le', args.output/output], check=True)
