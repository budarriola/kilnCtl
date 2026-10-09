#ifndef BACKUP_HTTP_INTERNAL_H
#define BACKUP_HTTP_INTERNAL_H

// Internal seams for the backup_http.c split (2026-09-04, ROADMAP.md M15's
// 1500-line item -- backup_http.c had grown to 1768 lines). This header is
// NOT public API -- backup_http.h stays that -- it exists purely so pieces
// that used to be one translation unit (and could reach each other's
// `static` state and helpers for free) can still do so now that they are
// four. Same shape as ota_http.c's 2026-09-04 split (ota_http_internal.h)
// and zones_config_json.c's split (zones_config_json_internal.h): every
// symbol declared below was `static` in the original single file and is
// widened to file-scope-internal linkage ONLY because a sibling .c file in
// this split now calls it directly. Every widened symbol is renamed with a
// `backup_`/`BACKUP_` prefix -- audited against every other file in
// App/drivers/ for both a non-static definition (immediate link error) and
// a same-named `static` elsewhere (links silently, breaks on the next
// split); see this pass's report for the audit results. Notably
// `json_escape` was NOT widened here even though the name is a real
// collision (dashboard_json.c defines a non-static `json_escape()`) --
// backup_export.c's copy stays `static` to that file, exactly as it was
// before the split, so the collision never becomes reachable.
//
// THIS IS A MOVE-ONLY REFACTOR: no logic, ordering, naming (beyond the
// widening rename above) or visibility change beyond what moving requires.
//
//   backup_http.c    -- includes, BACKUP_TAG, backup_http_start() (route
//                        registration only)
//   backup_json.c    -- generic, purpose-built JSON reader used by the
//                        import side (backup_json_skip_ws/_value,
//                        backup_json_obj_find, backup_json_arr_first/_next,
//                        backup_json_field_num/_opt_num/_str)
//   backup_export.c  -- GET /settings/backup page, GET /api/backup/export
//                        (streamed JSON writer, json_escape stays static)
//   backup_import.c  -- POST /api/backup/import (backup_import_apply(),
//                        the two-pass validate-then-commit parser, and the
//                        upload handler)

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

// Shared log tag. Defined (non-static) in backup_http.c; every split file
// logs under the same "backup_http" tag the single file used to, unchanged.
// NOT named plain `TAG` -- every other driver file in App/drivers/ has its
// own `static const char *TAG`, and a global `TAG` here would clash the
// moment two translation units of this split end up in the same link.
extern const char *BACKUP_TAG;

// Backup document format version. Bumped 4 -> 5 (task 9,
// bkfinish_assessment.md / docs/KILN_PROFILES_PLAN.md item 17 follow-up):
// version 5 adds the top-level "kiln_configs" array (every saved kiln
// config slot, each embedding its full section-5.1 package envelope when
// its Pico half is populated -- see backup_export.c's own comment on that
// array) and the "safety_tc_type" key. A v4-or-older backup has neither key
// and still imports cleanly under the v5 reader (kiln_configs[] simply
// absent = today's pre-item-17 behaviour, exactly); BACKUP_FORMAT_VERSION_MIN
// stays 1 for that reason -- only the WRITER (export) advanced, the READER
// (import) remains backward-compatible all the way down.
// 5 -> 6: profiles[].segments[] carry seg_kind/io_target/io_state/io_blocking/io_leave_on_at_end and
// profiles[] carry on_off_rules[]. A v5 reader ignores those keys, so it would restore a RELAY_IO segment as
// a ZONE_RAMP at 0 C and drop every rule; bumping makes older firmware refuse the file instead. This
// firmware still reads v5 (it carries none of the new keys, which are all optional).
#define BACKUP_FORMAT_VERSION 6
#define BACKUP_FORMAT_VERSION_MIN 1

