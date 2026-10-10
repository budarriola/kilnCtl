#include "zones_http_internal.h"
#include "persist_scratch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
/* nvs_flash.h kept for NVS_DEFAULT_PART_NAME only -- see wifi_prov_nvs.c's
 * identical comment; every actual nvs_*() call in this file below now goes
 * through hal_kv_*() instead. */
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cfg_save_lock.h" /* s_zcfg_save_mutex */
#include "freertos/task.h" /* xTaskGetCurrentTaskHandle() -- autosave dispatcher identity, 2026-09-16 */
#include "kiln_cfg_swap.h" /* kiln_cfg_swap_zone_edits_at_risk() -- review 12 LOW-1 */
#include "kiln_cfg_store.h" /* kiln_cfg_store_autosave_from_live() -- docs/KILN_PROFILES_PLAN.md
                             * section 2.4, item 13. */
#include "kiln_io.h"
#include "relay_cycles.h" /* RELAY_LIFE_BUDGET.md: relay_cycles_set_type() push
                            * on load, zones_config_push_relay_type()/_push_all_relay_types()
                            * below. */
#include "cfg_fs.h"
#include "zones_config_cfg_fs.h" /* docs/FILESYSTEM_USER_DATA.md section 5 step 5:
                            * read-through/dual-write bridge to the `cfg` LittleFS
                            * partition -- see that header for the full design. */
#include "cfg_fs_status.h" /* cfg_fs_status_item_diverged() -- zone_normals_get_dualwrite_status() */
#include "pref_cfg_fs.h" /* item 3 (relay names), section 5 step 5 close-out: relay
                            * names is a small fixed-size struct (69 bytes) with no
                            * migration chain of its own -- unlike the zones blob it
                            * does not need a bespoke bridge, it reuses the SAME
                            * generic module unit_pref.c/ramp_assist_cfg.c/
                            * display_power_cfg.c already share (see pref_cfg_fs.h's
                            * "WHY GENERIC"). PREF_CFG_FS_MAX_ITEM was raised from 32
                            * to 128 bytes to fit it -- see that constant's comment. */

/* Separate tiny NVS key for the dual-write rev counter, deliberately NOT a
 * field inside zones_cfg_t: that struct is already close to
 * ZONES_CONFIG_BLOB_MAX_SIZE (see NVS_KEY_RELAY_NAMES's own comment on why
 * relay names got their own key for the identical reason) and a rev counter
 * has nothing to do with a zone's own thermal record. Read once at
 * nvs_load() time, bumped and rewritten on every nvs_save(). */
#define NVS_KEY_ZONES_REV "zones_rev"
NVS_KEY_LEN_CHECK(NVS_KEY_ZONES_REV);

static uint32_t s_zones_cfg_rev = 0;

/* SAVE MUTEX (dev review 5 L5, modeled on profiles_http.c's profiles_save_lock()).
 * nvs_save(), relay_names_save() and zone_normals_save_locked() each compute rev+1 from a
 * file-scope counter, write the cfg file, then publish the counter; callers span httpd,
 * the profile executor and adaptive tune, so two concurrent saves could both stamp the
 * same rev. One static mutex makes rev read + write + rev bump one critical section per
 * store (a single mutex covers all three: they are short and never nest).
 * LOCK ORDER: s_zcfg_save_mutex is the OUTER lock; zones_cfg_lock() (portMUX critical
 * section, copy-only) is taken INSIDE it and never the other way round -- no code may
 * call these savers, or take the save mutex, while holding zones_cfg_lock().
 * Held across the module's own cfg-file I/O only, never across a producer call
 * (the kiln-config autosave dispatch in nvs_save() runs after the unlock). Created on
 * first use under a claim flag so every take sees a non-NULL handle.
 * cfg_save_lock_t since 2026-10-09: the take also reserves the flash worker when the
 * caller is not the worker -- these savers run as worker jobs too (CONTROL
 * SET_ZONE_PID/SET_ZONE_MODEL, AUTOTUNE ACCEPT) while their own cfg write dispatches
 * onto the worker; see cfg_save_lock.h. */
static cfg_save_lock_t s_zcfg_save_mutex = CFG_SAVE_LOCK_INIT;

static void zcfg_save_lock(void) { cfg_save_lock_take(&s_zcfg_save_mutex); }

static void zcfg_save_unlock(void) { cfg_save_lock_give(&s_zcfg_save_mutex); }

/* For the relay-name setters in zones_config_accessors.c (CFG_STORE_SAVE_RACE
 * audit MED-1): RAM edit and save in one section under the same mutex. */
void zones_cfg_save_section_lock(void) { zcfg_save_lock(); }

void zones_cfg_save_section_unlock(void) { zcfg_save_unlock(); }

/* CLAUDE.md's ota_rollback_esp() hazard, closed 2026-09-16 -- see
 * zones_cfg_load_fault_t's own doc comment (zones_config_accessors.h) for
 * the full rule. Latched below, in nvs_load_from_decode(), the one place a
 * real on-flash blob's decode outcome is known; read back only through
 * zones_config_get_load_fault(). Scoped to KILN_NVS_PARTITION only (the
 * authoritative store nvs_load() reads) -- migrate_from_default_partition()'s
 * probe of the pre-split legacy partition below must not latch this: that
 * path already has its own found/valid handling and a failure there is a
 * separate, older concern, not "this boot is running on defaults instead of
 * its own tuned config." */
static zones_cfg_load_fault_t s_zones_cfg_load_fault = {0};

void zones_config_load_fault_reset_for_test(void)
{
    memset(&s_zones_cfg_load_fault, 0, sizeof(s_zones_cfg_load_fault));
}

void zones_config_load_fault_clear(void)
{
    memset(&s_zones_cfg_load_fault, 0, sizeof(s_zones_cfg_load_fault));
}

bool zones_config_get_load_fault(zones_cfg_load_fault_t *out)
{
    if (out) {
        *out = s_zones_cfg_load_fault;
    }
    return s_zones_cfg_load_fault.occurred;
}

static void zones_cfg_load_fault_latch(zones_cfg_load_fault_kind_t kind, uint8_t on_disk_version,
                                        const char *reason)
{
    s_zones_cfg_load_fault.occurred = true;
    s_zones_cfg_load_fault.kind = kind;
    s_zones_cfg_load_fault.on_disk_version = on_disk_version;
    s_zones_cfg_load_fault.fw_version = ZONES_CFG_VERSION;
    snprintf(s_zones_cfg_load_fault.reason, sizeof(s_zones_cfg_load_fault.reason), "%s", reason ? reason : "");
}

/* M13 fix (2026-09-16): see zones_cfg_migration_persist_fault_t's doc
 * comment in zones_config_accessors.h. Latched only from
 * zones_config_persist_migrated_blob_verified()'s give-up path below --
 * never cleared mid-boot, same convention as s_zones_cfg_load_fault above. */
static zones_cfg_migration_persist_fault_t s_zones_cfg_migration_persist_fault = {0};

bool zones_config_get_migration_persist_fault(zones_cfg_migration_persist_fault_t *out)
{
    if (out) {
        *out = s_zones_cfg_migration_persist_fault;
    }
    return s_zones_cfg_migration_persist_fault.occurred;
}

static uint32_t zones_cfg_rev_load(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return 0;
    }
    uint32_t rev = 0;
    err = hal_kv_get_u32(&h, NVS_KEY_ZONES_REV, &rev);
    hal_kv_close(&h);
    return err == HAL_OK ? rev : 0;
}

/* ---- NVS ---------------------------------------------------------------- */

/* Brings up one NVS partition, erasing ONLY that partition if its contents
 * are unusable. Copied/adapted from wifi_prov.c's nvs_partition_init() (see
 * that file for the full rationale) -- NO_FREE_PAGES / NEW_VERSION_FOUND have
 * no other cure, so erasing is the only way forward, but the erase must stay
 * scoped to the partition that is actually broken rather than blast-radius
 * the rest of kiln_nvs (or, worse, the default partition) with it. */
esp_err_t nvs_partition_init(const char *partition)
{
    return hal_status_to_esp_err(hal_kv_init_partition(partition));
}

/* Reads NVS_NAMESPACE/NVS_KEY_ZONES out of `partition` into *out_cfg, applying
 * the three-outcome version handling nvs_load() relies on, via the shared
 * zones_config_json_decode_blob() decoder (length check first, typed per-version
 * conversion, zones_config_json_validate(), CRC on the current-version path). *out_found
 * reports whether the key held something WORTH NOT DISTURBING -- true for a
 * decoded-and-trustworthy blob (current or migrated-older) AND for a blob
 * refused as newer-than-firmware (a real config this build must not clobber,
 * even though it can't use it), false for genuine corruption (too short,
 * wrong length for its claimed version, bad CRC, or failed validation) where
 * there is nothing being protected and a caller is free to look elsewhere.
 * This is what the one-time migration below keys off. */
/* out_valid, if non-NULL, reports whether *out_cfg is a real decoded config
 * that later stages (zones_http_start(), zones_config_is_valid()) may treat
 * as trustworthy -- see s_zones_config_valid's comment for the exact rule.
 * Distinct from *out_found: found means "there is real data here that must
 * not be overwritten by a migration," valid means "and it's actually usable
 * to run a kiln against right now." A newer-refuses-to-load blob is found
 * but not valid; a migrated older blob is both; genuine corruption is
 * neither. */
static esp_err_t nvs_load_from_decode(const char *partition, zones_cfg_t *out_cfg, bool *out_found,
                                       bool *out_valid, bool *out_migrated, uint8_t *out_on_disk_version,
                                       bool *out_refused_newer);
static esp_err_t nvs_load_from_with_migration_info(const char *partition, zones_cfg_t *out_cfg, bool *out_found,
                                                     bool *out_valid, bool *out_migrated,
                                                     uint8_t *out_on_disk_version, bool *out_refused_newer);

/* Thin wrapper around the real load below, added 2026-09-09 (opus review
 * defect D). Every path through nvs_load_from_decode() that does NOT end in
 * a decoded, valid config leaves *out_cfg zero-initialised (blank NVS, an
 * unreadable blob, a refused newer-than-firmware blob, genuine corruption --
 * see the switch below and this function's own entry memset). Zero is the
 * wrong default for model_fit_temp_c/model_fit_ambient_c specifically: 0
 * degC is a plausible genuine ambient, so a virgin board reported every zone
 * as "fitted at 0 C" -- the exact conflation zone_cfg_t's comment says the
 * sentinel exists to prevent, and one zones_config_json_validate() cannot
 * catch (0.0 is inside its accepted range). The migration branch already
 * backfilled the sentinel; this covers the other half. Applied here, once,
 * around every return path rather than at each of them, so a future early
 * return cannot forget it. Nothing else about the defaults changes, and
 * neither the struct layout nor ZONES_CFG_VERSION moves. */
/* Signature deliberately unchanged (4 params) -- test_zones_http.c reaches
 * this `static` function directly by including this translation unit,
 * roughly 50 call sites, so this is not free to grow a parameter list. The
 * migration-write-back fix below needs the out_migrated/out_on_disk_version
 * outputs too, but only nvs_load() needs them -- it calls
 * nvs_load_from_with_migration_info() (right below) instead of this one. */
static esp_err_t nvs_load_from(const char *partition, zones_cfg_t *out_cfg, bool *out_found, bool *out_valid)
{
    bool migrated_unused = false;
    uint8_t on_disk_version_unused = 0;
    bool refused_newer_unused = false;
    return nvs_load_from_with_migration_info(partition, out_cfg, out_found, out_valid, &migrated_unused,
                                              &on_disk_version_unused, &refused_newer_unused);
}

