// firing_trace -- FD-02: durable per-firing temperature trace on the cfg LittleFS partition.
//
// /api/history.csv is a 640 x 30 s RAM ring lost on reboot. This module keeps, for the last
// FT_SLOTS firings, one sample per minute (uptime, UTC epoch, setpoint, per-zone temperature as
// int16 deci-C, per-zone duty, segment index) in "trace/ft<seq % 4>.bin".
//
// FLOW. firing_trace_note() is RAM-only (called from the control tick under s_exec.lock): it appends
// to a PSRAM buffer and never touches flash. Every FT_FLUSH_EVERY samples (and after the first sample)
// it marks a flush due; firing_trace_service() (called by the executor AFTER the lock is released)
// posts the flush to the flash worker WITHOUT blocking. firing_trace_end() (firing end/abort) flushes
// synchronously through the flash hop.
//
// FW-03 FILE FORMAT "FTR2" (flash wear): the first flush writes the file atomically (24 B header +
// one batch); every later flush APPENDS only the new samples as a batch through the io.append seam
// (8 B batch header {count u16, flags u16, crc32 u32 over count+flags+records} + records). The close
// is a batch of count 0 with FT_FLAG_CLOSED. Bytes written per firing are about 1x the file size
// instead of the old whole-file rewrite (quadratic, 2.1 MB for 28.8 KB). A torn last batch (power cut
// mid-append) fails its CRC or length and is DROPPED by the reader; a bad batch with data after it
// is real corruption and the file is rejected. If an append fails, or the batch count would pass
// FT_MAX_BATCHES, the next flush rewrites the whole file atomically as one batch. Without io.append the
// module rewrites the whole file every flush (the old behaviour). Legacy "FTR1" files are still read.
//
// FY-02/03: if the worker refuses every post (slot busy forever), no periodic flush happens and the
// loss on a power cut is the WHOLE firing so far (only the end-of-firing flush persists); the retry is
// rate-limited to once per new sample and logged once, so it costs the control tick nothing extra.
//
// WORST-CASE LOSS ON A POWER CUT: the samples not yet flushed, i.e. at most FT_FLUSH_EVERY (10) samples
// = 10 minutes (plus the one or two appended while a flush is in flight), plus a torn batch.
//
// SIZE: sample = 20 B, cap FT_MAX_SAMPLES = 1440 (24 h); file = 24 B header + batches, at most
// FT_FILE_MAX bytes per firing file, 4 slots (+ one transient temp copy during a rewrite) of the
// 2.3 MB cfg partition. A firing longer than 24 h keeps its first 24 h and sets the "truncated" flag.
//
// Integrity: header carries a CommonFW kilnlink_crc16_ccitt_false(); each batch a kiln_crc32_le().
#ifndef FIRING_TRACE_H
#define FIRING_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifndef ESP_ERR_INVALID_CRC /* real ESP-IDF defines it (0x109); the host esp_err.h stub does not */
#define ESP_ERR_INVALID_CRC 0x109
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FT_ZONES 3u
#define FT_SLOTS 4u
#define FT_MAX_SAMPLES 1440u
#define FT_SAMPLE_PERIOD_S 60u
#define FT_FLUSH_EVERY 10u
#define FT_REC_SIZE 20u
#define FT_HDR_SIZE 24u      /* file header */
#define FT_BHDR_SIZE 8u      /* FW-03 batch header */
#define FT_SAMPLES_OFF (FT_HDR_SIZE + FT_BHDR_SIZE) /* RAM buffer / opened view: samples start here */
#define FT_MAX_BATCHES 160u
#define FT_FILE_MAX (FT_SAMPLES_OFF + FT_MAX_SAMPLES * FT_REC_SIZE + (FT_MAX_BATCHES - 1u) * FT_BHDR_SIZE)
#define FT_TEMP_INVALID ((int16_t)INT16_MIN)
#define FT_DUTY_INACTIVE 255u /* duty is 0..200 in 0.5 % units */