// Generous headroom over a legitimate full backup -- see backup_http.c's own
// BACKUP_BODY_MAX comment. Only backup_import.c reads this (the export side
// streams and never buffers the whole document), but it stays here next to
// BACKUP_FORMAT_VERSION rather than moving into backup_import.c alone, since
// a reader of this header wants the whole size/version contract in one
// place.
//
// Raised 16384 -> 131072 (128 KiB) for the kiln_configs[] array (item 17
// follow-up): a full board carries up to KILN_CFG_MAX_COUNT (10) slots, each
// embedding a full package envelope up to KILN_CFG_EXPORT_JSON_MAX_LEN
// (6144) bytes, so a worst-case multi-slot backup alone is ~61 KB before
// profiles/zones/timing_profiles are even counted -- already over the old
// 16 KiB ceiling. 128 KiB gives roughly 2x headroom over that worst case
// plus everything else this document already carries, comfortably, without
// tuning to the exact byte count of today's KILN_CFG_MAX_COUNT/
// KILN_CFG_EXPORT_JSON_MAX_LEN (both of which may grow later).
//
// Why this is safe to just raise (owner-confirmed, see docs/KILN_PROFILES_
// PLAN.md item 17's restore section): backup_import.c's body buffer
// (backup_import_post_handler(), the `heap_caps_malloc((size_t)req->
// content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` call) is ALREADY a
// dynamically-sized SPIRAM-only allocation, sized to the actual request,
// never a fixed buffer -- raising this ceiling changes nothing about HOW
// that memory is allocated, only how large a request is accepted before the
// pre-flight `content_len > BACKUP_BODY_MAX` check refuses it outright (with
// a clean 400, before any allocation is attempted at all -- an oversized or
// malicious body still fails cleanly, it just fails at a higher line). This is
// explicitly NOT the httpd worker's own internal-DRAM task stack, and NOT
// zones_config_json.c's internal zones-JSON buffer -- the two buffers this
// codebase's standing rule prohibits enlarging, both of which live in
// internal DRAM where this board is genuinely tight. MALLOC_CAP_SPIRAM forces
// external-RAM-only allocation (ESP-IDF gives no internal-DRAM fallback for
// that flag), so this allocation cannot land in, or compete with, internal
// DRAM regardless of size. This board's PSRAM is plentiful and not a scarce
// resource the way internal DRAM is (see this codebase's PSRAM-stack-vs-NVS-
// write caution elsewhere, which is about STACK placement, not this kind of
// plain data buffer -- backup_import_apply()'s NVS writes still run on the
// httpd worker task, whose own stack is unchanged, internal-SRAM, and was
// never moved by this change).
#define BACKUP_BODY_MAX 131072

// --- Route handlers registered by backup_http_start() (backup_http.c) but --
// defined in one of the other split files. Each was `static` in the
// original single file; widened here purely so backup_http_start()'s
// httpd_uri_t table can name them. Not part of backup_http.h -- nothing
// outside this split calls a handler directly, httpd dispatches by URI.
esp_err_t backup_page_get_handler(httpd_req_t *req);      // backup_export.c
esp_err_t backup_export_get_handler(httpd_req_t *req);    // backup_export.c
esp_err_t backup_import_post_handler(httpd_req_t *req);   // backup_import.c

// printf format of the four tuning-quality floats (baseline/step_ambient/
// raw_rise/rise_inf) in the backup JSON. Shared so export's printing and
// import's "is the file value the same as the live one" comparison can never
// drift apart. %.9g round-trips a float32 exactly (these values are written
// into the kiln_configs blob by autotune, so a rounded export would change
// the blob on import); the comparison stays at the file's precision so older
// %.3f backups still match a live value that rounds to the same text.
#define BACKUP_TUNING_FLOAT_FMT "%.9g"
// Buffer for one such printed float: %.9g is at most ~15 chars ("-1.23456789e+38");
// 48 B is kept from the old "%.3f" sizing (FLT_MAX printed 45 B) and covers every finite float. Hardening only:
// the old 32 B buffer truncated a value like 1e30 (35 chars) but, as the leading
// digits still differ between distinct floats, that was not a false-match bug. 48 B on the
// httpd stack, two of them, is far inside the 4832 B per-handler frame cap.
#define BACKUP_TUNING_FLOAT_BUF 48

#endif // BACKUP_HTTP_INTERNAL_H
