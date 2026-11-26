"""Verify a console or emulator native.log recorded from the playable house prologue."""
import argparse
from pathlib import Path
import re


def check(text):
    assert 'load failed' not in text
    run, done, _ = text.partition('MENU_RETURN')
    assert done, 'Return to the menu after reaching the upstairs landing'
    events = [
        'COMIC page=3', 'COMIC page=4', 'COMIC page=5', 'PROLOGUE start P0_L1',
        'COMIC page=6', 'PROLOGUE Beretta rounds=99', 'ENTER Telephone from=Entry hall',
        'COMIC page=7', 'ENTER Living room from=Telephone', 'ENTER Stairs from=Living room',
        'PROLOGUE upstairs landing',
    ]
    rest = run
    for event in events:
        _, found, rest = rest.partition(event)
        assert found, f'Missing prologue event {event}'
    assert re.findall(r'^COMIC page=(\d+)', run, re.M) == list('34567')
    assert run.count('PROLOGUE Beretta rounds=99') == 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    check(parser.parse_args().log.read_text())
    print('Prologue introduction, wall mark, telephone, four rooms and upstairs landing passed.')
