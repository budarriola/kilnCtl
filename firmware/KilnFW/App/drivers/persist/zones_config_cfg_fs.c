// See zones_config_cfg_fs.h for the full design/rationale.
#include "zones_config_cfg_fs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "cfg_fs.h"
#include "persist_scratch.h"

static const char *ZCFG_FS_TAG = "zones_cfg_fs";

static zones_cfg_fs_write_fn_t s_write_fn = cfg_fs_write_atomic;

void zones_config_cfg_fs_set_write_fn(zones_cfg_fs_write_fn_t fn)
{
    s_write_fn = fn ? fn : cfg_fs_write_atomic;
}

void zones_config_cfg_fs_reset_write_fn_for_test(void)
{
    s_write_fn = cfg_fs_write_atomic;
}

zones_cfg_fs_write_fn_t zones_config_cfg_fs_get_write_fn(void)
{
    return s_write_fn;
}

/* rev(4 bytes LE) + the on-flash blob. Sized generously above
 * sizeof(zones_cfg_t) (itself capped at ZONES_CONFIG_BLOB_MAX_SIZE by
 * zones_config_accessors.h's own _Static_assert) so a future field growth
 * inside that budget never has to touch this buffer size too. */
#define ZCFG_FILE_BUF_MAX (4 + 1024)

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

/* Shared body for zones_config_cfg_fs_load_raw() and
 * zones_config_cfg_fs_resolve() -- out_on_disk_version is the extra output
 * only resolve() needs (the file blob's own claimed version byte, for the
 * migration-persist fault latch; see this file's header comment on
 * zones_config_cfg_fs_resolve()). Kept as one internal implementation
 * instead of two near-duplicate file reads, and instead of widening
 * zones_config_cfg_fs_load_raw()'s own public signature, which has over a
 * dozen call sites in test_zones_config_cfg_fs.c that do not need this
 * value. */