/* Same as nvs_load_from() above, plus the migration-write-back fix's two
 * extra outputs. Free to take whatever shape is convenient since its only
 * caller is nvs_load() itself, below. */
static esp_err_t nvs_load_from_with_migration_info(const char *partition, zones_cfg_t *out_cfg, bool *out_found,
                                                     bool *out_valid, bool *out_migrated,
                                                     uint8_t *out_on_disk_version, bool *out_refused_newer)
{
    bool valid = false;
    bool migrated = false;
    uint8_t on_disk_version = 0;
    esp_err_t err = nvs_load_from_decode(partition, out_cfg, out_found, &valid, &migrated, &on_disk_version,
                                          out_refused_newer);
    if (!valid) {
        zones_config_json_apply_model_fit_defaults(out_cfg);
    }
    if (out_valid) {
        *out_valid = valid;
    }
    if (out_migrated) {
        *out_migrated = migrated;
    }
    if (out_on_disk_version) {
        *out_on_disk_version = on_disk_version;
    }
    return err;
}

/* out_migrated reports whether the blob just decoded was written to flash by
 * an OLDER version than ZONES_CFG_VERSION and had to go through
 * convert_versioned_blob_to_current() to be usable -- true only alongside a
 * ZONES_DECODE_OK outcome, since a NEWER or CORRUPT outcome never adopts
 * anything. This is what the "persist the migration back to flash" fix below
 * (2026-09-16, single-migration-step hazard) keys off: without it, a config
 * migrated in RAM this boot is never written back, so a firmware that only
 * carries the single step from its immediate predecessor loses the config
 * entirely if two schema-bumping firmwares are installed in a row without an
 * intervening boot on the first one (see nvs_load()'s call site). */
static esp_err_t nvs_load_from_decode_buf(const char *partition, zones_cfg_t *out_cfg, bool *out_found,
                                           bool *out_valid, bool *out_migrated, uint8_t *out_on_disk_version,
                                           bool *out_refused_newer, uint8_t *raw)
{
    if (out_refused_newer) {
        *out_refused_newer = false;
    }
    if (out_found) {
        *out_found = false;
    }
    if (out_valid) {
        *out_valid = false;
    }
    if (out_migrated) {
        *out_migrated = false;
    }
    if (out_on_disk_version) {
        *out_on_disk_version = 0;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, partition);
    if (err == HAL_NOT_FOUND) {
        return ESP_OK; /* namespace never created -- nothing configured, not an error */
    }
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }

    /* Raw byte buffer, not out_cfg directly: zones_config_json_decode_blob() needs the
     * blob's ACTUAL on-disk bytes to run the length-vs-claimed-version check
     * and, for an older version, to reinterpret them through the right
     * historical struct -- not bytes already reshaped by a direct
     * nvs_get_blob() into the CURRENT struct's layout (that reshaping is
     * exactly the bug this pass fixes). Sized to the largest possible
     * on-flash layout, which by construction is the current one (every
     * historical struct above is smaller). */
    memset(raw, 0, sizeof(zones_cfg_t));
    size_t len = sizeof(zones_cfg_t);
    err = hal_kv_get_blob(&h, NVS_KEY_ZONES, raw, &len);
    hal_kv_close(&h);
    if (err == HAL_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != HAL_OK) {
        ESP_LOGW(ZONES_HTTP_TAG, "zones_cfg blob read from '%s' failed (%s) -- treating as unreadable",
                 partition, hal_status_to_name(err));
        return ESP_OK;
    }

    const char *reason = "";
    zones_decode_result_t result = zones_config_json_decode_blob(raw, len, out_cfg, &reason);
    switch (result) {
    case ZONES_DECODE_OK:
        /* LOAD path only -- collapse, never reject, a pre-existing stored
         * cycle. See zones_config_json_normalize_settings_source_cycles()'s own comment for
         * why this runs here and NOT inside zones_config_json_decode_blob() (which
         * zones_config_import_blob() also calls, and that path must reject
         * instead). */
        zones_config_json_normalize_settings_source_cycles(out_cfg, partition);
        if (out_found) {
            *out_found = true;
        }
        if (out_valid) {
            *out_valid = true;
        }
        if (out_on_disk_version) {
            *out_on_disk_version = raw[0];
        }
        if (out_migrated && raw[0] != ZONES_CFG_VERSION) {
            *out_migrated = true;
        }
        ESP_LOGI(ZONES_HTTP_TAG, "zones_cfg from '%s' loaded (on-disk version %u) as v%u", partition,
                 (unsigned)raw[0], (unsigned)ZONES_CFG_VERSION);
        return ESP_OK;
    case ZONES_DECODE_NEWER:
        /* Firmware-rollback case (TODO.md 8.1): leave flash untouched, fall
         * back to defaults for this boot only. *out_found MUST be true --
         * this is real, deliberately-protected data (kiln_nvs genuinely has
         * something), so migrate_from_default_partition() must not treat it
         * as "nothing here" and overwrite it with a stale pre-split copy. */
        ESP_LOGW(ZONES_HTTP_TAG, "zones_cfg from '%s' is version %u, newer than this firmware's %u -- "
                      "refusing to load, flash data left untouched",
                 partition, (unsigned)raw[0], (unsigned)ZONES_CFG_VERSION);
        if (strcmp(partition, KILN_NVS_PARTITION) == 0) {
            zones_cfg_load_fault_latch(ZONES_CFG_LOAD_FAULT_NEWER, raw[0], reason);
        }
        /* Reported separately from the latch above so nvs_load()'s `cfg`-file
         * write-back can tell this case apart from an absent/corrupt NVS
         * side: "found, but deliberately protected" must NOT be overwritten
         * by an older file-sourced config. The latch itself is boot-wide and
         * sticky, so it cannot answer "did THIS decode refuse?". */
        if (out_refused_newer) {
            *out_refused_newer = true;
        }
        if (out_found) {
            *out_found = true;
        }
        return ESP_OK;
    case ZONES_DECODE_OOM:
        /* Not judged: no fault latch, nothing found/valid, the load fails. */
        ESP_LOGW(ZONES_HTTP_TAG, "zones_cfg blob from '%s': out of memory while converting -- load fails, "
                      "flash data left untouched",
                 partition);
        return ESP_ERR_NO_MEM;
    case ZONES_DECODE_CORRUPT:
    default:
        /* Genuine corruption (too short, wrong length for the claimed
         * version, bad CRC, or a decoded config that failed
         * zones_config_json_validate()) -- loud enough that an operator can see it,
         * naming the on-disk version and the specific rejection reason.
         * Nothing worth protecting was found here, so a caller (the
         * legacy-partition migration) is free to look elsewhere. This bucket
         * also covers a version this firmware's migration chain does not
         * reach back far enough to consume (docs/CONFIG_MIGRATION_CHAIN.md's
         * one-step-at-a-time policy) -- there is no separate decode outcome
         * for that case today, so it is reported as UNREADABLE, same as any
         * other undecodable blob. */
        ESP_LOGW(ZONES_HTTP_TAG, "zones_cfg blob from '%s' (on-disk version %u, %u bytes) REJECTED: %s -- "
                      "falling back to defaults, NOT adopting this config",
                 partition, (unsigned)raw[0], (unsigned)len, reason);
        if (strcmp(partition, KILN_NVS_PARTITION) == 0) {
            zones_cfg_load_fault_latch(ZONES_CFG_LOAD_FAULT_UNREADABLE, raw[0], reason);
        }
        return ESP_OK;
    }
}

/* The raw blob buffer is a whole zones_cfg_t (~1 KB); heap, not stack, because
 * this runs on the shared 8 KB httpd stack via profile_exec_start_post_handler
 * -> profile_executor_run -> ... -> nvs_load (check_httpd_task_stack_budget).
 * OOM reads as a failed load (outputs already reset to "nothing found"). */
static esp_err_t nvs_load_from_decode(const char *partition, zones_cfg_t *out_cfg, bool *out_found,
                                       bool *out_valid, bool *out_migrated, uint8_t *out_on_disk_version,
                                       bool *out_refused_newer)
{
    uint8_t *raw = (uint8_t *)persist_scratch_alloc(sizeof(zones_cfg_t));
    if (!raw) {
        if (out_refused_newer) {
            *out_refused_newer = false;
        }
        if (out_found) {
            *out_found = false;
        }
        if (out_valid) {
            *out_valid = false;
        }
        if (out_migrated) {
            *out_migrated = false;
        }
        if (out_on_disk_version) {
            *out_on_disk_version = 0;
        }
        memset(out_cfg, 0, sizeof(*out_cfg));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = nvs_load_from_decode_buf(partition, out_cfg, out_found, out_valid, out_migrated,
                                           out_on_disk_version, out_refused_newer, raw);
    free(raw);
    return e;
}

/* One-time move of the persisted zones config out of the default partition's
 * NVS_NAMESPACE/NVS_KEY_ZONES and into KILN_NVS_PARTITION's, for boards
 * provisioned by firmware predating the 2026-08-13 split. Simplified from
 * wifi_prov.c's migrate_from_default_partition() to a one-directional copy:
 * kiln_nvs is only ever consulted first, so if it already has something there
 * is nothing to migrate and no "which wins" question to answer -- only the
 * old default-partition location could hold pre-migration data. The old copy
 * is deliberately left in place (not deleted), same rationale as
 * wifi_prov.c: it needs to still be there if someone rolls back to
 * pre-split firmware. */
void migrate_from_default_partition(void)
{
    zones_cfg_t from_default;
    bool found_in_default = false;
    bool valid_in_default = false;
    esp_err_t err = nvs_load_from(NVS_DEFAULT_PART_NAME, &from_default, &found_in_default, &valid_in_default);
    if (err != ESP_OK || !found_in_default) {
        return; /* nothing to migrate */
    }
    /* Review 11 MED-1: a zones.json from NEWER firmware (latched by nvs_load) must not be
     * overwritten by an older legacy copy, nor a rejected file with no .bad copy. */
    if (s_zones_cfg_load_fault.occurred &&
        (s_zones_cfg_load_fault.kind == ZONES_CFG_LOAD_FAULT_NEWER || s_zones_cfg_load_fault.bad_copy_failed)) {
        ESP_LOGE(ZONES_HTTP_TAG, "legacy default-partition zones_cfg NOT migrated: it would overwrite a rejected "
                      "zones.json that is newer or has no %s copy", ZONES_CFG_BAD_FILE_PATH);
        return;
    }
    if (!valid_in_default) {
        /* found_in_default is true for two different reasons now (see FIX 1
         * in nvs_load_from()): a refused newer-than-firmware blob, or a
         * genuinely current/older key nvs_get_blob() actually read that
         * turned out corrupt is instead reported found=false above, so the
         * only way to reach here with valid_in_default false is the
         * refused-newer case. Copying it forward would destroy exactly the
         * kind of data this whole refuse-and-leave-untouched discipline
         * exists to protect -- for the SAME reason it must not be
         * overwritten in kiln_nvs, it must not be blindly migrated out of
         * the default partition either. Leave both partitions as they are;
         * this runs again next boot with no data lost either way. */
        ESP_LOGW(ZONES_HTTP_TAG, "zones_cfg in the default NVS partition exists but nvs_load_from() refused it -- "
                      "not migrating it to '%s'", KILN_NVS_PARTITION);
        return;
    }

    ESP_LOGI(ZONES_HTTP_TAG, "migrating zones_cfg from the default NVS partition to '%s'", KILN_NVS_PARTITION);

    s_zones.cfg = from_default;
    /* Reaching here means valid_in_default was true -- nvs_load_from()
     * actually decoded from_default, current version or an older version
     * successfully migrated -- so this is always something ready to run a
     * kiln against, never a corrupt or refused blob (both return above). */
    s_zones_config_valid = valid_in_default;
    /* A legacy copy was adopted: no longer 'no trustworthy copy' (review 11 MED-2). */
    zones_config_load_fault_clear();
    /* RELAY_LIFE_BUDGET.md, "on load": this IS a load into
     * s_zones.cfg, same as nvs_load()'s own -- a board migrating forward
     * from the default partition must not run with relay_cycles.c still
     * holding whatever nvs_load()'s earlier, empty attempt against
     * KILN_NVS_PARTITION pushed (or nothing at all, on a first boot). */
    zones_config_push_all_relay_types();
    esp_err_t save_err = nvs_save();
    if (save_err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "migration write to '%s' failed: %s -- running from the old copy this boot, will retry",
                 KILN_NVS_PARTITION, esp_err_to_name(save_err));
    }
}

