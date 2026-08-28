// config_store_flash.c -- the real flash I/O and ARMED-check glue for
// config_store.h's pure record logic. NOT host-tested, for the same reason
// bootloader/main.c and src/tasks/update_task.c are not: it needs a real
// RP2040 (XIP-mapped reads, flash_safe_execute()'s multicore lockout under
// FreeRTOS SMP -- see update_task.c's header comment for the full "why
// flash_safe_execute(), not save_and_disable_interrupts()" reasoning, which
// applies identically here).
//
// Caches the current record in a static, so config_store_get_tc_type()/
// config_store_is_calibration_missing() are cheap, lock-free reads for
// hot-path callers (main.c at boot, and eventually thermo_task) rather than
// re-scanning flash every call. config_store_boot_load() must run once,
// early in main()'s boot sequence, before anything reads the cache.
#include "config_store.h"

#include <stdio.h>
#include <string.h>

#include "pico/error.h"
#include "pico/flash.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h" // XIP_BASE

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

// Compile-time cross-check, same pattern as the one above: config_store.h's
// CONFIG_STORE_FLASH_RC_* literals are a dependency-free duplicate of
// pico/error.h's `enum pico_error_codes` (config_store.h's own comment on
// config_store_flash_rc_reason() explains why config_store.c can't just
// #include pico/error.h directly). If a future pico-sdk upgrade ever
// renumbers PICO_ERROR_TIMEOUT/_NOT_PERMITTED/_INSUFFICIENT_RESOURCES, this
// fails the build instead of silently making config_store_flash_rc_reason()
// return the wrong string for a real failure.
typedef char config_store_flash_rc_ok_matches_pico_error
    [(CONFIG_STORE_FLASH_RC_OK == PICO_OK) ? 1 : -1];
typedef char config_store_flash_rc_timeout_matches_pico_error
    [(CONFIG_STORE_FLASH_RC_TIMEOUT == PICO_ERROR_TIMEOUT) ? 1 : -1];
typedef char config_store_flash_rc_not_permitted_matches_pico_error
    [(CONFIG_STORE_FLASH_RC_NOT_PERMITTED == PICO_ERROR_NOT_PERMITTED) ? 1 : -1];
typedef char config_store_flash_rc_insufficient_resources_matches_pico_error
    [(CONFIG_STORE_FLASH_RC_INSUFFICIENT_RESOURCES == PICO_ERROR_INSUFFICIENT_RESOURCES) ? 1
                                                                                          : -1];

static config_store_record_t s_cached_record;
static size_t s_cached_slot = CONFIG_STORE_NO_SLOT;
static bool s_loaded = false;
// True iff the sector held a structurally-intact (magic/CRC/format_version
// all valid) record that config_params_validate_ranges() refused, and no
// OTHER slot in the sector was good -- config_store.h's "Load-time rejection
// diagnostics" case 2, distinct from an ordinary never-committed board (case
// 1, this stays false). See config_store_is_config_rejected()'s own comment.
static bool s_load_rejected = false;

static size_t read_latest_or_default(config_store_record_t *out_rec,
                                      config_store_reject_info_t *out_reject)
{
    const uint8_t *region =
        (const uint8_t *)(XIP_BASE + SAFTYFW_CONFIG_STORE_FLASH_OFFSET);
    size_t latest = config_store_find_latest_ex(region, out_rec, out_reject);
    if (latest == CONFIG_STORE_NO_SLOT) {
        config_store_default(out_rec);
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

    s_cached_slot = read_latest_or_default(&s_cached_record, &reject_info);
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
    size_t  next_write_slot;
    bool    needs_erase;
    uint8_t record[CONFIG_STORE_RECORD_LEN];
} config_store_write_args_t;

static void config_store_write_cb(void *param)
{
    config_store_write_args_t *a = (config_store_write_args_t *)param;
    if (a->needs_erase) {
        flash_range_erase(SAFTYFW_CONFIG_STORE_FLASH_OFFSET, SAFTYFW_CONFIG_STORE_FLASH_SIZE);
    }
    flash_range_program(SAFTYFW_CONFIG_STORE_FLASH_OFFSET +
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

    config_store_record_t to_write = *rec;
    to_write.format_version = CONFIG_STORE_FORMAT_VERSION;
    to_write.seq = s_cached_record.seq + 1u;

    config_store_write_args_t args;
    args.next_write_slot = config_store_next_write_slot(s_cached_slot);
    args.needs_erase = config_store_next_write_needs_erase(s_cached_slot);
    config_store_pack(&to_write, args.record);

    int rc = flash_safe_execute(config_store_write_cb, &args, 1000u);
    if (rc != PICO_OK) {
        // Surface WHICH failure mode this was, not a single opaque string --
        // see config_store_flash_rc_reason()'s header comment (config_store.h)
        // for why "the other core never answered the lockout" (TIMEOUT) and
        // "safe execution isn't possible at all" (NOT_PERMITTED) must not be
        // reported identically: one is a transient/bench condition, the
        // other is a firmware init-order bug.
        if (out_reason != NULL) {
            *out_reason = config_store_flash_rc_reason(rc);
        }
        return false;
    }

    s_cached_record = to_write;
    s_cached_slot = args.next_write_slot;
    if (out_reason != NULL) {
        *out_reason = "ok";
    }
    return true;
}