static void load_raw_impl(zones_cfg_t *out_cfg, uint32_t *out_rev, bool *out_valid, uint8_t *out_on_disk_version)
{
    if (out_cfg) {
        memset(out_cfg, 0, sizeof(*out_cfg));
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_valid) {
        *out_valid = false;
    }
    if (out_on_disk_version) {
        *out_on_disk_version = 0;
    }
    if (!out_cfg || !out_rev || !out_valid) {
        return;
    }
    if (!cfg_fs_is_available()) {
        return;
    }

    /* HEAP, never the stack: this whole chain runs on the `main` task
     * (8192 B) at boot, and a stack buffer here plus a zones_cfg_t local
     * below made this one frame 1904 B -- part of the main-task overflow in
     * docs/audits/boot_hang_2026-09-08.md. */
    uint8_t *raw = persist_scratch_alloc(ZCFG_FILE_BUF_MAX);
    if (!raw) {
        ESP_LOGW(ZCFG_FS_TAG, "zones config file read buffer alloc failed -- cannot decide");
        if (out_on_disk_version) {
            *out_on_disk_version = ZONES_CFG_RESOLVE_OOM_VERSION;
        }
        return;
    }
    size_t len = 0;
    esp_err_t err = cfg_fs_read(ZONES_CFG_FILE_PATH, raw, ZCFG_FILE_BUF_MAX, &len);
    if (err != ESP_OK) {
        free(raw);
        /* ESP_ERR_NOT_FOUND (never migrated yet), ESP_ERR_INVALID_SIZE (file
         * larger than this buffer -- cannot happen for a well-formed file,
         * but a corrupted length must not be trusted either), or any other
         * read failure: none of these are "found but bad", so nothing is
         * logged here -- the caller's resolve() logic decides whether that
         * is worth a divergence warning (it is not, on its own; an absent
         * file is the normal state on every board today). */
        return;
    }
    if (len < 5) { /* need at least the rev prefix + a 1-byte version */
        ESP_LOGW(ZCFG_FS_TAG, "zones config file is %u bytes, too short to hold a rev + blob -- ignoring",
                 (unsigned)len);
        free(raw);
        return;
    }

    uint32_t rev = get_u32_le(raw);
    /* The blob's own claimed version byte (byte 0 of raw+4, same convention
     * as nvs_load_from_decode()'s `raw[0]` in zones_config_store.c) --
     * captured from the raw bytes before decode/migration overwrites
     * out_cfg->version with ZONES_CFG_VERSION, and before `raw` is freed
     * below. */
    uint8_t on_disk_version = raw[4];
    const char *reason = "";
    /* Decoded straight into the caller's buffer -- see the malloc comment
     * above; a zones_cfg_t local here was another 812 B of main-task stack.
     * out_cfg is re-zeroed on rejection so a caller that ignores *out_valid
     * still sees the same all-zero struct it did before. */
    zones_decode_result_t result = zones_config_json_decode_blob(raw + 4, len - 4, out_cfg, &reason);
    free(raw);
    if (result == ZONES_DECODE_OOM) {
        ESP_LOGW(ZCFG_FS_TAG, "zones config file decode ran out of memory -- cannot decide");
        memset(out_cfg, 0, sizeof(*out_cfg));
        if (out_on_disk_version) {
            *out_on_disk_version = ZONES_CFG_RESOLVE_OOM_VERSION;
        }
        return;
    }
    if (result != ZONES_DECODE_OK) {
        ESP_LOGW(ZCFG_FS_TAG, "zones config file (rev %lu) REJECTED: %s -- ignoring file, NVS candidate decides",
                 (unsigned long)rev, reason);
        memset(out_cfg, 0, sizeof(*out_cfg));
        return;
    }
    /* Same normalize-not-reject discipline zones_config_store.c's own
     * nvs_load_from_decode() applies to a decoded NVS blob -- see
     * zones_config_json_normalize_settings_source_cycles()'s own comment.
     * Without this, a settings_source cycle stored in the `cfg` file
     * (however it got there) survives THIS decode un-normalized: if the file
     * then wins zones_config_cfg_fs_resolve()'s tie-break, nvs_load()'s
     * resolve/memcmp compares an un-normalized file struct against a
     * normalized NVS one (nvs_load_from_decode() always normalizes), which
     * can read as a spurious divergence, and write-back would persist the
     * un-normalized cycle into NVS -- normalized only on THAT blob's own
     * next decode, two boots later instead of one. Normalizing here keeps
     * both stores' decode paths identical. */
    zones_config_json_normalize_settings_source_cycles(out_cfg, ZONES_CFG_FILE_PATH);

    *out_rev = rev;
    *out_valid = true;
    if (out_on_disk_version) {
        *out_on_disk_version = on_disk_version;
    }
}

void zones_config_cfg_fs_load_raw(zones_cfg_t *out_cfg, uint32_t *out_rev, bool *out_valid)
{
    load_raw_impl(out_cfg, out_rev, out_valid, NULL);
}

