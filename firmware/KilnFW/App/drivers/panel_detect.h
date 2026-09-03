// panel_detect.h -- pure panel-selection decision logic.
//
// DISPLAY_ST7796_PLAN.md Sec.6 Step 3 / Sec.12 Phase 4: given the three RDDID
// (0x04) bytes actually read off the panel on this wiring and which touch
// controller address (if any) answered on the shared I2C bus, decide which
// panel_desc_t to boot. Free of ESP-IDF/FreeRTOS/spi_owner/i2c includes --
// like panel_codec.h and st7796_panel.c, this links straight into the host
// test binary with no stub layer, the same split max31856_codec.c set the
// precedent for.
//
// *** THE ID TABLE IS EMPTY. *** Both ili9488_panel_desc.id_matches and
// st7796_panel_desc.id_matches (panel_spi.c, st7796_panel.c) are NULL today
// because Sec.4's "RDDID bytes from the ILI9488 on this wiring" and "...from
// the ST7796 on this wiring" bench measurements do not exist yet -- see the
// TODO block right above panel_detect_choose() below for exactly which two
// measurements fill it in. Per Sec.6 Step 3's explicit rule, matchers must be
// written against bytes actually read on this board, never datasheet nominal
// values -- so this file does not invent any. What it guarantees instead is
// that an empty (or a not-yet-populated) table is SAFE: with no candidate's
// id_matches able to return true, panel_detect_choose() always falls back to
// the caller-supplied Kconfig default and reports why. That fallback path,
// not the matching path, is what makes today's ILI9488-only board behave
// identically once this lands.
#ifndef PANEL_DETECT_H
#define PANEL_DETECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which touch controller, if any, corroborates a given panel candidate.
 * UNKNOWN means "this candidate has no touch signal to check" (a panel could
 * ship with no touch controller at all) -- it is not the same as "the touch
 * probe found nothing", which is instead represented by both
 * touch_ns2009_present and touch_ft6336_present being false in the call
 * below. Sec.7 / touch_dev.h already draws the same NS2009-vs-FT6336 line
 * for the read path; this enum exists separately because panel_detect.c
 * intentionally has no dependency on touch_dev.h (that header pulls in
 * esp_err.h and is about *reading* a touch sample, not identifying which
 * chip is on the bus). */
typedef enum {
    PANEL_DETECT_TOUCH_UNKNOWN = 0,
    PANEL_DETECT_TOUCH_NS2009,
    PANEL_DETECT_TOUCH_FT6336,
} panel_detect_touch_kind_t;

typedef struct {
    const panel_desc_t *panel;          /* candidate; never NULL */
    panel_detect_touch_kind_t touch;    /* which touch address, if any, this
                                          * candidate is expected to pair with */
} panel_detect_candidate_t;

/* Why panel_detect_choose() picked the panel it did -- reported by the
 * caller in the boot log/diagnostics/READ_ID reply per Sec.6 Step 3 point 5. */
typedef enum {
    PANEL_DETECT_SOURCE_SPI_MATCH,      /* exactly one candidate's id_matches()
                                          * returned true */
    PANEL_DETECT_SOURCE_TOUCH_TIEBREAK, /* more than one candidate matched on
                                          * SPI ID; the touch probe broke the
                                          * tie uniquely */
    PANEL_DETECT_SOURCE_FALLBACK,       /* no SPI match (or an ambiguous match
                                          * the touch probe could not resolve);
                                          * the Kconfig-configured default was
                                          * used instead */
} panel_detect_source_t;

typedef struct {
    const panel_desc_t *panel;   /* resolved panel; NEVER NULL -- always at
                                   * least `kconfig_default` */
    panel_detect_source_t source;
    bool disagreement;           /* the SPI-ID signal and the touch-probe
                                   * signal pointed at two DIFFERENT panels.
                                   * Diagnostic information, not an error --
                                   * log it loudly per Sec.6 Step 3 point 3,
                                   * do not silently prefer one side. Only
                                   * ever true when both signals actually
                                   * named a candidate; a missing signal
                                   * (touch found nothing at all) is not a
                                   * disagreement. */
    uint8_t matched_count;       /* how many candidates' id_matches(id)
                                   * returned true -- 0 with today's empty
                                   * table, always */
} panel_detect_result_t;

