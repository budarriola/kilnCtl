// event_log -- fixed-size BINARY event records for the on-flash log store.
//
// Replaces telemetry_log.c's old behaviour of writing a full text FIRE/TUNE
// line to flash every 5s/10s (~136 KiB/hour, truncating any firing over
// ~2 hours against the old 1 MiB cap -- see FLASH_BUDGET.md section
// 5.2). Owner decision, verbatim: "dont log the temps to flash, log
// errors,warnings,infos that are nessary for debug. be frugal. dont do it
// in human readable form. loging of temps for debug should be done over the
// uart interface" -- the UART path already exists (telemetry_log.c's
// ESP_LOGI of the SAME formatted lines, gated by telemetry_log_set_enabled(),
// unaffected by this change).
//
// So: flash now gets ONE fixed 32-byte binary record per genuinely-necessary
// event (run start/pause/resume/done/fault, autotune start/done/aborted) --
// never a per-tick sample. At 8 segments * 32 KiB = 256 KiB per kind
// (log_store.h), that is 8192 records/kind: thousands of firings' worth of
// state transitions, where the old per-5s text scheme filled the same 256
// KiB in under 2 hours of a single firing.
//
// Fixed-size, manually serialized (not a packed struct cast over the wire):
// this project's host tests build under MSVC and its device build under
// GCC/Xtensa -- relying on struct layout/packing agreeing across both is
// exactly the kind of "worked on one compiler" trap this file avoids by
// writing/reading each field at an explicit byte offset instead.
//
// Every record starts with its own magic+version byte pair (not just once
// per stream) so a reader can (a) detect the format at all -- an old
// deployment's flash log store still holds the PREVIOUS text-line format,
// which cannot begin with this magic byte by construction (ASCII 'K' from
// old "KTEL..." lines != EVENT_LOG_RECORD_MAGIC) -- and (b) resync after a
// corrupt/short record without losing the rest of the file. See
// event_log_decode()'s doc comment for exactly what a bad magic/version
// means to a caller (PC decoder or a future firmware reader): refuse THAT
// record with a clear "unrecognized format" signal, never guess.
#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "log_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_LOG_RECORD_MAGIC   ((uint8_t)0xE7u)
#define EVENT_LOG_RECORD_VERSION ((uint8_t)1u)
#define EVENT_LOG_NOTE_LEN       ((size_t)16u)
#define EVENT_LOG_RECORD_SIZE    ((size_t)32u)

typedef enum {
    EVENT_LOG_SEV_INFO = 0,
    EVENT_LOG_SEV_WARN = 1,
    EVENT_LOG_SEV_ERROR = 2,
} event_log_severity_t;

typedef enum {
    EVENT_LOG_SRC_FIRING = 0,
    EVENT_LOG_SRC_AUTOTUNE = 1,
    EVENT_LOG_SRC_SYSTEM = 2,
} event_log_source_t;

/* Source-specific event codes. Each is only meaningful under the source it
 * is grouped with below; a decoder keys off `source` first, then `code`. */
typedef enum {
    EVENT_CODE_FIRING_STARTED = 0,
    EVENT_CODE_FIRING_PAUSED = 1,
    EVENT_CODE_FIRING_RESUMED = 2,
    EVENT_CODE_FIRING_DONE = 3,
    EVENT_CODE_FIRING_FAULTED = 4,   /* arg = fault_guard (thermal_guard_trip_t) */

    /* PID_EXPANSION_PLAN.md sec 7.1/7.4: ramp assist's sustained-lag
     * warning, reported REGARDLESS of ramp_assist_enabled (see
     * ramp_assist_cfg.h) -- the owner wants raw lag visibility during PID
     * testing even with the assist feature off. `zone` is the lagging
     * zone. STARTED's arg = actual_c*100 (centidegC, int32) at the tick
     * the lag became sustained; CLEARED's arg = total seconds the lag was
     * continuously sustained. Both carry note[0] = commanded ramp rate,
     * note[1] = achieved rate, each degC/hr rounded to the nearest whole
     * degree and offset by +128 (so the stored byte is always 2..254,
     * NEVER 0) -- event_log_emit() copies note via strncpy(), which stops
     * at the first NUL byte, so a signed value that legitimately encodes to
     * a raw 0x00 (rate == -128) would otherwise silently truncate/zero
     * everything after it; the +128 offset with a clamp to [-126,126]
     * guarantees that never happens. Coarse (whole-degree) on purpose --
     * this is a flash breadcrumb naming roughly what happened, not a
     * measurement channel (the dashboard JSON's ramp_lag_*_rate_c_per_hr
     * fields carry the full-precision live figures). */
    EVENT_CODE_FIRING_RAMP_LAG_STARTED = 5,
    EVENT_CODE_FIRING_RAMP_LAG_CLEARED = 6,

    EVENT_CODE_TUNE_STARTED = 16,
    EVENT_CODE_TUNE_DONE = 17,
    EVENT_CODE_TUNE_ABORTED = 18,
} event_log_code_t;

/* 0xFF in the `zone` field means "not zone-specific" (a global/run-level
 * event). Kept out of the enum above -- it is a sentinel, not a code. */
#define EVENT_LOG_ZONE_NONE ((uint8_t)0xFFu)

typedef struct {
    uint8_t  severity;   /* event_log_severity_t */
    uint8_t  source;     /* event_log_source_t */
    uint8_t  code;       /* event_log_code_t, meaning depends on `source` */
    uint8_t  zone;       /* zone index, or EVENT_LOG_ZONE_NONE */
    uint32_t uptime_s;   /* seconds since boot (esp_timer_get_time()/1e6) */
    int32_t  arg;        /* event-specific: fault_guard, elapsed_s, etc. */
    char     note[EVENT_LOG_NOTE_LEN]; /* short free text; NUL-padded, may
                                         * be empty; truncated safely if the
                                         * caller's note is longer. */
} event_log_event_t;

/* Pure, host-testable: serializes `ev` into exactly EVENT_LOG_RECORD_SIZE
 * bytes at `out`, little-endian multi-byte fields, stamping the current
 * magic/version. `ev->note` need not be NUL-terminated if it fills all
 * EVENT_LOG_NOTE_LEN bytes; longer input than that is truncated, never
 * overrun. */
void event_log_encode(const event_log_event_t *ev, uint8_t out[EVENT_LOG_RECORD_SIZE]);

/* Pure, host-testable: parses EVENT_LOG_RECORD_SIZE bytes at `in` into
 * `out`. Returns false (out left untouched) if the magic or version byte is
 * not one this build recognizes -- e.g. a segment left over from the old
 * text-line format, or a byte-stream that has lost sync -- so a caller can
 * refuse that record with a clear message instead of misreading garbage as
 * a valid event. `out->note` is always NUL-terminated on success. */
bool event_log_decode(const uint8_t in[EVENT_LOG_RECORD_SIZE], event_log_event_t *out);

/* Device-only: encodes `ev` and appends it to the flash-backed store for
 * `kind` via the internal-stack flash worker (log_store_write_event(),
 * log_store_mount.c) -- same dispatch telemetry_log.c's old per-tick writes
 * used, just now called only on a genuine event instead of every 5s/10s.
 * Never blocks beyond that worker round trip; degrades the same way
 * log_store_append() always has (drops the record, returns an error, never
 * asserts/hangs) on a full or failing filesystem. */
esp_err_t event_log_emit(log_store_kind_t kind, event_log_severity_t severity, event_log_source_t source,
                          event_log_code_t code, uint8_t zone, int32_t arg, const char *note);

#ifdef __cplusplus
}
#endif

#endif // EVENT_LOG_H
