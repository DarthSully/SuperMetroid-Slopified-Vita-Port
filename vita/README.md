# Super Metroid Recomp — PS Vita port

A Vita build of SuperMetroidRecomp. It reuses the Android path: SDL2 presents
through the Vita's GXM renderer, the game runs on the pthread fiber backend,
and the ImGui launcher is left out (the game boots straight from your ROM).

## Build (Linux or WSL)

```sh
bash vita/build_vita.sh "/path/to/Super Metroid (JU) [!].smc"
```

The script verifies the ROM, generates `src/gen/` from it, builds with
VitaSDK and writes `build-vita/SuperMetroidRecomp.vpk`. If VitaSDK or Rust is
missing it prints the one-time install steps.

The VPK contains code generated from your ROM: keep it to yourself.

## Install and play

1. Install the VPK with VitaShell.
2. Copy your ROM into `ux0:data/SuperMetroidRecomp/`. Any single `*.sfc` or
   `*.smc` there is used; the game still checks it is the supported dump.
3. Launch **Super Metroid Recomp** from LiveArea.

| Input | Action |
| --- | --- |
| **Double-tap the left half of the screen** | 16:9 widescreen on/off (saved) |
| Cross / Circle / Square / Triangle | SNES B / A / Y / X |
| L / R, Select / Start | SNES L / R, Select / Start |
| Select + R | Save-state slot browser |

Widescreen widens the room view with the HUD anchored to the edges (energy
left, minimap right). In stock mode the picture is 4:3, pillarboxed.

Settings live in `ux0:data/SuperMetroidRecomp/config.ini` (seeded on first
launch) and `sm-video.ini`; saves in `saves/`. A crash leaves
`last_run_report.json` there.

## What changed for the Vita

- `snesrecomp/` (framework): Vita joins Android on the SDL_Renderer path and
  the pthread fiber backend; paths anchor to `ux0:data/<Title>/`; no process
  relaunch; the desktop file picker (needs `popen`, which Vita newlib lacks)
  is replaced by a message.
- Game side: `src/sm_vita.c` (444 MHz clocks, ROM lookup, first-launch config,
  the double-tap gesture, applied at a frame boundary), a `VITA` branch in
  `CMakeLists.txt` with VPK packaging, `vita/config.ini`, LiveArea art
  (`vita/make_livearea.py`, original artwork).
- Test: `tests/sm_vita_gesture_test.c` (runs in desktop `ctest`).

## Known limits

- Untested on hardware so far. Expect performance to be the open question:
  the PPU, SPC700 and DSP are still emulated every frame on a 444 MHz A9.
  Widescreen draws about a third more pixels than stock.
- Rewind is off (needs R3; costs RAM). Screenshots and GLSL shaders are
  desktop-GL only.
- Save states and run-ahead work in the default (LLE) execution mode. The
  optional HLE mode runs the game on a fiber, which the Vita's pthread
  backend cannot snapshot, so states are refused there (same as Android).
