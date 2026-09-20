#include "kiln_cfg_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" /* kiln_cfg_store_lock()/_unlock() -- H6, docs/KILN_PROFILES_PLAN.md item 5 */
#include "freertos/task.h"   /* xTaskGetCurrentTaskHandle() -- autosave-override owner identity, 2026-09-16 */

#include "hal_time.h" /* hal_time_now_us() -- rate-limited deferred-autosave WARN, HIGH 1 fix */
#include "cfg_fs_status.h"
#include "ota_state.h"
#include "zones_config_accessors.h"
#include "zones_config_json.h" /* zones_cfg_t -- needed by populate_pico_half_and_hash()'s H1
                                 * canonical-hash path (zones_config_export_canonical()); every
                                 * other function in this file only ever touched zones_cfg_t as an
                                 * opaque `void *`/`uint8_t[]` blob via zones_config_accessors.h's
                                 * deliberately type-erased export/import pair. */
#include "zones_config_query.h" /* zones_config_get_thermo_count() -- upload compatibility check
                                  * (docs/KILN_PROFILES_PLAN.md item 14), section 5.2a. */
#include "config_divergence.h" /* CONFIG_DIVERGENCE_REASON_MAX -- HIGH 1 autosave-suppression check */
#include "safety_ceiling_sync.h" /* safety_ceiling_expected_param_t (full definition) + SAFETY_PARAM_ID_ABS_MAX_TEMP_C
                                   * -- kiln_cfg_store_capture_expected_pico_fields(), 2026-09-15 audit fix */
#include "safety_cfg_store.h" /* safety_cfg_store_lookup() -- upload's "unknown Pico param id"
                                * validity check, section 5.2 rule 5. */

#include "kiln_cfg_store_internal.h"
#include "kiln_cfg_store_cfg_fs.h"
#include "kiln_package.h" /* kiln_package_capture_pico_half()/_compute_hash() -- explicit even
                            * though kiln_cfg_store_internal.h already drags this in transitively,
                            * since this file (not that header) is the one that actually calls
                            * into it. */
#include "kiln_board_identity.h" /* kiln_board_identity_get() -- section 5.3 rows 2/3 (mint/compare
                                   * a board id for cross-board CT-calibration invalidation) and
                                   * row 4 (ack_hardware_differs), 2026-09-16. */

static const char *TAG = "kiln_cfg_store";

/* kiln_nvs (0x18D000, 64KB) already holds zones/rules/relay_cycles/run_state
 * (see partitions.csv, TODO.md 8.1's 2026-08-13 split) -- a saved kiln config
 * belongs alongside them for the identical reason: reprogramming this board
 * only ever rewrites bootloader/partition-table/app (verified against this
 * project's actual OTA/JTAG flash args), so every `data` partition, kiln_nvs
 * included, survives a reflash untouched. Do not move this to a new
 * partition or the default `nvs` one -- see kiln_cfg_store.h's header
 * comment for the fuller version of this note. */
#define KILN_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);

/* Shared namespace name with zones_http.c/rules_http.c/relay_cycles.c/
 * run_state.c/ota_record.c/profiles_builtin.c/profiles_http.c/unit_pref.c --
 * each of those files independently #defines the identical "kiln_cfg"
 * string, same convention this file follows, distinguished by KEY not
 * namespace. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_STORE "kilncfgs"
/* Separate tiny NVS key for the dual-write rev counter, deliberately NOT a
 * field inside kiln_cfg_store_blob_t -- same reasoning as
 * zones_config_store.c's NVS_KEY_ZONES_REV: a rev counter that lived inside
 * the versioned blob would itself need a migration branch every time it
 * changed, and older firmware rewriting the blob without knowing about the
 * counter is exactly the rolled-back-firmware case the STRICT tie-break
 * (kiln_cfg_store_cfg_fs.c) has to detect. */
#define NVS_KEY_STORE_REV "kilncfgrv"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_STORE);
NVS_KEY_LEN_CHECK(NVS_KEY_STORE_REV);

/* KILN_CFG_STORE_VERSION (currently 2) and the current-version
 * kiln_cfg_entry_t/kiln_cfg_store_blob_t layout now live in
 * kiln_cfg_store_internal.h, shared with kiln_cfg_store_cfg_fs.c (the `cfg`
 * LittleFS dual-write bridge for this whole store, docs/
 * FILESYSTEM_USER_DATA_PLAN.md section 5's "kiln config slots" item) -- see
 * that header for the version-bump/migration discipline this mirrors from
 * zones_http.c's ZONES_CFG_VERSION. The frozen v1 layout below stays
 * private here: nothing outside this file's own migration path ever needs
 * it.
 *
 * v1's blob ceiling. ZONES_CONFIG_BLOB_MAX_SIZE was widened 512 -> 640 on
 * 2026-08-30 (zone_cfg_t gained the four PID_EXPANSION_PLAN.md Phase 2
 * fields), and because that macro sizes a member of the PERSISTED struct
 * below -- not just a runtime ceiling -- widening it changed
 * sizeof(kiln_cfg_store_blob_t). nvs_load_store()'s `len != sizeof(loaded)`
 * check would then have read every existing board's saved store as
 * corruption and silently discarded every named kiln config and active_id.
 * Frozen here so the v1 layout can still be read and migrated. */
#define KILN_CFG_STORE_BLOB_MAX_SIZE_V1 512u

/* Both frozen layouts below hardcode 8, NEVER KILN_CFG_MAX_COUNT (now 10) --
 * this is deliberate and load-bearing, not an oversight. KILN_CFG_MAX_COUNT
 * bumped 8 -> 10 in this same change (docs/KILN_PROFILES_PLAN.md item 1); a
 * frozen historical layout that silently tracked the live macro would grow
 * out from under itself the moment the macro changed again, making the
 * exact-size migration detection below (`stored_len == sizeof(kiln_cfg_
 * store_blob_v1_t)` / `..._v2_t`) compare against the WRONG size and either
 * mis-detect a v1/v2 blob as corrupt or, worse, misinterpret a differently-
 * shaped blob as one of these. Every historical layout's slot count is a
 * fact about bytes already on boards' flash, frozen at the value it was
 * written with -- never re-derived from a macro that keeps changing. */
#define KILN_CFG_STORE_V1_COUNT 8u
#define KILN_CFG_STORE_V2_COUNT 8u

/* ---- Frozen v1 on-flash layout -------------------------------------------
 * Used ONLY to reinterpret a stored v1 blob during migration. Never grown,
 * never reused: same discipline as zones_http.c's zone_cfg_v*_t snapshots,
 * and for the same reason -- a historical layout that no longer matches the
 * bytes actually in flash is worse than no migration at all. */
typedef struct {
    uint8_t in_use;
    int32_t id;
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    uint16_t blob_len;
    uint8_t blob[KILN_CFG_STORE_BLOB_MAX_SIZE_V1];
} kiln_cfg_entry_v1_t;

typedef struct {
    uint8_t version;
    int32_t active_id;
    int32_t next_id;
    kiln_cfg_entry_v1_t entries[KILN_CFG_STORE_V1_COUNT];
} kiln_cfg_store_blob_v1_t;

/* ---- Frozen v2 on-flash layout --------------------------------------------
 * This WAS kiln_cfg_store_blob_t/kiln_cfg_entry_t before docs/KILN_PROFILES_
 * PLAN.md items 1/2/12 (8 slots, ESP-only blob, no Pico half, no package
 * identity). Frozen here, unchanged field-for-field, purely so a v2 blob
 * already on a board's flash can still be read and migrated -- same
 * discipline as the v1 layout just above. Uses ZONES_CONFIG_BLOB_MAX_SIZE
 * (not a frozen numeric literal) because that ceiling has NOT changed
 * between v2 and v3 -- only the slot count and the addition of new trailing
 * fields did -- so tying it to the live macro here is correct, unlike the
 * v1 blob-size macro above which WAS itself the thing that changed. */
typedef struct {
    uint8_t in_use;
    int32_t id;
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    uint16_t blob_len;
    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
} kiln_cfg_entry_v2_t;

typedef struct {
    uint8_t version;
    int32_t active_id;
    int32_t next_id;
    kiln_cfg_entry_v2_t entries[KILN_CFG_STORE_V2_COUNT];
} kiln_cfg_store_blob_v2_t;

/* Budget guard: kiln_cfg_store_blob_t is a permanent member of s_store
 * (static, BSS-resident); the v1/v2 frozen structs are heap-allocated only
 * transiently, on their once-ever migration paths in nvs_load_store() below
 * (malloc'd, freed before that function returns) -- neither is ever an
 * ordinary function-local/stack buffer, so this is no longer a stack budget.
 * What it actually bounds now: s_store's permanent BSS footprint plus the
 * transient heap high-water mark either migration path can hit, added
 * together as a single loose tripwire so a future ZONES_CONFIG_BLOB_MAX_SIZE
 * widening (or KILN_CFG_MAX_COUNT bump, or KILN_PKG_SAFETY_PARAM_CAP bump)
 * gets caught here instead of silently growing the cost. Raised 16384 ->
 * 40000 for v3's Pico-half addition (kiln_pkg_safety_t adds ~770B/entry *
 * 10 slots) -- still deliberately loose, not a real budget; the point is
 * only to force a human back to this comment before any of these structs
 * doubles again. */
/* Portable compile-time assert (not _Static_assert): this file is compiled
 * both by the ESP-IDF (xtensa-gcc, C11) build and, #included directly, by
 * this repo's MSVC host tests (test_kiln_cfg_store.c) which are not
 * necessarily invoked in C11 mode. A negative array size is a hard error in
 * every C standard this file has ever been built under. */
typedef char kiln_cfg_store_blob_budget_check
    [(sizeof(kiln_cfg_store_blob_t) + sizeof(kiln_cfg_store_blob_v1_t) + sizeof(kiln_cfg_store_blob_v2_t) < 40000)
         ? 1
         : -1];

/* SAFETY_CFG_PARAM_COUNT must fit inside kiln_pkg_safety_t's fixed capacity
 * -- see kiln_package.h's own comment on why the cap is deliberately larger
 * than today's count. This is the forcing function: CONFIG_PARAM_TABLE
 * growing past the cap fails this build loudly, at compile time, rather than
 * kiln_package_capture_pico_half() silently refusing every save at runtime. */
typedef char kiln_cfg_store_pico_cap_check[(SAFETY_CFG_PARAM_COUNT <= KILN_PKG_SAFETY_PARAM_CAP) ? 1 : -1];

/* Migrates a v1 store into the v2 shape: every field copied by name, the
 * shorter v1 blob copied into the wider array and the remainder left
 * zeroed. Lossless -- a v1 blob is a complete zones config that a v1-era
 * build wrote, and zones_http.c's own decoder handles its version separately
 * (it carries its own ZONES_CFG_VERSION inside those bytes). Renamed from
 * the old migrate_store_v1_to_current() now that "current" is v3, one step
 * further along the chain -- this function's OWN target shape (v2) never
 * changes regardless of where "current" moves next. */
static void migrate_store_v1_to_v2(const kiln_cfg_store_blob_v1_t *src, kiln_cfg_store_blob_v2_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->version = 2;
    dst->active_id = src->active_id;
    dst->next_id = src->next_id;
    for (size_t i = 0; i < KILN_CFG_STORE_V1_COUNT && i < KILN_CFG_STORE_V2_COUNT; i++) {
        const kiln_cfg_entry_v1_t *se = &src->entries[i];
        kiln_cfg_entry_v2_t *de = &dst->entries[i];
        de->in_use = se->in_use;
        de->id = se->id;
        memcpy(de->name, se->name, sizeof(de->name));
        de->name[sizeof(de->name) - 1] = '\0';
        uint16_t n = se->blob_len;
        if (n > KILN_CFG_STORE_BLOB_MAX_SIZE_V1) {
            n = KILN_CFG_STORE_BLOB_MAX_SIZE_V1; /* stored length can never exceed v1's own array */
        }
        de->blob_len = n;
        memcpy(de->blob, se->blob, n);
    }
}

/* Migrates a v2 store into the current (v3) layout: every ESP-side field
 * copied by name, unchanged; the new v3-only fields (pico_populated,
 * pkg_schema, pkg_hash, pico) are left zeroed -- pico_populated=0 explicitly
 * marks these slots as "no Pico half captured" (see kiln_cfg_entry_t's own
 * comment, kiln_cfg_store_internal.h) rather than fabricating one from
 * nothing. The two NEW slots (index 8, 9) this bump adds are left zeroed/
 * not-in-use, same as any other never-used slot. Lossless for every v2
 * field: a user's existing 8 saved kiln configs, their names, ids, and
 * active_id/next_id all survive this migration untouched. */
static void migrate_store_v2_to_v3(const kiln_cfg_store_blob_v2_t *src, kiln_cfg_store_blob_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->version = KILN_CFG_STORE_VERSION;
    dst->active_id = src->active_id;
    dst->next_id = src->next_id;
    for (size_t i = 0; i < KILN_CFG_STORE_V2_COUNT && i < KILN_CFG_MAX_COUNT; i++) {
        const kiln_cfg_entry_v2_t *se = &src->entries[i];
        kiln_cfg_entry_t *de = &dst->entries[i];
        de->in_use = se->in_use;
        de->id = se->id;
        memcpy(de->name, se->name, sizeof(de->name));
        de->name[sizeof(de->name) - 1] = '\0';
        de->blob_len = se->blob_len;
        memcpy(de->blob, se->blob, sizeof(de->blob));
        /* pico_populated/pkg_schema/pkg_hash/pico already zeroed by the
         * memset above -- explicitly NOT set here. */
    }
}

static kiln_cfg_store_blob_t s_store;

/* Dual-write rev counter for the whole store document -- see
 * kiln_cfg_store_cfg_fs.h's header comment ("SHAPE") for why this is ONE
 * counter for the whole blob rather than a per-slot one like profiles_cfg_fs.c:
 * every mutation (save/clone/apply/delete/rename) already funnels through
 * nvs_save_store() below, so bumping it there once covers every slot,
 * including a delete -- exactly the "bump the rev on both save and delete"
 * discipline profiles_cfg_fs.c established, generalized to a single-document
 * store. Process-wide static, same convention as zones_config_store.c's
 * s_zones_cfg_rev: it is NOT reset by anything short of a real
 * nvs_load_store() resync. */
static uint32_t s_kiln_cfg_rev = 0;

/* ---- H3: corrupt-store quarantine ------------------------------------------
 * docs/audits/kiln_profiles_robustness_2026-09-14.md finding H3: a corrupt
 * store used to silently reset_to_defaults() with one ESP_LOGW, presenting
 * itself as a legitimately empty store. The very next auto-save (plan item
 * 13, not yet implemented, but nothing stops today's ordinary save/clone
 * either) would then overwrite the ONLY copy of the operator's saved kiln
 * configs -- both NVS and the `cfg` mirror -- with no operator action. That
 * is strictly worse than not having this feature.
 *
 * DELIBERATELY IN-RAM ONLY, no separate persisted quarantine key: the
 * corrupt bytes on flash are themselves what makes nvs_load_store()
 * re-derive this same quarantined state on every boot -- there is nothing
 * to keep in sync between two stores (the exact "reset one side of a pair"
 * bug class CLAUDE.md warns about), because there is only one store. Once
 * kiln_cfg_store_quarantine_clear() erases the blob key and writes a fresh,
 * valid, empty one, the NEXT boot reads a legitimately valid store and
 * never re-quarantines -- no flag to forget to clear alongside it. */
static bool s_quarantined = false;
static char s_quarantine_reason[128] = "";

static void set_quarantine(const char *reason)
{
    s_quarantined = true;
    strncpy(s_quarantine_reason, reason, sizeof(s_quarantine_reason) - 1);
    s_quarantine_reason[sizeof(s_quarantine_reason) - 1] = '\0';
    ESP_LOGE(TAG, "kiln_cfg_store QUARANTINED: %s -- every write (save/clone/rename/delete) is refused "
                  "until an operator explicitly clears it (POST /api/kiln_configs/quarantine_clear); "
                  "the live zones config on this board is UNAFFECTED",
             s_quarantine_reason);
}

