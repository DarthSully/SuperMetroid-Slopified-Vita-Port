#include "common_cpu_infra.h"
#include "sm_rtl.h"
#include "sm_post_mortem.h"

/* Called by the framework from SnesInit, once the machine exists. */
static void SmInitialize(void) {
  /* Add Super Metroid's object to the crash/exit report. Registered through
   * the framework hook rather than by forking post_mortem.c, which is how
   * this port previously carried its own 833-line copy of that file. */
  SmPostMortemRegister();
}

/* Game registration consumed by RtlRegisterGame() in main.c. The
 * `.title` / `.save_name_prefix` strings drive the save-file naming
 * (saves/save<N>.sav). run_frame / draw_ppu_frame are the per-frame
 * host hooks implemented in sm_rtl.c. */
const RtlGameInfo kSuperMetroidGameInfo = {
  .title = "sm",
  .initialize = &SmInitialize,
  .run_frame = &RunOneFrameOfGame,
  .draw_ppu_frame = &SmDrawPpuFrame,
  .save_name_prefix = "save",
  .state_save_extra = &SmStateSaveExtra,
  .state_load_extra = &SmStateLoadExtra,
  .on_state_loaded = &SmOnStateLoaded,
  /* The guest's position in its own code is this port's fiber call chain, and
   * a rollback that does not carry it leaves the fiber ahead of the RAM. */
  .exec_state_bound = &SmExecStateBound,
  .exec_state_save  = &SmExecStateSave,
  .exec_state_load  = &SmExecStateLoad,
};
