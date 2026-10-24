#pragma once
#include "spc_player.h"

SpcPlayer *SmSpcPlayer_Create(void);

/* Rollback-only state of the player itself.
 *
 * The player is HOST state the guest talks to: the four ports it answers on,
 * and the APU RAM image a track upload leaves in it. A rollback restores the
 * machine's RAM but not this, so speculative frames that touched the audio
 * queue left the guest's bookkeeping disagreeing with the player it was
 * talking to -- measured as two bytes of sfx state drifting on 21 frames in
 * 1,200 with run-ahead on. Carried in SM's exec_state chunk; see sm_rtl.c. */
size_t SmSpcPlayer_StateSize(void);
size_t SmSpcPlayer_SaveState(void *out, size_t capacity);
int    SmSpcPlayer_LoadState(const void *in, size_t size);
