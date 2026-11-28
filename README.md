# ReMaxPayne-3ds

![Max Payne fires two Berettas during a shoot-dodge on the upper screen of a Nintendo 3DS, with the port's touch-screen HUD below.](docs/remaxpayne-3ds-poster.png)

**ReMaxPayne-3ds** is a fan-made native Max Payne port that works on both **Old 3DS and New 3DS** through the Homebrew Launcher. Version **0.2.0** is an early-access source release for testing on real hardware. See the [changelog](CHANGELOG.md) for the new touch interface, selectable voice audio, graphics fixes, and gameplay improvements.

This project is intended for personal use with data from your own copy of Max Payne for Xbox. Build locally from your supported PAL or NTSC disc image. Keep the resulting game data and 3DSX for your own use.

## How it plays

The upper screen shows the third-person campaign, with optional stereoscopic 3D controlled by the console's slider. Move with the Circle Pad, slow down a firefight with Bullet Time, and press **L** while moving to shoot-dodge. The lower screen keeps health, ammunition, the Bullet Time meter, and your objective in view. Tap the weapon panel at the upper right to choose a weapon. The lower-left buttons control crouching and Bullet Time. The full-width objective strip supports two lines, and the enlarged health silhouette keeps its original proportions. Max's journal is available from the pause menu.

On **Old 3DS**, aim with the D-Pad or drag the stylus across the touch screen. Tap the Bullet Time meter to activate slow motion. On **New 3DS**, the C-Stick adds camera control and **ZL** toggles Bullet Time. The D-Pad and stylus also work on New 3DS.

From the main menu, choose **New Game** to start the campaign or **Tutorial** to try the controls. The campaign supports autosaves and manual save slots. In the pause menu, **X** quicksaves and **Y** quickloads.

### Graphics and aiming settings

Open **Options → Graphics** to select environment and sky texture limits independently. The choices use the mip levels available in your locally converted game data and apply on the next level load.

| Texture limit | Old 3DS | New 3DS |
| --- | --- | --- |
| Environment | 128 | Auto (512), 128, 256, 512 |
| Sky | Auto (128), 128, 256 | Auto (512), 128, 256, 512, 1024 |

Under **Options → Aim**, stick and touch aiming have separate horizontal and vertical sensitivity from **25% to 250%**, in steps of 5%. Gyroscope sensitivity remains independently adjustable on each axis from **25% to 200%**. Hold left or right to adjust repeatedly, use the touch-screen minus and plus buttons, or drag the slider. Existing sensitivity settings migrate when the updated configuration is first loaded.

### Touch menus and voice audio

The lower-screen menus use large touch tiles and a consistent **Back** button in the upper-right corner. Gameplay options and save slots show four compact rows per page. Loading progress, cutscene captions, notices, and journal text sit near the darker lower edge. Menu edges, hit areas, and lines use whole pixels on the native 320×240 display. Physical button navigation remains available.

Under **Options → Sound → Voice**, choose Russian or English when both audio sets were prepared from your own discs. Spoken lines and graphic-novel narration follow the selection; the interface, objectives, and original comic lettering keep the language of the base build. A line already playing finishes normally; subsequent sounds and the next comic page use the selected voice. Optional disc videos use an alternate version when supplied by the second disc. A single-language build keeps its original audio.

### Default campaign controls

| Action | Control |
| --- | --- |
| Move | Circle Pad |
| Aim and look | D-Pad, stylus drag, or C-Stick on New 3DS |
| Interact | A |
| Jump | B |
| Shoot-dodge | L while moving, with Bullet Time charge and a compatible weapon |
| Toggle Bullet Time | Touch-screen meter on Old 3DS or ZL on New 3DS |
| Fire | R, or ZR on New 3DS |
| Reload | X |
| Change weapon | Y or the touch-screen weapon picker |
| Crouch or stand | Touch-screen crouch button |
| Painkiller | SELECT |
| Pause | START |

### Graphic novel

Graphic-novel pages, lettering, and narration come from your Xbox copy. Press **A** to advance a page. **X** zooms into the original page and returns to the full view. In the zoomed view, use the **D-Pad** to pan. The supported PAL disc defaults to a Russian interface. The supported English NTSC disc defaults to English.

## Build and install

Install devkitARM, libctru, Citro3D, FFmpeg, liblz4, and Python 3.11 or newer with Pillow, NumPy, and Unicorn. Add the source directories of [dade](https://github.com/Tatsh/dade/tree/8fcd00aa194cfdbc1cee27fdf94c29502b012290) and [max_payne_sdk](https://github.com/m0nstr0/max_payne_ldb_importer/tree/708a64a3ff81c7ed645549ffcf3f451b91c5ba3d) to `PYTHONPATH`. Set `DEVKITPRO` to your SDK directory. On systems without a supported system font, set `MP_UI_FONT` to a TrueType font with Latin and Cyrillic glyphs.

The same commands work with both supported revisions. The extractor preserves the disc's original layout. The conversion tools find the RAS archives in the disc root or its `english/` directory. The animation packer reads the transition rules from the disc's `default.xbe` with Unicorn during asset preparation.

| Disc revision | `default.xbe` SHA-256 |
| --- | --- |
| Xbox PAL | `212c47d25f96b68996d085abf93cb43464b8cf34f156081b4919866a2be74102` |
| Xbox NTSC-U | `3e95f75a29bfd37b2d17d2ee8490fa2c4c516faf8b37a6b7ef5860fa83375b90` |

```sh
python3 scripts/extract_xiso.py /path/to/your-xbox-disc.iso game
python3 scripts/pack_native.py game build/native-assets
make campaign-assets
make native-full
make test
```

The extractor also accepts an MDF containing an XDVDFS image, using the same command. An already extracted disc can be placed in `game/` directly. Disc videos are included when the source contains the corresponding Bink files.

Use a separate checkout for each disc to keep the generated files separate. To choose the interface language explicitly, export `MP_LANG=en` or `MP_LANG=ru` before packing the assets and running Make. Objective text, narration, and comic pages use the data from your disc.

The campaign build writes `dist/native-full/3ds/maxpayne/maxpayne.3dsx`. Copy it to `3ds/maxpayne/maxpayne.3dsx` on your SD card and run it through the Homebrew Launcher. The game writes saves and settings beside the executable. Set `GAME_DISC` if you extracted the game to a different directory, and `ASSET_PYTHON` if your converters use a separate Python environment. The full 3DSX embeds your locally prepared game data.

When updating from 0.1.0, rerun asset conversion as well as the native build to regenerate weapon draw runs, particle atlases, and sky mip levels. Updating an existing 0.1.1 data set for the new interface only requires recompiling; adding a second voice language also requires the optional conversion below.

### Optional second voice language

To build a Russian interface and Russian comic pages with selectable English voices, first extract your Russian disc into `game/` and your English disc into a separate directory. Prepare the native assets from the base disc, then run:

```sh
export MP_LANG=ru
python3 scripts/pack_native.py game build/native-assets
make campaign-assets VOICE_DISC=/path/to/your-english-disc VOICE_LANGUAGE=en
make native-full
```

If your disc has English objective strings but you use a Russian interface, an optional private translation can be supplied with `MP_OBJECTIVE_TRANSLATION=/path/to/your-objectives.tsv` during `campaign-assets`. Use UTF-8 lines containing a disc string key, a tab, and the translated text. The packer validates keys against your disc; translation tables are not included here.

The second source must also be your own extracted Xbox copy. The voice packer converts its sound definitions and comic narration locally, adds language-marked audio variants, and deduplicates identical PCM data. No voice recordings, game tables, comics, or converted game data are supplied by this repository. For an English base with optional Russian speech, use `MP_LANG=en` and `VOICE_LANGUAGE=ru` with your Russian disc as `VOICE_DISC`. Re-run asset preparation when adding or changing a second voice source.

## Testing status

The release includes fixes for the reported transparency and flicker regressions: hardware-tested **Alpha Order**, corrected additive-particle fog blending, opaque explosion-square artifacts, shotgun mesh overlaps, and glass/splinter alpha masks. Campaign sky rendering retains its normal fog. Weapon smoke follows the authored muzzle sockets, including both Berettas.

The preceding combined gameplay fixes passed host checks, scripted progression through all 67 campaign transitions on PAL and NTSC data, and targeted Old/New profiles in Azahar. The new touch interface passed native hit-area, page navigation, settings migration, and fixed weapon-icon checks. Old 3DS emulator testing covered touch settings and loading a campaign save into the new HUD. Selectable voice audio has separate conversion and runtime checks.

Version 0.2.0 remains an early-access test build. Physical-console testing of the complete new release is still needed. Some hardware-specific graphical artifacts and Xbox cutscene letterboxing remain under investigation; further real-hardware reports are welcome.

## Credits

Thank you to Remedy Entertainment and the original Max Payne development team for creating the game.

The first 3DS prototype started from phobos665's Xbox recompilation project. That work used DirectXbox-Recomp and helped get the project running. The current 3DS release uses a native engine. [payne-recomp](https://github.com/phobos665/payne-recomp) · [DirectXbox-Recomp](https://github.com/phobos665/DirectXbox-Recomp)

Max Payne and Nintendo 3DS are trademarks of their respective owners. This project is independent of the original publishers.
