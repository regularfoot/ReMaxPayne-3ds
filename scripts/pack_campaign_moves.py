"""Bake the disc's shoot-dodge, crouch, melee and throwing poses for the native character rig."""
import argparse
import mmap
from pathlib import Path
import struct
from tempfile import TemporaryDirectory
import numpy as np
from dade.maxpayne import ras
from max_payne_sdk.max_kf2 import MaxKF2Reader
from pack_player import worlds
from animation_rules import encode as encode_body_sets
from xbox_disc import archive_root

CLIPS=('Crouch','ShootDodgeForward','ShootDodgeBackward','ShootDodgeLeft',
       'ShootDodgeRight','ShootBaseballBat','ShootGrenade')


def hierarchy(tracks):
    names=list(tracks)
    parents=[names.index(t.parent_name.lower()) if t.parent_name else -1 for t in tracks.values()]+[names.index('gun')]
    if any(parent>=i for i,parent in enumerate(parents)):
        raise ValueError('Skeleton parents must precede their children')
    return struct.pack('<4sI',b'MPT1',len(parents))+struct.pack('<%dh'%len(parents),*parents)

def pack(disc,dest):
    path='data/database/skeletons/default_skeleton/anim/'
    temp=dest.with_suffix('.partial')
    with TemporaryDirectory() as work,(archive_root(disc)/'x_data.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        entries={e.path.lower():e for e in ras.read_directory(data).entries}
        def channels(name):
            file=Path(work)/'source.kf2'
            file.write_bytes(ras.member_bytes(data,entries[path+name.lower()+'.kf2']))
            return {a.animation.object_name.lower():a for a in MaxKF2Reader().parse(file).keyframe_animations}
        wide=channels('Widepose');bones=list(wide);bind=worlds(wide,0)
        skeleton=ras.member_bytes(data,entries['data/database/skeletons/default_skeleton.txt']).decode('latin1')
        weapons={Path(name).stem:ras.member_bytes(data,entry).decode('latin1') for name,entry in entries.items()
                 if name.startswith('data/database/weapons/') and name.count('/')==3 and name.endswith('.txt')}
        (dest.parent/'bone_sets.bind').write_bytes(encode_body_sets(skeleton,bones,weapons))
        (dest.parent/'bones.tree').write_bytes(hierarchy(wide))
        rest=np.asarray([bind[b] for b in bones]+[np.eye(4)],dtype='<f4')[:,:,:3].transpose(0,2,1).copy()
        (dest.parent/'bones.rest').write_bytes(struct.pack('<4sI',b'RST1',len(rest))+rest.tobytes())
        (dest.parent/'bones.bind').write_bytes(struct.pack('<II',len(bones)+1,bones.index('gun'))+
            np.asarray([bind[b][3,:3] for b in bones]+[[0,0,0]],dtype='<f4').tobytes())
        (dest.parent/'torso.bind').write_bytes(struct.pack('<I3f',bones.index('torso'),*bind['torso'][3,:3]))
        (dest.parent/'hit_bones.bind').write_bytes(struct.pack('<4s4I',b'AHB1',len(bones)+1,*(bones.index(b) for b in ('neck','head','pelvis'))))
        reflection=np.diag([-1.,1.,1.,1.])
        left_socket=reflection@bind['gun']@reflection
        (dest.parent/'left_grip.bind').write_bytes(left_socket[:,:3].T.astype('<f4').tobytes())
        inverse=[np.linalg.inv(bind[b]) for b in bones]
        with temp.open('wb') as output:
            output.write(struct.pack('<4sII',b'MOV2',len(bones)+1,len(CLIPS)))
            for name in CLIPS:
                attrs=channels(name);root=attrs['pelvis']
                count=root.num_total_keyframes;duration=count/root.animation.fps
                output.write(struct.pack('<If',count,duration))
                first=worlds(attrs,0)['pelvis'][3,:3].copy()
                for frame in range(count):
                    pose=worlds(attrs,frame)
                    matrices=[inv@pose[b] for inv,b in zip(inverse,bones)]+[pose['gun'].copy()]
                    # The native collision controller owns horizontal root travel.
                    # Preserve every authored joint rotation and relative position.
                    if 'dodge' in name.lower():
                        travel=pose['pelvis'][3,:3]-first
                        for matrix in matrices:matrix[3,[0,2]]-=travel[[0,2]]
                    output.write(np.asarray(matrices,dtype='<f4')[:,:,:3].transpose(0,2,1).copy().tobytes())
    temp.replace(dest)
    for obsolete in ('strafe.anim','strides.bin'):
        (dest.parent/obsolete).unlink(missing_ok=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('disc',type=Path);p.add_argument('output',type=Path)
    args=p.parse_args();pack(args.disc,args.output)
