"""Add selectable speech from a second personally supplied Xbox disc to a campaign."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import struct
from tempfile import TemporaryDirectory

from xbox_disc import archive_root

RECORD = struct.Struct('<80sIIfI')
LANGUAGE_FLAGS = {'ru': 0x100, 'en': 0x200}


def records(path):
    size = path.stat().st_size
    with path.open('rb') as f:
        magic, count = struct.unpack('<4sI', f.read(8))
        if magic != b'MPA1' or not 0 < count <= 8192:
            raise ValueError('Invalid audio bank')
        for _ in range(count):
            key, offset, length, gain, flags = RECORD.unpack(f.read(RECORD.size))
            if not length or length % 2 or offset < 8 + count * RECORD.size or offset + length > size:
                raise ValueError('Invalid PCM extent')
            yield key, offset, length, gain, flags & ~0x300


def merge(banks, output):
    """Keep language variants in one bank; identical PCM occupies storage only once."""
    entries = [(key, language, i, source, at, length, gain, flags)
               for language, source in banks.items()
               for i, (key, at, length, gain, flags) in enumerate(records(source))]
    entries.sort(key=lambda row: (row[0], row[1], row[2]))
    if not entries or len(entries) > 8192:
        raise ValueError('Audio record limit exceeded')
    pending = output.with_suffix('.voice-partial')
    seen, table = {}, []
    with pending.open('wb') as out:
        out.write(struct.pack('<4sI', b'MPA1', len(entries)))
        out.seek(8 + len(entries) * RECORD.size)
        for key, language, _, source, at, length, gain, flags in entries:
            with source.open('rb') as f:
                f.seek(at)
                data = f.read(length)
            if len(data) != length:
                raise ValueError('Truncated PCM')
            identity = sha256(data).digest(), length
            if identity not in seen:
                seen[identity] = out.tell()
                out.write(data)
            table.append(RECORD.pack(key, seen[identity], length, gain, flags | LANGUAGE_FLAGS[language]))
        out.seek(8)
        out.writelines(table)
    pending.replace(output)
    return len(entries), len(seen)


def pack(disc, other_disc, source, campaign, other_language):
    from pack_campaign_audio import pack as pack_audio
    base_language = (campaign / 'language.txt').read_text().strip()
    if base_language not in LANGUAGE_FLAGS or other_language == base_language:
        raise ValueError('Supply the other language and keep the campaign language stamp')
    marker = campaign / 'voice/languages.json'
    if marker.exists():
        raise ValueError('Rebuild the original audio.bank before adding voices again')
    with (campaign/'audio.bank').open('rb') as f:
        _, count = struct.unpack('<4sI', f.read(8))
        if any(RECORD.unpack(f.read(RECORD.size))[4] & 0x300 for _ in range(count)):
            raise ValueError('Regenerate the base audio.bank after an interrupted voice conversion')
    with TemporaryDirectory(dir=campaign.parent) as scratch:
        alternate = Path(scratch) / 'alternate.bank'
        pack_audio(other_disc, source, alternate, reference_root=campaign)
        count, clips = merge({base_language: campaign/'audio.bank', other_language: alternate}, campaign/'audio.bank')
    # Only narration is added. The graphic novel's original pages remain untouched.
    waves = {p.stem.lower(): p for p in (archive_root(other_disc)/'data/database/sounds/story').rglob('*')
             if p.suffix.lower() == '.wav'}
    import subprocess
    target = campaign/'voice'/other_language/'pages'
    target.mkdir(parents=True, exist_ok=True)
    narrations = 0
    for page in sorted((campaign/'pages').glob('*.565')):
        if page.stem not in waves:
            continue
        subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-y', '-i', waves[page.stem],
                        '-ac', '1', '-ar', '22050', '-f', 's16le', target/(page.stem+'.pcm')], check=True)
        narrations += 1
    # Disc movies may contain speech too; prepare optional alternate streams.
    from pack_campaign_movies import MOVIES, pack as pack_movie
    from xbox_disc import movie_source
    for name in MOVIES:
        original = movie_source(disc, name)
        alternate = movie_source(other_disc, name)
        if not original or not alternate:
            continue
        # The stock credits and logo videos are often identical between discs.
        def digest(path):
            from hashlib import file_digest
            with path.open('rb') as f:
                return file_digest(f, 'sha256').digest()
        if digest(original) != digest(alternate):
            pack_movie(alternate, campaign/'voice'/other_language/(name+'.mpv'))
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps({'base': base_language, 'alternate': other_language,
                                 'records': count, 'unique_pcm': clips, 'narrations': narrations}, indent=2)+'\n')
    print(f'Selectable {base_language}/{other_language} voices: {count} variants, {narrations} alternate narrations', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', type=Path)
    parser.add_argument('other_disc', type=Path)
    parser.add_argument('source', type=Path)
    parser.add_argument('campaign', type=Path)
    parser.add_argument('--other-language', choices=tuple(LANGUAGE_FLAGS), required=True)
    args = parser.parse_args()
    pack(args.disc, args.other_disc, args.source, args.campaign, args.other_language)
