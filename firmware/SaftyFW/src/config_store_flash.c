// config_store_flash.c -- the flash I/O and ARMED-check glue for
// config_store.h's pure record logic. Rebased (docs/HW_ABSTRACTION.md
// Phase 3 item 2, 2026-09-06) onto hal_flash.h: real flash access now goes
// through hal_flash_read()/hal_flash_erase()/hal_flash_program()/
// hal_flash_safe_execute() instead of pico-sdk's XIP_BASE pointer read /
// flash_range_erase() / flash_range_program() / flash_safe_execute()
// directly. This is what makes this file host-testable for the first time
// (see test/test_config_store_flash.c) -- the pico backend
// (hal_flash_pico.c) still wraps the exact same pico-sdk calls this file
// used to make itself; the host backend (fake_flash.c) models an in-memory
// sector image instead. No change to what is preserved: the ARMED gate
// (config_store_decide_write() against relay_owner_get_state()), the
// seq/CRC round-robin log, and the format-version REFUSE policy are all
// still expressed at this layer, untouched by the rebase -- only the flash
// primitive calls underneath moved.
//
// Region binding: this file's two hal_flash_region_t's are bound once,
// lazily, to the config store's A and B sectors -- base ==
// flash_layout.h's real SAFTYFW_CONFIG_STORE_FLASH_OFFSET /
// SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B, size == SAFTYFW_CONFIG_STORE_FLASH_
// SIZE for each (see ensure_region() below) -- so every offset used
// elsewhere in this file against one of them is 0-based within THAT sector,
// not a whole-device offset. Binding at the REAL flash_layout.h offsets
// (rather than 0 on host, matching pico) is deliberate: it lets this file be
// identical on both backends, at the cost of the host fake needing to be
// sized to accept those real offsets (see firmware/hwAbstraction/host/
// fake_flash.h's FAKE_FLASH_MAX_SIZE_BYTES comment, bumped for exactly this).
//
// --- A/B sectors (flash_endurance_review_2026-09-07.md R2) -----------------
//
// The ORIGINAL single-sector design's failure mode: config_store_write()'s
// 8th write into a sector must erase that whole sector before it can program
// slot 0 again (config_store_next_write_needs_erase()) -- and between that
// erase and the next successful program, the sector held ZERO valid copies
// of the safety config. A power cut there lost TC type, abs_max_temp_c, CT
// cal outright. Nothing about the CRC/seq scan itself was ever unsafe; the
// gap was structural -- there was only ever one place to look.
//
// The fix adds a second sector (B) and never lets there be a moment with no
// valid copy anywhere:
//
//   - WRITE: config_store_plan_write() (config_store.c, pure) decides, from
//     the currently-cached (sector, slot), whether the next write fits in
//     the SAME sector (7 out of 8 writes -- no erase, byte-identical to the
//     old behaviour) or must SWITCH to the other sector (the 8th write --
//     erase that other sector, then program its slot 0).
//   - ATOMICITY: a switch never touches the sector holding the current
//     record. config_store_write_cb() below is handed only ONE
//     hal_flash_region_t (`args.region`, the target sector) -- it has no
//     way to erase or reprogram the other one even if it wanted to. So at
//     every instant during the erase-then-program pair -- including a crash
//     mid-erase, mid-program, or anywhere between the two -- the sector that
//     was NOT targeted still holds its full, untouched, CRC-valid record.
//     There is no separate "which sector is active" pointer stored anywhere
//     to tear: see the next bullet.
//   - ARBITRATION: read_latest_or_default() reads BOTH sectors, every time,
//     and config_store_find_latest_multi_ex() (config_store.c, pure) keeps
//     the single highest-`seq`, CRC-valid record across all 16 slots. A
//     torn/interrupted write in the target sector simply fails its own CRC
//     check and is skipped, exactly like any other corrupt slot always was
//     -- the reader then finds the OTHER sector's still-valid, lower-seq
//     record instead, with no special-casing needed for "was this a
//     switch-in-progress". The corrupt/superseded sector is not logged as a
//     distinct case (it looks, and is treated, exactly like any other
//     partially-written slot config_store_unpack_ex() already understands).
//   - MIGRATION: sector A keeps its original offset (SAFTYFW_CONFIG_STORE_
//     FLASH_OFFSET, unmoved) and its original 8-slot append-only format
//     (unchanged) -- a board already running the old single-sector firmware
//     IS running "A/B with B still erased/empty" already, with no data to
//     move and no migration step to run. The first boot of this firmware on
//     such a board reads sector A's existing highest-seq record exactly as
//     before (config_store_find_latest_multi_ex() finds nothing valid in the
//     still-blank sector B, same as an ordinary fresh sector) and proceeds
//     unaffected; TC type and abs_max_temp_c are not defaulted or lost.
//   - WEAR: each sector now absorbs erases only half as often (an erase
//     happens once every 8 writes, alternating sectors), which is the free
//     side effect the endurance review named -- not the reason this exists.
//
// Caches the current record in a static, so config_store_get_tc_type()/
// config_store_is_calibration_missing() are cheap, lock-free reads for
// hot-path callers (main.c at boot, and eventually thermo_task) rather than
// re-scanning flash every call. config_store_boot_load() must run once,
// early in main()'s boot sequence, before anything reads the cache.
#include "config_store.h"

