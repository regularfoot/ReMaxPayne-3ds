"""Embed campaign data in the 3DSX RomFS so console installation is a single file."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
from verify_retail_animations import verify as verify_animations
from verify_weapon_effects import verify as verify_weapon_effects
from tempfile import TemporaryDirectory
from campaign_refs import validate
from campaign_romfs import verify_image
from pack_campaign import verify_lights
from pack_campaign_text import encode as encode_texts
from pack_campaign_movies import MOVIES
from xbox_disc import language, movie_source

EXCLUDED_CAMPAIGN_DIRS = {'sounds', 'pages-reading', 'pages-readable', 'pages-captions'}

def root_path():return Path(__file__).resolve().parents[1]

def package(assets,campaign,elf,smdh,output,tool):
    disc=Path(os.environ.get("GAME_DISC","game"))
    locale=language(disc)
    for folder in (assets,campaign):
        stamp=folder/'language.txt'
        if stamp.is_file() and stamp.read_text().strip()!=locale:
            raise ValueError(f'Rebuild {folder} with MP_LANG={locale}')
    maps={p.stem for p in campaign.glob('*.mpc')}
    if len(maps)!=69 or not {'tutorial','startup_level','part0_level1','p3_l7-3','end_combat'}<=maps:
        raise ValueError('Missing campaign, opening or bonus map')
    for extension in ('evt','nav','bounds','surf','lights','dlg'):
        if maps!={p.stem for p in campaign.glob('*.'+extension)}:
            raise ValueError('Incomplete '+extension+' tables')
    for name in ('transitions.bin','difficulty.bin','combatants.bin','skin_rules.bin','bones.bind','bones.rest','poses.norm','bone_sets.bind','bones.tree','left_grip.bind','cues.bin','torso.bind','faces/max_payne.face','faces/young_max_payne.face','particles.pak','decals.pak','cameras.pak','poses.pak','poses.idx','poses.meta','animations.source.json','stages.idx','audio.bank','projectiles.bin','moves.anim','cast/rat.mesh','cast/rat.anim','items/medipack_small.mesh','items/berettadual.mesh',
                 'weapons/m79.mesh','pages/p1l0a_001.565'):
        if not (campaign/name).is_file():raise ValueError('Missing '+name)
    for name in MOVIES:
        if bool(movie_source(disc,name)) != (campaign/(name+'.mpv')).is_file():
            raise ValueError(f'Rebuild campaign movie data for {name}')
    for pattern,expected in [('cast/*.actor',47),('cases/*.mesh',5),('pages/*.565',211),('weapons/*.mesh',13),('weapons/*.motion',13),('weapons/*.fx',13),('loading/*.565',69)]:
        if len(list(campaign.glob(pattern)))!=expected:raise ValueError('Incomplete '+pattern)
    if not (assets/"weapons.ui").is_file():raise ValueError("Missing Xbox weapon icon atlas")
    for name in ('menu.ui','pause.tex'):
        if not (assets/name).is_file():raise ValueError('Missing '+name)
    verify_animations(disc,campaign)
    verify_weapon_effects(disc,campaign)
    anchors=(campaign/'hit_bones.bind').read_bytes()
    if len(anchors)!=20:raise ValueError('Missing projectile bone bindings')
    import struct
    tag,bones,*joints=struct.unpack('<4s4I',anchors)
    if tag!=b'AHB1' or bones!=struct.unpack_from('<I',(campaign/'bones.bind').read_bytes())[0] or any(j>=bones for j in joints):
        raise ValueError('Invalid projectile bone bindings')
    if (campaign/'texts.bin').read_bytes()!=encode_texts(campaign/'objectives'):
        raise ValueError('HUD text bank differs from source captions')
    for test in ('test_native_texture.py','test_triangulate.py'):
        subprocess.run([sys.executable,root_path()/'tests'/test],check=True)
    verify_lights(disc, campaign)
    validate(campaign)
    root=Path(__file__).resolve().parents[1]
    with TemporaryDirectory() as scratch:
        capture_probes=(
            ('aim','xbox_aim.bin',(root/'source/native/pose_bank.c',)),
            ('bone','xbox_bone_blends.bin',(root/'source/native/pose_bank.c',)),
            ('timing','xbox_animation_timing.bin',()),
        )
        for name,filename,modules in capture_probes:
            fixture=root/'tests/fixtures'/filename
            if not fixture.is_file():
                print(f'Skipping local Xbox capture comparison: {filename}')
                continue
            probe=Path(scratch)/(name+'_probe')
            subprocess.run(['cc','-std=c11','-O2','-I',root/'source/native',
                            root/'tests'/f'native_{name}_probe.c',*modules,'-lm','-o',probe],check=True)
            subprocess.run([probe,fixture],cwd=campaign,check=True)
        probe=Path(scratch)/'pose_probe'
        subprocess.run(['cc','-std=c11','-O2','-I',root/'source/native',root/'tests/native_pose_probe.c',
                        root/'source/native/pose_bank.c','-lm','-o',probe],check=True)
        subprocess.run([probe],cwd=campaign,check=True)
        cache=Path(scratch)/'pose_cache_probe'
        subprocess.run(['cc','-std=c11','-O2','-I',root/'source/native',root/'tests/native_pose_cache_probe.c','-lm','-o',cache],check=True)
        subprocess.run([cache],cwd=campaign,check=True)
        events=Path(scratch)/'event_probe'
        modules=('campaign_bounds','pose_bank','camera_path','save')
        subprocess.run(['cc','-std=c11','-O2','-I',root/'source/native',root/'tests/native_campaign_interactions.c',
                        *(root/'source/native'/(module+'.c') for module in modules),'-lm','-o',events],check=True)
        for stage in sorted(maps):
            subprocess.run([events,stage],cwd=campaign,check=True)
        doors=Path(scratch)/'door_probe'
        subprocess.run(['cc','-std=c11','-O2','-I',root/'source/native',root/'tests/native_door_probe.c',
                        *(root/'source/native'/(module+'.c') for module in ('director','campaign_bounds','pose_bank','camera_path','save','portal')),
                        '-lm','-o',doors],check=True)
        subprocess.run([doors],cwd=campaign,check=True)
        contact=Path(scratch)/'mesh_contact_probe'
        subprocess.run(['cc','-std=c11','-O2','-I',root/'source/native',root/'tests/native_mesh_contact_probe.c',
                        *(root/'source/native'/(module+'.c') for module in ('director','campaign_bounds','pose_bank','camera_path','save','portal','scene')),
                        '-lm','-o',contact],check=True)
        subprocess.run([contact],cwd=campaign,check=True)
        audio=Path(scratch)/'audio_probe'
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread','-I',root/'tests/audio_host',
                        '-I',root/'source/native',root/'tests/native_audio_probe.c','-lm','-o',audio],check=True)
        subprocess.run([audio],cwd=campaign,check=True)
        for name in ('async','gyro'):
            probe=Path(scratch)/(name+'_probe')
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread','-I',root/'tests/audio_host',
                            '-I',root/'source/native',root/'tests'/('native_'+name+'_probe.c'),'-lm','-o',probe],check=True)
            subprocess.run([probe],cwd=scratch,check=True,timeout=20)
        text_probe=Path(scratch)/'text_probe'
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I',root/'source/native',
                        root/'tests/native_text_probe.c',root/'source/native/localization.c','-o',text_probe],check=True)
        subprocess.run([text_probe],cwd=campaign,check=True)
        scene=Path(scratch)/'scene_probe'
        subprocess.run(['cc','-std=c11','-O2',root/'tests/test_native_scene.c',
                        *(root/'source/native'/(module+'.c') for module in ('scene','touch','portal')),
                        '-lm','-o',scene],check=True)
        subprocess.run([scene],check=True)
        portals=Path(scratch)/'portal_probe'
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I',root/'source/native',
                        root/'tests/native_portal_probe.c',root/'source/native/portal.c','-lm','-o',portals],check=True)
        totals=[0,0,0,0]
        for stage in sorted(maps):
            row=subprocess.check_output([portals,campaign/(stage+'.mpc')],text=True).split()
            totals=[a+int(b) for a,b in zip(totals,row[-4:])]
        print(f'All {totals[0]} portals: {totals[1]} aperture triangles, {totals[2]} motion checks, '
              f'{totals[3]} inclined/horizontal exits and receiving-room transforms verified')
        combat=Path(scratch)/'combat_probe'
        subprocess.run(['cc','-std=c11','-O2',root/'tests/test_campaign_ballistics.c',
                        root/'source/native/campaign_combat.c',root/'source/native/scene.c','-lm','-o',combat],check=True)
        subprocess.run([combat,campaign/'projectiles.bin'],check=True)
        nav=Path(scratch)/'nav_probe'
        subprocess.run(['cc','-std=c11','-O2',root/'tests/test_native_nav.c','-lm','-o',nav],check=True)
        subprocess.run([nav],check=True)
    output.parent.mkdir(parents=True,exist_ok=True)
    with TemporaryDirectory(dir=elf.parent) as temp:
        romfs=Path(temp)/'romfs';romfs.mkdir()
        def attach(src,dest):
            dest.parent.mkdir(parents=True,exist_ok=True)
            try:os.link(src,dest)
            except OSError:shutil.copyfile(src,dest)
        for src in assets.rglob('*'):
            if src.is_file():attach(src,romfs/src.relative_to(assets))
        for src in campaign.rglob('*'):
            rel=src.relative_to(campaign)
            if not src.is_file() or src.suffix=='.partial' or rel.parts[0] in EXCLUDED_CAMPAIGN_DIRS:continue
            attach(src,romfs/'campaign'/rel)
        staged=output.with_suffix('.partial')
        subprocess.run([tool,elf,staged,'--smdh='+str(smdh),'--romfs='+str(romfs)],check=True)
        verify_image(staged,romfs)
        staged.replace(output)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('campaign',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();sdk=Path(os.environ.get('DEVKITPRO','/opt/devkitpro'))
    package(Path('build/native-assets'),a.campaign,Path('build/native/maxpayne-native.elf'),Path('build/native/native.smdh'),
            a.output,sdk/'tools/bin/3dsxtool')
