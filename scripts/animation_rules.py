"""Read authored body bone sets and the separate Xbox weapon posing flags."""
import re
import struct


def clean(text):
    return re.sub(r'//[^\r\n]*|/\*.*?\*/', '', text, flags=re.S)


def body_sets(skeleton, bones):
    records = re.findall(r'\[(OneHanded|TwoHanded)(Upper|Lower)BodyBone\]\s*Name\s*=\s*"([^"]+)"', clean(skeleton), re.I)
    result = []
    for kind in ('onehanded', 'twohanded'):
        classes = {}
        for group, side, name in records:
            if group.lower() != kind:
                continue
            name = name.lower()
            if name in classes:
                raise ValueError('Duplicate body bone: ' + name)
            classes[name] = side.lower() == 'upper'
        if set(classes) != set(bones):
            raise ValueError(('Incomplete authored body set', kind, set(bones) ^ set(classes)))
        result.append(bytes([classes[b] for b in bones] + [1]))
    return result


def weapon_flags(text):
    fields = dict(re.findall(r'\b(TwoHandedBones|TwoHandedAnimation|UseMultipleGunBones)\s*=\s*(YES|NO)\s*;', clean(text), re.I))
    fields = {k.lower(): v.lower() == 'yes' for k, v in fields.items()}
    if not fields:
        return None
    names = ('twohandedbones', 'twohandedanimation', 'usemultiplegunbones')
    if set(fields) != set(names):
        raise ValueError('Incomplete weapon pose flags')
    return bytes([fields[n] for n in names] + [0])


def encode(skeleton, bones, weapons):
    masks = body_sets(skeleton, bones)
    rows = []
    for name, text in sorted(weapons.items()):
        flags = weapon_flags(text)
        if flags is None:
            continue
        if len(name.encode()) >= 24:
            raise ValueError(name)
        rows.append(struct.pack('<24s', name.encode()) + flags)
    if not rows:
        raise ValueError('No weapon pose rules')
    return struct.pack('<4sII', b'BMS1', len(bones) + 1, len(rows)) + b''.join(masks + rows)
