/*
 * Super Metroid — desktop host shim.
 *
 * Identity and hooks, nothing else. The host itself is the framework's
 * (snesrecomp/runner/src/desktop/host_main.c): launcher, ROM resolution,
 * config, window, presenters, audio, gamepads, overlays, pacing, crash
 * reporting. Until 2026-09 all of that was a 2,800-line copy in this file,
 * which is why a second scaffold of this game booted to a black frame while
 * this one played: the fixes lived here, not one level up.
 *
 * What stays here is what is actually Super Metroid:
 *   - the read-only custom widescreen renderer (sm_renderer.c) and its
 *     aspect/fps policy (sm_video.c), presented through the draw_frame hook;
 *   - the SPC upload interception (sm_spc_player.c);
 *   - the door-transition pacing rule: those frames may repay wall-time debt
 *     so the guest-driven audio queue stays fed while Samus has no control;
 *   - the presentation Mods page (sm_mods.c);
 *   - SM_AUDIO_PROBE, a per-frame audio/doorway measurement.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __ANDROID__
#define SDL_MAIN_HANDLED 1
#endif
#include "desktop/sdl_compat.h"
#ifdef __ANDROID__
/* SDLActivity loads libmain.so and dlsyms its SDL_main. */
#define main SDL_main
#endif

#include "host_main.h"
#include "host_clock.h"
#include "common_rtl.h"
#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "host_report.h"
#include "perf_counters.h"
#include "widescreen.h"
#include "audio_trace.h"
#include "snes/apu.h"
#include "snes/snes.h"
#include "snesrecomp_rom_identity.h"  /* generated from rom_identity.txt */

#include "sm_display.h"
#include "sm_renderer.h"
#include "sm_video.h"
#include "sm_spc_player.h"
#if defined(RECOMP_LAUNCHER)
#include "sm_mods.h"
#endif
#ifndef __vita__
#define SM_WD_STAGE(s) ((void)0)
#define SM_WD_FRAME_DONE() ((void)0)
#endif
#include "sm_vita.h"
#ifdef __vita__
/* The game fiber gets its own 8 MiB thread stack (sm_rtl.c); the main
 * thread runs the host loop (4 MiB, like a desktop main thread). Heap: the
 * generated program's tables, the PPU/APU state and the widescreen output
 * buffers, with room to spare. */
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
int _newlib_heap_size_user = 192 * 1024 * 1024;
/* Default stack for threads created without attributes (the widescreen
 * renderer's line workers keep a PPU register copy, ~200 KB, on the stack);
 * the Vita's pthread default is far smaller. */
unsigned int _pthread_stack_default_user = 1024 * 1024;
#endif

#ifndef SNESRECOMP_BUILD_VERSION
#define SNESRECOMP_BUILD_VERSION "dev"
#endif

extern const RtlGameInfo kSuperMetroidGameInfo;
extern Snes *g_snes;
extern uint8_t g_ram[0x20000];

static const char *kSmVideoConfig = "sm-video.ini";
static SmViewport g_sm_viewport;
static uint32_t g_sm_output[SM_MAX_WIDTH * SM_HEIGHT];

/* PS Vita always draws through the capture, also at 4:3: the stock picture
 * is then rendered on the drawing cores while core 0 runs the next frame. */
bool SmCustomRendererEnabled(void) {
#ifdef __vita__
  return true;
#else
  static int deferred_stock = -1;   /* SM_DEFER_STOCK=1: test the Vita path */
  if (deferred_stock < 0) {
    const char *e = getenv("SM_DEFER_STOCK");
    deferred_stock = e && atoi(e) != 0;
  }
  return deferred_stock || g_sm_video.enhanced || g_sm_video.fps_enabled;
#endif
}

bool SmDisplay_IsWidescreenActive(void) { return g_sm_viewport.enhanced; }
int SmDisplay_GetCurrentFrameWidth(void) { return snesrecomp_desktop_frame_width(); }

