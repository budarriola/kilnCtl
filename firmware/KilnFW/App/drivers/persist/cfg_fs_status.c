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

#define CFG_FS_STATUS_MAX_FILES 32
#define CFG_FS_STATUS_PATH_MAX 600

/* -1 means "size unknown" -- distinguished from a real 0-byte file. */
static long file_size_or_unknown(const char *base_dir, const char *rel_name)
{
    if (!base_dir) {
        return -1;
    }
    char path[CFG_FS_STATUS_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", base_dir, rel_name);
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

esp_err_t cfg_fs_status_build_json(const char *base_dir_for_sizes, const cfg_fs_capacity_info_t *cap,
                                    const cfg_fs_zones_dualwrite_info_t *dual,
                                    const cfg_fs_format_progress_t *fmt, char *buf, size_t buf_cap,
                                    size_t *out_len)
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
     * before returning (see the `cleanup` label). */
    cfg_fs_entry_t *files = malloc(sizeof(cfg_fs_entry_t) * CFG_FS_STATUS_MAX_FILES);
    cfg_fs_entry_t *tmp_files = malloc(sizeof(cfg_fs_entry_t) * CFG_FS_STATUS_MAX_FILES);
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

    APPEND(",\"file_count\":%lu,\"files\":[", (unsigned long)file_count);
    for (size_t i = 0; i < file_count; i++) {
        long sz = file_size_or_unknown(base_dir_for_sizes, files[i].name);
        if (sz >= 0) {
            APPEND("%s{\"name\":\"%s\",\"size_bytes\":%ld}", i == 0 ? "" : ",", files[i].name, sz);
        } else {
            APPEND("%s{\"name\":\"%s\",\"size_bytes\":null}", i == 0 ? "" : ",", files[i].name);
        }
    }
    APPEND("]");

    APPEND(",\"tmp_entries_now\":%lu", (unsigned long)tmp_count);

    APPEND(",\"dual_write\":{");
    if (dual) {
        /* file_rev >= nvs_rev is the healthy state (dual-write always writes
         * the file first, per zones_config_cfg_fs.h's tie-break doc) --
         * nvs_rev strictly higher means a prior file write failed and only
         * NVS advanced, worth flagging rather than leaving implicit. */
        bool diverged = dual->file_valid && (dual->nvs_rev > dual->file_rev);
        APPEND("\"zones\":{\"file_backed\":%s,\"file_rev\":%lu,\"nvs_rev\":%lu,\"diverged\":%s}",
              dual->file_valid ? "true" : "false", (unsigned long)dual->file_rev, (unsigned long)dual->nvs_rev,
              diverged ? "true" : "false");
    } else {
        APPEND("\"zones\":{\"file_backed\":false}");
    }
    /* 2026-09-08 audit (deaccc4f): this list used to also carry "prefs" and
     * "profiles", which went stale the moment 34927a77/530dc2f7 gave both
     * real persist/*_cfg_fs.c bridges (pref_cfg_fs.c, profiles_cfg_fs.c) --
     * an operator or the setup wizard reading /api/cfgfs would have been
     * told a migrated item was still pending. Verified against the actual
     * bridge modules under persist/ (kiln_cfg_store_cfg_fs.c,
     * pref_cfg_fs.c, profiles_cfg_fs.c, zones_config_cfg_fs.c), not against
     * this doc-derived array, which is why it's now just these three: MOVE
     * items from docs/FILESYSTEM_USER_DATA_PLAN.md section 5 with NO bridge
     * module yet (steps not landed) -- firing stats/history (item 7),
     * adaptive-tune state (item 9), relay cycle counters (item 4, plan
     * recommends moving it last). cfgfs_nvs_only_drift_check.py fails the
     * moment a new persist/*_cfg_fs.c bridge appears without this array (and
     * that check) being updated to match, so this list cannot go stale the
     * same way again. */
    APPEND(",\"nvs_only\":[\"firing_stats\",\"adaptive_tune\",\"relay_cycles\"]");
    /* Distinct from the above: items docs/FILESYSTEM_USER_DATA_PLAN.md's
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
    APPEND(",\"nvs_permanent\":[\"wifi_creds\",\"boot_guard_counter\",\"watchdog_panic_disable\","
          "\"ota_record\",\"crash_report\",\"touch_cal\",\"run_state_breadcrumb\",\"safety_mirror_esp\","
          "\"rp2040_config_store\",\"logs\",\"coredump\"]");
    APPEND("}");

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
