#include "sm_renderer.h"
#include "sm_mode7.h"
#include <string.h>
#include <math.h>
#include <stddef.h>

/* Immutable host snapshots. Presenting never invokes guest code or writes PPU,
 * WRAM, VRAM or OAM. Each visible line owns its memory image so raster DMA
 * cannot retroactively change an earlier line. */
/* VRAM almost never changes mid-frame, so lines share copies: `vram` points
 * at vram_pool[vram_slot] of the owning frame, and a new copy is taken only
 * when the PPU's vramWriteCount moved since the previous captured line (one
 * 64 KB copy per frame instead of 224, i.e. ~14 MB less copying a frame). */
typedef struct SmRasterLine {
  uint8_t registers[PPU_SAVESTATE_REGS_SIZE];
  uint16_t palette[256], oam[256];
  const uint16_t *vram;
  uint16_t vram_slot;
  uint8_t high_oam[32];
} SmRasterLine;
typedef struct SmSourceFrame {
  SmRasterLine lines[224];
  uint16_t vram_pool[224][0x8000];
  unsigned vram_pool_used;
  uint32_t vram_write_count;
  uint8_t ram[0x20000];
  uint32_t stock[256 * 224];
  unsigned number, captured;
  bool valid;
  uint8_t object_ram[0x20000];
  bool object_state_valid;
} SmSourceFrame;
static SmSourceFrame frames[2];
static unsigned current;
/* Deferred stock picture (PS Vita; SM_DEFER_STOCK=1 elsewhere): the emulated
 * PPU skips every visible line and the drawing pass renders the stock lines
 * it needs from the capture, on the drawing cores, instead of core 0 drawing
 * them during the guest's raster loop. `host_*` are the PPU's per-frame host
 * settings (the parts of Ppu outside the captured registers and memory that
 * line drawing reads), taken once a frame at line 1. Kept beside `frames` so
 * the capture file format is unchanged. */
enum {
  SM_STOCK_HOST_A = offsetof(Ppu, vramPointer),
  SM_STOCK_HOST_A_END = offsetof(Ppu, wsMode2Capture),
  SM_STOCK_HOST_B = offsetof(Ppu, wsLayerClamp),
  SM_STOCK_HOST_B_END = offsetof(Ppu, bgBuffers),
};
typedef struct SmStockState {
  bool deferrable, deferred;
  /* Captured with SmRendererSetStockOnly(true): only low WRAM was copied, so
   * the frame is drawn from the stock picture alone (4:3 needs nothing else). */
  bool stock_only;
  uint8_t host_a[SM_STOCK_HOST_A_END - SM_STOCK_HOST_A];
  uint8_t host_b[SM_STOCK_HOST_B_END - SM_STOCK_HOST_B];
  uint8_t mosaic_modulo[kPpuXPixels];
} SmStockState;
static SmStockState stock_state[2];
static int defer_stock = -1;
static bool stock_deferral_on(void) {
  if (defer_stock < 0) {
#ifdef __vita__
    defer_stock = 1;
#else
    const char *e = getenv("SM_DEFER_STOCK");
    defer_stock = e && atoi(e) != 0;
#endif
  }
  return defer_stock != 0;
}
/* The line being drawn's register set, one per drawing participant (PS Vita
 * draws lines on several cores; draw_line's `scanout` is its slot's). */
enum { SM_DRAW_SLOTS = 4 };
/* [slot] = the line being drawn, [SM_DRAW_SLOTS + slot] = the same line of the
 * previous frame (scroll/mode-7 interpolation). Static, not on the stack: a
 * Ppu is ~200 KB and worker threads have small stacks. */
static Ppu scanout_slots[2 * SM_DRAW_SLOTS];
static const uint8_t *renderer_rom;
static size_t renderer_rom_size;
static bool captured_mode7;
static bool captured_mosaic;
static bool captured_motion;
static bool captured_edge;
static bool captured_effect;
static bool captured_grapple;
static bool captured_rotation;
static struct { unsigned position, attr, high; int dx, dy; bool valid; } grapple_motion[128];
static unsigned captured_power_bomb_phases;
static unsigned power_bomb_phase_observations[5];
static unsigned captured_rooms[128], captured_room_count, stable_room_key, stable_room_frames;
static bool has_edge_motion(const SmSourceFrame *f, const SmSourceFrame *old);
static int moving_effect_slot(const SmSourceFrame *f, const SmSourceFrame *old);
/* Diagnostic counters. With the Vita's parallel line drawing they are
 * approximate (unsynchronized increments); tests draw serially. */
static SmRendererStats render_stats;
static uint8_t latched_object_ram[0x20000];
static bool object_state_latched;
void SmRendererLatchObjectState(const uint8_t ram[0x20000]) {
  memcpy(latched_object_ram, ram, sizeof(latched_object_ram));
  object_state_latched = true;
}
SmRendererStats SmRendererGetStats(void) { return render_stats; }
const uint32_t *SmRendererStockFrame(void) {
  return frames[current].valid ? frames[current].stock : NULL;
}
void SmRendererSetRom(const uint8_t *rom, size_t size) {
  renderer_rom = rom;
  renderer_rom_size = size;
}
static const uint8_t *rom_bytes(unsigned bank, unsigned address, size_t size) {
  if (!renderer_rom || address < 0x8000 || address > 0xffff ||
      size > 0x10000 - address) return NULL;
  size_t offset = ((bank & 0x7f) << 15) | (address & 0x7fff);
  if (offset > renderer_rom_size || size > renderer_rom_size - offset) return NULL;
  return renderer_rom + offset;
}

static bool save_capture(const SmSourceFrame *frame, const char *path) {
  if (!path || !frame->valid) return false;
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  const uint32_t header[3] = {0x534d5243, 2, sizeof(SmSourceFrame)};
  bool ok = fwrite(header, sizeof(header), 1, file) == 1 &&
            fwrite(frame, sizeof(SmSourceFrame), 1, file) == 1;
  return fclose(file) == 0 && ok;
}
bool SmRendererSaveCapture(const char *path) {
  return save_capture(&frames[current], path);
}
bool SmRendererLoadCapture(const char *path) {
  if (!path) return false;
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  uint32_t header[3];
  SmSourceFrame *next = &frames[current ^ 1];
  const size_t v1_size = (offsetof(SmSourceFrame, object_ram) + 3) & ~(size_t)3;
  bool ok = fread(header, sizeof(header), 1, file) == 1 &&
            header[0] == 0x534d5243 &&
            ((header[1] == 1 && header[2] == v1_size) ||
             (header[1] == 2 && header[2] == sizeof(*next))) &&
            fread(next, header[2], 1, file) == 1 &&
            next->valid && next->captured == 224 && fgetc(file) == EOF;
  fclose(file);
  if (ok && header[1] == 1) {
    /* Legacy captures did not retain pre-NMI owner state. Replay discrete
     * objects for inspection, but do not claim trustworthy interpolation. */
    memcpy(next->object_ram, next->ram, sizeof(next->object_ram));
    next->object_state_valid = false;
  }
  for (unsigned y = 0; ok && y < 224; ++y) {   /* pointers in the file are stale */
    if (next->lines[y].vram_slot >= 224) ok = false;
    else next->lines[y].vram = next->vram_pool[next->lines[y].vram_slot];
  }
  stock_state[current ^ 1].deferrable = stock_state[current ^ 1].deferred = false;
  if (ok) current ^= 1;
  else next->valid = false;
  return ok;
}

static unsigned read16(const uint8_t *ram, unsigned address) {
  return ram[address] | (ram[address + 1] << 8);
}
static bool room_state(unsigned state) {
  return (state >= 7 && state <= 13) || (state >= 16 && state <= 25) ||
         state == 27 || (state >= 32 && state <= 38) || state == 42;
}
bool SmRendererPowerBombWidths(const uint8_t profile[160], unsigned radius,
                              int16_t widths[192]) {
  if (!profile || !widths || radius > 255) return false;
  int16_t result[192];
  for (unsigned row = 0; row < 192; ++row) result[row] = -1;
  int row = (int)(radius * profile[128] >> 8);
  if (row >= 192) return false;
  int width = 0;
  for (unsigned point = 96; point < 128; ++point) {
    int end = (int)(radius * profile[point + 32] >> 8);
    if (end > row) return false;
    width = (int)(radius * profile[point] >> 8);
    /* Retail includes both endpoints; the next segment overwrites their
     * shared row. Preserve this detail and the final centerward fill. */
    for (int y = row; y >= end; --y) result[y] = (int16_t)width;
    row = end;
  }
  for (; row >= 0; --row) result[row] = (int16_t)width;
  memcpy(widths, result, sizeof(result));
  return true;
}
static int lerp_pixel(int old, int now, double alpha) {
  return (int)lround(old + (now - old) * alpha);
}
static bool continuous_frames(const SmSourceFrame *old, const SmSourceFrame *now) {
  return old->valid && old->number + 1 == now->number &&
      read16(old->ram, 0x79b) == read16(now->ram, 0x79b) &&
      read16(old->ram, 0x998) == read16(now->ram, 0x998) &&
      abs((int)read16(old->ram, 0x911) - (int)read16(now->ram, 0x911)) <= 32 &&
      abs((int)read16(old->ram, 0x915) - (int)read16(now->ram, 0x915)) <= 32;
}

/* Match an actual Samus ROM map entry, not a screen proximity or OAM-slot
 * heuristic. Echoes, projectiles and reused reservations stay discrete unless
 * their coordinates and tile identity genuinely match the player map. */
static bool samus_piece(const SmSourceFrame *f, unsigned position,
                        unsigned attr, unsigned high) {
  for (unsigned half = 0; half < 2; ++half) {
    unsigned index = read16(f->object_ram, 0xac8 + half * 2);
    if (index > 0x7fff || (half && !index)) continue;
    const uint8_t *table = rom_bytes(0x92, 0x808d + index * 2, 2);
    if (!table) continue;
    unsigned map = read16(table, 0);
    const uint8_t *data = rom_bytes(0x92, map, 2);
    if (!data) continue;
    unsigned count = read16(data, 0);
    if (!count || count > 128) continue;
    data = rom_bytes(0x92, map + 2, count * 5);
    if (!data) continue;
    for (unsigned i = 0; i < count; ++i) {
      const uint8_t *entry = data + i * 5;
      unsigned offset = read16(entry, 0);
      unsigned x = (read16(f->object_ram, 0xb04) + offset) & 511;
      unsigned y = (read16(f->object_ram, 0xb06) + entry[2]) & 255;
      if (attr == read16(entry, 3) &&
          x == ((position & 255) | ((high & 1) << 8)) &&
          y == (position >> 8) && (offset >> 15) == ((high >> 1) & 1))
        return true;
    }
  }
  return false;
}
static bool map_piece(unsigned bank, unsigned map, int x, int y,
                      unsigned palette, unsigned base, unsigned position,
                      unsigned attr, unsigned high, int *actual_x) {
  const uint8_t *data = rom_bytes(bank, map, 2);
  if (!data) return false;
  unsigned count = read16(data, 0);
  if (!count || count > 128) return false;
  data = rom_bytes(bank, map + 2, count * 5);
  if (!data) return false;
  for (unsigned i = 0; i < count; ++i) {
    const uint8_t *entry = data + i * 5;
    unsigned offset = read16(entry, 0);
    if (((x + offset) & 511) == ((position & 255) | ((high & 1) << 8)) &&
        ((y + entry[2]) & 255) == (position >> 8) &&
        (offset >> 15) == ((high >> 1) & 1) &&
        (uint16_t)(palette | (base + read16(entry, 3))) == attr) {
      int dx = (int)(offset & 511);
      if (dx & 256) dx -= 512;
      *actual_x = x + dx;
      return true;
    }
  }
  return false;
}
static bool enemy_piece(const SmSourceFrame *f, unsigned slot,
                        unsigned position, unsigned attr, unsigned high, int *actual_x) {
  const uint8_t *ram = f->object_ram, *e = ram + 0xf78 + slot * 64;
  if (!read16(e, 0) || read16(e, 0) == 0xdaff || (read16(e, 14) & 0x300))
    return false;
  /* Shaking is intentionally discrete: its timer is changed during OAM
   * construction, so the owner snapshot alone cannot prove its phase. */
  if (read16(e, 42)) return false;
  int x = (int)read16(e, 2) - (int)read16(ram, 0x911) +
          (int16_t)read16(ram, 0x7010 + slot * 64);
  int y = (int)read16(e, 6) - (int)read16(ram, 0x915) +
          (int16_t)read16(ram, 0x7012 + slot * 64);
  unsigned palette = read16(e, 30), frozen = read16(e, 38);
  if (read16(e, 36) && (read16(ram, 0xe44) & 2)) palette = 0;
  else if (frozen && (frozen >= 0x5a || (frozen & 2))) palette = 3072;
  unsigned bank = e[46], map = read16(e, 22), base = read16(e, 32);
  if (!(read16(e, 16) & 4))
    return map_piece(bank, map, x, y, palette, base, position, attr, high, actual_x);
  const uint8_t *ext = rom_bytes(bank, map, 2);
  if (!ext) return false;
  unsigned count = read16(ext, 0);
  if (!count || count > 64) return false;
  ext = rom_bytes(bank, map + 2, count * 8);
  if (!ext) return false;
  for (unsigned i = 0; i < count; ++i) {
    const uint8_t *child = ext + i * 8;
    if (map_piece(bank, read16(child, 4), x + (int16_t)read16(child, 0),
                  y + (int16_t)read16(child, 2), palette, base, position, attr, high, actual_x))
      return true;
  }
  return false;
}
static bool enemy_motion(const SmSourceFrame *f, const SmSourceFrame *old,
                         unsigned slot, int *dx, int *dy) {
  if (!old || !f->object_state_valid || !old->object_state_valid) return false;
  const uint8_t *e = f->object_ram + 0xf78 + slot * 64;
  const uint8_t *before = old->object_ram + 0xf78 + slot * 64;
  if (!read16(e, 0) || read16(e, 0) != read16(before, 0) || e[46] != before[46] ||
      read16(e, 22) < 0x8000 || read16(before, 22) < 0x8000 ||
      ((read16(e, 16) ^ read16(before, 16)) & 4) ||
      ((read16(e, 14) | read16(before, 14)) & 0x300) ||
      read16(e, 42) || read16(before, 42)) return false;
  int world_dx = (int)read16(before, 2) - (int)read16(e, 2);
  int world_dy = (int)read16(before, 6) - (int)read16(e, 6);
  *dx = world_dx + (int)read16(f->object_ram, 0x911) - (int)read16(old->object_ram, 0x911) +
      (int16_t)read16(old->object_ram, 0x7010 + slot * 64) -
      (int16_t)read16(f->object_ram, 0x7010 + slot * 64);
  *dy = world_dy + (int)read16(f->object_ram, 0x915) - (int)read16(old->object_ram, 0x915) +
      (int16_t)read16(old->object_ram, 0x7012 + slot * 64) -
      (int16_t)read16(f->object_ram, 0x7012 + slot * 64);
  return abs(world_dx) <= 32 && abs(world_dy) <= 32 && abs(*dx) <= 32 && abs(*dy) <= 32;
}
static bool has_samus_motion(const SmSourceFrame *f, const SmSourceFrame *old) {
  const SmRasterLine *line = &f->lines[100], *previous = &old->lines[100];
  for (unsigned slot = 0; slot < 128; ++slot) {
    unsigned a = previous->oam[slot * 2], b = line->oam[slot * 2];
    unsigned attr = line->oam[slot * 2 + 1];
    unsigned ah = previous->high_oam[slot / 4] >> ((slot % 4) * 2);
    unsigned bh = line->high_oam[slot / 4] >> ((slot % 4) * 2);
    if (a == b || attr != previous->oam[slot * 2 + 1] || ((ah ^ bh) & 2)) continue;
    if (abs((int)(a & 255) - (int)(b & 255)) > 32 ||
        abs((int)(a >> 8) - (int)(b >> 8)) > 32) continue;
    if (samus_piece(old, a, attr, ah) && samus_piece(f, b, attr, bh)) return true;
  }
  return false;
}
#ifdef __vita__
#include <pthread.h>
#include <psp2/kernel/threadmgr.h>
/* Pipelined drawing: one compose thread runs SmRendererDraw for frame N
 * (fanning lines out to the worker pool) while the main thread runs the guest
 * for frame N+1. Frame N's capture slot is left alone until it finishes. */