esp_err_t zones_config_cfg_fs_save(const zones_cfg_t *cfg, uint32_t rev)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t blob_len = sizeof(*cfg);
    if (4 + blob_len > (size_t)ZCFG_FILE_BUF_MAX) {
        /* Cannot happen given the _Static_assert on zones_cfg_t's size, but
         * fail loudly rather than silently truncate a partial write if that
         * ever regresses. */
        ESP_LOGE(ZCFG_FS_TAG, "zones_cfg_t (%u bytes) no longer fits the file buffer -- refusing to write",
                 (unsigned)blob_len);
        return ESP_ERR_INVALID_SIZE;
    }
    /* Re-stamp the CRC into OUR OWN COPY right before writing, never trust
     * the caller's embedded cfg->crc32 as still current: zones_config_store.c's
     * nvs_load_from() can mutate its decoded struct AFTER the CRC was
     * validated (zones_config_json_normalize_settings_source_cycles() collapsing a
     * stored settings_source cycle is the one that actually bit this --
     * 2026-09-07, found by this module's own divergence-resync test writing
     * a load-time-normalized struct back out and having it fail its OWN
     * subsequent decode) without ever re-stamping crc32 -- that struct is
     * still perfectly fine to RUN a kiln against (nothing reads its crc32
     * except a future save/decode), but it is no longer self-consistent to
     * serialize verbatim. Recomputing here, in the one place this module
     * actually writes bytes, means every file this function ever produces
     * is guaranteed to decode cleanly regardless of what happened to the
     * caller's copy before it got here.
     *
     * 2026-09-09 correction: the paragraph above used to claim this uses "a
     * local, properly-aligned copy (not a cast of `raw + 4`)" to avoid any
     * alignment assumption about a byte buffer. 379f3fe6's frame-reduction
     * pass (see the 2026-09-09 comment below) replaced that local struct
     * copy with exactly the cast this comment says is avoided --
     * `(zones_cfg_t *)(raw + 4)`, below. It is safe TODAY only because
     * `zones_cfg_t` contains no 8-byte-aligned member (no `uint64_t`,
     * `double`, etc.), so `_Alignof(zones_cfg_t) == 4` and `raw + 4` is
     * always 4-aligned (malloc's own alignment guarantee covers `raw`
     * itself). Nothing enforced that invariant, so a future 8-byte field
     * added to zones_cfg_t would silently misalign this access instead of
     * failing the build -- the _Static_assert immediately below exists to
     * turn that into a compile error instead. */
    /* HEAP, never the stack -- the same rule the load path above already
     * follows, and for the same reason. 2026-09-09: this was the one
     * remaining stack copy of this buffer, and with `stamped` beside it this
     * frame measured 1952 B. That is reached from httpd handlers as well as
     * from `main` (revert_post_handler -> adaptive_tune_revert ->
     * zones_config_set_model -> nvs_save -> here), and it pushed
     * revert_post_handler to 4880 B against check_httpd_task_stack_budget's
     * 4832 B ceiling on the shared 8 KB httpd_worker stack. Allocating it
     * takes ~1 KB off every one of those paths at once. */
    uint8_t *raw = persist_scratch_alloc(ZCFG_FILE_BUF_MAX);
    if (!raw) {
        ESP_LOGW(ZCFG_FS_TAG, "zones config file write buffer alloc failed -- file not written (rev %lu)",
                 (unsigned long)rev);
        return ESP_ERR_NO_MEM;
    }

    /* NOT a `zones_cfg_t stamped = *cfg` local anymore (2026-09-09 panic,
     * docs/audits/executor_panic_stack_overflow_2026-09-09.md): that ~900 B
     * stack copy was the other half of this frame's overflow on
     * profile_executor's 4096 B stack, alongside `raw` above (fixed
     * separately, same day). Copy straight into the heap buffer instead,
     * compute the CRC in place there (zones_config_json_compute_crc() itself
     * no longer needs a whole-struct copy either, see its own comment), and
     * stamp the result back into the copy's own crc32 field -- never touches
     * the caller's cfg. */
    put_u32_le(raw, rev);
    memcpy(raw + 4, cfg, blob_len);
    /* See the 2026-09-09 correction above: this cast is only well-aligned
     * because zones_cfg_t needs no more than 4-byte alignment. Pin that
     * assumption so an 8-byte field added later fails the build instead of
     * producing a misaligned struct access on the board. */
    _Static_assert(_Alignof(zones_cfg_t) <= 4, "zones_cfg_t alignment grew past 4 -- raw + 4 below is only "
                                                "4-aligned; a >4-byte-aligned member here needs a real "
                                                "aligned copy, not this cast");
    zones_cfg_t *raw_cfg = (zones_cfg_t *)(raw + 4);
    uint32_t crc = zones_config_json_compute_crc(raw_cfg);
    put_u32_le(raw + 4 + offsetof(zones_cfg_t, crc32), crc);

    esp_err_t err = s_write_fn(ZONES_CFG_FILE_PATH, raw, 4 + blob_len);
    free(raw);
    if (err != ESP_OK) {
        ESP_LOGW(ZCFG_FS_TAG, "zones config file write (rev %lu) failed: %s", (unsigned long)rev,
                 esp_err_to_name(err));
    }
    return err;
}

