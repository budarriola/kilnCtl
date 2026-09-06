/* hal_flash.h -- raw NOR-flash sector/page primitive. Pico-only, backs
 * config_store_flash.c (SaftyFW's versioned, CRC'd configuration record).
 * ESP is explicitly out of scope here: KilnFW's own raw-flash touchpoints
 * are esp_partition_* / esp_ota_* (OTA slot writes, partition table reads),
 * which docs/HW_ABSTRACTION_PLAN.md's hal_sysinfo section states plainly
 * are read-only there -- "OTA writes are out of scope". This header adds
 * nothing for that surface; it exists solely for the pico_flash_range_* /
 * XIP_BASE family SaftyFW uses. See docs/HW_ABSTRACTION_PLAN.md "hal_flash
 * -- pico backend for config_store_flash.c".
 *
 * WHAT THIS IS: the raw sector-erase / page-program / byte-read primitive,
 * plus the RP2040 multicore-XIP execution-context contract
 * (hal_flash_safe_execute() / hal_flash_write_safe_here()) that every write
 * must go through. Nothing more.
 *
 * WHAT THIS DELIBERATELY IS NOT: config_store's policy layer. The
 * seq-numbered/CRC'd 8-slot round-robin append-only log, the "keep the
 * highest seq among slots whose own CRC validates" scan, the hard ARMED
 * write interlock (config_store_decide_write() against
 * relay_owner_get_state()), and the format-version REFUSE-not-reinterpret
 * policy (config_store_unpack_ex()'s config_store_reject_info_t) all stay
 * in config_store.c/config_store_flash.c, layered on TOP of these
 * primitives -- exactly the same split hal_kv.h draws against config_store
 * (see hal_kv.h's own "why pico is excluded" comment): forcing that policy
 * down into this interface would bloat it with slot/seq/ARMED concepts no
 * other flash consumer needs. config_store_flash.c's eventual rebase onto
 * this header (docs/HW_ABSTRACTION_PLAN.md Phase 3 item 2) must keep the
 * ARMED gate, the seq/CRC log, and the REFUSE policy intact at that layer,
 * not bypassed or generalized into a namespace/key store here.
 *
 * config_store_flash.c operation -> hal_flash primitive mapping:
 *  - read_latest_or_default()'s XIP_BASE + SAFTYFW_CONFIG_STORE_FLASH_OFFSET
 *    pointer read over the whole sector -> hal_flash_read(), sized to
 *    SAFTYFW_CONFIG_STORE_FLASH_SIZE. (config_store_find_latest_ex() itself
 *    stays pure -- it just walks a caller-supplied buffer.)
 *  - config_store_write_cb()'s conditional flash_range_erase() over
 *    SAFTYFW_CONFIG_STORE_FLASH_OFFSET/_SIZE -> hal_flash_erase(), called
 *    only when config_store_next_write_needs_erase() says the sector must
 *    be reclaimed before the next slot wraps.
 *  - config_store_write_cb()'s flash_range_program() of one
 *    CONFIG_STORE_RECORD_LEN (512 B) slot -> hal_flash_program(). 512 is
 *    already a multiple of HAL_FLASH_PROGRAM_SIZE (256), matching
 *    flash_range_program()'s existing exact-multiple requirement.
 *  - config_store_write()'s flash_safe_execute(config_store_write_cb, &args,
 *    1000u) wrapping the erase+program pair -> hal_flash_safe_execute(),
 *    same callback/arg/timeout_ms shape. The three flash_safe_execute()
 *    failure modes config_store_flash_rc_reason() distinguishes today
 *    (TIMEOUT waiting for the other core's lockout handshake;
 *    NOT_PERMITTED, safe execution not possible at this point in boot;
 *    INSUFFICIENT_RESOURCES, an unrecognised/other failure) map onto
 *    HAL_TIMEOUT / HAL_NOT_READY / HAL_IO respectively -- see
 *    hal_flash_safe_execute()'s own comment below.
 *  - HAL_FLASH_ERASE_SIZE (4096) / HAL_FLASH_PROGRAM_SIZE (256) constants
 *    replace the bare hardware/flash.h FLASH_SECTOR_SIZE/FLASH_PAGE_SIZE
 *    literals config_store_flash.c and update_task.c reference today.
 *  - hal_flash_geometry() is new: config_store_flash.c currently gets
 *    offset/size from flash_layout.h's SAFTYFW_CONFIG_STORE_FLASH_OFFSET/
 *    _SIZE macros directly (correctly, per the plan's "flash_layout.h
 *    constants are shared with the bootloader from day one" -- those
 *    macros are NOT moved into this header). hal_flash_geometry() instead
 *    reports the whole-device bounds (total flash size / erase-sector
 *    size / program-page size) so a caller can validate an offset/len pair
 *    against real chip geometry rather than only against a compiled-in
 *    region macro. Region ownership (which offset belongs to config_store
 *    vs. the bootloader vs. an app slot) is out of scope here -- that is
 *    flash_layout.h's job, unchanged by this header.
 *
 * update_task.c's own flash_range_erase()/flash_range_program()/XIP_BASE
 * use (bootloader metadata log, app-slot OTA writes) is the SAME primitive
 * shape as config_store_flash.c's and is expressible through this header
 * too, but its rebase is not scheduled by the current plan pass -- noted
 * here only so a future pass does not have to re-derive that it fits.
 *
 * EXECUTION-CONTEXT CONTRACT (the RP2040 multicore-XIP hazard). Flash is
 * memory-mapped for execute-in-place; erasing or programming it while the
 * OTHER core (or an interrupt on either core) is still fetching code/data
 * from flash is undefined behaviour on this part. pico-sdk's answer is
 * flash_safe_execute(): it halts the other core and disables interrupts on
 * both for the duration of the callback, which is why config_store_flash.c
 * and update_task.c route every erase/program through it rather than a
 * bare save_and_disable_interrupts() (see update_task.c's header comment
 * for the fuller "why flash_safe_execute(), not save_and_disable_
 * interrupts()" reasoning -- that reasoning is unchanged by this header,
 * only relocated behind hal_flash_safe_execute()).
 *
 * Binding rule for every backend of this interface:
 *  - hal_flash_erase() and hal_flash_program() MUST be called only from
 *    inside a hal_flash_safe_execute() callback (or, on a backend with no
 *    multicore/XIP hazard at all -- the host fake -- from any context that
 *    is otherwise safe for that backend). Calling them directly, outside a
 *    safe_execute callback, on the pico backend is a caller bug this
 *    interface does not itself detect at runtime (matching hal_kv's
 *    "reports the contract, does not enforce it" stance on
 *    hal_kv_write_safe_here()).
 *  - hal_flash_read() carries NO such restriction: config_store_flash.c's
 *    XIP-pointer reads today run with no lockout, from ordinary task
 *    context, and that stays true here -- reads never race a concurrent
 *    erase/program from the SAME core (nothing else on this core would be
 *    issuing one), and a concurrent erase/program from the other core is
 *    exactly the hazard flash_safe_execute() already fences by halting
 *    that core first.
 *  - hal_flash_write_safe_here() mirrors hal_kv_write_safe_here(): it
 *    reports whether the CURRENT execution context is one from which
 *    hal_flash_safe_execute() may legally be invoked at all (e.g. false
 *    from inside an ISR, or before flash_safe_execute_core_init() has run
 *    on both cores -- the same "not permitted yet" condition
 *    config_store_flash_rc_reason() surfaces as NOT_PERMITTED today). It
 *    is advisory, like hal_kv's predicate: callers and lint may use it,
 *    hal_flash_safe_execute() itself still performs its own real check and
 *    fails with HAL_NOT_READY rather than trusting the caller to have
 *    asked first.
 */
