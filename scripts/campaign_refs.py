"""Read asset references from the same binary event tables consumed by the game."""
from collections import defaultdict
import struct
import math

from native_texture import TEXEL_BYTES

SOUNDS = {'a_play3dsound','a_playsound','a_playfloating3dsound'}
NATIVE_SOUNDS = {'menu/timeout','menu/slomo_heartbeat','projectiles/casehit_generic','level_items/pickup_ammo_full',
                 'characters/rat_idle','characters/rat_walk','characters/rat_run','characters/rat_death'}

def commands(path):
    data=path.read_bytes()
    tag,entities,events,clips,count,nstrings,nbytes=struct.unpack_from('<4s6I',data)
    if tag!=b'EVT1':raise ValueError(path)
    first=28+48*entities+12*events+104*clips
    offsets=first+20*count
    at=offsets+4*nstrings
    if data[at+nbytes:at+nbytes+4]!=b'XFR1':raise ValueError(path)
    strings=[data[at+i:data.index(0,at+i,at+nbytes)].decode() for i, in struct.iter_unpack('<I',data[offsets:at])]
    for _,op,n,*args in struct.iter_unpack('<10H',data[first:offsets]):
        yield strings[op], [strings[i] for i in args[:n]]

def resource(op,args):
    if op in SOUNDS:return 'audio','/'.join(args[:2])
    if op=='a_playmusic' and args and args[0] not in ('','none'):return 'audio','music/'+args[0]
    if op in ('p_createprojectile','p_createprojectiletobone'):return 'projectile',args[0]
    if op=='ps_starteffect':return 'particle',args[0]
    if op in ('cam_animateparented','cam_animateinplace','cam_animateplayerparented'):return 'camera',args[0]

def references(folder):
    result=defaultdict(set)
    for evt in sorted(folder.glob('*.evt')):
        for op,args in commands(evt):
            item=resource(op,args)
            if item:result[item].add(evt.stem)
    for key in NATIVE_SOUNDS:result['audio',key].add('native')
    return result

def texture_end(data,at):
    w,h,fmt,mips,clamp,res=struct.unpack_from('<2H4B',data,at)
    sizes=(8,16,32,64,128,256,512,1024)
    if w not in sizes or h not in sizes or fmt not in TEXEL_BYTES or clamp>1 or res or mips>min(w,h).bit_length()-4:
        raise ValueError('Invalid 3DS texture header')
    if mips<max(w,h).bit_length()-8:raise ValueError('Missing Old-3DS texture fallback')
    return at+8+sum((w>>i)*(h>>i)*TEXEL_BYTES[fmt] for i in range(mips+1))

