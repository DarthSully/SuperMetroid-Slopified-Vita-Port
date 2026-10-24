/*
 * snes_ovl_blit_panel: the compositing step that puts the framework's
 * save-state browser and rewind filmstrip onto this port's screen.
 *
 * Worth a test because the failure is silent in the worst way: the overlay
 * modules are linked, their state machines run, the panel is rasterized —
 * and if the blit is wrong the player simply sees the game, with no error
 * anywhere. That is exactly the shape this port already shipped for months
 * (the modules were linked and nothing called them at all).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_overlay_draw.h"

#define DST_W 256
#define DST_H 224
#define PANEL_W 512
#define PANEL_H 448

static int g_fail;
static int g_checks;

static void check(int cond, const char *what) {
    g_checks++;
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        g_fail++;
        printf("  FAIL  %s\n", what);
    }
}

static uint32_t *g_dst;
static uint32_t *g_panel;

static uint32_t px(int x, int y) { return g_dst[(size_t)y * DST_W + x]; }

static void reset_dst(uint32_t fill) {
    for (int i = 0; i < DST_W * DST_H; i++)
        g_dst[i] = fill;
}

static void fill_panel(uint32_t colour) {
    for (int i = 0; i < PANEL_W * PANEL_H; i++)
        g_panel[i] = colour;
}

int main(void) {
    g_dst = malloc(sizeof(uint32_t) * DST_W * DST_H);
    g_panel = malloc(sizeof(uint32_t) * PANEL_W * PANEL_H);
    if (!g_dst || !g_panel)
        return 2;

    printf("snes_ovl_blit_panel\n");

    /* An opaque panel at exactly 2x the destination covers all of it. The
     * real panels are authored at 512x448 over a 256x224 field, so this is
     * the case that actually ships. */
    reset_dst(0xFF000000u);
    fill_panel(0xFF3366CCu);
    snes_ovl_blit_panel((uint8_t *)g_dst, DST_W * 4, DST_W, DST_H,
                        g_panel, PANEL_W, PANEL_H);
    {
        int all = 1;
        for (int i = 0; i < DST_W * DST_H; i++)
            if (g_dst[i] != 0xFF3366CCu) { all = 0; break; }
        check(all, "an opaque 2:1 panel covers the whole frame");
    }

    /* Fully transparent pixels leave the game visible. A panel that blanked
     * the screen where it meant to show through would hide the very frame
     * the player is choosing a save-state from. */
    reset_dst(0xFF102030u);
    fill_panel(0x00FFFFFFu);
    snes_ovl_blit_panel((uint8_t *)g_dst, DST_W * 4, DST_W, DST_H,
                        g_panel, PANEL_W, PANEL_H);
    check(px(0, 0) == 0xFF102030u && px(DST_W - 1, DST_H - 1) == 0xFF102030u,
          "alpha 0 leaves the underlying frame untouched");

    /* Half alpha blends rather than replacing. */
    reset_dst(0xFF000000u);
    fill_panel(0x80FFFFFFu);
    snes_ovl_blit_panel((uint8_t *)g_dst, DST_W * 4, DST_W, DST_H,
                        g_panel, PANEL_W, PANEL_H);
    {
        uint32_t v = px(128, 112) & 0xFFu;
        check(v > 100 && v < 160, "alpha 0x80 over black blends to mid grey");
    }

    /* A panel wider than it is tall, into a taller frame, is letterboxed and
     * centred — not stretched, and never written outside its own box. */
    reset_dst(0xFF000000u);
    fill_panel(0xFFFF0000u);
    snes_ovl_blit_panel((uint8_t *)g_dst, DST_W * 4, DST_W, DST_H,
                        g_panel, 512, 128);
    {
        int top_clear = (px(0, 0) == 0xFF000000u);
        int middle_red = (px(DST_W / 2, DST_H / 2) == 0xFFFF0000u);
        int bottom_clear = (px(0, DST_H - 1) == 0xFF000000u);
        check(top_clear && middle_red && bottom_clear,
              "a wide panel is centred with the frame left visible above/below");
    }

    /* Degenerate inputs must not write anything. A crash here would land in
     * the present path of every frame. */
    reset_dst(0xFF5A5A5Au);
    snes_ovl_blit_panel(NULL, DST_W * 4, DST_W, DST_H, g_panel, PANEL_W, PANEL_H);
    snes_ovl_blit_panel((uint8_t *)g_dst, DST_W * 4, DST_W, DST_H, NULL, PANEL_W, PANEL_H);
    snes_ovl_blit_panel((uint8_t *)g_dst, DST_W * 4, 0, 0, g_panel, PANEL_W, PANEL_H);
    snes_ovl_blit_panel((uint8_t *)g_dst, DST_W * 4, DST_W, DST_H, g_panel, 0, 0);
    check(px(0, 0) == 0xFF5A5A5Au && px(DST_W - 1, DST_H - 1) == 0xFF5A5A5Au,
          "null / zero-sized arguments are refused without writing");

    /* The blit must stay inside the row it is given: a pitch wider than the
     * visible width is the normal case for a widescreen buffer. */
    {
        const int pitch_px = DST_W + 64;
        uint32_t *wide = malloc(sizeof(uint32_t) * pitch_px * DST_H);
        if (!wide) return 2;
        for (int i = 0; i < pitch_px * DST_H; i++) wide[i] = 0xFFDEAD00u;
        fill_panel(0xFF00FF00u);
        snes_ovl_blit_panel((uint8_t *)wide, pitch_px * 4, DST_W, DST_H,
                            g_panel, PANEL_W, PANEL_H);
        int guard_ok = 1;
        for (int y = 0; y < DST_H; y++)
            for (int x = DST_W; x < pitch_px; x++)
                if (wide[(size_t)y * pitch_px + x] != 0xFFDEAD00u) { guard_ok = 0; break; }
        check(guard_ok, "nothing is written past dst_w when pitch is wider");
        check(wide[0] == 0xFF00FF00u, "and the visible area still receives the panel");
        free(wide);
    }

    printf("%d/%d checks passed\n", g_checks - g_fail, g_checks);
    free(g_dst);
    free(g_panel);
    return g_fail ? 1 : 0;
}