static bool draw_frame_slot(unsigned frame_slot, uint32_t *out, SmViewport viewport,
                            bool hud_anchored, double alpha);
/* Handshake through kernel semaphores, not pthread condition variables:
 * pthread-embedded can lose a wake-up on the Vita (the Vandal Hearts port
 * deadlocked exactly that way). One job is in flight at most; only the main
 * thread (or the game fiber, never at the same time) starts and waits. */
static SceUID s_async_job_sema = -1, s_async_done_sema = -1;
static bool s_async_started, s_async_inflight, s_async_result;
volatile const char *g_sm_compose_stage = "not started";
static struct { unsigned slot; uint32_t *out; SmViewport viewport; bool hud; double alpha; } s_async_job;
static void *async_compose_main(void *arg) {
  (void)arg;
  /* Core 3 when a core-unlock plugin opens it, else cores 1-2 (core 0 keeps the guest). */
  SceUID tid = sceKernelGetThreadId();
  if (sceKernelChangeThreadCpuAffinityMask(tid, SCE_KERNEL_CPU_MASK_SYSTEM) < 0)
    sceKernelChangeThreadCpuAffinityMask(tid, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
  for (;;) {
    g_sm_compose_stage = "waiting for a frame";
    sceKernelWaitSema(s_async_job_sema, 1, NULL);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_sm_compose_stage = "drawing";
    bool ok = draw_frame_slot(s_async_job.slot, s_async_job.out, s_async_job.viewport,
                              s_async_job.hud, s_async_job.alpha);
    __atomic_store_n(&s_async_result, ok, __ATOMIC_SEQ_CST);
    g_sm_compose_stage = "reporting done";
    sceKernelSignalSema(s_async_done_sema, 1);
  }
  return NULL;
}
bool SmRendererDrawAsyncWait(void);
bool SmRendererDrawAsyncStart(uint32_t *out, SmViewport viewport, bool hud_anchored, double alpha) {
  if (!s_async_started) {
    s_async_job_sema = sceKernelCreateSema("sm_compose_job", 0, 0, 1, NULL);
    s_async_done_sema = sceKernelCreateSema("sm_compose_done", 0, 0, 1, NULL);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    s_async_started = s_async_job_sema >= 0 && s_async_done_sema >= 0 &&
                      pthread_create(&th, &attr, async_compose_main, NULL) == 0;
    pthread_attr_destroy(&attr);
    fprintf(stderr, "[vita] pipelined widescreen drawing: %s\n", s_async_started ? "on" : "unavailable");
    if (!s_async_started) return false;
  }
  SmRendererDrawAsyncWait();   /* never two in flight */
  s_async_job.slot = current; s_async_job.out = out; s_async_job.viewport = viewport;
  s_async_job.hud = hud_anchored; s_async_job.alpha = alpha;
  s_async_inflight = true;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  sceKernelSignalSema(s_async_job_sema, 1);
  return true;
}
bool SmRendererDrawAsyncWait(void) {
  if (!s_async_started) return false;
  if (s_async_inflight) {
    SceUInt timeout = 500000;
    if (sceKernelWaitSema(s_async_done_sema, 1, &timeout) < 0) {
      fprintf(stderr, "[vita] compose thread late (stage: %s); still waiting\n",
              (const char *)g_sm_compose_stage);
      sceKernelWaitSema(s_async_done_sema, 1, NULL);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    s_async_inflight = false;
  }
  return __atomic_load_n(&s_async_result, __ATOMIC_SEQ_CST);
}
#endif
void SmRendererReset(void) {
#ifdef __vita__
  SmRendererDrawAsyncWait();   /* never clear frames under an in-flight draw */
#endif
  memset(frames, 0, sizeof(frames));
  memset(stock_state, 0, sizeof(stock_state));
  current = 0;
  captured_mode7 = false;
  captured_mosaic = false;
  captured_motion = false;
  captured_edge = false;
  captured_effect = false;
  captured_grapple = false;
  captured_rotation = false;
  captured_power_bomb_phases = 0;
  memset(power_bomb_phase_observations, 0, sizeof(power_bomb_phase_observations));
  captured_room_count = stable_room_key = stable_room_frames = 0;
  object_state_latched = false;
}
static bool s_stock_only_next;
void SmRendererSetStockOnly(bool on) { s_stock_only_next = on; }
void SmRendererBeginFrame(const uint8_t ram[0x20000], unsigned number) {
  current ^= 1;
  SmSourceFrame *f = &frames[current];
  f->valid = false;
  f->captured = 0;
  f->vram_pool_used = 0;
  stock_state[current].deferrable = stock_state[current].deferred = false;
  f->number = number;
  stock_state[current].stock_only = s_stock_only_next;
  if (s_stock_only_next) {
    /* The game state, room and camera words a stock frame is judged by
     * (room_state, continuous_frames) all live in low WRAM. */
    memcpy(f->ram, ram, 0x2000);
    object_state_latched = false;
    f->object_state_valid = false;
    return;
  }
  memcpy(f->ram, ram, sizeof(f->ram));
  memcpy(f->object_ram, object_state_latched ? latched_object_ram : ram, sizeof(f->object_ram));
  /* The fallback copy is useful for discrete inspection, but is not the
   * pre-NMI owner state required to prove sprite interpolation. */
  f->object_state_valid = object_state_latched;
  object_state_latched = false;
}
void SmRendererCaptureLine(const Ppu *p, unsigned line) {
  if (!p || line < 1 || line > 224) return;
  SmSourceFrame *f = &frames[current];
  if (line != f->captured + 1) return;
  SmRasterLine *l = &f->lines[line - 1];
  memcpy(l->registers, p, sizeof(l->registers));
  memcpy(l->palette, p->cgram, sizeof(l->palette));
  memcpy(l->oam, p->oam, sizeof(l->oam));
  memcpy(l->high_oam, p->highOam, sizeof(l->high_oam));
  if (!f->vram_pool_used || p->vramWriteCount != f->vram_write_count) {
    memcpy(f->vram_pool[f->vram_pool_used], p->vram, sizeof(f->vram_pool[0]));
    f->vram_write_count = p->vramWriteCount;
    ++f->vram_pool_used;
  }
  l->vram_slot = (uint16_t)(f->vram_pool_used - 1);
  l->vram = f->vram_pool[l->vram_slot];
  if (line == 1) {
    SmStockState *st = &stock_state[current];
    st->deferrable = stock_deferral_on() && ppu_canDrawDetached(p);
    if (st->deferrable) {
      memcpy(st->host_a, (const uint8_t *)p + SM_STOCK_HOST_A, sizeof(st->host_a));
      memcpy(st->host_b, (const uint8_t *)p + SM_STOCK_HOST_B, sizeof(st->host_b));
      memcpy(st->mosaic_modulo, p->mosaicModulo, sizeof(st->mosaic_modulo));
    }
  }
  ++f->captured;
}
/* Should the emulated PPU skip drawing this line because the drawing pass
 * renders it from the capture? Call right after SmRendererCaptureLine. */
bool SmRendererStockDeferred(unsigned line) {
  return line >= 1 && line <= 224 && stock_state[current].deferrable &&
         frames[current].captured == line;
}
/* Will SmRendererDraw redraw this line itself (so the stock PPU need not)?
 * Call right after SmRendererCaptureLine(p, line). Mirrors draw_line: the
 * HUD band and non-room / non-mode-1/7 lines are taken from the stock frame. */
bool SmRendererLineReplaced(const Ppu *p, unsigned line) {
  const SmSourceFrame *f = &frames[current];
  if (!p || line < 1 || line > 224 || f->captured != line) return false;
  if (!room_state(read16(f->ram, 0x998))) return false;
  if (line - 1 < 32 && p->screenEnabled[0] == 4 && p->cgadsub == 0 && p->cgwsel == 0)
    return false;
  int mode = p->bgmode & 7;
  return mode == 1 || mode == 7;
}
bool SmRendererEndFrame(const uint32_t stock[256 * 224]) {
  SmSourceFrame *f = &frames[current];
  if (!stock || f->captured != 224) return false;
  stock_state[current].deferred = stock_state[current].deferrable;
  if (!stock_state[current].deferred) memcpy(f->stock, stock, sizeof(f->stock));
  f->valid = true;
  const char *motion_prefix = getenv("SM_CAPTURE_MOTION_PREFIX");
  const char *motion_after = getenv("SM_CAPTURE_MOTION_AFTER");
  const SmSourceFrame *previous = &frames[current ^ 1];
  const char *rotation_prefix = getenv("SM_CAPTURE_ROTATION_PREFIX");
  if (rotation_prefix && !captured_rotation && continuous_frames(previous, f) &&
      room_state(read16(f->ram, 0x998)) && (read16(f->ram, 0x93f) & 0x8000)) {
    Ppu now, before;
    memcpy(&now, f->lines[100].registers, PPU_SAVESTATE_REGS_SIZE);
    memcpy(&before, previous->lines[100].registers, PPU_SAVESTATE_REGS_SIZE);
    if ((now.bgmode & 7) == 7 && (before.bgmode & 7) == 7 && now.inidisp == 15 && before.inidisp == 15 &&
        abs((int16_t)now.m7matrix[1]) >= 16 &&
        memcmp(now.m7matrix, before.m7matrix, 4 * sizeof(now.m7matrix[0]))) {
      char path[1024];
      int n = snprintf(path, sizeof(path), "%s.previous.capture", rotation_prefix);
      bool saved = n > 0 && n < (int)sizeof(path) && save_capture(previous, path);
      n = snprintf(path, sizeof(path), "%s.current.capture", rotation_prefix);
      if (saved && n > 0 && n < (int)sizeof(path) && save_capture(f, path)) {
        captured_rotation = true;
        fprintf(stderr, "SM rotation capture: frame=%u room=%04x A=%d B=%d\n", f->number,
                read16(f->ram, 0x79b), (int16_t)now.m7matrix[0], (int16_t)now.m7matrix[1]);
      }
    }
  }
  const char *grapple_prefix = getenv("SM_CAPTURE_GRAPPLE_PREFIX");
  const char *grapple_min_env = getenv("SM_CAPTURE_GRAPPLE_MIN_LENGTH");
  unsigned grapple_min = grapple_min_env ? (unsigned)atoi(grapple_min_env) : 32;
  if (!grapple_min || grapple_min > 127) grapple_min = 32;
  if (grapple_prefix && !captured_grapple && continuous_frames(previous, f) &&
      room_state(read16(f->ram, 0x998)) &&
      read16(f->object_ram, 0xd32) != 0xc4f0 &&
      read16(f->object_ram, 0xcfe) >= grapple_min && read16(f->object_ram, 0xcfe) < 128) {
    Ppu raster;
    memcpy(&raster, f->lines[64].registers, PPU_SAVESTATE_REGS_SIZE);
    if (raster.inidisp == 15 && (raster.screenEnabled[0] & 16)) {
      char path[1024];
      int n = snprintf(path, sizeof(path), "%s.previous.capture", grapple_prefix);
      bool saved = n > 0 && n < (int)sizeof(path) && save_capture(previous, path);
      n = snprintf(path, sizeof(path), "%s.current.capture", grapple_prefix);
      if (saved && n > 0 && n < (int)sizeof(path) && save_capture(f, path)) {
        captured_grapple = true;
        fprintf(stderr, "SM grapple capture: frame=%u length=%u function=%04x\n", f->number,
                read16(f->object_ram, 0xcfe), read16(f->object_ram, 0xd32));
      }
    }
  }
  const char *rooms_prefix = getenv("SM_CAPTURE_ROOMS_PREFIX");
  if (rooms_prefix && captured_room_count < 128) {
    Ppu raster;
    memcpy(&raster, f->lines[64].registers, PPU_SAVESTATE_REGS_SIZE);
    unsigned mode = raster.bgmode & 7;
    unsigned key = read16(f->ram, 0x79b) | (mode << 16);
    bool ready = room_state(read16(f->ram, 0x998)) && (mode == 1 || mode == 7);
    for (unsigned y = 32; ready && y < 224; ++y) {
      memcpy(&raster, f->lines[y].registers, PPU_SAVESTATE_REGS_SIZE);
      ready = raster.inidisp == 15 && (raster.bgmode & 7) == mode;
    }
    if (!ready || key != stable_room_key) stable_room_frames = 0;
    stable_room_key = key;
    if (ready && ++stable_room_frames == 5) {
      bool seen = false;
      for (unsigned i = 0; i < captured_room_count; ++i) seen |= captured_rooms[i] == key;
      if (!seen) {
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s.%04x.m%u.capture", rooms_prefix, key & 0xffff, mode);
        if (n > 0 && n < (int)sizeof(path) && save_capture(f, path)) {
          captured_rooms[captured_room_count++] = key;
          fprintf(stderr, "SM room capture: frame=%u room=%04x mode=%u\n", f->number, key & 0xffff, mode);
        }
      }
    }
  }
  const char *power_bomb_prefix = getenv("SM_CAPTURE_POWERBOMB_PREFIX");
  if (power_bomb_prefix && (read16(f->ram, 0x592) & 0x8000)) {
    static const unsigned phases[] = {0x90df, 0x91a8, 0x8de9, 0x8eb2, 0x8b98};
    for (unsigned slot = 0; slot < 6; ++slot) {
      unsigned instruction = read16(f->ram, 0x18f0 + slot * 2);
      for (unsigned phase = 0; phase < 5; ++phase) {
        if (instruction != phases[phase] || (captured_power_bomb_phases & (1u << phase))) continue;
        /* Setup and the first generated table can precede the next NMI's
         * pointer latch. Retain a later observation for raster comparison. */
        if (++power_bomb_phase_observations[phase] < 3) continue;
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s.%04x.capture", power_bomb_prefix, instruction);
        if (n > 0 && n < (int)sizeof(path) && save_capture(f, path)) {
          captured_power_bomb_phases |= 1u << phase;
          fprintf(stderr, "SM power-bomb capture: frame=%u phase=%04x slot=%u\n", f->number, instruction, slot);
        }
      }
    }
  }
  const char *effect_prefix = getenv("SM_CAPTURE_EFFECT_PREFIX");
  if (effect_prefix && !captured_effect && continuous_frames(previous, f) &&
      room_state(read16(f->ram, 0x998))) {
    int slot = moving_effect_slot(f, previous);
    if (slot >= 0) {
      char path[1024];
      int n = snprintf(path, sizeof(path), "%s.previous.capture", effect_prefix);
      bool saved = n > 0 && n < (int)sizeof(path) && save_capture(previous, path);
      n = snprintf(path, sizeof(path), "%s.current.capture", effect_prefix);
      captured_effect = saved && n > 0 && n < (int)sizeof(path) && save_capture(f, path);
      if (captured_effect)
        fprintf(stderr, "SM effect capture: frames=%u/%u B4_slot=%d\n", previous->number, f->number, slot);
    }
  }
  const char *edge_prefix = getenv("SM_CAPTURE_EDGE_PREFIX");
  if (edge_prefix && !captured_edge && continuous_frames(previous, f) &&
      room_state(read16(f->ram, 0x998)) && has_edge_motion(f, previous)) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s.previous.capture", edge_prefix);
    bool saved = n > 0 && n < (int)sizeof(path) && save_capture(previous, path);
    n = snprintf(path, sizeof(path), "%s.current.capture", edge_prefix);
    captured_edge = saved && n > 0 && n < (int)sizeof(path) && save_capture(f, path);
  }
  if (motion_prefix && !captured_motion &&
      (!motion_after || f->number >= strtoul(motion_after, NULL, 10)) &&
      continuous_frames(previous, f) &&
      room_state(read16(f->ram, 0x998)) &&
      read16(f->object_ram, 0xa1c) == read16(previous->object_ram, 0xa1c) &&
      has_samus_motion(f, previous) &&
      (read16(f->object_ram, 0xb04) != read16(previous->object_ram, 0xb04) ||
       read16(f->object_ram, 0xb06) != read16(previous->object_ram, 0xb06))) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s.previous.capture", motion_prefix);
    bool saved = n > 0 && n < (int)sizeof(path) && save_capture(previous, path);
    n = snprintf(path, sizeof(path), "%s.current.capture", motion_prefix);
    captured_motion = saved && n > 0 && n < (int)sizeof(path) && save_capture(f, path);
  }
  const char *mode7_path = getenv("SM_CAPTURE_MODE7");
  if (mode7_path && !captured_mode7 && room_state(read16(f->ram, 0x998))) {
    for (unsigned y = 32; y < 224; ++y) {
      const uint8_t *regs = f->lines[y].registers;
      if ((regs[4] & 7) == 7 && regs[0] == 15) {
        captured_mode7 = SmRendererSaveCapture(mode7_path);
        break;
      }
    }
  }
  const char *mosaic_path = getenv("SM_CAPTURE_MOSAIC");
  if (mosaic_path && !captured_mosaic && room_state(read16(f->ram, 0x998))) {
    for (unsigned y = 32; y < 224; ++y) {
      Ppu raster;
      memcpy(&raster, f->lines[y].registers, PPU_SAVESTATE_REGS_SIZE);
      if ((raster.bgmode & 7) == 1 && !(raster.inidisp & 128) &&
          (raster.inidisp & 15) && raster.mosaic > 15 &&
          (raster.mosaic & (raster.screenEnabled[0] | raster.screenEnabled[1]) & 7)) {
        captured_mosaic = SmRendererSaveCapture(mosaic_path);
        break;
      }
    }
  }
  return true;
}

