/* fake_flash.h -- host fake backend for hal_flash.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION_PLAN.md "Host fakes (Phase 2 specs)" -- fake_flash
 * (must): sector model with erase-before-program enforcement and
 * safe_execute timeout injection. This unlocks config_store_flash.c's
 * seq/CRC/ARMED/format-REFUSE logic under host test for the first time once
 * that module rebases onto hal_flash.h (Phase 3 item 2).
 *
 * Storage model: a single in-memory image, `FAKE_FLASH_DEFAULT_SIZE_BYTES`
 * by default ("a few sectors" per the plan), reconfigurable up to
 * `FAKE_FLASH_MAX_SIZE_BYTES` via fake_flash_reset_all_sized(). Every byte
 * starts at the real NOR-flash erased value, 0xFF, matching
 * hal_flash_erase()'s documented "[offset, offset+len) to all-0xFF"
 * contract.
 *
 * Program semantics: hal_flash.h does NOT pin erase-before-program
 * enforcement at the interface level (it states the caller-bug contract for
 * safe_execute context, but says nothing about what a program-over-
 * unerased-flash call observes). Real NOR flash can only clear bits (1->0),
 * never set them (0->1), without an intervening erase -- pico-sdk's
 * flash_range_program() does not itself detect or refuse this; garbage
 * silently results. Per this header's own instruction ("if it does not pin
 * it, model AND semantics and say so"): fake_flash_program() ANDs the new
 * bytes into the existing image rather than overwriting it, so programming a
 * non-erased byte is DETECTABLE by a test that reads back the result and
 * finds bits that did not change to the intended value -- exactly what would
 * happen on real hardware, rather than either silently succeeding (masking
 * the bug class) or inventing an error hal_flash.h never promised.
 *
 * Wear tracking: fake_flash_get_erase_count() reports, per HAL_FLASH_ERASE_
 * SIZE-granularity sector, how many times hal_flash_erase() has touched it
 * (partial overlap counts once per covered sector) -- for the round-robin
 * wear-leveling assertions config_store_flash.c's eventual host tests need.
 *
 * Failure injection: fake_flash_script_next_op_status() arms a one-shot
 * override for a specific primitive (read/erase/program/safe_execute) so a
 * caller of hal_flash_safe_execute() can be shown a TIMEOUT/NOT_READY/IO
 * result without a real RP2040 to provoke it.
 *
 * Power-loss modeling: fake_flash_simulate_power_loss_during(FAKE_FLASH_OP_
 * PROGRAM) arms the state a real power failure mid-program leaves behind --
 * the NEXT hal_flash_program() call writes only the first program-page of
 * the requested range (still AND-combined, per the semantics above) and then
 * reports HAL_IO, exactly modeling flash_safe_execute() being interrupted
 * partway through pico-sdk's page-at-a-time write loop. The image is left
 * with that partial write applied -- a caller that does not defend against a
 * torn write (config_store's seq/CRC design exists precisely to detect this)
 * will read back a record whose CRC does not validate.
 */
#ifndef KILNCTL_FAKE_FLASH_H
#define KILNCTL_FAKE_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "A few sectors" by default, reconfigurable (fake_flash_reset_all_sized())
 * up to this compile-time cap. Both are exact multiples of
 * HAL_FLASH_ERASE_SIZE, matching real NOR-flash erase granularity. */
#define FAKE_FLASH_DEFAULT_SIZE_BYTES (4u * HAL_FLASH_ERASE_SIZE)
/* Bumped 2026-09-06 (Phase 3 item 2, config_store_flash.c's rebase) from
 * 16 sectors (64 KiB) to 512 sectors (2 MiB, matching the real RP2040's
 * PICO_FLASH_SIZE_BYTES): config_store_flash.c binds its hal_flash_region_t
 * at flash_layout.h's real SAFTYFW_CONFIG_STORE_FLASH_OFFSET (0x1B1000) so
 * the same production source runs unmodified on both backends -- host tests
 * need a fake image large enough for hal_flash_region_init() to accept that
 * offset, not just "a few sectors" at the bottom of the image. */
#define FAKE_FLASH_MAX_SIZE_BYTES     (512u * HAL_FLASH_ERASE_SIZE)
#define FAKE_FLASH_MAX_SECTORS        (FAKE_FLASH_MAX_SIZE_BYTES / HAL_FLASH_ERASE_SIZE)

typedef enum {
    FAKE_FLASH_OP_READ = 0,
    FAKE_FLASH_OP_ERASE,
    FAKE_FLASH_OP_PROGRAM,
    FAKE_FLASH_OP_SAFE_EXECUTE,
} fake_flash_op_t;

/* Resets the image to FAKE_FLASH_DEFAULT_SIZE_BYTES, all 0xFF, clears every
 * erase count, every injected fault, and the armed power-loss state. Call
 * between test cases. */
void fake_flash_reset_all(void);

/* Same as fake_flash_reset_all() but with an explicit total size (bytes),
 * which must be a nonzero multiple of HAL_FLASH_ERASE_SIZE and
 * <= FAKE_FLASH_MAX_SIZE_BYTES. Returns false (and leaves the image at its
 * previous size, NOT reset) on an invalid size. */
bool fake_flash_reset_all_sized(size_t total_size_bytes);

/* Current configured total size, in bytes. */
size_t fake_flash_get_total_size(void);

/* Number of times hal_flash_erase() has covered the sector at
 * `sector_index` (0-based, HAL_FLASH_ERASE_SIZE-granularity). Returns 0 for
 * an out-of-range index rather than asserting -- a test computing an index
 * from a live offset should not crash the harness on an off-by-one. */
uint32_t fake_flash_get_erase_count(uint32_t sector_index);

/* Forces the NEXT call to the named primitive to return `status` instead of
 * performing the operation, then reverts to normal behavior. Does not affect
 * any other primitive. Passing HAL_OK is equivalent to disarming it early. */
void fake_flash_script_next_op_status(fake_flash_op_t op, hal_status_t status);

/* Arms a one-shot simulated power loss for the given operation. Only
 * FAKE_FLASH_OP_PROGRAM has a real partial-effect model (see the header
 * comment above); arming any other op is equivalent to
 * fake_flash_script_next_op_status(op, HAL_IO) -- provided for symmetry so a
 * caller need not special-case which op it is testing. Reverts to normal
 * behavior after the next matching call, whether or not that call actually
 * reached the armed op (i.e. it is consumed by the next hal_flash_* call
 * regardless of which primitive is invoked, matching fake_kv's one-shot
 * scripting shape). */
void fake_flash_simulate_power_loss_during(fake_flash_op_t op);

/* Test-controllable override for hal_flash_write_safe_here() -- see
 * hal_flash.h's execution-context contract. Defaults to true after
 * fake_flash_reset_all() (the host models no ISR/core-init concept of its
 * own, same stance fake_kv takes for hal_kv_write_safe_here()). */
void fake_flash_set_write_safe_here(bool safe);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_FLASH_H */
