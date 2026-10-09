// kiln_cfg_store_internal -- the CURRENT-VERSION on-flash layout of the
// whole kiln config store, shared between kiln_cfg_store.c (which owns the
// NVS side, migration, and every public accessor) and
// kiln_cfg_store_cfg_fs.c (the `cfg` LittleFS read-through/dual-write bridge
// for it, docs/FILESYSTEM_USER_DATA.md section 5's "kiln config slots"
// item). Split out purely so the bridge module can operate on
// kiln_cfg_store_blob_t by value/pointer without kiln_cfg_store.c having to
// make its whole internal layout public via kiln_cfg_store.h -- the same
// "shared internal header, not a public one" convention profiles_http_
// internal.h and zones_http_internal.h already use for their own siblings.
//
// This header carries ONLY the current-version layout. The frozen v1 layout
// (kiln_cfg_entry_v1_t/kiln_cfg_store_blob_v1_t) stays private to
// kiln_cfg_store.c -- nothing outside its own migration path ever needs it,
// and the file format this pair's bridge module writes/reads is always the
// CURRENT version (like zones_config_cfg_fs.c's file: a stale-version file
// is just treated as invalid and the NVS candidate decides, see that
// module's own header comment).
#ifndef KILN_CFG_STORE_INTERNAL_H
#define KILN_CFG_STORE_INTERNAL_H

#include <stdint.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#include "kiln_cfg_store.h" /* KILN_CFG_NAME_MAX_LEN, KILN_CFG_MAX_COUNT */
#include "kiln_package.h" /* kiln_pkg_safety_t, KILN_PKG_SAFETY_PARAM_CAP -- the Pico half, v3+ */
#include "zones_config_accessors.h" /* ZONES_CONFIG_BLOB_MAX_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

/* Bump whenever kiln_cfg_store_blob_t's on-flash layout changes -- see
 * kiln_cfg_store.c's fuller comment (the version-1/2/3 migration chain lives
 * there, not here). Version 3 is current: docs/KILN_PROFILES_PLAN.md items
 * 1/2/12 raised KILN_CFG_MAX_COUNT 8 -> 10 and added each entry's Pico-half
 * package (pico_populated/pkg_schema/pkg_hash/pico below) alongside the
 * pre-existing ESP-only blob. */
#define KILN_CFG_STORE_VERSION 3

/* One saved kiln config slot. blob/blob_len hold whatever
 * zones_config_export_blob() produced at save time -- an opaque byte string
 * to this module, sized against zones_http.h's ZONES_CONFIG_BLOB_MAX_SIZE
 * ceiling so this struct's layout never has to change just because
 * zone_cfg_t grew a field.
 *
 * v3 additions (docs/KILN_PROFILES_PLAN.md items 1/2/12): the Pico's
 * commissioning params, captured via kiln_package_capture_pico_half() at
 * save/clone time, plus this entry's package identity.
 *   pico_populated: 0 for a v2-migrated legacy slot that has not been
 *     re-saved since the v2->v3 upgrade (migrate_store_v2_to_v3() cannot
 *     retroactively know what the Pico held when that slot was originally
 *     saved) -- NOT the same as "captured with the Pico reporting nothing
 *     set", which is pico_populated=1 with every entries[].flags == 0.
 *     kiln_cfg_store_apply() must not claim a Pico half exists for a
 *     pico_populated==0 slot.
 *   pkg_schema/pkg_hash: section 3.1.1/3.1.3's package identity, valid only
 *     when pico_populated != 0 (both are 0 on a migrated-but-not-resaved
 *     slot, which is never mistaken for a real hash -- 0 is not attainable
 *     from kiln_package_compute_hash() at pkg_schema=KILN_PKG_SCHEMA_VERSION
 *     unless the CRC of an all-zero/empty input genuinely happens to be 0,
 *     an astronomically unlikely coincidence this module does not attempt to
 *     special-case; pico_populated is the actual, authoritative "was this
 *     ever computed" flag). */
typedef struct {
    uint8_t in_use;
    int32_t id;
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    uint16_t blob_len;
    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint8_t pico_populated;
    uint16_t pkg_schema;
    uint32_t pkg_hash;
    kiln_pkg_safety_t pico;
} kiln_cfg_entry_t;

typedef struct {
    uint8_t version;
    /* KILN_CFG_NO_ACTIVE_ID (-1) if nothing is currently applied-and-tracked
     * as the "starting point" config. */
    int32_t active_id;
    /* Monotonic; never reused, even across a delete. */
    int32_t next_id;
    kiln_cfg_entry_t entries[KILN_CFG_MAX_COUNT];
} kiln_cfg_store_blob_t;

/* Transient scratch-buffer allocator for one kiln_cfg_store_blob_t (~7.5
 * KiB). Every caller of this helper uses the result as a READ/decode/memcmp
 * scratch buffer -- a destination for hal_kv_get_blob()/
 * kiln_cfg_store_cfg_fs_load_raw(), or migration-chain scratch that is only
 * ever memcpy'd INTO the live s_store -- never handed directly as the SOURCE
 * pointer to a flash write (nvs_set_blob()/esp_flash_write()) or a LittleFS
 * write. Both nvs_save_store() (kiln_cfg_store.c) and
 * kiln_cfg_store_cfg_fs_save() (kiln_cfg_store_cfg_fs.c) copy from the live
 * s_store into their OWN small, separately allocated buffer before writing,
 * so this helper's PSRAM preference never touches a flash write's source
 * buffer.
 *
 * ESP-IDF's esp_flash_read()/esp_flash_write() (components/spi_flash/
 * include/esp_flash.h, IDF 6.0.2 as vendored under this repo's toolchain)
 * document a PSRAM-resident buffer as legal for BOTH directions on the
 * ESP32-S3: "Buffer is in external PSRAM which cannot be concurrently
 * accessed" is bounce-buffered automatically through a temporary internal
 * buffer, only failing (ESP_ERR_NO_MEM) if that small temporary internal
 * buffer itself cannot be allocated. So even a hypothetical future direct
 * flash use of one of these buffers would remain legal, not merely
 * "currently unused for that purpose."
 *
 * Motivation: two or more of these ~7.5 KiB buffers can be briefly live at
 * once during boot (nvs_load_store_with_cfg_fs()'s `resolved` plus
 * kiln_cfg_store_cfg_fs_resolve()'s own `file_blob`), which is most of a
 * one-time ~17 KB internal-DRAM min_free dip -- the board has ~7.9 MB PSRAM
 * free. Falls back to plain, internal malloc() if the PSRAM allocation
 * fails (e.g. before PSRAM init, or PSRAM exhausted) so behavior degrades
 * exactly like every existing NULL-check at these call sites already
 * handles. Freed with an ordinary free() either way --
 * heap_caps_malloc()'s memory is free()-compatible. */
static inline kiln_cfg_store_blob_t *kiln_cfg_store_blob_alloc(void)
{
    kiln_cfg_store_blob_t *p =
        heap_caps_malloc(sizeof(kiln_cfg_store_blob_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(sizeof(kiln_cfg_store_blob_t));
    }
    return p;
}

#ifdef __cplusplus
}
#endif

#endif // KILN_CFG_STORE_INTERNAL_H
