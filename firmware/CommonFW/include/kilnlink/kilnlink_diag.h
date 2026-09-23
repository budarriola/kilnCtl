#ifndef KILNLINK_DIAG_H
#define KILNLINK_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP telemetry, Frame B: SAFETY_CMD_DIAG = 0x08 --
 * docs/LINK_PROTOCOL.md sec 6. "Everything the 23-byte frame has no room
 * for." Additive: a KilnFW that has never heard of 0x08 ignores it, so this
 * can ship before the ESP side decodes it.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same conventions as
 * kilnlink_status.c / kilnlink_power.c: byte-for-byte little-endian packing
 * via kilnlink_bytes.h, fixed-size frame (no variable-length fields). */

#define KILNLINK_DIAG_CMD 0x08u
/* 26 -> 30, KILNLINK_PROTOCOL_VERSION 15 -> 16 (kilnlink_version.h),
 * TODO.md's "Dropped-log-frame counter surfaced from the diagnostic frame":
 * appends log_frames_dropped (u32 LE, offset 26..29) -- log_task.c's own
 * s_dropped/log_task_get_dropped(), which existed on this side already
 * (log emission has always been best-effort/non-blocking, see log_task.h's
 * own comment) but was never surfaced past this boot's own RAM. A real
 * layout break of an EXISTING fixed-size frame, same class as the 13->14
 * stack-margin growth (kilnlink_version.h) -- kilnlink_diag_decode()
 * rejects any len != KILNLINK_DIAG_LEN before reading a single field, so a
 * skewed pair fails closed with ERR_LENGTH_MISMATCH, never a misparse. */
#define KILNLINK_DIAG_LEN 30u

/* boot_reason byte (offset 10). Bits 3-5 added 2026-09-09 (RP2040
 * fatal-fault diagnosability pass): SaftyFW's watchdog_hw->scratch[5] latch
 * (firmware/SaftyFW/src/watchdog_overdue_diag_codec.h's watchdog_fatal_diag_t)
 * now distinguishes a stack overflow / malloc failure / configASSERT
 * failure from an ordinary watchdog timeout with nothing recorded -- before
 * this, all four looked identical from the ESP's side (the incident that
 * motivated this: 5b8fc53d, a configASSERT fired by a stack-overflow-
 * corrupted queue control block, presented as "the safety link's status
 * frame never decodes" and cost hours of protocol investigation before an
 * SWD session found the real cause). Purely additive -- this byte already
 * exists in the fixed 26-byte frame and only used bits 0-2, so this is NOT a
 * KILNLINK_PROTOCOL_VERSION bump, same precedent as LINK_FLAG2_CJ_VALID
 * (link_frame.h) reusing a spare bit in an already-transmitted byte. At
 * most one of KILNLINK_DIAG_BOOT_STACK_OVERFLOW/_MALLOC_FAILED/_ASSERT_FAILED
 * is ever set for a given boot -- they are mutually exclusive by
 * construction on the SaftyFW side (see watchdog_fatal_diag_t's own doc
 * comment) -- and any of them may be set alongside KILNLINK_DIAG_BOOT_WATCHDOG,
 * since the fault is exactly what caused that watchdog reset. This byte does
 * NOT localize an assert to a file/line (no room in one byte's spare bits
 * for that); the fine-grained detail lives in SaftyFW's own boot-time
 * console UART banner and in watchdog_fatal_diag_get_cached() for anyone
 * with a probe attached -- see main.c's step 3c. */
typedef enum {
    KILNLINK_DIAG_BOOT_POWERON       = 0x01u,
    KILNLINK_DIAG_BOOT_WATCHDOG      = 0x02u,
    KILNLINK_DIAG_BOOT_BROWNOUT      = 0x04u,
    KILNLINK_DIAG_BOOT_STACK_OVERFLOW = 0x08u, /* vApplicationStackOverflowHook() latched a reason last boot */
    KILNLINK_DIAG_BOOT_MALLOC_FAILED  = 0x10u, /* vApplicationMallocFailedHook() latched a reason last boot */
    KILNLINK_DIAG_BOOT_ASSERT_FAILED  = 0x20u, /* configASSERT() latched a reason last boot */
} kilnlink_diag_boot_flag_t;