/* $7F:0002 block map and $7E:A000 block definitions, matching the
 * decomp's row/column streamer. Invalid coordinates are transparent, never
 * wrapped to unrelated room data. No scroll permissions/AI are modified. */
bool SmRendererRoomTile(const uint8_t ram[0x20000], int x, int y,
                        uint16_t *entry) {
  return SmRendererRoomLayerTile(ram, 0, x, y, entry);
}
bool SmRendererRoomLayerTile(const uint8_t ram[0x20000], unsigned layer,
                            int x, int y, uint16_t *entry) {
  if (!ram || !entry || layer > 1 || x < 0 || y < 0) return false;
  unsigned w = read16(ram, 0x7a5), h = read16(ram, 0x7a7);
  unsigned bx = (unsigned)x / 16, by = (unsigned)y / 16;
  /* LoadLevelDataAndOtherThings reserves 0x6400 bytes per layer. */
  if (!w || !h || (uint64_t)w * h > 0x3200 || bx >= w || by >= h) return false;
  unsigned address = (layer ? 0x19602 : 0x10002) + 2 * (by * w + bx);
  if (address >= 0x20000 - 1) return false;
  unsigned block = read16(ram, address);
  unsigned qx = ((unsigned)x / 8 & 1) ^ ((block >> 10) & 1);
  unsigned qy = ((unsigned)y / 8 & 1) ^ ((block >> 11) & 1);
  unsigned table = 0xa000 + (block & 1023) * 8 + (qy * 2 + qx) * 2;
  *entry = (uint16_t)(read16(ram, table) ^ ((block & 0xc00) << 4));
  return true;
}
static unsigned tile_pixel(const uint16_t *vram, unsigned address, int x, int y, int bpp) {
  unsigned a = (address + y) & 0x7fff;
  unsigned shift = 7 - x;
  unsigned bits = vram[a] >> shift;
  unsigned pixel = (bits & 1) | ((bits >> 7) & 2);
  if (bpp == 4) {
    bits = vram[(a + 8) & 0x7fff] >> shift;
    pixel |= ((bits & 1) << 2) | ((bits >> 5) & 8);
  }
  return pixel;
}

/* Host-only reconstruction of $91:CB8E's revealed block map. The high bit
 * marks replacement blocks, whose quadrant swaps do not flip tile pixels. */
static uint16_t xray_blocks[0x3200];
static const uint8_t *xray_rule(const uint8_t *ram, unsigned position) {
  unsigned type = read16(ram, 0x10002 + 2 * position) & 0xf000;
  unsigned bts = ram[0x16402 + position];
  for (unsigned i = 0; i < 17; ++i) {
    const uint8_t *entry = rom_bytes(0x91, 0xd2d6 + 4 * i, 4);
    if (!entry || read16(entry, 0) == 0xffff) return NULL;
    if (read16(entry, 0) != type) continue;
    unsigned list = read16(entry, 2);
    for (unsigned j = 0; j < 257; ++j) {
      const uint8_t *match = rom_bytes(0x91, list + 4 * j, 4);
      if (!match || read16(match, 0) == 0xffff) return NULL;
      if (read16(match, 0) == 0xff00 || read16(match, 0) == bts)
        return rom_bytes(0x91, read16(match, 2), 10);
    }
    return NULL;
  }
  return NULL;
}
static uint16_t xray_entry(const uint8_t *ram, unsigned block, unsigned quadrant) {
  unsigned q = quadrant ^ ((block >> 10) & 3);
  unsigned value = read16(ram, 0xa000 + (block & 1023) * 8 + q * 2);
  return (uint16_t)(value ^ ((block & 0x1000) ? 0 : (block & 0xc00) << 4));
}
bool SmRendererXrayBlocks(const uint8_t ram[0x20000], uint16_t blocks[0x3200]) {
  if (!ram || !blocks) return false;
  unsigned w = read16(ram, 0x7a5), h = read16(ram, 0x7a7);
  if (!w || !h || (uint64_t)w * h > 0x3200) return false;
  unsigned count = w * h;
  for (unsigned i = 0; i < count; ++i) blocks[i] = (uint16_t)(read16(ram, 0x10002 + 2 * i) & 0xfff);
  for (unsigned i = 0; i < count; ++i) {
    const uint8_t *rule = xray_rule(ram, i);
    if (!rule) continue;
    unsigned function = read16(rule, 0);
    if (function == 0xce79 || function == 0xcebb) {
      int x = (int)(i % w), y = (int)(i / w);
      bool vertical = function == 0xce79;
      int step = (int8_t)ram[0x16402 + i];
      unsigned target = i;
      bool resolved = !step;
      for (unsigned n = 0; step && n < count; ++n) {
        if (vertical) y += step; else x += step;
        if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) {
          blocks[i] = 0x10ff; break;
        }
        target = (unsigned)y * w + (unsigned)x;
        step = (int8_t)ram[0x16402 + target];
        unsigned type = read16(ram, 0x10002 + 2 * target) & 0xf000;
        if (type == 0xd000) vertical = true;
        else if (type == 0x5000) { if (step < 0) vertical = false; }
        else { resolved = true; break; }
      }
      if (resolved && (read16(ram, 0x10002 + 2 * target) & 0xf000) == 0x3000) {
        const uint8_t *target_rule = xray_rule(ram, target);
        if (target_rule) blocks[i] = (uint16_t)(0x1000 | (read16(target_rule, 2) & 1023));
      }
      continue;
    }
    if (function == 0xcf3e && read16(ram, 0x79f) != 1) continue;
    if (function != 0xcf36 && function != 0xcf3e && function != 0xcf4e && function != 0xcf62 && function != 0xcf6f) return false;
    blocks[i] = (uint16_t)(0x1000 | (read16(rule, 2) & 1023));
    if ((function == 0xcf4e || function == 0xcf6f) && i % w + 1 < w)
      blocks[i + 1] = (uint16_t)(0x1000 | (read16(rule, 4) & 1023));
    if ((function == 0xcf62 || function == 0xcf6f) && i + w < count)
      blocks[i + w] = (uint16_t)(0x1000 | (read16(rule, function == 0xcf62 ? 2 : 6) & 1023));
    if (function == 0xcf6f && i % w + 1 < w && i + w + 1 < count)
      blocks[i + w + 1] = (uint16_t)(0x1000 | (read16(rule, 8) & 1023));
  }
  /* Uncollected item PLMs override generic block reveals, in guest order. */
  for (int slot = 39; slot >= 0; --slot) {
    if (read16(ram, 0x1c37 + 2 * slot) < 0xdf89) continue;
    unsigned item = read16(ram, 0x1dc7 + 2 * slot);
    if (item >= 512 || (ram[0xd870 + item / 8] & (1u << (item & 7)))) continue;
    unsigned variable = read16(ram, 0xdf0c + 2 * slot);
    const uint8_t *pointer = rom_bytes(0x84, 0x839d + (variable & ~1u), 2);
    const uint8_t *drawing = pointer ? rom_bytes(0x84, read16(pointer, 0), 4) : NULL;
    unsigned block = read16(ram, 0x1c87 + 2 * slot) / 2;
    if (drawing && block < count) blocks[block] = (uint16_t)(0x1000 | (read16(drawing, 2) & 0xbff));
  }
  const uint8_t *state = rom_bytes(0x8f, read16(ram, 0x7bb), 26);
  unsigned specials = state ? read16(state, 16) : 0;
  for (unsigned i = 0; specials && i < count; ++i) {
    const uint8_t *entry = rom_bytes(0x8f, specials + 4 * i, 4);
    if (!entry) return false;
    if (!entry[0] && !entry[1]) break;
    if (entry[0] < w && entry[1] < h)
      blocks[entry[1] * w + entry[0]] = (uint16_t)(0x1000 | (read16(entry, 2) & 0xbff));
  }
  return true;
}

typedef struct SmBackgroundLine {
  int shift, size, scroll_x, scroll_y, world_x, world_y, bpp;
  unsigned layer, sc, base, low, high;
  const uint8_t *room;
  const uint16_t *revealed;
} SmBackgroundLine;

static SmBackgroundLine background_line(const Ppu *p, const uint8_t *room, unsigned layer) {
  static const unsigned low[] = {8, 7, 1}, high[] = {12, 11, 3};
  SmBackgroundLine b = {0};
  b.layer = layer;
  b.shift = PPU_bigTiles(p, layer) ? 4 : 3;
  b.size = 1 << b.shift;
  b.scroll_x = p->hScroll[layer]; b.scroll_y = p->vScroll[layer];
  b.sc = p->bgXsc[layer];
  b.bpp = layer == 2 ? 2 : 4;
  b.base = ((p->bgTileAdr >> (layer * 4)) & 15) * 4096;
  b.low = low[layer];
  b.high = layer == 2 && (p->bgmode & 8) ? 15 : high[layer];
  if (b.size == 8 && (layer == 0 || (layer == 1 && !(room[0x91b] & 1)))) {
    /* Native scroll includes ring-buffer offsets. Recover the raster delta
     * relative to the live camera, then sample decompressed world data. */
    int camera_x = (int)read16(room, layer ? 0x917 : 0x911);
    int camera_y = (int)read16(room, layer ? 0x919 : 0x915);
    int dx = ((int)p->hScroll[layer] - camera_x -
              (int)read16(room, layer ? 0x921 : 0x91d) + 512) & 1023;
    int dy = ((int)p->vScroll[layer] - camera_y -
              (int)read16(room, layer ? 0x923 : 0x91f) + 512) & 1023;
    b.world_x = camera_x + dx - 512;
    b.world_y = camera_y + dy - 512;
    b.room = room;
  }
  return b;
}

static uint16_t background_pixel(const SmBackgroundLine *b, const uint16_t *vram,
                                 int x, int y, bool margin) {
  int size = b->size;
  int px, py;
  unsigned tile;
  if (margin && b->room) {
    int wx = b->world_x + x, wy = b->world_y + y;
    uint16_t entry;
    if (b->revealed) {
      unsigned w = read16(b->room, 0x7a5), h = read16(b->room, 0x7a7);
      if (wx < 0 || wy < 0 || (unsigned)wx / 16 >= w || (unsigned)wy / 16 >= h) return 0;
      entry = xray_entry(b->room, b->revealed[(unsigned)wy / 16 * w + (unsigned)wx / 16],
                         ((unsigned)wx / 8 & 1) + ((unsigned)wy / 8 & 1) * 2);
    } else if (!SmRendererRoomLayerTile(b->room, b->layer, wx, wy, &entry)) return 0;
    tile = entry;
    px = wx & 1023;
    py = wy & 1023;
  } else {
    px = (x + b->scroll_x) & 1023; py = (y + b->scroll_y) & 1023;
    int tx = px >> b->shift, ty = py >> b->shift;
    unsigned sc = b->sc;
    unsigned address = (sc & 0xfc) * 256 + (tx & 31) + (ty & 31) * 32;
    if ((sc & 1) && (tx & 32)) address += 1024;
    if ((sc & 2) && (ty & 32)) address += (sc & 1) ? 2048 : 1024;
    tile = vram[address & 0x7fff];
  }
  int cx = px & (size - 1), cy = py & (size - 1);
  if (tile & 0x4000) cx = size - 1 - cx;
  if (tile & 0x8000) cy = size - 1 - cy;
  unsigned number = ((tile & 1023) + cx / 8 + (cy / 8) * 16) & 1023;
  unsigned pixel = tile_pixel(vram, b->base + number * (b->bpp * 4), cx & 7, cy & 7, b->bpp);
  if (!pixel) return 0;
  unsigned priority = tile & 0x2000 ? b->high : b->low;
  unsigned palette = ((tile >> 10) & 7) * (1u << b->bpp);
  return (priority << 12) | (b->layer << 8) | palette | pixel;
}

/* background_pixel for x0..x0+n-1 of one line (no mosaic), a tile chunk at a
 * time: within an 8-pixel-aligned chunk the tile entry, character number and
 * row are fixed, so each chunk is one lookup + one decode + a copy. */
/* plane_expand[b]: byte `col` (0..7) holds bit (7 - col) of b, so the four
 * bitplanes of one tile row combine into eight 4-bit pixels with three
 * shifts and ORs instead of a per-column bit shuffle. */
