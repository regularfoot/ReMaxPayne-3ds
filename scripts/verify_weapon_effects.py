"""Verify weapon-local origins and muzzle event names against the Xbox archive."""
import mmap
from pathlib import Path
import re
import struct
from dade.maxpayne import ras
from xbox_disc import archive_root


def verify(disc, campaign):
    data=(campaign/'particles.pak').read_bytes()
    tag,count,pages=struct.unpack_from('<4sII',data)
    assert tag==b'RPS1'
    effects={data[12+80*i:12+80*i+64].split(b'\0')[0].decode() for i in range(count)}
    checked=0
    with (archive_root(disc)/'x_data.ras').open('rb') as file, mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as archive:
        entries={e.path.lower():e for e in ras.read_directory(archive).entries}
        for fx in sorted((campaign/'weapons').glob('*.fx')):
            path='data/database/weapons/'+fx.stem+'.txt'
            text=ras.member_bytes(archive,entries[path]).decode('latin1')
            text=re.sub(r'//[^\r\n]*','',text)
            match=re.search(r'\bProjectileOffset\s*=\s*\(([^)]+)\)',text,re.I)
            expected=tuple(map(float,match[1].split(','))) if match else (0.,0.,0.)
            names=re.findall(r'PS_StartEffect\s*\(\s*(\w+)',text,re.I)
            name=names[0].lower() if names else ''
            magic,*values,actual=struct.unpack('<4s3f64s',fx.read_bytes())
            actual=actual.split(b'\0')[0].decode()
            if magic!=b'WFX1' or any(abs(a-b)>1e-6 for a,b in zip(values,expected)) or actual!=name:
                raise ValueError('Changed original weapon effect: '+fx.stem)
            if name and name not in effects:raise ValueError('Missing muzzle particle definition: '+name)
            checked+=1
    if checked!=13:raise ValueError('Incomplete weapon effect table')
    print('All 13 Xbox weapon-local origins and muzzle events verified; melee has no invented fire',flush=True)


if __name__=='__main__':
    import sys
    verify(Path(sys.argv[1]),Path(sys.argv[2]))
