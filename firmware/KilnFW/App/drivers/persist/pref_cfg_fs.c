// See pref_cfg_fs.h for the full design/rationale.
#include "pref_cfg_fs.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "cfg_fs.h"
#include "persist_scratch.h"

static const char *PREF_FS_TAG = "pref_cfg_fs";

static pref_cfg_fs_write_fn_t s_write_fn = cfg_fs_write_atomic;

void pref_cfg_fs_set_write_fn(pref_cfg_fs_write_fn_t fn)
{
    s_write_fn = fn ? fn : cfg_fs_write_atomic;
}

void pref_cfg_fs_reset_write_fn_for_test(void)
{
    s_write_fn = cfg_fs_write_atomic;
}

pref_cfg_fs_write_fn_t pref_cfg_fs_get_write_fn(void)
{
    return s_write_fn;
}

static pref_cfg_fs_save_enter_fn_t s_save_enter_fn = NULL;
static pref_cfg_fs_save_exit_fn_t s_save_exit_fn = NULL;

void pref_cfg_fs_set_save_section_hooks(pref_cfg_fs_save_enter_fn_t enter, pref_cfg_fs_save_exit_fn_t exit_fn)
{
    s_save_enter_fn = enter;
    s_save_exit_fn = exit_fn;
}

bool pref_cfg_fs_save_section_enter(void)
{
    pref_cfg_fs_save_enter_fn_t fn = s_save_enter_fn;
    return fn ? fn() : false;
}

void pref_cfg_fs_save_section_exit(bool reserved)
{
    pref_cfg_fs_save_exit_fn_t fn = s_save_exit_fn;
    if (fn) {
        fn(reserved);
    }
}

static pref_cfg_fs_reset_refuse_fn_t volatile s_reset_refuse_fn = NULL;

void pref_cfg_fs_set_reset_refuse_hook(pref_cfg_fs_reset_refuse_fn_t fn)
{
    s_reset_refuse_fn = fn;
}

bool pref_cfg_fs_reset_refuses_write(void)
{
    pref_cfg_fs_reset_refuse_fn_t fn = s_reset_refuse_fn;
    return fn ? fn() : false;
}

static void *s_lock_registry[PREF_CFG_FS_LOCK_REGISTRY_MAX];
static size_t s_lock_registry_n = 0;
static portMUX_TYPE s_lock_registry_mux = portMUX_INITIALIZER_UNLOCKED;

void pref_cfg_fs_lock_registry_add(void *lock)
{
    bool full = false;
    portENTER_CRITICAL(&s_lock_registry_mux);
    if (s_lock_registry_n < PREF_CFG_FS_LOCK_REGISTRY_MAX) {
        s_lock_registry[s_lock_registry_n++] = lock;
    } else {
        full = true;
    }
    portEXIT_CRITICAL(&s_lock_registry_mux);
    if (full) {
        ESP_LOGE(PREF_FS_TAG, "save lock registry full: raise PREF_CFG_FS_LOCK_REGISTRY_MAX");
    }
}

size_t pref_cfg_fs_lock_registry_count(void)
{
    portENTER_CRITICAL(&s_lock_registry_mux);
    size_t n = s_lock_registry_n;
    portEXIT_CRITICAL(&s_lock_registry_mux);
    return n;
}

void *pref_cfg_fs_lock_registry_get(size_t i)
{
    void *p = NULL;
    portENTER_CRITICAL(&s_lock_registry_mux);
    if (i < s_lock_registry_n) {
        p = s_lock_registry[i];
    }
    portEXIT_CRITICAL(&s_lock_registry_mux);
    return p;
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Scratch for "<4B rev><item>". Items up to PREF_CFG_FS_MAX_ITEM use the
 * caller's stack (the pre-existing behaviour, no heap); larger items (the
 * setup-wizard blob, the live-edit working profile) take a short-lived heap
 * block so no httpd-task stack ever carries a multi-hundred-byte buffer. */