static bool resolve_with_file_buf(const zones_cfg_t *nvs_cfg, bool nvs_valid, uint32_t nvs_rev, zones_cfg_t *out_cfg,
                                     uint32_t *out_rev, bool *out_used_file, uint8_t *out_on_disk_version,
                                     zones_cfg_t *file_cfg)
{
    if (out_cfg) {
        memset(out_cfg, 0, sizeof(*out_cfg));
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_used_file) {
        *out_used_file = false;
    }
    if (out_on_disk_version) {
        *out_on_disk_version = 0;
    }
    if (!nvs_cfg || !out_cfg || !out_rev || !out_used_file) {
        return false;
    }

    uint32_t file_rev = 0;
    bool file_valid = false;
    uint8_t file_on_disk_version = 0;
    load_raw_impl(file_cfg, &file_rev, &file_valid, &file_on_disk_version);

    if (!file_valid && file_on_disk_version == ZONES_CFG_RESOLVE_OOM_VERSION) {
        /* Could not read/decode the file: unknown, NOT absent. Do not fall back to (and
         * write over the file with) the NVS candidate. Keep the rev floor. */
        memset(out_cfg, 0, sizeof(*out_cfg));
        *out_rev = nvs_rev;
        *out_used_file = false;
        if (out_on_disk_version) {
            *out_on_disk_version = ZONES_CFG_RESOLVE_OOM_VERSION;
        }
        return false;
    }

    if (!file_valid) {
        /* No usable file. Fall back to the NVS candidate, and if it is
         * itself trustworthy, write it out -- this is the lazy, one-item-
         * at-a-time migration the plan calls for: the first successful load
         * after `cfg` becomes available (or after a corrupt file is
         * detected) writes a fresh file, no separate migration task. */
        *out_cfg = *nvs_cfg;
        *out_rev = nvs_rev;
        *out_used_file = false;
        if (nvs_valid) {
            esp_err_t werr = zones_config_cfg_fs_save(nvs_cfg, nvs_rev);
            if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(ZCFG_FS_TAG, "could not migrate NVS zones config to file: %s", esp_err_to_name(werr));
            }
        }
        return nvs_valid;
    }

    if (!nvs_valid) {
        /* File is good, NVS side has nothing trustworthy (fresh board with
         * a pre-populated file, or a refused-newer/corrupt NVS blob) -- use
         * the file outright. Not logged as a divergence: there is nothing
         * on the NVS side to disagree WITH. */
        *out_cfg = *file_cfg;
        *out_rev = file_rev;
        *out_used_file = true;
        if (out_on_disk_version) {
            *out_on_disk_version = file_on_disk_version;
        }
        return true;
    }

    /* Both sides decoded to something valid -- compare content, not just
     * rev, so two independently-arrived-at-identical configs never get
     * logged as a spurious divergence. */
    bool differs = memcmp(file_cfg, nvs_cfg, sizeof(*file_cfg)) != 0;
    if (!differs) {
        *out_cfg = *file_cfg;
        *out_rev = file_rev > nvs_rev ? file_rev : nvs_rev;
        *out_used_file = true;
        if (out_on_disk_version) {
            *out_on_disk_version = file_on_disk_version;
        }
        return true;
    }

    /* DIVERGENCE TIE-BREAK: the file wins only on a STRICTLY higher rev.
     * An EQUAL rev with differing content means NVS, and the comparison
     * must be `>` not `>=`. Enumerate how each side can get ahead:
     *
     *  - file strictly ahead: the file write landed and the crash/failure
     *    came before the NVS write. file_rev = N+1, nvs_rev = N. File wins,
     *    correctly, and this branch is what does it.
     *  - NVS strictly ahead: a file write failed after NVS already
     *    advanced. nvs_rev = N+1, file_rev = N. NVS wins in the else.
     *  - EQUAL revs, differing content: this can ONLY happen when NVS was
     *    written by something that does not know about `zones_rev` --
     *    i.e. firmware rolled back past this change (it rewrites
     *    NVS_KEY_ZONES and leaves zones_rev at N), or a crash between
     *    hal_kv_set_blob() and hal_kv_set_u32() in nvs_save(). In BOTH
     *    cases the NVS copy is the NEWER one and the file is stale. Both
     *    dual-write sides always stamp the SAME new rev, so an equal rev
     *    can never mean "the file is the newer of the two".
     *
     * `>=` here silently discarded every edit made on rolled-back firmware
     * and then overwrote it on the next save -- exactly the downgrade
     * hazard docs/FILESYSTEM_USER_DATA.md section 4 introduced the rev
     * counter to close. Fixed 2026-09-07
     * (docs/audits/filesystem_migration_review_2026-09-07.md), pinned by
     * check_cfg_fs_tie_break.ps1 and by
     * test_zones_config_cfg_fs.c's equal-rev case.
     *
     * Either way, log it and resync the loser so the disagreement does not
     * persist across boots. */
    if (file_rev > nvs_rev) {
        ESP_LOGW(ZCFG_FS_TAG,
                 "zones config file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting FILE (strictly higher rev)",
                 (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_cfg = *file_cfg;
        *out_rev = file_rev;
        *out_used_file = true;
        if (out_on_disk_version) {
            *out_on_disk_version = file_on_disk_version;
        }
        /* NVS resync happens on the next nvs_save() call driven by the
         * caller (zones_config_store.c's nvs_load() bumps its own rev and
         * re-saves both sides once it adopts this result) -- this module
         * does not write NVS directly, only the file (see header comment:
         * "this file only decides WHICH bytes win... it does not touch NVS
         * itself"). */
    } else {
        ESP_LOGW(ZCFG_FS_TAG,
                 "zones config file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting NVS (higher rev), "
                 "resyncing file",
                 (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_cfg = *nvs_cfg;
        *out_rev = nvs_rev;
        *out_used_file = false;
        esp_err_t werr = zones_config_cfg_fs_save(nvs_cfg, nvs_rev);
        if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(ZCFG_FS_TAG, "could not resync zones config file from NVS: %s", esp_err_to_name(werr));
        }
    }
    return true;
}

/* The decoded file candidate is a whole zones_cfg_t (~1 KB); it lives on the
 * heap, not this frame, because this runs on the shared 8 KB httpd stack via
 * profile_exec_start_post_handler -> ... -> nvs_load (check_httpd_task_stack_budget).
 * OOM is reported like any unresolvable load (false, out_cfg zeroed). */
bool zones_config_cfg_fs_resolve(const zones_cfg_t *nvs_cfg, bool nvs_valid, uint32_t nvs_rev, zones_cfg_t *out_cfg,
                                  uint32_t *out_rev, bool *out_used_file, uint8_t *out_on_disk_version)
{
    zones_cfg_t *file_cfg = (zones_cfg_t *)persist_scratch_alloc(sizeof(*file_cfg));
    if (!file_cfg) {
        if (out_cfg) {
            memset(out_cfg, 0, sizeof(*out_cfg));
        }
        /* Never report a rev below nvs_rev (every other false return keeps it):
         * a caller that adopted rev 0 would let the next save write the file at
         * rev 1 over a newer authoritative file. The sentinel version tells
         * nvs_load() this was OOM, not an untrusted load. */
        if (out_rev) {
            *out_rev = nvs_rev;
        }
        if (out_used_file) {
            *out_used_file = false;
        }
        if (out_on_disk_version) {
            *out_on_disk_version = ZONES_CFG_RESOLVE_OOM_VERSION;
        }
        ESP_LOGE(ZCFG_FS_TAG, "zones config resolve: out of memory");
        return false;
    }
    bool ok = resolve_with_file_buf(nvs_cfg, nvs_valid, nvs_rev, out_cfg, out_rev, out_used_file, out_on_disk_version,
                                    file_cfg);
    free(file_cfg);
    return ok;
}
