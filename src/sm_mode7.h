#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Renderer-owned scanline transform, in Q8 texture coordinates. No pointers
 * into mutable guest memory: callers latch registers and VRAM at scanout. */
typedef struct SmMode7Line {
  double origin_x, origin_y, step_x, step_y;
  uint8_t control;
} SmMode7Line;

SmMode7Line SmMode7Transform(const int16_t matrix[8], uint8_t control,
                                   unsigned scanline);
/* Returns a palette index; zero is transparent. X is a signed logical SNES
 * coordinate and may extend beyond either stock screen edge. */
uint8_t SmMode7Sample(const SmMode7Line *line,
                         const uint16_t vram[0x8000], double x);
/* Eligibility (scene, camera discontinuities, loads) belongs to the snapshot
 * publisher. Wrap periodic texture coordinates, never linearly blend pixels. */
SmMode7Line SmMode7Interpolate(SmMode7Line previous,
                                     SmMode7Line current, double alpha);
/* Solve this scanline's world-to-screen transform. Residual is measured in
 * texture pixels and lets the object projector find the matching scanline. */
bool SmMode7Project(const SmMode7Line *line, double world_x,
                        double world_y, double *screen_x, double *residual);
/* Retail $8B:8AD9 object projection (not the inverse raster transform).
 * Preserve separate signed products, Q8 truncation and 16-bit wrapping. */
void SmMode7ObjectPosition(uint16_t a, uint16_t b, uint16_t c,
                          uint16_t center_x, uint16_t center_y,
                          uint16_t world_x, uint16_t world_y,
                          uint16_t camera_x, uint16_t camera_y, int *x, int *y);
