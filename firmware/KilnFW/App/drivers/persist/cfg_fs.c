#include "cfg_fs.h"
#include "persist_scratch.h"

#include "esp_log.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define CFG_FS_MKDIR(path) _mkdir(path)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define CFG_FS_MKDIR(path) mkdir((path), 0755)
#endif

#define CFG_FS_BASE_MAX 480
#define CFG_FS_PATH_MAX 600

static char             s_base_dir[CFG_FS_BASE_MAX];
static cfg_fs_status_t  s_status = CFG_FS_STATUS_UNMOUNTED;

/* fsync a FILE* portably: _commit() on Windows (host tests), fsync() on
 * POSIX/newlib (on-device, once cfg_fs_mount.c wires this file up to a real
 * LittleFS mount). Both flush the OS-level write all the way to the backing
 * store, not just libc's own buffer -- fflush() alone is not enough to make
 * the "old file or new file, never truncated" guarantee hold across a real
 * power loss. */
static int cfg_fs_fsync(FILE *f)
{
#ifdef _WIN32
    return _commit(_fileno(f));
#else
    return fsync(fileno(f));
#endif
}

/* rename() onto an EXISTING destination fails on Windows (host tests) --
 * POSIX rename() and LittleFS's own rename() are both atomic-replace by
 * design, which is the guarantee this module relies on, so the Windows path
 * has to ask for that behavior explicitly rather than silently getting a
 * weaker one. */
