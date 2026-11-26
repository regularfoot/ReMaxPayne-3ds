"""Material channels and alpha-test thresholds from the same disc as the level."""
from dataclasses import dataclass
from functools import lru_cache
import re

from pack_native import member


@dataclass(frozen=True)
class MaterialRule:
    channels: int
    draw: bool
    blend: bool
    depth_write: bool
    alpha_reference: int


@lru_cache(None)
def material_rules(archives):
    text = member(archives/'x_data.ras', 'data/database/materials.txt').decode('latin1')
    text = re.sub(r'//[^\n]*', '', text)
    constants = dict(re.findall(r'^\s*#define\s+(\w+)\s+(\d+)\b', text, re.M))
    parts = re.split(r'(?m)^\s*\[(\w+)\]', text)
    rules = {}
    for name, block in zip(parts[1::2], parts[2::2]):
        values = dict(re.findall(r'(\w+)\s*=\s*([^;\s]+)\s*;', block))
        def flag(key):
            return {'TRUE': True, 'FALSE': False}[values[key].upper()]
        channels = sum((1 << i) for i, key in enumerate(('CharacterCollides', 'SmallProjectileCollides',
                       'BigProjectileCollides', 'SmallProjectilePenetrates')) if flag(key))
        ref = values['AlphaReference']
        rules[name.casefold()] = MaterialRule(channels, flag('DrawPolygons'), flag('BlendedAlphaTest'),
                                               flag('WritesZBuffer'), int(constants.get(ref, ref)))
    return rules