/* ---- NVS ------------------------------------------------------------------ */

/* Copied from zones_http.c's nvs_partition_init() (see that file for the
 * full rationale) -- each module using kiln_nvs brings the partition up
 * independently rather than assuming another module already has.
 * hal_kv_init_partition() already implements the erase-and-retry idiom this
 * used to do by hand (see hal_kv_esp.c). */
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

/* Forward decl -- see the full doc comment on the second declaration further
 * down, next to kiln_cfg_store_set_active_id_raw(). Declared this early
 * because the 2026-09-15 review's LOW 7 follow-up found five OTHER writers
 * of s_store.active_id above that point, all of which must drop a pending
 * per-slot recapture flag owned by the slot they are moving away from. */
static void pico_half_dirty_drop_if_owned_by(int32_t slot);

static void reset_to_defaults(void)
{
    /* LOW 7 (2026-09-15 review follow-up): s_pico_half_dirty/_slot are
     * file statics, NOT part of s_store, so the memset below does not clear
     * them -- a pending recapture would survive a reset-to-defaults and then
     * apply to whatever slot became active later. Drop it first. */
    pico_half_dirty_drop_if_owned_by(s_store.active_id);
    memset(&s_store, 0, sizeof(s_store));
    s_store.version = KILN_CFG_STORE_VERSION;
    s_store.active_id = KILN_CFG_NO_ACTIVE_ID;
    s_store.next_id = 1;
}

/* Loads the persisted store blob, or defaults to an empty store if nothing
 * was ever saved. One migration branch exists today -- a v1-sized blob is
 * upgraded via migrate_store_v1_to_current() below, mirroring
 * zones_http.c's nvs_load_from()/migrate_zones_cfg_v1_to_current(). Follows
 * that same file's "refuse and leave flash alone, don't reset-and-treat-as-
 * corrupt" discipline for a newer-than-firmware blob specifically -- see the
 * version-check block below for the per-branch reasoning (zones_http.c's
 * FIX 1 found exactly this distinction missing there). A future version
 * bump beyond 2 needs its own branch added alongside the v1 one. */
/* Defined below; nvs_load_store() calls it to rewrite a migrated v1 store in
 * the current layout. */
static hal_status_t nvs_save_store(void);

/* Returns true iff s_store now holds something trustworthy from NVS (either
 * the current-version happy path, or a successful v1 migration); false for
 * every "defaults stand" branch. kiln_cfg_store_cfg_fs_resolve() needs this
 * flag as its own `nvs_valid` input -- the same "found/valid must never be
 * reconstructed from a zeroed struct" fix zones_http.c's nvs_load_from()
 * already applies (see that function's FIX 1 comment). */
static bool nvs_load_store(void)
{
    reset_to_defaults();
    /* Re-derived fresh on every call -- see s_quarantined's own comment for
     * why there is no separate persisted flag to fall out of sync with
     * this. A board that boots cleanly after a previous quarantined boot
     * (e.g. the operator cleared it) must not still report quarantined. */
    s_quarantined = false;
    s_quarantine_reason[0] = '\0';

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false; /* HAL_NOT_FOUND (never saved) or partition trouble -- defaults stand */
    }

    /* Read the size first, so a v1-sized blob can be migrated instead of
     * being dismissed as corruption by the exact-size check below. */
    size_t stored_len = 0;
    err = hal_kv_get_blob(&h, NVS_KEY_STORE, NULL, &stored_len);
    if (err != HAL_OK) {
        hal_kv_close(&h);
        return false; /* nothing stored, or unreadable -- defaults stand */
    }

    if (stored_len == sizeof(kiln_cfg_store_blob_v1_t)) {
        /* A board saved by a pre-2026-08-30 build. Its entries are the same
         * data, just with a 512-byte blob array instead of 640/896. Migrate
         * rather than discard: this is a user's named kiln configs. Chains
         * through v2 (kiln_cfg_entry_v2_t, matching this era's real
         * on-flash shape) before reaching v3 -- migrate_store_v1_to_v2()'s
         * OWN target shape never moves, so this chain does not need to
         * change again the next time "current" advances past v3.
         *
         * Heap-allocated, not `static`/stack: sizeof(kiln_cfg_store_blob_v1_t)
         * is ~4.4KB (KILN_CFG_STORE_V1_COUNT=8 entries, each
         * KILN_CFG_STORE_BLOB_MAX_SIZE_V1=512 blob + header/padding). This
         * branch runs at most once per board (the very next boot takes the
         * fast, already-current-version path below), so it is not worth that
         * many bytes of *permanent* BSS the other ~99.99% of boots never
         * touch. malloc() failure is handled exactly like the "unreadable,
         * defaults stand" branch a few lines below -- there is nothing
         * special about running out of heap here versus any other read
         * failure. */
        kiln_cfg_store_blob_v1_t *v1 = malloc(sizeof(*v1));
        if (!v1) {
            hal_kv_close(&h);
            ESP_LOGW(TAG, "kiln_cfg_store v1 migration buffer alloc failed -- defaults stand");
            return false;
        }
        size_t v1_len = sizeof(*v1);
        err = hal_kv_get_blob(&h, NVS_KEY_STORE, v1, &v1_len);
        hal_kv_close(&h);
        if (err != HAL_OK || v1_len != sizeof(*v1)) {
            set_quarantine("a v1-sized kiln config blob existed but could not be fully re-read (truncated "
                           "or corrupted on flash)");
            free(v1);
            return false;
        }
        if (v1->version != 1) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "kiln config blob is v1-SIZED but claims version %u, which is not a v1 blob",
                     (unsigned)v1->version);
            set_quarantine(msg);
            free(v1);
            return false;
        }
        kiln_cfg_store_blob_v2_t *v2 = malloc(sizeof(*v2));
        if (!v2) {
            free(v1);
            ESP_LOGW(TAG, "kiln_cfg_store v1->v2 migration buffer alloc failed -- defaults stand");
            return false;
        }
        migrate_store_v1_to_v2(v1, v2);
        free(v1);
        migrate_store_v2_to_v3(v2, &s_store);
        free(v2);
        ESP_LOGI(TAG, "kiln_cfg_store migrated v1 -> v%u (blob ceiling %u -> %u, %u -> %u slots); saved "
                      "kiln configs kept, no Pico half (never captured by v1-era firmware)",
                 (unsigned)KILN_CFG_STORE_VERSION, (unsigned)KILN_CFG_STORE_BLOB_MAX_SIZE_V1,
                 (unsigned)ZONES_CONFIG_BLOB_MAX_SIZE, (unsigned)KILN_CFG_STORE_V1_COUNT,
                 (unsigned)KILN_CFG_MAX_COUNT);
        hal_status_t save_err = nvs_save_store(); /* rewrite in the current layout so the next boot takes the fast path */
        if (save_err != HAL_OK) {
            ESP_LOGW(TAG, "kiln_cfg_store v1->v%u rewrite failed: %s -- will re-migrate next boot",
                     (unsigned)KILN_CFG_STORE_VERSION, hal_status_to_name(save_err));
        }
        return true;
    }

    if (stored_len == sizeof(kiln_cfg_store_blob_v2_t)) {
        /* A board saved by a pre-KILN_PROFILES_PLAN build (8 slots, ESP-only,
         * no Pico half) -- docs/KILN_PROFILES_PLAN.md items 1/2/12. Same
         * heap-not-BSS reasoning as the v1 branch above; this one runs at
         * most once per board too. */
        kiln_cfg_store_blob_v2_t *v2 = malloc(sizeof(*v2));
        if (!v2) {
            hal_kv_close(&h);
            ESP_LOGW(TAG, "kiln_cfg_store v2 migration buffer alloc failed -- defaults stand");
            return false;
        }
        size_t v2_len = sizeof(*v2);
        err = hal_kv_get_blob(&h, NVS_KEY_STORE, v2, &v2_len);
        hal_kv_close(&h);
        if (err != HAL_OK || v2_len != sizeof(*v2)) {
            set_quarantine("a v2-sized kiln config blob existed but could not be fully re-read (truncated "
                           "or corrupted on flash)");
            free(v2);
            return false;
        }
        if (v2->version != 2) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "kiln config blob is v2-SIZED but claims version %u, which is not a v2 blob",
                     (unsigned)v2->version);
            set_quarantine(msg);
            free(v2);
            return false;
        }
        migrate_store_v2_to_v3(v2, &s_store);
        free(v2);
        ESP_LOGI(TAG, "kiln_cfg_store migrated v2 -> v%u (%u -> %u slots, Pico half added); saved kiln "
                      "configs kept, no Pico half on migrated slots until re-saved",
                 (unsigned)KILN_CFG_STORE_VERSION, (unsigned)KILN_CFG_STORE_V2_COUNT, (unsigned)KILN_CFG_MAX_COUNT);
        hal_status_t save_err = nvs_save_store();
        if (save_err != HAL_OK) {
            ESP_LOGW(TAG, "kiln_cfg_store v2->v%u rewrite failed: %s -- will re-migrate next boot",
                     (unsigned)KILN_CFG_STORE_VERSION, hal_status_to_name(save_err));
        }
        return true;
    }

    /* Read directly into s_store -- no separate whole-store scratch buffer.
     * Every rejection branch below (wrong size, unreadable, older version,
     * newer version) calls reset_to_defaults() before returning, so a read
     * that fails partway (leaving s_store partially overwritten) or succeeds
     * into a version this build refuses to use can never leave s_store in a
     * half-written or wrongly-versioned state -- behaviourally identical to
     * the previous "read into a scratch `loaded`, only copy to s_store on
     * the happy path" approach, at zero bytes of extra BSS/heap. */
    size_t len = sizeof(s_store);
    err = hal_kv_get_blob(&h, NVS_KEY_STORE, &s_store, &len);
    hal_kv_close(&h);
    if (err != HAL_OK) {
        reset_to_defaults(); /* nothing stored, or unreadable, or a partial read -- defaults stand */
        return false;
    }
    if (len != sizeof(s_store)) {
        /* Wrong size for ANY version's claimed layout is genuine corruption
         * -- a real blob is always written at exactly sizeof(s_store) (see
         * nvs_save_store()). H3 fix: this used to silently reset_to_
         * defaults() -- now it QUARANTINES (see set_quarantine()'s own
         * comment): the operator's saved configs are gone from the byte
         * count alone, but the bytes on flash are left untouched and every
         * write is refused until the operator explicitly discards them. */
        char msg[128];
        snprintf(msg, sizeof(msg), "kiln config blob is %u bytes, expected exactly %u for any known version",
                 (unsigned)len, (unsigned)sizeof(s_store));
        set_quarantine(msg);
        reset_to_defaults();
        return false;
    }
    if (s_store.version == KILN_CFG_STORE_VERSION) {
        return true; /* current version, right size -- happy path, s_store already holds it */
    }
    if (s_store.version < KILN_CFG_STORE_VERSION) {
        /* Reachable only for a version between 1 and 2 (both handled by the
         * size-based migration branches above) and KILN_CFG_STORE_VERSION
         * for which no migration chain has been written yet -- an older-version blob with
         * no defined conversion is exactly as unusable as a wrong-size one,
         * so it is quarantined (H3) rather than silently reset. */
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "kiln config blob is version %u, older than this firmware's %u, and no migration chain "
                 "exists for it",
                 (unsigned)s_store.version, (unsigned)KILN_CFG_STORE_VERSION);
        set_quarantine(msg);
        reset_to_defaults();
        return false;
    }
    /* s_store.version > KILN_CFG_STORE_VERSION: written by newer firmware
     * than this build -- the same firmware-rollback case zones_http.c's
     * nvs_load_from() guards against (see that function's comment, and its
     * FIX 1 note above). Refuse to load rather than reset flash: nothing on
     * flash is touched here regardless (this function never writes), but
     * s_store itself IS reset to defaults below -- unlike the old
     * scratch-buffer version, this function now reads straight into
     * s_store, so the newer-than-firmware bytes it just read must be wiped
     * back out of memory before returning, or the caller would be handed a
     * half-understood layout it never validated. kiln_cfg_store_init()
     * below only calls nvs_save_store() when s_store.active_id !=
     * KILN_CFG_NO_ACTIVE_ID, and reset_to_defaults() always clears
     * active_id, so this reset can never itself trigger the write-back that
     * would overwrite the newer blob on flash -- that write-back guard is
     * exactly why the distinct log message and this reasoning are written
     * down here rather than left for a future change to rediscover. */
    /* H3 fix: also quarantined, not only refused -- flash is left untouched
     * either way (this function never writes), but WITHOUT quarantine an
     * operator save on THIS boot would still stomp the newer blob via the
     * next nvs_save_store(), which writes s_store (now defaults) over
     * NVS_KEY_STORE unconditionally. Quarantining blocks exactly that
     * write until the operator acts -- e.g. flashing the firmware that
     * understands this version, or explicitly discarding it. */
    char newer_msg[128];
    snprintf(newer_msg, sizeof(newer_msg),
             "kiln config blob is version %u, newer than this firmware's %u -- flash data left untouched",
             (unsigned)s_store.version, (unsigned)KILN_CFG_STORE_VERSION);
    set_quarantine(newer_msg);
    reset_to_defaults();
    return false;
}

/* Reads NVS_KEY_STORE_REV -- the whole-store dual-write rev counter (see
 * s_kiln_cfg_rev's own comment). Same "0 if never written" convention as
 * zones_config_store.c's zones_cfg_rev_load(). */
static uint32_t kiln_cfg_rev_load(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return 0;
    }
    uint32_t rev = 0;
    err = hal_kv_get_u32(&h, NVS_KEY_STORE_REV, &rev);
    hal_kv_close(&h);
    return err == HAL_OK ? rev : 0;
}

/* Wraps nvs_load_store() with the `cfg` LittleFS read-through/dual-write
 * policy (docs/FILESYSTEM_USER_DATA_PLAN.md section 5, "kiln config slots"
 * item) -- exactly the shape zones_config_store.c's nvs_load() wraps
 * nvs_load_from() with. kiln_cfg_store_cfg_fs_resolve() never touches NVS
 * itself; it only decides whether the file or the NVS candidate above wins,
 * and may write a resync copy to whichever side lost. On every board today
 * (no `cfg` partition mounted) this is a fast no-op that hands the NVS
 * candidate straight back unchanged. */
static void nvs_load_store_with_cfg_fs(void)
{
    bool nvs_valid = nvs_load_store();
    uint32_t nvs_rev = kiln_cfg_rev_load();

    /* HEAP, never the stack: kiln_cfg_store_blob_t is ~7.5 KiB and this
     * runs on the `main` task (8192 B stack) during boot. A stack copy here
     * -- together with the identical one kiln_cfg_store_cfg_fs_load_raw()
     * used to keep -- overflowed the main task and panicked the board at
     * boot with IllegalInstruction: docs/audits/boot_hang_2026-09-08.md.
     * An allocation failure degrades exactly like "no cfg partition":
     * whatever nvs_load_store() already put in s_store stands. */
    kiln_cfg_store_blob_t *resolved = malloc(sizeof(*resolved));
    if (!resolved) {
        ESP_LOGW(TAG, "kiln cfg resolve scratch alloc failed (%u bytes) -- keeping the NVS candidate",
                 (unsigned)sizeof(*resolved));
        s_kiln_cfg_rev = nvs_rev;
        (void)nvs_valid;
        return;
    }
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool trustworthy =
        kiln_cfg_store_cfg_fs_resolve(&s_store, nvs_valid, nvs_rev, resolved, &resolved_rev, &used_file);
    s_kiln_cfg_rev = resolved_rev;
    if (used_file) {
        s_store = *resolved;
    }
    free(resolved);
    /* !trustworthy means neither side had anything valid -- s_store is
     * already the defaults nvs_load_store()'s own invalid-branch left in
     * place; nothing further to do. */
    (void)trustworthy;
}

