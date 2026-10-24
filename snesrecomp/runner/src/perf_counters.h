/* Lightweight always-compiled perf counters for handheld bring-up (PS Vita).
 * Off unless g_perf_on is set (SNESRECOMP_PERF=1); then the host prints a
 * per-window breakdown (desktop/host_main.c) and resets the totals. */
#ifndef SNESRECOMP_PERF_COUNTERS_H
#define SNESRECOMP_PERF_COUNTERS_H
#include <time.h>
extern int g_perf_on;
extern double g_perf_ppu_s, g_perf_apu_s, g_perf_irq_s, g_perf_prep_s, g_perf_draw_s, g_perf_note_s;
/* Inside "compose": a title waiting on its own drawing thread, and copying
 * the finished picture into the presenter's buffer. */
extern double g_perf_cwait_s, g_perf_ccopy_s;
/* The background APU thread's own share of apu(all) (the rest ran on the
 * game thread), and general-purpose DMA ($420B) on the game thread. */
extern double g_perf_apu_bg_s, g_perf_dma_s;
/* Time threads other than the APU thread spent waiting for the APU lock. */
extern double g_perf_apulock_s;
static inline double perf_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
#endif