static uint64_t plane_expand[256];
static void plane_expand_init(void) {
  static bool ready;
  if (ready) return;
  for (unsigned v = 0; v < 256; ++v) {
    uint64_t e = 0;
    for (unsigned col = 0; col < 8; ++col)
      if (v & (0x80u >> col)) e |= (uint64_t)1 << (col * 8);
    plane_expand[v] = e;
  }
  ready = true;
}
static void background_row(const SmBackgroundLine *b, const uint16_t *vram,
                           int y, int x0, int n, uint16_t *row) {
  const int size = b->size;
  int i = 0;
  while (i < n) {
    const int x = x0 + i;
    /* Where the margin/native status (and so the sampling path) changes. */
    int region_end = x < 0 ? -x0 : x < 256 ? 256 - x0 : n;
    if (region_end > n) region_end = n;
    const bool room = (x < 0 || x >= 256) && b->room;
    unsigned tile;
    int px, py, run;
    if (room) {
      const int wx = b->world_x + x, wy = b->world_y + y;
      run = 8 - (wx & 7);
      if (run > region_end - i) run = region_end - i;
      uint16_t entry = 0;
      bool have;
      if (b->revealed) {
        unsigned w = read16(b->room, 0x7a5), h = read16(b->room, 0x7a7);
        have = !(wx < 0 || wy < 0 || (unsigned)wx / 16 >= w || (unsigned)wy / 16 >= h);
        if (have)
          entry = xray_entry(b->room, b->revealed[(unsigned)wy / 16 * w + (unsigned)wx / 16],
                             ((unsigned)wx / 8 & 1) + ((unsigned)wy / 8 & 1) * 2);
      } else {
        have = SmRendererRoomLayerTile(b->room, b->layer, wx, wy, &entry);
      }
      if (!have) {
        for (int k = 0; k < run; ++k) row[i + k] = 0;
        i += run;
        continue;
      }
      tile = entry;
      px = wx & 1023;
      py = wy & 1023;
    } else {
      px = (x + b->scroll_x) & 1023;
      py = (y + b->scroll_y) & 1023;
      run = 8 - (px & 7);
      if (run > region_end - i) run = region_end - i;
      int tx = px >> b->shift, ty = py >> b->shift;
      unsigned sc = b->sc;
      unsigned address = (sc & 0xfc) * 256 + (tx & 31) + (ty & 31) * 32;
      if ((sc & 1) && (tx & 32)) address += 1024;
      if ((sc & 2) && (ty & 32)) address += (sc & 1) ? 2048 : 1024;
      tile = vram[address & 0x7fff];
    }
    int cx = px & (size - 1), cy = py & (size - 1);
    if (tile & 0x4000) cx = size - 1 - cx;
    if (tile & 0x8000) cy = size - 1 - cy;
    unsigned number = ((tile & 1023) + ((unsigned)cx >> 3) + ((unsigned)cy >> 3) * 16) & 1023;
    unsigned priority = tile & 0x2000 ? b->high : b->low;
    unsigned palette = ((tile >> 10) & 7) * (1u << b->bpp);
    unsigned head = (priority << 12) | (b->layer << 8) | palette;
    unsigned address = b->base + number * (b->bpp * 4);
    uint16_t decoded[8];
    /* tile_pixel for columns 0..7, each bitplane word read once. */
    unsigned a = (address + (unsigned)(cy & 7)) & 0x7fff;
    unsigned w01 = vram[a], w23 = b->bpp == 4 ? vram[(a + 8) & 0x7fff] : 0;
    uint64_t packed = plane_expand[w01 & 255] | plane_expand[w01 >> 8] << 1 |
                      plane_expand[w23 & 255] << 2 | plane_expand[w23 >> 8] << 3;
    if (!packed) {
      for (int k = 0; k < run; ++k) row[i + k] = 0;
      i += run;
      continue;
    }
    for (int col = 0; col < 8; ++col) {
      unsigned pixel = (unsigned)(packed >> (col * 8)) & 15;
      decoded[col] = pixel ? (uint16_t)(head | pixel) : 0;
    }
    const int start = px & 7;   /* chunk-relative column of the first pixel */
    if (tile & 0x4000)
      for (int k = 0; k < run; ++k) row[i + k] = decoded[7 - (start + k)];
    else
      for (int k = 0; k < run; ++k) row[i + k] = decoded[start + k];
    i += run;
  }
}

typedef struct SmWideWindow { bool valid; int left, right; } SmWideWindow;

static int q8_floor(int value) {
  return value >= 0 ? value / 256 : -((-value + 255) / 256);
}

bool SmRendererXrayBounds(const uint16_t tangent[129], int x, int y,
                          unsigned angle, unsigned half_width,
                          int32_t left[230], int32_t right[230]) {
  if (!tangent || !left || !right || x < -256 || x > 511 || y < 1 || y > 230 ||
      angle > 256 || half_width > 10) return false;
  int a = (int)angle - (int)half_width, b = (int)angle + (int)half_width;
  if (a < 0) a += 256;
  if (b > 256) b -= 256;
  int sa = tangent[a < 128 ? a : a - 128];
  int sb = tangent[b < 128 ? b : b - 128];
  int kind = a < 128 ? (a >= 64 ? 2 : b >= 64 ? 0 : 1) :
                      (a >= 192 ? 1 : b >= 192 ? 3 : 2);
  bool horizontal = !half_width && (angle == 64 || angle == 192);
  bool offscreen = x < 0 || x >= 256;
  for (int row = 0; row < 230; ++row) {
    int delta = row - (y - 1), distance = abs(delta);
    int l = 1, r = 0;
    if (offscreen) {
      /* $91:BE11 selects distinct offscreen routines. There is no apex
       * row: both ramps advance before their first write. Retain the
       * original 16-bit source and each routine's signed bank correction. */
      int base = (x * 256) & 0xffff;
      distance = delta <= 0 ? 1 - delta : delta;
      if (horizontal) {
        if (!delta) { l = -32768; r = 32768; }
      } else if (kind == 0) {
        l = q8_floor(base - 65536 + distance * (delta <= 0 ? sa : sb)); r = 32768;
      } else if (kind == 3) {
        l = -32768; r = q8_floor(base + 65536 - distance * (delta <= 0 ? sb : sa));
      } else if ((kind == 1 && delta <= 0) || (kind == 2 && delta > 0)) {
        int ls, rs, lb, rb;
        if (kind == 1) {
          ls = a < 192 ? sa : -sa; lb = a < 192 ? -65536 : 65536;
          rs = b < 192 ? sb : -sb; rb = b < 192 ? -65536 : 65536;
        } else {
          ls = b < 128 ? sb : -sb; lb = b < 128 ? -65536 : 65536;
          rs = a < 128 ? sa : -sa; rb = a < 128 ? -65536 : 65536;
        }
        l = q8_floor(base + lb + distance * ls);
        r = q8_floor(base + rb + distance * rs);
      }
      left[row] = l; right[row] = r;
      continue;
    }
    if (horizontal) {
      if (!delta) { l = angle == 64 ? x : -32768; r = angle == 64 ? 32768 : x; }
    } else if (kind == 0) {
      l = q8_floor(x * 256 + distance * (delta < 0 ? sa : sb)); r = 32768;
    } else if (kind == 3) {
      l = -32768; r = q8_floor(x * 256 - distance * (delta < 0 ? sb : sa));
    } else if ((kind == 1 && delta <= 0) || (kind == 2 && delta >= 0)) {
      int ls, rs;
      if (kind == 1) {
        ls = a < 192 ? sa : -sa;
        rs = b < 192 ? sb : -sb;
      } else {
        ls = b < 128 ? sb : -sb;
        rs = a < 128 ? sa : -sa;
      }
      l = q8_floor(x * 256 + distance * ls);
      r = q8_floor(x * 256 + distance * rs);
    }
    left[row] = l; right[row] = r;
  }
  return true;
}

static bool xray_windows(const SmSourceFrame *f, SmWideWindow rows[224]) {
  if (!read16(f->ram, 0xa78) || read16(f->ram, 0xa7a) > 2 ||
      read16(f->ram, 0x9d2) != 5) return false;
  const uint8_t *table = rom_bytes(0x91, 0xc9d4, 258);
  if (!table) return false;
  uint16_t tangent[129];
  for (unsigned i = 0; i < 129; ++i) tangent[i] = (uint16_t)read16(table, 2 * i);
  int x = (int)read16(f->ram, 0xaf6) - (int)read16(f->ram, 0x911) +
          (f->ram[0xa1e] == 4 ? -3 : 3);
  int y = (int)read16(f->ram, 0xafa) - (int)read16(f->ram, 0x915) -
          (f->ram[0xa1f] == 5 ? 12 : 16);
  int32_t left[230], right[230];
  if (!SmRendererXrayBounds(tangent, x, y, read16(f->ram, 0xa82),
                            read16(f->ram, 0xa84), left, right)) return false;
  /* Match the generated native table before trusting its extrapolation.
   * Do not infer an angle from clipped/empty rows. */
  for (unsigned row = 0; row < 230; ++row) {
    int l = left[row], r = right[row];
    unsigned native = read16(f->ram, 0x9800 + row * 2);
    bool empty = l > r || l > 255 || r < 0;
    if (empty ? (native & 255) <= (native >> 8) :
        (native & 255) != (unsigned)(l < 0 ? 0 : l) ||
        (native >> 8) != (unsigned)(r > 255 ? 255 : r)) return false;
  }
  for (unsigned row = 0; row < 224; ++row) {
    Ppu raster;
    memcpy(&raster, f->lines[row].registers, PPU_SAVESTATE_REGS_SIZE);
    unsigned native = read16(f->ram, 0x9800 + (row + 1) * 2);
    if (raster.window2left != (native & 255) || raster.window2right != (native >> 8)) continue;
    rows[row] = (SmWideWindow){true, left[row + 1], right[row + 1]};
  }
  return true;
}

/* NMI latches the indirect pointers before the game builds this frame's
 * row bytes. Use that same ordering, and verify both the generated table
 * and each raster register before extrapolating beyond native clipping. */
static void power_bomb_windows(const SmSourceFrame *f, SmWideWindow rows[224]) {
  memset(rows, 0, 224 * sizeof(*rows));
  if (!f->object_state_valid || !(read16(f->ram, 0x592) & 0x8000)) return;
  const uint8_t *old = f->object_ram;
  int center = (int)read16(f->ram, 0xce6) - 256;
  if (center < -256 || center > 511) return;
  for (unsigned slot = 0; slot < 5; ++slot) {
    unsigned phase = read16(f->ram, 0x18f0 + slot * 2);
    int16_t widths[192];
    if (phase == 0x90df || phase == 0x8de9) {
      const uint8_t *profile = rom_bytes(0x88, 0xa206, 160);
      if (!SmRendererPowerBombWidths(profile,
          read16(old, phase == 0x90df ? 0xcec : 0xcea) >> 8, widths)) continue;
    } else if (phase == 0x91a8 || phase == 0x8eb2 || phase == 0x8b98) {
      unsigned address = read16(old, 0xcf2);
      if (phase == 0x8b98) address -= 192;
      if (address < 0x9246 || address > 0xa146 || (address - 0x9246) % 192) continue;
      const uint8_t *profile = rom_bytes(0x88, address, 192);
      if (!profile) continue;
      bool ended = false;
      for (unsigned i = 0; i < 192; ++i) {
        ended |= profile[i] == 0;
        widths[i] = ended ? -1 : profile[i];
      }
    } else continue;
    bool valid = true;
    for (unsigned i = 0; i < 192; ++i) {
      int left = center - widths[i], right = center + widths[i];
      bool empty = widths[i] < 0 || right < 0 || left > 255;
      unsigned actual_left = f->ram[0xc406 + i], actual_right = f->ram[0xc506 + i];
      if (empty ? actual_left <= actual_right :
          actual_left != (unsigned)(left < 0 ? 0 : left) ||
          actual_right != (unsigned)(right > 255 ? 255 : right)) valid = false;
    }
    if (!valid) continue;
    unsigned pointer = read16(old, 0x18d8 + slot * 2);
    if (pointer < 0x9800 || pointer > 0xa100 || (pointer - 0x9800) % 3 ||
        read16(old, 0x18da + slot * 2) != pointer + 0x901) continue;
    for (unsigned y = 0; y < 224; ++y) {
      unsigned address = pointer + 3 * (y + 1);
      if (address > 0xa0fd) continue;
      const uint8_t *left = rom_bytes(0x89, address, 3);
      const uint8_t *right = rom_bytes(0x89, address + 0x901, 3);
      if (!left || !right || left[0] != 0x81 || right[0] != 0x81) continue;
      unsigned target = read16(left, 1), target_right = read16(right, 1);
      int width;
      if (target == 0xc606 && target_right == 0xc607) width = -1;
      else if (target >= 0xc406 && target < 0xc4c6 && target_right == target + 256)
        width = widths[target - 0xc406];
      else continue;
      Ppu raster;
      memcpy(&raster, f->lines[y].registers, PPU_SAVESTATE_REGS_SIZE);
      if (raster.window2left != f->ram[target] || raster.window2right != f->ram[target_right]) continue;
      rows[y] = (SmWideWindow){true, width < 0 ? 1 : center - width,
                                    width < 0 ? 0 : center + width};
    }
    return;
  }
}

static bool in_window(const Ppu *p, int layer, int x, int extra, const SmWideWindow *wide) {
  unsigned flags = (p->windowsel >> (layer * 4)) & 15;
  bool enabled1 = (flags & 2) != 0, enabled2 = (flags & 8) != 0;
  int l1 = p->window1left == 0 ? -extra : p->window1left;
  int r1 = p->window1right == 255 ? 255 + extra : p->window1right;
  int l2 = p->window2left == 0 ? -extra : p->window2left;
  int r2 = p->window2right == 255 ? 255 + extra : p->window2right;
  if (wide->valid && (x < 0 || x >= 256)) { l2 = wide->left; r2 = wide->right; }
  bool a = (x >= l1 && x <= r1) != ((flags & 1) != 0);
  bool b = (x >= l2 && x <= r2) != ((flags & 4) != 0);
  if (!enabled1) return enabled2 && b;
  if (!enabled2) return a;
  switch ((p->wbgobjlog >> (layer * 2)) & 3) {
  case 0: return a || b;
  case 1: return a && b;
  case 2: return a != b;
  default: return a == b;
  }
}

/* in_window(p, layer, x, extra, wide) for x = -extra .. width-extra-1, as a
 * mask row: the window edges and logic are resolved once per line. */
static void window_row(const Ppu *p, int layer, int extra, const SmWideWindow *wide,
                       int width, uint8_t *mask) {
  unsigned flags = (p->windowsel >> (layer * 4)) & 15;
  bool enabled1 = (flags & 2) != 0, enabled2 = (flags & 8) != 0;
  bool inv1 = (flags & 1) != 0, inv2 = (flags & 4) != 0;
  int l1 = p->window1left == 0 ? -extra : p->window1left;
  int r1 = p->window1right == 255 ? 255 + extra : p->window1right;
  int l2 = p->window2left == 0 ? -extra : p->window2left;
  int r2 = p->window2right == 255 ? 255 + extra : p->window2right;
  unsigned logic = (p->wbgobjlog >> (layer * 2)) & 3;
  for (int sx = 0; sx < width; ++sx) {
    int x = sx - extra, wl2 = l2, wr2 = r2;
    if (wide->valid && (x < 0 || x >= 256)) { wl2 = wide->left; wr2 = wide->right; }
    bool a = (x >= l1 && x <= r1) != inv1;
    bool b = (x >= wl2 && x <= wr2) != inv2;
    bool inside;
    if (!enabled1) inside = enabled2 && b;
    else if (!enabled2) inside = a;
    else inside = logic == 0 ? (a || b) : logic == 1 ? (a && b) : logic == 2 ? (a != b) : (a == b);
    mask[sx] = inside;
  }
}