#ifndef KILNCTL_HAL_FLASH_H
#define KILNCTL_HAL_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RP2040 (pico-sdk hardware/flash.h) erase-granularity and program-
 * granularity, in bytes. Every hal_flash_erase() offset/len and every
 * hal_flash_program() offset/len must be an exact multiple of the
 * respective constant -- flash_range_erase()/flash_range_program()'s own
 * documented requirement, unchanged here. */
#define HAL_FLASH_ERASE_SIZE   4096u
#define HAL_FLASH_PROGRAM_SIZE 256u

/* Reservation per docs/HW_ABSTRACTION_PLAN.md "Opaque handles": this
 * interface is stateless per call (offset-addressed, not handle-addressed
 * like hal_kv's open/close pairing), so no handle type carries backend
 * state today. Reserved anyway, sized like hal_uart's 64 B bus-scale
 * reservation, in case a future backend needs to cache e.g. a flash-safe-
 * execute readiness flag or a region-registration table without an ABI
 * break -- matches the opaque-storage pattern every other hal_*.h in this
 * directory uses, rather than this header being the one exception. */
#define HAL_FLASH_REGION_STORAGE_BYTES 64u

typedef struct {
    HAL_ALIGNAS8 uint8_t storage[HAL_FLASH_REGION_STORAGE_BYTES];
} hal_flash_region_t;

