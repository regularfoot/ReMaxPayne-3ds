"""Read the campaign order and authored room events from the locally extracted PAL game."""
import argparse
import gzip
import json
import mmap
import re
from pathlib import Path
from tempfile import TemporaryDirectory

from dade.maxpayne import ras
from max_payne_sdk.max_ldb import MaxLDBReader, EntityProperties, parseType, read_count
from pack_native import member


class CampaignReader(MaxLDBReader):
    def parseFSMs(self, stream):
        # The event name and its optional state filters are distinct LDB fields.
        def section():
            stream.read(1)
            return range(read_count(stream))

        def messages():
            return {'messages': [parseType(stream) for _ in section()]}

        def events():
            result = []
            for _ in section():
                event = parseType(stream)
                before = messages()
                filters = [{'state_name': parseType(stream), 'messages': messages()} for _ in section()]
                result.append(dict(state_name=event, before=before, state_specific={'messages': filters},
                                   after=messages()))
            return {'events': result}

        for _ in range(read_count(stream)):
            name = parseType(stream)
            props = EntityProperties(*(parseType(stream) for _ in range(5)))
            states = {'states': [parseType(stream) for _ in section()], 'default': parseType(stream)}
            before = messages()
            startup_states = [parseType(stream) for _ in section()]
            self.ldb.fsms.add(dict(shared_name=name, properties=props, states=states, startup_before=before,
                                   startup_states=startup_states, startup_after=messages(), state_switch=events(),
                                   string_specific=events(), entity_specific=events()))


def levels(archives):
    config = member(archives / 'x_data.ras', 'data/database/levels/levels.txt').decode('latin1')
    for match in re.finditer(r'^\[([^\]]+)\]\s*\{\s*\[properties\](.*?)^\s*\[difficulty\]', config, re.M | re.S | re.I):
        props = dict((k.lower(), v.strip().strip('"')) for k, v in
                     re.findall(r'^\s*(\w+)\s*=\s*([^;\r\n]+);', match[2], re.M))
        if props.get('directory', '').lower().startswith('part') or match[1].lower() in ('startup_level','end_combat','tutorial'):
            yield dict(id=match[1].lower(), **props)


def level_archive(archives,stage):
    folder=stage['directory'].lower()
    return archives/('x_data.ras' if folder=='front_end' else 'x_level'+folder[4]+'.ras')


def extract(archives, dest):
    dest.mkdir(parents=True, exist_ok=True)
    catalog = list(levels(archives))
    source = {}
    for archive in {level_archive(archives,stage) for stage in catalog}:
        with archive.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for entry in ras.read_directory(data).entries:
                if entry.path.lower().endswith('.ldb'):
                    source[entry.path.lower()] = archive
    with TemporaryDirectory() as tmp:
        for item in catalog:
            path = ('data/database/levels/' + item['directory'] + '/' + item['level']).lower()
            target = dest / (item['id'] + '.json.gz')
            if target.exists():
                continue
            ldb = Path(tmp) / 'level.ldb'
            ldb.write_bytes(member(source[path], path))
            level = CampaignReader(ldb).parse()
            script = {name: getattr(getattr(level, name), name) for name in
                      ('rooms', 'exits', 'characters', 'items', 'fsms', 'triggers', 'waypoints')}
            script['props'] = [{key: getattr(prop, key) for key in
                                ('shared_name', 'properties', 'config', 'animations', 'transform')}
                               for prop in level.dynamic_meshes.dynamic_meshes]
            temp = target.with_suffix('.tmp')
            with gzip.open(temp, 'wt', encoding='utf-8', compresslevel=4) as out:
                json.dump(script, out, default=vars, separators=(',', ':'))
            temp.replace(target)
            print(item['id'], len(script['rooms']), len(script['fsms']), flush=True)
    (dest / 'levels.json').write_text(json.dumps(catalog, indent=2))
    return catalog


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    extract(args.disc / 'english', args.output)
