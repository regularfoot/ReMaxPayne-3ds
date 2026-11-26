#!/usr/bin/env python3
"""Extract an XDVDFS image, rejecting corrupt entries and unsafe paths."""
import argparse
import struct
from pathlib import Path


def extract(image, destination):
    image_size = image.stat().st_size
    destination.mkdir(parents=True, exist_ok=True)
    count = 0
    active = set()
    with image.open('rb') as stream:
        def read(offset, size):
            if offset < 0 or size < 0 or offset + size > image_size:
                raise ValueError('Image extent outside file')
            stream.seek(offset)
            data = stream.read(size)
            if len(data) != size:
                raise ValueError('Truncated image')
            return data
        base = None
        for candidate in (0, 0xFD90000, 0x2080000):
            if candidate + 33 * 2048 <= image_size:
                header = read(candidate + 32 * 2048, 2048)
                if header[:20] == b'MICROSOFT*XBOX*MEDIA':
                    base = candidate
                    root_sector, root_size = struct.unpack_from('<II', header, 20)
                    break
        if base is None:
            raise ValueError('No XDVDFS volume found')
        def directory(sector, size, folder):
            nonlocal count
            key = (sector, size)
            if key in active:
                raise ValueError('Directory cycle')
            active.add(key)
            table = read(base + sector * 2048, size)
            visited = set()
            pending = [0] if size else []
            while pending:
                offset = pending.pop()
                if offset in visited or offset + 14 > size:
                    raise ValueError('Corrupt directory tree')
                visited.add(offset)
                left, right, start, length, flags, name_size = struct.unpack_from('<HHIIBB', table, offset)
                if offset + 14 + name_size > size or not name_size:
                    raise ValueError('Invalid filename extent')
                name = table[offset + 14:offset + 14 + name_size].decode('cp1252')
                if name in ('.', '..') or any(c in name for c in ('/', '\\', '\0', ':')):
                    raise ValueError('Unsafe filename')
                target = folder / name
                if target.is_symlink() or target.exists():
                    raise FileExistsError(target)
                if flags & 0x10:
                    target.mkdir()
                    directory(start, length, target)
                else:
                    if base + start * 2048 + length > image_size:
                        raise ValueError('File outside image')
                    stream.seek(base + start * 2048)
                    remaining = length
                    with target.open('xb') as output:
                        while remaining:
                            data = stream.read(min(1024 * 1024, remaining))
                            if not data:
                                raise ValueError('Truncated file')
                            output.write(data)
                            remaining -= len(data)
                    count += 1
                for child in (right, left):
                    if child:
                        pending.append(child * 4)
            active.remove(key)
        directory(root_sector, root_size, destination)
    print(f'Extracted {count} files into {destination}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    extract(args.image, args.destination)
