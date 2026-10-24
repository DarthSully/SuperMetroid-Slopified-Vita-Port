/*
 * sm_vita.c — PS Vita platform glue for Super Metroid. See sm_vita.h.
 *
 * The gesture lives entirely on the game side: an SDL event watch sees every
 * front-panel touch the moment SDL pumps it (so even a tap shorter than a
 * frame is caught), and only records a request. The request is applied in
 * before_run_frame, between simulated frames, which is the same point the
 * desktop launcher's Mods page takes effect at.
 */
#include "sm_vita.h"

#ifndef SM_VITA_BUILD
#define SM_VITA_BUILD "dev"
#endif

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>

#include "sm_renderer.h"
#include "snes_osd.h"

/* ── Gesture state machine (platform-independent, unit-tested) ─────────── */

/* Two touch-downs on the left half, close in time and place. Generous
 * enough for a thumb, tight enough that walking left on the d-pad while
 * resting a thumb on the screen does not trigger it. */
enum {
  kDoubleTapWindowMs = 350,
};
static const float kLeftHalf = 0.5f;
static const float kMaxDrift = 0.15f;   /* normalised panel units */

bool SmVita_TapDown(SmVitaTapState *s, float x, float y, unsigned t_ms,
                    int fingers_down) {
  return SmVita_TapDownSide(s, x, y, t_ms, fingers_down, false);
}

bool SmVita_TapDownSide(SmVitaTapState *s, float x, float y, unsigned t_ms,
                        int fingers_down, bool right_half) {
  /* A second finger, or a touch on the other half, cancels a pending tap:
   * the gesture is one thumb on this side, nothing else. */
  if (fingers_down > 1 || (right_half ? x < kLeftHalf : x >= kLeftHalf)) {
    s->have_first = false;
    return false;
  }
  if (s->have_first) {
    unsigned dt = t_ms - s->first_ms;   /* unsigned: wraps safely */
    float dx = x - s->first_x, dy = y - s->first_y;
    bool near = dx * dx + dy * dy <= kMaxDrift * kMaxDrift;
    if (dt <= kDoubleTapWindowMs && near) {
      s->have_first = false;
      return true;
    }
  }
  s->have_first = true;
  s->first_ms = t_ms;
  s->first_x = x;
  s->first_y = y;
  return false;
}

#ifdef __vita__

#include <psp2/power.h>
#include <SDL.h>

static SmVideoSettings *g_video;
static const char *g_video_ini;
static SmVitaTapState g_tap, g_tap_right;
static volatile int g_toggle_requested;
static bool g_installed;

void SmVita_EarlyInit(void) {
  /* The 65816 CPU is recompiled, but the PPU, SPC700 and DSP are emulated
   * every frame: run the A9 cores at their full 444 MHz. */
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  /* The front panel is a gesture surface here, not a mouse: without this
   * SDL would also synthesise clicks from every tap. */
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
}

void SmVita_SeedDataDir(void) {
  /* sceIoMkdir is not recursive: ux0:data always exists, the title folder
   * and saves/ may not. */
  char path[512];
  snprintf(path, sizeof(path), "%s", SNESRECOMP_VITA_DATA_DIR);
  size_t n = strlen(path);
  if (n && path[n - 1] == '/') path[n - 1] = '\0';
  mkdir(path, 0777);
  snprintf(path, sizeof(path), "%ssaves", SNESRECOMP_VITA_DATA_DIR);
  mkdir(path, 0777);

  /* Nothing reads stdout/stderr on the Vita: send both to log.txt, line-
   * buffered, so the last line before a hang or crash is on disk. */
  snprintf(path, sizeof(path), "%slog.txt", SNESRECOMP_VITA_DATA_DIR);
  if (freopen(path, "w", stderr)) {
    setvbuf(stderr, NULL, _IOLBF, 0);
    stdout = stderr;
  }
  fprintf(stderr, "[vita] Super Metroid Recomp, Vita build %s\n", SM_VITA_BUILD);

  /* An empty "profile" file in the data folder: per-300-frame timing lines
   * in log.txt (snesrecomp's SNESRECOMP_PERF). */
  snprintf(path, sizeof(path), "%sprofile", SNESRECOMP_VITA_DATA_DIR);
  FILE *pf = fopen(path, "rb");
  if (pf) {
    fclose(pf);
    setenv("SNESRECOMP_PERF", "1", 0);
    fprintf(stderr, "[vita] profile: timing lines every 300 frames\n");
  }

  /* First launch: copy the Vita-tuned config.ini out of the read-only app0:
   * mount. After that the file is the player's. */
  snprintf(path, sizeof(path), "%sconfig.ini", SNESRECOMP_VITA_DATA_DIR);
  FILE *dst = fopen(path, "rb");
  if (dst) { fclose(dst); return; }
  FILE *src = fopen("app0:config.ini", "rb");
  if (!src) return;
  dst = fopen(path, "wb");
  if (dst) {
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), src)) > 0)
      fwrite(buf, 1, got, dst);
    fclose(dst);
  }
  fclose(src);
}