/* Persists a just-migrated blob (already in s_zones.cfg, current layout)
 * back to KILN_NVS_PARTITION and reads it back to confirm the write actually
 * landed, rather than trusting nvs_save()'s return code alone -- same shape
 * as boot_guard_mark_healthy()'s read-back-confirmed fix (CLAUDE.md's "boot
 * order beats bounded waits" / write-lies section): an NVS write there once
 * reported HAL_OK while the persisted value never actually changed, and
 * bricked the board into a recovery loop three times before a read-back
 * check closed it. One bounded retry, same as that fix. Called only from
 * nvs_load()'s boot-time load path (never from the flash worker, never from
 * a PSRAM-stacked task -- see this function's caller), so a direct,
 * synchronous nvs_save() here is safe; nvs_save()'s own comment documents
 * why IT must never be called inline from arbitrary httpd/executor callers,
 * which does not apply to this one-time boot-load call site. */
static bool zones_config_persist_migrated_blob_verified(uint8_t on_disk_version_before_migration,
                                                          bool from_cfg_file)
{
    /* `from_cfg_file` names the actual winning source for the log lines
     * below -- nvs_load()'s file-won call site has no single "on-disk NVS
     * version" to report (NVS may have been invalid, or a different,
     * now-overwritten version), so the log text always says "cfg file
     * source" there rather than a version number. `on_disk_version_before_
     * migration` itself is real either way: the NVS-won call site passes
     * the NVS blob's on-disk version, and the file-won call site passes the
     * FILE blob's own on-disk version (from zones_config_cfg_fs_resolve()'s
     * out_on_disk_version). Reviewer advisory a03ead6c: this used to be a
     * hardcoded 0 on the file-won call site, which latched into
     * zones_cfg_migration_persist_fault_t::on_disk_version and told the
     * dashboard/LCD banner "on-disk v0" even when the cfg-file blob carried
     * a real version -- fixed by threading the real value through instead. */
    char source_desc[24];
    if (from_cfg_file) {
        snprintf(source_desc, sizeof(source_desc), "cfg file source");
    } else {
        snprintf(source_desc, sizeof(source_desc), "on-disk v%u", (unsigned)on_disk_version_before_migration);
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        esp_err_t save_err = nvs_save(); /* stamps version=ZONES_CFG_VERSION, a real crc32, and writes it */
        if (save_err != ESP_OK) {
            ESP_LOGW(ZONES_HTTP_TAG, "migrated zones_cfg (was %s) write-back attempt %d failed: %s", source_desc,
                     attempt, esp_err_to_name(save_err));
            continue;
        }
        /* cfg file only since the dual-write close: read the file back (a
         * second, independent read on top of cfg_fs_write_atomic's own). */
        /* Heap/PSRAM scratch, not a static (.dram0.bss is at its budget) and not the stack. */
        zones_cfg_t *readback = persist_scratch_alloc(sizeof(*readback));
        uint32_t rb_rev = 0;
        bool rb_valid = false;
        bool rb_match = false;
        if (readback) {
            memset(readback, 0, sizeof(*readback));
            zones_config_cfg_fs_load_raw(readback, &rb_rev, &rb_valid);
            rb_match = rb_valid && memcmp(readback, &s_zones.cfg, sizeof(s_zones.cfg)) == 0;
            free(readback);
        }
        if (rb_match) {
            ESP_LOGI(ZONES_HTTP_TAG, "migrated zones_cfg (was %s) persisted to the cfg file and verified by "
                          "read-back as v%u, crc32 0x%08x",
                     source_desc,
                     (unsigned)ZONES_CFG_VERSION, (unsigned)s_zones.cfg.crc32);
            return true;
        }
        ESP_LOGW(ZONES_HTTP_TAG, "migrated zones_cfg write-back attempt %d: read-back did not match what was "
                      "just written -- retrying",
                 attempt);
    }
    ESP_LOGE(ZONES_HTTP_TAG, "migrated zones_cfg (was %s) could NOT be verified as persisted to the cfg file "
                  "after retry -- this boot runs on the migrated in-RAM copy, but flash still holds the old "
                  "bytes; a second firmware install one step further (the one-step migration policy) will "
                  "be unable to read them and will treat this config as too old to consume",
             source_desc);
    /* M13 fix (2026-09-16): this used to be an ESP_LOGE only, invisible to
     * the operator -- see zones_cfg_migration_persist_fault_t's doc comment
     * (zones_config_accessors.h). Latch it so dashboard_http.c/LCD can name
     * it, same pattern as s_zones_cfg_load_fault above. */
    s_zones_cfg_migration_persist_fault.occurred = true;
    s_zones_cfg_migration_persist_fault.on_disk_version = on_disk_version_before_migration;
    s_zones_cfg_migration_persist_fault.fw_version = ZONES_CFG_VERSION;
    return false;
}

/* out_found/out_valid are nvs_load_from()'s own outputs, passed straight
 * through -- see FIX 1's history here: zones_http_start() used to reconstruct
 * "was anything found in kiln_nvs" from s_zones.cfg.version != 0 after this
 * call, which cannot tell "genuinely nothing was ever saved" (version reads
 * 0 because nothing was ever written) from "something WAS found, but it was
 * a newer-than-firmware blob nvs_load_from() refused and zeroed" (version
 * also reads 0, because the refusal path memsets out_cfg). Both callers now
 * get the real found/valid flags instead of guessing from the zeroed struct. */
