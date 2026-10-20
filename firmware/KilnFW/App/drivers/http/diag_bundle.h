// diag_bundle.h -- FD-01 support bundle: the pure framing + section runner
// behind GET /api/diag/bundle (diag_bundle_http.c).
//
// Wire format ("KDIAG1", a length-prefixed frame stream; not tar, because tar
// needs each member's size before its bytes and sections here are streamed
// from flash with unknown total size):
//
//   frame := "KDF1 " name " " decimal_len "\n" bytes[len] "\n"
//
// The first frame is "_manifest" (JSON: format, section names in order, plus
// the "pc_supplement" list of endpoints the PC tool fetches itself). One
// section may span several frames (same name, concatenate). The last frame is
// "_trailer" (JSON: per-section status and byte counts), so a truncated
// transfer is detectable (no trailer).
//
// ADDING A SECTION (FD-03 reset ring, FD-04 SNTP, ...): write one producer
// function and add ONE line to the s_sections table in diag_bundle_http.c.
//
// SECRETS: no producer reads Wi-Fi passwords, web password hashes, session
// tokens or TOTP seeds by name (test_diag_bundle.c scans the producer source
// for those identifiers). That scan is NOT a guarantee about the coredump:
// a RAM dump (CONFIG_ESP_COREDUMP_CAPTURE_DRAM) may hold the Wi-Fi PSK and
// live session data. So the coredump section is OPT-IN (GET
// /api/diag/bundle?coredump=1) and the manifest records "coredump_included"
// plus a warning when it is.
//
// STREAMING (DB-05): the whole bundle is produced inside one handler call on the single
// httpd task, so other HTTP requests wait until the transfer finishes. Accepted: the
// bundle is an on-demand support download, not a polled route.
//
// RESERVED NAMES: producers may not use "manifest", "trailer",
// "coredump.symbolize", names starting with "supplement_", "_" or "." (the
// PC tool writes those files itself). diag_bundle_run refuses such a table
// before emitting anything.
#ifndef DIAG_BUNDLE_H
#define DIAG_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*diag_sink_fn)(void *ctx, const void *data, size_t len);

typedef struct {
    diag_sink_fn sink;
    void *sink_ctx;
    uint32_t section_bytes; /* payload bytes written for the current section */
    esp_err_t sink_err;     /* first sink failure, sticky */
} diag_out_t;

/* Writes one frame. Returns ESP_ERR_INVALID_ARG for a bad name (only
 * [a-z0-9_.-], 1..31 chars), else the sink's error. */
esp_err_t diag_frame_write(diag_out_t *out, const char *name, const void *data, size_t len);

typedef esp_err_t (*diag_producer_fn)(diag_out_t *out, const char *name);

typedef struct {
    const char *name;
    diag_producer_fn produce;
} diag_section_t;

/* Emits manifest, every section (a failing producer is recorded in the
 * trailer and the run continues), then the trailer. `pc_supplement` is a
 * JSON array literal string (e.g. "[\"/api/status\"]"). Returns the sticky
 * sink error if the transport failed (the run stops there), else ESP_OK. */
esp_err_t diag_bundle_run(const diag_section_t *sections, size_t n, const char *pc_supplement,
                          diag_sink_fn sink, void *sink_ctx);

/* Same, with `manifest_extra` (raw JSON members, no braces, e.g.
 * "\"coredump_included\":false") appended to the manifest object. */
esp_err_t diag_bundle_run_x(const diag_section_t *sections, size_t n, const char *pc_supplement,
                            const char *manifest_extra, diag_sink_fn sink, void *sink_ctx);

/* Manifest members for diag_bundle_run_x: coredump_included true/false (+ warning when true). */
const char *diag_bundle_manifest_extra(bool coredump);

#ifdef __cplusplus
}
#endif

#endif
