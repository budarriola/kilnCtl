// kiln_cfg_store_internal -- the CURRENT-VERSION on-flash layout of the
// whole kiln config store, shared between kiln_cfg_store.c (which owns the
// NVS side, migration, and every public accessor) and
// kiln_cfg_store_cfg_fs.c (the `cfg` LittleFS read-through/dual-write bridge
// for it, docs/FILESYSTEM_USER_DATA_PLAN.md section 5's "kiln config slots"
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

#include "kiln_cfg_store.h" /* KILN_CFG_NAME_MAX_LEN, KILN_CFG_MAX_COUNT */
#include "zones_config_accessors.h" /* ZONES_CONFIG_BLOB_MAX_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

/* Bump whenever kiln_cfg_store_blob_t's on-flash layout changes -- see
 * kiln_cfg_store.c's fuller comment (the version-1/2 migration chain lives
 * there, not here). Version 2 is current. */
#define KILN_CFG_STORE_VERSION 2

/* One saved kiln config slot. blob/blob_len hold whatever
 * zones_config_export_blob() produced at save time -- an opaque byte string
 * to this module, sized against zones_http.h's ZONES_CONFIG_BLOB_MAX_SIZE
 * ceiling so this struct's layout never has to change just because
 * zone_cfg_t grew a field. */
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
     * as the "starting point" config. */
    int32_t active_id;
    /* Monotonic; never reused, even across a delete. */
    int32_t next_id;
    kiln_cfg_entry_t entries[KILN_CFG_MAX_COUNT];
} kiln_cfg_store_blob_t;

#ifdef __cplusplus
}
#endif

#endif // KILN_CFG_STORE_INTERNAL_H
