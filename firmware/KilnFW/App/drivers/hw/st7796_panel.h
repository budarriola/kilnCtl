// st7796_panel.h -- the ST7796's panel_desc_t (DISPLAY_ST7796_PLAN.md Sec.6
// Step 2, Sec.12 Phase 3). No SPI/expander code lives here: panel_spi.c (the
// old ILI9488.c, generic as of Phase 3) is the one driver for both panels,
// parameterised by whichever descriptor ILI9488_start() picks. This file is
// pure data -- host-testable, no ESP-IDF includes -- same reasoning as
// panel_codec.h/.c.
//
// THE ST7796 IS NOT CONNECTED TO THIS BOARD. Nothing here has run on
// hardware; see the STOP block at the top of DISPLAY_ST7796_PLAN.md before
// the MSP4031 module ever gets near J2. This descriptor exists so the
// KILNCTL_DISPLAY_PANEL=st7796 Kconfig choice compiles and is host-testable,
// not because bring-up has happened.
#ifndef ST7796_PANEL_H
#define ST7796_PANEL_H

#include "panel_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

// Native (unrotated) geometry -- identical to the ILI9488's, per
// DISPLAY_ST7796_PLAN.md Sec.6 Step 4 (both panels are 320x480 native,
// 480x320 landscape; the plan records this as an explicit invariant the
// static UI-budget asserts depend on).
#define ST7796_PANEL_WIDTH  320
#define ST7796_PANEL_HEIGHT 480

// Returns the ST7796's panel_desc_t. Always the same static instance, never
// NULL. Not yet reachable from a running board (KILNCTL_DISPLAY_PANEL
// defaults to ili9488, and there is no auto-detection until Phase 4), but
// selectable today via that Kconfig choice for host-testing and for a bench
// that has actually run the STOP-block measurement.
const panel_desc_t *ST7796_get_panel_desc(void);

#ifdef __cplusplus
}
#endif

#endif // ST7796_PANEL_H
