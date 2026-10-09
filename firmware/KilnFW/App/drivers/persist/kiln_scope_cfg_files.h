// Kiln-scope factory reset: the cfg_fs mirrors whose NVS side lives in the
// `kiln_nvs` partition.
//
// factory_reset.c's "kiln" scope erases the whole kiln_nvs partition. Every
// dual-written item whose NVS copy lives there also has a file under `cfg`,
// and that file wins the next boot's resolve (higher rev, or NVS empty) --
// so a mirror left behind silently undoes the reset. This module owns the
// list of those files and deletes them. Mirrors of the default `nvs`
// partition and of `profiles_nvs` are deliberately NOT here (the profiles
// scope deletes its own; wifi has none).
//
// When adding a dual-written item whose NVS partition is kiln_nvs, add its
// path here; tests/test_zone_normals_cfg_fs.c checks every listed path is
// deleted.
#ifndef KILN_SCOPE_CFG_FILES_H
#define KILN_SCOPE_CFG_FILES_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The relative cfg_fs paths this module deletes (static storage). */
const char *const *kiln_scope_cfg_files_list(size_t *out_count);

/* Best-effort cfg_fs_delete() of every listed file. Absent files and an
 * unmounted cfg_fs are normal (not errors). Every file is attempted even
 * after a failure; returns the FIRST hard error (a stale file that survives
 * must fail the reset), ESP_OK otherwise. *out_deleted (optional) receives
 * the number of files actually removed. */
esp_err_t kiln_scope_cfg_files_delete(int *out_deleted);

#ifdef __cplusplus
}
#endif

#endif // KILN_SCOPE_CFG_FILES_H