esp_err_t nvs_load(bool *out_found, bool *out_valid)
{
    bool migrated_from_nvs = false;
    uint8_t on_disk_version_before = 0;
    bool nvs_refused_as_newer = false;
    esp_err_t err = nvs_load_from_with_migration_info(KILN_NVS_PARTITION, &s_zones.cfg, out_found, out_valid,
                                                        &migrated_from_nvs, &on_disk_version_before,
                                                        &nvs_refused_as_newer);
    if (err == ESP_ERR_NO_MEM) {
        /* Decode OOM: could not judge the NVS bytes. Do not resolve (the file would
         * be compared against nothing), keep the rev floor, fail the load. */
        s_zones_cfg_rev = zones_cfg_rev_load();
        memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
        if (out_found) {
            *out_found = false;
        }
        if (out_valid) {
            *out_valid = false;
        }
        return err;
    }

    /* docs/FILESYSTEM_USER_DATA.md section 5 step 5: read-through
     * against the `cfg` file on top of whatever nvs_load_from() just
     * decoded. zones_config_cfg_fs_resolve() never touches NVS itself -- it
     * only decides whether the file or the NVS candidate above wins, per
     * its own header's tie-break rule, and may write a resync copy to
     * whichever side lost. On every board today (no `cfg` partition) this
     * is a fast no-op that hands the NVS candidate straight back
     * unchanged -- see test_zones_config_cfg_fs.c's "partition absent"
     * case. */
    bool nvs_valid = out_valid ? *out_valid : false;
    uint32_t nvs_rev = zones_cfg_rev_load();
    /* Heap, not stack (~1 KB): shared httpd stack, see check_httpd_task_stack_budget.
     * OOM is a clean load failure: config zeroed, nothing found/valid. */
    zones_cfg_t *resolved = persist_scratch_alloc(sizeof(*resolved));
    if (!resolved) {
        s_zones_cfg_rev = nvs_rev; /* rev floor: a later save must not restamp rev 1 */
        memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
        if (out_found) {
            *out_found = false;
        }
        if (out_valid) {
            *out_valid = false;
        }
        return ESP_ERR_NO_MEM;
    }
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    uint8_t file_on_disk_version = 0;
    bool trustworthy = zones_config_cfg_fs_resolve(&s_zones.cfg, nvs_valid, nvs_rev, resolved, &resolved_rev,
                                                    &used_file, &file_on_disk_version);
    zones_cfg_fs_reject_t file_reject;
    bool file_rejected = zones_config_cfg_fs_get_last_reject(&file_reject);
    /* Review 11 MED-2: latch only when NO trustworthy copy was adopted. A rejected file that fell back
     * to a trustworthy NVS copy is a logged warning, not a firing-refusing fault. (A later legacy
     * migration that adopts a copy clears the latch again.) */
    if (file_rejected && !trustworthy && !s_zones_cfg_load_fault.occurred) {
        zones_cfg_load_fault_latch(file_reject.newer ? ZONES_CFG_LOAD_FAULT_NEWER : ZONES_CFG_LOAD_FAULT_UNREADABLE,
                                   file_reject.on_disk_version, file_reject.reason);
        s_zones_cfg_load_fault.file_rejected = true;
        s_zones_cfg_load_fault.bad_copy_failed = !file_reject.preserved;
    } else if (file_rejected && trustworthy) {
        ESP_LOGW(ZONES_HTTP_TAG, "zones config: cfg file %s REJECTED (%s) but a trustworthy %s copy was adopted; "
                      "rejected file kept as %s: %s",
                 ZONES_CFG_FILE_PATH, file_reject.reason, used_file ? "file" : "NVS", ZONES_CFG_BAD_FILE_PATH,
                 file_reject.preserved ? "yes" : "NO (copy failed)");
    }
    if (!trustworthy && nvs_valid && file_rejected && file_reject.newer) {
        /* The NVS copy was valid but deliberately NOT adopted over a newer-version file. */
        memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
        if (out_valid) {
            *out_valid = false;
        }
    }
    if (!trustworthy && !used_file && file_on_disk_version != ZONES_CFG_RESOLVE_OOM_VERSION) {
        /* Audit (c) fix 4: name the path in the boot log. No copy adopted YET: a legacy default-partition
         * copy may still be adopted later this boot (migrate_from_default_partition). */
        if (file_rejected) {
            ESP_LOGE(ZONES_HTTP_TAG, "zones config UNTRUSTWORTHY: cfg file %s REJECTED (%s), NVS blob %s -- no "
                          "trustworthy copy adopted (config zeroed unless a legacy copy is adopted later this "
                          "boot); the rejected file is kept as %s",
                     ZONES_CFG_FILE_PATH, file_reject.reason, nvs_valid ? "usable but not adopted" : "absent/invalid",
                     ZONES_CFG_BAD_FILE_PATH);
        } else {
            ESP_LOGE(ZONES_HTTP_TAG, "zones config UNTRUSTWORTHY: cfg file %s %s and NVS blob absent/invalid -- "
                          "no trustworthy copy adopted",
                     ZONES_CFG_FILE_PATH,
                     cfg_fs_is_available() ? "missing/unreadable" : "unavailable (cfg not mounted)");
        }
    }
    if (file_on_disk_version == ZONES_CFG_RESOLVE_OOM_VERSION && !trustworthy && !used_file) {
        /* Resolve could not allocate: keep the rev floor and fail the load so the
         * legacy NVS copy is not adopted as valid (a later save would overwrite
         * the newer authoritative file). */
        s_zones_cfg_rev = nvs_rev;
        free(resolved);
        memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
        if (out_found) {
            *out_found = false;
        }
        if (out_valid) {
            *out_valid = false;
        }
        return ESP_ERR_NO_MEM;
    }
    s_zones_cfg_rev = resolved_rev;
    /* cfg is MOUNTED on the bench board as of 2026-09-21 (7 files, confirmed
     * via GET /api/cfgfs -- CLAUDE.md's "512K LittleFS cfg partition"), so
     * used_file below is reachable in practice now, not the theoretical case
     * the comment here used to describe. zones_config_cfg_fs_resolve() (and
     * zones_config_json_decode_blob() underneath it) migrates an old-version
     * `cfg` file exactly like it migrates an old NVS blob, entirely in RAM --
     * nothing on the file-read path writes anything back. Compare against
     * the NVS candidate BEFORE it is overwritten below: `s_zones.cfg` still
     * holds whatever nvs_load_from_with_migration_info() just decoded (or
     * the zeroed default, if !nvs_valid), and `resolved` is a struct this
     * function already owns on the heap (persist_scratch) -- no large local. If
     * the two don't already agree, NVS (and, redundantly but harmlessly,
     * the file itself, since zones_config_cfg_fs_save() re-stamps a fresh
     * CRC) needs a write-back once this boot's winner is adopted, same as
     * the migrated-from-NVS case below -- otherwise a from-file schema
     * migration would diverge the two stores across the very next boot. */
    bool file_side_needs_writeback = false;
    if (used_file) {
        /* NEVER when the NVS side was FOUND but refused as newer-than-this-
         * firmware (ZONES_DECODE_NEWER): that branch's whole contract is
         * "flash data left untouched" -- it is real, deliberately-protected
         * data written by a firmware ahead of this one, and writing an older
         * file-sourced config over it destroys it permanently. That is the
         * same downgrade hazard the `>` (not `>=`) tie-break in
         * zones_config_cfg_fs_resolve() exists to close, and it is reachable
         * asymmetrically: nvs_save() writes the FILE FIRST and swallows its
         * error, so a newer firmware can leave NVS at vN+1 while the file
         * still holds a valid, older vN. The file still wins THIS boot's
         * in-RAM config, exactly as before -- only the write-back is
         * suppressed. A found-but-CORRUPT NVS side is NOT protected and is
         * still written back: there is nothing there worth keeping. */
        /* The cfg file is the only store a save writes now (NVS is legacy,
         * read-only), so a missing or different NVS copy is no reason to write
         * anything: doing so would rewrite the file on EVERY boot of a
         * post-close board, whose NVS blob is permanently absent or stale. Only
         * an old-schema file that was migrated in RAM needs the rewrite. */
        /* A file whose settings_source cycle was collapsed by load-time normalization
         * is no longer self-consistent (normalization does not re-stamp crc32), so
         * that also needs the rewrite, once. */
        file_side_needs_writeback = !nvs_refused_as_newer &&
                                    (file_on_disk_version != ZONES_CFG_VERSION ||
                                     resolved->crc32 != zones_config_json_compute_crc(resolved));
        migrated_from_nvs = false;
        s_zones.cfg = *resolved;
        if (out_found) {
            *out_found = true;
        }
        if (out_valid) {
            *out_valid = true;
        }
    }
    free(resolved);

    if (err == ESP_OK && trustworthy) {
        /* RELAY_LIFE_BUDGET.md, "on load": s_zones.cfg is now
         * whatever this boot is actually going to run with (a decoded
         * current/migrated blob, or the zero-initialized defaults
         * nvs_load_from() leaves in place on a refused/corrupt load) --
         * either way its zones[].relay_type is the type relay_cycles.c
         * should be counting against from here on. Gated on out_valid so a
         * refused newer-than-firmware blob (found but not valid,
         * s_zones.cfg zeroed) does not push a fabricated all-SSR answer over
         * whatever relay_cycles.c already has persisted from a prior boot --
         * an unusable config load leaves the existing types alone rather
         * than resetting them to the zeroed struct's default. */
        zones_config_push_all_relay_types();
    }

    /* Single-migration-step hazard fix (2026-09-16, coordinator finding):
     * migration above happens only in RAM (s_zones.cfg) -- nothing on the
     * ordinary load path wrote it back to flash before this fix. Under the
     * one-step-per-firmware migration policy (docs/CONFIG_MIGRATION_CHAIN_
     * PLAN.md), an operator who installs vN+1, boots it, then installs vN+2
     * would find vN+2 unable to consume the still-on-flash vN bytes, even
     * though every step they took was correct -- a failure that punishes
     * doing the right thing. Persist it now, once, verified by read-back
     * (never trust nvs_save()'s return code alone -- see
     * zones_config_persist_migrated_blob_verified()'s comment). Gated on
     * `err == ESP_OK && trustworthy` so this never fires for a refused/
     * corrupt/zeroed load. A failed persist does not fail this boot's load
     * (the migrated in-RAM copy is still usable this boot, same as
     * migrate_from_default_partition()'s own error handling below) -- it is
     * logged loudly and will simply retry on the next boot that reaches
     * this migration branch again. Reused verbatim for the file-won case
     * (`file_side_needs_writeback`) above: zones_config_persist_migrated_
     * blob_verified() only stamps/writes/read-back-verifies whatever is
     * currently in `s_zones.cfg` against NVS -- it does not care which
     * source produced it, and nvs_save() underneath it already dual-writes
     * the `cfg` file first (zones_config_cfg_fs_save()), so one call closes
     * both sides. `on_disk_version_before` is only used for the log line in
     * the NVS-won case; the file-won case has no single "on-disk NVS
     * version" to name (NVS may have been invalid, or a different, now-
     * overwritten version), so it passes from_cfg_file=true and the log
     * names "cfg file source" instead of any version number there. The
     * VERSION argument passed here for the file-won case is
     * `file_on_disk_version` -- the file blob's own claimed version byte,
     * from zones_config_cfg_fs_resolve()'s matching out param above, not a
     * hardcoded 0 -- so the latched
     * zones_cfg_migration_persist_fault_t::on_disk_version field reports the
     * real source version to the dashboard/LCD banner instead of a
     * meaningless "v0" (reviewer advisory, a03ead6c: the fault struct has no
     * separate "came from the file" encoding, but the version number itself
     * is no longer fabricated). */
    if (err == ESP_OK && trustworthy && migrated_from_nvs) {
        (void)zones_config_persist_migrated_blob_verified(on_disk_version_before, false);
    } else if (err == ESP_OK && trustworthy && file_side_needs_writeback) {
        ESP_LOGI(ZONES_HTTP_TAG, "zones_cfg: `cfg` file source won this boot's load and diverged from NVS -- "
                      "writing the resolved config back to both NVS and the file");
        (void)zones_config_persist_migrated_blob_verified(file_on_disk_version, true);
    }
    return err;
}

/* Hand-declared, same convention relay_cycles.c/safety_cfg_store.c/
 * factory_reset.c/diagnostics_http.c already use (see flash_worker.h's own
 * doc comment) -- avoids pulling in the whole UART bridge API for one call.
 */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);
bool uart_bridge_ext_is_on_flash_worker(void);

/* Job body for nvs_save()'s auto-save dispatch, immediately below --
 * `arg` is unused (kiln_cfg_store_autosave_from_live() reads the live
 * config itself; there is nothing to pass in), and any failure is logged
 * here rather than surfaced through uart_bridge_ext_run_on_flash_worker()'s
 * own ESP_OK/fail return, which only reports whether the job was
 * DISPATCHED, not whether the job itself succeeded. */
static void zones_autosave_job(void *arg)
{
    /* `arg` carries the DISPATCHING TASK's handle, not a pointer -- 2026-09-16
     * cross-task autosave-override fix, see kiln_cfg_store.h's doc comment on
     * kiln_cfg_store_autosave_from_live_for_dispatcher(). It is a handle
     * VALUE, so it is safe on both dispatch branches: the awaited one (the
     * caller is blocked anyway) and the on-worker INLINE one (nothing is
     * queued at all). Passing it is what lets an in-flight import's OWN
     * autosave honor the target override while an unrelated task's
     * concurrent zones write -- the PC control bridge's SET_ZONE_PID, which
     * reaches this very function from bx_flash_worker -- does not, and is
     * still saved into the genuinely active slot rather than dropped. */
    char reason[96];
    reason[0] = '\0';
    if (!kiln_cfg_store_autosave_from_live_for_dispatcher(arg, reason, sizeof(reason))) {
        ESP_LOGW(ZONES_HTTP_TAG, "kiln config autosave failed: %s -- the active kiln package was NOT "
                      "updated with this change, though the change itself was saved",
                 reason[0] ? reason : "(no reason given)");
    }
}

bool zones_config_persisted_equals_ram(void)
{
    /* Re-reads the cfg FILE, the only place nvs_save() writes since the
     * dual-write close (docs/CONFIG_FILESYSTEM.md, "Dual-write window:
     * closed"); the NVS blob is a frozen legacy copy no save updates. */
    zones_cfg_t *raw = persist_scratch_alloc(sizeof(*raw));
    zones_cfg_t *snap = persist_scratch_alloc(sizeof(*snap));
    if (raw == NULL || snap == NULL) {
        free(raw);
        free(snap);
        return false;
    }
    memset(raw, 0, sizeof(*raw));
    /* Snapshot RAM under the zones lock (copy only), then stamp the copy exactly as
     * nvs_save() stamps what it writes, so the compare is against what a save would
     * have put in the file, not a live struct a writer can be mid-edit on. */
    zones_cfg_lock();
    memcpy(snap, &s_zones.cfg, sizeof(*snap));
    zones_cfg_unlock();
    snap->version = ZONES_CFG_VERSION;
    snap->crc32 = zones_config_json_compute_crc(snap);
    uint32_t rev = 0;
    bool valid = false;
    zones_config_cfg_fs_load_raw(raw, &rev, &valid);
    bool ok = valid && memcmp(raw, snap, sizeof(*snap)) == 0;
    free(raw);
    free(snap);
    return ok;
}

