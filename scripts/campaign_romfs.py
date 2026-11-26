"""Read and verify the level-3 RomFS embedded in a release 3DSX."""
import struct

class Rom:
    def __init__(self, f):
        self.f = f
        if f.read(4) != b'3DSX':
            raise ValueError('Expected 3DSX')
        f.seek(40)
        start, = struct.unpack('<I', f.read(4))
        f.seek(start)
        head, _, _, dirs, d_size, _, _, files, f_size, blob = struct.unpack('<10I', f.read(40))
        if head != 40:
            raise ValueError('Unknown RomFS header')
        f.seek(start+dirs)
        table = f.read(d_size)
        paths = {}
        at = 0
        while at < len(table):
            parent, _, _, _, _, n = struct.unpack_from('<6I', table, at)
            paths[at] = (parent, table[at+24:at+24+n].decode('utf-16-le'))
            at += 24 + ((n+3) & ~3)
        def directory(offset):
            names = []
            while offset:
                offset, name = paths[offset]
                names.append(name)
            return '/'.join(reversed(names))
        self.index = {}
        f.seek(start+files)
        table = f.read(f_size)
        at = 0
        while at < len(table):
            parent, _, offset, size, _, n = struct.unpack_from('<IIQQII', table, at)
            name = table[at+32:at+32+n].decode('utf-16-le')
            path = '/'.join(filter(None, (directory(parent),name)))
            if path in self.index or start+blob+offset+size > f.seek(0,2):
                raise ValueError(path)
            self.index[path] = start+blob+offset, size
            at += 32 + ((n+3) & ~3)

    def read(self, name, size=None):
        pos, length = self.index['campaign/'+name]
        self.f.seek(pos)
        return self.f.read(length if size is None else min(size,length))

def verify_image(image,tree):
    with image.open('rb') as src:
        rom=Rom(src)
        files={p.relative_to(tree).as_posix():p for p in tree.rglob('*') if p.is_file()}
        if set(rom.index)!=set(files):raise ValueError('Packaged RomFS file list differs from staging')
        for name,path in files.items():
            offset,length=rom.index[name]
            if length!=path.stat().st_size:raise ValueError('Incorrect embedded size for '+name)
            src.seek(offset)
            with path.open('rb') as original:
                while chunk:=original.read(1024*1024):
                    if src.read(len(chunk))!=chunk:raise ValueError('Incorrect embedded data in '+name)
    print(f'{len(files)} embedded files verified',flush=True)