static bool player_projectile_visible(const uint8_t *ram, unsigned slot) {
  unsigned offset = slot * 2, type = read16(ram, 0xc18 + offset);
  if (!read16(ram, 0xc40 + offset)) return false;
  unsigned category = type & 0xf00;
  if (category >= 0x300)
    return category != 0x300 || read16(ram, 0xc7c + offset) != 0;
  if (slot >= 5) return false;
  unsigned nmi = read16(ram, 0x5b6);
  if (type & 0xf10) return true;
  if (type & 12) return ((slot & 1) != 0) == ((nmi & 2) != 0);
  return ((slot & 1) != 0) != ((nmi & 1) != 0);
}
typedef struct SmProjectilePose {
  unsigned bank, map, palette, base, identity;
  int x, y, world_x, world_y;
} SmProjectilePose;
static void project_player_position(const uint8_t *ram, unsigned type,
                                    int world_x, int world_y, int *x, int *y) {
  unsigned category = type & 0xf00;
  if ((read16(ram, 0x93f) & 0x8000) && category != 0x300 && category != 0x500)
    SmMode7ObjectPosition((uint16_t)read16(ram, 0x78), (uint16_t)read16(ram, 0x7a),
        (uint16_t)read16(ram, 0x7c), (uint16_t)read16(ram, 0x80),
        (uint16_t)read16(ram, 0x82), (uint16_t)world_x, (uint16_t)world_y,
        (uint16_t)read16(ram, 0x911), (uint16_t)read16(ram, 0x915), x, y);
}
static bool projectile_pose(const SmSourceFrame *f, bool enemy, unsigned slot,
                            SmProjectilePose *pose) {
  const uint8_t *ram = f->object_ram;
  unsigned offset = slot * 2;
  if (enemy) {
    pose->identity = read16(ram, 0x1997 + offset);
    if (!pose->identity || read16(ram, 0x1840)) return false;
    unsigned gfx = read16(ram, 0x19bb + offset);
    pose->bank = 0x8d; pose->map = read16(ram, 0x1b6b + offset);
    pose->palette = gfx & 0xff00; pose->base = gfx & 255;
    pose->world_x = (int)read16(ram, 0x1a4b + offset);
    pose->world_y = (int)read16(ram, 0x1a93 + offset);
  } else {
    if (!player_projectile_visible(ram, slot)) return false;
    pose->identity = read16(ram, 0xc18 + offset);
    pose->bank = 0x93; pose->map = read16(ram, 0xcb8 + offset);
    pose->palette = pose->base = 0;
    pose->world_x = (int)read16(ram, 0xb64 + offset);
    pose->world_y = (int)read16(ram, 0xb78 + offset);
  }
  pose->x = pose->world_x - (int)read16(ram, 0x911);
  pose->y = pose->world_y - (int)read16(ram, 0x915);
  if (!enemy)
    project_player_position(ram, pose->identity, pose->world_x, pose->world_y, &pose->x, &pose->y);
  return pose->map >= 0x8000;
}
static bool projectile_motion(const SmSourceFrame *f, const SmSourceFrame *old,
                              bool enemy, unsigned slot,
                              SmProjectilePose *now, SmProjectilePose *before) {
  if (!old || !f->object_state_valid || !old->object_state_valid ||
      !projectile_pose(f, enemy, slot, now) || !projectile_pose(old, enemy, slot, before))
    return false;
  if (enemy && read16(f->object_ram, 0x1bd7 + slot * 2) !=
               read16(old->object_ram, 0x1bd7 + slot * 2)) return false;
  return now->identity == before->identity &&
      abs(now->world_x - before->world_x) <= 32 &&
      abs(now->world_y - before->world_y) <= 32 &&
      abs(now->x - before->x) <= 32 && abs(now->y - before->y) <= 32;
}
static bool sprite_object_pose(const SmSourceFrame *f, unsigned slot, SmProjectilePose *pose) {
  const uint8_t *ram = f->object_ram;
  unsigned offset = slot * 2;
  pose->identity = read16(ram, 0xef78 + offset);
  const uint8_t *data = rom_bytes(0xb4, pose->identity, 4);
  if (!data) return false;
  pose->bank = 0xb4; pose->map = read16(data, 2);
  unsigned gfx = read16(ram, 0xf078 + offset);
  pose->palette = gfx & 0xe00; pose->base = gfx & 0x1ff;
  pose->world_x = (int)read16(ram, 0xf0f8 + offset);
  pose->world_y = (int)read16(ram, 0xf1f8 + offset);
  pose->x = pose->world_x - (int)read16(ram, 0x911);
  pose->y = pose->world_y - (int)read16(ram, 0x915);
  return pose->map >= 0x8000 && pose->y >= 0 && pose->y < 272;
}
static bool sprite_object_motion(const SmSourceFrame *f, const SmSourceFrame *old,
                                 unsigned slot, SmProjectilePose *now, SmProjectilePose *before) {
  if (!old || !f->object_state_valid || !old->object_state_valid ||
      !sprite_object_pose(f, slot, now) || !sprite_object_pose(old, slot, before)) return false;
  /* There is no separate owner ID. Admit unchanged instructions and the
   * ordinary four-byte animation advance at timer expiry, not arbitrary
   * jumps/reused instruction lists. Never execute an instruction or timer. */
  bool continuous = now->identity == before->identity ||
      (now->identity == before->identity + 4 && read16(old->object_ram, 0xeff8 + slot * 2) == 1);
  return continuous && now->palette == before->palette && now->base == before->base &&
      abs(now->world_x - before->world_x) <= 32 && abs(now->world_y - before->world_y) <= 32 &&
      abs(now->x - before->x) <= 32 && abs(now->y - before->y) <= 32;
}
static int moving_effect_slot(const SmSourceFrame *f, const SmSourceFrame *old) {
  for (unsigned slot = 0; slot < 32; ++slot) {
    SmProjectilePose now, before;
    if (!sprite_object_motion(f, old, slot, &now, &before) ||
        (now.x == before.x && now.y == before.y) || now.y < 32 || now.y >= 216) continue;
    const SmRasterLine *line = &f->lines[now.y];
    Ppu p;
    memcpy(&p, line->registers, PPU_SAVESTATE_REGS_SIZE);
    if ((p.inidisp & 0x8f) != 15 || !(p.screenEnabled[0] & 16)) continue;
    for (unsigned obj = 0; obj < 128; ++obj) {
      unsigned high = (line->high_oam[obj / 4] >> ((obj & 3) * 2)) & 3;
      int actual_x;
      if (map_piece(now.bank, now.map, now.x, now.y, now.palette, now.base,
                    line->oam[obj * 2], line->oam[obj * 2 + 1], high, &actual_x))
        return (int)slot;
    }
  }
  return -1;
}
static bool grapple_flare_pose(const SmSourceFrame *f, SmProjectilePose *pose) {
  const uint8_t *ram = f->object_ram;
  unsigned frame = read16(ram, 0xcd6), function = read16(ram, 0xd32);
  if (!f->object_state_valid || read16(ram, 0xa5c) != 0xeb86 ||
      !read16(ram, 0xcd0) || function >= 0xc856 || frame > 63) return false;
  const uint8_t *base = rom_bytes(0x93, ram[0xa1e] == 4 ? 0xa22b : 0xa225, 2);
  if (!base) return false;
  unsigned index = read16(base, 0) + frame;
  if (index > 0x7fff) return false;
  const uint8_t *map = rom_bytes(0x93, 0xa1a1 + 2 * index, 2);
  if (!map) return false;
  *pose = (SmProjectilePose){0};
  pose->bank = 0x93; pose->map = read16(map, 0);
  pose->identity = function | ((unsigned)ram[0xa1e] << 16);
  pose->world_x = (int)read16(ram, 0xd1a); pose->world_y = (int)read16(ram, 0xd1c);
  pose->x = pose->world_x - (int)read16(ram, 0x911);
  pose->y = pose->world_y - (int)read16(ram, 0x915);
  return pose->map >= 0x8000 && pose->y >= 0 && pose->y < 256;
}
static bool grapple_flare_motion(const SmSourceFrame *f, const SmSourceFrame *old,
                                  SmProjectilePose *now, SmProjectilePose *before) {
  return old && grapple_flare_pose(f, now) && grapple_flare_pose(old, before) &&
      now->identity == before->identity &&
      abs(now->world_x - before->world_x) <= 32 && abs(now->world_y - before->world_y) <= 32 &&
      abs(now->x - before->x) <= 32 && abs(now->y - before->y) <= 32;
}
static bool projectile_piece_motion(const SmSourceFrame *f, const SmSourceFrame *old,
                                     unsigned position, unsigned attr, unsigned high,
                                     int *dx, int *dy, int *actual_x, bool *effect) {
  bool matched = false;
  *effect = false;
  *dx = *dy = 0;
  *actual_x = 0;
  for (unsigned kind = 0; kind < 4; ++kind) {
    for (unsigned slot = 0; slot < (kind == 3 ? 1u : kind == 2 ? 32u : kind ? 18u : 10u); ++slot) {
      SmProjectilePose now, before;
      bool moving = kind == 3 ? grapple_flare_motion(f, old, &now, &before)
                    : kind == 2 ? sprite_object_motion(f, old, slot, &now, &before)
                             : projectile_motion(f, old, kind != 0, slot, &now, &before);
      if (!moving) continue;
      int ax;
      if (!map_piece(now.bank, now.map, now.x, now.y, now.palette, now.base,
                      position, attr, high, &ax)) continue;
      int mx = before.x - now.x, my = before.y - now.y;
      if (matched && (*dx != mx || *dy != my || *actual_x != ax)) return false;
      *dx = mx; *dy = my; *actual_x = ax; matched = true;
      if (kind == 2) *effect = true;
    }
  }
  return matched && (*dx || *dy);
}
/* OAM normally stays constant across scanlines. Cache ownership only within
 * one presentation, keyed by the current complete entry, so raster OAM changes
 * still force verification and no match can leak across snapshots. */
typedef struct SmMatchCache {
  struct {
    unsigned position, attr, high;
    int dx, dy, actual_x;
    bool valid, matched;
    bool effect;
  } object[128];
  struct {
    unsigned position, attr, high;
    bool valid, matched;
  } samus[128];
  struct {
    unsigned position, attr, high;
    int dx, dy, actual_x;
    bool valid, matched;
  } enemy[128];
} SmMatchCache;
/* One cache per drawing participant (draw_line's slot), cleared per draw. */
static SmMatchCache match_caches[SM_DRAW_SLOTS];
static bool current_enemy_motion(SmMatchCache *mc, unsigned slot, const SmSourceFrame *f,
                                 const SmSourceFrame *old, unsigned position,
                                 unsigned attr, unsigned high, int *dx, int *dy, int *actual_x) {
  high &= 3;
  if (mc->enemy[slot].valid && mc->enemy[slot].position == position &&
      mc->enemy[slot].attr == attr && mc->enemy[slot].high == high) {
    *dx = mc->enemy[slot].dx; *dy = mc->enemy[slot].dy;
    *actual_x = mc->enemy[slot].actual_x;
    return mc->enemy[slot].matched;
  }
  bool matched = false;
  *dx = *dy = 0;
  *actual_x = 0;
  for (unsigned owner = 0; owner < 32; ++owner) {
    int mx, my, ax;
    if (!enemy_motion(f, old, owner, &mx, &my) ||
        !enemy_piece(f, owner, position, attr, high, &ax)) continue;
    /* Overlapping maps can describe the same OAM piece. Do not guess when
     * those owners imply different motion. */
    if (matched && (*dx != mx || *dy != my || *actual_x != ax)) { matched = false; break; }
    *dx = mx; *dy = my; *actual_x = ax; matched = true;
  }
  matched = matched && (*dx || *dy);
  mc->enemy[slot].position = position;
  mc->enemy[slot].attr = attr;
  mc->enemy[slot].high = high;
  mc->enemy[slot].dx = *dx; mc->enemy[slot].dy = *dy;
  mc->enemy[slot].actual_x = *actual_x;
  mc->enemy[slot].valid = true; mc->enemy[slot].matched = matched;
  return matched;
}
static bool current_samus_piece(SmMatchCache *mc, unsigned slot, const SmSourceFrame *f,
                                unsigned position, unsigned attr, unsigned high) {
  high &= 3;
  if (mc->samus[slot].valid && mc->samus[slot].position == position &&
      mc->samus[slot].attr == attr && mc->samus[slot].high == high)
    return mc->samus[slot].matched;
  bool matched = samus_piece(f, position, attr, high);
  mc->samus[slot].position = position;
  mc->samus[slot].attr = attr;
  mc->samus[slot].high = high;
  mc->samus[slot].valid = true;
  mc->samus[slot].matched = matched;
  return matched;
}
static bool object_motion_match(SmMatchCache *mc, unsigned slot, const SmSourceFrame *f,
                                 const SmSourceFrame *old, unsigned position,
                                 unsigned attr, unsigned high, int *dx, int *dy, int *actual_x) {
  high &= 3;
  if (mc->object[slot].valid && mc->object[slot].position == position &&
      mc->object[slot].attr == attr && mc->object[slot].high == high) {
    *dx = mc->object[slot].dx; *dy = mc->object[slot].dy;
    *actual_x = mc->object[slot].actual_x;
    return mc->object[slot].matched;
  }
  bool matched = projectile_piece_motion(f, old, position, attr, high, dx, dy, actual_x,
                                         &mc->object[slot].effect);
  mc->object[slot].position = position;
  mc->object[slot].attr = attr;
  mc->object[slot].high = high;
  mc->object[slot].dx = *dx; mc->object[slot].dy = *dy;
  mc->object[slot].actual_x = *actual_x;
  mc->object[slot].valid = true;
  mc->object[slot].matched = matched;
  return matched;
}
static bool has_edge_motion(const SmSourceFrame *f, const SmSourceFrame *old) {
  SmMatchCache *mc = &match_caches[0];
  if (!f->object_state_valid || !old->object_state_valid) return false;
  memset(mc->enemy, 0, sizeof(mc->enemy));
  memset(mc->object, 0, sizeof(mc->object));
  const SmRasterLine *line = &f->lines[100];
  Ppu p;
  memcpy(&p, line->registers, PPU_SAVESTATE_REGS_SIZE);
  if (!(p.screenEnabled[0] & 16) || (p.inidisp & 128) || !(p.inidisp & 15)) return false;
  static const int sizes[8][2] = {{8,16},{8,32},{8,64},{16,32},
                                  {16,64},{32,64},{16,32},{16,32}};
  for (unsigned slot = 0; slot < 128; ++slot) {
    unsigned position = line->oam[slot * 2], attr = line->oam[slot * 2 + 1];
    unsigned high = line->high_oam[slot / 4] >> ((slot % 4) * 2);
    int size = sizes[p.obsel >> 5][(high >> 1) & 1];
    int x = (position & 255) | ((high & 1) << 8);
    if (x >= 256) x -= 512;
    if (x + size > 0 && x < 256) continue;
    int dx, dy, actual_x;
    if (!current_enemy_motion(mc, slot, f, old, position, attr, high, &dx, &dy, &actual_x) &&
        !object_motion_match(mc, slot, f, old, position, attr, high, &dx, &dy, &actual_x)) continue;
    int mx = lerp_pixel(actual_x + dx, actual_x, 0.5);
    int my = lerp_pixel((int)(position >> 8) + dy, (int)(position >> 8), 0.5);
    if (mx < 256 && mx + size > 0 && my >= 32 && my < 224) return true;
  }
  return false;
}
static void sprites(SmMatchCache *mc, const Ppu *p, const SmRasterLine *line, int y,
                    SmViewport viewport, uint16_t *pixels,
                    const SmSourceFrame *f, const SmSourceFrame *previous, double alpha) {
  static const int sizes[8][2] = {{8,16},{8,32},{8,64},{16,32},
                                  {16,64},{32,64},{16,32},{16,32}};
  memset(pixels, 0, (size_t)viewport.width * sizeof(*pixels));
  int samus_dx = 0, samus_dy = 0;
  bool interpolate_objects = previous && f->object_state_valid && previous->object_state_valid && alpha < 1;
  if (interpolate_objects && read16(f->object_ram, 0xa1c) == read16(previous->object_ram, 0xa1c)) {
    int dx = (int16_t)(uint16_t)(read16(previous->object_ram, 0xb04) - read16(f->object_ram, 0xb04));
    int dy = (int16_t)(uint16_t)(read16(previous->object_ram, 0xb06) - read16(f->object_ram, 0xb06));
    if (abs(dx) <= 32 && abs(dy) <= 32) { samus_dx = dx; samus_dy = dy; }
  }
  for (int slot = 127; slot >= 0; --slot) {
    unsigned position = line->oam[slot * 2], attr = line->oam[slot * 2 + 1];
    unsigned high = line->high_oam[slot / 4] >> ((slot % 4) * 2);
    int size = sizes[p->obsel >> 5][(high >> 1) & 1];
    /* Without interpolation nothing below can move or recover a sprite, so
     * one that does not cover this line draws nothing: reject it first. */
    if (!interpolate_objects && ((y - (int)(position >> 8)) & 255) >= size) continue;
    int sprite_y = position >> 8;
    int x = (position & 255) | ((high & 1) << 8);
    if (x >= 256) x -= 512;
    /* Offscreen OAM is ambiguous until a live ROM-map owner supplies its
     * signed screen position. Only verified motion may recover such entries
     * into the native area; unrecognized hidden reservations stay hidden. */
    bool native_footprint = x + size > 0 && x < 256;
    bool recovered = false;
    int enemy_dx, enemy_dy, actual_x;
    if (interpolate_objects && grapple_motion[slot].valid &&
        grapple_motion[slot].position == position && grapple_motion[slot].attr == attr &&
        grapple_motion[slot].high == (high & 3)) {
      x = lerp_pixel(x + grapple_motion[slot].dx, x, alpha);
      sprite_y = lerp_pixel(sprite_y + grapple_motion[slot].dy, sprite_y, alpha);
      ++render_stats.matched_object_samples;
    } else if (native_footprint && interpolate_objects && (samus_dx || samus_dy) &&
        abs(y - sprite_y) <= size + 32 &&
        current_samus_piece(mc, (unsigned)slot, f, position, attr, high)) {
      /* Animate the current ROM piece at the interpolated owner origin.
       * Prior OAM reservations and animation tile identities may differ. */
      x = lerp_pixel(x + samus_dx, x, alpha);
      sprite_y = lerp_pixel(sprite_y + samus_dy, sprite_y, alpha);
      ++render_stats.matched_object_samples;
      ++render_stats.matched_samus_samples;
    } else if (interpolate_objects && abs(y - sprite_y) <= size + 32 &&
        current_enemy_motion(mc, (unsigned)slot, f, previous, position, attr, high, &enemy_dx, &enemy_dy, &actual_x)) {
      if (!native_footprint) x = actual_x;
      recovered = true;
      x = lerp_pixel(x + enemy_dx, x, alpha);
      sprite_y = lerp_pixel(sprite_y + enemy_dy, sprite_y, alpha);
      ++render_stats.matched_object_samples;
    } else if (interpolate_objects && abs(y - sprite_y) <= size + 32 &&
        object_motion_match(mc, (unsigned)slot, f, previous, position, attr, high, &enemy_dx, &enemy_dy, &actual_x)) {
      if (!native_footprint) x = actual_x;
      recovered = true;
      x = lerp_pixel(x + enemy_dx, x, alpha);
      sprite_y = lerp_pixel(sprite_y + enemy_dy, sprite_y, alpha);
      ++render_stats.matched_object_samples;
      if (mc->object[slot].effect) ++render_stats.matched_effect_samples;
    }
    if (!native_footprint && !recovered) continue;
    /* OBJ evaluation is one hardware line behind BG sampling. */
    int row = (y - sprite_y) & 255;
    if (row >= size) continue;
    x += viewport.extra;
    if (attr & 0x8000) row = size - 1 - row;
    unsigned base = (p->obsel & 7) * 8192;
    if (attr & 256) base += (((p->obsel >> 3) & 3) + 1) * 4096;
    unsigned palette = 128 + ((attr >> 9) & 7) * 16;
    unsigned priority = ((attr >> 12) & 3) * 4 + 2;
    unsigned layer = attr & 0x800 ? 4 : 6;
    for (int col = 0; col < size; ++col) {
      int dest = x + col;
      if (dest < 0 || dest >= viewport.width) continue;
      int cx = attr & 0x4000 ? size - 1 - col : col;
      unsigned tile = (((((attr & 255) >> 4) + row / 8) & 15) << 4) |
                      (((attr & 15) + cx / 8) & 15);
      unsigned pixel = tile_pixel(line->vram, base + tile * 16, cx & 7, row & 7, 4);
      if (pixel) {
        pixels[dest] = (priority << 12) | (layer << 8) | palette | pixel;
        if (!native_footprint && recovered && dest >= viewport.extra && dest < viewport.extra + 256)
          ++render_stats.recovered_edge_pixels;
      }
    }
  }
}
static bool window_condition(unsigned mode, bool inside) {
  return mode == 3 || (mode == 1 && !inside) || (mode == 2 && inside);
}