#define FT_FLAG_TRUNCATED 0x0001u
#define FT_FLAG_CLOSED 0x0002u
/* FY-05: never stored in a file. Derived in RAM: the file says closed:false but no firing is
 * writing it any more (ESP rebooted mid-firing, or the final flush failed). */
#define FT_FLAG_INTERRUPTED 0x0004u
/* FY-04: a listed/opened firing is "in progress" iff neither CLOSED nor INTERRUPTED. */
#define FT_IN_PROGRESS(flags) (((flags) & (FT_FLAG_CLOSED | FT_FLAG_INTERRUPTED)) == 0u)

typedef struct {
    uint32_t seq; /* firing id; 0 = empty slot */
    uint32_t start_epoch;
    uint16_t n;
    uint16_t flags;
    uint32_t last_epoch; /* FY-04: UTC epoch of the last DURABLE sample (0 = unsynced/none) */
} firing_trace_entry_t;

/* IO seam. All members optional; a NULL write disables persistence (host default). */
typedef struct {
    esp_err_t (*write)(const char *rel_path, const void *data, size_t len);
    /* FW-03: append p1[n1] then p2[n2] to an existing file (non-atomic, fsynced). NULL = rewrite each flush. */
    esp_err_t (*append)(const char *rel_path, const void *p1, size_t n1, const void *p2, size_t n2);
    esp_err_t (*read)(const char *rel_path, void *buf, size_t cap, size_t *out_len);
    esp_err_t (*post)(void (*fn)(void *arg));              /* non-blocking, one outstanding */
    esp_err_t (*run)(void (*fn)(void *arg), void *arg);    /* blocking, runs fn on the flash worker */
    void *(*alloc)(size_t bytes);                          /* PSRAM buffer */
} firing_trace_io_t;

/* Installs io, allocates the PSRAM buffer, and scans the slot files to restore the in-RAM index.
 * Call once at boot after cfg_fs mounts. Safe to call again (rescans). */
void firing_trace_init(const firing_trace_io_t *io);
void firing_trace_reset_for_test(void);

/* RAM-only append. run_token identifies the run (a new token starts a new firing). epoch 0 = unsynced.
 * temp_c[i] NaN = zone inactive or invalid; duty[i] 0..1, NaN = inactive. Returns true if stored. */
bool firing_trace_note(uint32_t run_token, uint32_t uptime_s, uint32_t epoch, float setpoint_c,
                       const float temp_c[FT_ZONES], const float duty[FT_ZONES], uint8_t segment);

/* Posts a flush if one is due; non-blocking. Call outside any executor lock. */
void firing_trace_service(void);

/* Firing finished or aborted: synchronous final flush, marks the file closed. No-op if none active. */
void firing_trace_end(void);

/* The flush body (exposed so the host test can run it directly as the worker would). */
void firing_trace_flush_job(void *arg);

/* Index (newest first). Returns the number of valid entries written to out (max FT_SLOTS). */
size_t firing_trace_list(firing_trace_entry_t out[FT_SLOTS]);

/* A validated view of one firing file. samples points into the caller's buffer. */
typedef struct {
    firing_trace_entry_t entry;
    const uint8_t *samples;
} firing_trace_view_t;

/* Reads firing `id` into scratch (cap >= FT_FILE_MAX) and validates it (the batches are compacted in
 * place so samples are contiguous; a torn last batch is dropped). ESP_ERR_NOT_FOUND when the
 * slot is empty/other firing; ESP_ERR_INVALID_CRC when corrupt. */
esp_err_t firing_trace_open(uint32_t id, uint8_t *scratch, size_t cap, firing_trace_view_t *out);

/* CSV helpers: both return bytes written (excluding NUL), 0 on overflow. */
size_t firing_trace_csv_header(char *out, size_t cap);
size_t firing_trace_csv_row(const firing_trace_view_t *v, uint16_t idx, char *out, size_t cap);
#define FT_CSV_LINE_CAP 128u

#ifdef __cplusplus
}
#endif

#endif /* FIRING_TRACE_H */