static void SmAfterConfig(void) {
  if (!SmVideoLoad(&g_sm_video, kSmVideoConfig))
    fprintf(stderr, "[video] Invalid settings in %s; valid entries retained.\n", kSmVideoConfig);
  SmRendererReset();
  /* Compatibility with existing capture commands: these select the custom
   * renderer, never the old guest activation/culling overrides. */
  {
    const char *ws_env = getenv("SNESRECOMP_WIDESCREEN");
    if (ws_env && *ws_env) g_sm_video.enhanced = atoi(ws_env) != 0;
    const char *aspect = getenv("SM_VIDEO_ASPECT");
    if (aspect && !SmParseAspect(aspect, &g_sm_video.aspect))
      fprintf(stderr, "[video] Ignoring invalid SM_VIDEO_ASPECT\n");
  }
  g_sm_viewport = SmCalculateViewport(&g_sm_video, 16, 9);
#ifdef __vita__
  /* Double-tap the left half of the front screen: 16:9 on/off. */
  SmVita_InstallGestures(&g_sm_video, kSmVideoConfig);
#endif
  host_report_breadcrumb("custom renderer: %d aspect=%s fps=%u hud=%d",
                         SmCustomRendererEnabled(), SmAspectName(g_sm_video.aspect),
                         g_sm_video.fps, g_sm_video.hud_anchored);
}

static void SmOnRomLoaded(const uint8_t *rom, size_t size) {
  SmRendererSetRom(rom, size);
}

static void SmOnReset(void) {
  SmRendererReset();
}

static void SmBeforeRunFrame(void) {
#ifndef __vita__
  { static int qol = -1;   /* the Vita's per-frame hooks, for host tests */
    if (qol < 0) {
      const char *e = getenv("SM_QOL_HOOKS");
      qol = e && atoi(e) != 0;
      if (e && strstr(e, "energy")) SmVita_RequestInfiniteEnergyToggle();
    }
    if (qol) SmVita_FrameHooks(g_ram); }
#endif
#ifdef __vita__
  SM_WD_FRAME_DONE();
  SM_WD_STAGE("running the game (fiber) for a frame");
  SmVita_ApplyPendingGestures();
  SmVita_FrameHooks(g_ram);   /* frame boundary: width may change here */
#endif
  /* Object snapshots only feed the widescreen renderer's own lines. */
  if (SmCustomRendererEnabled() && (g_sm_viewport.enhanced || g_sm_video.fps_enabled))
    SmRendererLatchObjectState(g_ram);
}

/* Door loading can take longer than a frame. Keep its debt only while the
 * guest is in its non-interactive transition states: those frames refill the
 * guest-driven SPC queue. The moment state 8 gameplay returns, drop any
 * remainder so Samus can never burst forward after the doorway. */
static int SmKeepPacingDebt(void) {
  uint8_t game_state = g_ram[0x0998];
  return game_state >= 9 && game_state <= 11;
}

static void SmPrepareFrame(int drawable_w, int drawable_h, int *frame_w, int *frame_h) {
  g_sm_viewport = SmCalculateViewport(&g_sm_video, drawable_w, drawable_h);
  *frame_w = g_sm_viewport.width;
  *frame_h = SM_HEIGHT;
}

/* Simulation owns these: the renderer's per-frame capture runs exactly once
 * per simulated frame, never for an interpolated present. */
static void SmBeginSimFrame(unsigned number) {
  SM_WD_STAGE("capturing the frame's raster lines");
  if (SmCustomRendererEnabled()) {
    SmRendererSetStockOnly(!g_sm_viewport.enhanced && !g_sm_video.fps_enabled);
    SmRendererBeginFrame(g_ram, number);
  }
}

static void SmEndSimFrame(const uint8_t *field, unsigned number) {
  if (SmCustomRendererEnabled())
    SmRendererEndFrame((const uint32_t *)field);
  const char *capture_frame = getenv("SM_CAPTURE_FRAME");
  const char *capture_path = getenv("SM_CAPTURE_PATH");
  if (capture_frame && capture_path && number == strtoul(capture_frame, NULL, 10))
    SmRendererSaveCapture(capture_path);
}