/* $A0:944A and $81:8AB8, interpreted only as rendering data. Do not call the
 * guest routines: WriteEnemyOams decrements shake timers and extended tilemap
 * handlers write guest RAM. Invisible/deleted enemies remain invisible. */
static void enemy_map(const SmRasterLine *line, const Ppu *p,
                      unsigned bank, unsigned map, int origin_x, int origin_y,
                      unsigned palette, unsigned base_tile, int y,
                      SmViewport viewport, uint16_t *pixels) {
  const uint8_t *data = rom_bytes(bank, map, 2);
  if (!data) return;
  unsigned count = read16(data, 0);
  if (!count || count > 128) return;
  data = rom_bytes(bank, map + 2, count * 5);
  if (!data) return;
  static const int sizes[8][2] = {{8,16},{8,32},{8,64},{16,32},
                                  {16,64},{32,64},{16,32},{16,32}};
  for (unsigned i = count; i-- > 0;) {
    const uint8_t *entry = data + i * 5;
    unsigned offset = read16(entry, 0);
    int dx = (int)(offset & 0x1ff);
    if (dx & 0x100) dx -= 512;
    int x = origin_x + dx;
    int sprite_y = origin_y + (int8_t)entry[2];
    int size = sizes[p->obsel >> 5][offset >> 15];
    int row = y - sprite_y;
    if (row < 0 || row >= size) continue;
    unsigned attr = (uint16_t)(palette | (base_tile + read16(entry, 3)));
    if (attr & 0x8000) row = size - 1 - row;
    unsigned base = (p->obsel & 7) * 8192;
    if (attr & 256) base += (((p->obsel >> 3) & 3) + 1) * 4096;
    for (int col = 0; col < size; ++col) {
      int native_x = x + col, dest = native_x + viewport.extra;
      if ((native_x >= 0 && native_x < 256) || dest < 0 || dest >= viewport.width)
        continue;
      int cx = attr & 0x4000 ? size - 1 - col : col;
      unsigned tile = (((((attr & 255) >> 4) + row / 8) & 15) << 4) |
                      (((attr & 15) + cx / 8) & 15);
      unsigned pixel = tile_pixel(line->vram, base + tile * 16, cx & 7, row & 7, 4);
      if (pixel)
        pixels[dest] = ((((attr >> 12) & 3) * 4 + 2) << 12) |
                       ((attr & 0x800 ? 4 : 6) << 8) |
                       128 | (((attr >> 9) & 7) * 16) | pixel;
    }
  }
}

static void enemies(const SmSourceFrame *f, const SmRasterLine *line,
                    const Ppu *p, int y, SmViewport viewport, uint16_t *pixels,
                    unsigned phase, const SmSourceFrame *previous, double alpha) {
  if (!viewport.extra || !renderer_rom) return;
  for (int slot = 31; slot >= 0; --slot) {
    const uint8_t *e = f->object_ram + 0xf78 + slot * 64;
    unsigned id = read16(e, 0), properties = read16(e, 14);
    if (!id || id == 0xdaff || (properties & 0x300)) continue;
    if (read16(e, 34) != phase) continue;
    unsigned map = read16(e, 22), bank = e[46];
    int x = (int)read16(e, 2) - (int)read16(f->object_ram, 0x911) +
            (int16_t)read16(f->object_ram, 0x7010 + slot * 64);
    int sy = (int)read16(e, 6) - (int)read16(f->object_ram, 0x915) +
             (int16_t)read16(f->object_ram, 0x7012 + slot * 64);
    unsigned palette = read16(e, 30), frozen = read16(e, 38);
    if (read16(e, 36) && (read16(f->object_ram, 0xe44) & 2)) palette = 0;
    else if (frozen && (frozen >= 0x5a || (frozen & 2))) palette = 3072;
    if (read16(e, 42)) x += read16(e, 44) & 2 ? -1 : 1;
    int dx, dy;
    if (alpha < 1 && enemy_motion(f, previous, (unsigned)slot, &dx, &dy)) {
      x = lerp_pixel(x + dx, x, alpha);
      sy = lerp_pixel(sy + dy, sy, alpha);
    }
    unsigned base_tile = read16(e, 32);
    if (read16(e, 16) & 4) {
      const uint8_t *ext = rom_bytes(bank, map, 2);
      if (!ext) continue;
      unsigned n = read16(ext, 0);
      if (!n || n > 64) continue;
      ext = rom_bytes(bank, map + 2, n * 8);
      if (!ext) continue;
      for (unsigned i = n; i-- > 0;) {
        const uint8_t *child = ext + i * 8;
        enemy_map(line, p, bank, read16(child, 4),
                  x + (int16_t)read16(child, 0), sy + (int16_t)read16(child, 2),
                  palette, base_tile, y, viewport, pixels);
      }
    } else {
      enemy_map(line, p, bank, map, x, sy, palette, base_tile, y, viewport, pixels);
    }
  }
}

static void enemy_projectiles(const SmSourceFrame *f, const SmRasterLine *line,
                              const Ppu *p, int y, SmViewport viewport,
                              uint16_t *pixels, bool low_priority,
                              const SmSourceFrame *previous, double alpha) {
  int shake_x = 0, shake_y = 0;
  unsigned earthquake = read16(f->object_ram, 0x183e), timer = read16(f->object_ram, 0x1840);
  if (timer && !read16(f->object_ram, 0xa78) && earthquake < 36) {
    const uint8_t *offset = rom_bytes(0x86, 0x846b + 4 * earthquake, 4);
    if (offset) {
      int sign = timer & 2 ? -1 : 1;
      shake_x = sign * (int16_t)read16(offset, 0);
      shake_y = sign * (int16_t)read16(offset, 2);
    }
  }
  /* $86:8390 / $86:83B2 emit slots in descending order. This compositor
   * overwrites pixels, so reverse that order to retain native OBJ ownership. */
  for (unsigned slot = 0; slot < 18; ++slot) {
    unsigned offset = slot * 2;
    if (!read16(f->object_ram, 0x1997 + offset) ||
        ((read16(f->object_ram, 0x1bd7 + offset) & 0x1000) != 0) != low_priority)
      continue;
    int x = (int)read16(f->object_ram, 0x1a4b + offset) - (int)read16(f->object_ram, 0x911) + shake_x;
    int sy = (int)read16(f->object_ram, 0x1a93 + offset) - (int)read16(f->object_ram, 0x915) + shake_y;
    SmProjectilePose now, before;
    if (alpha < 1 && projectile_motion(f, previous, true, slot, &now, &before)) {
      x = lerp_pixel(before.x, now.x, alpha);
      sy = lerp_pixel(before.y, now.y, alpha);
    }
    if (sy < -128 || sy >= 384) continue;
    unsigned gfx = read16(f->object_ram, 0x19bb + offset);
    enemy_map(line, p, 0x8d, read16(f->object_ram, 0x1b6b + offset), x, sy,
              gfx & 0xff00, gfx & 255, y, viewport, pixels);
  }
}

static void player_projectiles(const SmSourceFrame *f, const SmRasterLine *line,
                               const Ppu *p, int y, SmViewport viewport,
                               uint16_t *pixels, bool explosions,
                               const SmSourceFrame *previous, double alpha) {
  for (unsigned slot = 0; slot < (explosions ? 10u : 5u); ++slot) {
    unsigned offset = slot * 2;
    if (!player_projectile_visible(f->object_ram, slot)) continue;
    unsigned type = read16(f->object_ram, 0xc18 + offset);
    unsigned category = type & 0xf00;
    if ((category >= 0x300) != explosions) continue;
    int x = (int)read16(f->object_ram, 0xb64 + offset) - (int)read16(f->object_ram, 0x911);
    int sy = (int)read16(f->object_ram, 0xb78 + offset) - (int)read16(f->object_ram, 0x915);
    project_player_position(f->object_ram, type,
        (int)read16(f->object_ram, 0xb64 + offset), (int)read16(f->object_ram, 0xb78 + offset), &x, &sy);
    SmProjectilePose now, before;
    if (alpha < 1 && projectile_motion(f, previous, false, slot, &now, &before)) {
      x = lerp_pixel(before.x, now.x, alpha);
      sy = lerp_pixel(before.y, now.y, alpha);
    }
    if (sy < 0 || sy >= 256) continue;
    enemy_map(line, p, 0x93, read16(f->object_ram, 0xcb8 + offset),
              x, sy, 0, 0, y, viewport, pixels);
  }
}

static void sprite_objects(const SmSourceFrame *f, const SmRasterLine *line,
                           const Ppu *p, int y, SmViewport viewport, uint16_t *pixels,
                           const SmSourceFrame *previous, double alpha) {
  /* $B4:BD32: pickup/effect spritemaps. Read the current instruction's map
   * operand only; never advance instruction timers from presentation. */
  for (unsigned slot = 0; slot < 32; ++slot) {
    SmProjectilePose now, before;
    if (!sprite_object_pose(f, slot, &now)) continue;
    int x = now.x, sy = now.y;
    if (alpha < 1 && sprite_object_motion(f, previous, slot, &now, &before)) {
      x = lerp_pixel(before.x, now.x, alpha);
      sy = lerp_pixel(before.y, now.y, alpha);
    }
    enemy_map(line, p, now.bank, now.map, x, sy,
              now.palette, now.base, y, viewport, pixels);
  }
}

unsigned SmRendererGrapplePieces(const uint8_t ram[0x20000], SmGrapplePiece pieces[17]) {
  if (!ram || !pieces || read16(ram, 0xa5c) != 0xeb86) return 0;
  unsigned length = read16(ram, 0xcfe), function = read16(ram, 0xd32);
  if (!length || (length & 0x8000) || function == 0xc4f0 || function >= 0xc856) return 0;
  int dx = (int16_t)(uint16_t)(read16(ram, 0xd08) - read16(ram, 0xd1a));
  int dy = (int16_t)(uint16_t)(read16(ram, 0xd0c) - read16(ram, 0xd1c));
  if (abs(dx) > 255 || abs(dy) > 255) return 0;
  unsigned x = (unsigned)abs(dx), y = (unsigned)abs(dy), quadrant = (dx < 0 ? 2u : 0u) | (dy < 0 ? 1u : 0u);
  unsigned angle, ratio = (y < x ? (y * 256 / x) : y ? (x * 256 / y) : 65535) >> 3;
  static const int x_offsets[] = {64,64,-64,-64}, x_signs[] = {1,-1,-1,1};
  static const int y_offsets[] = {128,0,128,0}, y_signs[] = {-1,1,1,-1};
  angle = (unsigned)((y < x ? x_offsets[quadrant] : y_offsets[quadrant]) +
                    (int)ratio * (y < x ? x_signs[quadrant] : y_signs[quadrant])) & 255;
  const uint8_t *sine = rom_bytes(0xa0, 0xb3c3 + 2 * angle, 2);
  const uint8_t *cosine = rom_bytes(0xa0, 0xb443 + 2 * angle, 2);
  if (!sine || !cosine) return 0;
  int step_x = (int16_t)read16(cosine, 0) * 2048, step_y = (int16_t)read16(sine, 0) * 2048;
  int sx = (int)read16(ram, 0xd1a) - (int)read16(ram, 0x911) - 4;
  int sy = (int)read16(ram, 0xd1c) - (int)read16(ram, 0x915) - 4;
  unsigned flip = (ram[0xcfb] & 0x80) >> 1;
  flip = (flip | (2 * (((ram[0xcfb] ^ flip) & 0x40) ^ 0x40))) << 8;
  unsigned count = (length / 8) & 15;
  if (!count) count = 1;
  bool stopped = false;
  for (unsigned i = 0; i < count; ++i) {
    unsigned slot = 15 - i, pointer = read16(ram, 0xd62 + slot * 2);
    /* Visible slots have already advanced their instruction in the guest.
     * Slots after its native clipping break have not: peek their next art
     * without changing a timer or executing guest code. */
    if (stopped && read16(ram, 0xd42 + slot * 2) == 1) {
      bool found = false;
      for (unsigned hop = 0; hop < 16; ++hop) {
        const uint8_t *instruction = rom_bytes(0x94, pointer, 4);
        if (!instruction) return 0;
        unsigned value = read16(instruction, 0);
        if (!(value & 0x8000)) { pointer += 4; found = true; break; }
        if (value != 0xb0f4) return 0;
        pointer = read16(instruction, 2);
      }
      if (!found) return 0;
    }
    const uint8_t *art = rom_bytes(0x94, pointer - 2, 2);
    if (!art) return 0;
    int64_t fx = (int64_t)sx * 65536 + (int64_t)step_x * i;
    int64_t fy = (int64_t)sy * 65536 + (int64_t)step_y * i;
    int px = (int)(fx >= 0 ? fx / 65536 : -((-fx + 65535) / 65536));
    int py = (int)(fy >= 0 ? fy / 65536 : -((-fy + 65535) / 65536));
    pieces[i] = (SmGrapplePiece){px, py, (uint16_t)(flip | read16(art, 0)), false};
    stopped |= px < 0 || px > 255 || py < 0 || py > 255;
  }
  unsigned pose = read16(ram, 0xa1c);
  int end_y = (int)read16(ram, 0xd0c) - (int)read16(ram, 0x915);
  if ((end_y >= 0 && end_y < 256) || pose == 0xb2 || pose == 0xb3)
    pieces[count++] = (SmGrapplePiece){(int)read16(ram, 0xd08) - (int)read16(ram, 0x911) - 4, end_y - 4, 0x3a20, true};
  return count;
}

