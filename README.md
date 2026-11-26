# ReMaxPayne-3ds

![Max Payne fires two Berettas during a shoot-dodge on the upper screen of a Nintendo 3DS, with the port's touch-screen HUD below.](docs/remaxpayne-3ds-poster.png)

**ReMaxPayne-3ds** is a fan-made native Max Payne port that works on both **Old 3DS and New 3DS** through the Homebrew Launcher. Version **0.1.0** is an early-access build for testing on real hardware.

The repository contains the port source and conversion tools. Build the campaign from your own supported Xbox PAL or NTSC disc image.

## How it plays

The upper screen shows the third-person campaign, with optional stereoscopic 3D controlled by the console's slider. Move with the Circle Pad, slow down a firefight with Bullet Time, and press **L** while moving to shoot-dodge. The lower screen keeps health, ammunition, the Bullet Time meter, and your objective in view. Tap it to pick a weapon, crouch, or open Max's journal.

On **Old 3DS**, aim with the D-Pad or drag the stylus across the touch screen. Tap the Bullet Time meter to activate slow motion. On **New 3DS**, the C-Stick adds camera control and **ZL** toggles Bullet Time. The D-Pad and stylus also work on New 3DS.

From the main menu, choose **New Game** to start the campaign or **Tutorial** to try the controls. The campaign supports autosaves and manual save slots. In the pause menu, **X** quicksaves and **Y** quickloads.

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

Install devkitARM, libctru, Citro3D, FFmpeg, liblz4, and Python with Pillow and NumPy. Add the source directories of [dade](https://github.com/Tatsh/dade/tree/8fcd00aa194cfdbc1cee27fdf94c29502b012290) and [max_payne_sdk](https://github.com/m0nstr0/max_payne_ldb_importer/tree/708a64a3ff81c7ed645549ffcf3f451b91c5ba3d) to `PYTHONPATH`. Set `DEVKITPRO` to your SDK directory. On systems without a supported system font, set `MP_UI_FONT` to a TrueType font with Latin and Cyrillic glyphs.

The same commands work with both supported revisions. The extractor preserves the disc's original layout. The conversion tools find the RAS archives either in the disc root or in its `english/` directory.

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

Use a separate checkout for each disc to keep the generated files separate. To choose the interface and objective text language explicitly, export `MP_LANG=en` or `MP_LANG=ru` before packing the assets and running Make. The disc supplies the original narration and comic artwork.

The campaign build writes `dist/native-full/3ds/maxpayne/maxpayne.3dsx`. Copy it to `3ds/maxpayne/maxpayne.3dsx` on your SD card and run it through the Homebrew Launcher. The game writes saves and settings beside the executable. Set `GAME_DISC` if you extracted the game to a different directory, and `ASSET_PYTHON` if your converters use a separate Python environment. The full 3DSX embeds your prepared game data and is intended for local use.

Max Payne and Nintendo 3DS are trademarks of their respective owners. This project is independent of the original publishers.
