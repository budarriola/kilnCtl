#include "kiln_cfg_store.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ota_http.h"
#include "zones_config_accessors.h"

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

/* Shared namespace name with zones_http.c/rules_http.c/relay_cycles.c/
 * run_state.c/ota_record.c/profiles_builtin.c/profiles_http.c/unit_pref.c --
 * each of those files independently #defines the identical "kiln_cfg"
 * string, same convention this file follows, distinguished by KEY not
 * namespace. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_STORE "kilncfgs"

/* Bump whenever kiln_cfg_store_blob_t's on-flash layout changes -- mirrors
 * ZONES_CFG_VERSION's role in zones_http.c. Version 2 is current; version 1
 * is still readable via nvs_load_store()'s migration branch
 * (migrate_store_v1_to_current() below), the same discipline
 * migrate_zones_cfg_v1_to_current() established for zones_cfg_t. A future
 * bump needs another branch added alongside that one. */
#define KILN_CFG_STORE_VERSION 2

/* v1's blob ceiling. ZONES_CONFIG_BLOB_MAX_SIZE was widened 512 -> 640 on
 * 2026-08-30 (zone_cfg_t gained the four PID_EXPANSION_PLAN.md Phase 2
 * fields), and because that macro sizes a member of the PERSISTED struct
 * below -- not just a runtime ceiling -- widening it changed
 * sizeof(kiln_cfg_store_blob_t). nvs_load_store()'s `len != sizeof(loaded)`
 * check would then have read every existing board's saved store as
 * corruption and silently discarded every named kiln config and active_id.
 * Frozen here so the v1 layout can still be read and migrated. */
#define KILN_CFG_STORE_BLOB_MAX_SIZE_V1 512u

/* One saved kiln config slot. blob/blob_len hold whatever
 * zones_config_export_blob() produced at save time -- an opaque byte string
 * to this module, sized against zones_http.h's ZONES_CONFIG_BLOB_MAX_SIZE
 * ceiling so this struct's layout never has to change just because
 * zone_cfg_t grew a field (that only ever changes zones_config_blob_size()'s
 * RUNTIME return value, not this fixed-size array). */
typedef struct {
    uint8_t in_use;
    int32_t id;
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    uint16_t blob_len;
    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
} kiln_cfg_entry_t;

typedef struct {
    uint8_t version;
    /* KILN_CFG_NO_ACTIVE_ID (-1) if nothing is currently applied-and-tracked
     * as the "starting point" config -- see kiln_cfg_store_init()'s doc
     * comment (kiln_cfg_store.h) for the three boot-fallback outcomes this
     * drives. */
    int32_t active_id;
    /* Monotonic; never reused, even across a delete -- so a stale id a
     * client cached from before a delete can never silently resolve to a
     * DIFFERENT config that later reused the same number. Starts at 1 (0 is
     * never assigned) purely so "id == 0" reads as obviously-uninitialized
     * in a debug dump; nothing tests against 0 specially otherwise. */
    int32_t next_id;
    kiln_cfg_entry_t entries[KILN_CFG_MAX_COUNT];
} kiln_cfg_store_blob_t;

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
    kiln_cfg_entry_v1_t entries[KILN_CFG_MAX_COUNT];
} kiln_cfg_store_blob_v1_t;

/* Budget guard: kiln_cfg_store_blob_t is a permanent member of s_store
 * (static, BSS-resident) and kiln_cfg_store_blob_v1_t is heap-allocated only
 * transiently, on the once-ever v1-migration path in nvs_load_store() below
 * (malloc'd, freed before that function returns) -- neither is ever an
 * ordinary function-local/stack buffer, so this is no longer a stack budget.
 * What it actually bounds now: s_store's permanent BSS footprint plus the
 * transient heap high-water mark the migration path can hit, added together
 * as a single loose tripwire so a future ZONES_CONFIG_BLOB_MAX_SIZE widening
 * (or KILN_CFG_MAX_COUNT bump) gets caught here instead of silently growing
 * either cost. 16384 is deliberately loose, not a real budget -- the point
 * is only to force a human back to this comment and nvs_load_store()'s
 * reasoning before either struct doubles again. */
/* Portable compile-time assert (not _Static_assert): this file is compiled
 * both by the ESP-IDF (xtensa-gcc, C11) build and, #included directly, by
 * this repo's MSVC host tests (test_kiln_cfg_store.c) which are not
 * necessarily invoked in C11 mode. A negative array size is a hard error in
 * every C standard this file has ever been built under. */
typedef char kiln_cfg_store_blob_budget_check
    [(sizeof(kiln_cfg_store_blob_t) + sizeof(kiln_cfg_store_blob_v1_t) < 16384) ? 1 : -1];

/* Migrates a v1 store into the current layout: every field copied by name,
 * the shorter v1 blob copied into the wider array and the remainder left
 * zeroed. Lossless -- a v1 blob is a complete zones config that a v1-era
 * build wrote, and zones_http.c's own decoder handles its version separately
 * (it carries its own ZONES_CFG_VERSION inside those bytes). */
