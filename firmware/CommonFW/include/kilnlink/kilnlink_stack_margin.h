#ifndef KILNLINK_STACK_MARGIN_H
#define KILNLINK_STACK_MARGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_STACK_MARGIN = 0x2C -- docs/LINK_PROTOCOL.md sec 4.
 * Sent in reply to SAFETY_CMD_GET_STACK_MARGIN (0x2B, kilnlink_get_stack_
 * margin.h). Added KILNLINK_PROTOCOL_VERSION 12 -> 13 (kilnlink_version.h).
 *
 * Carries a SNAPSHOT of all KILNLINK_STACK_MARGIN_NUM_TASKS (9) SaftyFW
 * tasks' live `uxTaskGetStackHighWaterMark()` readings, one entry per task,
 * as last measured by the stack_margin_poller's round-robin walk (one task
 * measured per poll cycle -- see docs/audits/saftyfw_live_stack_reporting_
 * design_2026-09-11.md sec 1/3 for why this is a cached snapshot rather
 * than measured synchronously on request: measuring all nine tasks in one
 * request-handling instant would repeat the exact "many
 * uxTaskGetStackHighWaterMark() calls in a short window" shape whose
 * distortion of watchdog timing got the 2026-08-23 instrumentation removed).
 *
 * UNITS -- read this before touching either side of this frame:
 *   `high_water_words` is WORDS (uxTaskGetStackHighWaterMark()'s native
 *   RP2040/ARM Cortex-M FreeRTOS-port return unit), NOT bytes. Multiply by
 *   sizeof(StackType_t) (4 on this port) to get bytes. This is the OPPOSITE
 *   convention from KilnFW/ESP-IDF's own get_stack_margin(), whose
 *   uxTaskGetStackHighWaterMark() variant returns BYTES on that port --
 *   ESP-IDF's FreeRTOS fork changed the unit; vanilla FreeRTOS (this port)
 *   did not. Do not assume the two processors' numbers are directly
 *   comparable without converting one of them; the MCP-side renderer must
 *   label the unit explicitly rather than assume the reader already knows
 *   this.
 *
 * `stack_total_words` is each task's CONFIGURED depth (the value passed to
 * xTaskCreate(), in words -- xTaskCreate() on this port also takes WORDS,
 * unlike ESP-IDF's byte-based variant), so a consumer can compute a used
 * fraction without cross-referencing source.
 *
 * `rounds_completed` counts how many full round-robin cycles (all 9 tasks
 * measured at least once) the poller has completed since boot. 0 means at
 * least one entry below is still its startup sentinel
 * (KILNLINK_STACK_MARGIN_UNMEASURED, 0xFFFF) and has never actually been
 * sampled yet -- a consumer must check this before treating every entry as
 * live data, and must NOT report an unmeasured entry as a real number.
 *
 * FLOOR, NOT WORST CASE -- docs/audits/saftyfw_live_stack_reporting_design_
 * 2026-09-11.md sec 4 and saftyfw_bare_minimum_stack_measurement_2026-09-11.md's
 * own "Verdict": every value here is the tightest margin OBSERVED since
 * boot, on whatever code paths this specific board has actually taken. It
 * can never over-report (a high-water mark only grows, never shrinks, until
 * reboot) but it can silently under-report a deep branch that has not fired
 * yet (a dual-reflash timing window, a fault-injection path, etc). This
 * complements the static ELF-time checker
 * (check_saftyfw_task_stack_budgets.py); it does not replace it. Any
 * surfaced rendering of this frame's contents MUST carry this caveat
 * in its own text, not just in this comment.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Byte-for-byte
 * little-endian packing via kilnlink_bytes.h, fixed-size frame, same
 * conventions as kilnlink_ct_cal.c. */

#define KILNLINK_STACK_MARGIN_CMD 0x2Cu
#define KILNLINK_STACK_MARGIN_NUM_TASKS 9u
/* task_id u8(1) + high_water_words u16(2) + stack_total_words u16(2) */
#define KILNLINK_STACK_MARGIN_ENTRY_LEN 5u
#define KILNLINK_STACK_MARGIN_LEN \
    (1u + 1u + KILNLINK_STACK_MARGIN_NUM_TASKS * KILNLINK_STACK_MARGIN_ENTRY_LEN) /* 47 */

/* Sentinel for an entry the poller has never sampled yet (startup, before
 * the first full round completes). Not a plausible real high-water-mark
 * value for any task in this build (every stack here is well under 65535
 * words), so it is unambiguous on the wire. */
#define KILNLINK_STACK_MARGIN_UNMEASURED 0xFFFFu

/* Stable task ids for this frame -- NOT the same enum as watchdog_task.h's
 * watchdog_checkin_id_t (that one numbers only the watchdog's own
 * check-in bits; this one exists on the wire and must never be renumbered
 * once shipped, per the same rule watchdog_checkin_id_t's own header states
 * for itself). Order is arbitrary but fixed. */
typedef enum {
    KILNLINK_STACK_MARGIN_TASK_RELAY_OWNER = 0,
    KILNLINK_STACK_MARGIN_TASK_SAFETY_CORE,
    KILNLINK_STACK_MARGIN_TASK_DISCRETE_TASK,
    KILNLINK_STACK_MARGIN_TASK_THERMO_TASK,
    KILNLINK_STACK_MARGIN_TASK_CURRENT_TASK,
    KILNLINK_STACK_MARGIN_TASK_LINK_TASK,
    KILNLINK_STACK_MARGIN_TASK_LOG_TASK,
    KILNLINK_STACK_MARGIN_TASK_UPDATE_TASK,
    KILNLINK_STACK_MARGIN_TASK_WATCHDOG_TASK,
} kilnlink_stack_margin_task_id_t;

typedef enum {
    KILNLINK_STACK_MARGIN_OK = 0,
    KILNLINK_STACK_MARGIN_ERR_BUFFER_TOO_SMALL,
    KILNLINK_STACK_MARGIN_ERR_LENGTH_MISMATCH,
    KILNLINK_STACK_MARGIN_ERR_WRONG_CMD,
} kilnlink_stack_margin_status_t;

typedef struct {
    uint8_t  task_id;            /* kilnlink_stack_margin_task_id_t */
    uint16_t high_water_words;   /* WORDS -- see this file's header comment */
    uint16_t stack_total_words;  /* WORDS -- configured xTaskCreate() depth */
} kilnlink_stack_margin_entry_t;

typedef struct {
    uint8_t                       rounds_completed; /* saturates at 255 */
    kilnlink_stack_margin_entry_t entries[KILNLINK_STACK_MARGIN_NUM_TASKS];
} kilnlink_stack_margin_t;

/* Serializes `msg` (SAFETY_CMD_STACK_MARGIN payload, byte 0 = 0x2C included)
 * into `out`. Always exactly KILNLINK_STACK_MARGIN_LEN (47) bytes -- no
 * variable-length fields. Returns 47, or 0 on
 * KILNLINK_STACK_MARGIN_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_stack_margin_encode(const kilnlink_stack_margin_t *msg, uint8_t *out,
                                     size_t out_cap, kilnlink_stack_margin_status_t *status);

/* Parses a STACK_MARGIN payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_STACK_MARGIN_LEN -- this is
 * untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6). */
kilnlink_stack_margin_status_t kilnlink_stack_margin_decode(const uint8_t *payload, size_t len,
                                                              kilnlink_stack_margin_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_STACK_MARGIN_H */
