// safe_reboot.c -- see safe_reboot.h for the full safety rationale (why the
// MCP23017 expanders' retained state makes this more than an ordinary
// reboot) and the exact sequence this implements.
#include "safe_reboot.h"

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "pico/bootrom.h"

#include "i2c_owner.h"
#include "wave_owner.h"

// Poll cadence and total bound for confirming the safe-state commands landed
// (i2c_owner.h/wave_owner.h's queue-then-apply-next-tick contract -- neither
// setter takes effect synchronously). i2c_owner's own scan tick runs every
// MCP23017_DEBOUNCE_SCAN_MS (8 ms, mcp23017.h); wave_owner applies CT
// changes at the next zero crossing, up to one full 60 Hz cycle (~16.7 ms)
// away. SAFE_REBOOT_TIMEOUT_MS is chosen generously past both of those --
// on the order of 20 ticks/cycles -- so a healthy fixture confirms in well
// under one tenth of this budget, and a fixture that has NOT confirmed by
// this point is very likely genuinely wedged (I2C bus stuck, wave_owner
// task starved), not just unlucky timing -- exactly the case where refusing
// to reboot (safe_reboot.h's "refuse, never reboot anyway" policy) is the
// right call.
#define SAFE_REBOOT_POLL_MS    10u
#define SAFE_REBOOT_TIMEOUT_MS 300u

// True only once every commanded safe-state output has been read back as
// actually applied -- see safe_reboot.h's numbered sequence, step 2.
static bool safe_state_confirmed(void)
{
    if (!i2c_owner_get_estop_open()) {
        return false;
    }
    if (i2c_owner_get_dut_power_main_on()) {
        return false;
    }
    if (i2c_owner_get_dut_power_safety_on()) {
        return false;
    }

    for (uint8_t ch = 0; ch < CT_WAVE_NUM_CHANNELS; ch++) {
        ct_wave_channel_state_t st;
        if (!ct_wave_get_state(ch, &st) || !st.valid) {
            return false; // no confirmed reading yet -- cannot call this channel silent
        }
        if (st.mode != CT_WAVE_MODE_MANUAL || st.amps != 0.0f) {
            return false;
        }
    }
    return true;
}

bool safe_reboot_into_bootloader(void)
{
    // Step 1: queue every safe-state command. Order within this step does
    // not matter for correctness (each setter posts to its own owner's
    // queue independently), but ct_wave_set_amps() before
    // ct_wave_set_mode() per channel does matter -- see safe_reboot.h's
    // comment on why that ordering avoids a transient "MANUAL at a stale
    // nonzero amps" state.
    (void)i2c_owner_set_estop(true); // fail-safe direction: loop OPEN / STOP
    (void)i2c_owner_set_dut_power_main(false);
    (void)i2c_owner_set_dut_power_safety(false);
    for (uint8_t ch = 0; ch < CT_WAVE_NUM_CHANNELS; ch++) {
        (void)ct_wave_set_amps(ch, 0.0f);
        (void)ct_wave_set_mode(ch, CT_WAVE_MODE_MANUAL);
    }

    // Step 2/3: bounded poll-then-reboot. A queue-full return from any of
    // the setters above is not treated specially here -- if a setter's
    // command was dropped, the corresponding readback simply never
    // confirms, and this loop times out and refuses exactly as it would for
    // any other failure to reach the safe state.
    for (uint32_t waited_ms = 0; waited_ms < SAFE_REBOOT_TIMEOUT_MS; waited_ms += SAFE_REBOOT_POLL_MS) {
        if (safe_state_confirmed()) {
            reset_usb_boot(0, 0); // never returns
        }
        vTaskDelay(pdMS_TO_TICKS(SAFE_REBOOT_POLL_MS));
    }

    // One last check after the final delay -- avoids discarding a
    // confirmation that lands exactly at the timeout boundary.
    if (safe_state_confirmed()) {
        reset_usb_boot(0, 0); // never returns
    }

    return false; // refuse -- see safe_reboot.h's "refuse, never reboot anyway" policy
}