typedef struct {
    uint8_t stack[4 + PREF_CFG_FS_MAX_ITEM];
    uint8_t *p;
    size_t cap;
} raw_buf_t;

static bool raw_buf_get(raw_buf_t *b, size_t item_size)
{
    b->cap = 4 + item_size;
    if (item_size <= PREF_CFG_FS_MAX_ITEM) {
        b->p = b->stack;
        return true;
    }
    b->p = (uint8_t *)persist_scratch_alloc(b->cap);
    return b->p != NULL;
}

static void raw_buf_put(raw_buf_t *b)
{
    if (b->p != b->stack) {
        free(b->p);
    }
    b->p = NULL;
}

/* Returns ESP_OK when the read COMPLETED and *out_valid says whether the file held a usable item
 * (absent, wrong-size, oversized and validator-rejected files are all ESP_OK / valid=false: the file is
 * known not to hold a usable value). Returns an error -- ESP_ERR_NO_MEM for a failed scratch
 * allocation, otherwise cfg_fs_read()'s code -- when the file's state could NOT be determined
 * (K10-09/K10-10): a caller must then treat the file as possibly holding a newer value and must not
 * overwrite it. An unmounted cfg is ESP_OK / absent, matching the old void API (the normal state on a
 * board without cfg). */
static esp_err_t load_raw_impl(const char *rel_path, size_t item_size, pref_cfg_fs_validate_fn_t validate,
                               void *out_bytes, uint32_t *out_rev, bool *out_valid, bool quiet)
{
    if (out_bytes) {
        memset(out_bytes, 0, item_size);
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_valid) {
        *out_valid = false;
    }
    if (!rel_path || !out_bytes || !out_rev || !out_valid || item_size == 0 || item_size > PREF_CFG_FS_MAX_LARGE_ITEM) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_OK;
    }

    raw_buf_t rb;
    if (!raw_buf_get(&rb, item_size)) {
        ESP_LOGW(PREF_FS_TAG, "%s read skipped: scratch allocation failed -- file state unknown", rel_path);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *raw = rb.p;
    size_t len = 0;
    esp_err_t err = cfg_fs_read(rel_path, raw, rb.cap, &len);
    if (err != ESP_OK) {
        raw_buf_put(&rb);
        // ESP_ERR_NOT_FOUND (never migrated yet) is the normal state on
        // every board today -- not logged, same convention as
        // zones_config_cfg_fs_load_raw(). ESP_ERR_INVALID_SIZE is a file
        // bigger than any item this build writes: unusable, not unknown.
        // Anything else (I/O failure) leaves the file's state unknown.
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE) {
            return ESP_OK;
        }
        if (!quiet) {
            ESP_LOGW(PREF_FS_TAG, "%s read failed: %s -- file state unknown", rel_path, esp_err_to_name(err));
        }
        return err;
    }
    if (len != 4 + item_size) {
        if (!quiet) {
            ESP_LOGW(PREF_FS_TAG, "%s is %u bytes, expected %u (4-byte rev + %u-byte item) -- ignoring", rel_path,
                     (unsigned)len, (unsigned)(4 + item_size), (unsigned)item_size);
        }
        raw_buf_put(&rb);
        return ESP_OK;
    }

    uint32_t rev = get_u32_le(raw);
    const void *item_bytes = raw + 4;
    if (validate && !validate(item_bytes, item_size)) {
        if (!quiet) {
            ESP_LOGW(PREF_FS_TAG, "%s (rev %lu) REJECTED by validator -- ignoring file, NVS candidate decides",
                     rel_path, (unsigned long)rev);
        }
        raw_buf_put(&rb);
        return ESP_OK;
    }

    memcpy(out_bytes, item_bytes, item_size);
    *out_rev = rev;
    *out_valid = true;
    raw_buf_put(&rb);
    return ESP_OK;
}

esp_err_t pref_cfg_fs_load_raw_checked(const char *rel_path, size_t item_size, pref_cfg_fs_validate_fn_t validate,
                                       void *out_bytes, uint32_t *out_rev, bool *out_valid)
{
    return load_raw_impl(rel_path, item_size, validate, out_bytes, out_rev, out_valid, false);
}

