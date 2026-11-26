"""Compile authored LDB events into native tables with local entity and string identifiers."""
import argparse
from collections import defaultdict
import csv
import gzip
from io import BytesIO
import json
import mmap
from pathlib import Path
import re
import struct
import numpy as np
from dade.maxpayne import ras
from pack_native import member
from skin_poses import slots,definitions
from pack_campaign_cameras import catalog as camera_catalog
from xbox_disc import archive_root

NONE=65535
START,SWITCH,SEND,EVENT,LEAVE,ARRIVE,RETURN,DEATH,ACTIVATE,SPECIAL=range(10)
FIRST_PICKUP,PICKUP,POSE,CAMERA=10,11,12,13


def canonical(s):
    return re.sub(r'\.(do|trigger)$','',s.strip().lower())


def animation_samples(animation,inverse,to_room):
    start=np.asarray(animation['start_transform'],dtype=float)
    end=np.asarray(animation['end_transform'],dtype=float)
    if np.allclose(start,end,atol=1e-6):
        return b''
    begin=start[:3];finish=end[:3]
    left,_,right=np.linalg.svd(begin)
    first=left@right
    left,_,right=np.linalg.svd(finish)
    last=left@right
    relative=first.T@last
    angle=np.arccos(np.clip((np.trace(relative)-1)*.5,-1,1))
    axis=np.zeros((3,3))
    if angle>1e-5:
        # The eigenvector parallel to a rotation axis is stable at a half-turn.
        _,_,basis=np.linalg.svd(relative-np.eye(3))
        v=basis[-1]
        skew=np.array([relative[2,1]-relative[1,2],relative[0,2]-relative[2,0],relative[1,0]-relative[0,1]])
        if v@skew<0:v=-v
        x,y,z=v;axis=np.array([[0,-z,y],[z,0,-x],[-y,x,0]])
    u=np.linspace(0,1,33)
    def graph(key):
        values=animation[key]['points']
        if len(values)<2 or abs(values[-1]-values[0])<1e-7:
            return u
        # Translation keys store travelled metres. Convert the curve to the
        # fraction between the two authored transforms before interpolation.
        scale=(np.asarray(values)-values[0])/(values[-1]-values[0])
        return np.interp(u,np.linspace(0,1,len(values)),scale)
    samples=[]
    for move,turn in zip(graph('translation_graph'),graph('rotation_graph')):
        t=turn*angle
        rotation=first@(np.eye(3)+np.sin(t)*axis+(1-np.cos(t))*(axis@axis))
        m=np.eye(4)
        m[:3,:3]=(begin@first.T*(1-turn)+finish@last.T*turn)@rotation
        m[3,:3]=start[3]*(1-move)+end[3]*move
        samples.append((inverse@m@to_room)[:,:3])
    frames=np.asarray(samples,dtype='<f4')
    stride=1
    while stride<32:
        subset=frames[::stride]
        if np.max(abs(subset[1::2]-(subset[::2][:-1]+subset[::2][1:])*.5))>.001:break
        stride*=2
    frames=frames[::stride]
    return frames.tobytes()


def character_templates(disc):
    headers = member(disc/'x_headers.ras', 'data/database/ai_ranges.h').decode('latin1')
    constants = dict(re.findall(r'^#define\s+(\w+)\s+(\d+)\b', headers, re.M))
    templates = {}
    default_poses=definitions(member(disc/'x_data.ras','data/database/skeletons/default_skeleton.txt').decode('latin1'))
    skeletons={'default_skeleton':default_poses}
    with (disc/'x_data.ras').open('rb') as archive, mmap.mmap(archive.fileno(), 0, access=mmap.ACCESS_READ) as data:
        for entry in ras.read_directory(data).entries:
            match = re.fullmatch(r'data/database/skins/([^/]+)\.txt', entry.path.lower())
            if not match:
                continue
            body = re.sub(r'//[^\r\n]*|/\*.*?\*/', '', ras.member_bytes(data,entry).decode('latin1'),flags=re.S)
            health = re.search(r'\bMaximumHealth\s*=\s*(\w+)', body, re.I)
            if not health:
                continue
            value = health[1]
            events = {}
            for tag, channel in [('OnInit',START),('OnDeath',DEATH),('OnActivate',ACTIVATE),('OnSpecial',SPECIAL)]:
                section = re.search(r'\['+tag+r'\]\s*\{([^}]*)\}', body,re.I)
                if section:
                    events[channel] = re.findall(r'\[Message\]\s+String\s*=\s*"(.*)"\s*;',section[1],re.I)
            skeleton=re.search(r'\bSkeleton\s*=\s*(\w+)',body,re.I)
            name=skeleton[1].lower() if skeleton else 'default_skeleton'
            if name not in skeletons:
                skeletons[name]=definitions(member(disc/'x_data.ras','data/database/skeletons/'+name+'.txt').decode('latin1'))
            current=slots(skeletons[name],body)
            legacy=slots(default_poses,body) if name!='default_skeleton' else current
            templates[match[1]]=(int(constants.get(value,value)),events,current,legacy)
    return templates