static int cfg_fs_atomic_rename(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static bool path_join_ok(char *out, size_t cap, const char *base, const char *rel)
{
    int n = snprintf(out, cap, "%s/%s", base, rel);
    return n > 0 && (size_t)n < cap;
}

/* Splits `rel_path` into (parent-relative-dir, bare filename). Supports at
 * most one '/' -- matches the documented "one level of nesting" contract.
 * Returns false if `rel_path` is empty, has more than one '/', or either
 * half would be empty (e.g. "/x.json" or "profiles/"). */
static bool split_one_level(const char *rel_path, char *dir_out, size_t dir_cap, const char **name_out)
{
    const char *slash = strchr(rel_path, '/');
    if (!slash) {
        dir_out[0] = '\0';
        *name_out = rel_path;
        return rel_path[0] != '\0';
    }
    if (strchr(slash + 1, '/') != NULL) {
        return false; /* more than one level of nesting -- not supported */
    }
    size_t dir_len = (size_t)(slash - rel_path);
    if (dir_len == 0 || dir_len >= dir_cap || slash[1] == '\0') {
        return false;
    }
    memcpy(dir_out, rel_path, dir_len);
    dir_out[dir_len] = '\0';
    *name_out = slash + 1;
    return true;
}

static bool is_directory(const char *path)
{
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
#endif
}

/* Best-effort: EEXIST (already there) is success, not failure -- same
 * convention log_store_init() uses for its own base directory. */
static void ensure_dir(const char *path)
{
    CFG_FS_MKDIR(path);
}

/* Deletes every regular file directly inside `<base>/.tmp/` (never
 * recurses, never touches the directory itself). Called once from
 * cfg_fs_init() so a write interrupted by a crash/power-loss in a PRIOR
 * boot never leaves stale partial data lying around -- the corresponding
 * final file (old or already-renamed-new) is untouched by this sweep,
 * since sweeping only ever removes files under .tmp/. */
static size_t sweep_tmp(const char *base_dir)
{
    char tmp_dir[CFG_FS_PATH_MAX];
    if (!path_join_ok(tmp_dir, sizeof(tmp_dir), base_dir, ".tmp")) {
        return 0;
    }
    ensure_dir(tmp_dir);

    size_t reaped = 0;
#ifdef _WIN32
    char glob[CFG_FS_PATH_MAX];
    if (!path_join_ok(glob, sizeof(glob), tmp_dir, "*")) {
        return 0;
    }
    struct _finddata_t fd;
    intptr_t h = _findfirst(glob, &fd);
    if (h == -1) {
        return 0;
    }
    do {
        if (fd.attrib & _A_SUBDIR) {
            continue;
        }
        char victim[CFG_FS_PATH_MAX];
        if (path_join_ok(victim, sizeof(victim), tmp_dir, fd.name) && remove(victim) == 0) {
            reaped++;
        }
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(tmp_dir);
    if (!d) {
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char victim[CFG_FS_PATH_MAX];
        if (!path_join_ok(victim, sizeof(victim), tmp_dir, ent->d_name)) {
            continue;
        }
        if (is_directory(victim)) {
            continue;
        }
        if (remove(victim) == 0) {
            reaped++;
        }
    }
    closedir(d);
#endif
    return reaped;
}

esp_err_t cfg_fs_init(const char *base_dir, size_t *out_tmp_reaped)
{
    if (out_tmp_reaped) {
        *out_tmp_reaped = 0;
    }
    if (!base_dir || base_dir[0] == '\0' || strlen(base_dir) >= sizeof(s_base_dir)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_directory(base_dir)) {
        s_status = CFG_FS_STATUS_UNAVAILABLE;
        return ESP_FAIL;
    }

    strncpy(s_base_dir, base_dir, sizeof(s_base_dir) - 1);
    s_base_dir[sizeof(s_base_dir) - 1] = '\0';
    s_status = CFG_FS_STATUS_MOUNTED;

    size_t reaped = sweep_tmp(s_base_dir);
    if (out_tmp_reaped) {
        *out_tmp_reaped = reaped;
    }
    return ESP_OK;
}

esp_err_t cfg_fs_mount_or_skip(bool recovery_mode, const char *base_dir, size_t *out_tmp_reaped)
{
    if (out_tmp_reaped) {
        *out_tmp_reaped = 0;
    }
    if (recovery_mode) {
        /* Deliberately does not call cfg_fs_init() at all -- status stays
         * whatever it already was (UNMOUNTED on a fresh boot). Recovery
         * mode must be reachable with the cfg partition physically erased,
         * so this path must never touch the filesystem. */
        return ESP_OK;
    }
    return cfg_fs_init(base_dir, out_tmp_reaped);
}

void cfg_fs_deinit(void)
{
    s_status = CFG_FS_STATUS_UNMOUNTED;
    s_base_dir[0] = '\0';
}

cfg_fs_status_t cfg_fs_get_status(void)
{
    return s_status;
}

bool cfg_fs_is_available(void)
{
    return s_status == CFG_FS_STATUS_MOUNTED;
}

esp_err_t cfg_fs_exists(const char *rel_path, bool *out_exists)
{
    if (!rel_path || !out_exists) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_exists = false;
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[CFG_FS_PATH_MAX];
    if (!path_join_ok(path, sizeof(path), s_base_dir, rel_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        *out_exists = true;
    }
    return ESP_OK;
}

esp_err_t cfg_fs_read(const char *rel_path, void *buf, size_t cap, size_t *out_len)
{
    if (!rel_path || !buf || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[CFG_FS_PATH_MAX];
    if (!path_join_ok(path, sizeof(path), s_base_dir, rel_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if ((size_t)sz > cap) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        return ESP_FAIL;
    }
    if (out_len) {
        *out_len = got;
    }
    return ESP_OK;
}

/* Flattens a (possibly one-level-nested) rel_path into a single component
 * safe to place under .tmp/, e.g. "profiles/3.json" -> "profiles_3.json".
 * Collisions between "a/b.json" and "a_b.json" are accepted -- .tmp/ is
 * swept clean at every mount and a temp file only ever lives for the
 * duration of one write, so a same-boot collision between two writers of
 * literally those two paths is the only way it would matter, and nothing
 * in this codebase's planned layout produces that pair. */
static bool flatten_for_tmp(const char *rel_path, char *out, size_t cap)
{
    size_t n = strlen(rel_path);
    if (n == 0 || n >= cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (rel_path[i] == '/') ? '_' : rel_path[i];
    }
    out[n] = '\0';
    return true;
}

/* Strict read-back for cfg_fs_write_atomic()'s verification, same
 * discipline as boot_guard.c's verify_persisted_count() (see that
 * function's comment and docs/audits/boot_guard_recovery_loop_2026-09-08.md):
 * a successful rename()/MoveFileExA() return is NOT itself proof the bytes
 * landed -- that class of bug ("write reports success, storage disagrees")
 * has shipped twice already in this codebase (boot_guard's NVS clear, and
 * the underlying write-lies pattern documented for flash generally), and
 * cfg_fs is a second flash-backed write path with no coverage of its own.
 * Any failure to reopen/read/short-read/mismatch returns false -- there is
 * no "missing collapses to expected" shortcut here, unlike a normal read
 * path's defaults, because a verification step must tell "confirmed
 * correct" apart from "could not confirm" rather than conflating them. */
static bool verify_write_readback(const char *final_path, const void *data, size_t len,
                                  uint8_t *chunk, size_t chunk_cap)
{
    FILE *f = fopen(final_path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz != len) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    bool ok = true;
    if (len > 0) {
        /* Stream the comparison through a small fixed chunk instead of
         * malloc()'ing the whole blob -- this path runs on the
         * commissioning boot sequence and a whole-blob heap allocation here
         * was the strongest identified contributor to the internal-DRAM
         * low-water mark (docs/audits/heap_low_water_9051_2026-09-21.md).
         * Semantics are unchanged: any short read or any byte mismatch,
         * anywhere in the stream, still fails the verify. */
        const uint8_t *expected = (const uint8_t *)data;
        size_t remaining = len;
        while (remaining > 0) {
            size_t want = remaining < chunk_cap ? remaining : chunk_cap;
            size_t got = fread(chunk, 1, want, f);
            if (got != want || memcmp(chunk, expected, want) != 0) {
                ok = false;
                break;
            }
            expected += want;
            remaining -= want;
        }
    }
    fclose(f);
    return ok;
}

/* HEAP, never the stack -- the same rule cfg_fs_mount.c's
 * maybe_auto_format_and_remount() and zones_config_cfg_fs.c already follow,
 * and for the same reason. These six buffers used to be six locals of
 * cfg_fs_write_atomic(), whose frame measured 2000 B out of the flashed
 * ELF: the single largest first-party contributor to the stack overflow that
 * panicked bx_flash_worker on the first cfg write after a format
 * (docs/audits/bx_flash_worker_panic_after_cfgfs_format_2026-09-21.md).
 * CONTROL_CMD_SET_ZONE_PID is handled ON that worker, so the whole
 * control_handle_message -> zones nvs_save -> dual-write -> here ->
 * esp_vfs -> LittleFS chain nests in ONE 8 KB stack, and the LittleFS half
 * of it (lfs_dir_traverse recursion, deepest on a just-formatted volume)
 * cannot be bounded statically -- check_all_task_stack_budgets.py says so in
 * its own bx_flash_worker comment. Taking ~2 kB off this frame is the part
 * that is actually under our control.
 *
 * Allocation failure degrades, never panics (same convention as
 * scan_partition()'s chunk buffer in cfg_fs_mount.c): ESP_ERR_NO_MEM comes
 * back before anything on the filesystem has been touched, so the old file
 * -- if any -- is left exactly as it was, identical to every other
 * pre-rename failure branch below. */
typedef struct {
    char    parent_rel[CFG_FS_MAX_NAME];
    char    parent_abs[CFG_FS_PATH_MAX];
    char    tmp_dir[CFG_FS_PATH_MAX];
    char    flat[CFG_FS_MAX_NAME * 2];
    char    tmp_path[CFG_FS_PATH_MAX];
    char    final_path[CFG_FS_PATH_MAX];
    /* verify_write_readback()'s streaming compare buffer, carried here
     * rather than as a 256 B local of its own, for the same reason. */
    uint8_t chunk[256];
} cfg_fs_write_scratch_t;

esp_err_t cfg_fs_write_atomic(const char *rel_path, const void *data, size_t len)
{
    if (!rel_path || (!data && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* See cfg_fs_write_scratch_t's comment: every path buffer this function
     * needs lives in one heap block, not in this frame. */
    cfg_fs_write_scratch_t *sc = persist_scratch_alloc(sizeof(*sc));
    if (!sc) {
        return ESP_ERR_NO_MEM;
    }

    /* Ensure the immediate parent directory exists (one level of nesting
     * only, matching split_one_level()'s contract). */
    const char *bare_name = NULL;
    if (!split_one_level(rel_path, sc->parent_rel, sizeof(sc->parent_rel), &bare_name)) {
        free(sc);
        return ESP_ERR_INVALID_ARG;
    }
    if (sc->parent_rel[0] != '\0') {
        if (!path_join_ok(sc->parent_abs, sizeof(sc->parent_abs), s_base_dir, sc->parent_rel)) {
            free(sc);
            return ESP_ERR_INVALID_ARG;
        }
        ensure_dir(sc->parent_abs);
    }

    if (!path_join_ok(sc->tmp_dir, sizeof(sc->tmp_dir), s_base_dir, ".tmp")) {
        free(sc);
        return ESP_ERR_INVALID_ARG;
    }
    ensure_dir(sc->tmp_dir);

    if (!flatten_for_tmp(rel_path, sc->flat, sizeof(sc->flat))) {
        free(sc);
        return ESP_ERR_INVALID_ARG;
    }
    if (!path_join_ok(sc->tmp_path, sizeof(sc->tmp_path), sc->tmp_dir, sc->flat)) {
        free(sc);
        return ESP_ERR_INVALID_ARG;
    }
    if (!path_join_ok(sc->final_path, sizeof(sc->final_path), s_base_dir, rel_path)) {
        free(sc);
        return ESP_ERR_INVALID_ARG;
    }

    /* THE ATOMIC SEQUENCE. Every failure branch from here on removes the
     * temp file and returns without touching `final_path` at all -- the
     * old file (if any) is left exactly as it was. Binary mode ("wb") so
     * no CRLF translation touches the bytes on a Windows host build. */
    FILE *f = fopen(sc->tmp_path, "wb");
    if (!f) {
        free(sc);
        return ESP_FAIL;
    }
    size_t wrote = (len == 0) ? 0 : fwrite(data, 1, len, f);
    bool write_ok = (wrote == len);
    bool flush_ok = write_ok && (fflush(f) == 0);
    bool sync_ok = flush_ok && (cfg_fs_fsync(f) == 0);
    int close_rc = fclose(f);
    if (!write_ok || !flush_ok || !sync_ok || close_rc != 0) {
        remove(sc->tmp_path);
        free(sc);
        return ESP_FAIL;
    }

    if (cfg_fs_atomic_rename(sc->tmp_path, sc->final_path) != 0) {
        remove(sc->tmp_path);
        free(sc);
        return ESP_FAIL;
    }

    /* Read back what is now at final_path and compare against what was
     * asked to be written. The rename already happened -- unlike the
     * pre-rename failure branches above, there is no "leave the old file
     * untouched" option left at this point (the old file, if any, is
     * already gone) -- but a write that reports ESP_OK while the bytes on
     * disk disagree with the caller's buffer must never be handed back as
     * success. See verify_write_readback()'s comment for why this mirrors
     * boot_guard_mark_healthy()'s verified-clear discipline. */
    bool verified = verify_write_readback(sc->final_path, data, len, sc->chunk, sizeof(sc->chunk));
    free(sc);
    if (!verified) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t cfg_fs_delete(const char *rel_path)
{
    if (!rel_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[CFG_FS_PATH_MAX];
    if (!path_join_ok(path, sizeof(path), s_base_dir, rel_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (remove(path) != 0) {
        int e = errno;
        if (e == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGW("cfg_fs", "remove('%s') failed: errno %d", rel_path, e);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Every path buffer cfg_fs_list() needs lives in one heap block, not in this
 * frame -- same convention as cfg_fs_write_scratch_t above (dir_path/glob on
 * Windows-host builds, dir_path/full on the on-device POSIX/newlib path):
 * this function alone was a 1232 B httpd-worker stack frame from two
 * CFG_FS_PATH_MAX (600 B) locals, reachable from /api/cfgfs via
 * cfg_fs_status.c. */
typedef struct {
    char dir_path[CFG_FS_PATH_MAX];
    union {
        char glob[CFG_FS_PATH_MAX]; /* _WIN32 (host tests) */
        char full[CFG_FS_PATH_MAX]; /* POSIX/newlib (on-device) */
    };
} cfg_fs_list_scratch_t;

esp_err_t cfg_fs_list(const char *rel_dir, cfg_fs_entry_t *out, size_t max_out, size_t *out_count)
{
    if (!rel_dir || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    cfg_fs_list_scratch_t *sc = persist_scratch_alloc(sizeof(*sc));
    if (!sc) {
        return ESP_ERR_NO_MEM;
    }
    char *dir_path = sc->dir_path;

    if (rel_dir[0] == '\0') {
        strncpy(dir_path, s_base_dir, CFG_FS_PATH_MAX - 1);
        dir_path[CFG_FS_PATH_MAX - 1] = '\0';
    } else if (!path_join_ok(dir_path, CFG_FS_PATH_MAX, s_base_dir, rel_dir)) {
        free(sc);
        return ESP_ERR_INVALID_ARG;
    }

    size_t count = 0;
#ifdef _WIN32
    char *glob = sc->glob;
    if (!path_join_ok(glob, CFG_FS_PATH_MAX, dir_path, "*")) {
        free(sc);
        return ESP_ERR_INVALID_ARG;
    }
    struct _finddata_t fd;
    intptr_t h = _findfirst(glob, &fd);
    if (h == -1) {
        free(sc);
        return ESP_OK; /* empty (or missing) directory -- zero entries, not an error */
    }
    do {
        if (fd.attrib & _A_SUBDIR) {
            continue;
        }
        if (strcmp(fd.name, ".tmp") == 0) {
            continue;
        }
        if (count < max_out) {
            strncpy(out[count].name, fd.name, CFG_FS_MAX_NAME - 1);
            out[count].name[CFG_FS_MAX_NAME - 1] = '\0';
        }
        count++;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir_path);
    if (!d) {
        free(sc);
        return ESP_OK;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
            strcmp(ent->d_name, ".tmp") == 0) {
            continue;
        }
        char *full = sc->full;
        if (path_join_ok(full, CFG_FS_PATH_MAX, dir_path, ent->d_name) && is_directory(full)) {
            continue;
        }
        if (count < max_out) {
            strncpy(out[count].name, ent->d_name, CFG_FS_MAX_NAME - 1);
            out[count].name[CFG_FS_MAX_NAME - 1] = '\0';
        }
        count++;
    }
    closedir(d);
#endif
    free(sc);
    *out_count = (count < max_out) ? count : max_out;
    return ESP_OK;
}