esp_err_t nvs_save(void)
{
    /* Review 12 LOW-1: ONE shared gate for every zones writer (POST /api/zones, /pid, adaptive tune, autotune
     * finalize, UART bridge, accessors, backup import ...). While a kept rollback journal would re-import or
     * fault over a zones edit at the next boot, refuse the save; HTTP writers map this to 409. */
    if (kiln_cfg_swap_zone_edits_at_risk()) {
        ESP_LOGW(ZONES_HTTP_TAG, "zones config NOT saved: a kiln-config rollback journal is pending");
        return ESP_ERR_INVALID_STATE;
    }
    /* Persist a SNAPSHOT, never the live struct: copy s_zones.cfg and read the rev under
     * zones_cfg_lock() (portMUX: copy only, no I/O/alloc/log inside), stamp version/CRC
     * on the copy (last, after every other field is final -- see
     * zones_config_json_compute_crc()/zones_cfg_t::crc32's comments), and write the copy.
     * Every save re-stamps; there is no path that writes the blob without it. */
    zones_cfg_t *snap = persist_scratch_alloc(sizeof(*snap));
    if (snap == NULL) {
        ESP_LOGE(ZONES_HTTP_TAG, "zones config NOT persisted: no memory for the snapshot");
        return ESP_ERR_NO_MEM;
    }
    zcfg_save_lock(); /* outer lock; rev read .. rev bump below; see s_zcfg_save_mutex */
    if (cfg_save_lock_reset_refused()) { /* factory reset in flight (pref_cfg_fs.h writer fence) */
        zcfg_save_unlock();
        free(snap);
        return ESP_ERR_INVALID_STATE;
    }
    zones_cfg_lock();
    s_zones.cfg.version = ZONES_CFG_VERSION;
    memcpy(snap, &s_zones.cfg, sizeof(*snap));
    uint32_t new_zones_rev = s_zones_cfg_rev + 1;
    zones_cfg_unlock();
    snap->crc32 = zones_config_json_compute_crc(snap);
    /* Mirror the stamp into RAM (one scalar) ONLY if RAM still equals the snapshot (L5): a setter
     * that edited s_zones.cfg since the snapshot would otherwise be left with new fields under
     * this snapshot's CRC. On a mismatch RAM keeps its own (older) CRC and that setter's own
     * save stamps it. Compare is memcmp with the snapshot's crc field temporarily set to RAM's. */
    zones_cfg_lock();
    {
        uint32_t stamped = snap->crc32;
        snap->crc32 = s_zones.cfg.crc32;
        bool unchanged = memcmp(snap, &s_zones.cfg, sizeof(*snap)) == 0;
        snap->crc32 = stamped;
        if (unchanged) {
            s_zones.cfg.crc32 = stamped;
        }
    }
    zones_cfg_unlock();

    esp_err_t err = zones_config_cfg_fs_save(snap, new_zones_rev);
    free(snap);
    if (err == ESP_OK) {
        zones_cfg_lock();
        s_zones_cfg_rev = new_zones_rev;
        zones_cfg_unlock();
        zcfg_save_unlock();
    } else {
        zcfg_save_unlock();
        ESP_LOGE(ZONES_HTTP_TAG, "zones config NOT persisted: %s -- NVS is no longer written, the change "
                                 "lives in RAM until reboot",
                 esp_err_to_name(err));
    }

    /* docs/KILN_PROFILES_PLAN.md section 2.4 (2026-09-14 "finish upload/
     * download" follow-up, item 13 -- auto-save): every zones-config write
     * that reaches here is, by construction, "the user made a change" --
     * this is the SINGLE choke point every setter/POST-commit/import
     * already funnels through (see this function's own header comment).
     *
     * DISPATCHED onto the flash-safe worker via zones_autosave_job() below,
     * NEVER called directly from here -- kiln_cfg_store_save_current()'s own
     * stack frame is ~2.8 KB (its scratch[ZONES_CONFIG_BLOB_MAX_SIZE] +
     * zones_cfg_t locals), and this function (nvs_save()) is reachable from
     * dozens of callers across the whole zones/adaptive-tune/profile-
     * executor surface -- calling it inline here would add that ~2.8 KB to
     * EVERY one of those callers' own worst-case stack depth, which is
     * exactly how check_executor_task_stack_budget.ps1/check_httpd_task_
     * stack_budget.ps1 caught this on the first attempt (revert_post_handler
     * and executor_task_entry both went over budget with a direct call
     * here). Dispatching through a function pointer (zones_autosave_job)
     * moves that frame onto the flash worker's OWN dedicated stack instead
     * -- invisible to every caller's static stack-depth measurement, the
     * same reason relay_cycles.c's own persist path uses this pattern.
     *
     * nvs_save() IS reached on the flash worker: CONTROL SET_ZONE_PID/MODEL
     * and AUTOTUNE_CMD_ACCEPT run as worker jobs (corrected 2026-10-09; an
     * earlier comment here said it never was). On the worker the job runs
     * inline (the worker's own stack is the one the dispatch targets anyway),
     * but ONLY through a volatile function pointer, never a direct call: the
     * static stack analyser counts a direct call on every path, worker or
     * not, and the direct call 4271767d added put the ~2.8 KB autosave frame
     * back into executor_task_entry's worst case (2816 B against its 1936 B
     * ceiling, check_executor_task_stack_budget, 2026-10-09 on 047844c5).
     * The volatile load stops GCC from folding the pointer back into a direct
     * call. Do not "simplify" it to zones_autosave_job(...). Best-effort:
     * a dispatch/autosave failure is logged, never turned into this
     * function's own return value -- the zones write ITSELF already fully
     * succeeded by this point. */
    esp_err_t autosave_dispatch_err = ESP_OK; /* nothing saved: nothing to autosave */
    if (err == ESP_OK) {
        if (uart_bridge_ext_is_on_flash_worker()) {
            void (*volatile job)(void *arg) = zones_autosave_job;
            job((void *)xTaskGetCurrentTaskHandle());
        } else {
            autosave_dispatch_err =
                uart_bridge_ext_run_on_flash_worker(zones_autosave_job, (void *)xTaskGetCurrentTaskHandle());
        }
    }
    if (autosave_dispatch_err != ESP_OK) {
        ESP_LOGW(ZONES_HTTP_TAG, "kiln config autosave could not be dispatched: %s -- the active kiln "
                      "package was NOT updated with this change, though the change itself was saved",
                 esp_err_to_name(autosave_dispatch_err));
    }

    return err;
}

/* RELAY_LIFE_BUDGET.md -- see zones_http_internal.h's own
 * comment for when each of these is called. rated_override is always 0
 * here: there is no per-relay override UI yet (RELAY_LIFE_BUDGET.md's
 * "Design" section calls it out as a later addition), so every push uses
 * the type's own rated-life table entry. */
void zones_config_push_relay_type(uint8_t zone_index)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    relay_type_t type = (relay_type_t)z->relay_type;
    if (z->relay_type > ZONE_RELAY_TYPE_MAX) {
        type = RELAY_TYPE_SSR; /* defensive, same fallback zones_config_get_relay_type() uses */
    }
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        if (z->relay_mask & (1u << r)) {
            relay_cycles_set_type(r, type, 0);
        }
    }
}

void zones_config_push_all_relay_types(void)
{
    /* opus review finding (MEDIUM): the loop below only ever pushes a type
     * onto relays named in SOME zone's relay_mask -- a relay dropped from
     * every zone's mask (config edit, zone deleted, relay reassigned) is
     * simply never visited again, so relay_cycles' in-RAM/persisted type for
     * that slot keeps whatever contactor/mercury value it last had forever,
     * even though no zone claims it any more. Clear every heater-relay slot
     * (0..KILN_IO_RELAY_COUNT-1) to RELAY_TYPE_SSR first so an orphaned slot
     * falls back to "no budget" like a never-configured one would, then let
     * the per-zone loop below re-assert the real type for every relay that
     * IS still claimed. RELAY_CYCLES_SAFETY_INDEX is deliberately untouched
     * here -- that slot is safety_cfg_store.c's own, never zones config's
     * (relay_cycles.h's own doc comment on that index). */
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        relay_cycles_set_type(r, RELAY_TYPE_SSR, 0);
    }
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        zones_config_push_relay_type(i);
    }
}

/* ---- Relay names (owner request 2026-08-27+1: "the user should be able to
 * assign names to relays not assigned to zones as well") -------------------
 *
 * DESIGN DECISION, WITH NUMBERS: a SEPARATE NVS blob/key, not a field on
 * zones_cfg_t. zones_cfg_t is 500 bytes against the 512-byte
 * ZONES_CONFIG_BLOB_MAX_SIZE cap (_Static_assert below, zones_http.h) -- the
 * pass immediately before this one already spent the last 12 bytes of slack
 * reordering zone_cfg_t's fields and cutting TIMING_PROFILE_NAME_MAX_LEN to
 * 7 just to land there. KILN_IO_RELAY_COUNT (4) names at RELAY_NAME_MAX_LEN+1
 * (16) bytes each is 64 bytes on its own -- more than five TIMES the 12
 * bytes of headroom left, before even accounting for the version/CRC/length
 * bookkeeping a new field inside zones_cfg_t would also need. Putting it
 * there would mean either raising ZONES_CONFIG_BLOB_MAX_SIZE (a real
 * decision with its own consequences -- see that macro's own comment on what
 * else it sizes: kiln_cfg_store.c's fixed per-entry storage, multiplied by
 * however many saved kiln configs a board keeps) or clawing back another 64
 * bytes from zone_cfg_t the way the last pass clawed back 8/zone, for a
 * feature (relay names) that has nothing to do with a zone's own thermal
 * record at all -- a relay NOT in any zone is, by definition, data this
 * struct's own subject (zones) doesn't own.
 *
 * A relay name is also not safety-critical and nothing on the guard/control
 * path reads it (same "purely informational" status ct_mask has, per
 * ZONES_CFG_VERSION's 5->6 comment) -- there is no reason for it to share a
 * version number, a CRC, or a load/save transaction with the data that IS
 * safety-critical. Its own key, its own version, its own tiny struct,
 * loaded/saved independently, is the clean split.
 *
 * SIZE: 1 (version) + KILN_IO_RELAY_COUNT * (RELAY_NAME_MAX_LEN + 1)
 * (4 * 16 = 64) + 4 (crc32) = 69 bytes today, nowhere near even the smallest
 * NVS blob limits, and it grows only with KILN_IO_RELAY_COUNT -- never with
 * zones_cfg_t. The two ceilings are now completely independent, which is the
 * whole point of the split: a future zones_cfg_t growth pass never has to
 * think about relay names again, and a future relay-count growth never has
 * to think about ZONES_CONFIG_BLOB_MAX_SIZE.
 *
 * WHAT HAPPENS WHEN A NAMED RELAY BECOMES ZONE-OWNED: the name is KEPT, not
 * cleared. Clearing it the instant an operator ticks a relay checkbox while
 * still laying out their zones would throw away typing on nothing more than
 * a checkbox they may untick five minutes later -- and the string costs its
 * 16 bytes on flash whether populated or not, so there is no space pressure
 * pushing the other way, unlike thermo_mask's genuinely different "0 has a
 * real, different meaning" situation. The RENDERING layer (zones_page.html's
 * renderRelayNames()) is what hides a zone-owned relay's name field behind
 * "assigned to a zone" text instead of an editable box, so an operator never
 * SEES a stale name next to a zone relay even though it is still on flash
 * underneath, ready to reappear the moment the relay is unassigned again.
 *
 * "Not assigned to any zone" is computed LIVE from the current zone config
 * every time it matters, via zone_owned_relay_mask() below -- never cached.
 * Assignment changes whenever the operator edits the zones on the very same
 * page, exactly the live-recompute requirement rules_task.c's
 * compute_heater_relay_mask() and rules_http.c's check_relay_not_zone_owned()
 * already have for the identical question ("which relays does the zone
 * config currently claim"). Both of those files are DO-NOT-TOUCH for this
 * pass, so their rule is mirrored here rather than imported -- it is the
 * exact same computation (union of every configured zone's relay_mask), not
 * a second, independently-drifting one. */
/* RELAY_NAMES_CFG_VERSION/NVS_KEY_RELAY_NAMES/relay_names_cfg_t moved to
 * zones_http_internal.h -- zones_config_accessors.c and
 * zones_http_handlers.c need the type too, not just this file. */

zones_relay_names_state_t s_relay_names;
static uint32_t s_relay_names_rev = 0;

