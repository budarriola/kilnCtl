// cfg_fs_status -- observability for the `cfg` LittleFS partition
// (docs/FILESYSTEM_USER_DATA_PLAN.md). Builds the JSON body served at
// GET /api/cfgfs (diagnostics_http.c). Pure and host-testable: it calls
// ONLY cfg_fs.h's public API (cfg_fs_get_status()/is_available()/list()),
// plus a plain stat() on paths this module builds itself from a base
// directory the caller already knows -- it never edits or reaches into
// cfg_fs.c's/cfg_fs_mount.c's internals, which are foundation-module code
// owned elsewhere (docs/FILESYSTEM_USER_DATA_PLAN.md's mount-failure
// contract). On device the caller passes "/cfg" (cfg_fs_mount.c's fixed
// mount point); host tests pass whatever scratch directory their own
// cfg_fs_init() call used -- same on-disk shape either way, so this file's
// host coverage IS the code that runs on the board.
//
// Capacity (total/used bytes) and the dual-write (file vs NVS rev) picture
// are supplied by the CALLER, not read here: capacity needs
// esp_littlefs_info(), an ESP-IDF-only API, and the dual-write rev compare
// needs a read of zones config's NVS key -- both device/NVS specific and
// deliberately kept out of this pure module so it stays host-testable
// without pulling in ESP-IDF headers or the NVS stub. Pass NULL for either
// to omit that section of the JSON (reported as "unknown", not silently
// zeroed -- an unknown capacity must never render as "0 bytes free").
#ifndef CFG_FS_STATUS_H
#define CFG_FS_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "dualwrite_window.h" /* dualwrite_window_status_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool   known;       /* false => caller could not determine capacity (e.g. esp_littlefs_info() failed) */
    size_t total_bytes;
    size_t used_bytes;
} cfg_fs_capacity_info_t;

/* Dual-write rev-compare picture for ONE migrated item. 2026-09-08: widened
 * from zones-only to every persist/'*'_cfg_fs.c bridge (zones_config,
 * kiln_cfg_store, the pref_cfg_fs.c-backed items -- unit pref, ramp assist,
 * display power, TZ -- and profiles, one row per slot) -- each caller-side
 * accessor (unit_pref_get_dualwrite_status() and siblings) reads its OWN
 * bridge's actual NVS rev key (no naming convention assumed here) and
 * computes `diverged` itself via cfg_fs_status_item_diverged() below, so
 * this module stays a dumb renderer of whatever the callers hand it -- one
 * shared divergence RULE, many independent per-item DATA sources. `name` is
 * never copied -- callers must pass a string literal or other static
 * storage. */
typedef struct {
    const char *name;
    bool        file_valid;
    uint32_t    file_rev;
    bool        nvs_valid;
    uint32_t    nvs_rev;
    bool        diverged;
    /* 2026-09-08 (hardware verification 3e226f28): true iff this item's
     * boot-time migrate-on-load write was attempted before the flash-safe
     * worker existed and the bounded wait (flash_worker_wait.h) gave up --
     * i.e. the write was very likely dropped ("flash-safe worker not
     * started -- job dropped") and the item may still be NVS-only despite
     * having a bridge. Defaults false for every item that does not track
     * this (only relay_cycles/adaptive_tune report it as of this pass). */
    bool        migration_deferred;
} cfg_fs_dualwrite_item_t;

/* Upper bound on how many dual-write item rows one /api/cfgfs response
 * carries -- 1 (zones) + 1 (kiln_cfg_store) + 4 (pref-backed: unit pref,
 * ramp assist, display power, TZ) + PROFILES_MAX_COUNT (8) + 3 (2026-09-08:
 * relay_cycles, adaptive_tune ki-baseline, firing_stats -- the last three
 * items docs/FILESYSTEM_USER_DATA_PLAN.md section 5 tracked, all landed in
 * 762bb29e) = 17 today, with headroom for one more bridge before this needs
 * to grow again. Extra items past this cap are silently dropped by the
 * JSON builder rather than overflowing -- see cfg_fs_status_build_json()'s
 * own comment on why that is the right failure mode here (unlike
 * CFG_FS_STATUS_MAX_FILES, this list is built by firmware code, not by
 * whatever a user has dropped on the filesystem). */
#define CFG_FS_STATUS_MAX_ITEMS 18

/* THE single definition of "this item's file and NVS copies disagree",
 * shared by every caller so a future bridge cannot invent a second one.
 * Matches exactly what every *_cfg_fs.c bridge's resolve() function already
 * branches on (see e.g. zones_config_cfg_fs.c's `differs` local): both
 * sides must be valid, AND their decoded content must differ. A rev
 * mismatch alone is NOT sufficient -- dual-write always bumps both sides
 * together then writes the file first, so file_rev transiently ahead of
 * nvs_rev is the ordinary in-flight case, not a divergence -- and an EQUAL
 * rev with differing content is exactly the dangerous case
 * check_cfg_fs_tie_break.ps1 pins, which a rev-only check would miss
 * entirely. Callers must therefore pass the real result of comparing
 * decoded file vs. NVS bytes, never infer it from revs. */