static int SmDrawFrame(uint8_t *dst, size_t pitch, const uint8_t *field,
                       int frame_w, int frame_h, double alpha) {
  if (!SmCustomRendererEnabled())
    return 0;
#ifdef __vita__
  /* Pipelined: show the frame the compose thread finished, then hand it the
   * one just captured, so drawing overlaps the next guest frame (one frame
   * of display latency). Direct draws cover the start, size changes and a
   * failed draw. */
  if (alpha >= 1) {
    SM_WD_STAGE("waiting for the compose thread");
    static uint32_t s_out[2][SM_MAX_WIDTH * SM_HEIGHT];
    static int s_width[2], s_inflight = -1;
    int ready = -1;
    double t_wait = g_perf_on ? perf_now() : 0;
    if (s_inflight >= 0) {
      if (SmRendererDrawAsyncWait() && s_width[s_inflight] == frame_w) ready = s_inflight;
      s_inflight = -1;
    }
    if (g_perf_on) g_perf_cwait_s += perf_now() - t_wait;
    if (ready < 0) {
      ready = 0;
      if (!SmRendererDraw(s_out[0], g_sm_viewport, g_sm_video.hud_anchored, 1)) {
        memset(s_out[0], 0, sizeof(s_out[0]));
        for (int y = 0; y < SM_HEIGHT; ++y)
          memcpy(s_out[0] + y * frame_w + g_sm_viewport.extra, field + y * 256 * 4, 256 * 4);
      }
      s_width[0] = frame_w;
    }
    SM_WD_STAGE("copying the frame for presenting");
    double t_copy = g_perf_on ? perf_now() : 0;
    RtlWidescreenPresent(dst, pitch, (const uint8_t *)s_out[ready], frame_w, frame_h);
    if (g_perf_on) g_perf_ccopy_s += perf_now() - t_copy;
    SM_WD_STAGE("handing the next frame to the compose thread");
    int next = ready ^ 1;
    s_width[next] = frame_w;
    if (SmRendererDrawAsyncStart(s_out[next], g_sm_viewport, g_sm_video.hud_anchored, 1))
      s_inflight = next;
    SM_WD_FRAME_DONE();
    SM_WD_STAGE("presenting / VSync / host loop");
    return 1;
  }
#endif
  if (!SmRendererDraw(g_sm_output, g_sm_viewport, g_sm_video.hud_anchored, alpha)) {
    memset(g_sm_output, 0, sizeof(g_sm_output));
    for (int y = 0; y < SM_HEIGHT; ++y)
      memcpy(g_sm_output + y * frame_w + g_sm_viewport.extra,
             field + y * 256 * 4, 256 * 4);
  }
  RtlWidescreenPresent(dst, pitch, (const uint8_t *)g_sm_output, frame_w, frame_h);
  return 1;
}

/* Presentation-only iterations reuse a captured simulation frame, so they
 * are custom-renderer only; without the fps mod the picture is presented
 * in lockstep with the simulation (and vsync stays on). */
static double SmPresentationHzHook(double display_refresh) {
  if (!SmCustomRendererEnabled() || !g_sm_video.fps_enabled)
    return 0;
  return SmPresentationHz(g_sm_video.fps, display_refresh > 0 ? display_refresh : 60);
}

#if defined(RECOMP_LAUNCHER)
static const struct RecompLauncherCModProvider *SmMods(void) {
  return SmModsProvider(&g_sm_video, kSmVideoConfig);
}
#endif

/* SM_AUDIO_PROBE=<csv>: production-path measurement. Sampled after guest
 * execution so a trace proves that the saved doorway was crossed, and
 * attributes missing PCM to the transition rather than boot or loading the
 * save itself. Kept in memory until close: frequent small writes can block
 * behind filesystem/antivirus work and create the very underruns being
 * measured. */
static FILE *g_audio_probe;
static void SmCloseAudioProbe(void) {
  if (g_audio_probe) fclose(g_audio_probe);
  g_audio_probe = NULL;
}
static void SmAfterRunFrame(const SnesDesktopHostFrameStats *st) {
  static int probe_checked;
  if (!probe_checked) {
    probe_checked = 1;
    const char *path = getenv("SM_AUDIO_PROBE");
    g_audio_probe = path ? fopen(path, "w") : NULL;
    if (g_audio_probe) {
      setvbuf(g_audio_probe, NULL, _IOFBF, 1024 * 1024);
      fprintf(g_audio_probe, "frame,seconds,guest_ms,state,door_step,room,master,port_clock,guest_anchor,target_anchor,last_guest,last_target,produced,consumed,underflows,occupancy,missing_frames,dropped,output_rate\n");
      atexit(SmCloseAudioProbe);
    }
  }
  if (!g_audio_probe) return;
  AudioTraceStats stats;
  audio_trace_get_stats(&stats);
  Apu *apu = g_snes->apu;
  fprintf(g_audio_probe, "%d,%.6f,%.3f,%u,%04x,%04x,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%d\n",
      snes_frame_counter, st->run_seconds, st->guest_seconds * 1000,
      g_ram[0x998] | g_ram[0x999] << 8, g_ram[0x99c] | g_ram[0x99d] << 8,
      g_ram[0x79b] | g_ram[0x79c] << 8,
      (unsigned long long)g_cpu.master_cycles,
      (unsigned long long)apu->portClock, (unsigned long long)apu->portGuestAnchor,
      (unsigned long long)apu->portTargetAnchor, (unsigned long long)apu->portLastGuest,
      (unsigned long long)apu->portLastTarget, (unsigned long long)stats.produced,
      (unsigned long long)stats.consumed, (unsigned long long)stats.output_underflows,
      stats.occupancy_current,
      (unsigned long long)stats.output_missing_frames,
      (unsigned long long)stats.dropped, st->audio_output_rate);
}