#include <stdio.h>
#include <string.h>

#include "hal_flash.h"

#include "flash_layout.h" // bootloader/ -- SAFTYFW_CONFIG_STORE_FLASH_OFFSET/_SIZE
#include "max31856.h"      // MAX31856_TC_TYPE_K -- asserted to match CONFIG_STORE_DEFAULT_TC_TYPE
#include "tasks/console_uart.h" // console_uart_puts() -- the boot-time "record REJECTED" log line
#include "tasks/relay_owner.h" // relay_owner_get_state() -- the ARMED check

// Compile-time cross-check: config_store.h's CONFIG_STORE_DEFAULT_TC_TYPE is
// duplicated as a literal rather than #including max31856.h (see
// config_store.h's comment on why), so this catches the two ever drifting
// apart instead of silently defaulting to the wrong type.
typedef char config_store_default_tc_type_matches_max31856
    [(CONFIG_STORE_DEFAULT_TC_TYPE == MAX31856_TC_TYPE_K) ? 1 : -1];

// Same cross-check pattern for the other tc_type literal config_store.h
// duplicates: CONFIG_STORE_TC_TYPE_MAX_REAL must track MAX31856_TC_TYPE_T
// (the highest real, linearized thermocouple type) or config_store_unpack()'s
// voltage-mode clamp would silently protect the wrong boundary.
typedef char config_store_tc_type_max_real_matches_max31856
    [(CONFIG_STORE_TC_TYPE_MAX_REAL == MAX31856_TC_TYPE_T) ? 1 : -1];

// config_store.h's CONFIG_STORE_FLASH_RC_* literals used to be cross-checked
// here at compile time against pico/error.h's `enum pico_error_codes`
// directly. That check was deleted, not moved: no caller feeds a raw pico rc
// to config_store_flash_rc_reason() any more, so numeric agreement with the
// SDK enum is no longer a requirement anywhere. This file only sees the
// hal_status_t hal_flash_safe_execute() returns, translated to a
// CONFIG_STORE_FLASH_RC_* value by hal_status_to_config_store_flash_rc()
// below so config_store_flash_rc_reason()'s existing three named strings
// keep working unchanged.
static hal_status_t hal_status_to_config_store_flash_rc(hal_status_t status, int *out_rc)
{
    switch (status) {
        case HAL_OK:
            *out_rc = CONFIG_STORE_FLASH_RC_OK;
            break;
        case HAL_TIMEOUT:
            *out_rc = CONFIG_STORE_FLASH_RC_TIMEOUT;
            break;
        case HAL_NOT_READY:
            *out_rc = CONFIG_STORE_FLASH_RC_NOT_PERMITTED;
            break;
        default:
            // Catch-all, matching hal_flash_pico.c's own HAL_IO default case
            // ("PICO_ERROR_INSUFFICIENT_RESOURCES and anything not named
            // above") -- symmetric with that mapping so the round trip
            // through hal_status_t loses no distinction config_store_flash_
            // rc_reason() itself makes.
            *out_rc = CONFIG_STORE_FLASH_RC_INSUFFICIENT_RESOURCES;
            break;
    }
    return status;
}

