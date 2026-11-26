"""Compile projectile attributes, timed blast curves and pickup drops from the original data."""
import argparse
import mmap
from pathlib import Path
import re
import struct
from dade.maxpayne import ras
from pack_decals import pack_decals
from pack_campaign_events import FIRST_PICKUP, PICKUP, item_templates


def pack_grant(messages):
    weapons = []
    ammo, sound, caption, amount = '', '', '', 0
    for message in messages:
        match = re.fullmatch(r'([^>]+)->(\w+)\s*\((.*?)\);', message.lower())
        if not match:
            raise ValueError(message)
        target, op, raw = match.groups()
        args = [part.strip(' "') for part in raw.split(',')]
        if target == 'activator' and op == 'c_pickupweapon':
            weapons.append(args[0])
        elif target == 'activator' and op == 'c_pickupammo':
            ammo, amount = args[0], int(args[1])
        elif op == 'a_playsound' and args[0] == 'level_items':
            sound = args[1]
        elif op == 'mphm_print':
            caption = args[0]
        else:
            raise ValueError(message)
    if len(weapons) > 2 or not ammo or not sound or not caption:
        raise ValueError(messages)
    weapons += [''] * (2 - len(weapons))
    values = [*weapons, ammo, sound, caption]
    if any(len(s.encode()) >= n for s,n in zip(values,(16,16,16,32,64))):
        raise ValueError(values)
    return struct.pack('<16s16s16s32s64sI', *(s.encode() for s in values), amount)