void pref_cfg_fs_load_raw(const char *rel_path, size_t item_size, pref_cfg_fs_validate_fn_t validate,
                           void *out_bytes, uint32_t *out_rev, bool *out_valid)
{
    (void)load_raw_impl(rel_path, item_size, validate, out_bytes, out_rev, out_valid, false);
}

void pref_cfg_fs_load_raw_quiet(const char *rel_path, size_t item_size, pref_cfg_fs_validate_fn_t validate,
                                void *out_bytes, uint32_t *out_rev, bool *out_valid)
{
    (void)load_raw_impl(rel_path, item_size, validate, out_bytes, out_rev, out_valid, true);
}

bool pref_cfg_fs_probe_newer_wrong_size(const char *rel_path, size_t item_size, size_t version_offset,
                                        uint8_t current_version, uint8_t *out_version)
{
    if (!rel_path || item_size == 0 || version_offset >= item_size || !cfg_fs_is_available()) {
        return false;
    }
    /* Only the header is needed; a bounded read of a larger file would fail
     * cfg_fs_read()'s size check, so read into a buffer sized for the
     * largest supported file. */
    size_t cap = 4 + PREF_CFG_FS_MAX_LARGE_ITEM + 64;
    uint8_t *buf = (uint8_t *)persist_scratch_alloc(cap);
    if (!buf) {
        /* K10-11: cannot decide -> answer "newer" so the caller keeps the file untouched. */
        ESP_LOGW(PREF_FS_TAG, "%s newer-schema probe: scratch allocation failed -- treating file as newer", rel_path);
        if (out_version) {
            *out_version = 0xFF;
        }
        return true;
    }
    size_t len = 0;
    esp_err_t err = cfg_fs_read(rel_path, buf, cap, &len);
    bool newer = false;
    if (err == ESP_ERR_INVALID_SIZE) {
        /* The file exists but is bigger than anything this build can write
         * (pref_cfg_fs_save() refuses items over PREF_CFG_FS_MAX_LARGE_ITEM), so
         * it can only come from newer firmware. Count it as NEWER so resolve()
         * never overwrites it. The version byte is unreadable here: report 0xFF
         * ("unknown, newer"). */
        newer = true;
        if (out_version) {
            *out_version = 0xFF;
        }
    } else if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        /* K10-11: an unreadable file (I/O error) is undecidable, not "not newer". */
        newer = true;
        if (out_version) {
            *out_version = 0xFF;
        }
    } else if (err == ESP_OK && len != 4 + item_size && len > 4 + version_offset && buf[4 + version_offset] > current_version) {
        newer = true;
        if (out_version) {
            *out_version = buf[4 + version_offset];
        }
    }
    free(buf);
    return newer;
}