// Lazily bound the first time either read_latest_or_default() or
// config_store_write() needs it -- see the file header comment above for
// what this region covers and why the offset is the same on both backends.
//
// A/B sectors (flash_endurance_review_2026-09-07.md R2): s_region is sector
// A (the legacy single sector, unmoved -- SAFTYFW_CONFIG_STORE_FLASH_OFFSET),
// s_region_b is sector B (flash_layout.h's new
// SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B, immediately following it in the same
// 64K reserved region). Indexed as s_regions[0]/[1] wherever code needs to
// pick "the sector config_store_plan_write()/find_latest_multi_ex() named",
// so the sector-index values those pure functions return map directly to an
// array index here with no translation.
static hal_flash_region_t s_region;
static hal_flash_region_t s_region_b;
static hal_flash_region_t *s_regions[SAFTYFW_CONFIG_STORE_NUM_SECTORS];
static bool s_region_ready = false;

static bool ensure_region(void)
{
    if (s_region_ready) {
        return true;
    }
    if (hal_flash_region_init(&s_region, SAFTYFW_CONFIG_STORE_FLASH_OFFSET,
                               SAFTYFW_CONFIG_STORE_FLASH_SIZE) != HAL_OK) {
        return false;
    }
    if (hal_flash_region_init(&s_region_b, SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B,
                               SAFTYFW_CONFIG_STORE_FLASH_SIZE) != HAL_OK) {
        return false;
    }
    s_regions[0] = &s_region;
    s_regions[1] = &s_region_b;
    s_region_ready = true;
    return true;
}

static config_store_record_t s_cached_record;
static size_t s_cached_slot = CONFIG_STORE_NO_SLOT;
// Which of s_regions[0]/[1] (sector A/B) s_cached_slot indexes into. Only
// meaningful once s_cached_slot != CONFIG_STORE_NO_SLOT; config_store_plan_
// write() ignores it entirely in the NO_SLOT case (starts fresh at sector 0),
// same "don't trust a stale index against a sentinel" discipline as
// s_cached_slot's own callers already follow.
static size_t s_cached_sector = 0;
static bool s_loaded = false;
// True iff the sector held a structurally-intact (magic/CRC/format_version
// all valid) record that config_params_validate_ranges() refused, and no
// OTHER slot in the sector was good -- config_store.h's "Load-time rejection
// diagnostics" case 2, distinct from an ordinary never-committed board (case
// 1, this stays false). See config_store_is_config_rejected()'s own comment.
static bool s_load_rejected = false;

// Module-scope, not a local: SAFTYFW_CONFIG_STORE_FLASH_SIZE is 4096 bytes
// (one erase sector, bootloader/flash_layout.h), and read_latest_or_default()'s
// only caller, config_store_boot_load(), runs from main.c's boot sequence
// step 4 -- before vTaskStartScheduler() -- on core0's pre-scheduler boot
// stack. That stack is pico-sdk's default PICO_STACK_SIZE (0x800 == 2048
// bytes; not overridden anywhere in this project's CMakeLists.txt), so a
// 4096-byte local array here would be a guaranteed overflow of the ENTIRE
// boot stack on its own, independent of whatever else that stack frame
// holds -- not merely tight against a margin baseline. config_store_boot_load()
// is documented (config_store_flash.c's own header comment, "must run once,
// pre-scheduler") to run exactly once before any task exists, so there is no
// concurrent caller to serialize against; a single static buffer is safe.
static uint8_t s_read_sector[SAFTYFW_CONFIG_STORE_NUM_SECTORS][SAFTYFW_CONFIG_STORE_FLASH_SIZE];

