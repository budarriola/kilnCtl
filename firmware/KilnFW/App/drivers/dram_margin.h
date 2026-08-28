/* Internal-DRAM low-water alarm, checked at every main.c heap_stage() call.
 *
 * WHY these two thresholds, specifically: they are not a guess and not "the
 * size of the next allocation" (there isn't one to point to here -- LVGL's
 * 8192-byte task stack, the historical reason for an 8192 threshold, moved to
 * a static .bss array on 2026-08-21 and no longer competes for a dynamic
 * block at all; see lvgl_port.c's lvgl_port_start() comment). They are the
 * exact figures the board reported, live, the one time this project's own
 * memory of a real failure was captured with numbers attached:
 *
 *   2026-08-22, /api/status at 8000s uptime: heap_internal free=11903,
 *   largest_free_block=8704. A browser loading /app.js got
 *   ERR_CONNECTION_RESET, the truncated file failed to parse, and every page
 *   depending on it sat on "Loading..." forever. Root cause: wifi_provision_
 *   http.c's max_open_sockets=13 with an 8192-byte httpd stack, more lwIP
 *   sockets than internal SRAM could back at that fragmentation level.
 *
 * That is a STEADY-STATE figure (8000s of uptime, not a boot-time reading),
 * so it is not directly comparable to heap_stage()'s boot-time trace without
 * that caveat -- see the KilnFW DRAM investigation note in docs/PROJECT_
 * STATUS.md for the full discussion. What it is unambiguously good for is a
 * name for "the smallest largest-free-block value at which this exact
 * failure mode has already happened, once, for real" -- which is what an
 * early-warning alarm needs, not a theoretical safety margin invented for
 * the occasion.
 *
 * Pure and host-testable: see App/test/test_dram_margin.c. */
#ifndef DRAM_MARGIN_H
#define DRAM_MARGIN_H

#include <stdbool.h>
#include <stddef.h>

#define KILN_DRAM_LARGEST_ALARM_BYTES ((size_t)8704)
#define KILN_DRAM_FREE_ALARM_BYTES    ((size_t)11903)

/* The worst figures the CURRENT firmware actually reaches on the bench.
 *
 * Measured 2026-08-24 with this alarm live, across EVERY heap_stage() call
 * rather than the one stage that had been quoted before: the trough is not
 * `uart_bridges_1` (largest=7680, dram_free=12383) but `app_main_done`
 * (largest=7680, dram_free=11415) -- `lvgl_start` and `uart_bridges_2` sit
 * between them at 12039 and 11519. Earlier notes on this problem quoted the
 * uart_bridges_1 figure as the trough and reasoned from it; that was simply
 * the last stage anyone had looked at, and it understates the real minimum by
 * about 1 kB.
 *
 * That correction matters: the trough is BELOW the 11903 documented failure
 * figure above, so the end of boot is already inside the zone where this exact
 * failure has happened once -- not comfortably above it.
 *
 * UPDATED 2026-08-24, same day, from 11415 to 10015: rules_task's stack went
 * 3072 -> 4096 after the first real high-water-mark measurement showed it at
 * 10.9% headroom (rules_task.c has the full reasoning). That deliberately
 * spent 1400 bytes of this trough to stop the rule evaluator running 336 bytes
 * from a kernel-corrupting overflow. The alarm below did its job and announced
 * it with numbers:
 *
 *   E app_main: heap stage app_main_done DRAM REGRESSION: largest=7680
 *     (was 7680) dram_free=10015 (was 11415, WORSE)
 *
 * The baseline is updated here, in the SAME change, because the trough
 * legitimately moved and a regression detector pinned to a stale figure can no
 * longer detect the NEXT one. This is the documented procedure, not a way to
 * silence the alarm -- the distinction is that the cause is known, measured,
 * and written down. Re-verified at the new trough that the failure this alarm
 * is named after still does not reproduce: /app.js delivered 23737 bytes
 * complete and byte-identical on 4 consecutive fetches, and /, /status and
 * /api/safety/commissioning all answered 200.
 *
 * Recorded because `largest` is ALREADY below KILN_DRAM_LARGEST_ALARM_BYTES,
 * so the alarm is a STANDING condition on every boot, not an event.
 *
 * That distinction is the whole reason these exist. An alarm that fires on
 * every single boot is indistinguishable from a broken alarm: everyone
 * learns to scroll past it, and the first genuine regression then arrives
 * inside a line that has been ignored for months. So the check reports two
 * different things -- "you are in the documented failure zone", which is
 * true today and expected, and "you are worse than this firmware has ever
 * been", which is news and is the line worth acting on.
 *
 * KEEP THESE CURRENT. If a change legitimately improves the trough, move
 * them to the new measured figures in the SAME commit -- otherwise the
 * regression check silently stops being able to detect anything, which is
 * the same vacuous-check failure this repo has already shipped three times.
 * If a change makes them worse, that is precisely what the alarm is for:
 * do not move these to silence it.
 *
 * (This paragraph used to say "lower them" for an improvement. That was
 * backwards: these are low-water marks, so a trough that improves means the
 * numbers go UP. Corrected 2026-08-27 while doing exactly that.)
 *
 * 2026-08-27: 7680/10015 -> 13824/22123. Measured at app_main_done, which is
 * the real minimum -- uart_bridges_1 reads ~1 kB higher (23111) and is the
 * figure that gets quoted by mistake, including by the first draft of this
 * very change. The floor check below caught that: raising it temporarily to
 * prove it fires reported lvgl_start, uart_bridges_2 and app_main_done as
 * lower still, which is how the wrong baseline surfaced. The gain is four task stacks moved off
 * internal DRAM -- both uart_protocol RX tasks (4 kB each) -- plus the
 * uart_owner quartet resized 4096 -> 3072 on measurement. Note that `largest`
 * is now ABOVE KILN_DRAM_LARGEST_ALARM_BYTES for the first time, so the
 * standing alarm below stops being a standing condition; if it ever fires
 * again it is an event, which is what it was always meant to be. */