/* True iff the CURRENTLY EXECUTING task's own stack lives in external RAM
 * (PSRAM). Same predicate, same reasoning, and same incident class as
 * safety_cfg_store.c's caller_stack_is_external(): a flash/NVS write
 * disables the cache, which makes a PSRAM-resident stack unreachable and
 * aborts the whole board via ESP-IDF's own
 * esp_task_stack_is_sane_cache_disabled() rather than failing just this one
 * call. DRAM_PSRAM_PLAN.md section 7.2 lists this hazard as the reason
 * whole classes of task must never get a PSRAM stack; this is the
 * belt-and-suspenders check for the write side, so a FUTURE caller that
 * reaches this function from a PSRAM-stacked task (directly, or by some
 * future refactor bypassing today's callers, all of which currently run on
 * internal-stack tasks) is refused loudly instead of taking the board down. */
static bool caller_stack_is_external(void)
{
    return !hal_kv_write_safe_here();
}

static hal_status_t nvs_save_store(void)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "nvs_save_store: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). A flash/NVS write from here would abort the whole board "
                      "(ESP-IDF's esp_task_stack_is_sane_cache_disabled()). Route this call "
                      "through a task with an internal-SRAM stack instead -- see "
                      "DRAM_PSRAM_PLAN.md section 7.2 and uart_bridge_ext.c's flash-safe "
                      "worker for the established pattern.");
        return HAL_NOT_READY;
    }
    s_store.version = KILN_CFG_STORE_VERSION;

    /* Dual-write, FILE FIRST -- docs/FILESYSTEM_USER_DATA_PLAN.md's
     * "kiln config slots" item, requirement 1: the file write's own failure
     * is logged inside kiln_cfg_store_cfg_fs_save() and otherwise swallowed
     * here, since NVS below is still authoritative for older firmware and
     * for every board today (no `cfg` partition mounted -- ESP_ERR_INVALID_
     * STATE is the expected, silent outcome). Every mutating public
     * function in this module (save/clone/apply/delete/rename) funnels
     * through this one function, so bumping the rev here once covers a
     * delete exactly the same as a save -- see s_kiln_cfg_rev's own comment
     * for why this store needs no separate per-slot rev the way
     * profiles_cfg_fs.c does. */
    s_kiln_cfg_rev++;
    (void)kiln_cfg_store_cfg_fs_save(&s_store, s_kiln_cfg_rev);

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_set_blob(&h, NVS_KEY_STORE, &s_store, sizeof(s_store));
    if (err == HAL_OK) {
        err = hal_kv_set_u32(&h, NVS_KEY_STORE_REV, s_kiln_cfg_rev);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err;
}

/* ---- Internal helpers ------------------------------------------------------ */

static int find_index_by_id(int32_t id)
{
    if (id < 0) {
        return -1;
    }
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        if (s_store.entries[i].in_use && s_store.entries[i].id == id) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void)
{
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        if (!s_store.entries[i].in_use) {
            return i;
        }
    }
    return -1;
}

static bool set_reason(char *reason_out, size_t reason_cap, const char *msg)
{
    if (reason_out && reason_cap) {
        strncpy(reason_out, msg, reason_cap - 1);
        reason_out[reason_cap - 1] = '\0';
    }
    return false;
}

/* H3: the one gate every mutating entry point (save/clone/rename/delete)
 * calls FIRST -- refuses with a specific, actionable reason while
 * s_quarantined is set, so a corrupt store's bytes are never overwritten by
 * an operator action taken before they even know something is wrong. */
static bool refuse_if_quarantined(char *reason_out, size_t reason_cap)
{
    if (!s_quarantined) {
        return false;
    }
    /* Sized against s_quarantine_reason's own 128-byte cap plus the fixed
     * template text (~240 bytes) -- GCC's -Werror=format-truncation caught
     * the original 192-byte buffer as too small for the worst case at
     * target-build time (the MSVC host build has no equivalent check). */
    char msg[400];
    snprintf(msg, sizeof(msg),
             "kiln config store was unreadable at boot and is quarantined (%s); download a backup of "
             "any other saved configs is not possible from this state -- clear the quarantine "
             "(POST /api/kiln_configs/quarantine_clear) to start a fresh, empty store",
             s_quarantine_reason);
    set_reason(reason_out, reason_cap, msg);
    return true;
}

/* Trims leading/trailing whitespace from `raw` and writes the result to
 * `out` (out_cap must be >= KILN_CFG_NAME_MAX_LEN + 1). Returns false --
 * `out` untouched -- if `raw` is NULL, or if the TRIMMED result is empty
 * (covers both "" and a whitespace-only name -- an unnamed entry is exactly
 * as unusable in the operator's picker as a duplicated one) or longer than
 * KILN_CFG_NAME_MAX_LEN. Trimming happens before both the too-long check and
 * before storing/comparing -- " spare" and "spare" are the same name to the
 * duplicate check below AND the same bytes end up on flash, so a stray space
 * typed at either end never produces a name that reads as a duplicate in the
 * picker but somehow isn't (or vice versa). */
/* H9 fix (docs/audits/kiln_profiles_robustness_2026-09-14.md): validates
 * the CHARACTER SET of an already-trimmed name span -- length/emptiness/
 * duplicate checks stay normalize_name()'s job, this is only "is every
 * byte in here safe to put on flash, into JSON, into an HTTP header, and
 * on the LCD." Rejects:
 *   - control bytes 0x00-0x1F and 0x7F (a name containing '\n'/'\t'/etc.
 *     reaches the JSON export, a future Content-Disposition filename, and
 *     the LCD -- isspace() only trims these at the EDGES, never in the
 *     middle);
 *   - '"' and '\\' (breaks a future JSON/HTTP-header quoting) and '/'
 *     (hazardous in a filename; harmless in NVS but there is no reason to
 *     allow it in an operator-facing kiln name either);
 *   - invalid UTF-8, INCLUDING a multi-byte sequence truncated at the
 *     23-byte length boundary -- the length limit is in bytes (it sizes a
 *     flash field), so accepting a truncated final character would render
 *     as a replacement glyph in the browser and garbage on the LCD, and
 *     could make two visually-identical names compare as non-duplicates.
 * A minimal, mechanical validator -- not full Unicode normalization -- is
 * enough here: this only needs to catch "not well-formed enough to render
 * safely everywhere", not validate that every code point is assigned. */
static bool name_charset_and_utf8_valid(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7Fu || c == '"' || c == '\\' || c == '/') {
            return false;
        }
        if (c < 0x80u) {
            i++;
            continue;
        }
        size_t extra;
        unsigned char min_lead;
        if ((c & 0xE0u) == 0xC0u) {
            extra = 1;
            min_lead = 0xC2u; /* 0xC0/0xC1 would only ever encode an overlong 1-byte value */
        } else if ((c & 0xF0u) == 0xE0u) {
            extra = 2;
            min_lead = 0xE0u;
        } else if ((c & 0xF8u) == 0xF0u) {
            extra = 3;
            min_lead = 0xF0u;
        } else {
            return false; /* a bare continuation byte, or a lead byte for a >4-byte sequence -- never valid UTF-8 */
        }
        if (c < min_lead || i + extra >= len) {
            return false; /* overlong lead byte, or the sequence runs past the end -- TRUNCATED, not accepted */
        }
        for (size_t j = 1; j <= extra; j++) {
            if (((unsigned char)s[i + j] & 0xC0u) != 0x80u) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

static bool normalize_name(const char *raw, char *out, size_t out_cap)
{
    if (!raw || out_cap == 0) {
        return false;
    }
    size_t len = strlen(raw);
    size_t start = 0;
    while (start < len && isspace((unsigned char)raw[start])) {
        start++;
    }
    size_t end = len;
    while (end > start && isspace((unsigned char)raw[end - 1])) {
        end--;
    }
    size_t trimmed_len = end - start;
    if (trimmed_len == 0 || trimmed_len > KILN_CFG_NAME_MAX_LEN || trimmed_len >= out_cap) {
        return false;
    }
    if (!name_charset_and_utf8_valid(raw + start, trimmed_len)) {
        return false;
    }
    memcpy(out, raw + start, trimmed_len);
    out[trimmed_len] = '\0';
    return true;
}

/* Case-insensitive compare of two already-normalized (trimmed) names --
 * "Spare" and "spare" are indistinguishable to an operator reading a name
 * picker, so they are treated as the SAME name for collision purposes, even
 * though the exact case the operator originally typed is still what gets
 * stored/displayed (this function only decides collision-or-not, it never
 * changes what's stored). */
static bool names_equal_ci(const char *a, const char *b)
{
    /* Hand-rolled rather than strcasecmp()/_stricmp() -- this file is built
     * both by ESP-IDF's toolchain (on-target) and, unmodified, by MSVC's cl
     * for the host test harness (build_host_tests.ps1); the two disagree on
     * which non-standard name (or header) exposes a case-insensitive
     * compare, so a tolower()-based loop (plain C89, no extra header) avoids
     * the whole portability question. */
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* True if some OTHER entry (not `exclude_id`) already has `normalized_name`,
 * case-insensitively. `exclude_id` lets a caller allow "rename/save this
 * entry back to the name it already has" (a no-op, not a collision) by
 * passing that entry's own id; pass KILN_CFG_NO_ACTIVE_ID (or any id that
 * cannot exist, e.g. a save-as-new's not-yet-assigned id) when there is no
 * entry to exclude. */
static bool name_collides(const char *normalized_name, int32_t exclude_id)
{
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        if (!s_store.entries[i].in_use || s_store.entries[i].id == exclude_id) {
            continue;
        }
        if (names_equal_ci(s_store.entries[i].name, normalized_name)) {
            return true;
        }
    }
    return false;
}

/* ---- Public API ------------------------------------------------------------ */

esp_err_t kiln_cfg_store_init(void)
{
    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- kiln configs will not persist",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        reset_to_defaults();
        return hal_status_to_esp_err(part_err);
    }
    nvs_load_store_with_cfg_fs();

    /* H2 load-time invariant (docs/audits/kiln_profiles_robustness_2026-09-14.md):
     * pico_populated=1 with pkg_hash==0 is a contradiction (0 is the
     * documented "never computed" sentinel) that write-time now prevents
     * from being newly created, but a blob written by a hypothetically
     * buggy past build, or corrupted in a way that preserves the version/
     * size checks, could still carry it. Never trust it: downgrade to
     * "not yet captured" (same state a v2-migrated slot has) and log it,
     * rather than letting a slot claim a valid Pico-half identity backed by
     * a hash that was never actually computed. */
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        kiln_cfg_entry_t *e = &s_store.entries[i];
        if (e->in_use && e->pico_populated && e->pkg_hash == 0) {
            ESP_LOGW(TAG, "kiln config '%s' (id=%ld) claimed a captured Pico half with pkg_hash==0 -- "
                          "treating as not-yet-captured, never a valid-but-zero hash",
                     e->name, (long)e->id);
            e->pico_populated = 0;
            e->pkg_schema = 0;
        }
    }

    /* Boot-time active-config restore -- see kiln_cfg_store_init()'s doc
     * comment (kiln_cfg_store.h) for the exact three-outcome fallback this
     * implements. Called AFTER zones_http_start() (main.c's call order), so
     * "apply nothing" always means "keep whatever zones_http_start() already
     * loaded on its own", never a half-state. */
    if (s_store.active_id != KILN_CFG_NO_ACTIVE_ID) {
        int idx = find_index_by_id(s_store.active_id);
        if (idx < 0) {
            ESP_LOGW(TAG, "active kiln config id=%ld no longer exists -- clearing it, keeping "
                          "whatever zones config already loaded",
                     (long)s_store.active_id);
            /* LOW 7: the slot is going away, so any recapture pending
             * against it must go with it. */
            pico_half_dirty_drop_if_owned_by(s_store.active_id);
            s_store.active_id = KILN_CFG_NO_ACTIVE_ID;
            hal_status_t clear_err = nvs_save_store();
            if (clear_err != HAL_OK) {
                ESP_LOGW(TAG, "clearing missing active kiln config id failed to persist: %s -- will retry next boot",
                         hal_status_to_name(clear_err));
            }
        } else {
            char reason[96];
            reason[0] = '\0';
            if (!zones_config_import_blob(s_store.entries[idx].blob, s_store.entries[idx].blob_len,
                                          reason, sizeof(reason))) {
                ESP_LOGW(TAG, "active kiln config id=%ld failed validation at boot (%s) -- clearing "
                              "it, keeping whatever zones config already loaded",
                         (long)s_store.active_id, reason);
                /* LOW 7, same reasoning as the missing-id branch above. */
                pico_half_dirty_drop_if_owned_by(s_store.active_id);
                s_store.active_id = KILN_CFG_NO_ACTIVE_ID;
                hal_status_t clear_err = nvs_save_store();
                if (clear_err != HAL_OK) {
                    ESP_LOGW(TAG, "clearing invalid active kiln config id failed to persist: %s -- will retry next boot",
                             hal_status_to_name(clear_err));
                }
            } else {
                ESP_LOGI(TAG, "restored active kiln config id=%ld ('%s') at boot", (long)s_store.active_id,
                         s_store.entries[idx].name);
            }
        }
    }
    return ESP_OK;
}

uint8_t kiln_cfg_store_max_count(void)
{
    return KILN_CFG_MAX_COUNT;
}

uint8_t kiln_cfg_store_list(kiln_cfg_summary_t *out, uint8_t out_cap)
{
    if (!out || out_cap == 0) {
        return 0;
    }
    uint8_t n = 0;
    for (int i = 0; i < KILN_CFG_MAX_COUNT && n < out_cap; i++) {
        if (!s_store.entries[i].in_use) {
            continue;
        }
        out[n].id = s_store.entries[i].id;
        strncpy(out[n].name, s_store.entries[i].name, KILN_CFG_NAME_MAX_LEN);
        out[n].name[KILN_CFG_NAME_MAX_LEN] = '\0';
        out[n].is_active = (s_store.entries[i].id == s_store.active_id);
        n++;
    }
    return n;
}

uint8_t kiln_cfg_store_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        if (s_store.entries[i].in_use) {
            n++;
        }
    }
    return n;
}

int32_t kiln_cfg_store_get_active_id(void)
{
    return s_store.active_id;
}

bool kiln_cfg_store_get_name(int32_t id, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return false;
    }
    int idx = find_index_by_id(id);
    if (idx < 0) {
        return false;
    }
    strncpy(out, s_store.entries[idx].name, out_cap - 1);
    out[out_cap - 1] = '\0';
    return true;
}

/* Captures the Pico half (docs/KILN_PROFILES_PLAN.md items 1/2/12) into
 * *e->pico by walking the ESP-side mirror of CONFIG_PARAM_TABLE
 * (kiln_package.h's own header comment on why a table walk, never a curated
 * field list) and computes this slot's package identity (pkg_schema +
 * pkg_hash, section 3.1.1/3.1.3) over the just-written ESP blob plus that
 * Pico half.
 *
 * Deliberately NEVER fails the caller's save/clone-from-live: this module's
 * scope (see kiln_package.h's top comment) is capture-and-hash only, not the
 * two-processor apply transaction, so a Pico-half capture problem (only
 * possible if CONFIG_PARAM_TABLE ever outgrows KILN_PKG_SAFETY_PARAM_CAP --
 * caught at compile time by kiln_cfg_store_pico_cap_check above, so this is
 * defensive, not an expected runtime path) degrades to "this slot's Pico
 * half is not yet captured" (pico_populated=0), an HONEST state this
 * module's own header comment already defines and kiln_cfg_store_apply()
 * must treat as "no Pico half to push" -- never a silently wrong or stale
 * one. Losing the ESP-side save over a Pico-cache read glitch would be a
 * worse failure than the one this guards against. */