static void grapple_pixels(const SmRasterLine *line, const Ppu *p, int y,
                            SmViewport viewport, uint16_t *pixels,
                            const SmGrapplePiece *pieces, const bool *recover_native, unsigned count) {
  for (unsigned i = count; i-- > 0;) {
    int row = y - pieces[i].y;
    if (row < 0 || row >= 8) continue;
    unsigned attr = pieces[i].attr;
    if (attr & 0x8000) row = 7 - row;
    unsigned base = (p->obsel & 7) * 8192;
    if (attr & 256) base += (((p->obsel >> 3) & 3) + 1) * 4096;
    for (int col = 0; col < 8; ++col) {
      int x = pieces[i].x + col, dest = x + viewport.extra;
      bool native = x >= 0 && x < 256;
      if ((native && !recover_native[i]) || dest < 0 || dest >= viewport.width) continue;
      unsigned pixel = tile_pixel(line->vram, base + (attr & 255) * 16, attr & 0x4000 ? 7 - col : col, row, 4);
      if (pixel) {
        pixels[dest] = (uint16_t)(((((attr >> 12) & 3) * 4 + 2) << 12) |
                      ((attr & 0x800 ? 4 : 6) << 8) | 128 | (((attr >> 9) & 7) * 16) | pixel);
        if (native) ++render_stats.recovered_edge_pixels;
        else ++render_stats.grapple_margin_pixels;
      }
    }
  }
}

static __attribute__((unused)) uint32_t colour(const Ppu *p, const uint16_t *palette, const uint8_t *brightness, uint16_t main,
                       uint16_t sub, bool inside) {
  unsigned rgb = palette[main & 255], layer = (main >> 8) & 15;
  bool clipped = window_condition(p->cgwsel >> 6, inside);
  bool math = !window_condition((p->cgwsel >> 4) & 3, inside) &&
              ((p->cgadsub & 63) & (1u << layer));
  if (!clipped && !math)   /* the common case: plain palette colour */
    return (uint32_t)brightness[rgb & 31] << 16 | (uint32_t)brightness[(rgb >> 5) & 31] << 8 |
           brightness[(rgb >> 10) & 31];
  unsigned other = p->fixedColor;
  bool half = math && (p->cgadsub & 64) && !clipped;
  if (math && (p->cgwsel & 2)) {
    if ((sub & 255) != 0) other = palette[sub & 255];
    else half = false;
  }
  uint32_t result = 0;
  for (int component = 0; component < 3; ++component) {
    int c = clipped ? 0 : (rgb >> (component * 5)) & 31;
    if (math) {
      int second = (other >> (component * 5)) & 31;
      c += p->cgadsub & 128 ? -second : second;
      if (c < 0) c = 0;
      if (half) c /= 2;
      if (c > 31) c = 31;
    }
    c = brightness[c];
    result |= (uint32_t)c << (16 - component * 8);
  }
  return result;
}

/* Everything one output line needs, shared read-only by the line workers. */
typedef struct SmDrawCtx {
  uint32_t *out; SmViewport viewport; bool hud_anchored; double alpha;
  const SmSourceFrame *f, *previous; bool room;
  SmWideWindow *wide_windows; bool xray, xray_reveal;
  SmGrapplePiece *grapple; bool *grapple_recover_native; unsigned grapple_count;
  bool has_flare; SmProjectilePose flare; unsigned generation;
} SmDrawCtx;

/* One private Ppu per drawing slot for deferred stock lines. */
static Ppu stock_slots[SM_DRAW_SLOTS];
static unsigned stock_slot_generation[SM_DRAW_SLOTS];
static void draw_stock_line(const SmDrawCtx *c, int y, unsigned slot) {
  unsigned frame_slot = (unsigned)(c->f - frames);
  if (frame_slot > 1 || !stock_state[frame_slot].deferred) return;
  const SmStockState *st = &stock_state[frame_slot];
  if (slot >= SM_DRAW_SLOTS) slot = 0;
  Ppu *p = &stock_slots[slot];
  SmSourceFrame *f = &frames[frame_slot];
  if (stock_slot_generation[slot] != c->generation) {
    stock_slot_generation[slot] = c->generation;
    memcpy((uint8_t *)p + SM_STOCK_HOST_A, st->host_a, sizeof(st->host_a));
    memcpy((uint8_t *)p + SM_STOCK_HOST_B, st->host_b, sizeof(st->host_b));
    memcpy(p->mosaicModulo, st->mosaic_modulo, sizeof(p->mosaicModulo));
    p->lastBrightnessMult = 0xff;   /* this copy's own table is stale */
    p->extraObjects = NULL; p->extraObjectCount = 0;
    p->widescreenLineEnhancer = NULL;
    memset(&p->mode7Hd, 0, sizeof(p->mode7Hd));
    memset(p->overlayRenderBuffer, 0, sizeof(p->overlayRenderBuffer));
    p->renderBuffer = (uint8_t *)f->stock;
    p->renderPitch = 256 * sizeof(uint32_t);
  }
  const SmRasterLine *l = &f->lines[y];
  memcpy(p, l->registers, PPU_SAVESTATE_REGS_SIZE);
  memcpy(p->cgram, l->palette, sizeof(p->cgram));
  memcpy(p->oam, l->oam, sizeof(p->oam));
  memcpy(p->highOam, l->high_oam, sizeof(p->highOam));
  p->renderVram = l->vram;
  ppu_drawLineDetached(p, y + 1);
}

static void sm_max_row(uint16_t *restrict scr, const uint16_t *restrict src, int width) {
  for (int sx = 0; sx < width; ++sx) {
    const uint16_t a = scr[sx], b = src[sx];
    scr[sx] = b > a ? b : a;
  }
}
static void sm_max_row_windowed(uint16_t *restrict scr, const uint16_t *restrict src,
                                const uint8_t *restrict win, int width) {
  for (int sx = 0; sx < width; ++sx) {
    const uint16_t a = scr[sx];
    const uint16_t b = win[sx] ? 0 : src[sx];
    scr[sx] = b > a ? b : a;
  }
}

static void draw_line(const SmDrawCtx *c, int y, unsigned slot) {
  Ppu *const line_regs = &scanout_slots[slot < SM_DRAW_SLOTS ? slot : 0];
#define scanout (*line_regs)
  uint32_t *out = c->out; SmViewport viewport = c->viewport; bool hud_anchored = c->hud_anchored;
  double alpha = c->alpha; const SmSourceFrame *f = c->f, *previous = c->previous;
  bool room = c->room; SmWideWindow *wide_windows = c->wide_windows;
  bool xray = c->xray, xray_reveal = c->xray_reveal;
  SmGrapplePiece *grapple = c->grapple; bool *grapple_recover_native = c->grapple_recover_native;
  unsigned grapple_count = c->grapple_count; bool has_flare = c->has_flare;
  SmProjectilePose flare = c->flare;
  uint16_t objects[SM_MAX_WIDTH];
  uint8_t mode7_pixels[SM_MAX_WIDTH];
  (void)hud_anchored; (void)xray; (void)has_flare;

  const SmRasterLine *l = &f->lines[y];
  memcpy(&scanout, l->registers, PPU_SAVESTATE_REGS_SIZE);
  uint8_t brightness[32];
  for (unsigned c = 0; c < 32; ++c)
    brightness[c] = (uint8_t)(((c << 3) | (c >> 2)) * (scanout.inidisp & 15) / 15);
  /* The IRQ status band is an opaque screen-space composite. Moving its
   * three column groups together retains HUD OBJ icons, palette effects
   * and energy digits without moving similarly shaped world sprites. */
  if (room && y < 32 && scanout.screenEnabled[0] == 4 &&
      scanout.cgadsub == 0 && scanout.cgwsel == 0) {
    ++render_stats.hud_lines;
    draw_stock_line(c, y, slot);
    uint32_t *row = out + y * viewport.width;
    const uint32_t *native = f->stock + y * 256;
    if (hud_anchored) {
      memcpy(row, native, 80 * sizeof(*row));
      memcpy(row + 80 + viewport.extra, native + 80, 128 * sizeof(*row));
      memcpy(row + 208 + 2 * viewport.extra, native + 208, 48 * sizeof(*row));
    } else {
      memcpy(row + viewport.extra, native, 256 * sizeof(*row));
    }
    return;
  }
  int mode = scanout.bgmode & 7;
  if (!room || (mode != 1 && mode != 7)) {
    ++render_stats.stock_lines;
    draw_stock_line(c, y, slot);
    memcpy(out + y * viewport.width + viewport.extra,
           f->stock + y * 256, 256 * sizeof(*out));
    return;
  }
  ++render_stats.custom_lines;
  if (mode == 7) ++render_stats.mode7_lines;
  if (wide_windows[y].valid) {
    if (xray) ++render_stats.xray_lines;
    else ++render_stats.power_bomb_lines;
  }
  if (scanout.inidisp & 128) return;
  int mode7_y = y + 1;
  int mode7_size = (scanout.mosaic >> 4) + 1;
  /* Mode 7 quantizes screen coordinates before the affine transform and
   * flips. BG1 controls vertical mosaic for the shared Mode 7 source. */
  if (scanout.mosaic & 1) mode7_y -= mode7_y % mode7_size;
  SmMode7Line transform = SmMode7Transform(scanout.m7matrix, scanout.m7sel, mode7_y);
  if (mode == 7 && previous && alpha < 1) {
    Ppu *const old_regs = line_regs + SM_DRAW_SLOTS;   /* previous frame's registers */
    memcpy(old_regs, previous->lines[y].registers, PPU_SAVESTATE_REGS_SIZE);
    if ((old_regs->bgmode & 7) == 7 && old_regs->mosaic == scanout.mosaic)
      transform = SmMode7Interpolate(
          SmMode7Transform(old_regs->m7matrix, old_regs->m7sel, mode7_y), transform, alpha);
  }
  /* Interpolate raster scroll registers, not the final composed image.
   * Layout/mode changes and large jumps snap to the current frame. */
  if (previous && alpha < 1) {
    Ppu *const old_regs = line_regs + SM_DRAW_SLOTS;   /* previous frame's registers */
    memcpy(old_regs, previous->lines[y].registers, PPU_SAVESTATE_REGS_SIZE);
    if (old_regs->bgmode == scanout.bgmode && old_regs->bgTileAdr == scanout.bgTileAdr &&
        !memcmp(old_regs->bgXsc, scanout.bgXsc, sizeof(old_regs->bgXsc))) {
      for (int layer = 0; layer < (mode == 7 ? 0 : 3); ++layer) {
        int dx = ((int)old_regs->hScroll[layer] - scanout.hScroll[layer] + 512) & 1023;
        int dy = ((int)old_regs->vScroll[layer] - scanout.vScroll[layer] + 512) & 1023;
        dx -= 512; dy -= 512;
        if (layer == 0 && ((dx && abs(dx) <= 32) || (dy && abs(dy) <= 32)))
          ++render_stats.interpolated_scroll_lines;
        if (abs(dx) <= 32)
          scanout.hScroll[layer] = (uint16_t)(scanout.hScroll[layer] + lerp_pixel(dx, 0, alpha));
        if (abs(dy) <= 32)
          scanout.vScroll[layer] = (uint16_t)(scanout.vScroll[layer] + lerp_pixel(dy, 0, alpha));
      }
    }
  }
  SmBackgroundLine background_lines[3];
  if (mode == 1)
    for (unsigned layer = 0; layer < 3; ++layer)
      background_lines[layer] = background_line(&scanout, f->ram, layer);
  if (xray_reveal && mode == 1 && scanout.bgXsc[1] == 0x49 && scanout.windowsel == 0x8008c8) {
    background_lines[1].room = f->ram;
    background_lines[1].revealed = xray_blocks;
    background_lines[1].world_x = ((int)read16(f->ram, 0x911) & ~15) + (scanout.hScroll[1] & 15);
    background_lines[1].world_y = ((int)read16(f->ram, 0x915) & ~15) + (scanout.vScroll[1] & 15);
  }
  sprites(&match_caches[slot < SM_DRAW_SLOTS ? slot : 0], &scanout, l, y, viewport, objects, f, previous, alpha);
  if (viewport.extra && renderer_rom) {
    /* Reverse $A0:884D's emission order because pixel writes overwrite.
     * The native center continues to use captured OAM exclusively. */
    for (int phase = 7; phase >= 0; --phase) {
      enemies(f, l, &scanout, y, viewport, objects, (unsigned)phase, previous, alpha);
      if (phase == 6) enemy_projectiles(f, l, &scanout, y, viewport, objects, false, previous, alpha);
      if (phase == 3) player_projectiles(f, l, &scanout, y, viewport, objects, false, previous, alpha);
    }
    enemy_projectiles(f, l, &scanout, y, viewport, objects, true, previous, alpha);
    player_projectiles(f, l, &scanout, y, viewport, objects, true, previous, alpha);
    sprite_objects(f, l, &scanout, y, viewport, objects, previous, alpha);
  }
  grapple_pixels(l, &scanout, y, viewport, objects, grapple, grapple_recover_native, grapple_count);
  if (viewport.extra && has_flare)
    enemy_map(l, &scanout, flare.bank, flare.map, flare.x, flare.y, 0, 0, y, viewport, objects);
  if (mode == 7)
    for (int sx = 0; sx < viewport.width; ++sx)
      mode7_pixels[sx] = SmMode7Sample(&transform, l->vram, sx - viewport.extra);
  int layers = mode == 7 ? ((scanout.setini & 64) ? 2 : 1) : 3;
  unsigned enabled = scanout.screenEnabled[0] | scanout.screenEnabled[1];
  /* Per line: non-mosaic BG rows decoded tile by tile, and every window mask
   * the pixels consult, each computed once instead of per use. */
  uint16_t bg_rows[3][SM_MAX_WIDTH];
  bool bg_row_ready[3] = {false, false, false};
  if (mode != 7)
    for (int layer = 0; layer < layers; ++layer)
      if ((enabled & (1u << layer)) && !(scanout.mosaic & (1u << layer))) {
        background_row(&background_lines[layer], l->vram, y + 1, -viewport.extra,
                       viewport.width, bg_rows[layer]);
        bg_row_ready[layer] = true;
      }
  uint8_t windows[6][SM_MAX_WIDTH];
  unsigned windowed = scanout.screenWindowed[0] | scanout.screenWindowed[1];
  for (int w = 0; w < 6; ++w) {
    if (w < 5 && !(windowed & (1u << w))) continue;
    unsigned flags = (scanout.windowsel >> (w * 4)) & 15;
    if (!(flags & 2) && !(flags & 8)) {   /* both windows off: never inside */
      memset(windows[w], 0, (size_t)viewport.width);
      continue;
    }
    window_row(&scanout, w, viewport.extra, &wide_windows[y], viewport.width, windows[w]);
  }
  /* Remaining BG samples (mode 7, mosaic) into the same rows. */
  for (int layer = 0; layer < layers; ++layer) {
    if (!(enabled & (1u << layer)) || bg_row_ready[layer]) continue;
    uint16_t *row = bg_rows[layer];
    for (int sx = 0; sx < viewport.width; ++sx) {
      int x = sx - viewport.extra;
      if (mode == 7) {
        unsigned index = mode7_pixels[sx];
        if ((scanout.mosaic & (1u << layer)) && mode7_size > 1) {
          int mx = x - ((x % mode7_size) + mode7_size) % mode7_size;
          index = SmMode7Sample(&transform, l->vram, mx);
        }
        unsigned priority = 5;
        if (layer == 1) {
          priority = index & 128 ? 9 : 1;
          index &= 127;
        }
        row[sx] = index ? (uint16_t)((priority << 12) | (layer << 8) | index) : 0;
      } else {
        int sample_x = x, sample_y = y + 1;
        if (scanout.mosaic & (1u << layer)) {
          int size = (scanout.mosaic >> 4) + 1;
          /* Mosaic precedes scroll and tile lookup, independently per BG.
           * Floor division extends the native X=0 grid into negative X;
           * C's truncating remainder would shift blocks in the left margin. */
          sample_x -= ((sample_x % size) + size) % size;
          sample_y -= sample_y % size;
        }
        bool margin = sample_x < 0 || sample_x >= 256;
        row[sx] = background_pixel(&background_lines[layer], l->vram,
                                   sample_x, sample_y, margin);
      }
    }
  }
  /* Each screen's winning pixel is the maximum over its enabled, unwindowed
   * layers and OBJ (priority in the high bits): one tight pass per layer. */
  uint16_t screens[2][SM_MAX_WIDTH];
  const int width = viewport.width;
  for (int sub = 0; sub < 2; ++sub) {
    uint16_t *scr = screens[sub];
    for (int sx = 0; sx < width; ++sx) scr[sx] = 0x500;
    for (int layer = 0; layer < layers + 1; ++layer) {
      bool obj = layer == layers;
      unsigned bit = obj ? 16u : 1u << layer;
      if (!(scanout.screenEnabled[sub] & bit)) continue;
      const uint16_t *src = obj ? objects : bg_rows[layer];
      /* Branch-free max so the compiler can vectorize it (NEON vmax.u16). */
      if (scanout.screenWindowed[sub] & bit) {
        sm_max_row_windowed(scr, src, windows[obj ? 4 : layer], width);
      } else {
        sm_max_row(scr, src, width);
      }
    }
  }
  /* colour(), with its two window decisions resolved per line and the plain
   * (unclipped, no colour math) case taken inline. */
  uint32_t *dst = out + y * width;
  bool clip_when[2] = { window_condition(scanout.cgwsel >> 6, false),
                        window_condition(scanout.cgwsel >> 6, true) };
  bool math_allowed[2] = { !window_condition((scanout.cgwsel >> 4) & 3, false),
                           !window_condition((scanout.cgwsel >> 4) & 3, true) };
  unsigned math_layers = scanout.cgadsub & 63;
  /* Colour math as table lookups: index = main + (+/-)other + 31 per 5-bit
   * component, already clamped, halved and brightness-scaled exactly as
   * colour() does (clamp below 0, halve, clamp above 31). */
  const bool math_subtract = (scanout.cgadsub & 128) != 0;
  const bool math_half = (scanout.cgadsub & 64) != 0;
  const bool math_use_sub = (scanout.cgwsel & 2) != 0;
  const unsigned fixed_rgb = scanout.fixedColor;
  uint8_t math_full[94], math_halved[94];
  bool math_tables = false;
  for (int sx = 0; sx < width; ++sx) {
    unsigned main_px = screens[0][sx], inside = windows[5][sx];
    const bool clipped = clip_when[inside];
    const bool math = math_allowed[inside] && (math_layers & (1u << ((main_px >> 8) & 15)));
    if (!clipped && !math) {
      unsigned rgb = l->palette[main_px & 255];
      dst[sx] = (uint32_t)brightness[rgb & 31] << 16 | (uint32_t)brightness[(rgb >> 5) & 31] << 8 |
                brightness[(rgb >> 10) & 31];
      continue;
    }
    if (!math) { dst[sx] = (uint32_t)brightness[0] * 0x10101u; continue; }
    if (!math_tables) {
      math_tables = true;
      for (int i = 0; i < 94; ++i) {
        int c = i - 31;
        if (c < 0) c = 0;
        int h = c / 2;
        math_full[i] = brightness[c > 31 ? 31 : c];
        math_halved[i] = brightness[h > 31 ? 31 : h];
      }
    }
    unsigned rgb = clipped ? 0 : l->palette[main_px & 255];
    bool half = math_half && !clipped;
    unsigned other = fixed_rgb;
    if (math_use_sub) {
      unsigned sub = screens[1][sx];
      if ((sub & 255) != 0) other = l->palette[sub & 255];
      else half = false;
    }
    const uint8_t *t = half ? math_halved : math_full;
    int o0 = other & 31, o1 = (other >> 5) & 31, o2 = (other >> 10) & 31;
    if (math_subtract) { o0 = -o0; o1 = -o1; o2 = -o2; }
    dst[sx] = (uint32_t)t[(int)(rgb & 31) + o0 + 31] << 16 |
              (uint32_t)t[(int)((rgb >> 5) & 31) + o1 + 31] << 8 |
              t[(int)((rgb >> 10) & 31) + o2 + 31];
  }
#undef scanout
}

