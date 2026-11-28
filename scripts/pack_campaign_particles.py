"""Reduce the campaign's retail RPS sprite sheets to a set of shared PICA texture pages."""
import argparse
import hashlib
import io
import json
import math
import mmap
import re
import struct
from pathlib import Path
from PIL import Image, ImageChops
from dade.maxpayne import ras
from max_payne_sdk.max_type import parseType
from native_texture import texture, _tile, RGBA4
from rps_layers import layout_key, parse_layers, selected_frames, sprite_names
from xbox_disc import archive_root

CELL = 16
PER_PAGE = 64

def motion(name, filename):
    if name.startswith('light_'):return 7
    key = name + ' ' + filename
    for kind, words in enumerate((('snow',), ('flame','fire','torch','burn'), ('smoke','steam','mist','dust','gas_'),
                                   ('water','rain','sprinkl','drip','fluid'), ('explos','fireball','muzzle','blast'),
                                   ('spark','electric','arc_','flash','laser'))):
        if any(word in key for word in words):
            return kind
    return 6

def sample(curve,t,channel=0):
    frame=t*(len(curve)-1);i=int(frame);f=frame-i
    return curve[i][channel]*(1-f)+curve[min(i+1,len(curve)-1)][channel]*f

def police_curve(raw):
    stream=io.BytesIO(raw)
    recent=[]
    while stream.tell()<len(raw):
        recent=(recent+[parseType(stream)])[-4:]
        if recent==[113,3,3,256]:
            return bytes(max(0,min(255,round(parseType(stream)))) for _ in range(3*256))
    raise ValueError('Missing police light RGB curve')

def integrated_rate(layer, t):
    # Integral of the mean of the two authored rate curves. Preserve narrow bursts.
    rate=layer.rate
    if len(rate)==1:return max(0,sum(rate[0])*.5)*t
    key=t*(len(rate)-1);whole=int(key)
    area=sum((sum(rate[i])+sum(rate[i+1]))*.25 for i in range(whole))
    value=sample(rate,t,0)+sample(rate,t,1)
    return (area+(key-whole)*(sum(rate[whole])+value)*.25)/(len(rate)-1)