static void migrate_store_v1_to_current(const kiln_cfg_store_blob_v1_t *src, kiln_cfg_store_blob_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->version = KILN_CFG_STORE_VERSION;
    dst->active_id = src->active_id;
    dst->next_id = src->next_id;
    for (size_t i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        const kiln_cfg_entry_v1_t *se = &src->entries[i];
        kiln_cfg_entry_t *de = &dst->entries[i];
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

static kiln_cfg_store_blob_t s_store;

/* ---- NVS ------------------------------------------------------------------ */

/* Copied from zones_http.c's nvs_partition_init() (see that file for the
 * full rationale) -- each module using kiln_nvs brings the partition up
 * independently rather than assuming another module already has;
 * nvs_flash_init_partition() on an already-initialized partition is a
 * harmless no-op. */
static esp_err_t nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
                 partition, esp_err_to_name(err));
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return err;
}

static void reset_to_defaults(void)
{
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
static esp_err_t nvs_save_store(void);

static void nvs_load_store(void)
{
    reset_to_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return; /* ESP_ERR_NVS_NOT_FOUND (never saved) or partition trouble -- defaults stand */
    }

    /* Read the size first, so a v1-sized blob can be migrated instead of
     * being dismissed as corruption by the exact-size check below. */
    size_t stored_len = 0;
    err = nvs_get_blob(h, NVS_KEY_STORE, NULL, &stored_len);
    if (err != ESP_OK) {
        nvs_close(h);
        return; /* nothing stored, or unreadable -- defaults stand */
    }

    if (stored_len == sizeof(kiln_cfg_store_blob_v1_t)) {
        /* A board saved by a pre-2026-08-30 build. Its entries are the same
         * data, just with a 512-byte blob array instead of 640. Migrate
         * rather than discard: this is a user's named kiln configs.
         *
         * Heap-allocated, not `static`/stack: sizeof(kiln_cfg_store_blob_v1_t)
         * is 4396 bytes (KILN_CFG_MAX_COUNT=8 entries, each
         * KILN_CFG_STORE_BLOB_MAX_SIZE_V1=512 blob + header/padding =~ 548
         * bytes -- verified by hand against the struct layout above, not
         * assumed). This branch runs at most once per board (the very next
         * boot takes the fast, already-current-version path below), so it is
         * not worth 4396 bytes of *permanent* BSS the other ~99.99% of boots
         * never touch. malloc() failure is handled exactly like the
         * "unreadable, defaults stand" branch a few lines below -- there is
         * nothing special about running out of heap here versus any other
         * read failure. */
        kiln_cfg_store_blob_v1_t *v1 = malloc(sizeof(*v1));
        if (!v1) {
            nvs_close(h);
            ESP_LOGW(TAG, "kiln_cfg_store v1 migration buffer alloc failed -- defaults stand");
            return;
        }
        size_t v1_len = sizeof(*v1);
        err = nvs_get_blob(h, NVS_KEY_STORE, v1, &v1_len);
        nvs_close(h);
        if (err != ESP_OK || v1_len != sizeof(*v1)) {
            ESP_LOGW(TAG, "kiln_cfg_store v1 blob could not be re-read -- defaults stand");
            free(v1);
            return;
        }
        if (v1->version != 1) {
            ESP_LOGW(TAG, "kiln_cfg_store blob is v1-SIZED but claims version %u -- treating as corrupt",
                     (unsigned)v1->version);
            free(v1);
            return;
        }
        migrate_store_v1_to_current(v1, &s_store);
        free(v1);
        ESP_LOGI(TAG, "kiln_cfg_store migrated v1 -> v%u (blob ceiling %u -> %u); saved kiln configs kept",
                 (unsigned)KILN_CFG_STORE_VERSION, (unsigned)KILN_CFG_STORE_BLOB_MAX_SIZE_V1,
                 (unsigned)ZONES_CONFIG_BLOB_MAX_SIZE);
        nvs_save_store(); /* rewrite in the current layout so the next boot takes the fast path */
        return;
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
    err = nvs_get_blob(h, NVS_KEY_STORE, &s_store, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        reset_to_defaults(); /* nothing stored, or unreadable, or a partial read -- defaults stand */
        return;
    }
    if (len != sizeof(s_store)) {
        /* Wrong size for ANY version's claimed layout is genuine corruption
         * -- a real blob is always written at exactly sizeof(s_store) (see
         * nvs_save_store()). Nothing here is worth protecting; defaults
         * stand, same as "nothing was ever saved." */
        ESP_LOGW(TAG, "kiln_cfg_store blob is the wrong size -- treating as corrupt, resetting to an "
                      "empty store rather than risking a half-understood layout");
        reset_to_defaults();
        return;
    }
    if (s_store.version == KILN_CFG_STORE_VERSION) {
        return; /* current version, right size -- happy path, s_store already holds it */
    }
    if (s_store.version < KILN_CFG_STORE_VERSION) {
        /* Reachable only for a version between 1 (handled by the size-based
         * migration branch above) and KILN_CFG_STORE_VERSION for which no
         * migration chain has been written yet -- an older-version blob with
         * no defined conversion is exactly as unusable as a wrong-size one,
         * not a case where the data is newer than this firmware understands,
         * so it is treated as corrupt rather than refused. */
        ESP_LOGW(TAG, "kiln_cfg_store blob is version %u, older than this firmware's %u, and no "
                      "migration chain exists yet -- treating as corrupt, resetting to an empty store",
                 (unsigned)s_store.version, (unsigned)KILN_CFG_STORE_VERSION);
        reset_to_defaults();
        return;
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
    ESP_LOGW(TAG, "kiln_cfg_store blob is version %u, newer than this firmware's %u -- refusing to "
                  "load, flash data left untouched",
             (unsigned)s_store.version, (unsigned)KILN_CFG_STORE_VERSION);
    reset_to_defaults();
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
    volatile int stack_probe = 0; /* only its ADDRESS matters; volatile+initialised so -Werror=maybe-uninitialized doesn't flag it and it can't be optimised out of the frame. */
    return esp_ptr_external_ram((void *)&stack_probe);
}

static esp_err_t nvs_save_store(void)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "nvs_save_store: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). A flash/NVS write from here would abort the whole board "
                      "(ESP-IDF's esp_task_stack_is_sane_cache_disabled()). Route this call "
                      "through a task with an internal-SRAM stack instead -- see "
                      "DRAM_PSRAM_PLAN.md section 7.2 and uart_bridge_ext.c's flash-safe "
                      "worker for the established pattern.");
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    s_store.version = KILN_CFG_STORE_VERSION;
    err = nvs_set_blob(h, NVS_KEY_STORE, &s_store, sizeof(s_store));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
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
    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- kiln configs will not persist",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
        reset_to_defaults();
        return part_err;
    }
    nvs_load_store();

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
            s_store.active_id = KILN_CFG_NO_ACTIVE_ID;
            (void)nvs_save_store();
        } else {
            char reason[96];
            reason[0] = '\0';
            if (!zones_config_import_blob(s_store.entries[idx].blob, s_store.entries[idx].blob_len,
                                          reason, sizeof(reason))) {
                ESP_LOGW(TAG, "active kiln config id=%ld failed validation at boot (%s) -- clearing "
                              "it, keeping whatever zones config already loaded",
                         (long)s_store.active_id, reason);
                s_store.active_id = KILN_CFG_NO_ACTIVE_ID;
                (void)nvs_save_store();
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

bool kiln_cfg_store_save_current(const char *name, int32_t id_or_negative, int32_t *out_id,
                                  char *reason_out, size_t reason_cap)
{
    char normalized[KILN_CFG_NAME_MAX_LEN + 1];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return set_reason(reason_out, reason_cap, "name missing or too long");
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

    if (id_or_negative < 0) {
        /* A config just saved FROM the running kiln is, by construction,
         * exactly what's live right now -- marking it active is recording a
         * fact, not applying anything. Overwriting an existing entry
         * (id_or_negative >= 0) does NOT do this -- see this function's own
         * header comment. */
        s_store.active_id = id;
    }

    esp_err_t err = nvs_save_store();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_store after save failed: %s -- saved live but will not survive a reboot",
                 esp_err_to_name(err));
    }
    if (out_id) {
        *out_id = id;
    }
    return true;
}

