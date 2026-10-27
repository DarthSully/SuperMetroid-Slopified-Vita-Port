# Changelog

PS Vita port of [mstan's SuperMetroidRecomp](https://github.com/mstan/SuperMetroidRecomp).
All credit for the recompilation, the framework and the widescreen renderer goes to the
original authors — see [CREDITS.md](CREDITS.md).

## r21 — 2026-10-27

Download: [downloads/SuperMetroid-Vita-r21.vpk](downloads/SuperMetroid-Vita-r21.vpk)

### Performance
- **Door transitions are much faster.** The game decompresses each room's graphics
  through an entry point (`$80:B0FF`) that takes its source address from the bytes after
  the call. That entry was never declared to the recompiler, so all 63 call sites ran the
  whole decompression in the slow interpreter — roughly 1.3 s of CPU per door on the
  Vita. It is now native code. Over 43 door transitions on the desktop build, interpreted
  work dropped by 82%. This removes most of the door-transition slowdown and the audio
  cuts that came with it.
- `LoadLevelScrollAndCre` (room loading) is now native code too; its starting register
  widths were declared wrong, so the recompiler had always skipped it.
- About 100 more routines compile as a side effect: the recompiler had been misreading
  the argument bytes after those 63 calls as instructions.
- **Full speed again with current VitaSDK.** Newer VitaSDK builds of SDL2 default to an
  OpenGL ES renderer (through vitaGL) that cost about 12 ms per frame and several seconds
  at start-up. The port now asks SDL2 for its native GXM renderer. The log lists the
  available renderers and the one in use.

### Fixes
- **16:9: junk beside the pause map.** While the map faded out after unpausing, the
  widescreen renderer treated the frame as gameplay and drew room tiles in the side
  areas. Fixed (the 4:3 view was never affected).

### Build
- Links with current VitaSDK: `vitaGL`, `vitashark`, `SceShaccCgExt`, `taihen_stub`,
  `mathneon`, `SceShaccCg_stub` and `stdc++` are named explicitly, in dependency order.
- Desktop builds: `SM_PIPELINE=1` reproduces the Vita's pipelined compose thread, for
  testing the Vita rendering path on a PC.

### Known issues
- The room-object handler (`PlmHandler_Async`) still runs in the interpreter on purpose:
  item and save message boxes pause inside it. Rooms with many objects can still dip
  below 60 fps.
- In 16:9, the side areas show enemies the game keeps in memory. Enemies that only spawn
  or move once they enter the original 4:3 view (for example Zebs coming out of pipes)
  appear when they reach it — that is how the game itself works.
- The door-transition speed-up was measured on the desktop build; the Vita gain is
  expected to be larger (about 3 µs per interpreted instruction there).
- A full playthrough (Mother Brain, escape, credits) has not been verified on the Vita.

## r18 — 2026-10-20

First public release.
- Door-transition freeze fixed (an enemy routine returning through an uncompiled
  function leaked stack bytes).
- Saves persist when the game is closed with the PS button (battery SRAM is written
  shortly after every in-game save).
- Skipping the new-game intro no longer gives 900 missiles or changes the controls.
- The game's NMI handler, sound queue and Samus routines run as native code.
- Faster 16:9 drawing (vectorized layer blending) and the audio thread at idle priority
  across cores 0–2.
- Runs at 444 MHz.