/* A hal_flash_region_t is opaque storage, not a plain-old-data struct a
 * caller may zero-init and pass: it MUST be produced by
 * hal_flash_region_init() before use. NULL, or a hal_flash_region_t that has
 * never been through hal_flash_region_init() (including one that is merely
 * zeroed), is NOT a legal argument to any function below -- every one of
 * them returns HAL_NOT_READY rather than silently treating it as "whole
 * device" or crashing. This mirrors hal_kv's stance on an unopened handle;
 * it replaces this header's earlier undocumented behavior of accepting NULL.
 *
 * Binds `r` to the byte range [base, base+size) of the whole-chip flash
 * device (offset 0 == XIP_BASE on pico), bounds-checked against real chip
 * geometry: `size` must be nonzero and base+size must not exceed the
 * device's total flash size, else HAL_INVALID_ARG. Every later
 * hal_flash_read/_erase/_program/_geometry() call against this `r` treats
 * its `offset` argument as relative to `base` (matching the existing
 * "offset 0 == start of flash" convention, now start-of-region rather than
 * start-of-device) and is bounds-checked against `size`, not just against
 * whole-device geometry.
 *
 * A region may be re-initialized (e.g. to rebind it to a different range);
 * doing so does not erase or otherwise touch flash contents. */
hal_status_t hal_flash_region_init(hal_flash_region_t *r, uint32_t base, uint32_t size);

/* Whole-device geometry, independent of any one caller's region macros
 * (flash_layout.h's SAFTYFW_CONFIG_STORE_FLASH_OFFSET/_SIZE and friends
 * stay exactly where they are -- this reports chip-wide bounds so a caller
 * can sanity-check an offset/len pair against real hardware, not replace
 * region ownership). */
typedef struct {
    uint32_t flash_total_size;   /* whole-chip flash size, bytes */
    uint32_t erase_size;         /* == HAL_FLASH_ERASE_SIZE on pico */
    uint32_t program_size;       /* == HAL_FLASH_PROGRAM_SIZE on pico */
} hal_flash_geometry_t;

hal_status_t hal_flash_geometry(hal_flash_region_t *r, hal_flash_geometry_t *out);

/* Byte-granular read of already-programmed flash, XIP-mapped on pico (no
 * offset/len alignment requirement, no safe_execute lockout -- see the
 * execution-context contract above). offset/len are relative to the start
 * of `r` (see hal_flash_region_init() above), matching how
 * config_store_flash.c and update_task.c already compute XIP_BASE +
 * <flash_layout.h offset>. `r` must have been through
 * hal_flash_region_init() -- HAL_NOT_READY otherwise. */
hal_status_t hal_flash_read(hal_flash_region_t *r, uint32_t offset,
                             void *buf, size_t len);

/* Erases [offset, offset+len) to all-0xFF. offset and len MUST each be an
 * exact multiple of HAL_FLASH_ERASE_SIZE -- HAL_INVALID_SIZE otherwise. `r`
 * must have been through hal_flash_region_init() -- HAL_NOT_READY otherwise.
 * MUST be called from inside a hal_flash_safe_execute() callback; see the
 * execution-context contract above. */
hal_status_t hal_flash_erase(hal_flash_region_t *r, uint32_t offset, size_t len);

