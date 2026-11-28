"""Identify opaque surfaces with an overlapping face on their reverse side."""

from collections import defaultdict

import numpy as np

from native_texture import RGB565, RGB8

CULL_BACK_FACES = 0x2000
TEXTURE_INDEX = 0x1fff
PLANE_EPSILON = 0.0003


def _signed_area(points):
    return sum(a[0] * b[1] - a[1] * b[0]
               for a, b in zip(points, points[1:] + points[:1]))


def _overlap(left, right):
    # Clip one triangle against the three half-planes of the other. Test
    # area after clipping so shared edges and corners stay two-sided.
    polygon = list(left)
    sign = 1 if _signed_area(list(right)) > 0 else -1
    for a, b in zip(right, np.roll(right, -1, axis=0)):
        if len(polygon) < 3:
            return False
        edge = b - a
        normal = sign * np.array((-edge[1], edge[0]))
        distances = (polygon - a) @ normal
        visible = []
        for i, point in enumerate(polygon):
            start, end = distances[i - 1], distances[i]
            if (start < 0) != (end < 0):
                previous = polygon[i - 1]
                visible.append(previous + (point - previous) * (start / (start - end)))
            if end >= 0:
                visible.append(point)
        polygon = visible
    return abs(_signed_area(polygon)) > 1e-7


def opposing_faces(vertices):
    lo, hi = vertices.min(axis=1), vertices.max(axis=1)
    # Sweep along the axis that separates the most bounds first.
    sweep = np.argmax(np.ptp(lo, axis=0) / np.maximum((hi - lo).mean(axis=0), PLANE_EPSILON))
    order = np.argsort(lo[:, sweep], kind='stable')
    vertices, lo, hi = vertices[order], lo[order], hi[order]
    limit = np.searchsorted(lo[:, sweep], hi[:, sweep] + PLANE_EPSILON, side='right')
    edges = vertices[:, 1:] - vertices[:, :1]
    normals = np.cross(edges[:, 0], edges[:, 1])
    norms = np.linalg.norm(normals, axis=1)
    normals /= np.maximum(norms[:, None], 1e-16)
    pair = np.zeros(len(vertices), dtype=bool)
    for i, end in enumerate(limit):
        nearby = np.arange(i + 1, end)
        if not len(nearby):
            continue
        nearby = nearby[normals[nearby] @ normals[i] < -1 + 1e-7]
        if not len(nearby):
            continue
        overlaps = ((hi[nearby] >= lo[i] - PLANE_EPSILON) &
                    (lo[nearby] <= hi[i] + PLANE_EPSILON)).all(axis=1)
        nearby = nearby[overlaps]
        if not len(nearby):
            continue
        coplanar = np.max(abs((vertices[nearby] - vertices[i, 0]) @ normals[i]), axis=1) <= PLANE_EPSILON
        nearby = nearby[coplanar]
        axis = np.argmax(abs(normals[i]))
        left = np.delete(vertices[i], axis, axis=1)
        for j in nearby:
            if np.max(abs((vertices[i] - vertices[j, 0]) @ normals[j])) <= PLANE_EPSILON:
                right = np.delete(vertices[j], axis, axis=1)
                if _overlap(left, right):
                    pair[i] = pair[j] = True
    return order[pair]


def cull_opposing_faces(batches, textures):
    """Split opaque batches so only proven opposing faces use backface culling."""
    groups = defaultdict(list)
    vertices = []
    start = 0
    for base, light, owner, packed in batches:
        count = len(packed) // 84
        if (light >> 14 == 0 and not base & ~TEXTURE_INDEX
            and textures[base][4] in (RGB8, RGB565)):
            groups[owner].append(np.arange(start, start + count))
            vertices.append((start, np.frombuffer(packed, dtype='<f4').reshape(-1, 3, 7)[..., :3]))
        start += count
    marked = np.zeros(start, dtype=bool)
    if not vertices:
        return batches
    positions = np.empty((start, 3, 3), dtype=np.float64)
    for first, points in vertices:
        positions[first:first + len(points)] = points
    # Each prop is grouped separately, since independently animated owners
    # can share a plane at rest and then move out of it during a scene.
    for chunks in groups.values():
        indices = np.concatenate(chunks)
        if len(indices) > 1:
            marked[indices[opposing_faces(positions[indices])]] = True
    if not marked.any():
        return batches
    output = []
    start = 0
    for base, light, owner, packed in batches:
        triangles = np.frombuffer(packed, dtype='V84')
        flags = marked[start:start + len(triangles)]
        start += len(triangles)
        if flags.any():
            if not flags.all():
                output.append((base, light, owner, triangles[~flags].tobytes()))
            output.append((base, light | CULL_BACK_FACES, owner, triangles[flags].tobytes()))
        else:
            output.append((base, light, owner, packed))
    return output