/* 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md, finding
 * 1): `recapture_pico_half` lets a caller (kiln_cfg_store_autosave_from_live()
 * while a standing divergence is latched) recompute this slot's package
 * identity over its EXISTING, already-captured e->pico half instead of
 * re-snapshotting safety_cfg_store's live cache -- i.e. "save the ESP-side
 * edit, but do not touch what this slot believes the Pico holds." Re-
 * snapshotting from the live cache while the two processors are known to
 * disagree is exactly the "reset one side of a pair" bug class the standing-
 * divergence check exists to catch: the live cache could be a Pico's own
 * post-reboot REVERT, and capturing it here would silently launder that
 * revert into a freshly "confirmed" expected value. `false` leaves e->pico/
 * e->pico_populated/e->pkg_schema untouched (whatever this slot already
 * held) and only recomputes e->pkg_hash over the (possibly changed) ESP
 * canonical bytes plus that UNCHANGED Pico half -- the hash must still track
 * the ESP-side edit even when the Pico half is deliberately held back. */
static void populate_pico_half_and_hash(kiln_cfg_entry_t *e, const zones_cfg_t *cfg, bool recapture_pico_half)
{
    if (!recapture_pico_half) {
        if (!e->pico_populated) {
            /* Nothing captured yet to keep (a brand-new slot, or one already
             * downgraded to "not yet captured") -- there is no honest hash to
             * compute over an empty Pico half, so this slot stays exactly
             * that: not yet captured. Never silently invent a Pico half here. */
            e->pkg_hash = 0;
            e->pkg_schema = 0;
            return;
        }
        uint8_t canonical[ZONES_CONFIG_BLOB_MAX_SIZE];
        size_t canonical_len = 0;
        if (!zones_config_export_canonical(cfg, canonical, sizeof(canonical), &canonical_len)) {
            ESP_LOGW(TAG, "kiln_cfg_store: canonical ESP-half encoding failed for '%s' while keeping "
                          "the existing Pico half -- package hash left unchanged",
                     e->name);
            return;
        }
        uint32_t hash = 0;
        if (kiln_package_compute_hash(KILN_PKG_SCHEMA_VERSION, canonical, (uint16_t)canonical_len, &e->pico, &hash) &&
            hash != 0) {
            e->pkg_schema = KILN_PKG_SCHEMA_VERSION;
            e->pkg_hash = hash;
        } else {
            ESP_LOGW(TAG, "kiln_cfg_store: package hash recomputation failed (kept existing Pico half) "
                          "for '%s' -- package hash left unchanged",
                     e->name);
        }
        return;
    }

    kiln_pkg_pico_source_t source = kiln_pkg_pico_source_default();
    if (!kiln_package_capture_pico_half(&source, &e->pico)) {
        memset(&e->pico, 0, sizeof(e->pico));
        e->pico_populated = 0;
        e->pkg_schema = 0;
        e->pkg_hash = 0;
        ESP_LOGW(TAG, "kiln_cfg_store: Pico-half capture failed for '%s' -- this slot's package "
                      "identity reads as not-yet-captured, ESP half saved regardless",
                 e->name);
        return;
    }
    /* H1 fix (docs/audits/kiln_profiles_robustness_2026-09-14.md,
     * docs/audits/kiln_package_canonical_serializer_2026-09-14.md): the ESP
     * half fed into kiln_package_compute_hash() is now
     * zones_config_export_canonical()'s padding-free, declaration-order
     * encoding of `cfg` -- NOT the raw memcpy'd blob this file already keeps
     * in e->blob for on-flash storage. A JSON-reconstructed candidate
     * (upload path, not yet wired up) can reproduce this encoding exactly
     * regardless of what padding its own zones_cfg_t happened to hold, so a
     * package downloaded and re-uploaded unmodified now hashes identically.
     * e->blob itself is UNCHANGED -- still the raw struct, still what
     * kiln_cfg_store_apply() hands to zones_config_import_blob(), which does
     * its own CRC/version checking independently of this hash. */
    uint8_t canonical[ZONES_CONFIG_BLOB_MAX_SIZE];
    size_t canonical_len = 0;
    if (!zones_config_export_canonical(cfg, canonical, sizeof(canonical), &canonical_len)) {
        memset(&e->pico, 0, sizeof(e->pico));
        e->pico_populated = 0;
        e->pkg_schema = 0;
        e->pkg_hash = 0;
        ESP_LOGW(TAG, "kiln_cfg_store: canonical ESP-half encoding failed for '%s' -- this slot's "
                      "package identity reads as not-yet-captured, ESP half saved regardless",
                 e->name);
        return;
    }
    /* H2 fix (docs/audits/kiln_profiles_robustness_2026-09-14.md): pkg_hash
     * == 0 is the documented "never computed" sentinel
     * (kiln_cfg_store_internal.h), so a slot must never be left marked
     * pico_populated=1 with pkg_hash==0 -- that combination would be
     * indistinguishable from a genuinely-computed, astronomically-unlikely
     * zero CRC and would let the divergence check (once it lands) compare
     * against a hash that was never actually computed. Only mark
     * pico_populated=1 AFTER a successful hash; a hash failure downgrades
     * this slot to the same "not yet captured" state an unreachable capture
     * failure already produces above, rather than a half-marked one. */
    uint32_t hash = 0;
    if (kiln_package_compute_hash(KILN_PKG_SCHEMA_VERSION, canonical, (uint16_t)canonical_len, &e->pico, &hash) &&
        hash != 0) {
        e->pico_populated = 1;
        e->pkg_schema = KILN_PKG_SCHEMA_VERSION;
        e->pkg_hash = hash;
    } else {
        memset(&e->pico, 0, sizeof(e->pico));
        e->pico_populated = 0;
        e->pkg_schema = 0;
        e->pkg_hash = 0;
        ESP_LOGW(TAG, "kiln_cfg_store: package hash computation failed (or produced the reserved 0 "
                      "sentinel) for '%s' -- this slot's package identity reads as not-yet-captured, "
                      "ESP half saved regardless",
                 e->name);
    }
}

/* Shared body for kiln_cfg_store_save_current() (public, always recaptures
 * the Pico half -- an explicit operator save/clone-from-live is a deliberate
 * "snapshot everything live right now" action) and the autosave paths below
 * that may need to hold the Pico half back (see populate_pico_half_and_hash()'s
 * own doc comment on `recapture_pico_half`). */
static bool kiln_cfg_store_save_current_ex(const char *name, int32_t id_or_negative, int32_t *out_id,
                                            bool recapture_pico_half, char *reason_out, size_t reason_cap)
{
    if (refuse_if_quarantined(reason_out, reason_cap)) {
        return false;
    }
    char normalized[KILN_CFG_NAME_MAX_LEN + 1];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return set_reason(reason_out, reason_cap, "name missing, too long, or contains invalid characters");
    }
    /* Excluding id_or_negative itself (when >= 0, i.e. an overwrite-by-id)
     * means "re-save this config under the name it already has" is allowed
     * -- that's a legitimate re-save-from-live action, not a collision. A
     * save-as-new (id_or_negative < 0) has no existing id to exclude, so -1
     * (KILN_CFG_NO_ACTIVE_ID) is passed, which can never match a real
     * entry's id. */
    if (name_collides(normalized, id_or_negative)) {
        return set_reason(reason_out, reason_cap, "a saved kiln config already has that name");
    }

    uint8_t scratch[ZONES_CONFIG_BLOB_MAX_SIZE];
    size_t blob_size = zones_config_blob_size();
    if (blob_size == 0 || blob_size > sizeof(scratch)) {
        return set_reason(reason_out, reason_cap, "current zones config could not be exported");
    }
    if (!zones_config_export_blob(scratch, sizeof(scratch))) {
        return set_reason(reason_out, reason_cap, "current zones config could not be exported");
    }

    int idx;
    int32_t id;
    if (id_or_negative >= 0) {
        idx = find_index_by_id(id_or_negative);
        if (idx < 0) {
            return set_reason(reason_out, reason_cap, "no saved kiln config with that id");
        }
        id = id_or_negative;
    } else {
        idx = find_free_slot();
        if (idx < 0) {
            return set_reason(reason_out, reason_cap, "kiln config store is full");
        }
        id = s_store.next_id++;
    }

    kiln_cfg_entry_t *e = &s_store.entries[idx];
    e->in_use = 1;
    e->id = id;
    strncpy(e->name, normalized, KILN_CFG_NAME_MAX_LEN);
    e->name[KILN_CFG_NAME_MAX_LEN] = '\0';
    memset(e->blob, 0, sizeof(e->blob));
    memcpy(e->blob, scratch, blob_size);
    e->blob_len = (uint16_t)blob_size;
    /* Reinterpret the raw-exported bytes as a real zones_cfg_t for the H1
     * canonical hash path below -- via a properly-aligned struct copy, never
     * a cast of `scratch` (a plain uint8_t[] has no alignment guarantee
     * matching zones_cfg_t's, and aliasing a byte array as a struct pointer
     * is undefined behavior regardless). zero-init first so any bytes beyond
     * blob_size (only possible if a future older/shorter blob_size existed)
     * read as a defined, zeroed extension rather than stack garbage --
     * harmless today since blob_size == sizeof(zones_cfg_t) always, but
     * cheap insurance against that changing. */
    zones_cfg_t scratch_cfg;
    memset(&scratch_cfg, 0, sizeof(scratch_cfg));
    memcpy(&scratch_cfg, scratch, blob_size);
    populate_pico_half_and_hash(e, &scratch_cfg, recapture_pico_half);

    if (id_or_negative < 0) {
        /* A config just saved FROM the running kiln is, by construction,
         * exactly what's live right now -- marking it active is recording a
         * fact, not applying anything. Overwriting an existing entry
         * (id_or_negative >= 0) does NOT do this -- see this function's own
         * header comment. */
        /* LOW 7: drop a recapture still owed by the slot we are leaving.
         * Ordering matters -- populate_pico_half_and_hash() just above may
         * have set a fresh pending flag against the NEW slot `id`, and
         * drop_if_owned_by() only clears a flag whose owner matches, so the
         * new one survives. */
        if (s_store.active_id != id) {
            pico_half_dirty_drop_if_owned_by(s_store.active_id);
        }
        s_store.active_id = id;
    }

    hal_status_t err = nvs_save_store();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "nvs_save_store after save failed: %s -- saved live but will not survive a reboot",
                 hal_status_to_name(err));
    }
    if (out_id) {
        *out_id = id;
    }
    return true;
}

bool kiln_cfg_store_save_current(const char *name, int32_t id_or_negative, int32_t *out_id,
                                  char *reason_out, size_t reason_cap)
{
    /* Public entry point always recaptures the Pico half -- see kiln_cfg_
     * store_save_current_ex()'s own doc comment above for why an explicit
     * operator save/clone-from-live is exempt from the autosave paths'
     * divergence hold-back. */
    return kiln_cfg_store_save_current_ex(name, id_or_negative, out_id, /*recapture_pico_half=*/true, reason_out,
                                           reason_cap);
}

bool kiln_cfg_store_clone(int32_t src_id, const char *name, int32_t *out_id, char *reason_out,
                          size_t reason_cap)
{
    if (refuse_if_quarantined(reason_out, reason_cap)) {
        return false;
    }
    char normalized[KILN_CFG_NAME_MAX_LEN + 1];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return set_reason(reason_out, reason_cap, "name missing, too long, or contains invalid characters");
    }
    /* A clone always creates a brand-new entry/id, so there is no existing
     * entry to exempt from the collision check -- KILN_CFG_NO_ACTIVE_ID (-1)
     * can never match a real id. */
    if (name_collides(normalized, KILN_CFG_NO_ACTIVE_ID)) {
        return set_reason(reason_out, reason_cap, "a saved kiln config already has that name");
    }
    int src_idx = find_index_by_id(src_id);
    if (src_idx < 0) {
        return set_reason(reason_out, reason_cap, "no saved kiln config with that id");
    }
    int dst_idx = find_free_slot();
    if (dst_idx < 0) {
        return set_reason(reason_out, reason_cap, "kiln config store is full");
    }

    int32_t new_id = s_store.next_id++;
    kiln_cfg_entry_t *dst = &s_store.entries[dst_idx];
    const kiln_cfg_entry_t *src = &s_store.entries[src_idx]; /* read before any write to dst,
                                                              * safe even if src_idx==dst_idx
                                                              * could somehow coincide (it can't --
                                                              * dst_idx is always a FREE slot,
                                                              * src_idx always an in_use one). */
    *dst = *src;
    dst->id = new_id;
    strncpy(dst->name, normalized, KILN_CFG_NAME_MAX_LEN);
    dst->name[KILN_CFG_NAME_MAX_LEN] = '\0';

    /* Deliberately does NOT touch active_id -- a clone is a new saved slot,
     * not a change to what's live or what boots next. See this function's
     * header comment. */
    hal_status_t err = nvs_save_store();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "nvs_save_store after clone failed: %s -- cloned live but will not survive a reboot",
                 hal_status_to_name(err));
    }
    if (out_id) {
        *out_id = new_id;
    }
    return true;
}

/* Section 5.3 table row 4 helper: looks up param `id`'s current live value
 * (via the safety_cfg_store cache this controller already keeps, not a
 * fresh Pico round trip) as a bit pattern, mirroring how kiln_pkg_pico_
 * param_t stores its own value_bits so the two are directly comparable.
 * Returns false (value left untouched) if the live table does not currently
 * have this id SET -- "not commissioned yet" is not the same as "differs",
 * so callers must treat a false return as "cannot compare, skip this id"
 * rather than as a mismatch. */
static bool live_pico_param_bits(uint16_t param_id, uint32_t *out_bits)
{
    size_t n = safety_cfg_store_param_count();
    for (size_t i = 0; i < n; i++) {
        safety_cfg_param_t row;
        if (!safety_cfg_store_get_by_index(i, &row)) {
            continue;
        }
        if (row.param_id != param_id || !row.set) {
            continue;
        }
        union {
            float f;
            uint32_t bits;
        } conv;
        switch (row.type) {
        case KILNLINK_PARAM_TYPE_BOOL:
            *out_bits = (uint32_t)row.value.bool_val;
            return true;
        case KILNLINK_PARAM_TYPE_U8:
            *out_bits = (uint32_t)row.value.u8_val;
            return true;
        case KILNLINK_PARAM_TYPE_U16:
            *out_bits = (uint32_t)row.value.u16_val;
            return true;
        case KILNLINK_PARAM_TYPE_F32:
            conv.f = row.value.f32_val;
            *out_bits = conv.bits;
            return true;
        default:
            return false;
        }
    }
    return false;
}

/* Section 5.3 table row 4 (plan section 5.3, docs/audits/kiln_profiles_
 * feature_review_2026-09-15.md Defect 5): `ct_installed`, `ct_topology`,
 * `safety_tc_installed` are the three of row 4's five named fields that
 * are literal Pico CONFIG_PARAM_TABLE entries captured into a package's
 * pico half (0x0109, 0x031F, 0x0211) -- `relay_count`/`thermo_count` are
 * NOT package fields at all (they are live zones_config_accessors.h
 * queries against THIS firmware's own compiled-in hardware shape, already
 * hard-refused above at import time in kiln_cfg_store_import_package_json()
 * whenever a package's zone assignments exceed what this board reports),
 * so this gate deliberately covers only the three that a saved slot can
 * actually disagree with the live board about. Named loudly here, not
 * fixed silently: a scope decision, not an oversight -- see this
 * function's own report to the caller. Returns true (and fills `msg`) the
 * first time a comparable id's packaged value differs from the live
 * value; an id neither side has a SET value for is skipped, never treated
 * as a mismatch (an unset field cannot "differ"). */
