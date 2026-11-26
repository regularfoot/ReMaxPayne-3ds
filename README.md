# ReMaxPayne-3ds

Version **0.1.0** is a fan-made native Max Payne port for Nintendo 3DS. It targets both Old 3DS and New 3DS and runs through the Homebrew Launcher. The port uses the top screen for gameplay and the touch screen for controls, objectives, and menus.

You need your own compatible Xbox PAL copy to build the campaign. The repository contains the port source and conversion tools. Prepare the game data on your computer from that copy. The full 3DSX embeds the prepared data, so keep the generated build local.

## Build

Install devkitARM, libctru, Citro3D, FFmpeg, liblz4, and Python with Pillow and NumPy. Add the source directories of [dade](https://github.com/Tatsh/dade/tree/8fcd00aa194cfdbc1cee27fdf94c29502b012290) and [max_payne_sdk](https://github.com/m0nstr0/max_payne_ldb_importer/tree/708a64a3ff81c7ed645549ffcf3f451b91c5ba3d) to `PYTHONPATH`. Set `DEVKITPRO` to your SDK directory. On systems without a supported system font, set `MP_UI_FONT` to a TrueType font with Latin and Cyrillic glyphs.

Extract your disc image so that `game/default.xbe` and the `game/english/` data directory are present. The Xbox transition table packer checks for the PAL executable revision identified by SHA-256 `212c47d25f96b68996d085abf93cb43464b8cf34f156081b4919866a2be74102`.

```sh
python3 scripts/pack_native.py game build/native-assets
make campaign-assets
make native-full
make test
```

The campaign build writes `dist/native-full/3ds/maxpayne/maxpayne.3dsx`. Copy it to `3ds/maxpayne/maxpayne.3dsx` on your SD card. The game writes saves and settings beside the executable. Set `GAME_DISC` if you extracted the game to a different directory, and `ASSET_PYTHON` if your converters use a separate Python environment.

## Controls

| Action | Control |
| --- | --- |
| Move | Circle Pad |
| Look | C-Stick on New 3DS, D-Pad, or drag on the touch screen |
| Interact | A |
| Fire | R |
| Jump or shoot-dodge while moving | L |
| Reload | X |
| Change weapon | Y or the weapon control on the touch screen |
| Painkiller | SELECT |
| Pause | START |

Graphic-novel narration and artwork come from the local disc. For the supported Russian pages, large-text mode displays enlarged dialogue alongside the corresponding artwork. **A** advances the text or page. **D-Pad Left and Right** move within a large-text page. **X** switches between the large-text, original, and zoomed views where available. The current interface is Russian.

Max Payne and Nintendo 3DS are trademarks of their respective owners. This project is independent of the original publishers.