static const SnesDesktopHostGame kSuperMetroidHost = {
  .display_name = "Super Metroid",
  .window_title = "Super Metroid (Recompiled)",
  .region = SNESRECOMP_ROM_REGION,
  .rom_file = SNESRECOMP_ROM_FILE,
  .expected_sha256_hex = SNESRECOMP_ROM_EXPECTED_SHA256,
  .expected_crc32_hex = SNESRECOMP_ROM_EXPECTED_CRC32,
  .game_id = SNESRECOMP_ROM_GAME_ID,
  .build_version = SNESRECOMP_BUILD_VERSION,
  .env_prefix = "SM",
  .sram_path = "saves/save.srm",   /* battery SRAM: the launcher shows SAVES */
  .game_info = &kSuperMetroidGameInfo,
  .num_players = 1,
  /* Per-game debug server port: 4377 SMW, 4378 Zelda LttP, 4379 MMX, 4380 SM. */
  .debug_port = 4380,
  .widescreen_supported = 0,       /* widescreen is this title's Mods page */
  .msu1_supported = 0,
  .shader_supported = 1,
  .simulation_hz = SM_SIMULATION_HZ,
  .frame_width = 256,
  .frame_height = SM_HEIGHT,

  .create_spc_player = &SmSpcPlayer_Create,
#if defined(RECOMP_LAUNCHER)
  .mods_provider = &SmMods,
#endif
  .after_config = &SmAfterConfig,
  .on_rom_loaded = &SmOnRomLoaded,
  .on_reset = &SmOnReset,
  .before_run_frame = &SmBeforeRunFrame,
  .after_run_frame = &SmAfterRunFrame,
  .keep_pacing_debt = &SmKeepPacingDebt,

  .prepare_frame = &SmPrepareFrame,
  .begin_sim_frame = &SmBeginSimFrame,
  .end_sim_frame = &SmEndSimFrame,
  .draw_frame = &SmDrawFrame,
  .presentation_hz = &SmPresentationHzHook,
  .window_base_width = &SmDisplay_GetWindowBaseWidth,
  .window_base_height = &SmDisplay_GetWindowBaseHeight,
#ifdef __vita__
  /* Read [Rewind] from config.ini; the Vita config ships it off. */
  .rewind_settings = 1,
#endif
};

#ifndef __ANDROID__
#undef main   /* desktop: keep plain main() even if SDL_main.h remapped it */
#endif
int main(int argc, char **argv) {
#ifdef __vita__
  SmVita_EarlyInit();
  SmVita_SeedDataDir();
  SmVita_WatchdogStart();
  /* LiveArea starts us with no arguments. Accept any single *.sfc/*.smc in
   * the data folder (e.g. a renamed "sm.smc"); the host still checks that it
   * is the exact supported dump. */
  static char rom_path[512];
  static char *vita_argv[3];
  if (argc <= 1 &&
      SmVita_ResolveRom(SNESRECOMP_ROM_FILE, rom_path, sizeof(rom_path))) {
    vita_argv[0] = argc > 0 ? argv[0] : "eboot.bin";
    vita_argv[1] = rom_path;
    vita_argv[2] = NULL;
    argc = 2;
    argv = vita_argv;
  }
  fprintf(stderr, "[vita] ROM argument: %s\n", argc > 1 ? argv[1] : "(none)");
#endif
  return snesrecomp_desktop_main(&kSuperMetroidHost, argc, argv);
}

/* Called once by the interpreter on the first architectural BRK (a guest
 * that has gone off the rails): where in the game it happened. */
void snesrecomp_brk_report_extra(FILE *out) {
  const uint8_t *r = g_ram;
  if (!r || !out) return;
#define W(a) ((unsigned)(r[(a)] | (r[(a) + 1] << 8)))
  fprintf(out, "[sm_brk] game_state=$%04X room=$%04X area=%u door=$%04X "
          "door_fn=$%04X samus=(%u,%u) health=%u/%u\n",
          W(0x998), W(0x79B), W(0x79F), W(0x78D), W(0x99C),
          W(0xAF6), W(0xAFA), W(0x9C2), W(0x9C4));
#undef W
}
