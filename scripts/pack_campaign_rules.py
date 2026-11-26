"""Compile PAL difficulty profiles and chapter-specific New York Minute clocks."""
import argparse
from pathlib import Path
import re
import struct
from pack_native import member
from xbox_disc import archive_root

MODES = ('normal', 'nightmare', 'hell', 'timedmode')
FIELDS = ('EnemyHealthMultiplier', 'AmmoFoundMultiplier', 'PlayerHealthMultiplier',
          'AutoAimCapsuleRadiusMultiplier', 'AutoAimCapsuleHeightMultiplier',
          'HealthRegenerateMultiplier', 'HealthGainedImmediately', 'BulletTimeGainedPerKillMultiplier')

def fields(text):
    return dict(re.findall(r'^\s*(\w+)\s*=\s*([^;\s]+)', text, re.M))

def scalar(value):
    return float(value.rstrip('%')) / (100 if value.endswith('%') else 1)

def pack(disc, out):
    archive = archive_root(disc)/'x_data.ras'
    def read(name):
        return re.sub(r'//[^\r\n]*', '', member(archive,name).decode('latin1'))
    difficulty = read('data/difficulty.txt')
    def sections(text, names):
        return re.split(r'\[('+'|'.join(names)+r')\]',text,flags=re.I)[1:]
    profiles = []
    entries = sections(difficulty, MODES)
    for name in MODES:
        body = dict(zip(entries[::2],entries[1::2]))[name]
        props = fields(body)
        profile = struct.pack('<If',int(props['SaveGameLimit']),scalar(props['HealthGainedFromPainkiller']))
        blocks = sections(body,('minimum','default','maximum'))
        by_name = dict(zip(blocks[::2],blocks[1::2]))
        for tier in ('minimum','default','maximum'):
            v = fields(by_name[tier])
            profile += struct.pack('<8f',*(scalar(v[key]) for key in FIELDS))
        profiles.append(profile)
    levels = read('data/database/levels/levels.txt')
    defines = dict(re.findall(r'#define\s+(\w+)\s+(-?\d+)',levels))
    index = {p.stem for p in out.glob('*.mpc')}
    rows = []
    for name,body in re.findall(r'^\[([\w-]+)\](.*?)(?=^\[|\Z)',levels,re.M|re.S):
        if name.lower() not in index:continue
        v=fields(body)
        keys=('PlayTime','EnemyTimeBonus','MinimumHealth','MaximumHealth','MinimumDeaths','MaximumDeaths',
              'MinimumTime','MaximumTime','HardcoreHealth','HardcoreDeaths','HardcoreTime')
        gravity=re.search(r'Gravity\s*=\s*\(\s*0\s*,\s*([\w.-]+)\s*,\s*0\s*\)',body,re.I)[1]
        rows.append(struct.pack('<32s48s12f' ,name.lower().encode(),v['LevelName'].encode(),
                                *(scalar(defines.get(v[k],v[k])) for k in keys),-float(defines.get(gravity,gravity))))
    if len(rows)!=len(index) or not rows:raise ValueError('Incomplete playable level rules')
    path=out/'difficulty.bin';temp=path.with_suffix('.partial')
    temp.write_bytes(struct.pack('<4sI',b'MPR2',len(rows))+b''.join(profiles+rows));temp.replace(path)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();pack(args.disc,args.output)