static bool apply_hardware_differs(const kiln_pkg_safety_t *pico, char *msg, size_t msg_cap)
{
    static const struct {
        uint16_t param_id;
        const char *label;
    } kFields[] = {
        {0x0109u, "ct_installed"},
        {0x031Fu, "ct_topology"},
        {0x0211u, "safety_tc_installed"},
    };
    for (size_t f = 0; f < sizeof(kFields) / sizeof(kFields[0]); f++) {
        uint32_t pkg_bits = 0;
        bool pkg_has = false;
        for (uint16_t i = 0; i < pico->count; i++) {
            if (pico->entries[i].param_id == kFields[f].param_id &&
                (pico->entries[i].flags & KILN_PKG_PARAM_FLAG_SET) != 0) {
                pkg_bits = pico->entries[i].value_bits;
                pkg_has = true;
                break;
            }
        }
        if (!pkg_has) {
            continue;
        }
        uint32_t live_bits = 0;
        if (!live_pico_param_bits(kFields[f].param_id, &live_bits)) {
            continue;
        }
        if (pkg_bits != live_bits) {
            if (msg && msg_cap) {
                snprintf(msg, msg_cap,
                         "this saved config's '%s' differs from what this controller currently reports "
                         "-- confirm the hardware shape matches before applying",
                         kFields[f].label);
            }
            return true;
        }
    }
    return false;
}

/* Public read-only wrapper over apply_hardware_differs(), for the one caller
 * that must ask the question WITHOUT applying: kiln_cfg_http.c's apply
 * handler, since docs/KILN_PROFILES_PLAN.md item 5 moved the actual apply
 * onto kiln_cfg_swap_apply(), whose signature has no ack_hardware_differs
 * parameter of its own. Without this, routing the HTTP apply through the
 * two-processor transaction would silently drop section 5.3 table row 4's
 * gate entirely -- the gate would still exist in kiln_cfg_store_apply()
 * while nothing reached it. Exported rather than duplicated so the field
 * list (ct_installed / ct_topology / safety_tc_installed) keeps exactly one
 * definition; a second copy here is precisely the "reset one side of a
 * pair" shape CLAUDE.md warns about.
 *
 * Deliberately takes NO lock, matching kiln_cfg_store_apply()'s own call to
 * apply_hardware_differs() a few lines below (which likewise reads
 * s_store.entries[idx].pico unlocked) -- taking one only here would
 * introduce a lock-ordering question against live_pico_param_bits()'s own
 * module that the existing call site does not have. Returns false for an
 * unknown id and for a half-package slot (pico_populated == 0): both are
 * refused by kiln_cfg_store_apply()/kiln_cfg_swap_apply() for their own,
 * better-worded reasons, and reporting "hardware differs" for a slot that
 * has no Pico half to compare would send the operator to the wrong
 * problem. */
bool kiln_cfg_store_slot_hardware_differs(int32_t id, char *msg, size_t msg_cap)
{
    if (msg && msg_cap) {
        msg[0] = '\0';
    }
    int idx = find_index_by_id(id);
    if (idx < 0 || !s_store.entries[idx].pico_populated) {
        return false;
    }
    return apply_hardware_differs(&s_store.entries[idx].pico, msg, msg_cap);
}

bool kiln_cfg_store_apply(int32_t id, bool ack_no_safety_processor, bool ack_hardware_differs,
                          char *reason_out, size_t reason_cap)
{
    if (refuse_if_quarantined(reason_out, reason_cap)) {
        return false;
    }
    /* Backstop interlock -- see this function's SAFETY note
     * (kiln_cfg_store.h). Checked FIRST, before find_index_by_id() or
     * anything else touches the store, so a caller that forgets to
     * pre-check (kiln_cfg_http.c's apply handler and the LCD's confirm
     * callback both do, for a better-worded message, but that is now
     * redundant belt-and-suspenders, not load-bearing) still cannot apply a
     * config out from under a running kiln. Same predicate
     * POST /api/ota/esp and /api/ota/pico gate on -- "is a profile running /
     * are the heaters on" -- deliberately NOT heat_interlock.c, which
     * answers the opposite question (may heat run during an update). */
    if (ota_http_check_interlocks(ack_no_safety_processor, reason_out, reason_cap)
            != OTA_INTERLOCK_OK) {
        return false;
    }

    int idx = find_index_by_id(id);
    if (idx < 0) {
        return set_reason(reason_out, reason_cap, "no saved kiln config with that id");
    }
    /* H17 fix (docs/audits/kiln_profiles_robustness_2026-09-14.md): a slot
     * with pico_populated==0 (a v2-migrated slot never re-saved since, or a
     * hash-computation failure at save time -- see populate_pico_half_and_
     * hash()) is a HALF-PACKAGE: a real ESP half, no Pico half at all. This
     * function does not yet push anything to the Pico (docs/KILN_PROFILES_
     * PLAN.md item 5, out of scope for this pass), but the whole point of
     * refusing here NOW, before item 5 exists, is to make the future
     * two-processor apply structurally unable to silently apply the ESP
     * half while leaving the Pico on the PREVIOUS kiln's settings --
     * exactly the "obvious implementation" the audit warns would recreate
     * this plan's central defect. Refuse, do not best-effort; the operator
     * completes the slot with an ordinary Save once the safety settings are
     * checked/re-entered. */
    if (!s_store.entries[idx].pico_populated) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "'%s' was saved before this firmware stored the safety processor's settings. Select it, "
                 "check the safety settings, then press Save to complete it.",
                 s_store.entries[idx].name);
        return set_reason(reason_out, reason_cap, msg);
    }
    /* Section 5.3 table row 4: refuse unless the caller explicitly
     * acknowledges a hardware-shape mismatch. Checked after pico_populated
     * (a half-package is refused outright above, for a different reason)
     * and before anything touches zones_config_import_blob() or the store
     * -- same "refuse before mutating anything" discipline as every other
     * gate in this function. */
    if (!ack_hardware_differs) {
        char hw_msg[192];
        if (apply_hardware_differs(&s_store.entries[idx].pico, hw_msg, sizeof(hw_msg))) {
            return set_reason(reason_out, reason_cap, hw_msg);
        }
    }
    /* Section 5.3 table row 1, re-checked again HERE at apply time
     * (docs/KILN_PROFILES_PLAN.md, owner decision 2026-09-16): the slot's
     * configured zone ceilings were already checked against the PACKAGE's
     * own captured abs_max_temp_c at import time (see the "Compatibility
     * 5.3, table row 1" block in kiln_cfg_store_import_package_json()
     * below -- this block mirrors its zone-max-temp derivation exactly).
     * That is not the same thing as the LIVE board's current
     * safety_cfg_store abs_max_temp_c reading, which is what actually
     * arms the Pico. Inert today -- this function pushes no Pico half
     * (H17 above), so the live cache's abs_max_temp_c cannot have moved
     * since the slot was saved/imported on THIS board -- but added now,
     * unconditionally (no ack bypass; a ceiling mismatch is never a
     * "hardware shape differs, operator confirmed" situation), so the day
     * apply() starts pushing a Pico half this re-check is not a gap
     * discovered under pressure. live_pico_param_bits() returns false when
     * the live cache has no abs_max_temp_c reading yet -- SKIP, do not
     * refuse, same "not yet commissioned is not unsafe" convention
     * apply_hardware_differs() already uses; this keeps every existing
     * apply test (whose live cache starts empty) non-regressive. The
     * zones_cfg_t decode is HEAP-allocated, never a stack local -- same
     * stack-budget discipline as kiln_cfg_import_scratch_t above (a prior
     * checker caught a 5024 B vs 4832 B overflow on this same shared
     * 8192 B httpd_worker stack from exactly this class of plain-local). */
    {
        uint32_t live_bits = 0;
        if (live_pico_param_bits(0x0104u, &live_bits)) {
            union { uint32_t bits; float f; } conv;
            conv.bits = live_bits;
            float live_abs_max_temp_c = conv.f;

            zones_cfg_t *cand = (zones_cfg_t *)malloc(sizeof(*cand));
            if (!cand) {
                return set_reason(reason_out, reason_cap, "out of memory");
            }
            memset(cand, 0, sizeof(*cand));
            size_t copy_len = s_store.entries[idx].blob_len;
            if (copy_len > sizeof(*cand)) {
                copy_len = sizeof(*cand);
            }
            memcpy(cand, s_store.entries[idx].blob, copy_len);

            float max_zone_temp_c = 0.0f;
            for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
                const zone_cfg_t *zc = &cand->zones[z];
                if (zc->thermo_mask != 0 && zc->max_temp_c > max_zone_temp_c) {
                    max_zone_temp_c = zc->max_temp_c;
                }
            }
            free(cand);

            if (max_zone_temp_c > 0.0f && live_abs_max_temp_c < max_zone_temp_c) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "this controller's live safety-processor ceiling (abs_max_temp_c=%.1f C) is "
                         "lower than this kiln's highest configured zone max_temp_c (%.1f C) -- the "
                         "safety ceiling must never be tighter than the kiln it is armed for",
                         live_abs_max_temp_c, max_zone_temp_c);
                return set_reason(reason_out, reason_cap, msg);
            }
        }
    }
    /* zones_config_import_blob() does the actual all-or-nothing
     * version-check/re-validate/commit work; see its own doc comment
     * (zones_http.h). HIGH finding 1, adversarial review 2026-09-15
     * (docs/audits/review_autosave_slot_fix_a93ee77b_2026-09-15.md): this is
     * the production apply path (kiln_cfg_http.c's POST handler) -- the
     * commit that first fixed Defect 1 (a93ee77b) only guarded
     * kiln_cfg_swap.c's two-processor swap, which has no production caller,
     * and missed this one. active_id does not move to `id` until the
     * statement right after this import returns, so without the same
     * override this import's nvs_save() would still dispatch an autosave
     * that writes the incoming config over the OUTGOING kiln's saved slot.
     * kiln_cfg_store_lock() is the same s_swap_lock kiln_cfg_swap.c already
     * serializes this override through. */
    kiln_cfg_store_lock();
    kiln_cfg_store_set_autosave_target_override(id);
    bool import_ok = zones_config_import_blob(s_store.entries[idx].blob, s_store.entries[idx].blob_len,
                                              reason_out, reason_cap);
    kiln_cfg_store_set_autosave_target_override(KILN_CFG_AUTOSAVE_OVERRIDE_NONE);
    kiln_cfg_store_unlock();
    if (!import_ok) {
        return false;
    }
    /* LOW 7: same choke-point drop kiln_cfg_store_set_active_id_raw() does
     * -- this apply path changes the active slot without going through it. */
    if (s_store.active_id != id) {
        pico_half_dirty_drop_if_owned_by(s_store.active_id);
    }
    s_store.active_id = id;
    hal_status_t err = nvs_save_store();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "nvs_save_store after apply failed: %s -- active id applied live but will not "
                      "survive a reboot",
                 hal_status_to_name(err));
    }
    return true;
}

bool kiln_cfg_store_delete(int32_t id, bool ack_no_safety_processor, char *reason_out, size_t reason_cap)
{
    if (refuse_if_quarantined(reason_out, reason_cap)) {
        return false;
    }
    /* H5 fix -- backstop interlock, same pattern and same predicate as
     * kiln_cfg_store_apply() (see this function's own SAFETY doc comment,
     * kiln_cfg_store.h): checked FIRST, before find_index_by_id() or
     * anything else touches the store. */
    if (ota_http_check_interlocks(ack_no_safety_processor, reason_out, reason_cap) != OTA_INTERLOCK_OK) {
        return false;
    }

    int idx = find_index_by_id(id);
    if (idx < 0) {
        return set_reason(reason_out, reason_cap, "no saved kiln config with that id");
    }
    /* H5 fix, part 2: deleting the ACTIVE config is refused outright,
     * unconditionally (not only while a firing is literally running) --
     * it is the one stored copy of what this controller is running
     * (including, under docs/KILN_PROFILES_PLAN.md's model, the Pico half
     * the safety processor is currently running from RAM and the
     * divergence check's recorded reference), and "Delete" is exactly one
     * misclick away from "Apply a different config" in the same picker. */
    if (s_store.active_id == id) {
        /* Sized against KILN_CFG_NAME_MAX_LEN (23) plus the fixed template
         * text -- 96 was too small for the worst case (GCC's -Werror=
         * format-truncation catches this at target-build time; MSVC's host
         * build does not). */
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "'%s' is the kiln config this controller is running; select another kiln config first, "
                 "or use Save as to keep a copy",
                 s_store.entries[idx].name);
        return set_reason(reason_out, reason_cap, msg);
    }
    memset(&s_store.entries[idx], 0, sizeof(s_store.entries[idx]));
    hal_status_t err = nvs_save_store();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "nvs_save_store after delete failed: %s -- deleted live but will not survive a reboot",
                 hal_status_to_name(err));
    }
    return true;
}

bool kiln_cfg_store_rename(int32_t id, const char *name)
{
    if (s_quarantined) {
        return false; /* H3 -- no reason_out on this function's signature; kiln_cfg_http.c's rename
                       * handler should call kiln_cfg_store_is_quarantined() itself for a specific message,
                       * same "store's own check is the backstop, callers may pre-check for a better
                       * message" pattern used throughout this module. */
    }
    char normalized[KILN_CFG_NAME_MAX_LEN + 1];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return false;
    }
    int idx = find_index_by_id(id);
    if (idx < 0) {
        return false;
    }
    /* Excluding `id` itself allows a no-op rename (or a rename that only
     * changes case/whitespace of the SAME name) -- that is not a collision
     * with another entry, it is the entry keeping (an equivalent form of)
     * its own name. */
    if (name_collides(normalized, id)) {
        return false;
    }
    strncpy(s_store.entries[idx].name, normalized, KILN_CFG_NAME_MAX_LEN);
    s_store.entries[idx].name[KILN_CFG_NAME_MAX_LEN] = '\0';
    hal_status_t err = nvs_save_store();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "nvs_save_store after rename failed: %s -- renamed live but will not survive a reboot",
                 hal_status_to_name(err));
    }
    return true;
}

bool kiln_cfg_store_get_package_identity(int32_t id, bool *out_pico_populated, uint16_t *out_pkg_schema,
                                          uint32_t *out_pkg_hash)
{
    int idx = find_index_by_id(id);
    if (idx < 0) {
        return false;
    }
    const kiln_cfg_entry_t *e = &s_store.entries[idx];
    if (out_pico_populated) {
        *out_pico_populated = e->pico_populated != 0;
    }
    if (out_pkg_schema) {
        *out_pkg_schema = e->pico_populated ? e->pkg_schema : 0;
    }
    if (out_pkg_hash) {
        *out_pkg_hash = e->pico_populated ? e->pkg_hash : 0;
    }
    return true;
}

bool kiln_cfg_store_is_quarantined(char *reason_out, size_t reason_cap)
{
    if (!s_quarantined) {
        return false;
    }
    if (reason_out && reason_cap) {
        strncpy(reason_out, s_quarantine_reason, reason_cap - 1);
        reason_out[reason_cap - 1] = '\0';
    }
    return true;
}

bool kiln_cfg_store_quarantine_clear(bool confirm_discard, char *reason_out, size_t reason_cap)
{
    if (!s_quarantined) {
        return set_reason(reason_out, reason_cap, "kiln config store is not quarantined -- nothing to clear");
    }
    if (!confirm_discard) {
        return set_reason(reason_out, reason_cap,
                           "clearing the quarantine discards whatever kiln configs could not be read -- "
                           "pass confirm_discard=1 to proceed");
    }
    ESP_LOGW(TAG, "kiln_cfg_store: operator confirmed discard of quarantined store (%s) -- starting a "
                  "fresh, empty store",
             s_quarantine_reason);
    reset_to_defaults();
    s_quarantined = false;
    s_quarantine_reason[0] = '\0';
    hal_status_t err = nvs_save_store();
    if (err != HAL_OK) {
        /* The in-RAM store is a clean, empty, valid one regardless -- this
         * board can save fresh kiln configs starting now even if the write-
         * back itself failed; it will just re-attempt on the next mutation
         * (every mutating function's own nvs_save_store() call), same as
         * every other "logged but not fatal" persistence failure in this
         * module. */
        ESP_LOGE(TAG, "nvs_save_store after quarantine clear failed: %s -- store is clean in RAM but may "
                      "not survive a reboot yet",
                 hal_status_to_name(err));
    }
    return true;
}

