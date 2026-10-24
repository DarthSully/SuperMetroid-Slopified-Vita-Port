#include "sm_spc_player.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "types.h"
#include "snes/spc.h"
#include "snes/dsp_regs.h"

/* SmSpcPlayer is a minimal SPC-image loader (same shape as MMX/SMW's):
 * it allocates a 64KB APU-RAM buffer, resets the DSP to sane defaults,
 * and uploads the game's ROM-packed audio image into that buffer. Real
 * audio runs through the framework's SPC core in
 * snesrecomp/runner/src/snes/apu.c. */
typedef struct SmSpcPlayer {
  SpcPlayer base;
  uint8 ram[65536];
} SmSpcPlayer;

static void Dsp_Write(SmSpcPlayer *p, uint8_t reg, uint8 value) {
  if (p->base.dsp)
    dsp_write(p->base.dsp, reg, value);
}

static const uint8 kDefDspRegs[12] = { MVOLL,MVOLR,EVOLL,EVOLR,FLG,EFB,PMON,NON,EON,DIR,ESA,EDL };
static const uint8 kDefDspValues[12] = { 0x7F, 0x7F,  0,  0, 0x2F, 0x60,  0,  0,  0, 0x80, 0x60, 2 };

static void Spc_Reset(SmSpcPlayer *p) {
  memset(p->ram, 0, 0x500);
  memset(p->base.input_ports, 0, sizeof(p->base.input_ports));
  for (int i = 11; i >= 0; i--)
    Dsp_Write(p, kDefDspRegs[i], kDefDspValues[i]);
}

static void SmSpcPlayer_Initialize(SpcPlayer *p_in) {
  SmSpcPlayer *p = (SmSpcPlayer *)p_in;
  dsp_reset(p->base.dsp);
  Spc_Reset(p);
}

static void SmSpcPlayer_Upload(SpcPlayer *p_in, const uint8_t *data) {
  SmSpcPlayer *p = (SmSpcPlayer *)p_in;
  Dsp_Write(p, FLG, 0x60);
  Dsp_Write(p, KOF, 0xff);
  for (;;) {
    int numbytes = *(uint16 *)(data);
    if (numbytes == 0) {
      break;
    }
    int target = *(uint16 *)(data + 2);
    data += 4;
    do {
      p->ram[target++ & 0xffff] = *data++;
    } while (--numbytes);
  }
  p->base.port_to_snes[0] = p->base.port_to_snes[1] = p->base.port_to_snes[2] = p->base.port_to_snes[3] = 0;
  memset(p->base.input_ports, 0, sizeof(p->base.input_ports));
  Dsp_Write(p, FLG, 0x20);
}

/* Ports first, then the RAM image: everything the guest can observe about the
 * player. The DSP object hanging off it is audio OUTPUT state, which the
 * framework deliberately does not rewind (see the DspOutputRing note in
 * common_rtl.h) -- a rewound output ring would stutter the live audio thread
 * without changing anything the guest can see. */
typedef struct SmSpcPlayerState {
  uint8 input_ports[4];
  uint8 port_to_snes[4];
  uint8 ram[65536];
} SmSpcPlayerState;

size_t SmSpcPlayer_StateSize(void) {
  return g_spc_player ? sizeof(SmSpcPlayerState) : 0;
}

size_t SmSpcPlayer_SaveState(void *out, size_t capacity) {
  SmSpcPlayer *p = (SmSpcPlayer *)g_spc_player;
  SmSpcPlayerState st;
  if (!p || !out || capacity < sizeof(st)) return 0;
  memcpy(st.input_ports, p->base.input_ports, sizeof(st.input_ports));
  memcpy(st.port_to_snes, p->base.port_to_snes, sizeof(st.port_to_snes));
  memcpy(st.ram, p->ram, sizeof(st.ram));
  memcpy(out, &st, sizeof(st));
  return sizeof(st);
}

int SmSpcPlayer_LoadState(const void *in, size_t size) {
  SmSpcPlayer *p = (SmSpcPlayer *)g_spc_player;
  SmSpcPlayerState st;
  if (!p || !in || size < sizeof(st)) return 0;
  memcpy(&st, in, sizeof(st));
  memcpy(p->base.input_ports, st.input_ports, sizeof(st.input_ports));
  memcpy(p->base.port_to_snes, st.port_to_snes, sizeof(st.port_to_snes));
  memcpy(p->ram, st.ram, sizeof(st.ram));
  return 1;
}

SpcPlayer *SmSpcPlayer_Create(void) {
  SmSpcPlayer *p = (SmSpcPlayer *)malloc(sizeof(SmSpcPlayer));
  memset(p, 0, sizeof(SmSpcPlayer));
  p->base.dsp = dsp_init(p->ram);
  p->base.initialize = &SmSpcPlayer_Initialize;
  p->base.upload = &SmSpcPlayer_Upload;
  return &p->base;
}
