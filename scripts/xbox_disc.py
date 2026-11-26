"""Locate extracted Xbox archives and identify supported disc revisions."""

from hashlib import file_digest
import os
from pathlib import Path

PAL_XBE = '212c47d25f96b68996d085abf93cb43464b8cf34f156081b4919866a2be74102'
NTSC_XBE = '3e95f75a29bfd37b2d17d2ee8490fa2c4c516faf8b37a6b7ef5860fa83375b90'
REVISIONS = {PAL_XBE: 'PAL', NTSC_XBE: 'NTSC'}


def archive_root(disc):
    for folder in (disc / 'english', disc):
        if (folder / 'x_data.ras').is_file():
            return folder
    raise FileNotFoundError(f'Cannot find x_data.ras in {disc} or {disc / "english"}')


def executable_hash(disc):
    with (disc / 'default.xbe').open('rb') as file:
        return file_digest(file, 'sha256').hexdigest()


def revision(disc):
    digest = executable_hash(disc)
    try:
        return REVISIONS[digest]
    except KeyError:
        raise ValueError(f'Unsupported default.xbe revision: SHA-256 {digest}') from None


def language(disc):
    selected = os.getenv('MP_LANG', 'auto')
    if selected == 'auto':
        executable = disc / 'default.xbe'
        return 'en' if executable.is_file() and executable_hash(disc) == NTSC_XBE else 'ru'
    if selected in ('ru', 'en'):
        return selected
    raise ValueError('MP_LANG must be auto, ru or en')


def movie_source(disc, name):
    archives = archive_root(disc)
    return next((p for folder in (archives / '60', archives / '50', archives)
                 if (p := folder / (name + '.bik')).is_file()), None)