// Reads BOTH sectors and hands them to config_store_find_latest_multi_ex(),
// which is the actual arbiter (config_store.h's "A/B sector arbitration"
// comment) -- this function is now just the flash-I/O half of that: no
// erase/switch state lives here, only whatever the two sectors' own bytes
// say right now. `*out_sector` is filled with which sector the winning
// record came from (0 or 1), untouched on a CONFIG_STORE_NO_SLOT return.
static size_t read_latest_or_default(config_store_record_t *out_rec,
                                      size_t *out_sector,
                                      config_store_reject_info_t *out_reject)
{
    // ensure_region()/hal_flash_read() failing here is treated the same as
    // an unreadable/blank sector always was: "a missing part must not abort
    // boot" (max31856_configure()'s own doc comment) applies to
    // configuration exactly as much as to a missing sensor -- fall back to
    // config_store_default() rather than propagate the failure. A read
    // failure on EITHER sector is treated the same way -- there is no
    // partial-arbitration path that trusts one sector's bytes while
    // distrusting the other's read status.
    if (!ensure_region() ||
        hal_flash_read(&s_region, 0, s_read_sector[0], sizeof(s_read_sector[0])) != HAL_OK ||
        hal_flash_read(&s_region_b, 0, s_read_sector[1], sizeof(s_read_sector[1])) != HAL_OK) {
        config_store_default(out_rec);
        return CONFIG_STORE_NO_SLOT;
    }
    const uint8_t *sectors[SAFTYFW_CONFIG_STORE_NUM_SECTORS] = {s_read_sector[0], s_read_sector[1]};
    size_t sector_index = 0;
    size_t latest = config_store_find_latest_multi_ex(sectors, &sector_index, out_rec, out_reject);
    if (latest == CONFIG_STORE_NO_SLOT) {
        config_store_default(out_rec);
    } else {
        *out_sector = sector_index;
    }
    return latest;
}