def item_templates(disc):
    headers=member(disc/'x_headers.ras','data/database/level_items.h').decode('latin1')
    constants=dict(re.findall(r'^#define\s+(\w+)\s+([\d.]+)',headers,re.M))
    templates={}
    with (disc/'x_data.ras').open('rb') as archive,mmap.mmap(archive.fileno(),0,access=mmap.ACCESS_READ) as data:
        for entry in ras.read_directory(data).entries:
            match=re.fullmatch(r'data/database/level_items/([^/]+)\.txt',entry.path.lower())
            if not match:continue
            body=re.sub(r'//[^\r\n]*','',ras.member_bytes(data,entry).decode('latin1'))
            radius=re.search(r'\bTriggerRadius\s*=\s*([\w.]+)',body,re.I)
            events={}
            for tag,channel in [('OnFirstPickup',FIRST_PICKUP),('OnPickup',PICKUP)]:
                section=re.search(r'\['+tag+r'\]\s*\{([^}]*)\}',body,re.I)
                if section:
                    events[channel]=[re.sub(r'"\s*(\w+)\s*"',lambda m:constants.get(m[1].upper(),m[1]),m.replace('\\"','"'))
                        for m in re.findall(r'\[Message\]\s+String\s*=\s*"(.*)"\s*;',section[1],re.I)]
            if radius:templates[match[1]]=(float(constants.get(radius[1].upper(),radius[1])),events)
    return templates


def compile_level(s,out,templates,items,stage,cameras=None):
    # Checkpoints contain string indices. On affected maps retain the old ordering
    # and accept its signature only for an otherwise identical stage definition.
    skins=[templates[c['character_name'].lower()] for c in s['characters']]
    strings={'':0}
    legacy=None
    if any(t[2]!=t[3] for t in skins):
        legacy=BytesIO()
        _write_level(s,legacy,templates,items,stage,cameras,strings,3)
    data=BytesIO()
    _write_level(s,data,templates,items,stage,cameras,strings,2)
    with out.open('wb') as f:
        f.write(data.getbuffer())
        if legacy:
            f.write(struct.pack('<4sI',b'SCV1',_table_hash(legacy)^_table_hash(data)))

def _table_hash(stream):
    data=stream.getbuffer()
    _,entities,events,clips,msgs,nstrings,nbytes=struct.unpack_from('<4s6I',data)
    at=28
    result=0
    for size in (48*entities,12*events,104*clips,20*msgs,4*nstrings,nbytes):
        # The offset table is identical because saved indices are preserved.
        h=2166136261
        for byte in data[at:at+size]:
            h=((h^byte)*16777619)&0xffffffff
        result^=h
        at+=size
    return result

