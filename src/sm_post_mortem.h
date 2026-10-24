#pragma once

/* Install Super Metroid's section into the framework's crash/exit report.
 * Called once from SmRegisterGame(); see src/sm_post_mortem.c. */
void SmPostMortemRegister(void);