/* Same "compute over a zeroed-crc-field copy" convention as zones_config_json_compute_crc(). */
static uint32_t compute_relay_names_crc(const relay_names_cfg_t *cfg)
{
    relay_names_cfg_t tmp = *cfg;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

/* pref_cfg_fs_validate_fn_t for the relay-names file -- re-runs the EXACT
 * same version+CRC check relay_names_load()'s NVS path already applies
 * below, per this task's "validated on load exactly as its NVS path
 * validates today" requirement. Defensively NUL-terminates every name in
 * `bytes` in place before accepting it, same reason relay_names_load() does
 * it for the NVS candidate. */
static bool relay_names_validate(const void *bytes, size_t len)
{
    if (len != sizeof(relay_names_cfg_t)) {
        return false;
    }
    relay_names_cfg_t cand;
    memcpy(&cand, bytes, sizeof(cand));
    if (cand.version != RELAY_NAMES_CFG_VERSION) {
        return false;
    }
    uint32_t computed = compute_relay_names_crc(&cand);
    if (computed != cand.crc32) {
        return false;
    }
    return true;
}

/* ---- v1 -> v2 migration (docs/ZONE_GRAPHIC_PLAN.md stage 1) --------------
 *
 * WHY THIS EXISTS AT ALL. relay_names_validate() above discards the ENTIRE
 * blob -- every operator-entered relay name, with no operator-visible notice
 * -- on any length, version or CRC mismatch. Until 2026-09-18 only one
 * version of this blob had ever existed, so there was no migration function
 * of any kind. Bumping RELAY_NAMES_CFG_VERSION 1 -> 2 for types[] without
 * writing one would therefore have blanked the relay names on every board in
 * the field, silently, on the next firmware update. That is the blocking
 * hazard the plan names, and this block is its fix.
 *
 * The migration is deliberately boring: recognise the old layout by its own
 * byte length, validate it with the old CRC over the old struct, copy the
 * names across field-by-field, and default every new types[] entry to
 * RELAY_DEVICE_TYPE_UNSET. No memcpy of one struct shape over another, and
 * no reinterpretation of the old blob's padding bytes as new fields. */

/* The v1 CRC, computed against the FROZEN v1 layout. A separate function
 * rather than a length parameter on compute_relay_names_crc(): the CRC
 * covers the whole struct including its trailing alignment padding, so "the
 * same code over a different number of bytes" would not be the same
 * computation, and a v1 blob would fail its own integrity check. */
static uint32_t compute_relay_names_crc_v1(const relay_names_cfg_v1_t *cfg)
{
    relay_names_cfg_v1_t tmp = *cfg;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

/* pref_cfg_fs_validate_fn_t-shaped (so the file pre-pass in relay_names_load()
 * can hand it straight to pref_cfg_fs_load_raw()) v1 acceptance test: exact
 * v1 length, version field exactly 1, v1 CRC. Same length-before-
 * interpretation order as the v2 validator above. */
static bool relay_names_validate_v1(const void *bytes, size_t len)
{
    if (len != sizeof(relay_names_cfg_v1_t)) {
        return false;
    }
    relay_names_cfg_v1_t cand;
    memcpy(&cand, bytes, sizeof(cand));
    if (cand.version != 1) {
        return false;
    }
    return compute_relay_names_crc_v1(&cand) == cand.crc32;
}

/* The migration proper. `old` must already have passed
 * relay_names_validate_v1(). Produces a fully-formed, self-consistent v2
 * struct (version stamped, CRC recomputed) so the result is something the v2
 * validator accepts -- the caller can persist it or adopt it without a
 * second fixup step. */
static void relay_names_upgrade_v1(const relay_names_cfg_v1_t *old, relay_names_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = RELAY_NAMES_CFG_VERSION;
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        /* Field-by-field, bounded by the v1 array's own width -- THE point
         * of the migration. The names are the operator data being rescued. */
        memcpy(out->names[r], old->names[r], RELAY_NAME_MAX_LEN + 1);
        out->names[r][RELAY_NAME_MAX_LEN] = '\0';
        /* UNSET, never OTHER: a board upgrading from v1 has never been asked
         * what its relays drive, and must say so rather than assert a choice
         * nobody made (owner decision, docs/ZONE_GRAPHIC_PLAN.md q1). */
        out->types[r] = (uint8_t)RELAY_DEVICE_TYPE_UNSET;
    }
    out->crc32 = compute_relay_names_crc(out);
}

/* Decode a raw stored blob of ANY version this build knows into a v2 struct.
 * Returns false -- leaving *out untouched -- for anything else, which keeps
 * the pre-existing "unrecognised blob is discarded, names go blank" behaviour
 * exactly as it was for a genuinely corrupt or genuinely newer blob. Only the
 * specific, recognised v1 shape is rescued. */
static bool relay_names_decode_any_impl(const void *bytes, size_t len, relay_names_cfg_t *out, bool quiet)
{
    if (relay_names_validate(bytes, len)) {
        memcpy(out, bytes, sizeof(*out));
        return true;
    }
    if (relay_names_validate_v1(bytes, len)) {
        relay_names_cfg_v1_t old;
        memcpy(&old, bytes, sizeof(old));
        relay_names_upgrade_v1(&old, out);
        if (!quiet) {
            ESP_LOGW(ZONES_HTTP_TAG,
                     "relay_names blob is v1 -- migrating to v%u, names preserved, every device type defaults to "
                     "unset",
                     (unsigned)RELAY_NAMES_CFG_VERSION);
        }
        return true;
    }
    return false;
}

static bool relay_names_decode_any(const void *bytes, size_t len, relay_names_cfg_t *out)
{
    return relay_names_decode_any_impl(bytes, len, out, false);
}

/* Loads s_relay_names.cfg from NVS_KEY_RELAY_NAMES (same namespace/partition
 * as the zones blob -- see NVS_NAMESPACE/KILN_NVS_PARTITION above). Resets to
 * all-empty names -- not reported to the caller as a failure, this is purely
 * cosmetic data, unlike s_zones_config_valid's safety-relevant "cannot
 * trust this" gate -- on: namespace/key not found (first boot, or a board
 * that has never named a relay), a blob whose length matches no version this
 * build knows (the same "length before interpretation" order
 * zones_config_json_decode_blob() uses for the zones blob), an unrecognized
 * version, or a CRC mismatch.
 *
 * A v1 blob is NOT one of those cases any more -- it is migrated in place by
 * relay_names_decode_any() above, preserving every name. That version field
 * was added before there was a second version to get wrong, specifically so
 * this moment would have something to switch on instead of repeating this
 * codebase's own "grew the struct, forgot the old layout" history (see
 * ZONES_CFG_VERSION's 6->7 comment for exactly what that mistake cost); the
 * v1 -> v2 bump is the first time that foresight actually paid out. */
void relay_names_load(void)
{
    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
    s_relay_names_rev = 0;

    relay_names_cfg_t nvs_cand;
    memset(&nvs_cand, 0, sizeof(nvs_cand));
    bool nvs_valid = false;
    uint32_t nvs_rev = 0;

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err == HAL_OK) {
        uint8_t raw[sizeof(relay_names_cfg_t)];
        size_t len = sizeof(raw);
        hal_status_t rerr = hal_kv_get_blob(&h, NVS_KEY_RELAY_NAMES, raw, &len);
        if (rerr == HAL_OK) {
            /* `len` is the blob's ACTUAL stored length, which is how v1 and
             * v2 are told apart -- do not substitute sizeof(raw) here. The
             * buffer is v2-sized and v2 is the larger layout, so a v1 blob
             * reads into it whole and reports its own shorter length. */
            if (relay_names_decode_any(raw, len, &nvs_cand)) {
                nvs_valid = true;
            } else {
                ESP_LOGE(ZONES_HTTP_TAG,
                         "relay_names NVS blob (%u bytes) matches no known version, or failed version/CRC "
                         "validation -- discarding NVS candidate",
                         (unsigned)len);
            }
        } /* else HAL_NOT_FOUND (never saved) or a real error -- blank NVS candidate is safe either way */
        if (nvs_valid) {
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, NVS_KEY_RELAY_NAMES_REV, &rev) == HAL_OK) {
                nvs_rev = rev;
            }
        }
        hal_kv_close(&h);
    } /* else namespace not yet created -- first boot, blank NVS candidate */

    /* v1 FILE PRE-PASS. The generic bridge below is parameterized by ONE
     * fixed item size, so it can only ever see a v2-length file -- a v1-length
     * one fails its `len != 4 + item_size` check and is ignored, which for
     * relay names would mean silently dropping the names a v1 board had
     * dual-written. Upgrade it in place first, at the SAME rev, so resolve()
     * below then sees an ordinary v2 file and the tie-break is unaffected.
     *
     * The two reads are mutually exclusive by construction (one file, and
     * 4 + sizeof(v1) != 4 + sizeof(v2), pinned by the _Static_assert in
     * zones_http_internal.h), so this cannot fight a valid v2 file.
     *
     * Inert on every board today: no `cfg` partition is mounted, so
     * pref_cfg_fs_load_raw() returns "no file" before touching any of this. */
    {
        relay_names_cfg_v1_t file_v1;
        uint32_t v1_rev = 0;
        bool v1_valid = false;
        pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(file_v1), relay_names_validate_v1, &file_v1, &v1_rev,
                             &v1_valid);
        if (v1_valid) {
            relay_names_cfg_t upgraded;
            relay_names_upgrade_v1(&file_v1, &upgraded);
            esp_err_t uerr = pref_cfg_fs_save(RELAY_NAMES_FILE_PATH, &upgraded, sizeof(upgraded), v1_rev);
            if (uerr != ESP_OK && uerr != ESP_ERR_INVALID_STATE) {
                /* Not fatal: NVS is the persistence guarantee, and the NVS
                 * candidate above has already been migrated independently.
                 * The file simply stays v1 and keeps being ignored until a
                 * later save rewrites it. */
                ESP_LOGW(ZONES_HTTP_TAG, "could not upgrade the v1 relay-names file to v%u: %s",
                         (unsigned)RELAY_NAMES_CFG_VERSION, esp_err_to_name(uerr));
            } else if (uerr == ESP_OK) {
                ESP_LOGW(ZONES_HTTP_TAG, "relay-names file was v1 -- upgraded in place at rev %lu, names preserved",
                         (unsigned long)v1_rev);
            }
        }
    }

    /* Hand off to the generic file-vs-NVS read-through/tie-break policy
     * (pref_cfg_fs.h) -- see this file's top-of-file include comment. On
     * every board today (no `cfg` partition mounted) this is a pass-through:
     * the file is absent, resolve() returns exactly the NVS candidate. */
    relay_names_cfg_t resolved;
    memset(&resolved, 0, sizeof(resolved));
    uint32_t resolved_rev = 0;
    bool used_file = false;
    bool have_value = pref_cfg_fs_resolve(RELAY_NAMES_FILE_PATH, &nvs_cand, sizeof(relay_names_cfg_t), nvs_valid,
                                           nvs_rev, relay_names_validate, &resolved, &resolved_rev, &used_file);
    if (!have_value) {
        return; /* neither side trustworthy -- names stay blank, exactly the pre-existing behavior */
    }

    /* Defensive NUL-termination against a corrupted-but-CRC-lucky blob (or
     * a file byte-copy) -- every name must be a valid C string before JSON
     * emission or a POST scratch-buffer strncpy touches it. */
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        resolved.names[r][RELAY_NAME_MAX_LEN] = '\0';
    }
    s_relay_names.cfg = resolved;
    s_relay_names_rev = resolved_rev;
    ESP_LOGI(ZONES_HTTP_TAG, "relay names loaded (source=%s, rev=%lu)", used_file ? "file" : "NVS",
             (unsigned long)s_relay_names_rev);
}