bool kiln_cfg_store_name_would_collide(const char *name, int32_t exclude_id)
{
    char normalized[KILN_CFG_NAME_MAX_LEN + 1];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return false; /* an invalid name is reported separately -- not this predicate's job */
    }
    return name_collides(normalized, exclude_id);
}

void kiln_cfg_store_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                          bool *diverged)
{
    if (file_valid) {
        *file_valid = false;
    }
    if (file_rev) {
        *file_rev = 0;
    }
    if (nvs_valid) {
        *nvs_valid = false;
    }
    if (nvs_rev) {
        *nvs_rev = 0;
    }
    if (diverged) {
        *diverged = false;
    }

    /* HEAP, never the stack -- kiln_cfg_store_blob_t is ~7.5 KiB, same
     * reasoning as nvs_load_store()'s own malloc() for its v1 scratch
     * buffer. Two of them here (file + NVS candidates) briefly, freed
     * before returning on every path. */
    kiln_cfg_store_blob_t *f_blob = malloc(sizeof(*f_blob));
    kiln_cfg_store_blob_t *n_blob = malloc(sizeof(*n_blob));
    if (!f_blob || !n_blob) {
        ESP_LOGW(TAG, "kiln_cfg_store_get_dualwrite_status: malloc failed -- reporting unknown");
        free(f_blob);
        free(n_blob);
        return;
    }

    uint32_t f_rev = 0;
    bool f_valid = false;
    kiln_cfg_store_cfg_fs_load_raw(f_blob, &f_rev, &f_valid);

    bool n_valid = false;
    uint32_t n_rev = kiln_cfg_rev_load();
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err == HAL_OK) {
        size_t len = sizeof(*n_blob);
        if (hal_kv_get_blob(&h, NVS_KEY_STORE, n_blob, &len) == HAL_OK && len == sizeof(*n_blob) &&
            n_blob->version == KILN_CFG_STORE_VERSION) {
            n_valid = true;
        }
        hal_kv_close(&h);
    }

    bool content_equal = f_valid && n_valid && (memcmp(f_blob, n_blob, sizeof(*f_blob)) == 0);
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
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
    }

    free(f_blob);
    free(n_blob);
}

/* ---- Swap-transaction support (docs/KILN_PROFILES_PLAN.md item 5) --------- */
/* See kiln_cfg_store.h's own doc comments for the contract of each of these
 * four calls; kiln_cfg_swap.c is their only caller. */

bool kiln_cfg_store_get_full_package(int32_t id, uint8_t *blob_out, uint16_t cap, uint16_t *out_len,
                                     kiln_pkg_safety_t *pico_out, char *reason_out, size_t reason_cap)
{
    int idx = find_index_by_id(id);
    if (idx < 0) {
        return set_reason(reason_out, reason_cap, "no saved kiln config with that id");
    }
    const kiln_cfg_entry_t *e = &s_store.entries[idx];
    if (pico_out && !e->pico_populated) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "'%s' was saved before this firmware stored the safety processor's settings -- "
                 "it is a half-package and cannot be swapped to. Select it, check the safety "
                 "settings, then press Save to complete it.",
                 e->name);
        return set_reason(reason_out, reason_cap, msg);
    }
    if (blob_out) {
        if (cap < e->blob_len) {
            return set_reason(reason_out, reason_cap, "caller buffer too small for this slot's ESP blob");
        }
        memcpy(blob_out, e->blob, e->blob_len);
    }
    if (out_len) {
        *out_len = e->blob_len;
    }
    if (pico_out) {
        *pico_out = e->pico;
    }
    return true;
}

/* Forward decl -- defined with the rest of the pico-half-dirty machinery
 * below; this is the single choke point for runtime active-slot changes
 * (kiln_cfg_swap.c's only three call sites), so it is where a pending
 * recapture flag that no longer matches the new active slot gets dropped
 * (LOW 7: "reset one side of a pair" -- a per-slot flag must not silently
 * keep applying to whichever slot happens to be active later, nor get
 * orphaned forever once its owning slot stops being active). */
static void pico_half_dirty_drop_if_owned_by(int32_t slot);

bool kiln_cfg_store_set_active_id_raw(int32_t id, char *reason_out, size_t reason_cap)
{
    if (id != KILN_CFG_NO_ACTIVE_ID && find_index_by_id(id) < 0) {
        return set_reason(reason_out, reason_cap, "cannot mark a nonexistent id active");
    }
    if (id != s_store.active_id) {
        pico_half_dirty_drop_if_owned_by(s_store.active_id);
    }
    s_store.active_id = id;
    hal_status_t err = nvs_save_store();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "nvs_save_store after set_active_id_raw failed: %s -- active id set live but "
                      "will not survive a reboot",
                 hal_status_to_name(err));
    }
    return true;
}

/* Lazily created, same "single process-wide dummy on the host stub, a real
 * mutex on target" convention relay_cycles.c's ensure_lock() already
 * establishes -- see this header's own doc comment for the lock-ordering
 * rule (never held across the Pico round trip). */
static SemaphoreHandle_t s_swap_lock;

void kiln_cfg_store_lock(void)
{
    if (!s_swap_lock) {
        s_swap_lock = xSemaphoreCreateMutex();
    }
    if (s_swap_lock) {
        xSemaphoreTake(s_swap_lock, portMAX_DELAY);
    }
}

void kiln_cfg_store_unlock(void)
{
    if (s_swap_lock) {
        xSemaphoreGive(s_swap_lock);
    }
}

/* ---- Download / upload (docs/KILN_PROFILES_PLAN.md items 3/4/14) -------- */

bool kiln_cfg_store_export_package_json(int32_t id, char *out, size_t out_cap, size_t *out_len,
                                        char *reason_out, size_t reason_cap)
{
    if (!out || !out_len) {
        return set_reason(reason_out, reason_cap, "internal error: NULL output buffer");
    }

    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_len = 0;
    kiln_pkg_safety_t pico;
    /* kiln_cfg_store_get_full_package() already refuses a half-package
     * (pico_populated == 0) with a specific, operator-facing reason -- reuse
     * that message verbatim rather than inventing a second one for the same
     * fact. */
    if (!kiln_cfg_store_get_full_package(id, blob, sizeof(blob), &blob_len, &pico, reason_out, reason_cap)) {
        return false;
    }
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!kiln_cfg_store_get_name(id, name, sizeof(name))) {
        return set_reason(reason_out, reason_cap, "no saved kiln config with that id");
    }
    uint16_t pkg_schema = 0;
    uint32_t pkg_hash = 0;
    kiln_cfg_store_get_package_identity(id, NULL, &pkg_schema, &pkg_hash);

    if (!kiln_package_export_json(name, pkg_schema, blob, blob_len, &pico, pkg_hash,
                                  kiln_board_identity_get(), out, out_cap, out_len)) {
        return set_reason(reason_out, reason_cap, "package too large to encode, or an internal error");
    }
    return true;
}

/* Bundles every large local kiln_cfg_store_import_package_json() needs --
 * HEAP-allocated (never stack) as ONE malloc, freed at its single `done`
 * exit. Added after check_httpd_task_stack_budget.ps1 caught the first
 * version of this function (everything as plain locals) pushing
 * import_post_handler to 5024 B of the shared 8192 B httpd_worker stack,
 * over its 4832 B ceiling -- the exact "no stack buffer may grow for this
 * feature" rule this plan's own HTTP section states
 * (project_httpd_stack_blob_class). ~2283 (zones_cfg_t) + 896 (esp_blob) +
 * 896 (canonical) + ~770 (pico) bytes, all now off the caller's stack. */
typedef struct {
    zones_cfg_t cand;
    uint8_t esp_blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint8_t canonical[ZONES_CONFIG_BLOB_MAX_SIZE];
    kiln_pkg_safety_t pico;
} kiln_cfg_import_scratch_t;