def _write_level(s,f,templates,items,stage,cameras,strings,pose_table):
    used=set()
    def intern(name):
        name=name.strip().lower()
        used.add(name)
        return strings.setdefault(name,len(strings))
    names={}
    entities=[]
    def entity(name):
        name=canonical(name)
        if name not in names:
            names[name]=len(entities)
            entities.append(dict(name=name,model='',state='',room=NONE,prop=NONE,exit=NONE,pair=NONE,
                                 trigger=NONE,flags=0,position=(0,0,0,0),radius=0,events=[],clips=[]))
        return entities[names[name]]
    player_skin='young_max_payne' if stage=='part0_level1' else 'max_payne'
    entity('player').update(health=templates[player_skin][0],model=player_skin)
    for group in ['waypoints','items','characters','triggers','props','fsms']:
        for i,obj in enumerate(s[group]):
            e=entity(obj['shared_name']);p=obj.get('properties',obj.get('object_properties'))
            t=p['object_to_room_transform']
            e['room']=p['room_id']
            if group!='fsms' or not e['flags']:
                e['position']=(*t[3],np.arctan2(t[2][0],t[2][2]))
                e['basis']=(*t[2],*t[1])
                e['parent']=canonical(p['parent_dynamic_mesh_name'])
            e['flags']|={'waypoints':1,'items':2,'characters':4,'triggers':8,'props':16,'fsms':32}[group]
            if group=='items':e['model']=obj['item_name']
            if group=='characters':e['model']=obj['character_name']
            if group=='triggers':e.update(trigger=obj['type'],radius=obj['radius'])
            if group=='props':
                e['prop']=i
                e['flags']|=512
                if obj['config']['dynamic_collisions']:e['flags']|=128
                if obj['config']['bullet_collisions']:e['flags']|=256
            if group=='fsms':e['state']=obj['states']['default']
    exit_ids={x['exit_name'].lower():i for i,x in enumerate(s['exits'])}
    for i,x in enumerate(s['exits']):
        e=entity(x['exit_name']);e.update(exit=i,pair=exit_ids[x['parent_room_name'].lower()],room=x['room_id'])
        e['flags']|=64
    commands=[]
    def add(e,channel,key,condition,msgs):
        start=len(commands)
        for text in msgs['messages']:
            match=re.fullmatch(r'\s*(.*?)\s*->\s*(\w+)\s*\((.*)\)\s*;?\s*',text,re.S)
            if not match:raise ValueError(f'Invalid message {text!r}')
            target,op,raw=match.groups()
            args=next(csv.reader([raw],skipinitialspace=True)) if raw.strip() else []
            if len(args)>7:raise ValueError(text)
            commands.append(struct.pack('<10H',intern(canonical(target)),intern(op),len(args),
                                        *[intern(v.strip().strip('"')) for v in args],*([0]*(7-len(args)))))
        if len(commands)!=start:
            e['events'].append((channel,intern(key),NONE if condition is None else condition if channel in (POSE,CAMERA) else intern(condition),len(commands)-start,start))
    for x in s['fsms']:
        e=entity(x['shared_name'])
        for state in x['states']['states']:
            e['events'].append((SWITCH,intern(state),NONE,0,len(commands)))
        for key in ('startup_before','startup_after'):add(e,START,'',None,x[key])
        for cat,ch in [('state_switch',SWITCH),('string_specific',SEND),('entity_specific',EVENT)]:
            for event in x[cat]['events']:
                key=event['state_name']
                add(e,ch,key,None,event['before'])
                for sub in event['state_specific']['messages']:add(e,ch,key,sub['state_name'],sub['messages'])
                add(e,ch,key,None,event['after'])
    for x in s['characters']:
        e=entity(x['shared_name'])
        e['health'],defaults=templates[x['character_name'].lower()][:2]
        for k,ch in [('startup_before',START),('on_death_before',DEATH),('on_activate_before',ACTIVATE),('on_special_before',SPECIAL)]:
            add(e,ch,'',None,{'messages':[re.sub(r'^this\s*->',x['shared_name']+'->',m,flags=re.I)
                                         for m in defaults.get(ch,[])]})
            add(e,ch,'',None,x[k])
    for x in s['items']:
        e=entity(x['shared_name'])
        e['radius'],item_events=items[x['item_name'].lower()]
        for channel,messages in item_events.items():
            add(e,channel,'',None,{'messages':messages})
    for ent in entities:
        if ent['name']!='player' and not ent['flags']&4:continue
        skin=templates.get(ent['model'])
        if not skin:continue
        for variant,(_,cues) in enumerate(skin[pose_table]):
            key=f'pose{variant}'
            intern(key)
            for frame,cmd in cues:
                add(ent,POSE,key,frame,{'messages':[re.sub(r'^this\s*->',ent['name']+'->',cmd,flags=re.I)]})
    def add_camera(name):
        for frame,cmd in cameras[name][1]:
            add(entities[0],CAMERA,name,frame,{'messages':[cmd]})
    early_cameras=[name for name in (cameras or {}) if name in used]
    for name in early_cameras:add_camera(name)
    def mat(t):
        m=np.eye(4);m[:,:3]=t;return m
    clips=[];tracks=[]
    for x in s['props']:
        e=entity(x['shared_name'])
        rest=mat(x['properties']['object_to_room_transform'])
        # Local mesh vertices were baked into room coordinates by the scene packer.
        parent=mat(x['properties']['object_to_parent_transform'])
        for a in x['animations']['animations']:
            name=a['animation_name']
            for k,ch in [('leaving_first_frame',LEAVE),('reaching_second_frame',ARRIVE),('returning_first_frame',RETURN)]:
                add(e,ch,name,None,a[k])
            inverse=np.linalg.pinv(rest)
            to_room=np.linalg.pinv(parent)@rest
            start=inverse@mat(a['start_transform'])@to_room
            end=inverse@mat(a['end_transform'])@to_room
            e['clips'].append(struct.pack('<HH25f',intern(name),0,a['length_in_secs'],*start[:,:3].flat,*end[:,:3].flat))
            e.setdefault('tracks',[]).append(animation_samples(a,inverse,to_room))
    for weapon in ('beretta','berettadual','deserteagle','pumpshotgun','sawedshotgun','jackhammer',
                   'ingram','ingramdual','mp5','sniper','baseballbat','leadpipe','grenade','molotov','m79'):
        intern(weapon)
    # Previous saves store string indices. Intern the original record fields
    # before appending any new cues referenced only by prop animations.
    for e in entities:
        for field in ('name','model','state'):intern(e[field])
    for name in (cameras or {}):
        if name in used and name not in early_cameras:add_camera(name)
    records=[];events=[]
    for e in entities:
        records.append(struct.pack('<12H5f2H',intern(e['name']),intern(e['model']),intern(e['state']),e['flags'],
                                   e['room'],e['prop'],e['exit'],e['pair'],e['trigger'],len(events),
                                   len(e['events']),len(clips),*e['position'],e['radius'],len(e['clips']),e.get('health',100)))
        events.extend(struct.pack('<4HI',*row) for row in e['events']);clips.extend(e['clips'])
        tracks.extend(e.get('tracks',[]))
    dictionary=bytearray();offsets=[]
    for key in strings:offsets.append(len(dictionary));dictionary.extend(key.encode('utf-8')+b'\0')
    f.write(struct.pack('<4s6I',b'EVT1',len(records),len(events),len(clips),len(commands),len(offsets),len(dictionary)))
    f.writelines(records);f.writelines(events);f.writelines(clips);f.writelines(commands)
    f.write(struct.pack(f'<{len(offsets)}I',*offsets));f.write(dictionary)
    f.write(struct.pack('<4sI',b'XFR1',sum(len(track)//48 for track in tracks)))
    for e in entities:
        f.write(struct.pack('<2H6f',names.get(e.get('parent',''),NONE),0,*e.get('basis',(0,0,1,0,1,0))))
    at=0
    for track in tracks:
        count=len(track)//48
        f.write(struct.pack('<IHH',at,count,0));at+=count
    f.writelines(tracks)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--disc',type=Path,default=Path('game'))
    args=p.parse_args()
    templates=character_templates(archive_root(args.disc))
    items=item_templates(archive_root(args.disc))
    cameras=dict(camera_catalog(archive_root(args.disc)))
    for path in sorted(args.source.glob('*.json.gz')):
        dest=args.output/(path.name.split('.')[0]+'.evt')
        temp=dest.with_suffix('.partial')
        compile_level(json.load(gzip.open(path)),temp,templates,items,dest.stem,cameras);temp.replace(dest)
