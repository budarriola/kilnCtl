// log_store -- bounded, rotating on-flash storage for BINARY event records.
//
// Fills the gap the 2026-09-01 audit found: user config (kiln_nvs/wifi_nvs/
// profiles_nvs) and firing statistics both already live in NVS, but LOGS had
// no persistent store at all -- only telemetry_log.c's live debug-UART feed,
// which nothing captures unless a PC happens to be attached and listening.
//
// 2026-09-02: this store used to hold one text FIRE/TUNE line (telemetry_
// format.c) every 5s/10s -- ~136 KiB/hour, truncating any firing over ~2h
// against the (then) 1 MiB cap. Owner decision (FLASH_BUDGET_PLAN.md section
// 5.2 follow-on): flash keeps errors/warnings/info EVENTS only, never
// per-tick samples, and never as human-readable text -- see event_log.h for
// the fixed 32-byte binary record format and event_log_emit() for the
// producer side. log_store.c itself does not know or care what the bytes
// mean; it is a generic length-prefixed binary blob store, kept that way
// (rather than hardcoding EVENT_LOG_RECORD_SIZE in here) so this file's own
// rotation/degrade logic stays exactly as host-tested as before.
//
// WORDING NOTE: this board is an ESP32-S3 N16R8 -- ONE on-module 16 MB SPI
// flash, no separate flash chip, no SD card. Every "external flash" in this
// module's comments and commit history means that on-module SPI flash,
// external only to the SoC die itself -- never a second physical device.
//
// SPLIT, same reasoning as telemetry_format.c/telemetry_log.c: this file
// (log_store.c) is pure portable C -- stdio (fopen/fread/fwrite/remove/
// rename) plus stdint/string only, no ESP-IDF, no FreeRTOS -- so it host-
// tests directly against a real temp directory on disk (see
// test/test_log_store.c) with NO filesystem stub needed at all: SPIFFS is
// mounted at a VFS path on-device, and standard C stdio already works
// transparently against any mounted VFS path, so the same fopen()-based code
// this file uses on a host temp dir is EXACTLY what runs on-device against
// "/logs". The device-only glue -- mounting the partition, and routing
// writes through uart_bridge_ext_run_on_flash_worker() so a PSRAM-stacked
// caller never touches flash directly -- lives in log_store_mount.c/.h,
// which this file's tests never link.
//
// ROTATION POLICY (the "never fills up, never wedges" requirement):
//   - Each kind (firing / autotune) writes into its own numbered sequence of
//     segment files, "<kind>_%010u.log", monotonically increasing index.
//   - A segment is closed and a new one opened once appending the next line
//     would push it past LOG_STORE_SEGMENT_MAX_BYTES.
//   - After opening a new segment, if the kind's total on-disk size exceeds
//     LOG_STORE_MAX_TOTAL_BYTES, the OLDEST segment(s) (lowest index) are
//     deleted until it's back under the cap. This is the bound: the cap is
//     checked and enforced on every rotation, never merely hoped for.
//   - HARD CAP per kind: LOG_STORE_MAX_TOTAL_BYTES = 256 KiB (8 segments *
//     32 KiB, reduced from 1 MiB/32 segments 2026-09-02 -- see
//     FLASH_BUDGET_PLAN.md section 5.2). Two kinds (firing, autotune) => at
//     most 512 KiB total, well under the 768 KiB `logs` partition, leaving
//     headroom for SPIFFS' own metadata overhead (SPIFFS typically wants
//     slack below 100% full to avoid GC thrashing -- 512 KiB used of 768 KiB
//     stays at ~67%, matching this table's original ~2/3-fill ratio).
//   - DEGRADE, DON'T WEDGE: every fopen/fwrite/remove/rename call in
//     log_store.c is checked; any failure (full filesystem, corrupt
//     directory entry, anything) causes log_store_append() to return an
//     esp_err_t and DROP that one line -- it never retries, never blocks,
//     never asserts. The caller (telemetry_log.c) already treats ESP_LOGI
//     as fire-and-forget; log_store writes are handled the same way, and a
//     full/failing filesystem degrades to "no more logs persisted this
//     boot", not a hang or crash. See test_log_store.c's
//     "full/failing filesystem does not wedge" coverage.
#ifndef LOG_STORE_H
#define LOG_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-segment cap. 32 KiB keeps a single segment file small (fast to open/
 * scan/delete) while still holding hundreds of ~190-byte FIRE lines or
 * ~55-230-byte TUNE lines (telemetry_format.h) per file. */
#define LOG_STORE_SEGMENT_MAX_BYTES ((size_t)(32u * 1024u))

