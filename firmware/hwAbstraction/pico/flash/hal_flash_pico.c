/* hal_flash_pico.c -- pico-sdk backend for interface/hal_flash.h.
 *
 * Phase 1b/3 ("adapt" / config_store_flash.c rebase target): implements the
 * Phase-0 interface against pico-sdk 2.x's hardware/flash.h + pico/flash.h,
 * grounded in the REAL SaftyFW consumer this header names throughout its own
 * comment block: config_store_flash.c's read_latest_or_default() (XIP-mapped
 * read), config_store_write_cb() (conditional flash_range_erase() +
 * flash_range_program()) and config_store_write()'s
 * flash_safe_execute(config_store_write_cb, &args, 1000u) call. Not wired
 * into any CMakeLists yet, and config_store_flash.c's own rebase onto this
 * header (Phase 3 item 2 -- keep the ARMED gate, seq/CRC log, and REFUSE
 * policy at that layer, unchanged here) has not happened -- see
 * firmware/hwAbstraction/test/compile_pico_backends.ps1 for the syntax-only
 * compile check that stands in for a real build target until then.
 *
 * No hal_flash_region_t per-instance state: like hal_uart_pico.c/
 * hal_time_pico.c, the real hardware this wraps is a single, whole-chip
 * NOR-flash device addressed by XIP_BASE + offset, not a handle-scoped
 * resource -- `r` is accepted (matching the header's signature) and ignored,
 * exactly as hal_uart_pico.c's `u` parameter is documented to be.
 *
 * INTERFACE MISMATCH notes (docs/HW_ABSTRACTION_PLAN.md asks these to be
 * reported, not silently papered over by widening hal_flash.h):
 *
 * 1. hal_flash_geometry()'s flash_total_size has no pico-sdk runtime query --
 *    PICO_FLASH_SIZE_BYTES is a BUILD-TIME macro from the board header
 *    (boards/pico.h, 2 MiB on this board per docs/SaftyFW/HARDWARE.md:298
 *    and flash_layout.h's own comment), not something read back from the
 *    W25Q16 part at runtime. This backend reports that compiled-in constant;
 *    a board built for the wrong PICO_BOARD would misreport this field, but
 *    that is true of flash_layout.h's own region macros today too -- not a
 *    new hazard this header introduces.
 * 2. hal_flash_write_safe_here() has no real pico-sdk predicate to call.
 *    Nothing in pico-sdk exposes "would flash_safe_execute() succeed right
 *    now" as a queryable boolean (flash_safe_execute_core_init() itself
 *    returns void and asserts internally rather than returning a status) --
 *    the ONLY way to actually find out is to call flash_safe_execute() and
 *    look at its return code, which hal_flash_safe_execute() already does.
 *    This mirrors hal_kv.h's own "reports the contract, does not enforce
 *    it"/advisory framing: this backend returns true unconditionally
 *    (matching hal_kv_esp.c's fallback shape when a real predicate does not
 *    exist) rather than fabricating a check pico-sdk cannot answer. Callers
 *    must still treat hal_flash_safe_execute()'s own HAL_NOT_READY /
 *    HAL_TIMEOUT return as authoritative, per the header's own doc comment
 *    on hal_flash_write_safe_here() ("hal_flash_safe_execute() itself
 *    performs its own check regardless").
 *
 * No mismatch for read/erase/program/geometry-constants/safe_execute beyond
 * the two notes above: config_store_flash.c's XIP_BASE + offset read,
 * conditional flash_range_erase()/flash_range_program() pair, and
 * flash_safe_execute(cb, arg, timeout_ms) call are exactly the shapes
 * hal_flash_read()/hal_flash_erase()/hal_flash_program()/
 * hal_flash_safe_execute() were written for -- see hal_flash.h's own
 * "config_store_flash.c operation -> hal_flash primitive mapping" comment.
 */
#include "hal_flash.h"

#include <string.h>

#include "pico/error.h"
#include "pico/flash.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h" /* XIP_BASE */

/* hal_flash.h's HAL_FLASH_ERASE_SIZE/HAL_FLASH_PROGRAM_SIZE are pinned to
 * pico-sdk's own FLASH_SECTOR_SIZE/FLASH_PAGE_SIZE literals -- a build-time
 * cross-check, same pattern config_store_flash.c already uses against
 * pico/error.h's enum, so a future pico-sdk change that ever redefines
 * either granularity fails the build instead of silently mis-sizing every
 * erase/program call this backend makes. */
