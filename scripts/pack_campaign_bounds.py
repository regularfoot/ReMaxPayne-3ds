"""Keep compact prop contact volumes and walkable platform faces for off-screen actors."""
from argparse import ArgumentParser
from pathlib import Path
import struct


def pack(path):
    with path.open('rb') as src:
        header = struct.unpack('<4s4I4f', src.read(36))
        if header[0] != b'MPC1':
            raise ValueError(path)
        rooms, textures = header[1:3]
        offsets = struct.unpack(f'<{rooms + textures + 2}I', src.read(4 * (rooms + textures + 2)))
        boxes, floors = [], []
        for room in range(rooms):
            src.seek(offsets[room + 1])
            _, batches, solids, exits, vertices = struct.unpack('<64s4I', src.read(80))
            for _ in range(exits):
                _, _, points = struct.unpack_from('<3I', src.read(72))
                src.seek(points * 12, 1)
            src.seek(vertices * 28 + batches * 12, 1)
            for _ in range(solids):
                prop, count = struct.unpack('<iI', src.read(8))
                if prop < 0:
                    src.seek(count * 40, 1)
                    continue
                faces = list(struct.iter_unpack('<10f', src.read(count * 40)))
                points = [face[k:k + 3] for face in faces for k in (0, 3, 6)]
                low = tuple(map(min, zip(*points)))
                high = tuple(map(max, zip(*points)))
                first = len(floors)
                floors.extend(struct.pack('<9f', *face[:9]) for face in faces if face[9] > .58)
                boxes.append(struct.pack('<4H6f', room, prop, first, len(floors) - first, *low, *high))
            if src.tell() != offsets[room + 2]:
                raise ValueError(f'Invalid room {room} in {path}')
    target = path.with_suffix('.bounds')
    tmp = target.with_suffix('.bounds.partial')
    tmp.write_bytes(struct.pack('<4s2I', b'MPB1', len(boxes), len(floors)) + b''.join(boxes + floors))
    tmp.replace(target)


if __name__ == '__main__':
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    for path in sorted(args.directory.glob('*.mpc')):
        pack(path)
