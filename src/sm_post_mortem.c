/*
 * sm_post_mortem.c — Super Metroid's section of the crash/exit report.
 *
 * This is all that is left of what used to be an 833-line private fork of the
 * framework's post_mortem.c. The fork existed to add ONE game-specific JSON
 * object; carrying it meant this port also carried its own copy of every
 * generic dumper, and stopped inheriting fixes to them. The generic half has
 * been moved up into snesrecomp/runner/src/desktop/post_mortem.c (it was
 * ahead of the framework's: the DB tripwire, the stack-balance / unresolved-
 * abandon / dispatch-log / PPU-DMA dumps and the trace-depth knob all came
 * from here), and what remains registers through the framework's
 * recomp_post_mortem_set_game_section() hook.
 *
 * Registered from SmRegisterGame() in sm_cpu_infra.c, so the report carries
 * it from the first frame — including a crash during boot.
 */

#include <stdint.h>
#include <stdio.h>

#include "post_mortem.h"
#include "cpu_state.h"

extern uint8_t g_ram[0x20000];
extern CpuState g_cpu;

static void dump_ram_bytes_json(FILE *f, uint32_t off, uint32_t n) {
    fputc('[', f);
    for (uint32_t i = 0; i < n; i++)
        fprintf(f, "%s%u", (i ? "," : ""), (unsigned)g_ram[off + i]);
    fputc(']', f);
}

/* Runs inside the post-mortem's dump lock, from a crash handler: reads of
 * g_ram and g_cpu only, no allocation, no locks. */
static void sm_post_mortem_section(FILE *f) {
    /* Super Metroid WRAM landmarks. The generic report already carries the
     * CPU state and the rings; these are the addresses that make an SM crash
     * legible without a symbol table to hand. */
    fprintf(f, "  \"sm_wram\": {\n");
    fprintf(f, "    \"game_state_0998\": ");        dump_ram_bytes_json(f, 0x0998, 2);   fprintf(f, ",\n");
    fprintf(f, "    \"DP_0080\": ");                dump_ram_bytes_json(f, 0x0080, 32);  fprintf(f, ",\n");
    fprintf(f, "    \"DP_low_0000\": ");            dump_ram_bytes_json(f, 0x0000, 32);  fprintf(f, ",\n");
    fprintf(f, "    \"player_state_1925\": ");      dump_ram_bytes_json(f, 0x1925, 32);  fprintf(f, ",\n");
    fprintf(f, "    \"init_sig_7F8000\": ");        dump_ram_bytes_json(f, 0x18000, 4);  fprintf(f, "\n");
    fprintf(f, "  },\n");


    /* SM enemy / demo-state observability (2026-06-21, WriteEnemyOams f2689
     * crash). Always-on read of g_ram — no pause/step. The crashing enemy is
     * gEnemyData(cur_enemy_index) = g_ram + 0xF78 + cur_enemy_index; its
     * spritemap loop reads `n = *RomPtr(bank, spritemap_pointer)`, so a garbage
     * bank(+0x2E)/spritemap_pointer(+0x16)/extra_properties(+0x10) is the loop
     * runaway. enemy_ptr(+0)==0 means the slot is EMPTY (drawn dead slot). */
    {
        unsigned cur = (unsigned)(g_ram[0x0E54] | (g_ram[0x0E55] << 8));
        fprintf(f, "  \"sm\": {\n");
        fprintf(f, "    \"game_state_0998\": %u,\n",
                (unsigned)(g_ram[0x0998] | (g_ram[0x0999] << 8)));
        fprintf(f, "    \"cur_enemy_index_0E54\": %u,\n", cur);
        unsigned slot_off = 0x0F78u + (cur & 0xFFFFu);
        if (slot_off + 64u <= 0x20000u) {
            fprintf(f, "    \"crash_enemy_slot\": ");
            dump_ram_bytes_json(f, slot_off, 64);
            fprintf(f, ",\n");
        } else {
            fprintf(f, "    \"crash_enemy_slot\": null,\n");
        }
        /* Per-slot {enemy_ptr, extra_properties(+0x10), spritemap_ptr(+0x16),
         * bank(+0x2E)} for the first 8 slots — WriteEnemyOams takes the
         * extended-spritemap loop iff extra_properties&4; the loop reads the
         * count from bank:spritemap_ptr. Which slot has &4 set + its bank vs
         * the live DB pins the runaway. */
        fprintf(f, "    \"slots\": [");
        for (int i = 0; i < 8; i++) {
            unsigned off = 0x0F78u + (unsigned)i * 0x40u;
            fprintf(f, "%s{\"i\":%d,\"enemy_ptr\":%u,\"extra_prop\":%u,"
                    "\"spritemap_ptr\":%u,\"bank\":%u}",
                    (i ? "," : ""), i,
                    (unsigned)(g_ram[off] | (g_ram[off + 1] << 8)),
                    (unsigned)(g_ram[off + 0x10] | (g_ram[off + 0x11] << 8)),
                    (unsigned)(g_ram[off + 0x16] | (g_ram[off + 0x17] << 8)),
                    (unsigned)g_ram[off + 0x2E]);
        }
        fprintf(f, "],\n");
        fprintf(f, "    \"live_DB\": %u, \"live_X\": %u, \"live_Y\": %u\n  },\n",
                (unsigned)g_cpu.DB, (unsigned)g_cpu.X, (unsigned)g_cpu.Y);
    }
}

void SmPostMortemRegister(void) {
    recomp_post_mortem_set_game_section(&sm_post_mortem_section);
}