bool kiln_cfg_store_clone(int32_t src_id, const char *name, int32_t *out_id, char *reason_out,
                          size_t reason_cap)
{
    char normalized[KILN_CFG_NAME_MAX_LEN + 1];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return set_reason(reason_out, reason_cap, "name missing or too long");
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
    esp_err_t err = nvs_save_store();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_store after clone failed: %s -- cloned live but will not survive a reboot",
                 esp_err_to_name(err));
    }
    if (out_id) {
        *out_id = new_id;
    }
    return true;
}

bool kiln_cfg_store_apply(int32_t id, bool ack_no_safety_processor, char *reason_out,
                          size_t reason_cap)
{
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
    /* zones_config_import_blob() does the actual all-or-nothing
     * version-check/re-validate/commit work; see its own doc comment
     * (zones_http.h). */
    if (!zones_config_import_blob(s_store.entries[idx].blob, s_store.entries[idx].blob_len, reason_out,
                                  reason_cap)) {
        return false;
    }
    s_store.active_id = id;
    esp_err_t err = nvs_save_store();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_store after apply failed: %s -- active id applied live but will not "
                      "survive a reboot",
                 esp_err_to_name(err));
    }
    return true;
}

bool kiln_cfg_store_delete(int32_t id)
{
    int idx = find_index_by_id(id);
    if (idx < 0) {
        return false;
    }
    memset(&s_store.entries[idx], 0, sizeof(s_store.entries[idx]));
    if (s_store.active_id == id) {
        s_store.active_id = KILN_CFG_NO_ACTIVE_ID;
    }
    esp_err_t err = nvs_save_store();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_store after delete failed: %s -- deleted live but will not survive a reboot",
                 esp_err_to_name(err));
    }
    return true;
}

bool kiln_cfg_store_rename(int32_t id, const char *name)
{
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
    esp_err_t err = nvs_save_store();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_store after rename failed: %s -- renamed live but will not survive a reboot",
                 esp_err_to_name(err));
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
