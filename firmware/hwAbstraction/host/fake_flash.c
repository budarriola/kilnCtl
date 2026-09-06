/* fake_flash.c -- host fake backend for hal_flash.h. See fake_flash.h. */
#include "fake_flash.h"

#include <string.h>

static uint8_t  s_image[FAKE_FLASH_MAX_SIZE_BYTES];
static size_t   s_total_size = FAKE_FLASH_DEFAULT_SIZE_BYTES;
static uint32_t s_erase_counts[FAKE_FLASH_MAX_SECTORS];
static bool     s_write_safe_here = true;

static bool     s_next_status_armed[4];
static hal_status_t s_next_status[4];

static bool          s_power_loss_armed = false;
static fake_flash_op_t s_power_loss_op = FAKE_FLASH_OP_READ;

static void reset_common(void) {
    memset(s_erase_counts, 0, sizeof(s_erase_counts));
    memset(s_next_status_armed, 0, sizeof(s_next_status_armed));
    memset(s_next_status, 0, sizeof(s_next_status));
    s_power_loss_armed = false;
    s_power_loss_op = FAKE_FLASH_OP_READ;
    s_write_safe_here = true;
}

void fake_flash_reset_all(void) {
    s_total_size = FAKE_FLASH_DEFAULT_SIZE_BYTES;
    memset(s_image, 0xFF, sizeof(s_image));
    reset_common();
}

bool fake_flash_reset_all_sized(size_t total_size_bytes) {
    if (total_size_bytes == 0 || total_size_bytes > FAKE_FLASH_MAX_SIZE_BYTES) {
        return false;
    }
    if ((total_size_bytes % HAL_FLASH_ERASE_SIZE) != 0u) {
        return false;
    }
    s_total_size = total_size_bytes;
    memset(s_image, 0xFF, sizeof(s_image));
    reset_common();
    return true;
}

size_t fake_flash_get_total_size(void) {
    return s_total_size;
}

uint32_t fake_flash_get_erase_count(uint32_t sector_index) {
    if (sector_index >= FAKE_FLASH_MAX_SECTORS) {
        return 0;
    }
    return s_erase_counts[sector_index];
}

void fake_flash_script_next_op_status(fake_flash_op_t op, hal_status_t status) {
    if ((size_t)op >= (sizeof(s_next_status_armed) / sizeof(s_next_status_armed[0]))) {
        return;
    }
    if (status == HAL_OK) {
        s_next_status_armed[op] = false;
        return;
    }
    s_next_status_armed[op] = true;
    s_next_status[op] = status;
}

void fake_flash_simulate_power_loss_during(fake_flash_op_t op) {
    s_power_loss_armed = true;
    s_power_loss_op = op;
}

void fake_flash_set_write_safe_here(bool safe) {
    s_write_safe_here = safe;
}

/* Consumes (one-shot) an armed failure status for `op`, if any. Returns true
 * and sets *out if one was armed. */
static bool take_scripted_status(fake_flash_op_t op, hal_status_t *out) {
    if ((size_t)op >= (sizeof(s_next_status_armed) / sizeof(s_next_status_armed[0]))) {
        return false;
    }
    if (!s_next_status_armed[op]) {
        return false;
    }
    s_next_status_armed[op] = false;
    *out = s_next_status[op];
    return true;
}

/* Consumes (one-shot, regardless of which op actually reached this call --
 * see fake_flash.h's own comment) an armed power-loss request matching
 * `op`. Returns true if it applied to this call. */
static bool take_power_loss(fake_flash_op_t op) {
    if (!s_power_loss_armed) {
        return false;
    }
    bool matches = (s_power_loss_op == op);
    s_power_loss_armed = false;
    return matches;
}

static bool bounds_ok(uint32_t offset, size_t len) {
    return (uint64_t)offset + (uint64_t)len <= (uint64_t)s_total_size;
}

/* --- hal_flash.h implementation --- */

hal_status_t hal_flash_geometry(hal_flash_region_t *r, hal_flash_geometry_t *out) {
    (void)r;
    if (out == NULL) {
        return HAL_INVALID_ARG;
    }
    hal_status_t scripted;
    if (take_scripted_status(FAKE_FLASH_OP_READ, &scripted)) {
        /* No dedicated "geometry" op in fake_flash_op_t (it is not a data
         * primitive) -- geometry queries share READ's injection slot since
         * both are pure, side-effect-free queries. */
        return scripted;
    }
    out->flash_total_size = (uint32_t)s_total_size;
    out->erase_size = HAL_FLASH_ERASE_SIZE;
    out->program_size = HAL_FLASH_PROGRAM_SIZE;
    return HAL_OK;
}

