"""Pack material-specific Xbox decals, including independent opacity maps."""
from io import BytesIO
import re
import struct
from PIL import Image
from native_texture import texture

def pack_decals(read,out):
    base='data/database/decals/'
    text=re.sub(r'//[^\n]*','',read(base+'decals.txt').decode('latin1'))
    images={};pixels=[];records=[];names=[];events=[]
    for name,body in re.findall(r'^\[(\w+)\]([^\n]*\n[\s\S]*?)(?=^\[|\Z)',text,re.M):
        names.append(name.lower());index=len(names)
        messages=re.split(r'\[materials\]',body,flags=re.I)[0]
        for op,raw in re.findall(r'->(\w+)\s*\(([^)]*)\)',messages):
            args=[v.strip().strip('"').lower() for v in raw.split(',')]
            kind={'a_playfloating3dsound':2,'ps_starteffect':1}[op.lower()]
            key='/'.join(args[:2]) if kind==2 else args[0]
            events.append((index,kind,key))
        blocked=re.findall(r'\[NoDecalMaterial\]\s*Name\s*=\s*"([^"]+)"',body,re.I)
        for material in blocked:records.append((index,material.lower(),65535,0.))
        for tag,block in re.findall(r'\[(\w+)\]([^\[]*)',body):
            color=re.search(r'(?im)^\s*Filename\s*=\s*"([^"]+)"',block)
            if not color:continue
            alpha=re.search(r'(?im)^\s*AlphaFilename\s*=\s*"([^"]+)"',block)
            radius=re.search(r'(?im)^\s*MinimumRadius\s*=\s*([\d.]+)',block)
            key=(color[1].lower().replace('\\','/'),alpha[1].lower().replace('\\','/') if alpha else '')
            if key not in images:
                image=Image.open(BytesIO(read(base+key[0]))).convert('RGBA')
                if key[1]:
                    mask=Image.open(BytesIO(read(base+key[1]))).convert('L').resize(image.size)
                    image.putalpha(mask)
                image.thumbnail((64,64));images[key]=len(pixels);pixels.append(texture(image))
            records.append((index,tag.lower(),images[key],float(radius[1]) if radius else .10))
    rows=b''.join(struct.pack('<HHf32s',i,tex,r,mat.encode()) for i,mat,tex,r in records)
    (out/'decals.pak').write_bytes(struct.pack('<4sII',b'DEC1',len(records),len(pixels))+rows+b''.join(pixels))
    return names,events
