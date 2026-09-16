#include "zones_http_internal.h"

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
#include "freertos/task.h" /* xTaskGetCurrentTaskHandle() -- autosave dispatcher identity, 2026-09-16 */
#include "kiln_cfg_store.h" /* kiln_cfg_store_autosave_from_live() -- docs/KILN_PROFILES_PLAN.md
                             * section 2.4, item 13. */
#include "kiln_io.h"
#include "relay_cycles.h" /* RELAY_LIFE_BUDGET.md: relay_cycles_set_type() push
                            * on load, zones_config_push_relay_type()/_push_all_relay_types()
                            * below. */
#include "zones_config_cfg_fs.h" /* docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 5:
                            * read-through/dual-write bridge to the `cfg` LittleFS
                            * partition -- see that header for the full design. */
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
                                       bool *out_valid);

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
static esp_err_t nvs_load_from(const char *partition, zones_cfg_t *out_cfg, bool *out_found, bool *out_valid)
{
    bool valid = false;
    esp_err_t err = nvs_load_from_decode(partition, out_cfg, out_found, &valid);
    if (!valid) {
        zones_config_json_apply_model_fit_defaults(out_cfg);
    }
    if (out_valid) {
        *out_valid = valid;
    }
    return err;
}

static esp_err_t nvs_load_from_decode(const char *partition, zones_cfg_t *out_cfg, bool *out_found,
                                       bool *out_valid)
{
    if (out_found) {
        *out_found = false;
    }
    if (out_valid) {
        *out_valid = false;
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
    uint8_t raw[sizeof(zones_cfg_t)];
    memset(raw, 0, sizeof(raw));
    size_t len = sizeof(raw);
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
        if (out_found) {
            *out_found = true;
        }
        return ESP_OK;
    case ZONES_DECODE_CORRUPT:
    default:
        /* Genuine corruption (too short, wrong length for the claimed
         * version, bad CRC, or a decoded config that failed
         * zones_config_json_validate()) -- loud enough that an operator can see it,
         * naming the on-disk version and the specific rejection reason.
         * Nothing worth protecting was found here, so a caller (the
         * legacy-partition migration) is free to look elsewhere. */
        ESP_LOGW(ZONES_HTTP_TAG, "zones_cfg blob from '%s' (on-disk version %u, %u bytes) REJECTED: %s -- "
                      "falling back to defaults, NOT adopting this config",
                 partition, (unsigned)raw[0], (unsigned)len, reason);
        return ESP_OK;
    }
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
    esp_err_t err = nvs_load_from(KILN_NVS_PARTITION, &s_zones.cfg, out_found, out_valid);

    /* docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 5: read-through
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
    zones_cfg_t resolved;
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool trustworthy = zones_config_cfg_fs_resolve(&s_zones.cfg, nvs_valid, nvs_rev, &resolved, &resolved_rev,
                                                    &used_file);
    s_zones_cfg_rev = resolved_rev;
    if (used_file) {
        s_zones.cfg = resolved;
        if (out_found) {
            *out_found = true;
        }
        if (out_valid) {
            *out_valid = true;
        }
    }

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
    return err;
}

/* Hand-declared, same convention relay_cycles.c/safety_cfg_store.c/
 * factory_reset.c/diagnostics_http.c already use (see flash_worker.h's own
 * doc comment) -- avoids pulling in the whole UART bridge API for one call.
 */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);

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