static bool HasRomExtension(const char *name) {
  size_t n = strlen(name);
  return n > 4 && (!strcasecmp(name + n - 4, ".sfc") ||
                   !strcasecmp(name + n - 4, ".smc"));
}

bool SmVita_ResolveRom(const char *rom_file, char *out, size_t cap) {
  char canonical[512];
  snprintf(canonical, sizeof(canonical), "%s%s", SNESRECOMP_VITA_DATA_DIR,
           rom_file ? rom_file : "");
  FILE *f = rom_file ? fopen(canonical, "rb") : NULL;
  if (f) {           /* the host finds this one on its own */
    fclose(f);
    return false;
  }
  DIR *dir = opendir(SNESRECOMP_VITA_DATA_DIR);
  if (!dir) return false;
  int found = 0;
  struct dirent *e;
  while ((e = readdir(dir)) != NULL) {
    if (!HasRomExtension(e->d_name)) continue;
    if (++found == 1)
      snprintf(out, cap, "%s%s", SNESRECOMP_VITA_DATA_DIR, e->d_name);
  }
  closedir(dir);
  /* More than one candidate is ambiguous: let the host report the missing
   * canonical file rather than guess. */
  return found == 1;
}

static int SDLCALL GestureWatch(void *userdata, SDL_Event *ev) {
  (void)userdata;
  if (ev->type != SDL_FINGERDOWN) return 0;
  /* Front panel only: SDL registers it as a direct touch device and the
   * rear pad as indirect. */
  if (SDL_GetTouchDeviceType(ev->tfinger.touchId) != SDL_TOUCH_DEVICE_DIRECT)
    return 0;
  int fingers = SDL_GetNumTouchFingers(ev->tfinger.touchId);
  if (SmVita_TapDown(&g_tap, ev->tfinger.x, ev->tfinger.y,
                     ev->tfinger.timestamp, fingers))
    g_toggle_requested = 1;
  /* Double tap on the right half: infinite energy on/off. */
  if (SmVita_TapDownSide(&g_tap_right, ev->tfinger.x, ev->tfinger.y,
                         ev->tfinger.timestamp, fingers, true))
    SmVita_RequestInfiniteEnergyToggle();
  return 0;
}

void SmVita_InstallGestures(SmVideoSettings *video, const char *video_ini) {
  g_video = video;
  g_video_ini = video_ini;
  /* High-fps interpolation draws the wide frame more than once per game
   * frame; the Vita cannot afford that. */
  if (video) video->fps_enabled = false;
  if (g_installed) return;
  g_installed = true;
  SDL_AddEventWatch(GestureWatch, NULL);
}

bool SmVita_ApplyPendingGestures(void) {
  if (!g_toggle_requested || !g_video) return false;
  g_toggle_requested = 0;

  bool on = !g_video->enhanced;
  g_video->enhanced = on;
  if (on) {
    /* The gesture always means 16:9 -- the Vita's own panel shape -- even
     * if sm-video.ini was copied from a desktop that used Fit or 21:9. */
    g_video->aspect = SM_ASPECT_16_9;
    SmRendererReset();
  }
  if (!SmVideoSave(g_video, g_video_ini))
    fprintf(stderr, "[vita] could not save %s\n", g_video_ini);
  snes_osd_push(on ? "Widescreen 16:9" : "Widescreen off", 1500);
  return true;
}