/* Programs `len` bytes from `buf` starting at `offset` into flash. offset
 * and len MUST each be an exact multiple of HAL_FLASH_PROGRAM_SIZE --
 * HAL_INVALID_SIZE otherwise (config_store's 512 B record and
 * update_task's page_buf writes already satisfy this). `r` must have been
 * through hal_flash_region_init() -- HAL_NOT_READY otherwise. MUST be
 * called from inside a hal_flash_safe_execute() callback; see the
 * execution-context contract above.
 *
 * AND-programming semantics, pinned explicitly (real NOR flash can only
 * clear bits, 1->0, never set them without an intervening erase):
 * programming is NOT an overwrite. Programming into already-erased
 * (all-0xFF) flash reads back as `buf` exactly, indistinguishable from an
 * overwrite. Programming a byte that is NOT already erased is PERMITTED --
 * it is not an error this interface detects or refuses -- and the result is
 * `existing_byte & new_byte` bitwise, per byte: any bit `buf` asks to be 1
 * that the flash had already cleared to 0 stays 0. A caller that skips the
 * required erase gets back neither `buf` nor the pre-existing contents, but
 * their bitwise AND -- exactly what real hardware does, and exactly what
 * test/test_fake_flash.c:121 (programming 0xF0 over unerased 0x0F reads
 * back as 0x00) already asserts against the host fake. */
hal_status_t hal_flash_program(hal_flash_region_t *r, uint32_t offset,
                                const void *buf, size_t len);

/* Callback shape for hal_flash_safe_execute() -- deliberately identical to
 * pico-sdk's flash_safe_execute_func (void(*)(void*)), so an existing
 * config_store_write_cb()/update_metadata_write_cb()-shaped function needs
 * no signature change to be passed here. */
typedef void (*hal_flash_safe_execute_cb_t)(void *arg);

/* Runs `cb(arg)` with the multicore-XIP hazard fenced: pico backend wraps
 * pico-sdk's flash_safe_execute(), halting the other core and disabling
 * interrupts on both for the callback's duration; host backend (no
 * multicore/XIP hazard at all) calls cb(arg) directly. `timeout_ms` bounds
 * how long the pico backend waits for the other core's lockout handshake,
 * matching flash_safe_execute()'s own timeout_ms parameter (config_store
 * passes 1000u, update_task passes 1000u/2000u for its erase call).
 *
 * Return mapping (config_store_flash_rc_reason()'s three named failure
 * modes, so a caller can keep surfacing the same distinctions it does
 * today, now via hal_status_to_name() instead of a bespoke reason string):
 *   HAL_OK          - pico: PICO_OK. host: cb ran, unconditionally OK.
 *   HAL_TIMEOUT      - pico: PICO_ERROR_TIMEOUT (other core never answered
 *                       the lockout handshake within timeout_ms).
 *   HAL_NOT_READY    - pico: PICO_ERROR_NOT_PERMITTED (safe execution is
 *                       not possible right now -- e.g.
 *                       flash_safe_execute_core_init() never ran on the
 *                       other core; matches hal_status_t's existing
 *                       "!initialized, recoverable by init" meaning).
 *   HAL_IO           - pico: any other/unrecognised flash_safe_execute()
 *                       return (PICO_ERROR_INSUFFICIENT_RESOURCES and
 *                       anything not named above) -- catch-all, per
 *                       hal_status.h's own HAL_IO doc comment.
 */
hal_status_t hal_flash_safe_execute(hal_flash_safe_execute_cb_t cb, void *arg,
                                     uint32_t timeout_ms);

/* Advisory predicate mirroring hal_kv_write_safe_here(): reports whether
 * hal_flash_safe_execute() may legally be invoked from the CURRENT
 * execution context (false inside an ISR, or before both cores have
 * completed flash-safe-execute init). See the execution-context contract
 * above -- this reports the contract, it does not enforce it;
 * hal_flash_safe_execute() performs its own check regardless and returns
 * HAL_NOT_READY on failure whether or not a caller consulted this first. */
bool hal_flash_write_safe_here(void);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_FLASH_H */