/* Read-only dual-write status for GET /api/cfgfs's "relay_names" row -- same
 * contract and no-lock rationale as zone_normals_get_dualwrite_status(): a
 * fresh re-read of both sides, never a resync write, safe to poll. The NVS
 * side goes through relay_names_decode_any() so a still-v1 blob reads as
 * valid (upgraded in memory) exactly as relay_names_load() sees it; the file
 * side is v2 only because a v1 file is upgraded in place by the load path
 * and the bridge cannot see one. */
void relay_names_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                      bool *diverged)
{
    relay_names_cfg_t f_cfg;
    memset(&f_cfg, 0, sizeof(f_cfg));
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw_quiet(RELAY_NAMES_FILE_PATH, sizeof(f_cfg), relay_names_validate, &f_cfg, &f_rev,
                               &f_valid);

    relay_names_cfg_t n_cfg;
    memset(&n_cfg, 0, sizeof(n_cfg));
    bool n_valid = false;
    uint32_t n_rev = 0;
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
        uint8_t raw[sizeof(relay_names_cfg_t)];
        size_t len = sizeof(raw);
        if (hal_kv_get_blob(&h, NVS_KEY_RELAY_NAMES, raw, &len) == HAL_OK &&
            relay_names_decode_any_impl(raw, len, &n_cfg, true)) {
            n_valid = true;
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, NVS_KEY_RELAY_NAMES_REV, &rev) == HAL_OK) {
                n_rev = rev;
            }
        }
        hal_kv_close(&h);
    }

    if (file_valid) {
        *file_valid = f_valid;
    }
    if (file_rev) {
        *file_rev = f_rev;
    }
    if (nvs_valid) {
        *nvs_valid = n_valid;
    }
    if (nvs_rev) {
        *nvs_rev = n_rev;
    }
    if (diverged) {
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid,
                                                f_valid && n_valid && memcmp(&f_cfg, &n_cfg, sizeof(f_cfg)) == 0);
    }
}

/* Caller holds zones_cfg_save_section_lock(). Commits a snapshot taken under
 * zones_cfg_lock(): zones_http_post.c assigns s_relay_names.cfg under that
 * lock, so the file is never a torn mix of two writers' names. */
esp_err_t relay_names_save_locked(void)
{
    relay_names_cfg_t snap;
    zones_cfg_lock();
    snap = s_relay_names.cfg;
    zones_cfg_unlock();
    snap.version = RELAY_NAMES_CFG_VERSION;
    snap.crc32 = compute_relay_names_crc(&snap);
    uint32_t new_rev = s_relay_names_rev + 1;

    /* cfg file ONLY -- docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed". */
    esp_err_t err = pref_cfg_fs_commit(RELAY_NAMES_FILE_PATH, &snap, sizeof(snap), new_rev, "relay names");
    if (err == ESP_OK) {
        s_relay_names_rev = new_rev;
    }
    return err;
}

esp_err_t relay_names_save(void)
{
    zcfg_save_lock(); /* rev read .. rev bump; see s_zcfg_save_mutex */
    if (cfg_save_lock_reset_refused()) { /* factory reset in flight (pref_cfg_fs.h writer fence) */
        zcfg_save_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = relay_names_save_locked();
    zcfg_save_unlock();
    return err;
}

/* ---- Task 1's persisted normal-current results ----------------------------
 * Same reasoning as relay_names_cfg_t just above, applied to a different
 * field: a SEPARATE NVS blob/key, not a field on zones_cfg_t.
 * ZONES_CONFIG_BLOB_MAX_SIZE (640) already has zones_cfg_t sitting well up;
 * three more floats plus the version/CRC bookkeeping would either force that
 * ceiling up (a real decision with knock-on effects on kiln_cfg_store.c's
 * fixed per-entry size, see that macro's own comment) or claw back yet more
 * bytes from zone_cfg_t for data that has nothing to do with a zone's own
 * thermal record. Also not safety-critical -- nothing on the guard/control
 * path reads it, only Task 2's WARNING predicate above -- so it has no
 * business sharing a version/CRC/load transaction with data that is. */
/* History of ZONE_NORMALS_CFG_VERSION and NVS_KEY_ZONE_NORMALS, both now defined in
 * zones_http_internal.h (moved there so the host tests can see them):
 *
 * 1 -> 2 (M12): the sweep now also derives which CT channel watches which
 * zone (ct_map_*, see zone_sweep_derive_ct_channel() below) and that record
 * has to outlive the sweep task -- the commissioning page reads it on a
 * later page load to decide whether ct_channel_map[0..2] renders as DERIVED
 * or as a manual-entry field. A v1 blob is discarded by the version check
 * below rather than migrated: the whole blob is re-measured by one button
 * press, and the alternative (reading a short struct and zero-filling the
 * tail) is a migration path worth writing only for data that cannot simply
 * be measured again.
 *
 * 2 -> 3 (M12b): the sweep now also derives k_ct_v_per_a[0..2] from the
 * nameplate power the operator already answered (COMMISSIONING_UX.md Q3/Q4)
 * and this run's own measured current, and the commissioning page has to be
 * able to say DERIVED on a later page load for that field too. A v2 blob is
 * discarded rather than migrated, for exactly the reason v1 was: one button
 * press re-measures the whole thing.
 *
 * RENAMED 2026-09-06 from "zone_normals_cfg" (16 chars) -- confirmed against
 * the installed ESP-IDF (nvs.h: `#define NVS_KEY_NAME_MAX_SIZE 16` "including
 * null terminator"; nvs_page.cpp's Item::MAX_KEY_LENGTH = sizeof(key)-1 = 15,
 * checked as `if (keySize > Item::MAX_KEY_LENGTH) return ESP_ERR_NVS_KEY_TOO_LONG;`)
 * that a 16-character key is one character too long for real NVS to ever
 * accept. This was NOT a fake_kv.h sizing gap found during the HAL migration
 * -- it is a production defect that predates this migration entirely (the
 * key was introduced with this exact 16-char spelling in ddbd024, "Stop the
 * commissioning page reporting writes that never landed") and was invisible
 * because the pre-migration host stub (stubs/nvs.h) modeled ONE shared blob
 * slot with no key-length check of any kind, and the board's own
 * zone_normals_save_locked() logged nothing on a non-OK return (unlike every
 * sibling setter in this file) -- fixed in the 2026-09-07 persist-logging
 * audit (docs/audits/persist_save_logging_2026-09-07.md), zone_normals_save_locked()
 * now logs an ESP_LOGW naming the module and esp_err_to_name() on failure.
 * At the time the key-length bug was live, EVERY zone_normals_set()/
 * zone_ct_map_set()/zone_k_ct_set() write since that commit has silently
 * failed at nvs_set_blob() with ESP_ERR_NVS_KEY_TOO_LONG -- measured normal
 * currents and derived CT-channel/k_ct_v_per_a maps have never actually
 * persisted across a reboot on this board. Because nothing was ever
 * written under the old name, there is no on-flash data to migrate: the
 * rename is a plain one-time swap, not a migration. See
 * fake_kv.h's FAKE_KV_MAX_KEY_LEN (also 15 usable chars, deliberately kept
 * equal to NVS's real limit rather than raised) -- it is what caught this
 * during the nvs.h -> hal_kv.h migration's host-test pass. */

/* Compile-time guard so this class of bug (a >15-char NVS key that silently
 * never persists on real hardware) cannot recur in this file: every
 * NVS_KEY_* literal used here must fit ESP-IDF's real NVS_KEY_NAME_MAX_SIZE
 * (16 bytes INCLUDING the NUL terminator, i.e. 15 usable characters). Macro
 * shared via nvs_key_check.h so every other module with NVS key literals
 * gets the identical check. */
NVS_KEY_LEN_CHECK(NVS_KEY_ZONES);
NVS_KEY_LEN_CHECK(NVS_KEY_RELAY_NAMES);

/* docs/CONFIG_FILESYSTEM.md item 2: zone normals dual-write to the `cfg`
 * LittleFS partition through the generic pref_cfg_fs.h bridge, exactly as
 * relay names do (fixed-size struct, no migration chain: a version mismatch
 * is discarded, see ZONE_NORMALS_CFG_VERSION in zones_http_internal.h). The dual-write rev
 * counter lives in its own tiny NVS key for the same reason NVS_KEY_ZONES_REV
 * and NVS_KEY_RELAY_NAMES_REV do -- it is not part of the measured data. */

static struct {
    zone_normals_cfg_t cfg;
} s_zone_normals;
static uint32_t s_zone_normals_rev = 0;

static uint32_t compute_zone_normals_crc(const zone_normals_cfg_t *cfg)
{
    zone_normals_cfg_t tmp = *cfg;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

/* pref_cfg_fs_validate_fn_t for the zone-normals file: the exact length +
 * version + CRC acceptance zone_normals_load()'s NVS path applies. */
static bool zone_normals_validate(const void *bytes, size_t len)
{
    if (len != sizeof(zone_normals_cfg_t)) {
        return false;
    }
    zone_normals_cfg_t cand;
    memcpy(&cand, bytes, sizeof(cand));
    if (cand.version != ZONE_NORMALS_CFG_VERSION) {
        return false;
    }
    return compute_zone_normals_crc(&cand) == cand.crc32;
}

/* Same "reset to blank, log, move on" convention as relay_names_load() --
 * this is measured convenience data, not safety state, so a bad blob is
 * simply forgotten (every zone reads back as "never measured") rather than
 * blocking anything. */
void zone_normals_load(void)
{
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    s_zone_normals_rev = 0;

    zone_normals_cfg_t nvs_cand;
    memset(&nvs_cand, 0, sizeof(nvs_cand));
    bool nvs_valid = false;
    uint32_t nvs_rev = 0;

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err == HAL_OK) {
        uint8_t raw[sizeof(zone_normals_cfg_t)];
        size_t len = sizeof(raw);
        if (hal_kv_get_blob(&h, NVS_KEY_ZONE_NORMALS, raw, &len) == HAL_OK) {
            if (len != sizeof(zone_normals_cfg_t)) {
                ESP_LOGE(ZONES_HTTP_TAG, "zone_normals blob is %u bytes, expected %u -- discarding",
                         (unsigned)len, (unsigned)sizeof(zone_normals_cfg_t));
            } else if (!zone_normals_validate(raw, len)) {
                ESP_LOGE(ZONES_HTTP_TAG, "zone_normals blob failed version/CRC validation -- discarding");
            } else {
                memcpy(&nvs_cand, raw, sizeof(nvs_cand));
                nvs_valid = true;
            }
        } /* never saved, or a read error -- blank NVS candidate is safe either way */
        if (nvs_valid) {
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, NVS_KEY_ZONE_NORMALS_REV, &rev) == HAL_OK) {
                nvs_rev = rev;
            }
        }
        hal_kv_close(&h);
    }

    /* File-vs-NVS read-through/tie-break via the generic bridge
     * (pref_cfg_fs.h). With no `cfg` mount it is a pass-through returning
     * exactly the NVS candidate. */
    zone_normals_cfg_t resolved;
    memset(&resolved, 0, sizeof(resolved));
    uint32_t resolved_rev = 0;
    bool used_file = false;
    if (!pref_cfg_fs_resolve(ZONE_NORMALS_FILE_PATH, &nvs_cand, sizeof(zone_normals_cfg_t), nvs_valid, nvs_rev,
                             zone_normals_validate, &resolved, &resolved_rev, &used_file)) {
        return; /* neither side trustworthy -- every zone reads back as "never measured" */
    }
    s_zone_normals.cfg = resolved;
    s_zone_normals_rev = resolved_rev;
    ESP_LOGI(ZONES_HTTP_TAG, "zone normals loaded (source=%s, rev=%lu)", used_file ? "file" : "NVS",
             (unsigned long)s_zone_normals_rev);
}