/* Segments retained per kind before the oldest is deleted. 8 * 32 KiB =
 * 256 KiB per kind -- see this header's rotation-policy comment above for the
 * full arithmetic against the 768 KiB `logs` partition.
 *
 * REDUCED from 32 (1 MiB/kind) to 8 (256 KiB/kind) 2026-09-02, owner
 * decision recorded in FLASH_BUDGET_PLAN.md section 5.2: "1Mb seems
 * excessive for logs... maybe 256k?". Applied per-kind (this constant),
 * not as a combined total, to keep firing/autotune rotation independent as
 * before -- worst-case COMBINED retention is therefore 512 KiB (two kinds),
 * not 256 KiB; see partitions.csv's `logs` entry for the sizing math against
 * that combined figure and for the measured logging-rate finding (256 KiB
 * per kind is a tight fit for a single long kiln firing at the ~136 KiB/hour
 * FIRE-line rate telemetry_log.c documents -- roughly 1.9 hours of segments
 * before the oldest rotate out). */
#define LOG_STORE_MAX_SEGMENTS ((size_t)8u)

/* Hard per-kind cap enforced on every rotation -- the number this module's
 * "never fills up" guarantee is measured against. */
#define LOG_STORE_MAX_TOTAL_BYTES (LOG_STORE_SEGMENT_MAX_BYTES * LOG_STORE_MAX_SEGMENTS)

/* Ceiling on a single record's payload length, stored as a uint16_t length
 * prefix ahead of the raw bytes (see log_store.c). event_log.h's records
 * are a fixed EVENT_LOG_RECORD_SIZE (32) bytes; 64 leaves headroom for a
 * future field addition without immediately needing to touch this cap.
 * Reader callers must supply a buffer at least this large. */
#define LOG_STORE_MAX_RECORD_LEN ((size_t)64u)

typedef enum {
    LOG_STORE_KIND_FIRING = 0,
    LOG_STORE_KIND_AUTOTUNE = 1,
    LOG_STORE_KIND_COUNT = 2,
} log_store_kind_t;

/* Sets the directory every subsequent call reads/writes under (created if
 * missing). On-device this is "/logs" (the SPIFFS mount point,
 * log_store_mount.c); host tests pass a real temp directory. Safe to call
 * again to repoint the store (used by tests); NOT thread-safe against
 * concurrent log_store_append() from another task -- callers serialize
 * through the flash worker (log_store_mount.c) or, on host, call this from
 * a single test thread before any append. */
esp_err_t log_store_init(const char *base_dir);

bool log_store_is_init(void);

/* Appends one binary record (`len` bytes at `data`, NOT expected to be a
 * NUL-terminated C string -- may contain any byte value including 0x00 and
 * 0x0A) to the given kind's current segment, rotating and trimming to the
 * cap as described in this header's rotation-policy comment. Stored as a
 * uint16_t length prefix followed by the raw bytes, so no delimiter byte is
 * ever reserved out of the payload. NEVER blocks beyond a normal buffered
 * file write, and never asserts/aborts on I/O failure -- see "DEGRADE,
 * DON'T WEDGE" above. Returns ESP_ERR_INVALID_STATE if log_store_init() has
 * not been called, ESP_ERR_INVALID_ARG for a NULL/zero-length/over-length
 * record, ESP_FAIL for an I/O failure (record dropped), ESP_OK on a
 * successful append. */
esp_err_t log_store_append(log_store_kind_t kind, const void *data, size_t len);

/* Sum, in bytes, of every retained segment file's size for `kind`. Used by
 * tests to verify the rotation cap, and by log_http.c to size its response. */
size_t log_store_total_bytes(log_store_kind_t kind);

/* How many segment files are currently retained for `kind`. */
size_t log_store_segment_count(log_store_kind_t kind);

/* Oldest-to-newest, line-by-line reader over every retained segment for one
 * kind. Opening never fails even if there is nothing retained yet -- the
 * first _next() call simply returns false. */
typedef struct log_store_reader log_store_reader_t;

log_store_reader_t *log_store_reader_open(log_store_kind_t kind);

/* Fills `out` with the next record's raw bytes (`cap` must be >=
 * LOG_STORE_MAX_RECORD_LEN for a record to never be silently truncated --
 * a too-small cap truncates safely, never overruns) and, if `out_len` is
 * non-NULL, stores the record's true byte length there (which may exceed
 * `cap` if truncated). Returns true, or returns false at end-of-stream /
 * on error. */
bool log_store_reader_next(log_store_reader_t *rd, void *out, size_t cap, size_t *out_len);

void log_store_reader_close(log_store_reader_t *rd);

#ifdef __cplusplus
}
#endif

#endif // LOG_STORE_H