def pack(disc,out):
    constants={}
    with (disc/'english/x_headers.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        for entry in ras.read_directory(data).entries:
            if entry.path.lower().endswith('.h'):
                body=ras.member_bytes(data,entry).decode('latin1')
                for name,value in re.findall(r'^\s*#define\s+(\w+)\s+([-+.\d]+)\b',body,re.M):
                    constants[name.lower()]=value
    records=[];bursts=[];drops=[];effects=[];additional=[];pending=[];contacts={}
    items=item_templates(disc/'english')
    with (disc/'english/x_data.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        files={e.path.lower():e for e in ras.read_directory(data).entries}
        death_drops={}
        for path,entry in files.items():
            weapon=re.fullmatch(r'data/database/weapons/(\w+)\.txt',path)
            if not weapon:continue
            body=re.sub(r'//[^\r\n]*','',ras.member_bytes(data,entry).decode('latin1')).lower()
            for block in re.split(r'\[animation\]',body):
                if not re.match(r'\s*index\s*=\s*weaponanim_dropondeath\b',block):continue
                for frame,key,amount in re.findall(r'\[message\]\s*frame\s*=\s*(\d+)\s*;[^\n]*?->p_createprojectile\s*\(\s*(\w+)\s*,\s*(\d+)',block):
                    if int(frame)!=0 or int(amount)!=1 or key in death_drops:raise ValueError((path,frame,key,amount))
                    death_drops[key]=weapon[1]
        decals,impact_events=pack_decals(lambda path:ras.member_bytes(data,files[path]),out.parent)
        for entry in sorted(files.values(),key=lambda e:e.path.lower()):
            match=re.fullmatch(r'data/database/projectiles/(\w+)\.txt',entry.path.lower())
            if not match:continue
            body=re.sub(r'//[^\r\n]*','',ras.member_bytes(data,entry).decode('latin1')).lower()
            props=dict(re.findall(r'^\s*(\w+)\s*=\s*([^;\s]+)\s*;',body,re.M))
            def number(value):
                return float(constants.get(value,value))
            def prop(key,default='0'):
                return number(props.get(key,default))
            def flag(key):
                return int(props.get(key) in ('true','yes','1'))
            first=len(bursts)
            for frame,args in re.findall(r'\[message\]\s*frame\s*=\s*(\d+)\s*;[^\n]*?->p_explosion\s*\(([^)]*)\)',body):
                values=[number(v.strip()) for v in args.split(',')[:4]]
                bursts.append(struct.pack('<5f',int(frame)/30,*values))
            drop=re.search(r'->li_createlevelitem\s*\(\s*(\w+)',body)
            old=match[1].startswith(('bullet_','damage_')) or len(bursts)>first or drop or match[1] in ('grenade','molotov')
            if drop:
                _,events=items[drop[1]]
                drops.append(struct.pack('<I',len(records)) + pack_grant(events[FIRST_PICKUP]) + pack_grant(events[PICKUP]))
            row=struct.pack('<48s32s4f4H',match[1].encode(),drop[1].encode() if drop else b'',
                prop('speed'),prop('gravitymultiplier'),prop('explosiondelay'),prop('damage'),
                first,len(bursts)-first,flag('collisionexplosion'),flag('delayedexplosion'))
            direction=re.search(r'(?m)^\s*direction\s*=\s*\(([^)]+)',body)
            xyz=[float(v) for v in direction[1].split(',')] if direction else [0,0,1]
            decal=re.search(r'\[decal\][^\[]*?->d_createdecal\s*\(\s*(\w+)',body,re.S)
            particle=re.search(r'->ps_starteffect\s*\(\s*(\w+)',body)
            ricochet=re.search(r'projectileanim_ricochet[^\[]*(.*?)(?=\[animation\]|\[attributes\]|\Z)',body,re.S)
            sound=re.search(r'->a_play3dsound\s*\(\s*(\w+)\s*,\s*(\w+)',ricochet[1]) if ricochet else None
            if not sound:sound=re.search(r'->a_play3dsound\s*\(\s*(\w+)\s*,\s*(\w+)',body)
            mesh=match[1] if match[1] in ('debris_blue_sign','debris_wood_crate','wood_crate','case_9mm') else 'case_12ga' if match[1]=='case_12ga_sawed' else ''
            if drop:
                # The airborne object uses the weapon model. Dual pickups become
                # the two-gun inventory item only after they touch the floor.
                model=re.search(r'\bexportdata\s*=\s*([^;]+)',body)[1].strip().replace('\\','/')
                mesh=model.split('/')[-2]
                if mesh=='coltcommando':mesh='mp5'
            death_weapon=death_drops.pop(match[1],None)
            if death_weapon and (not drop or drop[1]!=death_weapon):raise ValueError((match[1],death_weapon))
            contacts[match[1]]=struct.pack('<f4B',prop('ricochetminimumspeed'),flag('ricochetuntilonground'),flag('damagescharacter'),bool(death_weapon),flag('smallprojectile'))
            fx=struct.pack('<3f3fHH48s40s32s',*xyz,prop('accuracy'),float(props.get('speedrandom','0').strip('%'))/100,
                prop('ricochetspeedmultiplier'),decals.index(decal[1])+1 if decal else 0,flag('debris'),
                particle[1].encode() if particle else b'',('/'.join(sound.groups())).encode() if sound else b'',mesh.encode())
            phases={'default':0,'ricochet':1,'frozen':2,'explode':3}
            ops={'ps_starteffect':1,'a_play3dsound':2,'a_playfloating3dsound':2,'p_createprojectile':3,'p_alertenemies':4,'d_createdecal':5}
            for block in re.split(r'\[animation\]',body)[1:]:
                phase=re.match(r'\s*index\s*=\s*projectileanim_(\w+)',block)
                if not phase or phase[1] not in phases:continue
                segment=re.split(r'\[(?:decal|characterhit|attributes|camera)\]',block,maxsplit=1)[0]
                for cue in re.finditer(r'\[message\]\s*frame\s*=\s*(\d+)\s*;\s*string\s*=\s*"[^>]+->(\w+)\s*\(([^)]*)\)',segment):
                    frame,op,args=cue.groups()
                    if op not in ops:continue
                    argv=[v.strip(' \\"') for v in args.split(',')]
                    key='/'.join(argv[:2]) if ops[op]==2 else argv[0]
                    amount=1
                    if op=='p_createprojectile':amount=int(argv[1])
                    if op=='p_alertenemies':amount=round(100*float(re.match(r'\d+(?:\.\d+)?',key)[0]));key=''
                    if op=='d_createdecal':amount=decals.index(key)+1;key=''
                    pending.append((match[1],phases[phase[1]],int(frame),ops[op],amount,key))
            hit=re.search(r'\[characterhit\][^\[]*?p_createprojectile\s*\(\s*(\w+)\s*,\s*(\d+)',body)
            if hit:pending.append((match[1],4,0,3,int(hit[2]),hit[1]))
            if old:records.append(row);effects.append(fx)
            else:additional.append((row,fx))
    if death_drops:raise ValueError(('Missing weapon death projectiles',death_drops))
    for row,fx in additional:records.append(row);effects.append(fx)
    ids={row[:48].split(b'\0')[0].decode():i for i,row in enumerate(records)}
    cues=sorted([(ids[name],phase,frame,op,amount,key) for name,phase,frame,op,amount,key in pending],key=lambda r:r[:3])
    temp=out.with_suffix('.partial')
    with temp.open('wb') as f:
        f.write(struct.pack('<4sII',b'MPB1',len(records),len(bursts)));f.writelines(records);f.writelines(bursts)
        f.write(struct.pack('<4sI',b'DRO1',len(drops)));f.writelines(drops)
        f.write(struct.pack('<4sI',b'VFX1',len(effects)));f.writelines(effects)
        f.write(struct.pack('<4sI',b'PHX1',len(cues)))
        for *values,key in cues:
            if len(key.encode())>=64:raise ValueError(key)
            f.write(struct.pack('<5H64s',*values,key.encode()))
        f.write(struct.pack('<4sI',b'DFX1',len(impact_events)))
        for decal,op,key in impact_events:
            if len(key.encode())>=64:raise ValueError(key)
            f.write(struct.pack('<HH64s',decal,op,key.encode()))
        f.write(struct.pack('<4sI',b'PHY2',len(records)))
        f.writelines(contacts[name] for name in ids)
    temp.replace(out)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc',type=Path);parser.add_argument('out',type=Path)
    args=parser.parse_args();pack(args.disc,args.out)
