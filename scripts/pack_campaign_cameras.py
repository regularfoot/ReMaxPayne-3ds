"""Pack the original camera paths and preserve their frame and exit messages."""
import argparse,gzip,json,re,struct
from pathlib import Path
from tempfile import TemporaryDirectory
import numpy as np
from max_payne_sdk.max_kf2 import MaxKF2Reader
from pack_native import member
from pack_player import worlds


def catalog(disc):
    text=member(disc/'x_data.ras','data/database/camerapaths/camerapaths.txt').decode('latin1')
    values=dict(re.findall(r'#define\s+(\w+)\s+([.\d]+)',text))
    values.update(TRUE='true',FALSE='false')
    text=re.sub(r'//[^\r\n]*','',text)
    for name,body in re.findall(r'^\[(\w+)\]([^\[]*\{.*?)(?=^\[|\Z)',text,re.M|re.S):
        filename=re.search(r'Filename\s*=\s*"([^"]+)"',body,re.I)
        if not filename:continue
        events=[]
        for frame,msg in re.findall(r'\[Message\]\s*(?:Frame\s*=\s*(\d+)\s*;\s*)?String\s*=\s*"([^\r\n]*)"\s*;',body,re.I):
            events.append((int(frame) if frame else 65535,re.sub(r'"\s*(\w+)\s*"',lambda m:values.get(m[1],m[1]),msg)))
        yield name.lower(),(filename[1].replace('\\','/'),events)


def pack(disc,source,out):
    conf=dict(catalog(disc/'english'))
    tracks={}
    for src in source.glob('*.json.gz'):
        with gzip.open(src,'rt') as f:
            for mode,name in re.findall(r'->CAM_Animate(InPlace|Parented)\(\s*(\w+)',f.read(),re.I):
                tracks[name.lower()]=mode.lower()=='parented'
    rows=[];frames=[]
    with TemporaryDirectory() as scratch:
        original=sorted(tracks.items())
        extra={'gunshot_big':False,'gunshot_small':False,'player_death_01':True}
        for name,parented in original+[(k,v) for k,v in extra.items() if k not in tracks]:
            filename,_=conf[name]
            path=Path(scratch)/'camera.kf2';path.write_bytes(member(disc/'english/x_data.ras','data/database/camerapaths/'+filename))
            data=MaxKF2Reader().parse(path)
            channels={a.animation.object_name.lower():a for a in data.keyframe_animations}
            cam=next(k for k in channels if k.startswith('camera'))
            track=channels[cam];fps=track.animation.fps
            count=max(2,round(track.num_total_keyframes/fps*20)+1)
            ref=np.eye(4) if parented else np.linalg.inv(worlds(channels,0)[cam])
            rows.append(struct.pack('<32sII2f',name.encode(),len(frames),count,track.num_total_keyframes/fps,fps))
            for frame in np.linspace(0,track.num_total_keyframes-1,count):
                world=worlds(channels,frame)[cam]@ref
                frames.append(world[:,:3].astype('<f4').tobytes())
    tmp=out.with_suffix('.partial')
    tmp.write_bytes(struct.pack('<4sII',b'MPCV',len(rows),len(frames))+b''.join(rows+frames));tmp.replace(out)
    print(len(rows),'original camera paths,',len(frames),'poses')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('disc','source','output'):p.add_argument(key,type=Path)
    a=p.parse_args();pack(a.disc,a.source,a.output)