def pack(disc, output):
    # Port-authored atlas coordinates in the order of the RPS sprite references.
    layouts = json.loads(Path(__file__).with_name('particle_atlas_layout.json').read_text())
    slots={};atlases=[];open_pages={};definitions=[];shape_curves=[];groups=[];layers=[];variants=[];lifetimes=[];drag=[]
    with (archive_root(disc)/'x_data.ras').open('rb') as file, mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as archive:
        entries = {e.path.lower():e for e in ras.read_directory(archive).entries}
        def read(path):
            return ras.member_bytes(archive,entries[path])
        base = 'data/database/particles/'
        pulse=police_curve(read(base+'lights/light_policecar.rps'))
        manifest = read(base+'particles.txt').decode('cp1252')
        for match in re.finditer(r'^\[(\w+)\][^\n]*\n\s*\{(.*?)^\}',manifest,re.M|re.S):
            name,body=match.groups();name=name.lower()
            default=re.search(r'\[Default\].*?filename\s*=\s*([^;]+);',body,re.I|re.S)
            if not default:continue
            key=default[1].strip().replace('\\','/').lower()
            path=base+key
            prefix=base+'combined_images/_'+path[:-4].replace('/','_')
            if path not in entries or prefix+'1.dds' not in entries:continue
            cap,period,components=parse_layers(read(path));sheets={}
            names = sprite_names(components)
            coordinates = dict(zip(names, layouts[layout_key(key, names)], strict=True))
            groups.append(struct.pack('<4Hf',len(layers),len(components),cap,0,period))
            for number,layer in enumerate(components):
                first_variant=len(variants)
                for sequence in layer.sequences:
                    frames=[]
                    for i,(image,mask,w,h) in enumerate(selected_frames(sequence)):
                        page,x,y,flip=coordinates[image]
                        if page not in sheets:
                            sheets[page]=Image.open(io.BytesIO(read(prefix+str(page+1)+'.dds'))).convert('RGBA')
                        source=sheets[page]
                        if (x+1)*w>source.width or (y+1)*h>source.height:raise ValueError(image)
                        cell=source.crop((x*w,y*h,(x+1)*w,(y+1)*h))
                        if flip:cell=cell.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
                        if layer.additive:cell.putalpha(255)
                        # Glass and thin splinters lose their silhouette when
                        # the authored 32-pixel alpha mask is halved.
                        hires=name.startswith('muzzle_') or (mask and image.startswith('debris\\') and min(w,h)>=32)
                        cell_size=32 if hires else CELL
                        cell=cell.resize((cell_size,cell_size),Image.Resampling.LANCZOS)
                        t=(i+.5)/4
                        tint=[sample(layer.curves[3],t,c) for c in range(3)]+[sample(layer.curves[2],t)]
                        color=tuple(max(0,min(255,round(v))) for v in tint)
                        cell=ImageChops.multiply(cell,Image.new('RGBA',cell.size,color))
                        digest=hashlib.sha256(cell.tobytes()).digest()
                        if digest not in slots:
                            if cell_size not in open_pages or len(atlases[open_pages[cell_size]][1])==PER_PAGE:
                                open_pages[cell_size]=len(atlases)
                                atlases.append((cell_size,[]))
                            page=open_pages[cell_size]
                            cells=atlases[page][1]
                            slots[digest]=page*PER_PAGE+len(cells)
                            cells.append(cell)
                        frames.append(slots[digest])
                    variants.append(frames)
                frames=variants[first_variant]
                shape=[max(0,sample(layer.curves[0],i/15)) for i in range(16)]
                peak=max(shape)
                encoded=bytes(round(255*v/peak) if peak else 0 for v in shape)
                controls=layer.controls
                lifetimes.append(struct.pack('<ff',controls[4],controls[5]))
                strength,size_mode,velocity_mode=layer.drag
                if not math.isfinite(strength) or size_mode not in range(3) or velocity_mode not in range(3):raise ValueError((key,layer.drag))
                drag.append(struct.pack('<fBB2x',strength,size_mode,velocity_mode))
                dynamics=[layer.gravity,*controls[1],*controls[2],*controls[16],*controls[17],controls[14],controls[15]]
                for i in range(8):
                    t=i/7;dynamics.append(integrated_rate(layer,t)*period)
                    for curve,mult in ((layer.life,1),(layer.speed,1),(layer.scale,peak*.5),(layer.drift,1)):
                        dynamics.extend(sample(curve,t,c)*mult for c in range(len(curve[0])))
                if not all(map(math.isfinite,dynamics)):raise ValueError(key)
                spin=[math.radians(sample(layer.curves[1],i/15)) for i in range(16)]
                spin_scale=[sample(layer.rotation,i/7,c) for i in range(8) for c in range(2)]
                layers.append(struct.pack('<4H16sBB2x95fHHI34f',*frames,encoded,layer.additive,controls[9],*dynamics,
                                          first_variant,len(layer.sequences),layer.flags,*map(math.radians,layer.angles),*spin,*spin_scale))
                if number==0:
                    loop=bool(re.search(r'Looping\s*=\s*true',body,re.I))
                    size=re.search(r'(?<!\d)(\d+)(?:x\d+|m)(?:_|\b)',name)
                    area=min(16,float(size[1])) if size else 1.5
                    if len(name.encode())>=64:raise ValueError(name)
                    definitions.append(struct.pack('<64s4HBB2xf',name.encode(),*frames,motion(name,path),loop,area))
                    shape_curves.append(encoded)
    pages=[]
    if len(atlases)>64:raise ValueError('Too many particle texture pages')
    for size,cells in atlases:
        page=Image.new('RGBA',(size*8,size*8))
        for i,sprite in enumerate(cells):
            page.paste(sprite,((i%8)*size,(i//8)*size))
        if size==32:
            # New uses the sharper sprite atlas. Old skips its 256 base and
            # retains the original 16-pixel cells, with no extra resident cost.
            low=page.resize((128,128),Image.Resampling.BOX)
            pages.append(struct.pack('<HHBBBB',256,256,RGBA4,1,1,0)+_tile(page,RGBA4)+_tile(low,RGBA4))
        else:pages.append(texture(page,lightmap=True))
    data=struct.pack('<4sII',b'RPS1',len(definitions),len(pages))+b''.join(definitions)+b''.join(pages)+b'PLC1'+pulse+b'RSC1'+b''.join(shape_curves)
    data+=struct.pack('<4sII',b'LAY2',len(layers),len(variants))+b''.join(groups)+b''.join(layers)
    data+=b''.join(struct.pack('<4H',*v) for v in variants)
    data+=b'ELT1'+b''.join(lifetimes)
    data+=b'DRG1'+b''.join(drag)
    tmp=output.with_suffix('.partial');tmp.write_bytes(data);tmp.replace(output)
    print(f'{len(definitions)} particle definitions, {len(layers)} emitters, {len(variants)} sprite sequences, {len(slots)} distinct sprites on {len(pages)} pages')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc',type=Path);parser.add_argument('output',type=Path)
    a=parser.parse_args();pack(a.disc,a.output)
