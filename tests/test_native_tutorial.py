"""Check a native Tutorial walkthrough against the events in native.log."""
import argparse
from pathlib import Path
import re


def check(log):
    text = log.read_text()
    menu, returned, revisit = text.partition('MENU_RETURN')
    assert returned and menu.startswith('MENU elapsed_ms='), 'Menu and return required'
    assert 'ROOM load failed' not in text
    assert menu.count('ROOM elapsed_ms=') == 1 and 'ROOM elapsed_ms=' in revisit
    assert 'USE StartRoom::Use_01' in menu, 'Walk to the first phone and press A'
    frame = re.compile(r'FRAME \d+ fps=([\d.]+) pos=([\d.-]+),([\d.-]+),([\d.-]+) yaw=([\d.-]+)')
    frames = [tuple(map(float, row)) for row in frame.findall(menu)]
    assert len(frames) >= 2 and all(f[0] > 0 for f in frames)
    assert any(abs(x-frames[0][1]) + abs(z-frames[0][3]) > 1 for _, x, _, z, _ in frames), 'No movement'
    assert any(abs(yaw-frames[0][4]) > .2 for *_, yaw in frames), 'No camera turn'
    return len(frames)


def check_combat(log):
    first, _, second = log.read_text().partition('MENU_RETURN')
    shot = re.compile(r'SHOT ammo=(\d+) hit=(\d+) distance=([\d.]+) moving=(\d+)')
    shots = shot.findall(first)
    assert len(shots) >= 2 and any(hit == '1' and float(distance) > 0 and moving == '1'
                                   for _, hit, distance, moving in shots), 'No moving hit'
    assert 'RELOADED ammo=18 reserve=68' in first, 'First magazine refill'
    distances = [float(s) for s in re.findall(r'camera=([\d.]+)', first)]
    assert max(distances) > 2.7 and min(distances) < 2.2, 'Camera never approached cover'
    assert 'DRAW frame=2' in second, 'No second visit render'
    assert set(map(int, re.findall(r'GAIT (\d+)', second))) >= {1, 3, 4, 5}, 'Missing locomotion clips'
    assert [int(i[0]) for i in shot.findall(second)] == [*range(17, -1, -1), 17], 'Empty-magazine sequence'
    assert 'RELOADED ammo=18 reserve=54' in second, 'Empty reload failed'


def check_mission(log):
    text = log.read_text()
    assert 'load failed' not in text, 'Tutorial resources failed to load'
    milestones = ['USE StartRoom::Use_01', 'VAN open pickup=pumpshotgun shells=21',
                  'GATE open', 'ENTER Alley2 from=StartRoom', 'ENTER Street from=Alley2',
                  'USE Street::Use_04', 'FIGHT Street enemies=2']
    remaining = text
    for name in milestones:
        _, found, remaining = remaining.partition(name)
        assert found, f'Missing mission event {name}'
    remaining = remaining.split('ENTER Subway from=Street', 1)[0]
    if 'RETRY Street' in remaining:
        remaining = remaining.rsplit('RETRY Street', 1)[1]
        assert 'FIGHT Street enemies=2' in remaining, 'Retry did not restart both opponents'
    fallen = re.findall(r'^ENEMY ([12]) hp=0 down=1$', remaining, re.MULTILINE)
    assert sorted(fallen) == ['1', '2'], 'Both opponents must be defeated in the same encounter'


def check_rifle(log):
    text = log.read_text()
    pickup = 'VAN Street pickup=sniper ammo=5 reserve=7'
    assert text.count(pickup) == 1, 'Street van must give the rifle once'
    back, found, fight = text.partition(pickup)
    assert found and 'ENTER Street from=Subway' in back, 'Get the rifle after returning from the subway'
    assert 'FIGHT Rifle enemies=3' in fight
    fight = fight.rsplit('FIGHT Rifle enemies=3', 1)[-1]
    cleared, done, _ = fight.partition('TRAINING complete')
    assert done and text.count('TRAINING complete') == 1
    assert re.search(r'SCOPE 2x.*SCOPE 4x', cleared, re.S), 'Exercise both scope settings'
    down = re.findall(r'^ENEMY ([123]) hp=0 down=1$', cleared, re.M)
    assert sorted(down) == ['1', '2', '3'], 'Defeat all three in the same encounter'


def check_practice(log):
    after_training = log.read_text().partition('TRAINING complete\n')[2]
    start = 'PRACTICE begin round=1\nFIGHT Practice enemies=6\n'
    active = after_training.partition(start)[2]
    assert active and after_training.count(start) == 1, 'Start the first practice round at the switch'
    clear, done, following = active.partition('PRACTICE clear round=1\n')
    assert done, 'Finish the first practice round'
    retry = 'RETRY Practice\nFIGHT Practice enemies=6\n'
    attempts = clear.split(retry)
    for attempt in attempts[:-1]:
        assert 'DAMAGE player=0\n' in attempt, 'Only restart the encounter after Max goes down'
    room = 'Street'
    defeated = {}
    for line in attempts[-1].splitlines():
        transition = re.fullmatch(r'ENTER (.+) from=(.+)', line)
        if transition:
            destination, previous = transition.groups()
            assert previous == room, 'Discontinuous room transition'
            room = destination
        enemy = re.fullmatch(r'ENEMY (\d+) hp=0 down=1', line)
        if enemy:
            ident = int(enemy[1])
            assert ident not in defeated, 'A defeated gunman respawned on re-entry'
            defeated[ident] = room
    assert defeated == {1: 'Street', 2: 'Street', 3: 'Street', 4: 'Subway',
                        5: 'Side alley', 6: 'Dead end'}, 'Clear all four areas in the same round'
    second = following.partition('PRACTICE begin round=2\nFIGHT Practice enemies=6\n')[2]
    assert second and 'MENU_RETURN' in second, 'Start another wave and return to the menu'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--combat', action='store_true', help='Check the full combat replay')
    parser.add_argument('--subway', action='store_true', help='Check the subway cabinet, healing and return')
    parser.add_argument('--rifle', action='store_true', help='Check both gunfights, the scope and training completion')
    parser.add_argument('--practice', action='store_true', help='Also check the six practice opponents and a second round')
    parser.add_argument('--mission', action='store_true', help='Check the van, rooms and first encounter')
    args = parser.parse_args()
    if args.subway or args.rifle or args.practice:
        check_mission(args.log)
        events=args.log.read_text().split('ENTER Subway from=Street', 1)
        assert len(events)==2, 'Street stairs did not lead into Subway'
        cabinet, opened, after=events[1].partition('CABINET open painkillers=4')
        assert opened and 'PAINKILLER remaining=3' in after and 'HEAL player=' in after
        assert 'ENTER Street from=Subway' in after and after.count('CABINET open')==0
        print('Subway cabinet, healing and return passed.')
        if args.rifle or args.practice:
            check_rifle(args.log)
            print('Street van, rifle pickup, both zoom levels and final encounter passed.')
        if args.practice:
            check_practice(args.log)
            print('Practice opponents, four connected areas and repeat activation passed.')
        if not args.combat:
            raise SystemExit(0)
    if args.mission:
        check_mission(args.log)
        print('Native van, gate, two room transitions and both opponents passed.')
        if not args.combat:
            raise SystemExit(0)
    print(f'Native menu, phone, movement, camera and re-entry passed across {check(args.log)} sampling intervals.')
    if args.combat:
        check_combat(args.log)
        print('Native movement clips, camera, moving hits and both reload paths passed.')