// Reads the config store once, at boot, before the scheduler/other tasks
// start (same "call this before anything reads the cache" contract as
// spi_owner_init()/relay_owner_start() have in main.c's boot sequence). On a
// blank or corrupt sector this leaves the cache holding config_store_default()
// -- "a missing part must not abort boot" (max31856_configure()'s own doc
// comment) applies to configuration exactly as much as to a missing sensor.
//
// 2026-08-27 fail-open fix: that fallback is silent and correct for an
// ordinary fresh board, but WRONG to leave silent when the sector instead
// held a committed record that config_params_validate_ranges() just refused
// -- see config_store.h's "Load-time rejection diagnostics" block comment.
// That case gets a loud console_uart_puts() line naming the specific field
// and rule that failed (not just "validation failed" -- someone has to debug
// this from a log line alone, on a board with no other output channel this
// early in boot), and s_load_rejected latches so config_store_is_config_
// rejected() can report it to any later consumer. console_uart_puts() is
// safe to call here: console_uart_init() already ran (main.c step ~2, well
// before this function's own call site at step 4) and this is still
// pre-scheduler, the same context console_uart.h's own header comment
// documents as safe.
void config_store_boot_load(void)
{
    config_store_reject_info_t reject_info;
    memset(&reject_info, 0, sizeof(reject_info));

    s_cached_slot = read_latest_or_default(&s_cached_record, &s_cached_sector, &reject_info);
    s_load_rejected = (s_cached_slot == CONFIG_STORE_NO_SLOT) && reject_info.rejected;

    if (s_load_rejected) {
        // Two calls, not one. The single formatted line this replaced did not
        // fit: -Wformat-truncation proved at compile time that the fixed
        // consequence text alone needed 167 bytes of a buffer with 111-113
        // left after the field and rule strings, so the part an operator most
        // needs -- what the board is now DOING about it -- is exactly the part
        // that would have been cut off. The consequence text is a constant, so
        // it does not belong in a format buffer at all.
        char line[256];
        // %s on a possibly-NULL field/rule can't happen here: config_store_
        // unpack_ex() only ever sets rejected == true alongside non-NULL
        // field/rule (config_store.c's two RANGE-check call sites always
        // pass real out_field/out_rule pointers to config_params_validate_
        // ranges()), but "?" is printed instead of trusting that invariant
        // silently, matching this codebase's general preference for a
        // defensive fallback over an unverified assumption -- see e.g.
        // config_store.c's REC_OFF_SAFETY_TC_INSTALLED comment for the same
        // discipline applied to a wire byte instead of a pointer.
        snprintf(line, sizeof(line),
                 "SaftyFW: config_store REJECTED a committed record at load "
                 "(seq=%lu): field '%s' -- %s\r\n",
                 (unsigned long)reject_info.seq, reject_info.field ? reject_info.field : "?",
                 reject_info.rule ? reject_info.rule : "?");
        console_uart_puts(line);
        console_uart_puts("SaftyFW: falling back to compiled defaults: "
                          "abs_max_temp_c=0 (S1 will NOT trip until recommissioned), "
                          "calibration_missing stays true, config_crc reports 0 "
                          "(UNCOMMISSIONED).\r\n");
    }

    s_loaded = true;
}

bool config_store_is_config_rejected(void)
{
    return s_loaded && s_load_rejected;
}

uint8_t config_store_get_tc_type(void)
{
    if (!s_loaded) {
        // Defensive: a caller that runs before config_store_boot_load() gets
        // the safe default rather than uninitialised/zeroed memory -- same
        // "safe defaults over silence" discipline the rest of this module
        // follows. This is not the intended call order (main.c always loads
        // first) but a defensive default costs nothing and a wrong tc_type
        // is exactly the kind of silent hazard docs/THERMOCOUPLE.md section 2
        // warns about.
        return CONFIG_STORE_DEFAULT_TC_TYPE;
    }
    return s_cached_record.tc_type;
}

float config_store_get_tc_offset_c(void)
{
    if (!s_loaded) {
        return 0.0f; // safe default: no correction until proven otherwise
    }
    return s_cached_record.tc_offset_c;
}

bool config_store_is_calibration_missing(void)
{
    if (!s_loaded) {
        return true; // safe default: missing until proven otherwise
    }
    return s_cached_record.calibration_missing;
}

bool config_store_is_tc_type_set(void)
{
    if (!s_loaded) {
        return false; // safe default: treat as uncommissioned until proven otherwise
    }
    return (s_cached_record.fields_set & CONFIG_STORE_SET_TC_TYPE) != 0u;
}

void config_store_get_ct_cal(config_store_ct_channel_cal_t out[CONFIG_STORE_CT_CAL_NUM_CHANNELS])
{
    if (!s_loaded) {
        // Defensive, same reasoning as config_store_get_tc_type() above: a
        // caller that runs before config_store_boot_load() gets
        // config_store_default()'s shape (every channel calibrated ==
        // false) rather than uninitialised/zeroed memory that happens to
        // look the same today but is not guaranteed to.
        config_store_record_t def;
        config_store_default(&def);
        memcpy(out, def.ct_cal, sizeof(def.ct_cal));
        return;
    }
    memcpy(out, s_cached_record.ct_cal, sizeof(s_cached_record.ct_cal));
}

