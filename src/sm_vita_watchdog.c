/*
 * sm_vita_watchdog.c — hang watchdog for the PS Vita build.
 *
 * A low-priority thread watches the host's frame counter. When it stops for
 * several seconds (longer at boot), it writes every thread's last stage to
 * hang_report.txt and to the end of log.txt, then faults on purpose so the
 * Vita writes a psp2core dump of all threads. Read the dump against the
 * build's SuperMetroid-Vita-<tag>.elf. An empty "no_watchdog" file in the
 * data folder turns it off.
 */
#include "sm_vita.h"

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include "render_workers.h"

const char *volatile g_sm_wd_stage = "boot";
volatile unsigned g_sm_wd_frames;

extern volatile const char *g_sm_compose_stage;                        /* sm_renderer.c */
extern volatile const char *g_render_worker_stage[SNES_RENDER_WORKERS_MAX];
extern volatile unsigned g_render_worker_batches;                      /* render_workers.c */
extern volatile const char *g_apu_bg_stage;                            /* common_rtl.c */
extern volatile int g_fiber_switches;                                  /* fiber_compat.c */

enum { kPollUs = 250000, kBootPolls = 120, kRunPolls = 40 };   /* 30 s at boot, then 10 s */

static const char *str(volatile const char *s) { return s ? (const char *)s : "-"; }

static void report(unsigned frames, int polls) {
  /* The stuck thread may hold stdio's lock: write through sceIo only. */
  static char buf[2048];
  int n = snprintf(buf, sizeof(buf),
      "\n[watchdog] HANG: no frame for %.1f s (frames=%u)\n"
      "[watchdog] main thread: %s\n"
      "[watchdog] game fiber switches: %d (%s)\n"
      "[watchdog] compose thread: %s\n"
      "[watchdog] drawing batches: %u\n"
      "[watchdog] APU thread: %s\n",
      polls * (kPollUs / 1e6), frames, str(g_sm_wd_stage), g_fiber_switches,
      (g_fiber_switches & 1) ? "odd: the game fiber should be running"
                             : "even: the host should be running",
      str(g_sm_compose_stage), g_render_worker_batches, str(g_apu_bg_stage));
  for (int i = 0; i < SNES_RENDER_WORKERS_MAX && n > 0 && n < (int)sizeof(buf); ++i) {
    if (!g_render_worker_stage[i]) continue;
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "[watchdog] drawing slot %d: %s\n",
                  i, str(g_render_worker_stage[i]));
  }
  if (n > 0 && n < (int)sizeof(buf))
    snprintf(buf + n, sizeof(buf) - (size_t)n,
             "[watchdog] faulting on purpose: send log.txt, hang_report.txt and the newest "
             "ux0:data/psp2core-*.psp2dmp\n");
  size_t len = strlen(buf);
  SceUID fd = sceIoOpen(SNESRECOMP_VITA_DATA_DIR "hang_report.txt",
                        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0) { sceIoWrite(fd, buf, len); sceIoClose(fd); }
  fd = sceIoOpen(SNESRECOMP_VITA_DATA_DIR "log.txt", SCE_O_WRONLY, 0777);
  if (fd >= 0) {
    sceIoLseek(fd, 0, SCE_SEEK_END);   /* add to the end; never overwrite the log */
    sceIoWrite(fd, buf, len);
    sceIoClose(fd);
  }
}

static int watchdog_thread(SceSize args, void *argp) {
  (void)args; (void)argp;
  unsigned last = g_sm_wd_frames;
  int still = 0;
  for (;;) {
    sceKernelDelayThread(kPollUs);
    unsigned now = g_sm_wd_frames;
    if (now != last) { last = now; still = 0; continue; }
    if (++still < (last == 0 ? kBootPolls : kRunPolls)) continue;
    report(last, still);
    sceKernelDelayThread(200000);
    *(volatile unsigned *)8 = 0xDEADu;   /* deliberate fault -> psp2core dump */
    still = 0;
  }
  return 0;
}

void SmVita_WatchdogStart(void) {
  FILE *off = fopen(SNESRECOMP_VITA_DATA_DIR "no_watchdog", "rb");
  if (off) {
    fclose(off);
    fprintf(stderr, "[watchdog] off (no_watchdog file)\n");
    return;
  }
  SceUID tid = sceKernelCreateThread("sm_watchdog", watchdog_thread, 100, 0x4000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_2, NULL);
  if (tid < 0 || sceKernelStartThread(tid, 0, NULL) < 0) {
    fprintf(stderr, "[watchdog] could not start (0x%08x)\n", (unsigned)tid);
    return;
  }
  fprintf(stderr, "[watchdog] armed: 10 s without a frame (30 s at boot)\n");
}
