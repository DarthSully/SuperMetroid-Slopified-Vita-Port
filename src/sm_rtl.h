#ifndef SM_SM_RTL_H_
#define SM_SM_RTL_H_
#include "common_rtl.h"
#include "common_cpu_infra.h"
#include "snes/snes_regs.h"

/* Per-frame host orchestration for Super Metroid (see sm_rtl.c). */
void SmDrawPpuFrame(void);
void RunOneFrameOfGame(void);

/* Yield the game fiber back to the host (called from the WaitForNMI
 * HLE in gen_stubs.c). */
void sm_host_yield(void);

/* File snapshot extension for the default LLE scheduler (no host pointers). */
void SmStateSaveExtra(struct SaveLoadInfo *sli);
void SmStateLoadExtra(struct SaveLoadInfo *sli, uint32_t version);
void SmOnStateLoaded(uint32_t version);

/* Rollback-only execution position: the game fiber's live stack and context
 * plus the register file parked beside it. See the block comment in
 * sm_rtl.c. Registered as RtlGameInfo.exec_state_*; 0/failure everywhere the
 * fiber backend cannot snapshot (Windows fibers, Android threads), which is
 * what makes run-ahead decline there instead of corrupting the timeline. */
size_t SmExecStateBound(void);
size_t SmExecStateSave(void *out, size_t capacity);
int    SmExecStateLoad(const void *in, size_t size);

#endif  /* SM_SM_RTL_H_ */