def check_visual_banks(folder):
    data=(folder/'decals.pak').read_bytes()
    magic,rows,textures=struct.unpack_from('<4sII',data)
    at=12+40*rows
    if magic!=b'DEC1' or not textures or not rows:raise ValueError('Missing decal resources')
    for decal,image,radius,material in struct.iter_unpack('<HHf32s',data[12:at]):
        if not decal or (image!=65535 and image>=textures) or radius<0 or b'\0' not in material:raise ValueError('Invalid decal material')
    for _ in range(textures):at=texture_end(data,at)
    if at!=len(data):raise ValueError('Invalid decal textures')
    data=(folder/'particles.pak').read_bytes()
    tag,defs,pages=struct.unpack_from('<4sII',data)
    if tag!=b'RPS1' or not 0<pages<=64:raise ValueError('Invalid particle atlas')
    at=12+80*defs
    particle_names=[]
    for name,*fields in struct.iter_unpack('<64s4HBB2xf',data[12:at]):
        particle_names.append(name.split(b"\0",1)[0].decode())
        if max(fields[:4])>=64*pages:raise ValueError('Missing particle sprite')
    for _ in range(pages):at=texture_end(data,at)
    if data[at:at+4]!=b'PLC1' or data[at+772:at+776]!=b'RSC1':
        raise ValueError('Missing particle curves')
    at+=776+defs*16
    tag,count,variants=struct.unpack_from('<4sII',data,at)
    at+=12
    groups=list(struct.iter_unpack('<4Hf',data[at:at+12*defs]))
    at+=12*defs
    layers=list(struct.iter_unpack('<4H16sBB2x95fHHI34f',data[at:at+count*552]))
    at+=count*552
    animations=list(struct.iter_unpack('<4H',data[at:at+variants*8]))
    at+=variants*8
    if data[at:at+4]!=b'ELT1':raise ValueError('Missing emitter lifetimes')
    lifetimes=list(struct.iter_unpack('<ff',data[at+4:at+4+count*8]));at+=4+count*8
    if len(lifetimes)!=count:raise ValueError('Incomplete emitter lifetimes')
    if data[at:at+4]!=b'DRG1' or len(data)!=at+4+count*8:raise ValueError('Missing original particle resistance')
    resistance=list(struct.iter_unpack('<fBBH',data[at+4:]))
    if any(not math.isfinite(k) or size>2 or velocity>2 or reserved for k,size,velocity,reserved in resistance):
        raise ValueError('Invalid particle resistance')
    if any(not math.isfinite(m+s) or m<=0 or s<0 for m,s in lifetimes):raise ValueError('Invalid emitter lifetime')
    first,n,*_=groups[particle_names.index('bullethit_generic')]
    if any(abs(m-.10)>.00001 or s for m,s in lifetimes[first:first+n]):raise ValueError('Incorrect bullet impact emission window')
    if not variants or variants!=len(animations) or any(max(v)>=pages*64 for v in animations):
        raise ValueError('Invalid particle material sequence')
    if tag!=b'LAY2' or not 0<count<=4096 or len(layers)!=count:raise ValueError('Invalid emitter bank')
    cursor=0
    for name,(first,n,cap,res,period) in zip(particle_names,groups):
        if first!=cursor or not n or first+n>count or not cap or res or not math.isfinite(period) or period<.001:
            raise ValueError('Invalid emitter group '+name)
        cursor+=n
    if cursor!=count:raise ValueError('Orphan particle emitters')
    for row in layers:
        integral=row[22:102:10]
        first,n=row[102:104]
        if not n or first+n>variants or tuple(row[:4])!=animations[first]:raise ValueError('Invalid emitter sequence range')
        if (max(row[:4])>=64*pages or max(row[5:7])>1 or not all(map(math.isfinite,row[7:])) or
            integral[0]!=0 or list(integral)!=sorted(integral) or integral[-1]>65535):
            raise ValueError('Invalid emitter material or timing')
    for name,n in [('fire_fireplace',2),('blood_medium',2),('cable_sparks',3),('explosion_grenade',8)]:
        first,actual,*_=groups[particle_names.index(name)]
        if actual!=n or len(set(layers[first:first+n]))!=n:
            raise ValueError('Missing distinct retail emitters in '+name)
    first,_,*_=groups[particle_names.index('fire_fireplace')]
    start,variants_in_layer=layers[first][102:104]
    if variants_in_layer!=6 or len(set(animations[start:start+variants_in_layer]))!=6:
        raise ValueError('Missing fireplace flame sequences')
    for path in folder.glob('*.dlg'):
        data=path.read_bytes()
        with path.with_suffix('.mpc').open('rb') as f:_,rooms=struct.unpack('<4sI',f.read(8))
        surface=path.with_suffix('.surf')
        with surface.open('rb') as f:
            if struct.unpack('<4sI',f.read(8))!=(b'SRF2',rooms):
                raise ValueError(f'{surface}: rebuild projectile and character collision channels')
            f.seek(64*rooms,1)
            offsets=struct.unpack(f'<{rooms+1}I',f.read(4*(rooms+1)))
            if offsets[0]!=f.tell() or offsets[-1]!=surface.stat().st_size:raise ValueError(surface)
            for start,end in zip(offsets,offsets[1:]):
                f.seek(start)
                faces,=struct.unpack('<I',f.read(4))
                if end-start!=4+faces+24*((faces+15)//16):raise ValueError(surface)
        tag,count=struct.unpack_from('<4sI',data)
        if tag!=b'DLG1' or len(data)!=8+52*count:raise ValueError(path)
        for room,*values in struct.iter_unpack('<I12f',data[8:]):
            if room>=rooms or not all(map(math.isfinite,values)):raise ValueError(path)

def validate(folder):
    check_visual_banks(folder)
    refs=references(folder)
    for frame,op,args in frame_commands((folder/'cues.bin').read_bytes()):
        item=resource(op,args)
        if item:refs[item].add('animation frame '+str(frame))
    for item,name in projectile_refs(folder):refs[item].add('projectile '+name)
    for kind,name,tag,start,stride,width in (
        ('audio','audio.bank',b'MPA1',8,96,80),('projectile','projectiles.bin',b'MPB1',12,104,48),
        ('particle','particles.pak',b'RPS1',12,80,64),('camera','cameras.pak',b'MPCV',12,48,32)):
        with (folder/name).open('rb') as f:
            magic,count=struct.unpack('<4sI',f.read(8))
            if magic!=tag:raise ValueError(name)
            f.seek(start)
            keys=set()
            for _ in range(count):
                row=f.read(stride)
                if len(row)!=stride:raise ValueError('Short asset index in '+name)
                keys.add(row[:width].split(b'\0',1)[0].decode())
        missing=[key+' ('+', '.join(sorted(owners)[:3])+')' for (k,key),owners in refs.items() if k==kind and key not in keys]
        if missing:raise ValueError(name+' missing '+', '.join(sorted(missing)))

def frame_commands(data):
    tag,skins,actions,count,nstrings,nbytes=struct.unpack_from('<4s5I',data)
    first=24+4*skins*actions;end=first+16*count
    if tag!=b'CUE1' or len(data)!=end+nbytes:raise ValueError('Invalid cues.bin')
    strings=[v.decode() for v in data[end:].split(b'\0')[:-1]]
    if len(strings)!=nstrings:raise ValueError('Invalid cue dictionary')
    for base,size in struct.iter_unpack('<2H',data[24:first]):
        if base+size>count:raise ValueError('Invalid character event range')
        frames=[struct.unpack_from('<H',data,first+16*i)[0] for i in range(base,base+size)]
        if frames!=sorted(frames):raise ValueError('Unordered character events')
    for frame,target,op,n,*args in struct.iter_unpack('<8H',data[first:end]):
        if n>4 or max(target,op,*args)>=nstrings:raise ValueError('Invalid character event')
        yield frame,strings[op],[strings[v] for v in args[:n]]

def projectile_refs(folder):
    data=(folder/'projectiles.bin').read_bytes()
    _,count,bursts=struct.unpack_from('<4sII',data)
    end=12+104*count+20*bursts
    tag,drops=struct.unpack_from('<4sI',data,end)
    if tag!=b'DRO1':raise ValueError('Missing projectile drops')
    end+=8+drops*(4+2*148)
    tag,rows=struct.unpack_from('<4sI',data,end)
    if tag!=b'VFX1' or rows!=count or len(data)<end+8+148*rows:raise ValueError('Missing projectile effects')
    phase=end+8+148*rows
    for i,row in enumerate(struct.iter_unpack('<6f2H48s40s32s',data[end+8:phase])):
        name=data[12+104*i:60+104*i].split(b'\0')[0].decode()
        effect,sound,mesh=(v.split(b'\0')[0].decode() for v in row[-3:])
        if effect:yield ('particle',effect),name
        if sound:yield ('audio',sound),name
        drop=data[60+104*i:92+104*i].split(b'\0')[0].decode()
        category=('items' if mesh=='medipack_small' else 'weapons') if drop else ('cases' if mesh.startswith('case_') else 'debris')
        if drop and (not mesh or not (folder/'items'/(drop+'.mesh')).is_file()):
            raise ValueError('Missing pickup or airborne model for '+name)
        if mesh and not (folder/category/(mesh+'.mesh')).is_file():
            raise ValueError('Missing mesh for '+name)
    tag,count=struct.unpack_from('<4sI',data,phase)
    if tag!=b'PHX1' or len(data)<phase+8+74*count:raise ValueError('Missing projectile frame events')
    for index,stage,frame,op,amount,raw in struct.iter_unpack('<5H64s',data[phase+8:phase+8+74*count]):
        if index>=rows or stage>4 or op not in (1,2,3,4,5):raise ValueError('Invalid projectile cue')
        if op<=3:
            name=data[12+104*index:60+104*index].split(b'\0')[0].decode()
            yield (('particle','audio','projectile')[op-1],raw.split(b'\0')[0].decode()),name
    at=phase+8+74*count
    tag,count=struct.unpack_from('<4sI',data,at)
    end=at+8+68*count
    if tag!=b'DFX1' or len(data)<end:raise ValueError('Missing decal events')
    for decal,op,key in struct.iter_unpack('<HH64s',data[at+8:end]):
        if not decal or op not in (1,2):raise ValueError('Invalid decal event')
        yield (('particle','audio')[op-1],key.split(b'\0')[0].decode()),'decal '+str(decal)
    tag,count=struct.unpack_from('<4sI',data,end)
    if tag!=b'PHY2' or count!=rows or len(data)!=end+8+8*rows:raise ValueError('Missing projectile size and contact rules')
    for i,(speed,ground_only,characters,on_death,small) in enumerate(struct.iter_unpack('<f4B',data[end+8:])):
        if not math.isfinite(speed) or speed<0 or max(ground_only,characters,on_death,small)>1 or (on_death and not data[60+104*i]):
            raise ValueError('Invalid projectile contact rule')
