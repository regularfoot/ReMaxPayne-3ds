"""Export the face materials and original alternate bitmaps used by character skins."""
from io import BytesIO
import argparse,mmap,re,struct
from pathlib import Path
from PIL import Image
from dade.maxpayne import ras
from dade.maxpayne.model import read_model
from native_texture import texture
from xbox_disc import archive_root

def pack(disc,out):
    dest=out/'faces';dest.mkdir(parents=True,exist_ok=True)
    path='data/database/skins/'
    with (archive_root(disc)/'x_data.ras').open('rb') as file,mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
        entries={e.path.lower():e for e in ras.read_directory(data).entries}
        def read(k):return ras.member_bytes(data,entries[k])
        for name in sorted({'max_payne','young_max_payne'}|{p.stem for p in (out/'cast').glob('*.actor')}):
            conf=read(path+name+'.txt').decode('latin1')
            models=[k[:-4] for k in entries if k.startswith(path+name+'/') and k.endswith('.skd')]
            if not models:
                choices={Path(x.replace('\\','/')).stem.lower() for x in re.findall(r'[^\s;="]+\.kfs',conf,re.I)}
                models=[k[:-4] for k in entries if k.endswith('.skd') and Path(k).stem in choices]
            if not models:continue
            model=models[0];face=None
            for i,(mat,bitmap) in enumerate(read_model(read(model+'.kfs')).materials.items()):
                if 'face' in mat.lower() or 'face' in bitmap.lower():
                    face=i,bitmap.lower();break
            if not face:continue
            slot,bitmap=face;stem=Path(bitmap).stem
            root=re.sub(r'_(default|normal|live)$','',stem)
            images=[];mask=0
            for j,suffixes in enumerate([('_death','_dead'),('_wounded',)]):
                keys=[folder+'/'+root+end+'.dds' for folder in [str(Path(model).parent)+'/textures',path+'sharedtextures'] for end in suffixes]
                key=next((k for k in keys if k in entries),None)
                if key:
                    mask|=1<<j;images.append(texture(Image.open(BytesIO(read(key)))))
            if not mask:continue
            (dest/(name+'.face')).write_bytes(struct.pack('<4sII',b'MPF1',slot,mask)+b''.join(images))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('disc',type=Path);p.add_argument('out',type=Path)
    a=p.parse_args();pack(a.disc,a.out)
