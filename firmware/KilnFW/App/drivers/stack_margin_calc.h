/* Pure, host-testable arithmetic for the task-stack high-water-mark report
 * (TODO.md section 13's "Left open, needs bench measurement" item -- the
 * internal-DRAM investigation that found six internal-only task stacks
 * totalling ~20.5KB and was blocked from touching any of them by this
 * repo's own "a stack was sized by comment, not measurement, and a large
 * local then overflowed it" bug class). This file makes NO stack-size
 * decision by itself -- it only converts and classifies numbers that
 * stack_margin.c reads from uxTaskGetStackHighWaterMark() on-target.
 *
 * No FreeRTOS, no ESP-IDF, no I/O -- see App/test/test_stack_margin.c,
 * which #includes this header directly the same way test_dram_margin.c
 * includes dram_margin.h. */
#ifndef STACK_MARGIN_CALC_H
#define STACK_MARGIN_CALC_H

#include <stdint.h>

/* ON ESP-IDF, uxTaskGetStackHighWaterMark() ALREADY RETURNS BYTES. This is a
 * documented, deliberate deviation from vanilla FreeRTOS, stated in ESP-IDF's
 * own task.h (v6.0.2, freertos/FreeRTOS-Kernel/include/freertos/task.h:1509):
 *
 *   "@return The smallest amount of free stack space there has been in bytes
 *    (as opposed to words in the standard FreeRTOS documentation) since the
 *    task started"
 *
 * So there is NO conversion to do, and the factor is 1. An earlier revision of
 * this file multiplied by 4, reasoning from the vanilla FreeRTOS contract and
 * asserting in its own comment that ESP-IDF's port left it "unchanged". It
 * does not. The result was caught on the bench the first time real numbers
 * were read, because it produced arithmetic that cannot happen:
 *
 *   uart_owner_task      hwm=12752B of  4096B   311.3% headroom  OK
 *   uart_owner_evt_task  hwm=13440B of  4096B   328.1% headroom  OK
 *
 * A task cannot have more stack free than its stack has. Every figure was 4x
 * too large and every task classified OK -- including rules_task, which on the
 * corrected numbers has the thinnest margin of the six. An over-reporting bug
 * in a stack-safety report is the dangerous direction: it says "plenty of
 * headroom" right up until the overflow.
 *
 * Kept as a named constant rather than deleting the call, so the reasoning
 * above stays attached to the arithmetic. If this is ever ported to a
 * non-ESP-IDF FreeRTOS, THIS is the line to revisit -- and
 * stack_margin_classify()'s impossibility check below is what will catch you
 * getting it wrong again. */
#define STACK_MARGIN_WORD_BYTES ((uint32_t)1u)

static inline uint32_t stack_margin_words_to_bytes(uint32_t hwm_raw)
{
    return hwm_raw * STACK_MARGIN_WORD_BYTES;
}

/* Headroom classification against the stack size the task was actually
 * created with (the usStackDepth argument to xTaskCreate*, in BYTES --
 * already the unit every xTaskCreate* call in this codebase uses) -- never
 * against some other task's figure or an invented constant. `hwm_bytes` is
 * the SMALLEST amount of stack ever seen free since the task started (that
 * is what the high-water mark means), so this classifies the worst case
 * observed so far, not the current one.
 *
 * The 15%/30% cut points are a first-pass triage heuristic only -- ordinary
 * embedded-FreeRTOS practice, not a figure measured on this board. No real
 * high-water-mark reading exists yet for any of the tasks this module
 * reports on (see TODO.md section 13): these bands exist so a bench
 * operator gets a flagged reading to look at instead of nine bare integers,
 * NOT as a substitute for reading the actual numbers before resizing
 * anything. Do not tighten or loosen them "by feel" once real figures come
 * in -- rederive them from what the bench actually shows, the same rule
 * dram_margin.h's thresholds already follow. */
#define STACK_MARGIN_CRITICAL_PCT ((uint32_t)15u)
#define STACK_MARGIN_LOW_PCT      ((uint32_t)30u)

typedef enum {
    STACK_MARGIN_LEVEL_OK = 0,       /* >= LOW_PCT of the configured stack still unused at the worst point seen */
    STACK_MARGIN_LEVEL_LOW = 1,      /* between CRITICAL_PCT and LOW_PCT -- worth a look, not yet urgent */
    STACK_MARGIN_LEVEL_CRITICAL = 2, /* < CRITICAL_PCT -- uncomfortably close to overflow */
} stack_margin_level_t;

/* configured_stack_bytes == 0 is treated as CRITICAL rather than dividing by
 * zero or reading as OK -- a task this module has no configured size for is
 * a reporting bug, and a bug in a stack-safety report must never present as
 * "everything's fine." */
static inline stack_margin_level_t stack_margin_classify(uint32_t hwm_bytes, uint32_t configured_stack_bytes)
{
    if (configured_stack_bytes == 0u) {
        return STACK_MARGIN_LEVEL_CRITICAL;
    }
    /* An hwm ABOVE the configured size is arithmetically impossible: the
     * high-water mark is free space within that stack, so it can equal the
     * configured size (a task that has not run yet) but never exceed it.
     * Treated as CRITICAL, exactly like configured_stack_bytes == 0 above and
     * for the same reason -- a bug in a stack-safety report must never present
     * as "everything's fine".
     *
     * This check is here because its absence is what let a 4x unit error ship
     * unnoticed: the previous revision explicitly rationalised >100% as
     * legitimate ("FreeRTOS seeds the high-water mark from the full depth"),
     * so readings of 311% and 328% classified as OK instead of announcing that
     * the module's own arithmetic was broken. The rationalisation was wrong on
     * its own terms too -- seeding gives hwm == depth, never more. */
    if (hwm_bytes > configured_stack_bytes) {
        return STACK_MARGIN_LEVEL_CRITICAL;
    }
    uint32_t pct = (hwm_bytes * (uint32_t)100u) / configured_stack_bytes;
    if (pct < STACK_MARGIN_CRITICAL_PCT) {
        return STACK_MARGIN_LEVEL_CRITICAL;
    }
    if (pct < STACK_MARGIN_LOW_PCT) {
        return STACK_MARGIN_LEVEL_LOW;
    }
    return STACK_MARGIN_LEVEL_OK;
}

#endif /* STACK_MARGIN_CALC_H */