#define KILN_DRAM_LARGEST_KNOWN_BYTES ((size_t)13824)
#define KILN_DRAM_FREE_KNOWN_BYTES    ((size_t)22123)

/* Boot-to-boot slack. The figures above are single-boot measurements, and the
 * late stages depend on Wi-Fi association and DHCP timing, so a few hundred
 * bytes of variation between two boots of the SAME binary is expected.
 * Without slack the regression line would flap, which destroys its value just
 * as surely as firing on every boot does. 512 bytes is chosen to be larger
 * than observed jitter and far smaller than any real growth worth catching --
 * the 44-byte max_uri_handlers bump is the smallest deliberate change
 * measured so far, and a genuine leak or a new task stack is kilobytes. If
 * the regression line starts flapping anyway, MEASURE the spread across
 * several boots and widen this with the numbers recorded -- do not widen it
 * by feel. */
#define KILN_DRAM_REGRESSION_SLACK_BYTES ((size_t)512)

/* The owner's requested operating floor for free internal DRAM (2026-08-27:
 * "i would also like to see us maintain about 20k of free dram").
 *
 * Deliberately a SEPARATE figure from KILN_DRAM_FREE_ALARM_BYTES above. That
 * one is evidence: the measured free size at which HTTP sockets actually
 * reset and /app.js actually arrived truncated. This one is policy: the
 * margin we have chosen to keep above that evidence. Conflating them would
 * lose the distinction between "we are in the zone where the failure has been
 * observed" and "we have eaten into the buffer we said we would keep", and
 * the second is supposed to be the early warning for the first.
 *
 * 20480, against a measured trough of 23111 -- about 2.6 kB of room. If a
 * future change pushes below this the boot log says so while there is still
 * evidence-backed headroom left to spend. */
#define KILN_DRAM_FREE_FLOOR_BYTES ((size_t)20480)

typedef struct {
    bool tripped;      /* either alarm condition below is true */
    bool largest_low;  /* largest contiguous internal 8-bit block < alarm */
    bool free_low;     /* total free internal 8-bit heap < alarm */
    bool regressed;    /* worse than the known-current trough -- the NEW news */
    bool largest_regressed;
    bool free_regressed;
    bool below_floor;  /* below the owner's 20 kB operating floor (policy, not evidence) */
} dram_margin_result_t;

/* Pure decision function -- no I/O, no ESP-IDF calls. Takes the same two
 * figures main.c's heap_stage() already computes via heap_caps_get_largest_
 * free_block()/heap_caps_get_free_size() and decides whether either has
 * reached the documented failure zone above. */
static inline dram_margin_result_t dram_margin_check(size_t largest_free_bytes, size_t total_free_bytes)
{
    dram_margin_result_t r;
    r.largest_low = largest_free_bytes < KILN_DRAM_LARGEST_ALARM_BYTES;
    r.free_low = total_free_bytes < KILN_DRAM_FREE_ALARM_BYTES;
    r.tripped = r.largest_low || r.free_low;
    /* Strictly below the known trough minus slack: equalling the trough, or
     * missing it by less than boot-to-boot jitter, is this firmware behaving
     * as measured. Only going meaningfully past it is news. */
    r.largest_regressed =
        largest_free_bytes + KILN_DRAM_REGRESSION_SLACK_BYTES < KILN_DRAM_LARGEST_KNOWN_BYTES;
    r.free_regressed =
        total_free_bytes + KILN_DRAM_REGRESSION_SLACK_BYTES < KILN_DRAM_FREE_KNOWN_BYTES;
    r.regressed = r.largest_regressed || r.free_regressed;
    /* No slack on the floor: it is a round number we chose with room to
     * spare, not a measurement with jitter to absorb. */
    r.below_floor = total_free_bytes < KILN_DRAM_FREE_FLOOR_BYTES;
    return r;
}

#endif /* DRAM_MARGIN_H */
