"""Locate named particle frames in the original Xbox DDS sheets using the mobile material names.

Only sheet coordinates are saved. Running the Xbox asset packer uses the checked-in
index and needs no mobile installation.
"""
import argparse
import io
import json
import mmap
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image
from dade.maxpayne import ras
from max_payne_sdk.max_type import parseType
from rps_layers import parse_layers, selected_frames

BASE='data/database/particles/'


def mobile_materials(obb):
    with zipfile.ZipFile(obb) as bundle:
        archive=bundle.read('x_data.ras')
    images={}
    for entry in ras.read_directory(archive).entries:
        if not entry.path.lower().endswith('.rps'):continue
        data=ras.member_bytes(archive,entry)
        stream=io.BytesIO(data)
        while stream.tell()<len(data):
            name=parseType(stream)
            if isinstance(name,str):
                size=parseType(stream)
                images.setdefault(name.lower(),stream.read(size))
    return images


def signature(image,additive):
    image=image.convert('RGBA')
    if additive:image.putalpha(255)
    else:
        alpha=image.getchannel('A')
        premultiplied=Image.new('RGB',image.size)
        premultiplied.paste(image,mask=alpha)
        premultiplied.putalpha(alpha)
        image=premultiplied
    return np.asarray(image.resize((12,12),Image.Resampling.BOX),dtype=np.float32)


def candidates(pages,w,h,additive):
    coordinates=[];pixels=[]
    for page,image in enumerate(pages):
        for y in range(0,image.height-h+1,h):
            for x in range(0,image.width-w+1,w):
                tile=signature(image.crop((x,y,x+w,y+h)),additive)
                for flip,sample in enumerate((tile,tile[::-1])):
                    coordinates.append((page,x//w,y//h,flip));pixels.append(sample)
    return coordinates,np.array(pixels)


def build(disc,obb,output):
    images=mobile_materials(obb)
    result={}
    with (disc/'english/x_data.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as archive:
        files={e.path.lower():e for e in ras.read_directory(archive).entries}
        for path,entry in files.items():
            if not path.endswith('.rps'):continue
            prefix=BASE+'combined_images/_'+path[:-4].replace('/','_')
            pages=[]
            while prefix+str(len(pages)+1)+'.dds' in files:
                page=files[prefix+str(len(pages)+1)+'.dds']
                pages.append(Image.open(io.BytesIO(ras.member_bytes(archive,page))).convert('RGBA'))
            if not pages:continue
            _,_,layers=parse_layers(ras.member_bytes(archive,entry))
            frames={};grids={}
            for layer in layers:
                for seq in layer.sequences:
                    for rgb,mask,w,h in selected_frames(seq):
                        if rgb in frames:continue
                        ref=Image.open(io.BytesIO(images[rgb])).convert('RGBA')
                        if mask:ref.putalpha(Image.open(io.BytesIO(images[mask])).convert('L').resize(ref.size))
                        grid=(w,h,layer.additive)
                        if grid not in grids:grids[grid]=candidates(pages,*grid)
                        coordinates,pixels=grids[grid]
                        distance=((pixels-signature(ref,layer.additive))**2).mean(axis=(1,2,3))
                        frames[rgb]=coordinates[int(distance.argmin())]
            result[path.removeprefix(BASE)]=frames
    rows=[json.dumps(k)+':'+json.dumps(v,sort_keys=True,separators=(',',':')) for k,v in sorted(result.items())]
    output.write_text('{\n'+',\n'.join(rows)+'\n}\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc',type=Path)
    parser.add_argument('obb',type=Path)
    parser.add_argument('output',type=Path)
    a=parser.parse_args();build(a.disc,a.obb,a.output)
