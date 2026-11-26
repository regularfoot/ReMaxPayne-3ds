"""Exercise the PAL menu handler reached by B after dismissing the attract movie."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def exercise(directory, arm_index, disk_image=None):
    disk = directory / 'disk'
    if disk_image:
        shutil.copytree(disk_image, disk)
    else:
        disk.mkdir()
    pad = directory / 'pad'
    log = directory / 'boot.log'
    calls = directory / 'calls'
    pad.write_text('0 0 -2100 1470 0 0')
    env = {**os.environ, 'MP_HOST_CLOCK_SCALE': '30', 'MP_HOST_PAD_FILE': str(pad),
           'MP_BOOT_TRACE_TARGETS': '1'}
    with log.open('w') as journal, calls.open('w') as trace, subprocess.Popen(
            [ROOT / 'build/boot-host/maxpayne-boot', ROOT / 'game/default.xbe', disk],
            env=env, stdout=journal, stderr=trace) as game:
        def wait_for(marker, offset=0):
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                data = log.read_text()[offset:]
                if game.poll() is not None or 'STOP ' in data:
                    raise RuntimeError(data[-800:])
                if marker in data:
                    return
                time.sleep(.04)
            raise TimeoutError(marker)

        try:
            wait_for('Attract00.bik status=00000000')
            time.sleep(1)
            for digital, analog in [(0, 1), (0, 0), (2, 0), (0, 0), (1, 0), (0, 0), (0, 2), (0, 0)]:
                offset = log.stat().st_size
                pending = pad.with_suffix('.tmp')
                pending.write_text(f'{digital:x} {analog:x} -2100 1470 0 0')
                pending.replace(pad)
                wait_for(f'digital={digital:02X} analog={analog:02X}', offset)
                time.sleep(.12)
            wait_for('VIDEO frame=', log.stat().st_size)
        finally:
            game.terminate()
            game.wait(timeout=10)
    observed = set(re.findall(r'^[0-9A-F]{8}$', calls.read_text(), re.M))
    linked = set(re.findall(r'\{0x([0-9A-F]{8})u,', arm_index.read_text()))
    assert '00052680' in observed, 'The console-reported B handler was never called'
    assert observed <= linked, f'Missing ARM menu targets: {sorted(observed - linked)}'
    print(f'Attract movie, B handler and next frame passed with {len(observed)} linked targets.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arm-index', type=Path, default=ROOT / 'build/boot-3ds/linked_dispatch.c')
    parser.add_argument('--disk-image', type=Path, help='copy an existing Xbox disk into the test')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        exercise(Path(directory), args.arm_index, args.disk_image)