esp_err_t pref_cfg_fs_save(const char *rel_path, const void *bytes, size_t item_size, uint32_t rev)
{
    if (!rel_path || !bytes || item_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (item_size > PREF_CFG_FS_MAX_LARGE_ITEM) {
        ESP_LOGE(PREF_FS_TAG, "%s item is %u bytes, exceeds PREF_CFG_FS_MAX_LARGE_ITEM (%u) -- refusing to write",
                 rel_path, (unsigned)item_size, (unsigned)PREF_CFG_FS_MAX_LARGE_ITEM);
        return ESP_ERR_INVALID_SIZE;
    }
    if (pref_cfg_fs_reset_refuses_write()) {
        ESP_LOGW(PREF_FS_TAG, "%s write refused: factory reset in progress", rel_path);
        return ESP_ERR_INVALID_STATE;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    raw_buf_t rb;
    if (!raw_buf_get(&rb, item_size)) {
        return ESP_ERR_NO_MEM;
    }
    put_u32_le(rb.p, rev);
    memcpy(rb.p + 4, bytes, item_size);

    esp_err_t err = s_write_fn(rel_path, rb.p, 4 + item_size);
    raw_buf_put(&rb);
    if (err != ESP_OK) {
        ESP_LOGW(PREF_FS_TAG, "%s write (rev %lu) failed: %s", rel_path, (unsigned long)rev, esp_err_to_name(err));
    }
    return err;
}

esp_err_t pref_cfg_fs_commit(const char *rel_path, const void *bytes, size_t item_size, uint32_t rev,
                             const char *what)
{
    esp_err_t err = pref_cfg_fs_save(rel_path, bytes, item_size, rev);
    if (err != ESP_OK) {
        ESP_LOGE(PREF_FS_TAG, "%s NOT persisted: cfg write of %s (rev %lu) failed: %s -- NVS is no longer written, "
                              "the value lives in RAM until reboot",
                 what ? what : "setting", rel_path ? rel_path : "?", (unsigned long)rev, esp_err_to_name(err));
    }
    return err;
}

esp_err_t pref_cfg_fs_load_var_checked(const char *rel_path, void *out, size_t cap, size_t *out_len,
                                       uint32_t *out_rev)
{
    if (out_len) {
        *out_len = 0;
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (!rel_path || !out || !out_len || !out_rev || cap == 0 || cap > PREF_CFG_FS_MAX_LARGE_ITEM ||
        !cfg_fs_is_available()) {
        return ESP_ERR_NOT_FOUND;
    }
    raw_buf_t rb;
    if (!raw_buf_get(&rb, cap)) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    esp_err_t err = cfg_fs_read(rel_path, rb.p, rb.cap, &len);
    if (err != ESP_OK || len < 4 + 1) {
        raw_buf_put(&rb);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE) {
            return ESP_ERR_NOT_FOUND; /* absent / too short / over cap: no usable file */
        }
        return err; /* I/O failure: state unknown */
    }
    *out_rev = get_u32_le(rb.p);
    *out_len = len - 4;
    memcpy(out, rb.p + 4, len - 4);
    raw_buf_put(&rb);
    return ESP_OK;
}

bool pref_cfg_fs_load_var(const char *rel_path, void *out, size_t cap, size_t *out_len, uint32_t *out_rev)
{
    return pref_cfg_fs_load_var_checked(rel_path, out, cap, out_len, out_rev) == ESP_OK;
}

esp_err_t pref_cfg_fs_remove(const char *rel_path)
{
    if (!rel_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Absent is the state the caller asked for: report it as success, as the
     * header documents, so a clear of an already-clear store is idempotent. */
    esp_err_t err = cfg_fs_delete(rel_path);
    return (err == ESP_ERR_NOT_FOUND) ? ESP_OK : err;
}

static bool resolve_with_file(const char *rel_path, const void *nvs_bytes, size_t item_size, bool nvs_valid,
                              uint32_t nvs_rev, pref_cfg_fs_validate_fn_t validate, void *out_bytes,
                              uint32_t *out_rev, bool *out_used_file, uint8_t *file_bytes)
{
    uint32_t file_rev = 0;
    bool file_valid = false;
    esp_err_t rerr = pref_cfg_fs_load_raw_checked(rel_path, item_size, validate, file_bytes, &file_rev, &file_valid);
    if (rerr != ESP_OK) {
        /* K10-09/10: the file's state is unknown (allocation or I/O failure). It may hold a newer value
         * than the NVS candidate: adopt nothing, write nothing, report failure. */
        ESP_LOGE(PREF_FS_TAG, "%s unreadable (%s) -- not resolving, file left untouched", rel_path,
                 esp_err_to_name(rerr));
        memset(out_bytes, 0, item_size);
        *out_rev = 0;
        *out_used_file = false;
        return false;
    }

    if (!file_valid) {
        // No usable file -- fall back to the NVS candidate, and if it is
        // itself trustworthy, opportunistically write it out (lazy,
        // one-item-at-a-time migration: the first successful load after
        // `cfg` becomes available, or after a corrupt file is detected,
        // writes a fresh file).
        memcpy(out_bytes, nvs_bytes, item_size);
        *out_rev = nvs_rev;
        *out_used_file = false;
        if (nvs_valid) {
            esp_err_t werr = pref_cfg_fs_save(rel_path, nvs_bytes, item_size, nvs_rev);
            if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(PREF_FS_TAG, "could not migrate NVS %s to file: %s", rel_path, esp_err_to_name(werr));
            }
        }
        return nvs_valid;
    }

    if (!nvs_valid) {
        // File is good, NVS side has nothing trustworthy -- use the file
        // outright. Not logged as a divergence: nothing on the NVS side to
        // disagree WITH.
        memcpy(out_bytes, file_bytes, item_size);
        *out_rev = file_rev;
        *out_used_file = true;
        return true;
    }

    bool differs = memcmp(file_bytes, nvs_bytes, item_size) != 0;
    if (!differs) {
        memcpy(out_bytes, file_bytes, item_size);
        *out_rev = file_rev > nvs_rev ? file_rev : nvs_rev;
        *out_used_file = true;
        return true;
    }

    // DIVERGENCE TIE-BREAK: STRICTLY higher rev wins, same rule
    // zones_config_cfg_fs.c uses (docs/audits/filesystem_migration_review_
    // 2026-09-07.md section 2). Under normal dual-write operation (file
    // written first, then NVS, same rev on both) file_rev > nvs_rev always
    // holds after a successful save; EQUAL revs with differing bytes can
    // only mean an NVS-only writer (firmware from before this dual-write
    // existed, rolled back to) wrote the blob without touching the rev --
    // the NVS side is then the newer one, never the file. `>=` here would
    // silently discard that edit and then overwrite it permanently on the
    // next save.
    if (file_rev > nvs_rev) {
        ESP_LOGW(PREF_FS_TAG, "%s file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting FILE (strictly higher rev)",
                 rel_path, (unsigned long)file_rev, (unsigned long)nvs_rev);
        memcpy(out_bytes, file_bytes, item_size);
        *out_rev = file_rev;
        *out_used_file = true;
        // NVS resync happens on the caller's next save() call (mirrors
        // zones_config_cfg_fs_resolve()'s identical note) -- this module
        // never touches NVS directly.
    } else {
        ESP_LOGW(PREF_FS_TAG,
                 "%s file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting NVS (higher rev), resyncing file",
                 rel_path, (unsigned long)file_rev, (unsigned long)nvs_rev);
        memcpy(out_bytes, nvs_bytes, item_size);
        *out_rev = nvs_rev;
        *out_used_file = false;
        esp_err_t werr = pref_cfg_fs_save(rel_path, nvs_bytes, item_size, nvs_rev);
        if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(PREF_FS_TAG, "could not resync %s from NVS: %s", rel_path, esp_err_to_name(werr));
        }
    }
    return true;
}


bool pref_cfg_fs_resolve(const char *rel_path, const void *nvs_bytes, size_t item_size, bool nvs_valid,
                          uint32_t nvs_rev, pref_cfg_fs_validate_fn_t validate, void *out_bytes, uint32_t *out_rev,
                          bool *out_used_file)
{
    if (out_bytes) {
        memset(out_bytes, 0, item_size);
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_used_file) {
        *out_used_file = false;
    }
    if (!rel_path || !nvs_bytes || !out_bytes || !out_rev || !out_used_file || item_size == 0 ||
        item_size > PREF_CFG_FS_MAX_LARGE_ITEM) {
        return false;
    }

    uint8_t file_stack[PREF_CFG_FS_MAX_ITEM];
    uint8_t *file_bytes = file_stack;
    if (item_size > PREF_CFG_FS_MAX_ITEM) {
        file_bytes = (uint8_t *)persist_scratch_alloc(item_size);
        if (!file_bytes) {
            return false;
        }
    }
    bool ok = resolve_with_file(rel_path, nvs_bytes, item_size, nvs_valid, nvs_rev, validate, out_bytes, out_rev,
                                out_used_file, file_bytes);
    if (file_bytes != file_stack) {
        free(file_bytes);
    }
    return ok;
}