#ifdef __vita__
#include "render_workers.h"
/* PS Vita: the wide frame is drawn across the cores (lines are independent:
 * the per-line PPU copy and the sprite-match caches are per thread). */
static void draw_lines_range(void *context, size_t begin, size_t end, unsigned slot) {
  const SmDrawCtx *c = (const SmDrawCtx *)context;
  for (size_t y = begin; y < end; ++y) draw_line(c, (int)y, slot);
}
static SnesRenderWorkers *line_workers(void) {
  static bool tried;
  static SnesRenderWorkers *pool;
  if (!tried) {
    tried = true;
    /* An empty "single_core" file in the data folder keeps drawing serial. */
    FILE *off = fopen(SNESRECOMP_VITA_DATA_DIR "single_core", "rb");
    if (off) fclose(off);
    else pool = SnesRenderWorkersCreate(SM_DRAW_SLOTS);
    fprintf(stderr, "[vita] widescreen drawing on %u core(s)\n", SnesRenderWorkersCount(pool));
  }
  return pool;
}
#endif

static bool draw_frame_slot(unsigned frame_slot, uint32_t *out, SmViewport viewport,
                            bool hud_anchored, double alpha);
bool SmRendererDraw(uint32_t *out, SmViewport viewport, bool hud_anchored, double alpha) {
  return draw_frame_slot(current, out, viewport, hud_anchored, alpha);
}
static bool draw_frame_slot(unsigned frame_slot, uint32_t *out, SmViewport viewport,
                            bool hud_anchored, double alpha) {
  plane_expand_init();
  memset(grapple_motion, 0, sizeof(grapple_motion));
  memset(&render_stats, 0, sizeof(render_stats));
  memset(match_caches, 0, sizeof(match_caches));
  const SmSourceFrame *f = &frames[frame_slot];
  const SmSourceFrame *previous = &frames[frame_slot ^ 1];
  if (!isfinite(alpha)) alpha = 1;
  if (alpha < 0) alpha = 0;
  if (alpha > 1) alpha = 1;
  /* The previous frame only feeds interpolation (alpha < 1). Not touching it
   * otherwise lets the next frame be captured into it while this one draws. */
  if (alpha >= 1 || !continuous_frames(previous, f)) previous = NULL;
  if (!f->valid || !out || viewport.width < 256 || viewport.width > SM_MAX_WIDTH ||
      viewport.extra < 0 || viewport.width != 256 + 2 * viewport.extra) return false;
  memset(out, 0, (size_t)viewport.width * 224 * sizeof(*out));
  unsigned state = read16(f->ram, 0x998);
  /* At 4:3 the stock picture is shown as is: the custom lines only exist to
   * fill the side margins. */
  bool room = room_state(state) && viewport.extra > 0 &&
              !stock_state[frame_slot].stock_only;
  SmWideWindow wide_windows[224];
  power_bomb_windows(f, wide_windows);
  bool xray = xray_windows(f, wide_windows);
  bool xray_reveal = xray && SmRendererXrayBlocks(f->ram, xray_blocks);
  SmGrapplePiece grapple[17];
  bool grapple_recover_native[17] = {false};
  SmProjectilePose flare, old_flare;
  bool has_flare = grapple_flare_pose(f, &flare);
  if (has_flare) {
    const SmRasterLine *line = &f->lines[64];
    for (unsigned slot = 0; slot < 128; ++slot) {
      int actual_x;
      if (map_piece(flare.bank, flare.map, flare.x, flare.y, 0, 0,
                    line->oam[slot * 2], line->oam[slot * 2 + 1],
                    (line->high_oam[slot / 4] >> (2 * (slot & 3))) & 3, &actual_x))
        ++render_stats.grapple_flare_native_matches;
    }
    has_flare = render_stats.grapple_flare_native_matches != 0;
  }
  if (has_flare && alpha < 1 && grapple_flare_motion(f, previous, &flare, &old_flare)) {
    flare.x = lerp_pixel(old_flare.x, flare.x, alpha);
    flare.y = lerp_pixel(old_flare.y, flare.y, alpha);
  }
  unsigned grapple_count = f->object_state_valid ? SmRendererGrapplePieces(f->object_ram, grapple) : 0;
  for (unsigned i = 0; i < grapple_count; ++i) {
    if (grapple[i].x < 0 || grapple[i].x >= 256 || grapple[i].y < 0 || grapple[i].y >= 256) continue;
    bool matched = false;
    const SmRasterLine *line = &f->lines[64];
    for (unsigned slot = 0; slot < 128; ++slot)
      if (line->oam[slot * 2] == (unsigned)(grapple[i].x | (grapple[i].y << 8)) &&
          line->oam[slot * 2 + 1] == grapple[i].attr &&
          !((line->high_oam[slot / 4] >> (2 * (slot & 3))) & 3)) matched = true;
    if (!matched) { grapple_count = 0; break; }
    ++render_stats.grapple_native_matches;
  }
  if (grapple_count && previous && previous->object_state_valid && alpha < 1 &&
      read16(f->object_ram, 0xd32) == read16(previous->object_ram, 0xd32) &&
      f->object_ram[0xa1e] == previous->object_ram[0xa1e]) {
    bool continuous = true;
    const unsigned coordinates[] = {0xd08,0xd0c,0xd1a,0xd1c};
    for (unsigned i = 0; i < 4; ++i)
      if (abs((int)read16(f->object_ram, coordinates[i]) - (int)read16(previous->object_ram, coordinates[i])) > 32)
        continuous = false;
    SmGrapplePiece before[17];
    unsigned old_count = continuous ? SmRendererGrapplePieces(previous->object_ram, before) : 0;
    int origin_dx = old_count ? before[0].x - grapple[0].x : 0;
    int origin_dy = old_count ? before[0].y - grapple[0].y : 0;
    for (unsigned i = 0; old_count && i < grapple_count; ++i) {
      unsigned old_index = grapple[i].endpoint ? old_count - 1 : i;
      bool new_segment = old_index >= old_count || before[old_index].endpoint != grapple[i].endpoint;
      if (new_segment && grapple[i].endpoint) continue;
      /* Newly emitted segments have no previous piece. Move their current
       * geometry with the beam origin instead of leaving a translation gap. */
      int dx = new_segment ? origin_dx : before[old_index].x - grapple[i].x;
      int dy = new_segment ? origin_dy : before[old_index].y - grapple[i].y;
      if (abs(dx) > 32 || abs(dy) > 32 || (!dx && !dy)) continue;
      grapple_recover_native[i] = grapple[i].x < 0 || grapple[i].x >= 256 ||
                                  grapple[i].y < 0 || grapple[i].y >= 256;
      const SmRasterLine *line = &f->lines[64];
      for (unsigned slot = 0; slot < 128; ++slot) {
        unsigned position = line->oam[slot * 2], attr = line->oam[slot * 2 + 1];
        unsigned high = (line->high_oam[slot / 4] >> (2 * (slot & 3))) & 3;
        if (grapple[i].x >= 0 && grapple[i].x < 256 && grapple[i].y >= 0 && grapple[i].y < 256 &&
            !high && position == (unsigned)(grapple[i].x | (grapple[i].y << 8)) && attr == grapple[i].attr) {
          grapple_motion[slot].position = position; grapple_motion[slot].attr = attr;
          grapple_motion[slot].high = high; grapple_motion[slot].dx = dx;
          grapple_motion[slot].dy = dy; grapple_motion[slot].valid = true;
        }
      }
      grapple[i].x = lerp_pixel(grapple[i].x + dx, grapple[i].x, alpha);
      grapple[i].y = lerp_pixel(grapple[i].y + dy, grapple[i].y, alpha);
      ++render_stats.grapple_interpolated_pieces;
    }
  }
  if (xray_reveal) {
    const char *report_path = getenv("SM_XRAY_TILE_REPORT");
    FILE *report = report_path ? fopen(report_path, "w") : NULL;
    if (report) fputs("world_x,world_y,quadrant,block,expected,actual,bg1_backup,base_room\n", report);
    unsigned w = read16(f->ram, 0x7a5), h = read16(f->ram, 0x7a7);
    unsigned bx = read16(f->ram, 0x911) / 16, by = read16(f->ram, 0x915) / 16;
    for (unsigned y = 0; y < 16 && by + y < h; ++y)
      for (unsigned x = 0; x < 16 && bx + x < w; ++x)
        for (unsigned q = 0; q < 4; ++q) {
          unsigned expected = xray_entry(f->ram, xray_blocks[(by + y) * w + bx + x], q);
          unsigned actual = read16(f->ram, 0x4000 + y * 128 + x * 4 + (q & 1) * 2 + (q >> 1) * 64);
          if (report) {
            unsigned tx = (((read16(f->ram, 0x911) + read16(f->ram, 0x91d)) & ~15u) / 8 + 2 * x + (q & 1)) & 63;
            unsigned ty = (((read16(f->ram, 0x915) + read16(f->ram, 0x91f)) & ~15u) / 8 + 2 * y + (q >> 1)) & 31;
            unsigned backup = read16(f->ram, 0x6000 + 2 * ((tx & 31) + ty * 32 + ((tx & 32) ? 1024 : 0)));
            uint16_t base = 0;
            SmRendererRoomTile(f->ram, (int)((bx + x) * 16 + (q & 1) * 8),
                               (int)((by + y) * 16 + (q >> 1) * 8), &base);
            fprintf(report, "%u,%u,%u,%04x,%04x,%04x,%04x,%04x\n", bx + x, by + y, q,
                    xray_blocks[(by + y) * w + bx + x], expected, actual, backup, base);
          }
          ++render_stats.xray_tiles_compared;
          if (expected != actual) ++render_stats.xray_tiles_differ;
        }
    if (report) fclose(report);
  }
  static unsigned s_generation;
  SmDrawCtx ctx = {
    .out = out, .viewport = viewport, .hud_anchored = hud_anchored, .alpha = alpha,
    .f = f, .previous = previous, .room = room, .wide_windows = wide_windows,
    .xray = xray, .xray_reveal = xray_reveal, .grapple = grapple,
    .grapple_recover_native = grapple_recover_native, .grapple_count = grapple_count,
    .has_flare = has_flare, .flare = flare, .generation = ++s_generation,
  };
#ifdef __vita__
  SnesRenderWorkers *pool = line_workers();
  static bool s_started;
  if (pool && !s_started) { s_started = true; fprintf(stderr, "[vita] starting the first multi-core widescreen frame\n"); }
  if (pool && SnesRenderWorkersRun(pool, 224, 8, draw_lines_range, &ctx)) {
    static bool logged;
    if (!logged) { logged = true; fprintf(stderr, "[vita] first multi-core widescreen frame done\n"); }
    return true;
  }
#endif
  for (int y = 0; y < 224; ++y) draw_line(&ctx, y, 0);
  return true;
}