/* Caller holds zcfg_save_lock() across its RAM edit AND this save
 * (CFG_STORE_SAVE_RACE audit MED-1): every setter below mutates
 * s_zone_normals.cfg only inside that section, so a concurrent setter can
 * neither interleave its edit into this commit nor publish a rev for it. */
static esp_err_t zone_normals_save_locked(void)
{
    if (cfg_save_lock_reset_refused()) { /* factory reset in flight (pref_cfg_fs.h writer fence) */
        return ESP_ERR_INVALID_STATE;
    }
    s_zone_normals.cfg.version = ZONE_NORMALS_CFG_VERSION;
    s_zone_normals.cfg.crc32 = compute_zone_normals_crc(&s_zone_normals.cfg);
    uint32_t new_rev = s_zone_normals_rev + 1;

    /* cfg file ONLY (same policy as relay_names_save()). */
    esp_err_t err = pref_cfg_fs_commit(ZONE_NORMALS_FILE_PATH, &s_zone_normals.cfg, sizeof(s_zone_normals.cfg),
                                       new_rev, "zone normals / CT map");
    if (err == ESP_OK) {
        s_zone_normals_rev = new_rev;
    }
    return err;
}

/* Read-only dual-write status for GET /api/cfgfs's "zone_normals" row -- same
 * contract as profiles_builtin_get_dualwrite_status()/unit_pref_get_
 * dualwrite_status(): a fresh re-read of BOTH sides, never a resync write
 * (zone_normals_load() performs those; a status GET must not), safe to
 * poll. The rev is the dual-write rev counter (znorm_rev / the file's rev
 * prefix), not anything in the measured data. Takes no lock: nothing in the
 * zone-normals store is guarded by one (s_zone_normals is read bare by
 * zones_config_get_normal_current() too), and this reads NVS and the file
 * directly instead of s_zone_normals, so there is no shared RAM to guard
 * and no lock to hold across the cfg_fs read. */
void zone_normals_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                       bool *diverged)
{
    zone_normals_cfg_t f_cfg;
    memset(&f_cfg, 0, sizeof(f_cfg));
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw_quiet(ZONE_NORMALS_FILE_PATH, sizeof(f_cfg), zone_normals_validate, &f_cfg, &f_rev,
                               &f_valid);

    zone_normals_cfg_t n_cfg;
    memset(&n_cfg, 0, sizeof(n_cfg));
    bool n_valid = false;
    uint32_t n_rev = 0;
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
        uint8_t raw[sizeof(zone_normals_cfg_t)];
        size_t len = sizeof(raw);
        if (hal_kv_get_blob(&h, NVS_KEY_ZONE_NORMALS, raw, &len) == HAL_OK && zone_normals_validate(raw, len)) {
            memcpy(&n_cfg, raw, sizeof(n_cfg));
            n_valid = true;
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, NVS_KEY_ZONE_NORMALS_REV, &rev) == HAL_OK) {
                n_rev = rev;
            }
        }
        hal_kv_close(&h);
    }

    if (file_valid) {
        *file_valid = f_valid;
    }
    if (file_rev) {
        *file_rev = f_rev;
    }
    if (nvs_valid) {
        *nvs_valid = n_valid;
    }
    if (nvs_rev) {
        *nvs_rev = n_rev;
    }
    if (diverged) {
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid,
                                                f_valid && n_valid && memcmp(&f_cfg, &n_cfg, sizeof(f_cfg)) == 0);
    }
}

bool zones_config_get_normal_current(uint8_t zone_index, float *out_amps, bool *out_measured)
{
    if (!out_amps || !out_measured || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    *out_measured = (s_zone_normals.cfg.measured_mask & (1u << zone_index)) != 0;
    *out_amps = *out_measured ? s_zone_normals.cfg.normal_current_a[zone_index] : 0.0f;
    return true;
}

/* Persists one zone's measured normal -- called only by zone_sweep_task()
 * below, once per zone that actually produced a sample. Not exposed in
 * zones_http.h: the sweep is the only legitimate writer (this is measured
 * data, not an operator-entered field), so there is no setter for anything
 * outside this file to call. */
bool zone_normals_set(uint8_t zone_index, float amps)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(amps) || amps < 0.0f) {
        return false;
    }
    zcfg_save_lock();
    s_zone_normals.cfg.normal_current_a[zone_index] = amps;
    s_zone_normals.cfg.measured_mask |= (uint8_t)(1u << zone_index);
    bool ok = zone_normals_save_locked() == ESP_OK;
    zcfg_save_unlock();
    return ok;
}

/* 2026-09-10: originally written to clear a zone's measured normal current
 * whenever its channel's k_ct_v_per_a changed, on the theory that refusing
 * to rescale (a sign/formula error there could silently halve or double an
 * armed guard) was strictly safer than computing the conversion. Superseded
 * the same day (opus review): clearing the ESP's OWN bookkeeping did not
 * tell the safety processor anything -- the Pico is the only processor that
 * actually runs S14/S15, and it kept its previously-committed i_normal_a
 * unchanged, now silently on the WRONG scale relative to the k_ct it had
 * just accepted. Worse, since a cleared zone can never be re-armed except
 * by a sweep whose own k_ct derivation refused, the guard could only ever
 * arm when calibration did NOT happen. The real fix
 * (zone_sweep_push_kct_and_inormal() in zones_current_sweep_task.c) rescales
 * the amps EXACTLY (amps_new = amps_old * k_old/k_new, the reciprocal of the
 * ratio zone_sweep_derive_k_ct() already computed -- not a guess) and pushes
 * k_ct and the corrected i_normal_a as ONE staged transaction, so the two
 * processors can never disagree about scale, and a successful calibration
 * now correctly arms the guard instead of erasing the evidence for it.
 *
 * This function is no longer called from the sweep path, but is kept as a
 * correct, independently-tested primitive (clear a zone's measured normal
 * and its measured_mask bit, e.g. for a future explicit "forget this zone's
 * calibration" operator action) -- `zone_mask` is a bitmask, bit i = zone
 * i's stored normal should be forgotten. */
bool zone_normals_invalidate_mask(uint8_t zone_mask)
{
    if (zone_mask == 0) {
        return true;
    }
    zone_mask &= (uint8_t)((1u << MAX31856_CHANNEL_COUNT) - 1u);
    zcfg_save_lock();
    if ((s_zone_normals.cfg.measured_mask & zone_mask) == 0) {
        zcfg_save_unlock();
        return true; /* nothing measured in the affected set -- nothing to invalidate */
    }
    s_zone_normals.cfg.measured_mask &= (uint8_t)~zone_mask;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (zone_mask & (1u << zi)) {
            s_zone_normals.cfg.normal_current_a[zi] = 0.0f;
        }
    }
    bool ok = zone_normals_save_locked() == ESP_OK;
    zcfg_save_unlock();
    return ok;
}

/* Same "the sweep is the only legitimate writer" rule zone_normals_set()
 * above states, applied to the derived CT map: this is measured data, and
 * an operator who wants to say it by hand says it on the commissioning page
 * (which writes the Pico's ct_channel_map directly), never here. Clearing
 * the whole record at the START of a sweep is deliberate -- a re-sweep of
 * rewired hardware must not leave a channel's stale "derived" claim behind
 * to be shown as current. */
bool zone_ct_map_clear(void)
{
    zcfg_save_lock();
    s_zone_normals.cfg.ct_map_derived_mask = 0;
    memset(s_zone_normals.cfg.ct_map_zone, 0, sizeof(s_zone_normals.cfg.ct_map_zone));
    /* Persisted immediately, not left for the first zone_ct_map_set() to
     * flush: a sweep that clears the map and then fails outright never
     * reaches a set(), and leaving the old map in NVS would resurrect it on
     * the next boot as though it were still current. Failure is already
     * logged inside zone_normals_save_locked() itself. */
    esp_err_t clear_err = zone_normals_save_locked();
    zcfg_save_unlock();
    return clear_err == ESP_OK;
}

bool zone_ct_map_set(uint8_t ct_channel, uint8_t zone_index)
{
    if (ct_channel >= ZONE_CT_CHANNEL_COUNT || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    zcfg_save_lock();
    s_zone_normals.cfg.ct_map_zone[ct_channel] = zone_index;
    s_zone_normals.cfg.ct_map_derived_mask |= (uint8_t)(1u << ct_channel);
    bool ok = zone_normals_save_locked() == ESP_OK;
    zcfg_save_unlock();
    return ok;
}

/* M12b: same discipline as zone_ct_map_clear()/zone_ct_map_set() just above
 * -- this record is provenance ONLY. The value the guards and the power
 * estimate actually use lives on the Pico and is written by
 * zone_sweep_push_k_ct_v_per_a(); nothing here is ever read back as a
 * calibration. */
bool zone_k_ct_clear(void)
{
    zcfg_save_lock();
    s_zone_normals.cfg.k_ct_derived_mask = 0;
    memset(s_zone_normals.cfg.k_ct_v_per_a, 0, sizeof(s_zone_normals.cfg.k_ct_v_per_a));
    /* Failure is already logged inside zone_normals_save_locked() itself. */
    esp_err_t clear_err = zone_normals_save_locked();
    zcfg_save_unlock();
    return clear_err == ESP_OK;
}

bool zone_k_ct_set(uint8_t ct_channel, float k_v_per_a)
{
    if (ct_channel >= ZONE_CT_CHANNEL_COUNT || !isfinite(k_v_per_a) || k_v_per_a <= 0.0f) {
        return false;
    }
    zcfg_save_lock();
    s_zone_normals.cfg.k_ct_v_per_a[ct_channel] = k_v_per_a;
    s_zone_normals.cfg.k_ct_derived_mask |= (uint8_t)(1u << ct_channel);
    bool ok = zone_normals_save_locked() == ESP_OK;
    zcfg_save_unlock();
    return ok;
}

void zones_ct_k_v_per_a_derived(uint8_t *out_derived_mask, float *out_k_v_per_a)
{
    if (out_derived_mask) {
        *out_derived_mask = s_zone_normals.cfg.k_ct_derived_mask;
    }
    if (out_k_v_per_a) {
        memcpy(out_k_v_per_a, s_zone_normals.cfg.k_ct_v_per_a, sizeof(s_zone_normals.cfg.k_ct_v_per_a));
    }
}

void zones_ct_channel_map_derived(uint8_t *out_derived_mask, uint8_t *out_zone_for_ch)
{
    if (out_derived_mask) {
        *out_derived_mask = s_zone_normals.cfg.ct_map_derived_mask;
    }
    if (out_zone_for_ch) {
        memcpy(out_zone_for_ch, s_zone_normals.cfg.ct_map_zone, ZONE_CT_CHANNEL_COUNT);
    }
}

/* Union of every currently-configured zone's relay_mask -- bit N-1 set iff
 * relay N (1-based) is claimed by SOME zone. Computed fresh from `cfg` every
 * call, never cached -- see this section's header comment. Deliberately a
 * local mirror of rules_task.c's compute_heater_relay_mask() / the loop
 * inside rules_http.c's check_relay_not_zone_owned(): both files are
 * off-limits for this pass, so the identical rule (union of relay_mask over
 * every zone index < thermo_count) is reimplemented here rather than
 * imported -- it must stay the SAME rule, not a second one that can drift. */
uint8_t zone_owned_relay_mask(const zones_cfg_t *cfg)
{
    uint8_t mask = 0;
    for (uint8_t zi = 0; zi < cfg->thermo_count && zi < MAX31856_CHANNEL_COUNT; zi++) {
        mask |= cfg->zones[zi].relay_mask;
    }
    return mask;
}

