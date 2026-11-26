"""Check that the native room files contain the original render meshes on both sides of a portal."""
import argparse
from pathlib import Path
import struct

import numpy as np


ROOMS = ('start', 'alley', 'street', 'subway', 'sidealley', 'deadend',
         'home', 'homephone', 'homeliving', 'homestairs')


def read(path):
    data = path.read_bytes()
    magic, nt, nb, nv, nc = struct.unpack_from('<4s4I', data)
    assert magic == b'MPR7'
    textures = []
    offset = 424
    for _ in range(nt):
        w, h, _, mip, _, _ = struct.unpack_from('<HHBBBB', data, offset)
        end = offset + 8 + sum(w*h*2 >> (2*i) for i in range(mip+1))
        textures.append(data[offset:end])
        offset = end
    batches = list(struct.iter_unpack('<4I', data[offset:offset+nb*16]))
    offset += nb*16
    vertices = np.frombuffer(data, '<f4', count=nv*7, offset=offset).reshape(-1, 7)
    offset += nv*28 + nc*40
    n, = struct.unpack_from('<I', data, offset)
    openings = list(struct.iter_unpack('<6fi', data[offset+4:]))
    assert len(openings) == n
    groups = {}
    at = 0
    for tex, light, count, group in batches:
        groups.setdefault(group, []).append((textures[tex], textures[light], vertices[at:at+count]))
        at += count
    assert at == len(vertices)
    exits = {e[-1]: np.array(e[6:9]) for e in struct.iter_unpack('<10fi', data[96:272]) if e[-1] >= 0}
    return groups, openings, exits


def check(assets):
    rooms = [read(assets/(name+'.room')) for name in ROOMS]
    pairs = 0
    for groups, openings, exits in rooms:
        assert sum(len(v) for _, _, v in groups[5]) >= 36, 'Missing worldsphere'
        for slot, opening in enumerate(openings):
            dest = opening[-1]
            base = 8 + 8*slot
            assert base+5 in groups, 'Missing portal mask'
            if dest < 0:
                assert sum(len(v) for _, _, v in groups[base]) > 300, 'Missing distant scenery'
                continue
            offset = exits[dest]
            for state in range(5):
                local = groups.get(base+state, [])
                adjacent = rooms[dest][0].get(state, [])
                assert len(local) == len(adjacent)
                for (tx, lm, v), (tx2, lm2, v2) in zip(local, adjacent, strict=True):
                    assert tx == tx2 and lm == lm2
                    np.testing.assert_allclose(v[:, :3]+offset, v2[:, :3], atol=2e-5, rtol=0)
                    np.testing.assert_array_equal(v[:, 3:], v2[:, 3:])
            pairs += 1
    assert pairs == 16
    title = np.fromfile(assets/'menu.565', '<u2').reshape(400, 240)
    assert all(np.count_nonzero(title[:, row]) > 20 for row in (0, 239))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('assets', type=Path, nargs='?', default=Path('build/native-assets'))
    check(parser.parse_args().assets)
    print('Full-screen title and original meshes, materials, UVs and lighting across 16 room transitions passed.')