hal_status_t hal_flash_read(hal_flash_region_t *r, uint32_t offset,
                             void *buf, size_t len) {
    (void)r;
    hal_status_t scripted;
    if (take_scripted_status(FAKE_FLASH_OP_READ, &scripted)) {
        return scripted;
    }
    if (take_power_loss(FAKE_FLASH_OP_READ)) {
        return HAL_IO;
    }
    if (buf == NULL) {
        return HAL_INVALID_ARG;
    }
    if (len == 0) {
        return HAL_OK;
    }
    if (!bounds_ok(offset, len)) {
        return HAL_INVALID_ARG;
    }
    memcpy(buf, &s_image[offset], len);
    return HAL_OK;
}

hal_status_t hal_flash_erase(hal_flash_region_t *r, uint32_t offset, size_t len) {
    (void)r;
    hal_status_t scripted;
    if (take_scripted_status(FAKE_FLASH_OP_ERASE, &scripted)) {
        return scripted;
    }
    if (len == 0) {
        return HAL_INVALID_SIZE;
    }
    if ((offset % HAL_FLASH_ERASE_SIZE) != 0u || (len % HAL_FLASH_ERASE_SIZE) != 0u) {
        return HAL_INVALID_SIZE;
    }
    if (!bounds_ok(offset, len)) {
        return HAL_INVALID_ARG;
    }

    bool power_loss = take_power_loss(FAKE_FLASH_OP_ERASE);
    size_t apply_len = len;
    if (power_loss) {
        /* Partial-effect model for erase, symmetric with program's: only the
         * first sector actually gets erased before the simulated crash. */
        apply_len = HAL_FLASH_ERASE_SIZE;
    }

    memset(&s_image[offset], 0xFF, apply_len);
    for (uint32_t o = offset; o < offset + apply_len; o += HAL_FLASH_ERASE_SIZE) {
        uint32_t sector = o / HAL_FLASH_ERASE_SIZE;
        if (sector < FAKE_FLASH_MAX_SECTORS) {
            s_erase_counts[sector]++;
        }
    }

    return power_loss ? HAL_IO : HAL_OK;
}

hal_status_t hal_flash_program(hal_flash_region_t *r, uint32_t offset,
                                const void *buf, size_t len) {
    (void)r;
    hal_status_t scripted;
    if (take_scripted_status(FAKE_FLASH_OP_PROGRAM, &scripted)) {
        return scripted;
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
    if (!bounds_ok(offset, len)) {
        return HAL_INVALID_ARG;
    }

    bool power_loss = take_power_loss(FAKE_FLASH_OP_PROGRAM);
    size_t apply_len = len;
    if (power_loss) {
        /* Half-written page: only the first program-granularity page lands
         * before the simulated crash -- see fake_flash.h's power-loss
         * comment. At least one page (never zero) if len >= one page, which
         * the alignment check above already guarantees. */
        apply_len = HAL_FLASH_PROGRAM_SIZE;
    }

    /* AND semantics: models real NOR flash, which can only clear bits
     * without an intervening erase -- see fake_flash.h's own "Program
     * semantics" comment for why this, not overwrite, is the accurate
     * model. Programming onto already-erased (0xFF) flash is
     * indistinguishable from an overwrite; programming onto NOT-erased
     * flash silently ANDs in the new bits, exactly like real hardware, so a
     * test reading back the result can detect the missing erase. */
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < apply_len; i++) {
        s_image[offset + i] &= src[i];
    }

    return power_loss ? HAL_IO : HAL_OK;
}

hal_status_t hal_flash_safe_execute(hal_flash_safe_execute_cb_t cb, void *arg,
                                     uint32_t timeout_ms) {
    (void)timeout_ms;
    hal_status_t scripted;
    if (take_scripted_status(FAKE_FLASH_OP_SAFE_EXECUTE, &scripted)) {
        return scripted;
    }
    if (take_power_loss(FAKE_FLASH_OP_SAFE_EXECUTE)) {
        return HAL_IO;
    }
    if (cb == NULL) {
        return HAL_INVALID_ARG;
    }
    /* Host backend has no multicore/XIP hazard at all -- see hal_flash.h's
     * own doc comment on this function: "host: calls cb(arg) directly." Any
     * erase/program failure the callback triggers is surfaced through ITS
     * own return path (config_store_write_cb()-shaped callbacks are void and
     * do not propagate a status themselves; the effect is observable via the
     * image/erase-count state this fake exposes), not through this
     * function's return value. */
    cb(arg);
    return HAL_OK;
}

bool hal_flash_write_safe_here(void) {
    return s_write_safe_here;
}
