"""Keep HUD captions in one small bank, loaded before scripts can show them."""
import argparse
from pathlib import Path
import re
import struct


def encode(directory):
    rows=[]
    for path in sorted(directory.glob('*.txt')):
        if not re.fullmatch(r'[a-z0-9_-]{1,100}',path.stem):
            raise ValueError('Invalid caption key: '+str(path))
        text=path.read_bytes().split(b'\n',1)[0]  # Same first line as fgets.
        if b'\0' in text:raise ValueError('NUL in caption: '+str(path))
        rows.append(path.stem.encode()+b'\0'+text+b'\0')
    if not rows:raise ValueError('No captions')
    data=b''.join(rows)
    return struct.pack('<4sII',b'MPT1',len(rows),len(data))+data


def pack(campaign):
    (campaign/'texts.bin').write_bytes(encode(campaign/'objectives'))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('campaign',type=Path)
    pack(p.parse_args().campaign)
