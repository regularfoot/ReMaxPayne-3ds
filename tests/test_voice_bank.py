"""Voice conversion tests use synthetic PCM, without proprietary game data."""
import importlib.util
from pathlib import Path
import struct
from tempfile import TemporaryDirectory
import unittest

module = Path(__file__).resolve().parents[1]/'scripts/pack_voice.py'
spec = importlib.util.spec_from_file_location('pack_voice', module)
import sys
sys.path.insert(0, str(module.parent))
voice = importlib.util.module_from_spec(spec)
spec.loader.exec_module(voice)


def bank(path, entries):
    start = 8+96*len(entries)
    table, data = [], bytearray()
    for key, pcm in entries:
        table.append(struct.pack('<80sIIfI', key.encode(), start+len(data), len(pcm), 1.0, 2))
        data.extend(pcm)
    path.write_bytes(struct.pack('<4sI', b'MPA1', len(entries))+b''.join(table)+data)


class VoiceBank(unittest.TestCase):
    def test_languages_share_effect_storage_and_preserve_voice_variants(self):
        with TemporaryDirectory() as name:
            root = Path(name)
            bank(root/'ru.bank', [('dialog/line', b'RU'*32), ('weapons/fire', b'FX'*32)])
            bank(root/'en.bank', [('dialog/line', b'EN'*32), ('weapons/fire', b'FX'*32)])
            count, clips = voice.merge({'ru': root/'ru.bank', 'en': root/'en.bank'}, root/'combined.bank')
            self.assertEqual((count, clips), (4, 3))
            payload = (root/'combined.bank').read_bytes()
            rows = [voice.RECORD.unpack_from(payload, 8+i*96) for i in range(count)]
            lines = [r for r in rows if r[0].split(b'\0')[0]==b'dialog/line']
            self.assertEqual({r[4]&0x300 for r in lines}, {0x100, 0x200})
            for row in lines:
                self.assertEqual(payload[row[1]:row[1]+row[2]], (b'RU' if row[4]&0x100 else b'EN')*32)
            effects = [r for r in rows if r[0].split(b'\0')[0]==b'weapons/fire']
            self.assertEqual(effects[0][1:3], effects[1][1:3])
            self.assertEqual(len(payload), 8+4*96+3*64)

    def test_reject_truncated_bank(self):
        with TemporaryDirectory() as name:
            root = Path(name)
            bank(root/'ru.bank', [('dialog/line', b'RU'*32)])
            data = (root/'ru.bank').read_bytes()
            (root/'ru.bank').write_bytes(data[:-2])
            with self.assertRaises(ValueError):
                voice.merge({'ru': root/'ru.bank'}, root/'out.bank')


if __name__=='__main__':
    unittest.main()