esp_err_t nvs_save(void)
{
    s_zones.cfg.version = ZONES_CFG_VERSION;
    /* Stamped last, after every other field is final for this write -- see
     * zones_config_json_compute_crc()/zones_cfg_t::crc32's comments. Any in-RAM edit that
     * lands here (a setter, a POST commit, an import) gets a fresh, correct
     * CRC every time this function runs; there is no path that writes the
     * blob without also re-stamping it. */
    s_zones.cfg.crc32 = zones_config_json_compute_crc(&s_zones.cfg);

    /* Dual-write, FILE FIRST: requirement 1 of the zones-config-move task.
     * The file write's own failure is logged inside
     * zones_config_cfg_fs_save() and otherwise swallowed here -- NVS below
     * is still authoritative for older firmware and for a board with no
     * `cfg` partition (ESP_ERR_INVALID_STATE is the expected, silent
     * outcome on every board today), so a file-write failure must not stop
     * the NVS write that every existing caller of nvs_save() still depends
     * on for its actual persistence guarantee. */
    s_zones_cfg_rev++;
    (void)zones_config_cfg_fs_save(&s_zones.cfg, s_zones_cfg_rev);

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_ZONES, &s_zones.cfg, sizeof(s_zones.cfg));
    if (err == HAL_OK) {
        err = hal_kv_set_u32(&h, NVS_KEY_ZONES_REV, s_zones_cfg_rev);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);

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
     * uart_bridge_ext_run_on_flash_worker() is not reachable on-worker from
     * here: nvs_save() is called from ordinary httpd/executor/adaptive-tune
     * contexts, never from a job already running on the flash worker
     * itself, so no is_on_flash_worker() re-entrancy guard is needed (see
     * flash_worker_lint.py's own header comment for this justification
     * phrase's precedent). Best-effort: a dispatch/autosave failure is
     * logged, never turned into this function's own return value -- the
     * zones write ITSELF already fully succeeded by this point. */
    esp_err_t autosave_dispatch_err =
        uart_bridge_ext_run_on_flash_worker(zones_autosave_job, (void *)xTaskGetCurrentTaskHandle());
    if (autosave_dispatch_err != ESP_OK) {
        ESP_LOGW(ZONES_HTTP_TAG, "kiln config autosave could not be dispatched: %s -- the active kiln "
                      "package was NOT updated with this change, though the change itself was saved",
                 esp_err_to_name(autosave_dispatch_err));
    }

    return hal_status_to_esp_err(err);
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

/* Loads s_relay_names.cfg from NVS_KEY_RELAY_NAMES (same namespace/partition
 * as the zones blob -- see NVS_NAMESPACE/KILN_NVS_PARTITION above). Resets to
 * all-empty names -- not reported to the caller as a failure, this is purely
 * cosmetic data, unlike s_zones_config_valid's safety-relevant "cannot
 * trust this" gate -- on: namespace/key not found (first boot, or a board
 * that has never named a relay), a blob whose length doesn't match
 * sizeof(relay_names_cfg_t) (only one version exists so far, so there is
 * only one valid length, but the check is written the same "length before
 * interpretation" way zones_config_json_decode_blob() uses for the zones blob, not
 * skipped just because there is nothing to switch on yet), an unrecognized
 * version, or a CRC mismatch. RELAY_NAMES_CFG_VERSION exists now, before
 * there is a second version to get wrong, specifically so a future format
 * change has an established version field to switch on instead of repeating
 * this codebase's own "grew the struct, forgot the old layout" history (see
 * ZONES_CFG_VERSION's 6->7 comment for exactly what that mistake cost). */
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
            if (len != sizeof(relay_names_cfg_t)) {
                ESP_LOGE(ZONES_HTTP_TAG,
                         "relay_names blob is %u bytes, expected %u -- discarding NVS candidate",
                         (unsigned)len, (unsigned)sizeof(relay_names_cfg_t));
            } else if (!relay_names_validate(raw, sizeof(raw))) {
                ESP_LOGE(ZONES_HTTP_TAG,
                         "relay_names NVS blob failed version/CRC validation -- discarding NVS candidate");
            } else {
                memcpy(&nvs_cand, raw, sizeof(nvs_cand));
                nvs_valid = true;
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

esp_err_t relay_names_save(void)
{
    s_relay_names.cfg.version = RELAY_NAMES_CFG_VERSION;
    s_relay_names.cfg.crc32 = compute_relay_names_crc(&s_relay_names.cfg);
    uint32_t new_rev = s_relay_names_rev + 1;

    /* FILE FIRST (best-effort; a failure here is logged and swallowed --
     * NVS below remains the persistence guarantee every existing caller
     * already depends on), THEN NVS (authoritative, failure returned to the
     * caller) -- same policy unit_pref_set()/zones_config_cfg_fs.c's step-5
     * note document. */
    esp_err_t file_err =
        pref_cfg_fs_save(RELAY_NAMES_FILE_PATH, &s_relay_names.cfg, sizeof(s_relay_names.cfg), new_rev);
    if (file_err != ESP_OK && file_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(ZONES_HTTP_TAG, "relay names file write failed: %s -- NVS remains the source of truth this boot",
                 esp_err_to_name(file_err));
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_RELAY_NAMES, &s_relay_names.cfg, sizeof(s_relay_names.cfg));
    if (err == HAL_OK) {
        err = hal_kv_set_u32(&h, NVS_KEY_RELAY_NAMES_REV, new_rev);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        ESP_LOGW(ZONES_HTTP_TAG, "relay_names_save failed: %s -- relay names will not survive a reboot",
                 hal_status_to_name(err));
    } else {
        s_relay_names_rev = new_rev;
    }
    return hal_status_to_esp_err(err);
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
/* 1 -> 2 (M12): the sweep now also derives which CT channel watches which
 * zone (ct_map_*, see zone_sweep_derive_ct_channel() below) and that record
 * has to outlive the sweep task -- the commissioning page reads it on a
 * later page load to decide whether ct_channel_map[0..2] renders as DERIVED
 * or as a manual-entry field. A v1 blob is discarded by the version check
 * below rather than migrated: the whole blob is re-measured by one button
 * press, and the alternative (reading a short struct and zero-filling the
 * tail) is a migration path worth writing only for data that cannot simply
 * be measured again. */
/* 2 -> 3 (M12b): the sweep now also derives k_ct_v_per_a[0..2] from the
 * nameplate power the operator already answered (COMMISSIONING_UX.md Q3/Q4)
 * and this run's own measured current, and the commissioning page has to be
 * able to say DERIVED on a later page load for that field too. A v2 blob is
 * discarded rather than migrated, for exactly the reason v1 was: one button
 * press re-measures the whole thing. */
#define ZONE_NORMALS_CFG_VERSION 3
/* RENAMED 2026-09-06 from "zone_normals_cfg" (16 chars) -- confirmed against
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
 * zone_normals_save() logged nothing on a non-OK return (unlike every
 * sibling setter in this file) -- fixed in the 2026-09-07 persist-logging
 * audit (docs/audits/persist_save_logging_2026-09-07.md), zone_normals_save()
 * now logs an ESP_LOGW naming the module and esp_err_to_name() on failure.
 * At the time the key-length bug was live, EVERY zone_normals_set()/
 * zone_ct_map_set()/zone_k_ct_set() write since that commit has silently
 * failed at nvs_set_blob() with ESP_ERR_NVS_KEY_TOO_LONG -- measured normal
 * currents and derived CT-channel/k_ct_v_per_a maps have never actually
 * persisted across a reboot on this board. Because nothing was ever
 * written under the old name, there is no on-flash data to migrate: the
 * rename below is a plain one-time swap, not a migration. See
 * fake_kv.h's FAKE_KV_MAX_KEY_LEN (also 15 usable chars, deliberately kept
 * equal to NVS's real limit rather than raised) -- it is what caught this
 * during the nvs.h -> hal_kv.h migration's host-test pass. */
#define NVS_KEY_ZONE_NORMALS "zone_norm_cfg"

/* Compile-time guard so this class of bug (a >15-char NVS key that silently
 * never persists on real hardware) cannot recur in this file: every
 * NVS_KEY_* literal used here must fit ESP-IDF's real NVS_KEY_NAME_MAX_SIZE
 * (16 bytes INCLUDING the NUL terminator, i.e. 15 usable characters). Macro
 * shared via nvs_key_check.h so every other module with NVS key literals
 * gets the identical check. */
NVS_KEY_LEN_CHECK(NVS_KEY_ZONES);
NVS_KEY_LEN_CHECK(NVS_KEY_RELAY_NAMES);
NVS_KEY_LEN_CHECK(NVS_KEY_ZONE_NORMALS);

typedef struct {
    uint8_t  version;
    uint8_t  measured_mask; /* bit i = zone i has a measured normal current */
    float    normal_current_a[MAX31856_CHANNEL_COUNT];
    /* v2: the derived CT-channel -> zone mapping. bit c of
     * ct_map_derived_mask set means ct_map_zone[c] is a zone index the sweep
     * derived UNAMBIGUOUSLY (COMMISSIONING_UX.md sec 1.2's condition); a
     * clear bit means "never derived", and ct_map_zone[c] is meaningless. */
    uint8_t  ct_map_derived_mask;
    uint8_t  ct_map_zone[ZONE_CT_CHANNEL_COUNT];
    /* v3: the derived CT volts-per-amp scale. bit c of k_ct_derived_mask set
     * means k_ct_v_per_a[c] is a value this board CALIBRATED from a complete
     * sweep and confirmed written to the safety processor; a clear bit means
     * "never derived here" and k_ct_v_per_a[c] is meaningless -- it says
     * nothing about whether the Pico's own k_ct_v_per_a[c] is set, which an
     * operator may always have entered by hand. */
    uint8_t  k_ct_derived_mask;
    float    k_ct_v_per_a[ZONE_CT_CHANNEL_COUNT];
    uint32_t crc32;
} zone_normals_cfg_t;

static struct {
    zone_normals_cfg_t cfg;
} s_zone_normals;

static uint32_t compute_zone_normals_crc(const zone_normals_cfg_t *cfg)
{
    zone_normals_cfg_t tmp = *cfg;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

/* Same "reset to blank, log, move on" convention as relay_names_load() --
 * this is measured convenience data, not safety state, so a bad blob is
 * simply forgotten (every zone reads back as "never measured") rather than
 * blocking anything. */
void zone_normals_load(void)
{
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return;
    }
    uint8_t raw[sizeof(zone_normals_cfg_t)];
    size_t len = sizeof(raw);
    err = hal_kv_get_blob(&h, NVS_KEY_ZONE_NORMALS, raw, &len);
    hal_kv_close(&h);
    if (err != HAL_OK) {
        return; /* never saved, or a read error -- blank is safe either way */
    }
    if (len != sizeof(zone_normals_cfg_t)) {
        ESP_LOGE(ZONES_HTTP_TAG, "zone_normals blob is %u bytes, expected %u -- discarding",
                 (unsigned)len, (unsigned)sizeof(zone_normals_cfg_t));
        return;
    }
    zone_normals_cfg_t cand;
    memcpy(&cand, raw, sizeof(cand));
    if (cand.version != ZONE_NORMALS_CFG_VERSION) {
        ESP_LOGE(ZONES_HTTP_TAG, "zone_normals blob version %u is not %u -- discarding", cand.version,
                 ZONE_NORMALS_CFG_VERSION);
        return;
    }
    uint32_t computed = compute_zone_normals_crc(&cand);
    if (computed != cand.crc32) {
        ESP_LOGE(ZONES_HTTP_TAG, "zone_normals blob CRC mismatch -- discarding");
        return;
    }
    s_zone_normals.cfg = cand;
}

static esp_err_t zone_normals_save(void)
{
    s_zone_normals.cfg.version = ZONE_NORMALS_CFG_VERSION;
    s_zone_normals.cfg.crc32 = compute_zone_normals_crc(&s_zone_normals.cfg);

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_ZONE_NORMALS, &s_zone_normals.cfg, sizeof(s_zone_normals.cfg));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        ESP_LOGW(ZONES_HTTP_TAG, "zone_normals_save failed: %s -- measured normals/CT map/k_ct will not "
                                  "survive a reboot",
                 hal_status_to_name(err));
    }
    return hal_status_to_esp_err(err);
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
    s_zone_normals.cfg.normal_current_a[zone_index] = amps;
    s_zone_normals.cfg.measured_mask |= (uint8_t)(1u << zone_index);
    return zone_normals_save() == ESP_OK;
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
    if ((s_zone_normals.cfg.measured_mask & zone_mask) == 0) {
        return true; /* nothing measured in the affected set -- nothing to invalidate */
    }
    s_zone_normals.cfg.measured_mask &= (uint8_t)~zone_mask;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (zone_mask & (1u << zi)) {
            s_zone_normals.cfg.normal_current_a[zi] = 0.0f;
        }
    }
    return zone_normals_save() == ESP_OK;
}

