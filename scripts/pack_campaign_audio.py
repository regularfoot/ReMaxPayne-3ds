"""Build a seekable mono NDSP bank using the original sound group and music definitions."""
from pathlib import Path
import argparse
import gzip
import mmap
import re
import struct
import subprocess
from tempfile import TemporaryDirectory
from dade.maxpayne import ras
from pack_native import member
from campaign_refs import references
from xbox_disc import archive_root


def sections(script):
    body=re.sub(r'//[^\r\n]*|/\*.*?\*/','',script,flags=re.S)
    tags=list(re.finditer(r'^\[([^\]]+)\]',body,re.M))
    for i,tag in enumerate(tags):
        yield tag[1].lower(),body[tag.end():tags[i+1].start() if i+1<len(tags) else len(body)]


def pack(disc,source,out,reference_root=None):
    arcs=archive_root(disc);root=arcs/'data/database/sounds';music=disc/'data/database/music'
    waves={p.relative_to(root).as_posix().casefold():p for p in root.rglob('*') if p.suffix.lower()=='.wav'}
    tracks={p.relative_to(music).as_posix().casefold():p for p in music.rglob('*') if p.suffix.lower()=='.wav'}
    keys={key for kind,key in references(reference_root or out.parent) if kind=='audio'}
    for path in source.glob('*.gz'):
        s=gzip.open(path,'rt').read().replace('\\"','')
        for group,name in re.findall(r'->a_play(?:floating)?(?:3d)?sound\(\s*([^,()]+),\s*([^,()]+)',s,re.I):
            keys.add(group.strip().lower()+'/'+name.strip().lower())
    defs={}
    def add(key,body,paths,constants=None):
        filenames=re.findall(r'(?im)\bfilename\s*=\s*"?([^;"\r\n]+)',body)
        gain=re.search(r'(?i)\bvolume\s*=\s*([\w.]+)',body)
        value=(constants or {}).get(gain[1],gain[1]) if gain else '1'
        looping=bool(re.search(r'(?i)\blooping\s*=\s*(yes|true|1)',body))
        spatial=bool(re.search(r'(?i)\b3dsound\s*=\s*(yes|true|1)',body))
        audio=[paths[p] for f in filenames if (p:=f.strip().replace(chr(92),'/').lower()) in paths]
        for i,wav in enumerate(dict.fromkeys(audio)):
            defs[key,i]=(wav,float(value) if re.fullmatch(r'[\d.]+',value) else 1.0,int(looping)|int(spatial)<<1)
    with (arcs/'x_data.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        for entry in ras.read_directory(data).entries:
            path=Path(entry.path.lower())
            if str(path.parent)!='data/database/sounds' or path.suffix!='.txt':continue
            script=ras.member_bytes(data,entry).decode('latin1')
            constants=dict(re.findall(r'(?m)^\s*#define\s+(\w+)\s+([\d.]+)',script))
            for name,body in sections(script):
                key=path.stem+'/'+name
                if key in keys or path.stem in ('weapons','characters','enemy','debris','projectiles','level_items','camera'):
                    groups=re.split(r'(?im)^\s*\[Material\]',body)
                    add(key,groups[0],waves,constants)
                    for group in groups[1:]:
                        material=re.search(r'(?im)^\s*Name\s*=\s*(\w+)',group)
                        if material:add(key+'/'+material[1].lower(),group,waves,constants)
    for name,body in sections(member(arcs/'x_music.ras','data/database/music/music.txt').decode('latin1')):
        add('music/'+name,body,tracks)
    temp=out.with_suffix('.partial');out.parent.mkdir(exist_ok=True,parents=True)
    with TemporaryDirectory() as scratch,temp.open('wb') as bank:
        table=[];offsets={}
        bank.write(struct.pack('<4sI',b'MPA1',len(defs)));bank.seek(8+96*len(defs))
        sample=Path(scratch)/'sample.pcm'
        for (key,variant),(wave,gain,flags) in sorted(defs.items()):
            if len(key.encode())>=80:raise ValueError(key)
            if wave not in offsets:
                subprocess.run(['ffmpeg','-nostdin','-v','error','-y','-i',wave,'-ac','1','-ar','22050','-f','s16le',sample],check=True)
                data=sample.read_bytes();offsets[wave]=(bank.tell(),len(data));bank.write(data)
            table.append(struct.pack('<80sIIfI',key.encode(),*offsets[wave],gain,flags))
        bank.seek(8);bank.writelines(table)
    temp.replace(out)
    if out.name=='audio.bank':(out.parent/'voice/languages.json').unlink(missing_ok=True)
    print(f'{len(defs)} variants, {len(offsets)} source clips, {out.stat().st_size} bytes',flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for a in ('disc','source','out'):p.add_argument(a,type=Path)
    a=p.parse_args();pack(a.disc,a.source,a.out)
