"""Read the original typed movement networks and preserve their inter-room node edges."""
from io import BytesIO
from pathlib import Path
import argparse
import mmap
from dade.maxpayne import ras
import struct
from max_payne_sdk.max_type import parseType
from campaign_source import levels, level_archive
from pack_native import member
from xbox_disc import archive_root


def graph(data):
    f=BytesIO(data);a=[]
    while f.tell()<len(data):a.append(parseType(f))
    nodes={};pending=[]
    for i in range(2,len(a)-12):
        v=a[i]
        if not isinstance(v,list) or len(v)!=3 or not isinstance(v[0],float):continue
        if v[0]==0 and v[2]==0 and v[1]<-5 and all(isinstance(a[i-j],float) for j in (1,2,3,4)):
            room=a[i-5]
            if not isinstance(room,int):raise ValueError('Room identifier')
            for id,pos,edges in pending:
                if id in nodes:raise ValueError('Duplicate AI node')
                nodes[id]=(room,pos,edges)
            pending=[];continue
        if not all(type(a[k])==int for k in (i-2,i-1,i+1,i+3,i+5,i+7,i+10,i+11)):continue
        if not all(type(a[k])==float for k in (i+2,i+4,i+6,i+8,i+9)):continue
        n=a[i+11]; extra=a[i+12:i+12+n]
        if not 0<=n<256 or not all(type(j)==int for j in extra):raise ValueError('Malformed movement edge')
        links=list(dict.fromkeys([v for v in (*a[i+1:i+9:2],*extra) if v]))
        pending.append((a[i-2],tuple(v),links))
    if pending: raise ValueError('Missing room metadata')
    return nodes


def pack(disc,out,only=None):
    available={}
    catalog=[cfg for cfg in levels(archive_root(disc)) if not only or cfg['id'] in only]
    for archive in {level_archive(archive_root(disc),stage) for stage in catalog}:
        with archive.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
            available[archive]={entry.path.lower() for entry in ras.read_directory(data).entries}
    for cfg in catalog:
        arc=level_archive(archive_root(disc),cfg)
        path='data/database/levels/'+cfg['directory']+'/'+str(Path(cfg['level']).with_suffix('.ai'))
        nodes=graph(member(arc,path)) if path.lower() in available[arc] else {}
        remap={k:i for i,k in enumerate(nodes)}
        edges=[];rows=[]
        for room,pos,links in nodes.values():
            # The editor leaves indices to discarded nodes in several PAL networks.
            valid=[remap[k] for k in links if k in remap]
            rows.append(struct.pack('<3fHHI',*pos,room,len(valid),len(edges)));edges+=valid
        if len(rows)>65535:raise ValueError(cfg['id'])
        dest=out/(cfg['id']+'.nav')
        dest.write_bytes(struct.pack('<4sII',b'MPN1',len(rows),len(edges))+b''.join(rows)+struct.pack(f'<{len(edges)}H',*edges))
        print(cfg['id'],len(rows),len(edges),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('disc',type=Path);p.add_argument('out',type=Path);p.add_argument('--level',action='append');a=p.parse_args();pack(a.disc,a.out,a.level)