typedef char hal_flash_erase_size_matches_pico_sdk
    [(HAL_FLASH_ERASE_SIZE == FLASH_SECTOR_SIZE) ? 1 : -1];
typedef char hal_flash_program_size_matches_pico_sdk
    [(HAL_FLASH_PROGRAM_SIZE == FLASH_PAGE_SIZE) ? 1 : -1];

/* Region concept -- see hal_flash.h's doc comment on hal_flash_region_t and
 * hal_flash_region_init(). Mirrors the host fake's implementation: an
 * initialized region is tagged with a magic value plus its bound
 * [base, base+size) range within the whole-chip device; every op below
 * requires this tag (HAL_NOT_READY otherwise) and treats its `offset`
 * argument as relative to the region's base. */
#define HAL_FLASH_PICO_REGION_MAGIC 0x464C5348u /* "FLSH" */

typedef struct {
    uint32_t magic;
    uint32_t base;
    uint32_t size;
} hal_flash_pico_region_data_t;

typedef char hal_flash_pico_region_fits_storage
    [(sizeof(hal_flash_pico_region_data_t) <= HAL_FLASH_REGION_STORAGE_BYTES) ? 1 : -1];

hal_status_t hal_flash_region_init(hal_flash_region_t *r, uint32_t base, uint32_t size) {
    if (r == NULL || size == 0u) {
        return HAL_INVALID_ARG;
    }
    if ((uint64_t)base + (uint64_t)size > (uint64_t)PICO_FLASH_SIZE_BYTES) {
        return HAL_INVALID_ARG;
    }
    hal_flash_pico_region_data_t d;
    d.magic = HAL_FLASH_PICO_REGION_MAGIC;
    d.base = base;
    d.size = size;
    memcpy(r->storage, &d, sizeof(d));
    return HAL_OK;
}

/* Returns true and fills *out if `r` is an initialized region; false for
 * NULL or an uninitialized/zeroed hal_flash_region_t. */
static bool region_get(const hal_flash_region_t *r, hal_flash_pico_region_data_t *out) {
    if (r == NULL) {
        return false;
    }
    hal_flash_pico_region_data_t d;
    memcpy(&d, r->storage, sizeof(d));
    if (d.magic != HAL_FLASH_PICO_REGION_MAGIC) {
        return false;
    }
    *out = d;
    return true;
}

/* Bounds-checks [offset, offset+len) against the region's own [0, size)
 * range and, on success, writes the absolute device offset to *abs_offset. */
static bool region_bounds_ok(const hal_flash_pico_region_data_t *d, uint32_t offset,
                              size_t len, uint32_t *abs_offset) {
    if ((uint64_t)offset + (uint64_t)len > (uint64_t)d->size) {
        return false;
    }
    *abs_offset = d->base + offset;
    return true;
}

hal_status_t hal_flash_geometry(hal_flash_region_t *r, hal_flash_geometry_t *out) {
    hal_flash_pico_region_data_t d;
    if (!region_get(r, &d)) {
        return HAL_NOT_READY;
    }
    if (out == NULL) {
        return HAL_INVALID_ARG;
    }
    /* See INTERFACE MISMATCH 1 above: PICO_FLASH_SIZE_BYTES is compiled in,
     * not queried from the part. Reports whole-device geometry, independent
     * of `r`'s own range -- see hal_flash.h's doc comment. */
    out->flash_total_size = PICO_FLASH_SIZE_BYTES;
    out->erase_size = HAL_FLASH_ERASE_SIZE;
    out->program_size = HAL_FLASH_PROGRAM_SIZE;
    return HAL_OK;
}

hal_status_t hal_flash_read(hal_flash_region_t *r, uint32_t offset,
                             void *buf, size_t len) {
    hal_flash_pico_region_data_t d;
    if (!region_get(r, &d)) {
        return HAL_NOT_READY;
    }
    if (buf == NULL) {
        return HAL_INVALID_ARG;
    }
    if (len == 0) {
        return HAL_OK;
    }
    uint32_t abs_offset;
    if (!region_bounds_ok(&d, offset, len, &abs_offset)) {
        return HAL_INVALID_ARG;
    }
    /* XIP-mapped read, no lockout -- see hal_flash.h's execution-context
     * contract: reads never race a concurrent erase/program from the SAME
     * core, and a concurrent erase/program from the OTHER core is exactly
     * the hazard flash_safe_execute() fences by halting that core first. */
    const uint8_t *src = (const uint8_t *)(XIP_BASE + abs_offset);
    memcpy(buf, src, len);
    return HAL_OK;
}