bool cfg_fs_status_item_diverged(bool file_valid, bool nvs_valid, bool content_equal);

/* Boot-hang-2026-09-08 follow-up (docs/audits/boot_hang_2026-09-08.md, "A
 * bounded-time format... would be the more robust fix"): the auto-format
 * that used to run inline on the boot task now runs on a deferred background
 * task (cfg_fs_mount.c), so a slow format is never confused with a hang --
 * this struct is how the caller (diagnostics_http.c, reading cfg_fs_mount.c's
 * getters, which need ESP-IDF and so cannot live in this pure module) hands
 * that picture to cfg_fs_status_build_json() for GET /api/cfgfs. `known`
 * false means no auto/deferred format has run at all this boot (the common
 * case -- the partition mounted cleanly). */
typedef struct {
    bool      known;
    bool      in_progress;
    bool      completed;
    bool      succeeded;   /* meaningful only when completed */
    uint32_t  elapsed_ms;  /* time so far while in_progress, final duration once completed, 0 otherwise */
    esp_err_t result;      /* meaningful only when completed && !succeeded -- the actual esp_err_t (e.g.
                             * from esp_littlefs_format(), the VFS re-register, or the dispatch onto the
                             * flash worker) so a failure is one GET /api/cfgfs away from a name, not
                             * another investigation. ESP_OK here with succeeded==false cannot happen by
                             * construction (succeeded is derived from this same value at the call site). */
} cfg_fs_format_progress_t;

/* Ceiling used to flag a running format as STALLED rather than merely slow.
 * Arithmetic (see cfg_fs_status.c's file banner for the full derivation):
 * the `cfg` partition is 512 KiB = 128 4-KiB sectors; typical SPI-NOR sector
 * erase is ~45 ms, worst-case (datasheet max) ~400 ms, so a full-partition
 * format costs ~5.8 s typical / ~51.2 s worst-case, plus small LittleFS
 * superblock overhead. This ceiling (120 s) sits comfortably above 2x the
 * worst-case estimate so a legitimately slow format on tired flash is never
 * misreported as stuck, while a format that is still running after this long
 * is worth a loud flag. It does NOT abort anything -- LittleFS/esp_partition
 * offer no cooperative abort point mid-erase -- it only changes what
 * GET /api/cfgfs reports. */
#define CFG_FS_FORMAT_CEILING_MS 120000u

/* Pure predicate, unit-testable without any real elapsed time: true iff a
 * format that has been running for `elapsed_ms` should be reported STALLED. */
bool cfg_fs_format_is_stalled(bool in_progress, uint32_t elapsed_ms);

/* Builds the full /api/cfgfs JSON body into `buf` (capacity `buf_cap`).
 * `base_dir_for_sizes` may be NULL to omit per-file sizes (e.g. cfg_fs is
 * not mounted, so there is nothing to stat). `cap`/`fmt` may be NULL to omit
 * their sections (reported as unknown/absent, not defaulted). `items`/
 * `item_count` may be NULL/0 to omit the dual-write item list entirely
 * (rendered as an empty array, not omitted -- same "absent is not the same
 * as zero" discipline as everything else in this module); item_count above
 * CFG_FS_STATUS_MAX_ITEMS is clamped, not rejected.
 *
 * Returns ESP_OK and sets *out_len to the JSON length on success.
 * ESP_ERR_INVALID_ARG for NULL buf/out_len or buf_cap == 0.
 * ESP_ERR_INVALID_SIZE if the JSON did not fit `buf_cap` -- *out_len is
 * unset in that case, exactly like cfg_fs_read()'s own truncation-detection
 * convention, so a caller can grow its buffer and retry rather than ship a
 * silently-truncated response. */
esp_err_t cfg_fs_status_build_json(const char *base_dir_for_sizes, const cfg_fs_capacity_info_t *cap,
                                    const cfg_fs_dualwrite_item_t *items, size_t item_count,
                                    const cfg_fs_format_progress_t *fmt, char *buf, size_t buf_cap,
                                    size_t *out_len);

/* Same as cfg_fs_status_build_json(), plus the dual-write window's progress
 * (dualwrite_window.h's read-only dualwrite_window_get_status() result, filled
 * in by the caller -- this module stays pure and never touches NVS) rendered as
 * a top-level "dual_write_window" object between "dual_write" and "format":
 * {"known":true,"consecutive_clean_boots":N,"clean_boots_target":N,
 *  "firing_complete":b,"restore_verified":b,"window_may_close":b}.
 * `win` NULL renders {"known":false} -- unknown, never defaulted to zeros.
 * window_may_close is REPORT ONLY (dualwrite_window.h); nothing acts on it.
 * cfg_fs_status_build_json() is this function with win == NULL. */
esp_err_t cfg_fs_status_build_json_ex(const char *base_dir_for_sizes, const cfg_fs_capacity_info_t *cap,
                                       const cfg_fs_dualwrite_item_t *items, size_t item_count,
                                       const cfg_fs_format_progress_t *fmt, const dualwrite_window_status_t *win,
                                       char *buf, size_t buf_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif // CFG_FS_STATUS_H
