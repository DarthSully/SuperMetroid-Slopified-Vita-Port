#pragma once
/*
 * sm_vita.h — PS Vita platform glue for Super Metroid.
 *
 * Compiled only into the Vita build (see CMakeLists.txt). Everything here is
 * presentation or process setup; nothing touches the guest.
 *
 *   - SmVita_EarlyInit():  CPU/GPU clocks and SDL hints, before the host
 *                          initialises SDL.
 *   - SmVita_ResolveRom(): find a ROM in the data folder when the canonical
 *                          file name is not there (e.g. "sm.smc").
 *   - SmVita_InstallGestures() / SmVita_ApplyPendingGestures():
 *                          double-tap the LEFT half of the front touch screen
 *                          to toggle the 16:9 widescreen renderer.
 */
#include <stdbool.h>
#include <stddef.h>

#include "sm_video.h"

/* Writable home for config.ini, saves/, sm-video.ini and the ROM. */
#ifndef SNESRECOMP_VITA_DATA_DIR
#define SNESRECOMP_VITA_DATA_DIR "ux0:data/SuperMetroidRecomp/"
#endif

void SmVita_EarlyInit(void);

/* Hang watchdog (sm_vita_watchdog.c): call once the log is open. The main
 * thread marks its stage and each finished frame; on a stall the watchdog
 * writes hang_report.txt and faults on purpose for a psp2core dump. */
void SmVita_WatchdogStart(void);
#ifdef __vita__
extern const char *volatile g_sm_wd_stage;
extern volatile unsigned g_sm_wd_frames;
#define SM_WD_STAGE(s) (g_sm_wd_stage = (s))
#define SM_WD_FRAME_DONE() (g_sm_wd_frames++)
#else
#define SM_WD_STAGE(s) ((void)0)
#define SM_WD_FRAME_DONE() ((void)0)
#endif

/* Creates the data folder and saves/, and seeds config.ini from app0: on
 * first launch. Call before the host reads its config. */
void SmVita_SeedDataDir(void);

/* Fills `out` with a ROM path and returns true when the canonical
 * `rom_file` is missing from the data folder but exactly one other
 * *.sfc / *.smc file is there. The host still verifies its SHA-256. */
bool SmVita_ResolveRom(const char *rom_file, char *out, size_t cap);

/* Registers an SDL event watch for the front touch panel. Call once SDL is
 * initialised (from the host's after_config hook). Idempotent. */
void SmVita_InstallGestures(SmVideoSettings *video, const char *video_ini);

/* Applies a toggle requested by the gesture. Called at a frame boundary
 * (before_run_frame), so the renderer never changes width mid-frame.
 * Returns true when the widescreen state changed. */
bool SmVita_ApplyPendingGestures(void);
/* Once per frame before the game runs: the right-half double tap's infinite
 * energy, and Start skipping the new-game intro. Edits the game's RAM. */
#include <stdint.h>
void SmVita_FrameHooks(uint8_t *ram);
void SmVita_RequestInfiniteEnergyToggle(void);

/* Pure gesture state machine, exposed for the host-side unit test.
 * x is normalised 0..1 across the front panel, t in milliseconds.
 * Returns true when this touch-down completes a left-side double tap. */
typedef struct SmVitaTapState {
  bool have_first;
  unsigned first_ms;
  float first_x, first_y;
} SmVitaTapState;
bool SmVita_TapDown(SmVitaTapState *s, float x, float y, unsigned t_ms, int fingers_down);
/* Same machine for either half (SmVita_TapDown is the left half). */
bool SmVita_TapDownSide(SmVitaTapState *s, float x, float y, unsigned t_ms,
                        int fingers_down, bool right_half);