hal_status_t hal_flash_map(hal_flash_region_t *r, uint32_t offset, size_t len,
                            const void **out_ptr) {
    hal_flash_pico_region_data_t d;
    if (!region_get(r, &d)) {
        return HAL_NOT_READY;
    }
    if (out_ptr == NULL) {
        return HAL_INVALID_ARG;
    }
    uint32_t abs_offset;
    if (!region_bounds_ok(&d, offset, len, &abs_offset)) {
        return HAL_INVALID_ARG;
    }
    /* Same "no lockout" execution-context contract as hal_flash_read() above
     * -- see hal_flash.h's own doc comment on hal_flash_map(). */
    *out_ptr = (const void *)(XIP_BASE + abs_offset);
    return HAL_OK;
}

hal_status_t hal_flash_erase(hal_flash_region_t *r, uint32_t offset, size_t len) {
    hal_flash_pico_region_data_t d;
    if (!region_get(r, &d)) {
        return HAL_NOT_READY;
    }
    if (len == 0) {
        return HAL_INVALID_SIZE;
    }
    if ((offset % HAL_FLASH_ERASE_SIZE) != 0u || (len % HAL_FLASH_ERASE_SIZE) != 0u) {
        return HAL_INVALID_SIZE;
    }
    uint32_t abs_offset;
    if (!region_bounds_ok(&d, offset, len, &abs_offset)) {
        return HAL_INVALID_ARG;
    }
    /* Caller-bug contract, not runtime-checked here (see hal_flash.h's
     * "Binding rule for every backend of this interface"): this MUST be
     * called from inside a hal_flash_safe_execute() callback. */
    flash_range_erase(abs_offset, len);
    return HAL_OK;
}

hal_status_t hal_flash_program(hal_flash_region_t *r, uint32_t offset,
                                const void *buf, size_t len) {
    hal_flash_pico_region_data_t d;
    if (!region_get(r, &d)) {
        return HAL_NOT_READY;
    }
    if (buf == NULL) {
        return HAL_INVALID_ARG;
    }
    if (len == 0) {
        return HAL_INVALID_SIZE;
    }
    if ((offset % HAL_FLASH_PROGRAM_SIZE) != 0u || (len % HAL_FLASH_PROGRAM_SIZE) != 0u) {
        return HAL_INVALID_SIZE;
    }
    uint32_t abs_offset;
    if (!region_bounds_ok(&d, offset, len, &abs_offset)) {
        return HAL_INVALID_ARG;
    }
    /* Same caller-bug contract as hal_flash_erase() above. */
    flash_range_program(abs_offset, (const uint8_t *)buf, len);
    return HAL_OK;
}

hal_status_t hal_flash_safe_execute(hal_flash_safe_execute_cb_t cb, void *arg,
                                     uint32_t timeout_ms) {
    if (cb == NULL) {
        return HAL_INVALID_ARG;
    }
    /* pico-sdk's flash_safe_execute_func is void(*)(void*), identical to
     * hal_flash_safe_execute_cb_t by construction (see hal_flash.h's own
     * comment on that typedef) -- no adapter/trampoline needed. */
    int rc = flash_safe_execute(cb, arg, timeout_ms);
    /* Mapping per hal_flash.h's own doc comment on this function, matching
     * config_store_flash_rc_reason()'s three named failure modes. */
    switch (rc) {
        case PICO_OK:
            return HAL_OK;
        case PICO_ERROR_TIMEOUT:
            return HAL_TIMEOUT;
        case PICO_ERROR_NOT_PERMITTED:
            return HAL_NOT_READY;
        default:
            return HAL_IO;
    }
}

bool hal_flash_write_safe_here(void) {
    /* See INTERFACE MISMATCH 2 above: pico-sdk has no queryable predicate for
     * this. Advisory only, per hal_flash.h's own contract -- callers must
     * still treat hal_flash_safe_execute()'s real return code as
     * authoritative. */
    return true;
}
