"""Prepare Max's skinned mesh, gun and resampled KF2 bone palettes for the 3DS."""
from collections import defaultdict
from io import BytesIO
from pathlib import Path
import struct
from tempfile import TemporaryDirectory

import numpy as np
from PIL import Image
from dade.maxpayne.model import read_model
from max_payne_sdk.max_kf2 import MaxKF2Reader

CLIPS = ('idle_aim_beretta', 'Walk', 'Run', 'WalkBackward', 'StrafeLeft', 'StrafeRight',
         'ShootBeretta', 'ReloadBeretta', 'idle_aim_pumpshotgun', 'ShootPumpShotgun',
         'ReloadPumpShotgun', 'death_shotgun_a', 'JumpUp',
         'idle_aim_coltcommando', 'ShootMP5', 'ReloadMP5')


def affine(rows):
    matrix = np.eye(4)
    matrix[:, :3] = rows
    return matrix


def sample(track, frame):
    keys = track.keyframes
    if not keys:
        raise ValueError(f'Missing keys for {track.animation.object_name}')
    for left, right in zip(keys, keys[1:]):
        if frame <= right.frame_id:
            mix = np.clip((frame-left.frame_id)/(right.frame_id-left.frame_id), 0, 1)
            mat = affine(np.array(left.transform)*(1-mix) + np.array(right.transform)*mix)
            # Restore a rigid local rotation between sparse keyframes.
            u, _, v = np.linalg.svd(mat[:3, :3])
            mat[:3, :3] = u @ v
            return mat
    return affine(keys[-1].transform)


def worlds(tracks, frame):
    matrices = {}

    def resolve(name):
        if name not in matrices:
            track = tracks[name]
            local = sample(track, frame)
            parent = track.parent_name.casefold()
            matrices[name] = local @ resolve(parent) if parent else local
        return matrices[name]

    for name in tracks:
        resolve(name)
    return matrices


def pack_aim_rig(dest, bones, bind):
    groups = [0] * (len(bones) + 1)
    anchors = bytearray()
    for group, side in enumerate(('r', 'l'), 1):
        shoulder, hand = 'uparm-' + side, 'hand-' + side
        anchors.extend(struct.pack('<II6f', bones.index(shoulder), bones.index(hand),
                                   *bind[shoulder][3, :3], *bind[hand][3, :3]))
        for key in (shoulder, 'loarm-' + side, hand):
            groups[bones.index(key)] = group
    groups[bones.index('gun')] = groups[-1] = 1
    dest.write_bytes(anchors + bytes(groups))


