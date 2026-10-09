#ifndef PERSIST_SCRATCH_H
#define PERSIST_SCRATCH_H

/* PSRAM-preferring allocator for plain CPU-side transient scratch anywhere in
 * the app (persist/, http/ and control/ all use it), not only the persist layer.
 *
 * Bench finding 2026-10-04: a no-op POST /api/backup/import dropped
 * heap_internal min_free from 15727 B to 2595 B. CONFIG_SPIRAM_MALLOC_
 * ALWAYSINTERNAL=8192 sends every plain malloc() <= 8192 B to internal RAM,
 * and the kiln_configs commit path holds several such scratch structs at
 * once (kiln_cfg_import_scratch_t, export scratch, zones_cfg_t, the hash and
 * hex buffers, cfg_fs write scratch) on top of the 8192 B internal
 * http_async_job stack. Every user of this helper is plain CPU-side scratch:
 * never DMA, never touched with the flash cache disabled, never a task
 * stack. (A buffer later handed to a flash/NVS/VFS read or write is still
 * legal in PSRAM -- esp_flash bounce-buffers it, see
 * kiln_cfg_store_internal.h -- but nothing here depends on that for
 * correctness of the cache-disabled window itself.)
 *
 * Falls back to plain internal malloc() on NULL (PSRAM not up yet, or
 * exhausted), so existing NULL-check handling is unchanged. Release with an
 * ordinary free(). tools/check_persist_scratch_malloc_caps.ps1 keeps new
 * plain malloc()s of structs / >= 1 KiB out of the files it lists.
 *
 * A buffer that feeds a flash write may live in PSRAM (esp_flash
 * bounce-buffers it, see above), but write-source buffers such as
 * firing_stats_cfg_fs_save()'s deliberately stay MALLOC_CAP_INTERNAL to skip
 * that bounce copy; keep that choice unless there is a measured reason to
 * change it. */

#include <stddef.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

static inline void *persist_scratch_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(size);
    }
    return p;
}

#endif /* PERSIST_SCRATCH_H */