#else  /* !__vita__: only the state machine is built (unit test) */

void SmVita_EarlyInit(void) {}
void SmVita_SeedDataDir(void) {}
bool SmVita_ResolveRom(const char *rom_file, char *out, size_t cap) {
  (void)rom_file; (void)out; (void)cap;
  return false;
}
void SmVita_InstallGestures(SmVideoSettings *video, const char *video_ini) {
  (void)video; (void)video_ini;
}
bool SmVita_ApplyPendingGestures(void) { return false; }

#endif /* __vita__ */

/* ── Per-frame hooks (built everywhere so the host test can drive them; the
 * desktop host calls them only with SM_QOL_HOOKS=1) ───────────────────── */
#include "snes_osd.h"
static volatile int g_energy_requested;
static bool g_infinite_energy;
void SmVita_RequestInfiniteEnergyToggle(void) { g_energy_requested = 1; }
/* Between frames (the game fiber is parked at WaitForNMI), on the game's own
 * RAM. Addresses and the intro exit are the snesrev/sm decomp's. */
static uint16_t ram16(const uint8_t *ram, unsigned a) { return (uint16_t)(ram[a] | ram[a + 1] << 8); }
static void ram16_set(uint8_t *ram, unsigned a, uint16_t v) { ram[a] = (uint8_t)v; ram[a + 1] = (uint8_t)(v >> 8); }
void SmVita_FrameHooks(uint8_t *ram) {
  if (g_energy_requested) {
    g_energy_requested = 0;
    g_infinite_energy = !g_infinite_energy;
    snes_osd_push(g_infinite_energy ? "Infinite energy ON" : "Infinite energy OFF", 1500);
  }
  unsigned state = ram16(ram, 0x998);                    /* game_state */
  /* Infinite energy: top up Samus (samus_health $09C2 = samus_max_health
   * $09C4) in play, pauses and doors (states 7-18); never during the death
   * sequence or cutscenes. */
  if (g_infinite_energy && state >= 0x07 && state <= 0x12) {
    uint16_t max = ram16(ram, 0x9C4);
    if (max && ram16(ram, 0x9C2) < max) ram16_set(ram, 0x9C2, max);
  }
  /* Start skips the new-game intro (kGameState_30_IntroCinematic) through
   * the game's own exit: the end-of-intro instruction (CinematicCommonInstr_
   * Func70, $8B:C0A2) sets the fade-out routine $8B:C0C5 with a 1-frame fade
   * step; that fades, then $8B:C100 sets up the new game and saves. Not while
   * the intro is still setting up ($8B:A395) or already leaving. */
  if (state == 0x1E && (ram16(ram, 0x8F) & 0x1000)) {    /* joypad1_newkeys: Start */
    uint16_t fn = ram16(ram, 0x1F51);                    /* cinematic_function */
    if (fn != 0xA395 && fn != 0xC0C5 && fn != 0xC100) {
      /* The flashback ($8B:A3B0) gives Samus 900 missiles and swaps in the
       * demo's button bindings (saving the player's in $0D82-$0D8E,
       * $8B:BC08); its end ($8B:B72F) undoes both. Skipping from inside it
       * must undo them too, or the new game starts with 900 missiles and
       * the demo's controls. */
      if (ram16(ram, 0x9C8) == 900) {
        for (unsigned i = 0; i < 7; ++i)                 /* $8B:BC75 */
          ram16_set(ram, 0x9B2 + 2 * i, ram16(ram, 0xD82 + 2 * i));
        ram16_set(ram, 0x9C8, 0);
        ram16_set(ram, 0x9C6, 0);
      }
      ram16_set(ram, 0x1F51, 0xC0C5);
      ram16_set(ram, 0x723, 1);                          /* screen_fade_delay */
      ram16_set(ram, 0x725, 1);                          /* screen_fade_counter */
      snes_osd_push("Intro skipped", 1200);
    }
  }
}