bool kiln_cfg_store_import_package_json(const char *json, int32_t *out_id, char *reason_out,
                                        size_t reason_cap)
{
    if (refuse_if_quarantined(reason_out, reason_cap)) {
        return false;
    }

    kiln_cfg_import_scratch_t *s = (kiln_cfg_import_scratch_t *)malloc(sizeof(*s));
    if (!s) {
        return set_reason(reason_out, reason_cap, "out of memory");
    }
    bool result = false;
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    uint16_t pkg_schema = 0;
    uint16_t esp_blob_len = 0;
    uint32_t declared_hash = 0;
    bool has_source_board = false;
    uint32_t source_board_id = 0;
#define IMPORT_REFUSE(...)                                                                                      \
    do {                                                                                                         \
        set_reason(reason_out, reason_cap, __VA_ARGS__);                                                        \
        goto done;                                                                                              \
    } while (0)

    if (!kiln_package_import_json(json, name, sizeof(name), &pkg_schema, s->esp_blob, sizeof(s->esp_blob),
                                  &esp_blob_len, &s->pico, &declared_hash, &has_source_board, &source_board_id,
                                  reason_out, reason_cap)) {
        goto done; /* reason already filled by kiln_package_import_json() */
    }

    /* ---- Validity 5.2 rule 3: decode the ESP half and let
     * zones_config_json_validate() -- the single source of truth -- judge
     * it, before anything else touches it. */
    /* Same "zero-init first, then copy whatever the real export produced"
     * pattern kiln_cfg_store_save_current()'s own scratch_cfg construction
     * above uses -- esp_blob_len is always exactly sizeof(zones_cfg_t) on a
     * real board (zones_config_blob_size()'s own contract), but this does
     * not hard-require equality so a byte-identical construction is
     * host-testable against a stubbed, deliberately-shorter blob size
     * without weakening anything: a package larger than this firmware's own
     * zones_cfg_t cannot possibly be one of its fields (only whole-struct
     * copies are ever produced), so oversized is still refused outright. */
    if (esp_blob_len > sizeof(zones_cfg_t)) {
        IMPORT_REFUSE("package's ESP configuration section is larger than this firmware's own "
                      "configuration -- likely from an incompatible build");
    }
    memset(&s->cand, 0, sizeof(s->cand));
    memcpy(&s->cand, s->esp_blob, esp_blob_len);
    const char *val_err = NULL;
    if (!zones_config_json_validate(&s->cand, &val_err)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "package's kiln configuration is not valid: %s",
                 val_err ? val_err : "unspecified");
        IMPORT_REFUSE(msg);
    }

    /* ---- Validity 5.2 rule 4/5: every Pico param id must be one this
     * firmware's own table recognises -- an unknown id is refused, not
     * skipped. This is a MIRROR of the Pico's own config_params_set()
     * range/id table (project_negative_test_on_a_mirror_is_vacuous) --
     * it catches the obvious case cheaply; a value this mirror wrongly
     * accepts is still subject to the Pico's own authoritative check at
     * apply time (kiln_cfg_swap.c), which this function never bypasses
     * since upload never applies. */
    for (uint16_t i = 0; i < s->pico.count; i++) {
        uint8_t known_type = 0;
        const char *known_name = NULL;
        if (!safety_cfg_store_lookup(s->pico.entries[i].param_id, &known_type, &known_name)) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "package's safety-processor section names parameter id %u, which this firmware "
                     "does not recognise -- refused, not skipped",
                     (unsigned)s->pico.entries[i].param_id);
            IMPORT_REFUSE(msg);
        }
    }

    /* ---- Hash (section 5.1: "one CRC over the whole package"). Recomputed
     * from the CANONICAL ESP form (H1) plus the decoded Pico half -- the
     * exact same two ingredients populate_pico_half_and_hash() feeds
     * kiln_package_compute_hash() for an ordinary save, so a package
     * downloaded and re-uploaded unmodified hashes identically. */
    size_t canonical_len = 0;
    if (!zones_config_export_canonical(&s->cand, s->canonical, sizeof(s->canonical), &canonical_len)) {
        IMPORT_REFUSE("internal error: could not canonicalize package for hash verification");
    }
    uint32_t recomputed = 0;
    if (!kiln_package_compute_hash(pkg_schema, s->canonical, (uint16_t)canonical_len, &s->pico, &recomputed)) {
        IMPORT_REFUSE("internal error: hash computation failed");
    }
    if (recomputed != declared_hash) {
        IMPORT_REFUSE("package hash does not match its contents -- the file is corrupted, was "
                      "hand-edited, or was truncated in transit");
    }

    /* ---- Compatibility 5.3, table rows 2/3 (plan section 5.3,
     * docs/audits/kiln_profiles_feature_review_2026-09-15.md Defect 5):
     * `ct_cal[].calibrated` is a property of the CT sensor physically wired
     * to ONE controller -- it means nothing, and is actively dangerous
     * (project_negative_test_on_a_mirror_is_vacuous's sibling hazard: a
     * fabricated-looking "calibrated" reading is worse than an honest
     * "uncalibrated" one), on any OTHER controller. A package is "foreign"
     * -- and its calibration is force-cleared -- whenever `source_board_id`
     * is either ABSENT (an old package, or one from firmware that predates
     * this field: fail-safe treats "unknown" the same as "different", never
     * the same as "same") or present and numerically different from this
     * board's own id. Only an exact match leaves calibration untouched.
     * `source_board_id` itself is never written into a slot and never
     * enters the hash (see kiln_package.h's ruling comment on
     * kiln_package_compute_hash()) -- it is consumed here, once, at import
     * time, and its only effect is this mutation. */
    bool foreign = !has_source_board || (source_board_id != kiln_board_identity_get());
    if (foreign) {
        for (uint16_t i = 0; i < s->pico.count; i++) {
            kiln_pkg_pico_param_t *e = &s->pico.entries[i];
            if (e->param_id == 0x0316u || e->param_id == 0x0317u || e->param_id == 0x0318u) {
                /* ct_cal[0..2].calibrated -- force to a SET, false value
                 * rather than leaving it unset: an explicit "known false"
                 * is a stronger, more auditable signal than "we don't
                 * know", and matches what a fresh commissioning run would
                 * report before the CT sweep has ever executed. */
                e->flags = KILN_PKG_PARAM_FLAG_SET;
                e->value_bits = 0; /* BOOL false */
            } else if (e->param_id == 0x031Au || e->param_id == 0x031Bu || e->param_id == 0x031Cu) {
                /* i_normal_a[0..2] -- forced UNSET (not zeroed-but-set): a
                 * S14/S15 "normal current" baseline of exactly 0.0 A read as
                 * SET would look like a real, trusted commissioning value
                 * rather than the absence of one -- the same "reading a
                 * fabricated current is worse than no reading" rule this
                 * plan states for ct_cal itself. */
                e->flags = 0;
                e->value_bits = 0;
            }
        }
        /* Re-hash: the mutation above changed the pico half's content, so
         * the hash stored in the new slot must cover what was ACTUALLY
         * stored, not the pre-mutation declared_hash -- otherwise a later
         * re-export/re-import round trip would fail its own hash check
         * against a slot whose content this function itself just edited.
         * `recomputed` is intentionally reassigned in place: everything
         * downstream (e->pkg_hash = recomputed) already expects this name. */
        if (!kiln_package_compute_hash(pkg_schema, s->canonical, (uint16_t)canonical_len, &s->pico, &recomputed)) {
            IMPORT_REFUSE("internal error: hash computation failed after cross-board calibration reset");
        }
    }

    /* ---- Compatibility 5.2a: properties of THIS CONTROLLER, reported
     * distinctly from the validity checks above. A package that is merely
     * DIFFERENT (different names/gains/a CT-less package on a CT-equipped
     * controller) is accepted -- only what this hardware cannot run at all
     * is refused. */
    uint8_t live_relay_count = zones_config_get_relay_count();
    uint8_t live_thermo_count = zones_config_get_thermo_count();
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        const zone_cfg_t *zc = &s->cand.zones[z];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if ((zc->relay_mask & (1u << bit)) && (uint8_t)(bit + 1) > live_relay_count) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "package configures zone %u on relay %u; this controller has only %u relay(s) "
                         "-- refused, not truncated",
                         (unsigned)z + 1, (unsigned)bit + 1, (unsigned)live_relay_count);
                IMPORT_REFUSE(msg);
            }
        }
        if (zc->thermo_mask != 0 && live_thermo_count > 0) {
            for (uint8_t bit = 0; bit < 8; bit++) {
                if ((zc->thermo_mask & (1u << bit)) && (uint8_t)(bit + 1) > live_thermo_count) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "package configures zone %u on thermocouple channel %u; this controller "
                             "has only %u channel(s) -- refused, not truncated",
                             (unsigned)z + 1, (unsigned)bit + 1, (unsigned)live_thermo_count);
                    IMPORT_REFUSE(msg);
                }
            }
        }
    }

    /* ---- Compatibility 5.3, table row 1 (docs/KILN_PROFILES_PLAN.md
     * section 5.3, docs/audits/kiln_profiles_feature_review_2026-09-15.md
     * Defect 5): the Pico's abs_max_temp_c ceiling must never be TIGHTER
     * than the highest configured zone max_temp_c in the SAME package --
     * the same rule zones_http_post.c's live-write path already enforces
     * (its own comment there is the citation), applied here at upload time
     * against the package's own two halves rather than the live config, and
     * bounded above by ZONE_MAX_TEMP_C_MAX, the one firmware-wide "no
     * temperature field may exceed this" sanity ceiling
     * (zones_config_accessors.h) -- reused rather than inventing a second
     * absolute-ceiling constant. Param id 0x0104 is "abs_max_temp_c" per
     * safety_cfg_store.c's own CONFIG_PARAM_TABLE mirror; looked up by id
     * (not name -- the wire table has no name lookup) with
     * safety_cfg_store_lookup() used only for the diagnostic name string. A
     * package whose Pico half never sets abs_max_temp_c at all cannot be
     * safety-checked against this rule and is refused rather than assumed
     * safe (0.0f as a checked value would look "tighter than everything",
     * masking the real defect: no ceiling packaged at all). */
    {
        float max_zone_temp_c = 0.0f;
        for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
            const zone_cfg_t *zc = &s->cand.zones[z];
            if (zc->thermo_mask != 0 && zc->max_temp_c > max_zone_temp_c) {
                max_zone_temp_c = zc->max_temp_c;
            }
        }
        bool have_abs_max = false;
        float pico_abs_max_temp_c = 0.0f;
        for (uint16_t i = 0; i < s->pico.count; i++) {
            if (s->pico.entries[i].param_id == 0x0104u
                    && (s->pico.entries[i].flags & KILN_PKG_PARAM_FLAG_SET) != 0) {
                union { uint32_t bits; float f; } conv;
                conv.bits = s->pico.entries[i].value_bits;
                pico_abs_max_temp_c = conv.f;
                have_abs_max = true;
                break;
            }
        }
        if (max_zone_temp_c > 0.0f) {
            if (!have_abs_max) {
                IMPORT_REFUSE("package's safety-processor section has no abs_max_temp_c ceiling set, "
                              "so it cannot be checked against the package's own configured zone "
                              "temperatures -- refused rather than assumed safe");
            }
            if (pico_abs_max_temp_c < max_zone_temp_c) {
                /* Pass the FLOAT expressions directly, not cast to (double):
                 * varargs promotes float to double either way, but an
                 * explicit (double) cast changes the expression's STATIC
                 * type as GCC's -Wformat-truncation sees it, which then sizes
                 * %.1f's worst case off double's full range (~1024 bytes for
                 * two substitutions) instead of float's (~192 bytes) --
                 * autotune_engine_step_identify.c's identify-duty refusal
                 * documents the same rule. No stack growth this way, per the
                 * project's httpd-stack-blob rule (never enlarge these
                 * buffers). */
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "package's safety-processor ceiling (abs_max_temp_c=%.1f C) is lower than its "
                         "own highest configured zone max_temp_c (%.1f C) -- the safety ceiling must "
                         "never be tighter than the kiln it packages with",
                         pico_abs_max_temp_c, max_zone_temp_c);
                IMPORT_REFUSE(msg);
            }
        }
        if (have_abs_max && pico_abs_max_temp_c > ZONE_MAX_TEMP_C_MAX) {
            /* Same float-not-double fix as the sibling IMPORT_REFUSE just
             * above -- see that comment. ZONE_MAX_TEMP_C_MAX is a float
             * literal (2500.0f), so this substitution stays float-sized
             * too. */
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "package's safety-processor ceiling (abs_max_temp_c=%.1f C) exceeds this "
                     "firmware's absolute sanity ceiling (%.1f C)",
                     pico_abs_max_temp_c, (float)ZONE_MAX_TEMP_C_MAX);
            IMPORT_REFUSE(msg);
        }
    }

    /* ---- Everything passed: create a NEW slot only -- never overwrite,
     * never apply (section 5.3: "a rejected upload leaves the active
     * configuration bit-for-bit untouched... upload writes into a new slot
     * only and never applies"). Since every check above already ran against
     * `cand`/`pico`, this is the same shape as kiln_cfg_store_save_current()
     * but sourcing its blob from the UPLOADED candidate rather than the
     * live config -- so it duplicates that function's slot-allocation and
     * persistence tail rather than routing the live config through it. */
    char normalized[KILN_CFG_NAME_MAX_LEN + 1];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        IMPORT_REFUSE("package's name is missing, too long, or invalid");
    }
    if (name_collides(normalized, KILN_CFG_NO_ACTIVE_ID)) {
        /* Same treatment save/clone already give a name collision -- append
         * nothing automatically; the operator renames afterward. */
        IMPORT_REFUSE("a saved kiln config already has that name -- rename it and upload again");
    }
    int idx = find_free_slot();
    if (idx < 0) {
        IMPORT_REFUSE("kiln config store is full");
    }
    int32_t id = s_store.next_id++;
    kiln_cfg_entry_t *e = &s_store.entries[idx];
    e->in_use = 1;
    e->id = id;
    strncpy(e->name, normalized, KILN_CFG_NAME_MAX_LEN);
    e->name[KILN_CFG_NAME_MAX_LEN] = '\0';
    memset(e->blob, 0, sizeof(e->blob));
    memcpy(e->blob, s->esp_blob, esp_blob_len);
    e->blob_len = esp_blob_len;
    e->pico = s->pico;
    e->pico_populated = 1;
    e->pkg_schema = pkg_schema;
    e->pkg_hash = recomputed;

    hal_status_t nvs_err = nvs_save_store();
    if (nvs_err != HAL_OK) {
        /* Defect 4, docs/audits/kiln_profiles_feature_review_2026-09-15.md:
         * this used to log and then still report success, leaving the new
         * slot RAM-only -- it silently disappeared at the next reboot with
         * no operator warning. Roll the RAM-only slot back so the store's
         * in-memory state matches what is reported (nothing persisted, so
         * nothing should appear to exist), and refuse instead of claiming
         * success. */
        memset(e, 0, sizeof(*e));
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "package was valid but could not be saved to flash (%s) -- not imported",
                 hal_status_to_name(nvs_err));
        IMPORT_REFUSE(msg);
    }
    if (out_id) {
        *out_id = id;
    }
    result = true;

done:
#undef IMPORT_REFUSE
    free(s);
    return result;
}

/* See kiln_cfg_store.h's own comment on this pair -- the escape hatch for
 * Defect 1 (docs/audits/kiln_profiles_feature_review_2026-09-15.md): the
 * slot an in-flight import's autosave should target when it is not (yet)
 * the same as s_store.active_id. Plain static, not behind s_swap_lock: the
 * two writers are kiln_cfg_swap.c and this file's own kiln_cfg_store_apply()
 * (adversarial review 2026-09-15,
 * docs/audits/review_autosave_slot_fix_a93ee77b_2026-09-15.md's HIGH finding
 * 1 -- the swap module has no production caller, kiln_cfg_store_apply() is
 * the one every real POST/LCD apply goes through), always while already
 * holding kiln_cfg_store_lock() around the same zones_config_import_blob()
 * call whose nvs_save() autosave dispatch this value steers. That shared
 * lock is what keeps the two writers from interleaving with each other; it
 * does NOT cover every other nvs_save() caller in the system (an ordinary
 * zones POST, an autotune write) -- one of those queuing onto the flash
 * worker while this override is set is a narrow, believed-unreachable-in-
 * practice race, not a proven-impossible one (LOW finding 5, same review). */
static int32_t s_autosave_target_override = KILN_CFG_AUTOSAVE_OVERRIDE_NONE;

/* The task that set the override above; NULL whenever no override is set.
 * 2026-09-16 cross-task fix -- see kiln_cfg_store.h's doc comment on
 * kiln_cfg_store_autosave_from_live_for_dispatcher() for the defect, the
 * confirmed trigger, and the two disproven alternative fixes. Written only
 * by kiln_cfg_store_set_autosave_target_override() below, which both
 * writers call while already holding kiln_cfg_store_lock(); read only
 * through autosave_active_target(). A single aligned pointer compare, so no
 * second lock is taken here -- one would deadlock against apply()'s own
 * s_swap_lock, which is held across an import whose nvs_save() blocks on
 * the flash worker. */
static void *s_autosave_target_override_owner = NULL;

/* Which slot an autosave dispatched by `dispatcher_task` must target.
 *
 * The override applies ONLY to the task that set it. Why that is both
 * sufficient and necessary:
 *   - Different task: an interloper (the PC control bridge's SET_ZONE_PID,
 *     running on bx_flash_worker) never matches, so it saves into
 *     s_store.active_id. Its save still HAPPENS -- it is merely no longer
 *     mis-steered. Nothing is dropped, suppressed or deferred.
 *   - Same task: the only way to be the owner task is to be inside the
 *     owner's own call chain, i.e. to BE the import's own autosave.
 *   - Owner == bx_flash_worker (an apply() reached from a job already
 *     running on the worker): still sound. The worker is a single task
 *     running jobs strictly one at a time, so while apply() is that job no
 *     other job can interleave at all -- there is no second "worker"
 *     dispatcher to collide with.
 *   - dispatcher_task == NULL (the plain entry points, and every caller
 *     reached through the posted fire-and-forget path, which has no `arg`
 *     to carry identity in): never matches, so such a job can never be
 *     steered by someone else's in-flight import. */
static int32_t autosave_active_target(void *dispatcher_task)
{
    if (s_autosave_target_override != KILN_CFG_AUTOSAVE_OVERRIDE_NONE && dispatcher_task != NULL &&
        dispatcher_task == s_autosave_target_override_owner) {
        return s_autosave_target_override;
    }
    return s_store.active_id;
}

/* 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md,
 * HIGH 1): true whenever an autosave deferred the active slot's Pico-half
 * recapture because a divergence was latched at the time -- see
 * kiln_cfg_store_autosave_from_live()'s own comment. Cleared the next time
 * either that function or kiln_cfg_store_recapture_pico_half_confirmed()
 * successfully performs the deferred recapture. Exposed read-only via
 * kiln_cfg_store_pico_half_recapture_pending() for a non-PSRAM-stacked,
 * NVS-capable caller to poll and retry -- wired up to ui_page_home_refresh.c's
 * LVGL tick (2026-09-15 review HIGH 1 fix); this module itself still has no
 * polling task of its own.
 *
 * 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md,
 * LOW 7/8): now three tasks can touch this pair (the flash worker via
 * kiln_cfg_store_autosave_from_live(), the httpd task via kiln_cfg_store_
 * recapture_pico_half_confirmed(), and the LVGL task via the new poller
 * above) -- s_pico_half_dirty_lock (a lazily-created mutex, same convention
 * as s_swap_lock just below) serializes every read/write of both fields so
 * no update can be lost. s_pico_half_dirty_slot records WHICH slot the
 * pending recapture belongs to ("reset one side of a pair" -- CLAUDE.md):
 * if the active slot changes (kiln swap, revert) while a recapture is
 * still owed, the flag must not silently get applied to the NEW slot, nor
 * leave the OLD slot's stale Pico half stuck forever with no flag pointing
 * at it -- kiln_cfg_store_set_active_id_raw() below drops a pending flag
 * that no longer matches when active_id changes away from the owing slot. */
static bool s_pico_half_dirty = false;
static int32_t s_pico_half_dirty_slot = KILN_CFG_NO_ACTIVE_ID;
static SemaphoreHandle_t s_pico_half_dirty_lock;

static void pico_half_dirty_lock_take(void)
{
    if (!s_pico_half_dirty_lock) {
        s_pico_half_dirty_lock = xSemaphoreCreateMutex();
    }
    if (s_pico_half_dirty_lock) {
        xSemaphoreTake(s_pico_half_dirty_lock, portMAX_DELAY);
    }
}

static void pico_half_dirty_lock_give(void)
{
    if (s_pico_half_dirty_lock) {
        xSemaphoreGive(s_pico_half_dirty_lock);
    }
}

static void pico_half_dirty_set(int32_t slot)
{
    pico_half_dirty_lock_take();
    s_pico_half_dirty = true;
    s_pico_half_dirty_slot = slot;
    pico_half_dirty_lock_give();
}

/* Clears unconditionally -- callers that just performed the recapture for
 * `slot` already know it matches (they read the slot alongside the pending
 * flag first); this helper is also reused by kiln_cfg_store_set_active_id_
 * raw() to drop a flag that no longer applies to any slot the caller cares
 * about. */
static void pico_half_dirty_clear(void)
{
    pico_half_dirty_lock_take();
    s_pico_half_dirty = false;
    s_pico_half_dirty_slot = KILN_CFG_NO_ACTIVE_ID;
    pico_half_dirty_lock_give();
}

/* Returns whether a recapture is pending AND, if `out_slot` is non-NULL,
 * the slot it is owed for. */
static bool pico_half_dirty_get(int32_t *out_slot)
{
    pico_half_dirty_lock_take();
    bool dirty = s_pico_half_dirty;
    int32_t slot = s_pico_half_dirty_slot;
    pico_half_dirty_lock_give();
    if (out_slot) {
        *out_slot = slot;
    }
    return dirty;
}

/* LOW 7 support -- called from kiln_cfg_store_set_active_id_raw() above
 * (forward-declared there) whenever active_id is about to change away from
 * `slot`. Drops the pending flag ONLY if it still points at the slot that
 * is losing active-id status; a flag already re-owned by (or never owned
 * by) that slot is left untouched. Prevents both halves of LOW 7/LOW 8: a
 * flag silently surviving to apply to the wrong slot, and one orphaned
 * forever once its slot can never become active again to clear it. */
static void pico_half_dirty_drop_if_owned_by(int32_t slot)
{
    pico_half_dirty_lock_take();
    if (s_pico_half_dirty && s_pico_half_dirty_slot == slot) {
        s_pico_half_dirty = false;
        s_pico_half_dirty_slot = KILN_CFG_NO_ACTIVE_ID;
    }
    pico_half_dirty_lock_give();
}

void kiln_cfg_store_set_autosave_target_override(int32_t id_or_none_sentinel)
{
    s_autosave_target_override = id_or_none_sentinel;
    /* Owner recorded and cleared in the SAME statement pair as the value it
     * guards -- "reset one side of a pair" (CLAUDE.md): these two must never
     * be able to move apart, so there is deliberately no separate setter for
     * the owner. */
    s_autosave_target_override_owner =
        (id_or_none_sentinel != KILN_CFG_AUTOSAVE_OVERRIDE_NONE) ? (void *)xTaskGetCurrentTaskHandle() : NULL;
}

