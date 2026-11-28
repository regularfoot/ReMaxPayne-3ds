"""Bake rigid KF2 node motion relative to the same bind pose as the item mesh."""
from pathlib import Path
import posixpath,re,struct
import numpy as np
from max_payne_sdk.max_kf2 import MaxKF2Reader
from pack_player import affine,sample

def pack_weapon_motion(read,config,base,doc,draw_runs,scratch,out):
    # Muzzle particles and projectiles use the authored weapon-local frame.
    clean=re.sub(r'//[^\r\n]*','',config)
    offset=re.search(r'\bProjectileOffset\s*=\s*\(([^)]+)\)',clean,re.I)
    offset=tuple(map(float,offset[1].split(','))) if offset else (0.,0.,0.)
    effects=re.findall(r'PS_StartEffect\s*\(\s*(\w+)',clean,re.I)
    if len(set(effects))>1:raise ValueError('Multiple weapon muzzle effects: '+base)
    effect=effects[0].lower().encode() if effects else b''
    out.with_suffix('.fx').write_bytes(struct.pack('<4s3f64s',b'WFX1',*offset,effect))
    bones=[g.node.name.lower() for g in doc.meshes]
    parents=[g.node.parent_name.lower() for g in doc.meshes]
    rest=[affine(g.node.object_to_parent_transform) for g in doc.meshes]
    def world(local):
        result={}
        def resolve(n):
            if n not in result:
                i=bones.index(n);p=parents[i]
                result[n]=local[i]@resolve(p) if p in bones else local[i]
            return result[n]
        return [resolve(n) for n in bones]
    inverse=[np.linalg.inv(m) for m in world(rest)]
    runs=[struct.pack('<HHI',node,material|(0x8000 if cull else 0),count)
          for node,material,cull,count in draw_runs]
    animations=dict((k.upper(),v.strip().replace('\\','/').lower()) for k,v in re.findall(
        r'\[Animation\]\s*Index\s*=\s*(\w+);\s*Filename\s*=\s*([^;]+)',config,re.I))
    tracks=[]
    for action in ('DEFAULT','SHOOT','RELOAD'):
        local=Path(scratch)/'weapon-track.kf2'
        local.write_bytes(read(posixpath.normpath(base+'/'+animations['WEAPONANIM_'+action])))
        keys={c.animation.object_name.lower():c for c in MaxKF2Reader().parse(local).keyframe_animations}
        duration=max((c.num_total_keyframes/c.animation.fps for c in keys.values()),default=1/30)
        frames=max(2,min(240,round(duration*30)+1))
        tracks.append(struct.pack('<If',frames,duration))
        for t in np.linspace(0,duration,frames):
            local_pose=[sample(keys[n],min(t*keys[n].animation.fps,keys[n].num_total_keyframes-1)) if n in keys and keys[n].keyframes else rest[i] for i,n in enumerate(bones)]
            tracks.extend((inv@pose)[:,:3].T.astype('<f4').tobytes() for inv,pose in zip(inverse,world(local_pose)))
    out.write_bytes(struct.pack('<4sII',b'WPN1',len(bones),len(runs))+b''.join(runs+tracks))
