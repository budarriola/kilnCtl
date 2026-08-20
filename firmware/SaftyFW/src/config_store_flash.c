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

#include <string.h>

#include "pico/flash.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h" // XIP_BASE

#include "flash_layout.h" // bootloader/ -- SAFTYFW_CONFIG_STORE_FLASH_OFFSET/_SIZE
#include "max31856.h"      // MAX31856_TC_TYPE_K -- asserted to match CONFIG_STORE_DEFAULT_TC_TYPE
#include "tasks/relay_owner.h" // relay_owner_get_state() -- the ARMED check

// Compile-time cross-check: config_store.h's CONFIG_STORE_DEFAULT_TC_TYPE is
// duplicated as a literal rather than #including max31856.h (see
// config_store.h's comment on why), so this catches the two ever drifting
// apart instead of silently defaulting to the wrong type.
typedef char config_store_default_tc_type_matches_max31856
    [(CONFIG_STORE_DEFAULT_TC_TYPE == MAX31856_TC_TYPE_K) ? 1 : -1];

static config_store_record_t s_cached_record;
static size_t s_cached_slot = CONFIG_STORE_NO_SLOT;
static bool s_loaded = false;

static size_t read_latest_or_default(config_store_record_t *out_rec)
{
    const uint8_t *region =
        (const uint8_t *)(XIP_BASE + SAFTYFW_CONFIG_STORE_FLASH_OFFSET);
    size_t latest = config_store_find_latest(region, out_rec);
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
void config_store_boot_load(void)
{
    s_cached_slot = read_latest_or_default(&s_cached_record);
    s_loaded = true;
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
        if (out_reason != NULL) {
            *out_reason = "flash write failed";
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
