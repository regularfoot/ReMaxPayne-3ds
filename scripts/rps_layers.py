"""Read the sprite templates and their paired emitters from an Xbox RPS stream."""
import hashlib
import io
from dataclasses import dataclass
from max_payne_sdk.max_type import parseType


@dataclass
class Layer:
    curves: list
    sequences: list
    additive: bool
    rate: list
    speed: list
    life: list
    scale: list
    drift: list
    controls: list
    gravity: float
    angles: list
    flags: int
    rotation: list
    drag: tuple


def graph(values, at):
    tag, version, channels, length = values[at:at+4]
    if (tag, version) != (113, 3) or channels not in (1, 2, 3) or not 0 < length <= 4096:
        raise ValueError(f'Invalid RPS graph at {at}')
    at += 4
    end = at + length * channels
    return [values[i:i+channels] for i in range(at, end, channels)], end


def parse_layers(raw):
    stream = io.BytesIO(raw)
    values = []
    while stream.tell() < len(raw):
        values.append(parseType(stream))
    if values[:2] != [96, 6]:
        raise ValueError('Unsupported Xbox particle system')
    result = []
    for i in range(len(values)):
        if values[i:i+4] != [48, 4, 32, 3]:
            continue
        at = i + 15
        curves = []
        for _ in range(4):
            curve, at = graph(values, at)
            curves.append(curve)
        images, additive, count = values[at:at+3]
        at += 3
        sequences = []
        for _ in range(count):
            frames = values[at]
            at += 1
            sequence = []
            for _ in range(frames):
                rgb, _, w, h = values[at:at+4]
                at += 4
                alpha = None
                if not additive:
                    alpha, _, aw, ah = values[at:at+4]
                    at += 4
                    if (aw, ah) != (w, h):
                        raise ValueError(f'Mismatched particle mask for {rgb}')
                sequence.append((rgb.lower(), alpha.lower() if alpha else None, w, h))
            sequences.append(sequence)
        if images != 1 or values[at:at+4] != [64, 2, 33, 5]:
            raise ValueError(f'Missing emitter for template at {i}')
        at += 4
        rate, at = graph(values, at)
        speed, at = graph(values, at)
        life, at = graph(values, at)
        drift, at = graph(values, at)
        controls = values[at:at+18]
        scale, at = graph(values, at+18)
        rotation, at = graph(values, at)
        result.append(Layer(curves, sequences, bool(additive), rate, speed, life, scale, drift, controls, values[i+4],
                            values[i+12:i+14], values[i+14], rotation,(values[i+7],values[i+5],values[i+6])))
    return values[2], values[16], result


def selected_frames(sequence):
    # One complete named animation per sprite; a frame must never jump between variants.
    return [sequence[min(len(sequence)-1, int((i+.5)*len(sequence)/4))] for i in range(4)]


def sprite_names(layers):
    return list(dict.fromkeys(
        name
        for layer in layers
        for sequence in layer.sequences
        for name, _, _, _ in selected_frames(sequence)
    ))


def layout_key(path, names):
    return hashlib.sha256('\0'.join([path, *names]).encode()).hexdigest()