// SAFETY_CMD_FW_VERSION's config_version/config_crc fields (LINK_PROTOCOL.md
// sec 4), wired to the real cache now that config_store exists -- see
// link_task.c's link_task_send_fw_version(), which used to hard-code both to
// 0 with a "no config_store yet" comment.
void config_store_get_full_record(config_store_record_t *out)
{
    if (!out) {
        return;
    }
    if (!s_loaded) {
        config_store_default(out);
        return;
    }
    *out = s_cached_record;
}

uint8_t config_store_get_config_version(void)
{
    if (!s_loaded) {
        return 0;
    }
    // config_store_seq_to_version() (config_store.c, pure/host-tested) is
    // the actual mapping -- see its own header comment in config_store.h for
    // why this can no longer be a bare truncating `& 0xFFu`: that collided
    // with the "never loaded" sentinel (0) every 256th commit.
    return config_store_seq_to_version(s_cached_record.seq);
}

uint16_t config_store_get_config_crc(void)
{
    if (!s_loaded) {
        return 0;
    }
    // See config_store.h's own header comment on this function for the full
    // reasoning: seq == 0 is the same sentinel config_store_get_config_
    // version() already treats as "no CRC-verified record was ever
    // committed" (never written, OR committed-then-rejected-at-load), and
    // this getter must report the same "not confirmed" answer for the same
    // reason config_store_confirm_crc_ok() does -- KilnFW's safety_page.html/
    // diagnostics_page.html test THIS field, alone, for "UNCOMMISSIONED".
    // Returning the default record's own real (non-zero) packed CRC here,
    // as this function did before this fix, made a never-committed OR
    // rejected-at-load board read back as commissioned on both pages.
    if (s_cached_record.seq == 0u) {
        return 0u;
    }
    return (uint16_t)(config_store_record_crc(&s_cached_record) & 0xFFFFu);
}

typedef struct {
    hal_flash_region_t *region; // s_regions[plan.sector_index] -- the ONLY
                                 // sector this callback touches; the other
                                 // one (holding the still-current record
                                 // whenever needs_erase is true) is never
                                 // passed here at all, so there is no way for
                                 // this callback to erase or program the
                                 // sector a crash must still be able to fall
                                 // back on.
    size_t  next_write_slot;
    bool    needs_erase;
    uint8_t record[CONFIG_STORE_RECORD_LEN];
    hal_status_t result; // persist/save logging audit (2026-09-06): this file's
                          // own header comment used to argue the erase/program
                          // status could never be anything but HAL_OK here, and
                          // discarded it -- but update_task.c's near-identical
                          // update_metadata_write_cb() was fixed the same day
                          // ("discarding it would let a failed erase/program
                          // still report success up the call chain") for the
                          // exact same shape of call. hal_flash_safe_execute()'s
                          // own HAL_OK only means the callback RAN, not that the
                          // op it ran succeeded, so this must be captured and
                          // checked by config_store_write() too -- see that
                          // function below.
} config_store_write_args_t;

static void config_store_write_cb(void *param)
{
    config_store_write_args_t *a = (config_store_write_args_t *)param;
    a->result = HAL_OK;
    if (a->needs_erase) {
        a->result = hal_flash_erase(a->region, 0, SAFTYFW_CONFIG_STORE_FLASH_SIZE);
        if (a->result != HAL_OK) {
            return; // do not attempt the program half over a failed erase
        }
    }
    a->result = hal_flash_program(a->region,
                                   (uint32_t)a->next_write_slot * CONFIG_STORE_RECORD_LEN,
                                   a->record, CONFIG_STORE_RECORD_LEN);
}

