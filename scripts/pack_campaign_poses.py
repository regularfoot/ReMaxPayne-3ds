"""Bake the original character-specific cinematic poses into a seekable palette bank."""
import argparse
import hashlib
import json
from pathlib import Path
import mmap
import struct
from tempfile import TemporaryDirectory
import numpy as np
from dade.maxpayne import ras
from max_payne_sdk.max_kf2 import MaxKF2Reader
from pack_player import worlds
from xbox_animation import sample_raw,normalization,track_scale
from skin_poses import definitions,slots
from pack_animation_timing import pack as pack_timing


def pack(disc,out):
    base='data/database/'
    with TemporaryDirectory() as work,(disc/'english/x_data.ras').open('rb') as archive,mmap.mmap(archive.fileno(),0,access=mmap.ACCESS_READ) as blob:
        entries={e.path.lower():e for e in ras.read_directory(blob).entries}
        def read(name):return ras.member_bytes(blob,entries[base+name])
        def skeleton(name):
            f=Path(work)/'clip.kf2'
            f.write_bytes(read('skeletons/default_skeleton/anim/'+name+'.kf2'))
            return {a.animation.object_name.lower():a for a in MaxKF2Reader().parse(f).keyframe_animations}
        table=definitions(read('skeletons/default_skeleton.txt').decode('latin1'))
        models={p.stem for p in (out/'cast').glob('*.actor')} | {'max_payne','young_max_payne'}
        names={};skins=[];tables=[]
        defaults=definitions(read("skeletons/default_skeleton.txt").decode("latin1"),all_slots=True)
        for model in sorted(models):
            ids=[]
            tables.append({**defaults,**definitions(read("skins/"+model+".txt").decode("latin1"),all_slots=True)})
            for name,_ in slots(table,read('skins/'+model+'.txt').decode('latin1')):
                ids.append(names.setdefault(name,len(names)+1) if name else 0)
            skins.append(struct.pack('<64s16H',model.encode(),*ids))
        # Keep existing cinematic IDs stable for saves. Append every retail human clip.
        for path in sorted(entries):
            if path.startswith(base+'skeletons/default_skeleton/anim/') and path.endswith('.kf2'):
                names.setdefault(Path(path).stem,len(names)+1)
        actions=sorted(set().union(*(t.keys() for t in tables)))
        def fixed(value,n):
            value=value.encode('ascii')
            if len(value)>=n:raise ValueError(value)
            return value.ljust(n,b'\0')
        index=bytearray(struct.pack('<4s3I',b'MPI1',len(names),len(actions),len(skins)))
        for action in actions:index.extend(fixed(action.removeprefix('charanim_'),64))
        for name in names:index.extend(fixed(name,64))
        for table in tables:
            for action in actions:
                entry=table.get(action);seen=set()
                while isinstance(entry,str) and entry not in seen:
                    seen.add(entry);entry=table.get(entry)
                index.extend(struct.pack('<H',names.get(entry[0],0) if isinstance(entry,tuple) else 0))
        wide=skeleton('widepose');bind=worlds(wide,0);bones=list(wide);inv={b:np.linalg.inv(bind[b]) for b in bones}
        rest=np.asarray([bind[b] for b in bones]+[np.eye(4)],dtype='<f4')[:,:,:3].transpose(0,2,1).copy()
        (out/'bones.rest').write_bytes(struct.pack('<4sI',b'RST1',len(rest))+rest.tobytes())
        normalization_rows=[];scaling_rows=[]
        grip=bind['gun']@inv['hand-r']
        first=16+96*len(skins)+16*len(names)
        tmp=out/'poses.partial'
        with tmp.open('wb') as f:
            f.write(struct.pack('<4s3I',b'MPP2',len(skins),len(names),len(bones)+1))
            f.writelines(skins);f.write(bytes(16*len(names)))
            clips=[];metadata=[];sources=[]
            for name in names:
                tracks=skeleton(name);root=tracks.get('pelvis') or next(iter(tracks.values()));fps=root.animation.fps
                secs=root.num_total_keyframes/fps
                count=root.num_total_keyframes
                if not 2<=count<=8192:raise ValueError((name,count))
                anim=root.animation
                loop_start=int(root.loop_to_frame)
                flags=int(bool(anim.is_looping)) | (int(bool(root.use_loop_interpolation))<<1)
                if flags&1 and not 0<=loop_start<count:raise ValueError((name,loop_start))
                metadata.append(struct.pack('<3I',count,loop_start,flags))
                sources.append(dict(id=names[name],name=name,frames=count,fps=fps,loop_start=loop_start,
                                    flags=flags,source_sha256=hashlib.sha256(read('skeletons/default_skeleton/anim/'+name+'.kf2')).hexdigest()))
                normalization_rows.append(bytes([normalization(tracks[b]) if b in tracks else 0 for b in bones]+[0]))
                if any(t.maintain_matrix_scaling for t in tracks.values()):
                    scales=np.asarray([track_scale(tracks[b]) if b in tracks else [1,1,1] for b in bones]+[[1,1,1]],dtype='<f4')
                    scaling_rows.append(struct.pack('<I',names[name])+scales.tobytes())
                if f.tell()!=first:raise ValueError(name)
                clips.append(struct.pack('<2I2f',first,count,secs,fps))
                for t in range(count):
                    # Store local matrices before model-wide orthonormalization.
                    # Interpolating finalized world palettes changes the Xbox curve.
                    pose=[]
                    for bone in bones:
                        if bone in tracks:local=sample_raw(tracks[bone],t)
                        else:
                            parent=wide[bone].parent_name.lower()
                            local=bind[bone]@inv[parent] if parent else bind[bone]
                        pose.append(local)
                    # Socket follows the authored Gun node; its bind frame is identity.
                    pose.append(np.eye(4))
                    f.write(np.asarray(pose,dtype='<f4')[:,:,:3].transpose(0,2,1).copy().tobytes())
                first=f.tell()
            f.seek(16+96*len(skins));f.writelines(clips)
        tmp.replace(out/'poses.pak')
        (out/'poses.idx').write_bytes(index)
        (out/'poses.norm').write_bytes(struct.pack('<4sIII',b'MPN2',len(names),len(bones)+1,len(scaling_rows))+b''.join(normalization_rows+scaling_rows))
        (out/'poses.meta').write_bytes(struct.pack('<4sI',b'MPM1',len(metadata))+b''.join(metadata))
        manifest=dict(format='xbox-local-matrix-v2',codec='xbox-12bit-quaternion-half-position',interpolation='local-matrix-gram-schmidt',clips=sources,
                      bank_sha256=hashlib.sha256((out/'poses.pak').read_bytes()).hexdigest(),
                      metadata_sha256=hashlib.sha256((out/'poses.meta').read_bytes()).hexdigest())
        (out/'animations.source.json').write_text(json.dumps(manifest,indent=2)+'\n')
        pack_timing(disc,out)
        print(len(skins),'skins,',len(names),'original character clips')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('disc',type=Path);p.add_argument('out',type=Path);a=p.parse_args();pack(a.disc,a.out)
