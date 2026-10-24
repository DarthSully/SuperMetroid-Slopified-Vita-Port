/* Unit test for the Vita double-tap-left gesture state machine (sm_vita.c).
 * Platform-independent: sm_vita.c builds only the state machine off-Vita. */
#include <stdio.h>
#include <stdlib.h>

#include "sm_vita.h"

/* sm_vita.c's per-frame hooks post on-screen messages; not under test here. */
void snes_osd_push(const char *msg, int duration_ms) { (void)msg; (void)duration_ms; }

static int failures;
#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL: %s\n", what); ++failures; } \
    else printf("ok:   %s\n", what); } while (0)

static SmVitaTapState fresh(void) { SmVitaTapState s = {0}; return s; }

int main(void) {
  SmVitaTapState s;

  s = fresh();
  CHECK(!SmVita_TapDown(&s, 0.20f, 0.50f, 1000, 1), "first left tap does not toggle");
  CHECK(SmVita_TapDown(&s, 0.22f, 0.52f, 1200, 1), "second left tap within 200 ms toggles");
  CHECK(!SmVita_TapDown(&s, 0.22f, 0.52f, 1300, 1), "third tap starts a new pair (no double toggle)");
  CHECK(SmVita_TapDown(&s, 0.21f, 0.50f, 1500, 1), "fourth tap completes the next pair");

  s = fresh();
  SmVita_TapDown(&s, 0.20f, 0.50f, 1000, 1);
  CHECK(!SmVita_TapDown(&s, 0.20f, 0.50f, 1400, 1), "taps 400 ms apart do not toggle");

  s = fresh();
  SmVita_TapDown(&s, 0.70f, 0.50f, 1000, 1);
  CHECK(!SmVita_TapDown(&s, 0.70f, 0.50f, 1100, 1), "right-side double tap ignored");

  s = fresh();
  SmVita_TapDown(&s, 0.20f, 0.50f, 1000, 1);
  CHECK(!SmVita_TapDown(&s, 0.60f, 0.50f, 1100, 1), "left then right does not toggle");

  /* Right half (infinite energy): the mirror image of the left gesture. */
  s = fresh();
  CHECK(!SmVita_TapDownSide(&s, 0.80f, 0.50f, 1000, 1, true), "first right tap does not toggle");
  CHECK(SmVita_TapDownSide(&s, 0.81f, 0.51f, 1200, 1, true), "second right tap within 200 ms toggles");
  s = fresh();
  SmVita_TapDownSide(&s, 0.20f, 0.50f, 1000, 1, true);
  CHECK(!SmVita_TapDownSide(&s, 0.20f, 0.50f, 1100, 1, true), "left-side double tap ignored by the right gesture");
  s = fresh();
  SmVita_TapDownSide(&s, 0.80f, 0.50f, 1000, 1, true);
  CHECK(!SmVita_TapDownSide(&s, 0.80f, 0.50f, 1100, 2, true), "two fingers cancel the right gesture");

  s = fresh();
  SmVita_TapDown(&s, 0.10f, 0.20f, 1000, 1);
  CHECK(!SmVita_TapDown(&s, 0.30f, 0.80f, 1100, 1), "two far-apart left taps do not toggle");

  s = fresh();
  SmVita_TapDown(&s, 0.20f, 0.50f, 1000, 1);
  CHECK(!SmVita_TapDown(&s, 0.20f, 0.50f, 1100, 2), "second finger cancels");

  s = fresh();
  SmVita_TapDown(&s, 0.20f, 0.50f, 0xFFFFFF00u, 1);
  CHECK(SmVita_TapDown(&s, 0.20f, 0.50f, 0x00000010u, 1), "timestamp wraparound handled");

  s = fresh();
  CHECK(!SmVita_TapDown(&s, 0.4999f, 0.5f, 1000, 1) &&
        SmVita_TapDown(&s, 0.4999f, 0.5f, 1100, 1), "just left of centre counts");
  s = fresh();
  SmVita_TapDown(&s, 0.50f, 0.5f, 1000, 1);
  CHECK(!SmVita_TapDown(&s, 0.50f, 0.5f, 1100, 1), "exact centre counts as right");

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
         failures == 1 ? "" : "s");
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