def pack_player(read, encode_texture, dest, enemy=False, young=False, skin_model=None):
    skin_dir = 'data/database/skins/'+('C1_All_Johnny/' if enemy else 'young_max_payne/' if young else 'max_payne/')
    body = 'doe_johnny_l0' if enemy else 'young_max_payne_L0' if young else 'Payne_Max_L0'
    if skin_model:
        skin_dir,body=skin_model
    gun_dir = 'data/database/weapons/beretta/'
    anim_dir = 'data/database/skeletons/default_skeleton/anim/'
    with TemporaryDirectory() as directory:
        def document(name):
            data = read(name)
            path = Path(directory)/Path(name).name
            path.write_bytes(data)
            return MaxKF2Reader().parse(path)

        def tracks(name):
            doc = document(anim_dir + name + '.kf2')
            return {a.animation.object_name.casefold(): a for a in doc.keyframe_animations}

        skin, = document(skin_dir+body+'.skd').skins
        source_bones = [b.casefold() for b in skin.skeleton_object_names]
        bones = list(tracks('Widepose'))
        remap = [bones.index(b) for b in source_bones]
        bind = worlds(tracks('Widepose'), 0)
        if not enemy and not young:
            pack_aim_rig(dest.with_name('aim.bind'), bones, bind)
        inverse = {b: np.linalg.inv(bind[b]) for b in bones}
        weights = {v.vertex_index: (v.vertex_bone_indices, v.vertex_weights) for v in skin.skin_vertices}
        # A separate palette entry carries the gun socket transform in object space.
        socket = len(bones)
        upper = bytes([b not in ('pelvis',) and not b.startswith(('upleg','loleg','foot','toe'))
                       for b in bones] + [1])
        textures, parts, points, corners, indices = [], [], [], [], defaultdict(list)
        models = [(skin_dir,body+'.kfs',0),(gun_dir,'beretta_lod0.kf2',1),
                  ('data/database/weapons/pumpshotgun/','pumpshotgun_Lod0.kf2',2)]
        if young:
            models=models[:2]
        elif not enemy:
            models.append(('data/database/weapons/sniper/','sniper_lod0.kf2',3))
        ends = []
        for folder, filename, part in models:
            geom = document(folder+filename)
            model = read_model(read(folder+filename))
            texture_ids = {}
            for mat, name in model.materials.items():
                texture_ids[mat] = len(textures)
                texture_dir='data/database/skins/sharedtextures/' if part==0 and (enemy or (young and mat in ('Pants','Jacket'))) else folder+'textures/'
                if skin_model and part==0:
                    try:
                        bitmap=read(folder+'textures/'+name)
                    except KeyError:
                        bitmap=read('data/database/skins/sharedtextures/'+name)
                else:
                    bitmap=read(texture_dir+name)
                textures.append(encode_texture(Image.open(BytesIO(bitmap))))
                parts.append(part)
            transforms = {}
            for mesh, src in zip(model.meshes, geom.meshes, strict=True):
                node = src.node
                mat = affine(node.object_to_parent_transform)
                if node.parent_name:
                    mat = mat @ transforms[node.parent_name]
                transforms[node.name] = mat
                start = len(points)
                for i, raw in enumerate(src.geometry.vertices):
                    pos = (np.array([*raw, 1.]) @ mat)[:3]
                    joint, weight = ([socket], [1.]) if part else weights[i]
                    if not 1 <= len(joint) <= 3 or sum(weight) <= 0:
                        raise ValueError('Invalid skin weights')
                    if not part: joint = [remap[b] for b in joint]
                    weight = [w/sum(weight) for w in weight]
                    points.append(struct.pack('<3f4B3f', *pos, *(joint+[0]*(3-len(joint))),
                                              len(joint), *(weight+[0.]*(3-len(weight)))))
                shared = {}
                for face in mesh.faces:
                    batch = indices[texture_ids[mesh.materials[face.material]]]
                    for vi, ti in zip(face.positions, face.coords, strict=True):
                        key = vi, ti
                        if key not in shared:
                            shared[key] = len(corners)
                            corners.append(struct.pack('<I2f', start+vi, *mesh.coords[ti]))
                        batch.append(shared[key])
            ends.append((len(points),len(corners)))
        while len(ends)<4:
            ends.append(ends[-1])
        palettes = []
        for clip in (() if enemy else CLIPS[:-3] if young else CLIPS):
            channels = tracks(clip)
            root = channels['pelvis']
            duration = root.num_total_keyframes / root.animation.fps
            loop = bool(root.animation.is_looping)
            frame_count = max(2, round(duration*20))
            frames = bytearray()
            for i in range(frame_count):
                time = i*root.num_total_keyframes/frame_count if loop else i*(root.num_total_keyframes-1)/(frame_count-1)
                pose = worlds(channels, time)
                rows = [inverse[b] @ pose[b] for b in bones] + [pose['gun']]
                for matrix in rows:
                    frames.extend(matrix[:, :3].T.astype('<f4').tobytes())
            palettes.append(struct.pack('<If', frame_count, duration) + frames)
        if not enemy and not young:
            with dest.with_name('rifle.anim').open('wb') as anim:
                anim.write(struct.pack('<4sII',b'RFA1',len(upper),3))
                anim.write(b''.join(palettes[-3:]))
            palettes=palettes[:-3]
        with dest.open('wb') as out:
            out.write(struct.pack('<4s7I', b'MAX5' if skin_model else b'MAX4', len(points), len(corners),
                        sum(map(len, indices.values())),len(textures),len(upper),len(palettes),socket))
            for end in ends: out.write(struct.pack('<II',*end))
            out.write(upper)
            out.write(bytes(parts))
            # Both rigs use exactly the same weapon models and material order.
            for data, part in zip(textures, parts, strict=True):
                if skin_model or not enemy or not part:
                    out.write(data)
            out.write(b''.join(points))
            out.write(b''.join(corners))
            for i in range(len(textures)):
                batch = indices[i]
                out.write(struct.pack('<I', len(batch)))
                out.write(np.asarray(batch, dtype='<u2').tobytes())
            out.write(b''.join(palettes))
    print(f'{body}: {len(corners)} textured vertices, {len(bones)} bones, {len(palettes)} clips, {dest.stat().st_size} bytes')
