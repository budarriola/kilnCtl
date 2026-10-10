// See cfg_fs_status.h for the design/rationale.
#include "cfg_fs_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#define CFG_FS_STATUS_STAT struct _stat
#define CFG_FS_STATUS_STAT_FN _stat
#else
#include <sys/stat.h>
#define CFG_FS_STATUS_STAT struct stat
#define CFG_FS_STATUS_STAT_FN stat
#endif

#include "cfg_fs.h"
#include "esp_log.h"
#include "persist_scratch.h"

#define CFG_FS_STATUS_MAX_FILES 32
#define CFG_FS_STATUS_PATH_MAX 600

/* One-level subdirectories cfg_fs holds files in (cfg_fs.h supports at most
 * one '/' of nesting). Only profiles/ exists today (profiles_cfg_fs.h's
 * PROFILES_CFG_FS_PATH_FMT, profiles_builtin.c's hidden.json). */
static const char *const CFG_FS_STATUS_SUBDIRS[] = { "profiles" };

/* -1 means "size unknown" -- distinguished from a real 0-byte file.
 * `sub_dir` is NULL for a root file, else the one-level subdirectory the
 * file lives in; the join happens here, in the one path buffer this
 * function already needs, so callers never hold a second path buffer on the
 * httpd stack. */
static long file_size_or_unknown(const char *base_dir, const char *sub_dir, const char *name)
{
    if (!base_dir) {
        return -1;
    }
    char path[CFG_FS_STATUS_PATH_MAX];
    int n = sub_dir ? snprintf(path, sizeof(path), "%s/%s/%s", base_dir, sub_dir, name)
                    : snprintf(path, sizeof(path), "%s/%s", base_dir, name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }
    CFG_FS_STATUS_STAT st;
    if (CFG_FS_STATUS_STAT_FN(path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

static const char *status_str(cfg_fs_status_t s)
{
    switch (s) {
    case CFG_FS_STATUS_MOUNTED:
        return "mounted";
    case CFG_FS_STATUS_UNAVAILABLE:
        return "unavailable";
    case CFG_FS_STATUS_UNMOUNTED:
    default:
        return "unmounted";
    }
}

/* Best-effort human explanation for why cfg_fs is not usable right now --
 * cfg_fs.h's status enum does not itself carry a reason string (see that
 * header: UNMOUNTED covers both "never called" and "recovery mode skipped
 * it", UNAVAILABLE covers both "partition absent" and "mount failed"), so
 * this is deliberately the coarsest true statement rather than a guess at
 * detail the API does not expose. */
static const char *status_reason(cfg_fs_status_t s)
{
    switch (s) {
    case CFG_FS_STATUS_MOUNTED:
        return "";
    case CFG_FS_STATUS_UNAVAILABLE:
        return "mount was attempted and failed -- partition absent, not yet added to the partition table, or "
               "corrupt; every read/write falls back to firmware defaults";
    case CFG_FS_STATUS_UNMOUNTED:
    default:
        return "not mounted this boot -- either recovery mode skipped the mount, or boot has not reached it yet";
    }
}

bool cfg_fs_format_is_stalled(bool in_progress, uint32_t elapsed_ms)
{
    return in_progress && elapsed_ms > CFG_FS_FORMAT_CEILING_MS;
}

bool cfg_fs_status_item_diverged(bool file_valid, bool nvs_valid, bool content_equal)
{
    return file_valid && nvs_valid && !content_equal;
}

bool cfg_fs_status_item_nvs_stale(bool raw_diverged, uint32_t file_rev, uint32_t nvs_rev)
{
    return raw_diverged && file_rev > nvs_rev;
}

esp_err_t cfg_fs_status_build_json(const char *base_dir_for_sizes, const cfg_fs_capacity_info_t *cap,
                                    const cfg_fs_dualwrite_item_t *items, size_t item_count,
                                    const cfg_fs_format_progress_t *fmt, char *buf, size_t buf_cap,
                                    size_t *out_len)
{
    return cfg_fs_status_build_json_ex(base_dir_for_sizes, cap, items, item_count, fmt, NULL, buf, buf_cap, out_len);
}

esp_err_t cfg_fs_status_build_json_ex(const char *base_dir_for_sizes, const cfg_fs_capacity_info_t *cap,
                                       const cfg_fs_dualwrite_item_t *items, size_t item_count,
                                       const cfg_fs_format_progress_t *fmt, const dualwrite_window_status_t *win,
                                       char *buf, size_t buf_cap, size_t *out_len)
{
    if (!buf || !out_len || buf_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cfg_fs_status_t st = cfg_fs_get_status();
    bool mounted = cfg_fs_is_available();

    /* Two 32-entry cfg_fs_entry_t arrays (48 bytes each => 1536 B apiece,
     * ~3 KB together) used to be stack locals in this function -- on top of
     * the caller's own ~3.7 KB frame (diagnostics_http.c's
     * cfgfs_status_get_handler()), on the httpd task whose MEASURED
     * worst-case margin is 64 B (CLAUDE.md's "httpd stack" note;
     * docs/audits/filesystem_migration_review_2026-09-07.md finding #1).
     * Heap-allocated instead so this function's frame stays small
     * regardless of CFG_FS_STATUS_MAX_FILES; every path below frees both
     * before returning (see the `cleanup` label). PSRAM first via
     * persist_scratch_alloc() (2026-10-04: plain malloc <= 8 KB is internal RAM). */
    cfg_fs_entry_t *files = persist_scratch_alloc(sizeof(cfg_fs_entry_t) * CFG_FS_STATUS_MAX_FILES);
    cfg_fs_entry_t *tmp_files = persist_scratch_alloc(sizeof(cfg_fs_entry_t) * CFG_FS_STATUS_MAX_FILES);
    if (!files || !tmp_files) {
        free(files);
        free(tmp_files);
        return ESP_ERR_NO_MEM;
    }

    size_t file_count = 0;
    if (mounted) {
        cfg_fs_list("", files, CFG_FS_STATUS_MAX_FILES, &file_count);
    }

    /* Files sitting in .tmp/ RIGHT NOW. cfg_fs_init()'s sweep already ran
     * (once, at mount) and cleared any crash residue from a PRIOR boot --
     * this call re-lists .tmp/ live, so on a healthy board moments after
     * boot it reads 0. A nonzero count here either means a write is
     * mid-flight (sub-second, benign) or a write attempt is stuck/failing
     * repeatedly (worth investigating) -- this endpoint cannot tell those
     * apart from a single snapshot, which is why the field is named for
     * what it literally measures rather than claimed to be the historical
     * mount-time reap count (cfg_fs.c does not persist that number anywhere
     * -- see this module's header comment). */
    size_t tmp_count = 0;
    if (mounted) {
        cfg_fs_list(".tmp", tmp_files, CFG_FS_STATUS_MAX_FILES, &tmp_count);
    }

    esp_err_t ret = ESP_OK;
    size_t o = 0;
    int n;
#define APPEND(...)                                                                                                  \
    do {                                                                                                             \
        n = snprintf(buf + o, buf_cap - o, __VA_ARGS__);                                                             \
        if (n < 0 || (size_t)n >= buf_cap - o) {                                                                     \
            ret = ESP_ERR_INVALID_SIZE;                                                                              \
            goto cleanup;                                                                                            \
        }                                                                                                            \
        o += (size_t)n;                                                                                             \
    } while (0)

    APPEND("{\"mounted\":%s,\"status\":\"%s\"", mounted ? "true" : "false", status_str(st));
    if (!mounted) {
        APPEND(",\"reason\":\"%s\"", status_reason(st));
    }

    if (cap && cap->known) {
        size_t free_bytes = (cap->total_bytes > cap->used_bytes) ? (cap->total_bytes - cap->used_bytes) : 0;
        APPEND(",\"capacity\":{\"known\":true,\"total_bytes\":%lu,\"used_bytes\":%lu,\"free_bytes\":%lu}",
              (unsigned long)cap->total_bytes, (unsigned long)cap->used_bytes, (unsigned long)free_bytes);
    } else {
        APPEND(",\"capacity\":{\"known\":false}");
    }

    /* persfx MED-1: stores frozen this boot (file unreadable / zones undecided): saves to them are refused until
     * reboot. At most 6 names listed, the count is exact. */
    {
        int dn = cfg_fs_degraded_count();
        APPEND(",\"degraded_count\":%d,\"degraded\":[", dn);
        for (int i = 0; i < dn && i < 6; i++) {
            char dname[48];
            if (cfg_fs_degraded_name(i, dname, sizeof(dname))) {
                APPEND("%s\"%s\"", i == 0 ? "" : ",", dname);
            }
        }
        APPEND("]");
    }

    APPEND(",\"file_count\":%lu,\"files\":[", (unsigned long)file_count);
    for (size_t i = 0; i < file_count; i++) {
        long sz = file_size_or_unknown(base_dir_for_sizes, NULL, files[i].name);
        if (sz >= 0) {
            APPEND("%s{\"name\":\"%s\",\"size_bytes\":%ld}", i == 0 ? "" : ",", files[i].name, sz);
        } else {
            APPEND("%s{\"name\":\"%s\",\"size_bytes\":null}", i == 0 ? "" : ",", files[i].name);
        }
    }
    APPEND("]");

    /* cfg_fs_list("") skips subdirectories, so files nested one level down
     * (profiles/prof<id>.json, profiles/hidden.json) never reach files[]
     * above. Summarize each known subdirectory as a count plus total bytes
     * instead of listing names: names would not be fetchable through
     * /api/cfgfs/file (which refuses '/'), and a summary keeps the response
     * inside CFG_FS_STATUS_HANDLER_JSON_BUF. tmp_files is free scratch by
     * now (only tmp_count was needed from it). Sizes come from the same
     * stat path as files[]; an entry whose size is unknown adds 0 to bytes
     * and bumps "unknown_size". */
    APPEND(",\"subdirs\":[");
    for (size_t d = 0; d < sizeof(CFG_FS_STATUS_SUBDIRS) / sizeof(CFG_FS_STATUS_SUBDIRS[0]); d++) {
        size_t sub_count = 0;
        unsigned long sub_bytes = 0;
        unsigned long sub_unknown = 0;
        if (mounted && cfg_fs_list(CFG_FS_STATUS_SUBDIRS[d], tmp_files, CFG_FS_STATUS_MAX_FILES, &sub_count) ==
                           ESP_OK) {
            for (size_t i = 0; i < sub_count; i++) {
                long sz = file_size_or_unknown(base_dir_for_sizes, CFG_FS_STATUS_SUBDIRS[d], tmp_files[i].name);
                if (sz >= 0) {
                    sub_bytes += (unsigned long)sz;
                } else {
                    sub_unknown++;
                }
            }
        } else {
            sub_count = 0;
        }
        APPEND("%s{\"name\":\"%s\",\"file_count\":%lu,\"size_bytes\":%lu,\"unknown_size\":%lu}",
              d == 0 ? "" : ",", CFG_FS_STATUS_SUBDIRS[d], (unsigned long)sub_count, sub_bytes, sub_unknown);
    }
    APPEND("]");

    APPEND(",\"tmp_entries_now\":%lu", (unsigned long)tmp_count);

    APPEND(",\"dual_write\":{\"write_mode\":\"cfg_only\",\"items\":[");
    size_t n_items = items ? item_count : 0;
    if (n_items > CFG_FS_STATUS_MAX_ITEMS) {
        /* Clamped, not rejected -- see cfg_fs_status.h's CFG_FS_STATUS_MAX_ITEMS
         * comment. Every item this codebase actually has fits comfortably
         * under the cap, so hitting this in practice would itself be a bug
         * worth a log line. */
        ESP_LOGW("cfg_fs_status", "dual-write item_count %u exceeds CFG_FS_STATUS_MAX_ITEMS %u -- truncating",
                 (unsigned)n_items, (unsigned)CFG_FS_STATUS_MAX_ITEMS);
        n_items = CFG_FS_STATUS_MAX_ITEMS;
    }
    for (size_t i = 0; i < n_items; i++) {
        const cfg_fs_dualwrite_item_t *it = &items[i];
        APPEND("%s{\"name\":\"%s\",\"file_backed\":%s,\"file_rev\":%lu,\"nvs_backed\":%s,\"nvs_rev\":%lu,"
              "\"diverged\":%s,\"nvs_stale\":%s,\"migration_deferred\":%s}",
              i == 0 ? "" : ",", it->name ? it->name : "?", it->file_valid ? "true" : "false",
              (unsigned long)it->file_rev, it->nvs_valid ? "true" : "false", (unsigned long)it->nvs_rev,
              it->diverged ? "true" : "false", it->nvs_stale ? "true" : "false",
              it->migration_deferred ? "true" : "false");
    }
    APPEND("]");
    /* 2026-09-08 audit (deaccc4f): this list used to also carry "prefs" and
     * "profiles", which went stale the moment 34927a77/530dc2f7 gave both
     * real persist/'*'_cfg_fs.c bridges (pref_cfg_fs.c, profiles_cfg_fs.c) --
     * an operator or the setup wizard reading /api/cfgfs would have been
     * told a migrated item was still pending. Verified against the actual
     * bridge modules under persist/ (kiln_cfg_store_cfg_fs.c,
     * pref_cfg_fs.c, profiles_cfg_fs.c, zones_config_cfg_fs.c,
     * firing_stats_cfg_fs.c), not against this doc-derived array.
     *
     * 2026-09-08 follow-up (this pass): the same staleness recurred for the
     * remaining three -- 762bb29e landed firing_stats_cfg_fs.c and gave
     * relay_cycles/adaptive_tune real pref_cfg_fs.c-backed dual-write paths,
     * but this array (and cfgfs_status_get_handler()'s item list) were left
     * pointing at the old "not migrated yet" state. All three now report
     * through dual_write.items[] instead (see diagnostics_http.c's
     * cfgfs_add_item() call sites) -- verified against relay_cycles.c's
     * RELAY_CYCLES_FILE_PATH/pref_cfg_fs_load_raw() call, adaptive_tune.c's
     * ADAPTIVE_TUNE_KIBASE_FILE_PATH/pref_cfg_fs_resolve() call, and
     * firing_stats_cfg_fs.c/.h existing as a real bridge module, not against
     * the prior report alone. This list is empty by construction now: every
     * item docs/FILESYSTEM_USER_DATA.md section 5 tracks as "MOVE" has
     * a bridge module AND a dual_write.items[] row. cfgfs_nvs_only_drift_
     * check.py fails the moment a new persist/'*'_cfg_fs.c bridge appears
     * without a matching row, so this list cannot go stale the same way a
     * third time without the check catching it. */
    APPEND(",\"nvs_only\":[]");
    /* Distinct from the above: items docs/FILESYSTEM_USER_DATA.md's
     * "KEEP in NVS" section says stay in NVS FOREVER, by design, for boot-
     * ordering or safety-isolation reasons -- not "not migrated yet". Listed
     * separately so an operator reading /api/cfgfs cannot mistake "working
     * as designed" for "unfinished migration" (the exact confusion this
     * audit was raised to prevent). See that doc section for the reason
     * behind each: wifi_creds/boot_guard_counter/watchdog_panic_disable
     * (must work before or independent of any mount), ota_record/
     * crash_report (must be writable from panic/OTA paths), touch_cal
     * (needed for the recovery UI before mount), run_state_breadcrumb
     * (read at early boot, UNDECIDED-leaning-KEEP), safety_mirror_esp (a
     * cache the safety path must not depend on the filesystem for),
     * rp2040_config_store (a different chip -- out of scope permanently),
     * logs/coredump (already on their own dedicated partitions). */
    /* 2026-10-07 audit: the list below also carries every persist/control
     * store that is NVS-only today, found by grepping for hal_kv_* users
     * with no cfg_fs or pref_cfg_fs path: profiles_favorites.c
     * (prof_favusr/prof_favbi), live_profile.c (live_edit_v1/live_prof),
     * firing_shadow.c (shadow_tune/sdwblob), ct_verify_store.c (ct_verify),
     * kiln_cfg_swap.c (swap_pend), aux_outputs_cfg.c's convert journal
     * (aux_conv_jrnl -- the aux_outputs config itself is a dual-write row),
     * setup_wizard_progress.c (progress_v1), plus pico_update_attempts.c
     * and pico_image_manifest.c (boot-time records in kiln_cfg). The
     * estop_verification.c (estop_verif) and dualwrite_window.c (dwwin, the
     * window tracker itself, which must not depend on the filesystem it
     * judges) are NVS-only too. The
     * credential stores are deliberately NOT named here: check_kiln_auth_config_
     * isolation.ps1 forbids any config-path file from referencing them. run_state.c
     * is the existing "run_state_breadcrumb" entry. iter_tune_store.c,
     * profiles_favorites.c, live_profile.c, ct_verify_store.c and
     * setup_wizard_progress.c are NOT listed: they moved to cfg files on
     * 2026-10-07 and report dual_write rows instead. */
    APPEND(",\"nvs_permanent\":[\"wifi_creds\",\"boot_guard_counter\",\"watchdog_panic_disable\","
          "\"ota_record\",\"crash_report\",\"touch_cal\",\"run_state_breadcrumb\",\"safety_mirror_esp\","
          "\"rp2040_config_store\",\"logs\",\"coredump\","
          "\"firing_shadow\",\"kiln_cfg_swap\",\"aux_convert_journal\","
          "\"pico_update_attempts\",\"pico_image_manifest\",\"estop_verification\",\"dualwrite_window\"]");
    APPEND("}");

    /* "dual_write_window" -- progress toward closing the dual-write window
     * (dualwrite_window.h), supplied by the caller from the read-only
     * dualwrite_window_get_status(); `known:false` when not supplied, same
     * "unknown is not zero" discipline as capacity/format. */
    if (win) {
        APPEND(",\"dual_write_window\":{\"known\":true,\"consecutive_clean_boots\":%lu,"
              "\"clean_boots_target\":%lu,\"firing_complete\":%s,\"restore_verified\":%s,"
              "\"window_may_close\":%s}",
              (unsigned long)win->consecutive_clean_boots, (unsigned long)win->clean_boots_target,
              win->firing_complete ? "true" : "false", win->restore_verified ? "true" : "false",
              win->window_may_close ? "true" : "false");
    } else {
        APPEND(",\"dual_write_window\":{\"known\":false}");
    }

    /* "format" -- observability for the deferred background auto-format
     * (docs/audits/boot_hang_2026-09-08.md follow-up): distinguishes a slow
     * format still running from a hang, and never claims knowledge this
     * process doesn't have (`known:false` when no format has run this boot,
     * same "unknown, not silently zeroed" discipline as capacity above). */
    APPEND(",\"format\":{");
    if (fmt && fmt->known) {
        bool stalled = cfg_fs_format_is_stalled(fmt->in_progress, fmt->elapsed_ms);
        APPEND("\"known\":true,\"in_progress\":%s,\"completed\":%s,\"succeeded\":%s,\"elapsed_ms\":%lu,"
              "\"stalled\":%s,\"ceiling_ms\":%lu",
              fmt->in_progress ? "true" : "false", fmt->completed ? "true" : "false",
              fmt->succeeded ? "true" : "false", (unsigned long)fmt->elapsed_ms, stalled ? "true" : "false",
              (unsigned long)CFG_FS_FORMAT_CEILING_MS);
        /* Failure detail: named esp_err_t, not just the boolean -- a format
         * that fails silently with only "succeeded":false is exactly what
         * turned the deferred-format worker-not-started race into a fresh
         * investigation instead of a one-query answer (see cfg_fs_mount.c's
         * wait_for_flash_worker()). Omitted (not "null") on success/still-
         * running, same "unknown is not zero" discipline as capacity above. */
        if (fmt->completed && !fmt->succeeded) {
            APPEND(",\"error\":\"%s\"", esp_err_to_name(fmt->result));
        }
    } else {
        APPEND("\"known\":false");
    }
    APPEND("}");

    APPEND("}");
#undef APPEND

    *out_len = o;

cleanup:
    free(files);
    free(tmp_files);
    return ret;
}