/* Same "the sweep is the only legitimate writer" rule zone_normals_set()
 * above states, applied to the derived CT map: this is measured data, and
 * an operator who wants to say it by hand says it on the commissioning page
 * (which writes the Pico's ct_channel_map directly), never here. Clearing
 * the whole record at the START of a sweep is deliberate -- a re-sweep of
 * rewired hardware must not leave a channel's stale "derived" claim behind
 * to be shown as current. */
void zone_ct_map_clear(void)
{
    s_zone_normals.cfg.ct_map_derived_mask = 0;
    memset(s_zone_normals.cfg.ct_map_zone, 0, sizeof(s_zone_normals.cfg.ct_map_zone));
    /* Persisted immediately, not left for the first zone_ct_map_set() to
     * flush: a sweep that clears the map and then fails outright never
     * reaches a set(), and leaving the old map in NVS would resurrect it on
     * the next boot as though it were still current. Failure is already
     * logged inside zone_normals_save() itself. */
    esp_err_t clear_err = zone_normals_save();
    (void)clear_err;
}

bool zone_ct_map_set(uint8_t ct_channel, uint8_t zone_index)
{
    if (ct_channel >= ZONE_CT_CHANNEL_COUNT || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    s_zone_normals.cfg.ct_map_zone[ct_channel] = zone_index;
    s_zone_normals.cfg.ct_map_derived_mask |= (uint8_t)(1u << ct_channel);
    return zone_normals_save() == ESP_OK;
}

/* M12b: same discipline as zone_ct_map_clear()/zone_ct_map_set() just above
 * -- this record is provenance ONLY. The value the guards and the power
 * estimate actually use lives on the Pico and is written by
 * zone_sweep_push_k_ct_v_per_a(); nothing here is ever read back as a
 * calibration. */
void zone_k_ct_clear(void)
{
    s_zone_normals.cfg.k_ct_derived_mask = 0;
    memset(s_zone_normals.cfg.k_ct_v_per_a, 0, sizeof(s_zone_normals.cfg.k_ct_v_per_a));
    /* Failure is already logged inside zone_normals_save() itself. */
    esp_err_t clear_err = zone_normals_save();
    (void)clear_err;
}

bool zone_k_ct_set(uint8_t ct_channel, float k_v_per_a)
{
    if (ct_channel >= ZONE_CT_CHANNEL_COUNT || !isfinite(k_v_per_a) || k_v_per_a <= 0.0f) {
        return false;
    }
    s_zone_normals.cfg.k_ct_v_per_a[ct_channel] = k_v_per_a;
    s_zone_normals.cfg.k_ct_derived_mask |= (uint8_t)(1u << ct_channel);
    return zone_normals_save() == ESP_OK;
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

