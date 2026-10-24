#include "sm_mode7.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* Independent integer reference for the native PPU's non-mosaic mode-7
 * address path. Unsigned arithmetic intentionally models its Q8 wrap. */
static unsigned reference(const int16_t m[8], unsigned control, unsigned y,
                          int x, const uint16_t *vram) {
  int h = (int16_t)((uint16_t)m[6] << 3) / 8;
  int v = (int16_t)((uint16_t)m[7] << 3) / 8;
  int cx = (int16_t)((uint16_t)m[4] << 3) / 8;
  int cy = (int16_t)((uint16_t)m[5] << 3) / 8;
  h -= cx; v -= cy;
  h = h & 0x2000 ? h | ~1023 : h & 1023;
  v = v & 0x2000 ? v | ~1023 : v & 1023;
  int ry = control & 2 ? 255 - (int)y : (int)y;
  int rx = control & 1 ? 255 - x : x;
  uint32_t qx = (uint32_t)((m[0] * h & ~63) + (m[1] * ry & ~63) +
                         (m[1] * v & ~63) + cx * 256 + m[0] * rx);
  uint32_t qy = (uint32_t)((m[2] * h & ~63) + (m[3] * ry & ~63) +
                         (m[3] * v & ~63) + cy * 256 + m[2] * rx);
  unsigned tile;
  if ((control & 128) && (qx | qy) > 0x3ffff) {
    if (!(control & 64)) return 0;
    tile = 0;
  } else {
    tile = vram[((qy >> 11) & 127) * 128 + ((qx >> 11) & 127)] & 255;
  }
  return vram[tile * 64 + ((qy >> 8) & 7) * 8 + ((qx >> 8) & 7)] >> 8;
}

int main(void) {
  static uint16_t vram[0x8000];
  /* Every map cell selects tile 1; each texel encodes its column and row.
   * Tile 0 has a separate value for overflow-fill assertions. */
  for (int i = 0; i < 0x4000; ++i) vram[i] = 1;
  for (int i = 0; i < 64; ++i) {
    vram[i] |= 200 << 8;
    vram[64 + i] |= (i + 1) << 8;
  }
  int16_t m[8] = {256, 0, 0, 256, 0, 0, 0, 0};
  SmMode7Line line = SmMode7Transform(m, 0, 3);
  /* Both margins sample real map texels, including negative coordinates. */
  for (int x = -213; x < 469; ++x)
    CHECK(SmMode7Sample(&line, vram, x) == 25 + ((x % 8 + 8) % 8));
  line = SmMode7Transform(m, 1, 3);
  CHECK(SmMode7Sample(&line, vram, 0) == 32);
  CHECK(SmMode7Sample(&line, vram, -1) == 25);
  line = SmMode7Transform(m, 2, 3);
  CHECK(SmMode7Sample(&line, vram, 0) == 33); /* y=252 */
  line = SmMode7Transform(m, 0x80, 3);
  CHECK(SmMode7Sample(&line, vram, -1) == 0);
  CHECK(SmMode7Sample(&line, vram, 1024) == 0);
  line = SmMode7Transform(m, 0xc0, 3);
  CHECK(SmMode7Sample(&line, vram, -1) == 200);
  CHECK(SmMode7Sample(&line, vram, 0) == 25);
  /* Quarter-turn camera: screen x changes world y; scanline changes world x. */
  m[0] = 0; m[1] = -256; m[2] = 256; m[3] = 0;
  line = SmMode7Transform(m, 0, 3);
  CHECK(SmMode7Sample(&line, vram, 2) == 22);
  double sx, residual;
  CHECK(SmMode7Project(&line, -3, 300, &sx, &residual));
  CHECK(sx == 300 && residual == 0);
  CHECK(SmMode7Project(&line, 1, 300, &sx, &residual));
  CHECK(residual == 4);
  SmMode7Line a = {1023 * 256, 0, 256, 0, 0}, b = {1 * 256, 0, 256, 0, 0};
  line = SmMode7Interpolate(a, b, 0.5);
  CHECK(line.origin_x == 1024 * 256); /* crosses seam by 2 pixels, not 1022 */
  CHECK(SmMode7Sample(&line, vram, 0) == 1);
  a.control = b.control = 0x80;
  CHECK(SmMode7Interpolate(a, b, 0.5).origin_x == 512 * 256);
  a.control = 0;
  CHECK(SmMode7Interpolate(a, b, 0.1).origin_x == b.origin_x);
  line = (SmMode7Line){0};
  CHECK(!SmMode7Project(&line, 1, 1, &sx, &residual));
  line.origin_x = NAN;
  CHECK(SmMode7Sample(&line, vram, 0) == 0);
  uint32_t rng = 0x12345678;
  for (unsigned test = 0; test < 1000; ++test) {
    for (unsigned i = 0; i < 8; ++i) {
      rng = rng * 1664525u + 1013904223u;
      m[i] = (int16_t)(rng >> 16);
    }
    unsigned control = (test & 3) | ((test & 12) << 4);
    unsigned y = 1 + test % 224;
    line = SmMode7Transform(m, (uint8_t)control, y);
    for (int x = -213; x < 469; x += 7)
      CHECK(SmMode7Sample(&line, vram, x) == reference(m, control, y, x, vram));
  }
  int object_x, object_y;
  SmMode7ObjectPosition(256, 0, 0, 128, 200, 300, 150, 20, 10, &object_x, &object_y);
  CHECK(object_x == 280 && object_y == 140);
  SmMode7ObjectPosition(0, 256, (uint16_t)-256, 128, 200, 128, 0, 0, 0, &object_x, &object_y);
  CHECK(object_x == 328 && object_y == 200);
  /* Independent floating-point floor oracle: each signed product truncates
   * separately before the 16-bit additions, including negative fractions. */
  for (unsigned test = 0; test < 10000; ++test) {
    uint16_t values[9];
    for (unsigned i = 0; i < 9; ++i) {
      rng = rng * 1664525u + 1013904223u;
      values[i] = (uint16_t)(rng >> 16);
    }
    int dx = (int16_t)(uint16_t)(values[5] - values[3]);
    int dy = (int16_t)(uint16_t)(values[4] - values[6]);
    int rx = (int)floor(dx * (double)(int16_t)values[0] / 256) +
             (int)floor((int16_t)values[1] * (double)dy / 256);
    int ry = (int)floor((int16_t)values[2] * (double)dx / 256) +
             (int)floor((int16_t)values[0] * (double)dy / 256);
    SmMode7ObjectPosition(values[0], values[1], values[2], values[3], values[4],
                          values[5], values[6], values[7], values[8], &object_x, &object_y);
    CHECK(object_x == (int16_t)(uint16_t)(rx + values[3] - values[7]));
    CHECK(object_y == (int16_t)(uint16_t)(values[4] - ry - values[8]));
  }
  puts("Super Metroid Mode 7: signed margins, flips, overflow, projection, and interpolation passed");
  return 0;
}