/* --- TODO(Sec.4 bench measurements): fills panel_spi.c's
 * ili9488_panel_desc.id_matches and st7796_panel.c's st7796_panel_desc.
 * id_matches, both currently NULL --------------------------------------
 *
 * CORRECTION (2026-09-02): this block used to say "boot the board as it is
 * today (KILNCTL_DISPLAY_PANEL=ili9488, the default) and read the WARN this
 * file's caller logs on fallback". That is wrong -- panel_spi.c's
 * ILI9488_start() only calls ILI9488_read_id() inside the
 * `#elif CONFIG_KILNCTL_DISPLAY_PANEL_AUTO` branch. The explicit-ILI9488
 * default branch (`#else`, today's shipped config) calls ILI9488_init()
 * directly and never reads RDDID at all, so nothing is ever logged on a
 * default-config boot. See firmware/KilnFW/sdkconfig.paneldetect.defaults
 * and DISPLAY_ST7796_PLAN.md Sec.4 for the corrected one-build procedure
 * that actually exercises the read (build with AUTO selected for exactly
 * one flash, capture the boot log line, then rebuild normally).
 *
 *   1. "RDDID (0x04) bytes from the ILI9488 on this wiring" -- build with
 *      the AUTO sdkconfig fragment (ILI9488 is AUTO's own bootstrap/
 *      fallback panel, so no wiring change is needed), flash, and read the
 *      "panel auto-detect: no RDDID match (read 0x.. 0x.. 0x.. ...)" WARN
 *      it logs. Record those three bytes in DISPLAY_ST7796_PLAN.md Sec.4.
 *   2. "RDDID (0x04) bytes from the ST7796 on this wiring" -- same, with the
 *      MSP4031 wired to J2 (only after the STOP-block 5V hazard check) and
 *      KILNCTL_DISPLAY_PANEL=st7796 forced explicitly (skips probing, see
 *      Sec.6 Step 3 point 1) so the panel actually inits while its ID gets
 *      read and recorded the same way.
 *
 * Once both are recorded, the one-step fill-in is: add a small static
 * matcher next to each panel_desc_t built on panel_detect_id_equals() below
 * (it already rejects all-0x00/all-0xFF, so the matcher body is just the
 * three bench bytes -- see panel_spi.c/st7796_panel.c for the exact
 * before/after) and point that descriptor's `.id_matches` at it. Nothing
 * else in this file, or its call site, needs to change: an empty table
 * already exercises the fallback path this function was built around, and a
 * populated one exercises the match path the same tests below already
 * cover with synthetic candidates -- see
 * test_panel_detect.c:test_recorded_id_matcher_pattern() for that exact
 * recipe proven against a synthetic ID ahead of the real bytes existing. */

/* Convenience equality check for id_matches implementations: true iff `id`
 * exactly equals {b0,b1,b2}. Guards the same "no panel wired" case
 * panel_spi.c's ILI9488_read_id() already treats specially -- all-0x00 or
 * all-0xFF never matches, even if a caller passes one of those triples as
 * b0..b2, because a bench-recorded RDDID that came back all-0x00/all-0xFF
 * would mean the read failed, not that this is a real ID. This is the one
 * piece of logic every id_matches implementation needs, so it lives here
 * (pure, host-testable) rather than being hand-rolled per descriptor. */
static inline bool panel_detect_id_equals(const uint8_t id[3], uint8_t b0, uint8_t b1, uint8_t b2)
{
    if ((id[0] | id[1] | id[2]) == 0x00 || (id[0] & id[1] & id[2]) == 0xFF) return false;
    return id[0] == b0 && id[1] == b1 && id[2] == b2;
}

/* Pure decision function -- no I/O, no logging, safe to call from a host
 * test with fabricated inputs.
 *
 *   id                    -- the three RDDID bytes actually read off the
 *                             panel this boot (caller's responsibility to
 *                             have read them; garbage in is fine, it just
 *                             will not match anything real).
 *   touch_ns2009_present  -- true if the I2C touch probe answered at
 *                             NS2009_ADDR_A0_LOW or NS2009_ADDR_A0_HIGH.
 *   touch_ft6336_present  -- true if it answered at the FT6336U address
 *                             (0x38). Both true is possible in principle
 *                             (two chips on one bus) and is treated as "no
 *                             usable tiebreak signal", same as both false.
 *   candidates/n_candidates -- the panel table to test id against, in
 *                             caller-chosen order. May be empty (n==0);
 *                             that degenerates to always-fallback, same as
 *                             every candidate having a NULL/never-true
 *                             id_matches.
 *   kconfig_default       -- the panel to boot when nothing matches
 *                             (KILNCTL_DISPLAY_PANEL's non-auto default,
 *                             i.e. ILI9488_get_panel_desc()). Must not be
 *                             NULL -- a kiln controller with a blank screen
 *                             is worse than one with a wrong gamma table,
 *                             so this function always has *something* safe
 *                             to hand back.
 *
 * A candidate whose id_matches is NULL is treated as "never matches" rather
 * than crashing -- exactly today's real descriptors. */
panel_detect_result_t panel_detect_choose(const uint8_t id[3], bool touch_ns2009_present,
                                           bool touch_ft6336_present,
                                           const panel_detect_candidate_t *candidates,
                                           size_t n_candidates,
                                           const panel_desc_t *kconfig_default);

#ifdef __cplusplus
}
#endif

#endif // PANEL_DETECT_H