/* flags byte (offset 25). */
typedef enum {
    KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN      = 0x01u,
    KILNLINK_DIAG_FLAG_CALIBRATION_MISSING   = 0x02u,
    KILNLINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT = 0x04u,
    /* bit3, added 2026-09-16: surfaces clear_trip_diag.h's reset-surviving
     * CLEAR_TRIP checkpoint (watchdog_hw->scratch[7]) -- set iff LAST boot
     * left behind a checkpoint with a valid magic tag, i.e. this boot's
     * clear_trip_diag_read() (main.c step 3b, before the register is
     * cleared) found the PREVIOUS boot mid-way through a CLEAR_TRIP drain
     * when it reset. That is a strong signal of the exact reboot this
     * module was built to catch (see clear_trip_diag.h's own header
     * comment) -- one bit, not the checkpoint's stage/reason/outcome
     * detail, which stays SWD-only via clear_trip_diag_get_cached() (no
     * room in this byte, and no consumer has asked for finer granularity
     * over the link yet). TODO.md Phase 8: previously captured but never
     * surfaced past a debugger; this is the fix. */
    KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT = 0x08u,
    /* bit4, added 2026-09-16: thermo_task.c's periodic MAX31856 tc_type
     * reconfigure retry (max31856_reconfig_retry.h) has exhausted
     * MAX31856_RECONFIG_RETRY_MAX_ATTEMPTS with the type still unverified.
     * Before this bit, that fact was visible only to a debugger attached to
     * thermo_task.c's s_reconfig_gave_up -- the WARN log line at the same
     * transition (log_task_log()) travels over the isolated link as an
     * ordinary LOG-task frame, which nothing on the ESP side decodes. This
     * is visibility only: S5 already treats the resulting sensor-invalid
     * snapshot (max31856_tc_type_verified() == false) as a trip condition
     * on its own, unaffected by this bit either way. A consumer of this bit
     * MUST NOT describe it as "bad reading" -- the underlying cause named
     * here is a configuration/verification failure (a CR1 readback
     * mismatch), not necessarily an out-of-range or noisy temperature; see
     * CLAUDE.md's "safety TC invalid is one CR1 byte" note. */
    KILNLINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP = 0x10u,
    /* bit5, added 2026-09-22: S1 (abs_max_temp_c, absolute ceiling) ships
     * disabled-by-zero (safety_guards.c: "abs_max_temp_c == 0 means not
     * commissioned -- never trip") with no distinct wire signal before this
     * -- SAFETY_MODEL.md's "guards with no defensible default ship
     * disabled, and say so in telemetry" line was unchecked for S1/S8 until
     * now. config_store_is_calibration_missing()/KILNLINK_DIAG_FLAG_
     * CALIBRATION_MISSING is a bundled "board not fully commissioned"
     * summary across many fields and does not distinguish "S1 specifically
     * is inert" from any other missing field -- see config_store.h's
     * comment on config_store_is_abs_max_temp_disabled(). Purely
     * additive, same spare-bit-in-an-already-transmitted-byte precedent as
     * bits 1-4 above -- no KILNLINK_PROTOCOL_VERSION bump. */
    KILNLINK_DIAG_FLAG_S1_ABS_MAX_TEMP_DISABLED = 0x20u,
    /* bit6, added 2026-09-22: S8 (max_rate_c_per_min, implausible rate of
     * rise) ships disabled-by-zero, same rationale and same "distinct from
     * CALIBRATION_MISSING" reasoning as bit5 above, for S8 instead of S1. */
    KILNLINK_DIAG_FLAG_S8_RATE_GUARD_DISABLED = 0x40u,
} kilnlink_diag_flag_t;

/* state byte (offset 24) -- LINK_PROTOCOL.md sec 6, Frame B: 0 init,
 * 1 grace, 2 armed, 3 warn, 4 tripped. */
typedef enum {
    KILNLINK_DIAG_STATE_INIT    = 0,
    KILNLINK_DIAG_STATE_GRACE   = 1,
    KILNLINK_DIAG_STATE_ARMED   = 2,
    KILNLINK_DIAG_STATE_WARN    = 3,
    KILNLINK_DIAG_STATE_TRIPPED = 4,
} kilnlink_diag_state_t;

/* context_age_100ms sentinel: "never received" -- same convention link_task's
 * own build uses (LINK_PROTOCOL.md sec 6, byte 11). */
#define KILNLINK_DIAG_CONTEXT_AGE_NEVER 255u

typedef enum {
    KILNLINK_DIAG_OK = 0,
    KILNLINK_DIAG_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_DIAG_LEN */
    KILNLINK_DIAG_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_DIAG_LEN (fixed-size frame) */
    KILNLINK_DIAG_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_DIAG_CMD */
} kilnlink_diag_status_t;

typedef struct {
    uint8_t  trip_reason;         /* SAFETY_TRIP_* (SaftyFW's safety_guards.h), 0 = none */
    uint16_t warn_mask;           /* one bit per guard currently warning */
    uint16_t trip_mask;           /* one bit per guard currently tripped */
    uint32_t uptime_ms;           /* Pico uptime */
    uint8_t  boot_reason;         /* kilnlink_diag_boot_flag_t bits */
    uint8_t  context_age_100ms;   /* KILNLINK_DIAG_CONTEXT_AGE_NEVER if never received */
    uint32_t context_frames_ok;
    uint32_t context_frames_bad;  /* CRC/framing/length errors */
    uint32_t tx_frames_dropped;   /* TX ring full */
    uint8_t  state;               /* kilnlink_diag_state_t */
    uint8_t  flags;               /* kilnlink_diag_flag_t bits */
    uint32_t log_frames_dropped;  /* log_task.c's own count -- LOG frames the
                                    * Pico never even attempted to enqueue/send
                                    * (queue full, or level-filtered doesn't
                                    * count -- see log_task_log()'s own doc
                                    * comment). Added KILNLINK_PROTOCOL_VERSION
                                    * 15 -> 16. */
} kilnlink_diag_t;

/* Serializes `dg` (SAFETY_CMD_DIAG payload, byte 0 = 0x08 included) into
 * `out`. Always exactly KILNLINK_DIAG_LEN (30) bytes -- this frame has no
 * variable-length fields. Returns 30, or 0 on
 * KILNLINK_DIAG_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_diag_encode(const kilnlink_diag_t *dg, uint8_t *out, size_t out_cap,
                            kilnlink_diag_status_t *status);

/* Parses a DIAG payload (as extracted from kilnlink_frame_t::payload) into
 * `out`. `len` must be exactly KILNLINK_DIAG_LEN -- this is untrusted input
 * from another processor across an isolated link (CommonFW/README.md
 * rule 6). */
kilnlink_diag_status_t kilnlink_diag_decode(const uint8_t *payload, size_t len,
                                            kilnlink_diag_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_DIAG_H */
