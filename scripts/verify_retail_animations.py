"""Reject generated movement assets and verify palettes against the local Xbox disc."""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import struct
from tempfile import TemporaryDirectory
import numpy as np
from dade.maxpayne import ras
from max_payne_sdk.max_kf2 import MaxKF2Reader
from pack_player import worlds
from xbox_animation import sample_raw,normalization,track_scale
from pack_campaign_moves import CLIPS
from skin_poses import definitions
from animation_rules import encode as encode_body_sets
from pack_animation_timing import encode as encode_timing
from xbox_disc import archive_root


def verify(disc,folder):
    if (folder/'transitions.bin').read_bytes()!=encode_timing(disc,folder):raise ValueError('Changed Xbox transition timing or phase-pair rules')
    print('All Xbox action transition times and 176 phase/time pairs verified',flush=True)
    for name in ('strafe.anim','strides.bin'):
        if (folder/name).exists():raise ValueError('Retired generated animation asset: '+name)
    manifest=json.loads((folder/'animations.source.json').read_text())
    if manifest['format']!='xbox-local-matrix-v2':raise ValueError('Unknown animation provenance')
    data=(folder/'poses.pak').read_bytes();meta=(folder/'poses.meta').read_bytes();idx=(folder/'poses.idx').read_bytes()
    if hashlib.sha256(data).hexdigest()!=manifest['bank_sha256'] or hashlib.sha256(meta).hexdigest()!=manifest['metadata_sha256']:
        raise ValueError('Animation data changed after source export')
    tag,skins,count,bone_count=struct.unpack_from('<4s3I',data)
    itag,icount,actions,iskins=struct.unpack_from('<4s3I',idx)
    if (tag,itag,icount,iskins)!=(b'MPP2',b'MPI1',count,skins) or len(manifest['clips'])!=count:
        raise ValueError('Invalid source animation index')
    if meta[:8]!=struct.pack('<4sI',b'MPM1',count) or len(meta)!=8+12*count:raise ValueError('Invalid source loop metadata')
    names=[idx[16+64*actions+64*i:16+64*actions+64*(i+1)].split(b'\0')[0].decode() for i in range(count)]
    with TemporaryDirectory() as scratch,(archive_root(disc)/'x_data.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as archive:
        entries={e.path.lower():e for e in ras.read_directory(archive).entries}
        base='data/database/skeletons/default_skeleton/anim/'
        source_names={Path(p).stem for p in entries if p.startswith(base) and p.endswith('.kf2')}
        if set(names)!=source_names:raise ValueError('Missing or non-retail human animation')
        def text(path):return ras.member_bytes(archive,entries['data/database/'+path]).decode('latin1')
        defaults=definitions(text('skeletons/default_skeleton.txt'),all_slots=True)
        source_tables=[]
        for i in range(skins):
            skin=data[16+96*i:16+96*i+64].split(b'\0')[0].decode()
            source_tables.append({**defaults,**definitions(text('skins/'+skin+'.txt'),all_slots=True)})
        expected_actions=sorted(set().union(*(t.keys() for t in source_tables)))
        indexed_actions=[idx[16+64*i:16+64*(i+1)].split(b'\0')[0].decode() for i in range(actions)]
        if indexed_actions!=[a.removeprefix('charanim_') for a in expected_actions]:raise ValueError('Non-retail action table')
        bindings=np.frombuffer(idx,dtype='<u2',offset=16+64*(actions+count)).reshape(skins,actions)
        name_ids={name:i+1 for i,name in enumerate(names)}
        for row,table in zip(bindings,source_tables):
            for actual,action in zip(row,expected_actions):
                entry=table.get(action);seen=set()
                while isinstance(entry,str) and entry not in seen:
                    seen.add(entry);entry=table.get(entry)
                wanted=name_ids.get(entry[0],0) if isinstance(entry,tuple) else 0
                if actual!=wanted:raise ValueError('Changed Xbox action binding: '+action)
        def tracks(name):
            raw=ras.member_bytes(archive,entries[base+name.lower()+'.kf2'])
            file=Path(scratch)/'clip.kf2';file.write_bytes(raw)
            parsed=MaxKF2Reader().parse(file)
            return {a.animation.object_name.lower():a for a in parsed.keyframe_animations},hashlib.sha256(raw).hexdigest()
        wide,_=tracks('widepose');bones=list(wide);bind=worlds(wide,0)
        weapons={Path(p).stem:ras.member_bytes(archive,entry).decode('latin1') for p,entry in entries.items()
                 if p.startswith('data/database/weapons/') and p.count('/')==3 and p.endswith('.txt')}
        wanted=encode_body_sets(text('skeletons/default_skeleton.txt'),bones,weapons)
        if (folder/'bone_sets.bind').read_bytes()!=wanted:
            raise ValueError('Changed Xbox body bone sets or weapon pose flags')
        print('Xbox one/two-handed bone sets and independent weapon pose flags verified',flush=True)
        inverse={b:np.linalg.inv(bind[b]) for b in bones};grip=bind['gun']@inverse['hand-r']
        rest=np.asarray([bind[b] for b in bones]+[np.eye(4)],dtype='<f4')[:,:,:3].transpose(0,2,1).copy()
        if (folder/'bones.rest').read_bytes()!=struct.pack('<4sI',b'RST1',len(rest))+rest.tobytes():raise ValueError('Changed bind axes')
        norms=(folder/'poses.norm').read_bytes()
        if norms[:12]!=struct.pack('<4sII',b'MPN2',count,bone_count):raise ValueError('Invalid temporal normalization table')
        scaling_rows=[]
        if bone_count!=len(bones)+1:raise ValueError('Unexpected source skeleton')
        samples=0;keys=0;worst=0.
        for i,info in enumerate(manifest['clips']):
            name=names[i];ch,sha=tracks(name);root=ch.get('pelvis') or next(iter(ch.values()));a=root.animation
            offset,n,seconds,fps=struct.unpack_from('<2I2f',data,16+96*skins+16*i)
            flags=int(bool(a.is_looping)) | (int(bool(root.use_loop_interpolation))<<1)
            expected=(root.num_total_keyframes,int(root.loop_to_frame),flags)
            if info['id']!=i+1 or info['name']!=name or info['source_sha256']!=sha or n!=expected[0] or abs(fps-a.fps)>.0001 or abs(seconds-n/fps)>.0001:
                raise ValueError('Non-retail clip or altered playback rate: '+name)
            if struct.unpack_from('<3I',meta,8+12*i)!=expected:raise ValueError('Altered retail loop: '+name)
            wanted_modes=bytes([normalization(ch[b]) if b in ch else 0 for b in bones]+[0])
            if norms[16+i*bone_count:16+(i+1)*bone_count]!=wanted_modes:raise ValueError('Changed source normalization flags: '+name)
            if any(t.maintain_matrix_scaling for t in ch.values()):
                scales=np.asarray([track_scale(ch[b]) if b in ch else [1,1,1] for b in bones]+[[1,1,1]],dtype='<f4')
                scaling_rows.append(struct.pack('<I',i+1)+scales.tobytes())
            # All authored key frames, not just first/last poses. This catches a
            # modified middle of a clip even when its name and timing are intact.
            frames=sorted({0,n//2,n-1}|{k.frame_id for t in ch.values() for k in t.keyframes if 0<=k.frame_id<n})
            for frame in frames:
                expected_pose=[]
                for bone in bones:
                    if bone in ch:local=sample_raw(ch[bone],frame)
                    else:
                        parent=wide[bone].parent_name.lower();local=bind[bone]@inverse[parent] if parent else bind[bone]
                    expected_pose.append(local)
                expected_pose.append(np.eye(4))
                expected_pose=np.asarray(expected_pose)[:,:,:3].transpose(0,2,1)
                actual=np.frombuffer(data,dtype='<f4',count=bone_count*12,offset=offset+frame*bone_count*48).reshape(bone_count,3,4)
                error=float(np.max(np.abs(actual-expected_pose)));worst=max(worst,error)
                if error>.00003:raise ValueError(f'Altered source pose {name} frame {frame}: {error}')
                samples+=1
            keys+=sum(len(t.keyframes) for t in ch.values())
        if norms!=struct.pack('<4sIII',b'MPN2',count,bone_count,len(scaling_rows))+norms[16:16+count*bone_count]+b''.join(scaling_rows):raise ValueError('Changed maintained source scaling')
        moves=(folder/'moves.anim').read_bytes();tag,n_bones,n_clips=struct.unpack_from('<4sII',moves);offset=12
        if (tag,n_bones,n_clips)!=(b'MOV2',bone_count,len(CLIPS)):raise ValueError('Generated resident movement data')
        for name in CLIPS:
            ch,_=tracks(name);root=ch['pelvis'];n,seconds=struct.unpack_from('<If',moves,offset);offset+=8
            if n!=root.num_total_keyframes or abs(seconds-n/root.animation.fps)>.0001:raise ValueError('Altered resident clip: '+name)
            origin=worlds(ch,0)['pelvis'][3,:3].copy()
            for frame in range(n):
                pose=worlds(ch,frame);palettes=[inverse[b]@pose[b] for b in bones]+[pose['gun'].copy()]
                if 'dodge' in name.lower():
                    shift=pose['pelvis'][3,:3]-origin
                    for m in palettes:m[3,[0,2]]-=shift[[0,2]]
                expected_pose=np.asarray(palettes)[:,:,:3].transpose(0,2,1)
                actual=np.frombuffer(moves,dtype='<f4',count=bone_count*12,offset=offset).reshape(bone_count,3,4)
                if not np.allclose(actual,expected_pose,rtol=0,atol=.00003):raise ValueError(f'Generated resident pose: {name} {frame}')
                offset+=bone_count*48
        if offset!=len(moves):raise ValueError('Extra resident animation data')
    print(f'Xbox-only animation gate: {count} source clips, {samples} sampled key frames, {keys} track keys, max palette error {worst:.8g}',flush=True)
    return dict(clips=count,frames=samples,track_keys=keys,maximum_error=worst)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('disc',type=Path);p.add_argument('folder',type=Path)
    a=p.parse_args();verify(a.disc,a.folder)
