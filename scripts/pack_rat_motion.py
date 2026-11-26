"""Bake the small pigrat rig against its own Xbox bind pose."""
from collections import defaultdict
from pathlib import Path
import struct

import numpy as np
from dade.maxpayne.model import read_model
from max_payne_sdk.max_kf2 import MaxKF2Reader
from pack_player import affine, worlds

TRACKS=('stand','walk','run','strafeleft','straferight','run_forwardleft','run_forwardright',
        'turnleft','turnright','death_a','death_b','shoot','shoot_up','shoot_down','stand_pose')


def pack_rat(read,tmp,dest):
    def document(key):
        file=Path(tmp)/Path(key).name
        file.write_bytes(read(key))
        return MaxKF2Reader().parse(file)

    base='data/database/skins/rat/rat_l0'
    rig='data/database/skeletons/pigrat/'
    mesh,=document(base+'.kfs').meshes
    surface,=read_model(read(base+'.kfs')).meshes
    skin,=document(base+'.skd').skins
    bones=[b.lower() for b in skin.skeleton_object_names]
    bind={}
    for joint in document(rig+'pigrat-collision.kfs').meshes:
        node=joint.node
        matrix=affine(node.object_to_parent_transform)
        if node.parent_name:matrix=matrix@bind[node.parent_name.lower()]
        bind[node.name.lower()]=matrix
    points=np.array([(*v,1) for v in mesh.geometry.vertices])@affine(mesh.node.object_to_parent_transform)
    weights=np.zeros((len(bones),len(points)))
    for row in skin.skin_vertices:
        w=np.asarray(row.vertex_weights)
        weights[row.vertex_bone_indices,row.vertex_index]=w/w.sum()
    if not np.allclose(weights.sum(axis=0),1):raise ValueError('Invalid pigrat weights')
    local=np.stack([points@np.linalg.inv(bind[b]) for b in bones])
    corners={};batches=defaultdict(list)
    for face in surface.faces:
        batch=batches[surface.materials[face.material]]
        for key in zip(face.positions,face.coords,strict=True):
            batch.append(corners.setdefault(key,len(corners)))
    ids=np.array([k[0] for k in corners]);uv=np.array([surface.coords[k[1]] for k in corners])
    indices=np.asarray([k for group in batches.values() for k in group],dtype='<u2')
    with dest.with_suffix('.partial').open('wb') as f:
        f.write(struct.pack('<4s3I',b'RAT1',len(corners),len(indices),len(TRACKS)))
        f.write(indices.tobytes())
        f.write(uv.astype('<f4').tobytes())
        for name in TRACKS:
            doc=document(rig+'anim/rat_'+name+'.kf2')
            tracks={t.animation.object_name.lower():t for t in doc.keyframe_animations}
            root=tracks['pelvis'];length=root.num_total_keyframes;duration=length/root.animation.fps
            n=max(2,1+round(duration*12))
            f.write(struct.pack('<If',n,duration))
            start=worlds(tracks,0)['pelvis'][3,[0,2]]
            for frame in np.linspace(0,length-1,n):
                pose=worlds(tracks,frame)
                positions=sum((local[i]@pose[b])[:,:3]*weights[i,:,None] for i,b in enumerate(bones))
                if name not in ('death_a','death_b'):
                    positions[:,[0,2]]-=pose['pelvis'][3,[0,2]]-start
                packed=np.zeros((len(ids),4),dtype='<i2')
                xyz=np.rint(positions[ids]*4096)
                if abs(xyz).max()>32767:raise ValueError(name)
                packed[:,:3]=xyz
                f.write(packed.tobytes())
    dest.with_suffix('.partial').replace(dest)
