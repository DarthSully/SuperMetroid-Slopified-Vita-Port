# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

The per-game side of a static recompilation of Super Metroid (SNES) into native C, built on the `snesrecomp` framework (game #4, after Mega Man X, Super Mario World, and A Link to the Past). LLE-first: proven-hot 65816 functions are statically translated to C; anything absent or rejected executes the original ROM through the embedded LLE interpreter. Only the CPU is recompiled — PPU, APU/SPC700, DMA/HDMA, and register I/O run through the SNES emulation in `snesrecomp/runner/src/snes/`.

`snesrecomp` (framework: recompiler, runner, tools) and `recomp-ui` (shared Dear ImGui launcher) are git submodules. The ROM (`Super Metroid (Japan, USA) (En,Ja).sfc` at the repo root) is never committed — `*.sfc` is gitignored.

## Hard rules

- **Never edit `src/gen/`** (generated, gitignored, ~93 MB). Fix the generator (`snesrecomp/recompiler/`), the cfg directives in `recomp/*.cfg`, or the C runtime in `src/`, then regenerate. No stubs or dummy returns.
- **Always full regen** — never a partial per-bank regen (`--banks`); partial output breaks cross-bank M/X variant references. After cfg changes, `recomp/funcs.h` must be re-synced (regen.sh does this).
- **Owner-gated (do not do without explicit decision):** merging investigation branches to main, releasing, reconciling the multi-tier engine branches.
- Root-level `_`-prefixed files and `*.jsonl` traces are per-session scratch (gitignored) — the expected place for throwaway diff/inspection scripts.

## Commands

```sh
# One-time: clone the snesrev/sm decomp (commit pinned in refs/snesrev-sm.pin)
# and ingest its symbols into recomp/*.cfg + recomp/sm_decomp_symbols.json
git clone --depth 1 https://github.com/snesrev/sm.git refs/snesrev-sm
python tools/ingest_sm_decomp.py

# Regenerate src/gen/ (deterministic; needs the ROM at repo root).
# --strict-idempotent regenerates twice and requires byte-identical output.
# --no-tests skips the framework test suite. Native analyzer needs rustup;
# SNESRECOMP_ANALYSIS_BACKEND=python selects the slower reference path.
./tools/regen.sh --strict-idempotent

# Configure + build (SDL2 + OpenGL required; fails if src/gen/ is empty)
cmake -G Ninja -B build -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc
cmake --build build -j 8

# Run (binary name has .exe even under MinGW paths; drop it on POSIX builds)
./build/SuperMetroidSNESRecomp "Super Metroid (Japan, USA) (En,Ja).sfc"

# Android (arm64 APK; needs Android SDK/NDK + JDK 17; see android/)
bash android/fetch_sdl.sh    # one-time: pin SDL2 source + Java glue
( cd android && ./gradlew assembleDebug )
# Then push rom.sfc, rom.cfg (absolute ROM path), and a config.ini with
# SkipLauncher=1 into /sdcard/Android/data/com.snesrecomp.supermetroid/files/
```

### Tests

```sh
ctest --test-dir build                       # all game-side tests
ctest --test-dir build -R sm_widescreen      # single test by name regex
python3 snesrecomp/tests/v2/run_tests.py     # framework (recompiler) suite
```

Game-side ctest targets: `sm_display_geometry`, `ppu_widescreen_windows`, `sm_widescreen_helpers` (plain C binaries in `tests/`), and `sm_widescreen_visual_smoke` (Python; skips with code 77 when artifacts are missing).

## Architecture

### Generation pipeline

`recomp/bankXX.cfg` files carry the per-bank directives and correctness exclusions.
`tools/regen.sh` delegates to `tools/regen.py`, which verifies the ROM and
builds `profiles/release.json` using the pinned native analyzer. CFG declarations
seed AOT coverage; unproved code retains the portable ROM interpreter fallback.
All release platforms compile the same generated program. Setup hosts and the
in-launcher Generate & rebuild wizard are retired. Generated C stays private
and is never committed or publicly packaged. See `docs/precompiled-releases.md`.


### Single-fiber frame model (src/sm_rtl.c)

Unlike MMX's multi-slot scheduler, Super Metroid runs as one linear program: reset falls into the main game loop (`$82:8948`), which calls `WaitForNMI` (`$80:8338`) from arbitrary call depth. The whole game therefore runs on ONE host fiber. `WaitForNMI` is HLE-replaced (`bank00.cfg` → body in `src/gen_stubs.c`) with a yield to the host; the host emulates the frame (NMI + PPU), then resumes the fiber, preserving the full C call stack. Architectural register state is saved/restored around each fiber switch. The Win32 Fiber API is shimmed onto ucontext (macOS/Linux) and onto pthread+condvar handoffs on Android (bionic has no makecontext/swapcontext) by the framework's `desktop/fiber_compat.c`, wired in with `snesrecomp_target_fiber_compat()`. The Android backend originated here and was moved up in 2026-09.

Other game-side runtime: `sm_cpu_infra.c` (game registration consumed by the runner), `sm_spc_player.c` (audio), `sm_post_mortem.c` (the `sm{}` crash-report section, registered through the framework hook), `main.c`, `sm_display.c`/`sm_video.c` (presentation geometry, aspect/HUD policy, the simulation/presentation clock).

### What this port does NOT own

Everything generic now comes from the framework through `snesrecomp_target_*`
helpers in `CMakeLists.txt`, rather than from a private fork in `src/`:

| Was (deleted 2026-09) | Now |
|---|---|
| `src/config.c`, `src/config.h` | `snesrecomp_target_mmx_config()` |
| `src/post_mortem.c`, `.h` | `snesrecomp_target_post_mortem(TIER2)` |
| `src/glsl_shader.c`, `.h` | via `snesrecomp_target_opengl()` |
| `src/opengl.c`, `third_party/gl_core`, `third_party/stb` | `snesrecomp_target_opengl()` |
| `src/fiber_compat.c`, `.h` | `snesrecomp_target_fiber_compat()` |
| inline SHA-256/CRC32 in `main.c`, `src/codegen_setup.c` | `snesrecomp_rom_identity()` → `snesrecomp_rom_identity.h`, from `rom_identity.txt` |

The forks were roughly 2,400 lines, some identical to the framework's and some
ahead of it. What this port was ahead on was moved UP (the Android fiber
backend, the post-mortem's DB tripwire and stack/dispatch/PPU-DMA dumps, the
GL presenter itself) rather than deleted. **Do not re-fork these into `src/`**;
fix them in `snesrecomp/` so every port inherits it.

### In-game overlays

The save-state slot browser (Select+R on the pad, or `[KeyMap] SaveStateMenu`,
F11) and the rewind filmstrip (`[Controller] RewindGesture`, Select+R3 by
default, or `[KeyMap] Rewind`, Shift+F12 — plain F12 became `Screenshot`
when the shader/screenshot work landed) are framework modules driven by the
framework host. The guest is frozen while a panel is up; the host presents the
last frame with the panel composited at 512x448 and never runs guest code from
a modal loop. Two bugs this port shipped and that now have tests: the modal
pumps dropped controller events, so a pad player saw a frozen game and a panel
that ignored them (`SNESRECOMP_OVERLAY_SELFTEST_PAD=<frame>` drives both panels
with a virtual gamepad through real SDL events); and the button that closes a
panel leaked into the guest for a frame (held-at-close buttons are masked
until released). A traced run with either self-test armed must match one
without it.

### Widescreen

This worktree replaces the old generated guest widescreen overrides with a
read-only custom renderer. Do not reintroduce the deleted override script or
AOT roots. The guest stays at 256 pixels and `g_ws_active` remains false.
See `docs/custom-renderer.md` for current implementation status and validation
gaps. Full regeneration remains mandatory after changing generation inputs.

### Debugging workflow

- `python3 snesrecomp/tools/check_link_closure.py src/gen` reports whether a generated tree will link — every `<Name>_M<m>X<x>` it calls is defined — in seconds, without building. `regen.sh` runs this automatically before publishing, so a fresh regen needs it only for a tree generation does not own: one a player produced through the launcher, or one attached to a bug report.
- `build/last_run_report.json` is the always-on post-mortem written on crash/exit: CPU state, recomp stack, abandons, tier2 coverage, dispatch-log ring, DB/PB ring, and an SM-specific `sm{}` section (game_state, enemy slots). It is the primary crash-diagnosis artifact — the TCP debug server is not usable for SM.
- Differential oracle: `snesrecomp/tools/snesref` (headless snes9x libretro, per-frame WRAM trace via `SNESREF_FRAMES`/`SNESREF_TRACE_FILE`, frame dumps via `SNESREF_FRAME_DUMP_DIR`); build it with `tools/snesref/build.sh` on Linux and point it at any libretro SNES core. Recomp side traces via `SNESRECOMP_WRAM_TRACE_FILE`. Whole-WRAM traces don't align frame-for-frame — diff a single semantic variable's timeline instead (e.g. game_state `$0998`).
- The `EnableSnes9xOracle` runtime option only makes sense from boot (it can't follow save-state loads); see the warning in `config.ini`.
- Env-gated probes: `SNESRECOMP_SBOUND=lo-hi` (S/DB at every block in a PC range), `SNESRECOMP_IBRWATCH=lo-hi` (interp-bridge per-step trace), `SM_RASTER_PROBE=<path>` (the shape of each rendered frame: the HUD/room IRQ split's schedule, the layers and BG3 tilemap it left enabled, the HDMA mask armed, and the per-line BG mode; written on change plus any frame differing from BOTH neighbours).
- One-frame glitches: `SNESRECOMP_PRESENT_LOG=<csv>` logs every PRESENT (present, frame, interpolation weight, CRC32, mean luma) so a session can be scanned for the one present that is wrong; `SNESRECOMP_SCREENSHOT_DIR=<dir>` with `_FROM`/`_TO` then dumps that range as `present_NNNNNN.ppm`. Per present, not per simulated frame: with the enhanced renderer the host presents at the display rate and can show one simulated frame twice with different weights.
- Headless repro: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SNESRECOMP_RUN_FRAMES=N ./build/SuperMetroidSNESRecomp --script <file> "<absolute rom path>"`. The host anchors cwd to the executable's directory, so the ROM path must be absolute; `saves/`, `config.ini` and the report land beside the binary.

`DEVELOPMENT.md` is the durable in-repo dev log (root-cause writeups, current blockers, open items) — read it for the current state of bring-up work and append milestone writeups there.

## Configuration

`config.ini` (tracked) holds this port's tuned defaults: hotkeys, gamepad maps, renderer/audio options. The host reads the copy **beside the executable** (`build-release/config.ini`), which the build seeds from the tracked file once (`snesrecomp_target_config_seed`) and which is the player's afterwards: the launcher and the host write settings back into it. A change to the tracked file therefore reaches an existing build tree only by deleting that copy, or through the framework's key migrations (a former generated default is rewritten to the current one, once, with a breadcrumb). Per-developer overrides go in `config.local.ini` (gitignored), applied after `config.ini`. `SNESRECOMP_KEYMAP_DUMP=1` prints what the system hotkeys resolved to.
