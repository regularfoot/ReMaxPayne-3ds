"""Convert graphic-novel pages and scripted sounds referenced by local campaign data."""
import argparse
import json
import mmap
from pathlib import Path
import re
import subprocess
import struct
from dade.maxpayne import ras
from pack_native import write_screen
from pack_readable_comics import pack as readable_comics, pack_captions, pack_reading_page, reading_layouts
from xbox_disc import archive_root, language


def stage_info(source,out,english):
    parts=('I','II','III')
    rows=[]
    for level in json.loads((source/'levels.json').read_text()):
        match=re.fullmatch(r'LNID_PART([123])_(PROLOGUE|CHAPTER(\d+))',level['levelname'],re.I)
        if match:
            number=parts[int(match[1])-1]
            if english:
                chapter=f'Chapter {match[3]}' if match[3] else 'Prologue'
                title=f'Part {number} / {chapter}'
            else:
                chapter=f'Глава {match[3]}' if match[3] else 'Пролог'
                title=f'Часть {number}   /   {chapter}'
        elif english:
            title={'startup_level':'Opening','tutorial':'Tutorial','end_combat':'Last Challenge'}[level['id']]
        else:
            title={'startup_level':'Вступление','tutorial':'Обучение','end_combat':'Последняя схватка'}[level['id']]
        color=[int(n) for n in re.findall(r'\d+',level.get('fogcolor','0 0 0'))]
        fog=(color[0]<<16)|(color[1]<<8)|color[2]
        rows.append(struct.pack('<32s64s2I2f',level['id'].encode(),title.encode(),
            level.get('fogging','false').lower()=='true',fog,float(level.get('fogstart',5)),float(level.get('fogend',130))))
    (out/'stages.idx').write_bytes(struct.pack('<4sI',b'MPSI',len(rows))+b''.join(rows))


def loading_screens(disc,source,out):
    dest=out/'loading';dest.mkdir(parents=True,exist_ok=True)
    with (archive_root(disc)/'x_english.ras').open('rb') as file,mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
        entries={e.path.lower():e for e in ras.read_directory(data).entries}
        for level in json.loads((source/'levels.json').read_text()):
            name=Path(level['loadingscreen'].replace(chr(92),'/')).name.lower()
            key='data/database/levels/autosave/'+name
            write_screen(ras.member_bytes(data,entries[key]),dest/(level['id']+'.565'),comic=True,preserve_aspect=name=='p0l0.dds')


def pack(disc,source,out):
    locale=language(disc)
    english=locale=='en'
    stage_info(source,out,english)
    loading_screens(disc,source,out)
    archives=archive_root(disc)
    pages=out/'pages'
    pages.mkdir(parents=True,exist_ok=True)
    zoom=out/'pages-zoom'
    zoom.mkdir(exist_ok=True)
    reading=out/'pages-reading'
    layouts={} if english else reading_layouts()
    waves={p.relative_to(archives/'data/database/sounds').as_posix().casefold():p
           for p in (archives/'data/database/sounds').rglob('*') if p.suffix.casefold()=='.wav'}
    def convert(key,dest):
        tmp=dest.with_suffix('.partial')
        subprocess.run(['ffmpeg','-nostdin','-loglevel','error','-y','-i',waves[key],'-ac','1','-ar','22050','-f','s16le',tmp],check=True)
        tmp.replace(dest)
    with (archives/'x_english.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        for entry in ras.read_directory(data).entries:
            path=Path(entry.path.lower())
            if 'graphicnovelpages' not in path.parts or path.suffix!='.dds':continue
            dest=pages/(path.stem+'.565')
            image=ras.member_bytes(data,entry)
            tmp=dest.with_suffix('.partial');write_screen(image,tmp,comic=True);tmp.replace(dest)
            if path.stem!='black':
                large=zoom/dest.name
                tmp=large.with_suffix('.partial');write_screen(image,tmp,comic=True,size=(800,480));tmp.replace(large)
            if not pack_reading_page(path.stem,image,reading,layouts):
                (reading/dest.name).unlink(missing_ok=True)
            key=f'story/{path.parts[-3]}/{path.stem}.wav'
            narration=dest.with_suffix('.pcm')
            if key in waves:convert(key,narration)
            else:narration.unlink(missing_ok=True)
    table=(out/'objectives')
    table.mkdir(exist_ok=True)
    with (archives/'x_english.ras').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        entry=next(e for e in ras.read_directory(data).entries if e.path.lower()=='data/database/stringtable.txt')
        strings=ras.member_bytes(data,entry).decode('cp1251')
    local={}
    if not english:
        local=dict(line.split("\t",1) for line in (Path(__file__).parent/"locale/objectives.ru.tsv").read_text().splitlines())
    for key,body in re.findall(r'^\[(\w+)\]\s*String\s*=\s*"([^"]*)"',strings,re.M|re.I):
        (table/(key.lower()+'.txt')).write_text(local.get(key.lower(),body)+'\n',encoding='utf-8')
    from pack_campaign_text import pack as pack_text
    pack_text(out)
    print(f'{len(list(pages.glob("*.565")))} pages, {len(list(pages.glob("*.pcm")))} narrations')
    if english:
        for folder,extension in (('pages-readable','565'),('pages-captions','txt')):
            for path in (out/folder).glob('*.'+extension):
                path.unlink()
    else:
        readable_comics(Path(__file__).resolve().parents[1]/'assets/comics/prologue',out)
        pack_captions(out)
    (out/'language.txt').write_text(locale+'\n')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for a in ('disc','source','output'):p.add_argument(a,type=Path)
    args=p.parse_args()
    pack(args.disc,args.source,args.output)
