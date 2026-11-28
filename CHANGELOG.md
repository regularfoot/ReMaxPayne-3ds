# Changelog

## 0.1.1 — 2026-11-28

Early-access source update for Old 3DS and New 3DS.

### Graphics and effects

- Use the hardware-tested Alpha Order transparency path, drawing decals first and preserving submission order for the remaining transparent materials.
- Correct additive-particle fog blending that exposed rectangular sprite backgrounds, and calculate fog using the active projection's near plane.
- Cull proven overlapping opposite faces in weapon meshes to address partially transparent shotguns while preserving thin surfaces.
- Retain sharper alpha masks for glass and splinters, with lower-resolution mip levels for Old 3DS.
- Emit weapon smoke and flashes from the corresponding weapon sockets on authored animation cues, including both Berettas.
- Add independent environment and sky texture settings. New 3DS supports environment limits through 512 and sky limits through 1024. Old 3DS uses 128 for the environment and offers sky limits through 256. Changes apply on the next level load.

### Performance

- Batch compatible particle geometry and transparent draws while preserving the tested draw order.
- Reuse GPU state within each eye, cache shared particle drag calculations, and reduce per-vertex lighting work in busy scenes.
- Reduce pause-menu redraw work during settings adjustment and save configuration changes together when leaving the menu.

### Gameplay and collision

- Detect train contact in an adjoining tunnel before the moving train has registered with the receiving room, allowing the original fatal collision script to run.
- Detect contact from the falling lift hatch above the player and dispatch its authored damage event.
- Reserve projectile capacity for live gameplay objects so bursts of cosmetic debris and decals cannot consume every slot needed by grenades, damage volumes, and pickups.

### Controls, saves, and audio

- Add independent horizontal and vertical stick and touch sensitivity from 25% to 250%, in steps of 5%. Improve held-button adjustment and migrate previous configuration values.
- Write updated settings through a temporary file and recover an interrupted configuration write.
- Correct held ZR firing and the weapon-selection labels for alternate X/Y control layouts.
- Show Tutorial and Last Challenge autosaves in the load menu. Validate the selected save before leaving the menu and fall back to the older valid copy when the latest payload is damaged.
- Preserve adaptive retry death counts when restarting a level without a checkpoint.
- Restore the previous scoped view immediately after pause and resolve the orbit camera's room through the portals it crosses.
- Exclude HOME-menu and sleep time from active gameplay timers.
- Continue positional audio across cached and streamed sample boundaries when a sound becomes audible partway through playback.

### Verification and remaining work

- Passed host tests and scripted progression through all 67 campaign transitions with both PAL and NTSC data.
- Verified original lighting across 69 maps, 1,622 portals, and the animation and weapon-effect data against the supplied Xbox disc.
- Checked the combined build in Azahar on Old and New 3DS profiles, including graphics settings, stereoscopic intro rendering, quicksave loading, and scoped pause/resume. Train tests also covered the first station, an adjoining tunnel, a later train, and a safe platform position.
- The final combined 0.1.1 build still needs physical-console verification. Occasional graphical artifacts and Xbox cutscene letterboxing remain open work.

## 0.1.0

Initial early-access source release for Old 3DS and New 3DS. Includes the native campaign, stereoscopic rendering, touch-screen HUD, original graphic novels, saves, and build tools for supported PAL and NTSC Xbox discs.