// Writes `rec` as the new current config record, refusing while ARMED
// (TODO.md Phase 9: "Config writes refused while ARMED") -- checked here,
// against relay_owner_get_state(), not left to the caller, so every write
// path gets the same guarantee regardless of who calls this. Returns false
// and sets `*out_reason` to a human-readable explanation on refusal (ARMED)
// or on a flash_safe_execute() failure; the caller is expected to surface
// that string over HTTP/PC UART the same way other refusal reasons in this
// codebase are (see config_store_write_decision_reason()).
//
// No caller exists yet: TODO.md Phase 9 deliberately scopes this pass to
// "the store plus its first (read-only) consumer" -- there is no
// SAFTY_CMD_SET_CONFIG wire command wired to call this. It is built and
// ready for that follow-on work, not exercised by anything today.
bool config_store_write(const config_store_record_t *rec, const char **out_reason)
{
    config_store_write_decision_t decision =
        config_store_decide_write(relay_owner_get_state() == RELAY_OWNER_STATE_ARMED);
    if (decision != CONFIG_STORE_WRITE_OK) {
        if (out_reason != NULL) {
            *out_reason = config_store_write_decision_reason(decision);
        }
        return false;
    }

    if (!ensure_region()) {
        // Same NOT_PERMITTED family as a flash_safe_execute() failure below
        // -- the region has never bound (e.g. this is somehow called before
        // any successful boot_load), which is exactly the "safe execution
        // isn't possible at all" class config_store_flash_rc_reason()
        // already names, not a transient timeout.
        if (out_reason != NULL) {
            *out_reason = config_store_flash_rc_reason(CONFIG_STORE_FLASH_RC_NOT_PERMITTED);
        }
        return false;
    }

    config_store_record_t to_write = *rec;
    to_write.format_version = CONFIG_STORE_FORMAT_VERSION;
    to_write.seq = s_cached_record.seq + 1u;

    // config_store.h's "A/B sector arbitration" -- this plan names which
    // sector this write actually targets, and whether that sector needs an
    // erase first. When it is a sector SWITCH (needs_erase == true), the
    // target is always the OTHER sector from s_cached_sector -- the one
    // holding the still-valid current record is passed to config_store_
    // write_cb() only as a value already captured in s_cached_record, never
    // as `args.region`, so this callback cannot erase or reprogram it no
    // matter when a crash interrupts it.
    config_store_write_plan_t plan = config_store_plan_write(s_cached_sector, s_cached_slot);

    config_store_write_args_t args;
    args.region = s_regions[plan.sector_index];
    args.next_write_slot = plan.slot_index;
    args.needs_erase = plan.needs_erase;
    args.result = HAL_NOT_READY; // overwritten by the callback if it ever runs
    config_store_pack(&to_write, args.record);

    hal_status_t status = hal_flash_safe_execute(config_store_write_cb, &args, 1000u);
    // Both must succeed: `status` reports whether the callback ran at all
    // (lockout handshake), args.result reports whether the erase/program it
    // ran actually landed -- see config_store_write_args_t's own result field
    // comment for why neither check alone is sufficient (a HAL_OK `status`
    // with a failed args.result used to be silently reported as success).
    if (status != HAL_OK || args.result != HAL_OK) {
        // Surface WHICH failure mode this was, not a single opaque string --
        // see config_store_flash_rc_reason()'s header comment (config_store.h)
        // for why "the other core never answered the lockout" (TIMEOUT) and
        // "safe execution isn't possible at all" (NOT_PERMITTED) must not be
        // reported identically: one is a transient/bench condition, the
        // other is a firmware init-order bug. hal_flash_safe_execute()
        // already made this same distinction (hal_status_t); translated back
        // to the legacy CONFIG_STORE_FLASH_RC_* value so config_store_flash_
        // rc_reason()'s existing strings need no change.
        hal_status_t failing_status = (status != HAL_OK) ? status : args.result;
        int rc;
        (void)hal_status_to_config_store_flash_rc(failing_status, &rc);
        if (out_reason != NULL) {
            *out_reason = config_store_flash_rc_reason(rc);
        }
        return false;
    }

    s_cached_record = to_write;
    s_cached_slot = plan.slot_index;
    s_cached_sector = plan.sector_index;
    if (out_reason != NULL) {
        *out_reason = "ok";
    }
    return true;
}