/* 2026-09-15 review (review_autosave_rework_5bc9afb5_2026-09-15.md, MEDIUM)
 * -- see this function's own doc comment in kiln_cfg_store.h. Registered by
 * kiln_cfg_swap.c at bring-up; NULL (never registered, or a host test that
 * doesn't need it) reads as "no swap pending", matching every other
 * off-by-default seam in this file. */
static bool (*s_swap_pending_fn)(void) = NULL;

void kiln_cfg_store_set_swap_pending_source(bool (*fn)(void))
{
    s_swap_pending_fn = fn;
}

static bool autosave_blocked_by_swap_pending(void)
{
    return s_swap_pending_fn ? s_swap_pending_fn() : false;
}

bool kiln_cfg_store_autosave_from_live_for_dispatcher(void *dispatcher_task, char *reason_out,
                                                      size_t reason_cap)
{
    /* 2026-09-15 review (review_divergence_check_561efa3b_2026-09-15.md,
     * HIGH 1 / docs/KILN_PROFILES_PLAN.md sec 2.4 rule 6): "Suppressed while
     * CONFIG_DIVERGENCE is latched. Writing a package hash while the two
     * processors are known to disagree would launder the divergence into a
     * 'consistent' saved state." Before this check, an ordinary zones/
     * autotune/coupling autosave after a Pico reboot-revert would re-snapshot
     * the Pico's OWN (reverted) live cache back into the active slot via
     * populate_pico_half_and_hash() -> kiln_package_capture_pico_half(),
     * silently making the slot's "expected" record agree with the reverted
     * value and clearing the standing-divergence alarm on the very next save
     * -- the "reset one side of a pair" bug class. Checked against BOTH the
     * ceiling-enforcing verdict and the broadened (HIGH 2 interim,
     * warning-only) verdict: a known disagreement in either is a real
     * disagreement between the two processors' configs and must not be
     * laundered by this function, regardless of which one currently forces
     * heat off. This is a skip, not a failure -- the dirty flag this function
     * was called to flush simply persists until the next opportunity, same
     * as rule 5's apply-transaction/firing-tick suppression already does.
     *
     * 2026-09-15 review (review_autosave_rework_5bc9afb5_2026-09-15.md,
     * MEDIUM): also suppressed while a kiln_cfg_swap.c transaction has a
     * pending record (marker != NONE), NOT only while the latch already
     * reads diverged -- see kiln_cfg_store_set_swap_pending_source()'s doc
     * comment (kiln_cfg_store.h) for the exact race this closes: the swap
     * moves active_id to target_id before its own divergence check runs,
     * and a recapture racing into that same window is what would clear the
     * latch, so the latch alone cannot be trusted to have caught it yet.
     *
     * 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md,
     * HIGH 1): the block above used to suppress the WHOLE save (ESP half
     * included) while EITHER verdict was latched, and reported that as
     * success (`return true`). A standing (non-ceiling) divergence has no
     * self-clearing trigger anywhere in this codebase -- nothing re-arms an
     * autosave attempt once one is skipped -- so any ordinary standing
     * mismatch left autosave silently, permanently off: every later zones/
     * autotune/coupling edit kept being "saved" (callers saw `true`, no
     * warning) while the slot's on-disk blob silently stopped tracking live
     * state. Fix: only the PICO-HALF RECAPTURE is skipped while diverged
     * (still the correct call -- see the "reset one side of a pair" comment
     * above, unchanged), not the ESP-half export/persist. The ESP half is
     * always safe to save regardless of Pico divergence; it is the Pico
     * half specifically that must not be re-snapshotted from a cache known
     * to disagree with what was last confirmed pushed. A skipped Pico-half
     * recapture sets a dirty flag; the very next call that finds the
     * divergence cleared performs the deferred recapture (kiln_cfg_store_
     * save_current_ex's `recapture_pico_half=true` path) and clears the
     * flag, logged at INFO. Every call that must defer logs a rate-limited
     * WARNING (never merely a debug line -- HIGH 2 above already means the
     * divergence itself is visible; this is the SEPARATE "your saved
     * profile may now be behind the live Pico expectation" signal) and
     * ALWAYS fills reason_out describing the deferral -- this function must
     * never report a silent, unqualified success while a recapture was
     * actually skipped. */
    /* 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md,
     * MEDIUM 3): the divergence latches above are updated once per
     * safety_poll_task tick, on a different task than this one. Reading
     * "not diverged" here proves only that the LATCH was clear as of its
     * last evaluation -- not that the safety_cfg_store cache this function
     * is about to snapshot into the "expected" record hasn't since been
     * refreshed by a refetch racing in on that other task. Bracket the
     * divergence read with the cache's generation counter (bumped exactly
     * once per successful refetch, safety_cfg_store.c) and treat "the
     * generation moved while we were deciding" the same as "diverged" --
     * defer the Pico-half recapture rather than risk capturing a value one
     * refetch newer than what the divergence check actually evaluated. This
     * closes the window without a cross-module lock (never hold a lock
     * across the producer/blocking calls this function already avoids). */
    char div_reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool ceiling_diverged = safety_ceiling_sync_is_diverged(div_reason, sizeof(div_reason));
    bool standing_diverged = !ceiling_diverged && safety_ceiling_sync_is_standing_diverged(div_reason, sizeof(div_reason));
    bool diverged = ceiling_diverged || standing_diverged;
    if (!diverged && safety_cfg_store_cache_generation() != safety_ceiling_sync_latch_evaluated_generation()) {
        /* The Pico-config cache has been refreshed (a refetch landed) more
         * recently than the divergence latch above was last recomputed
         * against it -- e.g. safety_poll_task's own maybe_refetch() just ran
         * this tick but enforce_ceiling_divergence() has not yet re-evaluated
         * against the new cache contents (same tick, sequential, but this
         * task can be preempted in between). The latch reading "not
         * diverged" here proves nothing about the value we are about to
         * snapshot; treat it as diverged and defer, same as an actual
         * mismatch would. */
        diverged = true;
        snprintf(div_reason, sizeof(div_reason), "Pico config cache refreshed since the divergence check last ran");
    }

    if (autosave_blocked_by_swap_pending()) {
        if (reason_out && reason_cap > 0) {
            snprintf(reason_out, reason_cap, "autosave suppressed: a kiln config swap transaction is pending");
        }
        return true; /* not a failure -- see comment above */
    }

    int32_t active = autosave_active_target(dispatcher_task);
    if (active == KILN_CFG_NO_ACTIVE_ID) {
        return true; /* nothing to autosave into -- not a failure */
    }
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!kiln_cfg_store_get_name(active, name, sizeof(name))) {
        /* active_id points at a slot that no longer exists (deleted out from
         * under it, or a corrupt/never-persisted store) -- nothing sane to
         * autosave into; not this function's job to repair active_id. */
        return true;
    }

    if (diverged) {
        static int64_t s_last_deferred_log_us = 0;
        int64_t now_us = (int64_t)hal_time_now_us();
        if (now_us - s_last_deferred_log_us >= 60000000LL) {
            ESP_LOGW(TAG,
                     "kiln_cfg_store: autosave of '%s' deferred (ESP half only) -- Pico-half "
                     "recapture skipped while ESP/Pico config divergence is latched (%s); "
                     "will retry once cleared",
                     name, div_reason);
            s_last_deferred_log_us = now_us;
        }
        pico_half_dirty_set(active);
    } else if (pico_half_dirty_get(NULL)) {
        ESP_LOGI(TAG,
                 "kiln_cfg_store: divergence cleared -- catching up deferred Pico-half recapture "
                 "for '%s'",
                 name);
    }

    /* Re-saves OVER the active slot -- id_or_negative == active means
     * kiln_cfg_store_save_current_ex() overwrites entry `active` in place
     * rather than allocating a new one, and (since id_or_negative >= 0)
     * does NOT touch active_id itself, which is already correct. This is
     * the exact "export blob / recapture Pico half / recompute pkg_hash /
     * persist" sequence section 2.4 asks for, reusing rather than
     * re-implementing it -- except the Pico-half recapture step is skipped
     * (existing captured half kept) while `diverged` is true. */
    bool ok = kiln_cfg_store_save_current_ex(name, active, NULL, /*recapture_pico_half=*/!diverged, reason_out,
                                              reason_cap);
    if (ok && !diverged) {
        pico_half_dirty_clear();
    }
    if (ok && diverged && reason_out && reason_cap > 0) {
        /* save_current_ex() succeeded and filled reason_out with nothing (it
         * only fills it on failure) -- overwrite with the deferral notice so
         * a caller inspecting reason_out never sees an empty string next to
         * a bare `true` while a recapture was actually skipped. */
        snprintf(reason_out, reason_cap,
                 "autosave: ESP half saved, Pico-half recapture deferred -- ESP/Pico config "
                 "divergence latched (%s)",
                 div_reason);
    }
    return ok;
}

bool kiln_cfg_store_pico_half_recapture_pending(void)
{
    return pico_half_dirty_get(NULL);
}

bool kiln_cfg_store_recapture_pico_half_confirmed_for_dispatcher(void *dispatcher_task, char *reason_out,
                                                                 size_t reason_cap)
{
    /* 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md,
     * MEDIUM 3 / HIGH3 race): callers of this function have JUST performed
     * and confirmed (via readback) their own push to the Pico -- e.g.
     * safety_cfg_http.c's commissioning handler, right after its own
     * set+commit+confirm sequence. A freshly-confirmed push cannot be
     * "laundering" a stale/reverted value the way an ordinary autosave
     * triggered by an unrelated zones/autotune edit could, so this entry
     * point deliberately bypasses the standing-diverged gate that
     * kiln_cfg_store_autosave_from_live() enforces above -- gating THIS
     * call on that same latch would deadlock: the commissioning push is
     * often the very thing that would clear the divergence once its Pico
     * half is recaptured, so "wait for divergence to clear before
     * recapturing" can never resolve for this caller. Still respects the
     * swap-pending gate (a swap transaction mid-flight is a real reason to
     * defer, unrelated to divergence) and still targets the active slot,
     * same as the ordinary autosave path. */
    if (autosave_blocked_by_swap_pending()) {
        if (reason_out && reason_cap > 0) {
            snprintf(reason_out, reason_cap, "recapture suppressed: a kiln config swap transaction is pending");
        }
        return true; /* not a failure -- see comment above */
    }
    int32_t active = autosave_active_target(dispatcher_task);
    if (active == KILN_CFG_NO_ACTIVE_ID) {
        return true;
    }
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!kiln_cfg_store_get_name(active, name, sizeof(name))) {
        return true;
    }
    bool ok = kiln_cfg_store_save_current_ex(name, active, NULL, /*recapture_pico_half=*/true, reason_out, reason_cap);
    if (ok) {
        pico_half_dirty_clear();
    }
    return ok;
}

/* Plain entry points: no dispatcher identity, so the autosave-target
 * override is never honored (kiln_cfg_store.h explains why that default is
 * the safe one). Every caller that is NOT an in-flight import's own
 * autosave -- safety_poll_task's POSTED deferred recapture, safety_cfg_http.c's
 * commissioning recapture, the LVGL poller -- goes through these. */
bool kiln_cfg_store_autosave_from_live(char *reason_out, size_t reason_cap)
{
    return kiln_cfg_store_autosave_from_live_for_dispatcher(NULL, reason_out, reason_cap);
}

bool kiln_cfg_store_recapture_pico_half_confirmed(char *reason_out, size_t reason_cap)
{
    return kiln_cfg_store_recapture_pico_half_confirmed_for_dispatcher(NULL, reason_out, reason_cap);
}

uint32_t kiln_cfg_store_generation(void)
{
    /* s_kiln_cfg_rev already IS this store's generation counter -- bumped
     * exactly once inside nvs_save_store(), the single choke-point every
     * mutating public function (save/clone/apply/delete/rename, and this
     * file's own two swap-support writers above) already funnels through.
     * Reusing it rather than adding a second counter avoids a second
     * "reset one side, not the other" hazard (project_reset_one_side_bug_
     * class) between two counters that would otherwise need to move
     * together forever. */
    return s_kiln_cfg_rev;
}

/* ---- Standing ESP/Pico config-divergence fix (docs/audits/
 * kiln_profiles_feature_review_2026-09-15.md Defect 2) --------------------
 * See kiln_cfg_store.h's doc comment for this function's contract, and
 * safety_ceiling_sync.h's doc comment on safety_ceiling_expected_pico_
 * fields_fn for why this lives here (the persist layer) rather than in
 * safety_ceiling_sync.c itself. */
size_t kiln_cfg_store_capture_expected_pico_fields(safety_ceiling_expected_param_t *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    int32_t active = kiln_cfg_store_get_active_id();
    if (active == KILN_CFG_NO_ACTIVE_ID) {
        return 0; /* no saved kiln config applied this boot -- nothing to broaden the check with */
    }
    kiln_pkg_safety_t pico;
    /* blob_out=NULL: only the Pico half is needed here, and kiln_cfg_store_
     * get_full_package() documents NULL as skipping the ESP-blob copy
     * entirely (confirmed by reading its implementation above) -- cheap,
     * no ESP zones-blob memcpy for a call that runs every safety_poll_task
     * tick. Also safely refuses (returns false) on a legacy half-package
     * (pico_populated == 0), which this function treats the same as "no
     * active slot": nothing to broaden with, not an error. */
    if (!kiln_cfg_store_get_full_package(active, NULL, 0, NULL, &pico, NULL, 0)) {
        return 0;
    }
    size_t n = 0;
    for (uint16_t i = 0; i < pico.count && n < cap; i++) {
        const kiln_pkg_pico_param_t *p = &pico.entries[i];
        if (!(p->flags & KILN_PKG_PARAM_FLAG_SET)) {
            continue; /* never fabricate a value for a param this slot never captured */
        }
        if (p->param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C) {
            continue; /* stays the existing dedicated field in safety_ceiling_sync.c, never duplicated */
        }
        if (p->param_id == SAFETY_PARAM_ID_TC_TYPE) {
            /* 2026-09-15 review (MEDIUM 4): the ESP has no push path for
             * tc_type (commissioning-page-owned, ESP read-only) -- see
             * SAFETY_PARAM_ID_TC_TYPE's own doc comment. Including it here
             * would count every Pico-side tc_type change (including a
             * post-reboot default) as a standing divergence the ESP can
             * never clear, which wedges the deferred recapture via the
             * autosave gate above. */
            continue;
        }
        /* Type-aware widen into float -- identical switch to kiln_cfg_
         * swap.c's pico_readback_matches()/normalize_f32_like_wire() call
         * site, so the expected side here and the live side safety_
         * ceiling_sync.c decodes from safety_cfg_store's cache use the same
         * convention for non-float params. No %.9g wire-round-trip
         * normalization here -- config_divergence_check()/config_identity_
         * normalize_f32() (config_divergence.c) already normalizes every
         * float field exactly once, at comparison time; doing it twice
         * would be redundant, not wrong, but this keeps ONE place that
         * owns it. */
        float value;
        switch (p->type) {
        case KILNLINK_PARAM_TYPE_BOOL:
        case KILNLINK_PARAM_TYPE_U8:
            value = (float)(uint8_t)(p->value_bits & 0xFFu);
            break;
        case KILNLINK_PARAM_TYPE_U16:
            value = (float)(uint16_t)(p->value_bits & 0xFFFFu);
            break;
        case KILNLINK_PARAM_TYPE_F32:
        default: {
            uint32_t bits = p->value_bits;
            memcpy(&value, &bits, sizeof(value));
            break;
        }
        }
        out[n].param_id = p->param_id;
        out[n].value = value;
        n++;
    }
    return n;
}
