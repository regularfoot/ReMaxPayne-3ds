#!/usr/bin/env python3
"""Build the native 3DS executable and embed the prepared assets as RomFS."""
import os
from pathlib import Path
import struct
import subprocess
from xbox_disc import language

root = Path(__file__).resolve().parents[1]
source = root/'source/native'
build = root/'build/native'
assets = root/'build/native-assets'
sdk = Path(os.environ.get('DEVKITPRO', '/opt/devkitpro')).resolve()
arm = Path(os.environ.get('DEVKITARM', sdk/'devkitARM')).resolve()
locale = language(Path(os.environ.get('GAME_DISC', root/'game')))
assets_locale = assets/'language.txt'
if assets_locale.is_file() and assets_locale.read_text().strip() != locale:
    raise SystemExit('Run scripts/pack_native.py again for the selected language')
if not all((assets/name).is_file() for name in ('menu.ui','pause.tex','aim.bind','weapons.ui','lower.ui','credits0.565','credits1.565','menu.565', 'start.room', 'ring.pcm', 'answer.pcm', 'max.actor', 'shot.pcm', 'reload.pcm', 'alley.room', 'street.room', 'subway.room', 'sidealley.room', 'deadend.room',
                                              'street.nav', 'johnny.actor', 'pump.pcm', 'pump_reload.pcm', 'door.pcm', 'cabinet.pcm', 'dose.pcm', 'sniper.pcm', 'sniper_reload.pcm', 'rifle.anim', 'switch.pcm','young.actor','home.room','homephone.room','homeliving.room','homestairs.room',
                                              *(f'page{i}.{ext}' for i in range(3,8) for ext in ('565','pcm')))):
    raise SystemExit('Prepare native assets with scripts/pack_native.py first')
build.mkdir(parents=True, exist_ok=True)
shader = build/'room.shbin'
subprocess.run([sdk/'tools/bin/picasso','-o',shader,source/'room.v.pica'],check=True)
words = ','.join(hex(w) for w, in struct.iter_unpack('<I',shader.read_bytes()))
(build/'shader.inc').write_text('static const uint32_t room_shader[] = {'+words+'};\n')
elf = build/'maxpayne-native.elf'
flags = ['-O2','-g','-std=gnu11','-Wall','-Wextra','-Werror','-march=armv6k','-mtune=mpcore','-mfpu=vfp',
         '-mfloat-abi=hard','-mtp=soft','-D__3DS__','-mword-relocations','-ffunction-sections','-fdata-sections',
         '-I'+str(build),'-I'+str(sdk/'libctru/include')]
if locale == 'en':
    flags.append('-DMP_LANGUAGE_EN')
if (arm/'arm-none-eabi/lib/sysbase.specs').exists():
    flags.append('-specs=sysbase.specs')
subprocess.run([arm/'bin/arm-none-eabi-gcc',*flags,source/'main.c',source/'ui.c',source/'campaign_file.c',source/'audio_bank.c',source/'save.c',source/'async_save.c',source/'async_log.c',source/'campaign.c',source/'game_rules.c',source/'director.c',source/'campaign_world.c',source/'portal.c',source/'campaign_bounds.c',source/'presentation.c',source/'pose_bank.c',source/'camera_path.c',source/'campaign_nav.c',source/'campaign_combat.c',source/'room.c',source/'alpha_pass.c',source/'phone.c',
                source/'item_mesh.c',source/'movie.c',source/'comic.c',source/'touch.c',source/'gyro.c',source/'texture.c',source/'actor.c',source/'scene.c',source/'effects.c',source/'particles.c',source/'mission.c',source/'navigation.c',source/'localization.c','-specs=3dsx.specs',
                '-Wl,--gc-sections','-L'+str(sdk/'libctru/lib'),'-lcitro3d','-lctru','-lm','-o',elf],check=True)
smdh = build/'native.smdh'
subprocess.run([sdk/'tools/bin/smdhtool','--create','Max Payne','Native port for Nintendo 3DS',
                'Max Payne 3DS',sdk/'libctru/default_icon.png',smdh],check=True)
subprocess.run([sdk/'tools/bin/3dsxtool',elf,elf.with_suffix('.3dsx'),'--smdh='+str(smdh),
                '--romfs='+str(assets)],check=True)
