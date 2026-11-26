"""Compile per-skin frame messages. Actions sharing a KF2 can have different messages."""
import argparse
import mmap
import re
import struct
from pathlib import Path
from dade.maxpayne import ras
from skin_poses import definitions,SLOTS
from pack_native import member
import skin_motion
from xbox_disc import archive_root

def pack(disc,folder):
    idx=(folder/'poses.idx').read_bytes()
    tag,nclips,na,ns=struct.unpack_from('<4s3I',idx)
    assert tag==b'MPI1'
    actions=[idx[16+64*i:80+64*i].split(b'\0')[0].decode() for i in range(na)]
    with (folder/'poses.pak').open('rb') as f:
        f.seek(16)
        skins=[f.read(96)[:64].split(b'\0')[0].decode() for _ in range(ns)]
    ai=skin_motion.constants(member(archive_root(disc)/'x_headers.ras','data/database/ai_ranges.h').decode('latin1'))
    shooting=skin_motion.constants(member(archive_root(disc)/'x_headers.ras','data/database/shooting.h').decode('latin1'))
    common=skin_motion.constants(member(archive_root(disc)/'x_headers.ras','data/database/characters.h').decode('latin1'))
    physical=[]
    weapons=[]
    characters=[]
    strings={'':0};rows=[];groups={};bindings=[]
    def sid(key):return strings.setdefault(key,len(strings))
    with (archive_root(disc)/'x_data.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        files={e.path.lower():e for e in ras.read_directory(data).entries}
        def read(path):return ras.member_bytes(data,files['data/database/'+path+'.txt']).decode('latin1')
        for weapon in ('empty','beretta','berettadual','deserteagle','pumpshotgun','sawedshotgun','jackhammer','ingram','ingramdual','mp5','sniper','baseballbat','leadpipe','grenade','molotov','m79'):
            txt=skin_motion.clean(read('weapons/'+weapon))
            rates=[skin_motion.number(re.search(field+r'\s*=\s*([^;]+);',txt,re.I)[1],shooting) for field in ('MaximumShootingFrequency','DefaultShootingFrequency','ShootingFrequencyMultiplier')]
            weapons.append(struct.pack('<20s3f',weapon.encode(),*rates))
        for skin in sorted(set(skins)|{'rat'}):
            fields=dict(re.findall(r'\b(\w+)\s*=\s*([^;{}]+);',skin_motion.clean(read('skins/'+skin))))
            def value(key):
                raw=fields[key].strip()
                if raw.endswith('%'):return float(raw[:-1])/100
                return skin_motion.number(raw,common)
            aim=fields['AutoAimTarget'].strip().lower() in ('yes','true','1')
            timed=fields['GiveTimedModeBonusForKiller'].strip().lower() in ('yes','true','1')
            physical.append(struct.pack('<64s5fBB2x',skin.encode(),*(value(k) for k in
                ('CapsuleTop','CapsuleBottom','CapsuleRadius','HealthBonusForKiller','SlowMotionBonusForKiller')),aim,timed))
        default=read('skeletons/default_skeleton')
        for skin in skins:
            source=read('skins/'+skin)
            params={**ai,**skin_motion.constants(default),**skin_motion.constants(source)}
            characters.append((skin_motion.skin_row(source,params),
                               {**skin_motion.movements(default,params),**skin_motion.movements(source,params)}))
            constants={k.lower():v.lower() for k,v in re.findall(r'^\s*#define\s+(\w+)\s+([-.\w]+)',default+'\n'+source,re.M)}
            table={**definitions(default,all_slots=True),**definitions(source,all_slots=True)}
            for action in actions:
                name='charanim_'+action;value=table.get(name);visited=set()
                while isinstance(value,str) and value not in visited:
                    visited.add(value);value=table.get(value)
                cues=[]
                if name not in SLOTS and isinstance(value,tuple):
                    for frame,msg in sorted(value[1],key=lambda pair:pair[0]):
                        m=re.fullmatch(r'\s*([^>]+?)->\s*(\w+)\s*\((.*?)\)\s*;?\s*',msg)
                        if not m:raise ValueError((skin,action,msg))
                        target,op,args=m.groups()
                        args=args.replace(r'\"','').lower().strip()
                        args=re.sub(r'"\s*([\w.-]+)\s*"',lambda m:constants.get(m[1],m[1]),args)
                        args=[sid(v.strip()) for v in args.split(',')] if args else []
                        if len(args)>4:raise ValueError(msg)
                        cues.append((frame,sid(target.strip().lower()),sid(op.lower()),len(args),*(args+[0]*(4-len(args)))))
                key=tuple(cues)
                if key not in groups:
                    groups[key]=len(rows);rows.extend(cues)
                bindings.append((groups[key],len(key)))
    dest=folder/'combatants.bin'
    tmp=dest.with_suffix('.partial');tmp.write_bytes(struct.pack('<4sI',b'NPC1',len(physical))+b''.join(physical));tmp.replace(dest)
    skin_motion.write_rules(folder,actions,characters,weapons)
    def records(fmt,items):return b''.join(struct.pack(fmt,*r) for r in items)
    dictionary=b''.join(s.encode()+b'\0' for s in strings)
    payload=struct.pack('<4s5I',b'CUE1',ns,na,len(rows),len(strings),len(dictionary))+records('<2H',bindings)+records('<8H',rows)+dictionary
    dest=folder/'cues.bin';tmp=dest.with_suffix('.partial');tmp.write_bytes(payload);tmp.replace(dest)
    print(len(rows),'frame messages in',len(groups),'distinct sequences for',ns,'skins')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('disc',type=Path);p.add_argument('folder',type=Path)
    a=p.parse_args();pack(a.disc,a.folder)
