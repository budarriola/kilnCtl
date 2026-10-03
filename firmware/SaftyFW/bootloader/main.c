// main.c -- SaftyFW bootloader entry point. Implements docs/BOOTLOADER.md
// section 3 ("What the bootloader does") in order, calling out to the
// frozen, host-tested logic in metadata.h/crc32.h for anything that does not
// need real flash I/O.
//
// This is a SEPARATE executable from src/main.c's FreeRTOS application --
// bare-metal, no RTOS, built by this directory's own CMakeLists.txt. It is
// written once over SWD and never updates itself (docs/BOOTLOADER.md
// section 1): the whole point of this file's existence is to be the
// recovery path underneath the application, so it stays as small and as
// auditable as it can.
//
//   1. GPIO6 -> output, driven LOW.        <- FIRST, before anything else,
//      exactly as src/main.c does for the same reason: the relay must be
//      open before any other decision -- including a flash-capacity probe,
//      a metadata read or an 832K CRC -- is taken.
//   2. Flash-capacity sanity check (new this session, not in
//      docs/BOOTLOADER.md's original text -- see
//      flash_capacity_at_least_expected()'s own comment).
//   3. Read and validate metadata (XIP, no flash I/O of our own).
//   4. bootloader_decide_boot().
//   5. CRC the chosen slot, every boot, with one retry via
//      bootloader_decide_after_crc_fail().
//   6. Persist metadata if any decision call asked for it.
//   7. Jump to the application. Never returns.
//
// If no slot is bootable at any point, or the flash-capacity check fails,
// this falls into recovery mode instead -- see enter_recovery()'s comment
// for exactly what that does and does not implement this pass.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pico/stdlib.h"

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/regs/addressmap.h" // XIP_BASE
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"

#include "board_pins.h"
#include "crc32.h"
#include "flash_layout.h"
#include "metadata.h"
#include "persist.h"
#include "recovery_update.h"

// --- Step 2: flash-capacity sanity check --------------------------------
//
// New, deliberately conservative logic requested by the coordinator this
// session -- NOT originally specified by docs/BOOTLOADER.md. flash_layout.h's
// BOOTLOADER_FLASH_TOTAL_SIZE (2 MiB) is a *build-time* assumption baked in
// from PICO_BOARD=pico's pico-sdk board definition (TODO.md Phase 10 item
// 10.1: "not a bench measurement" -- it is trusted because a stock Pico's
// onboard flash is a fixed hardware fact, not because anyone measured this
// specific chip). This function is the *runtime* check that verifies that
// assumption against the physical part actually soldered down, rather than
// blindly trusting the compile-time constant -- a Pico clone or a W variant
// could genuinely differ.
//
// JEDEC "Read ID" (0x9F), sent as a 4-byte transaction ({0x9F,0,0,0} out,
// capturing the 4-byte response): byte index 3 of the response is the
// capacity code for the common Winbond/GigaDevice-style convention this
// board's onboard flash is expected to follow, where actual size in bytes =
// 1u << capacity_code (e.g. a W25Q16-class part reports 0x15, and
// 1u << 0x15 == 2097152 == 2 MiB, matching BOOTLOADER_FLASH_TOTAL_SIZE).
//
// Verified on real hardware 2026-08-23: read over SWD, this board's onboard
// flash reports JEDEC id 0x1540ef -- a W25Q16JV, 2048 KiB -- so capacity code
// 0x15 decoding to 1u << 0x15 == 2097152 bytes is correct and matches
// BOOTLOADER_FLASH_TOTAL_SIZE exactly. The encoding below is confirmed, not
// merely assumed.
static bool flash_capacity_at_least_expected(void)
{
    uint8_t txbuf[4] = { 0x9Fu, 0x00u, 0x00u, 0x00u };
    uint8_t rxbuf[4] = { 0u, 0u, 0u, 0u };

    // flash_do_cmd() handles the XIP-disable dance internally -- safe to
    // call from code that is itself executing from flash.
    flash_do_cmd(txbuf, rxbuf, sizeof(txbuf));

    uint8_t capacity_code = rxbuf[3];

    // 1u << capacity_code is only defined behaviour for capacity_code < 32;
    // treat anything at or above that the same as "implausible" below
    // rather than invoking it.
    uint32_t decoded_size = (capacity_code < 32u) ? (1u << capacity_code) : 0u;

    // Anything outside [64KB, 128MB] is "couldn't determine", not trusted --
    // this also naturally catches an all-0x00 or all-0xFF response (a
    // failed/no-response JEDEC read) without a separate special case. This
    // is a *sanity* check meant to catch a definitely-too-small chip, not a
    // hard gate that must always positively confirm capacity: "can't tell"
    // maps to "assume big enough" rather than blocking boot on an uncertain
    // read.
    const uint32_t plausible_min = 64u * 1024u;
    const uint32_t plausible_max = 128u * 1024u * 1024u;
    if (decoded_size < plausible_min || decoded_size > plausible_max) {
        return true; // couldn't determine -- assume big enough
    }

    return decoded_size >= BOOTLOADER_FLASH_TOTAL_SIZE;
}

// --- Recovery mode (docs/BOOTLOADER.md section 4) -----------------------
//
// Brings up UART1, drives the recovery_update.c frame-handling loop
// (UPDATE_BEGIN/UPDATE_DATA/UPDATE_END/UPDATE_ABORT + a periodic
// UPDATE_STATUS beacon), and never returns -- "no timeout out of it"
// (TODO.md item 10.4). See recovery_update.h's header comment for exactly
// what that loop implements and what it deliberately does not.
//
// GPIO6 was already driven low as this file's first statement and is never
// touched again below -- neither this function nor recovery_update.c
// reference it, by design.
static void enter_recovery(void) __attribute__((noreturn));

static void enter_recovery(void)
{
    // Must equal the application's UART_OWNER_BAUD_RATE
    // (src/tasks/uart_owner.c) and KilnFW's CONFIG_KILNCTL_SAFETY_BAUD_RATE:
    // recovery mode is useless if the host cannot talk to it.
    //
    // Raised from 9600 on 2026-08-25 along with the application. The 9600
    // ceiling belonged to the TCMT1109 optocouplers, which are gone -- the
    // barrier is now one ADuM1201WT digital isolator (U6). The sweep that
    // chose this rate is recorded in KilnFW/App/drivers/Kconfig under
    // KILNCTL_SAFETY_BAUD_RATE.
    uart_init(uart1, 230400u);
    gpio_set_function(SAFTYFW_PIN_UART1_TX, GPIO_FUNC_UART);
    gpio_set_function(SAFTYFW_PIN_UART1_RX, GPIO_FUNC_UART);
    // Plain hardware UART, no inversion, no PIO -- now genuinely the same as
    // src/tasks/uart_owner.c's uart_owner_init(), which is a change worth
    // recording: until 2026-08-25 that function applied
    // gpio_set_outover(GPIO_OVERRIDE_INVERT) to the TX pin and this function
    // never did, so recovery mode's Pico->ESP polarity was the opposite of
    // the application's for as long as the optocouplers were fitted. The
    // comment here claimed they matched; they did not. Neither end inverts
    // now, because the ADuM1201 does not invert either.
    uart_set_hw_flow(uart1, false, false);
    uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart1, true);

    recovery_update_run(); // never returns
    __builtin_unreachable();
}

// --- Step 7: jump to the application -------------------------------------
//
// Standard RP2040 vector-table relocation. `slot_offset` is the chosen
// slot's flash-relative offset (BOOTLOADER_SLOT_A_FLASH_OFFSET or _B); the
// application's own vector table lives at the very start of its slot.
//
// Watchdog: armed just before the jump. A CRC-good image that hangs before it
// arms its own watchdog (crt0, runtime init, console/stdio init) would
// otherwise never reset, and metadata.c's 3-attempt boot_attempts fallback
// only advances on a reset -- i.e. only on a power cycle. With this armed, a
// hang becomes a watchdog reset, the bootloader runs again, boot_attempts has
// already been persisted for this attempt, and after the cap the fallback /
// recovery path is taken without a human at the board.
//
// 8000 ms is just under the RP2040 hardware maximum (watchdog_enable()
// asserts delay_ms * 2000 <= 0xFFFFFF, i.e. 8388 ms). The application's own
// main() calls watchdog_enable(1000 ms) as its "Step 2" (src/main.c), which
// reloads the counter and replaces this timeout; the only work ahead of that
// is pico-sdk runtime init, the relay GPIO, console_uart_init() and
// stdio_init_all(), estimated at tens of ms. That is more than 100x inside
// the 8 s budget (an estimate: no on-target measurement was available).
// pause_on_debug = true, same as the application, so an attached probe that
// halts the core does not reset the board mid-session.
#define BOOTLOADER_APP_WATCHDOG_MS 8000u

static void jump_to_app(uint32_t slot_offset) __attribute__((noreturn));

static void jump_to_app(uint32_t slot_offset)
{
    uint32_t app_vtor = XIP_BASE + slot_offset;

    // Point the System Control Block at the application's vector table
    // before touching SP/PC -- any fault taken between here and the branch
    // below must land in the new image's handlers, not this bootloader's.
    scb_hw->vtor = app_vtor;

    // The application's vector table: word 0 is its initial stack pointer,
    // word 1 is its reset handler (the standard Cortex-M layout).
    uint32_t app_sp = ((uint32_t *)app_vtor)[0];
    uint32_t app_entry = ((uint32_t *)app_vtor)[1];

    watchdog_enable(BOOTLOADER_APP_WATCHDOG_MS, true);

    // Never coming back -- no ISR of this bootloader's may fire mid-jump,
    // and there is nothing here to restore state to.
    save_and_disable_interrupts();

    __asm volatile (
        "msr msp, %0 \n"
        "bx  %1 \n"
        :
        : "r" (app_sp), "r" (app_entry)
        :
    );

    __builtin_unreachable();
}

int main(void)
{
    // Step 1. Literal first statement, before any other init.
    gpio_init(SAFTYFW_PIN_RELAY);
    gpio_set_dir(SAFTYFW_PIN_RELAY, GPIO_OUT);
    gpio_put(SAFTYFW_PIN_RELAY, 0);

    // Defensive: a watchdog left enabled by the previous image (the
    // application's 1 s one, or the 8 s one armed in jump_to_app()) must never
    // count down underneath recovery mode, which has no timeout and never
    // feeds it. jump_to_app() re-arms right before handing over. GPIO6 is
    // already low and untouched by this.
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);

    // Step 2. If the physical chip is definitely smaller than the frozen
    // layout assumes, nothing below is trustworthy -- reading metadata or
    // jumping anywhere could read out of bounds or alias. GPIO6 is already
    // low, so recovery's beacon at least tells a human the board is up,
    // with nothing about it trusted to boot.
    if (!flash_capacity_at_least_expected()) {
        enter_recovery();
    }

    // Step 3. Metadata is memory-mapped for direct reads at
    // XIP_BASE + BOOTLOADER_METADATA_FLASH_OFFSET -- no flash I/O of our
    // own needed for this step.
    const uint8_t *metadata_region =
        (const uint8_t *)(XIP_BASE + BOOTLOADER_METADATA_FLASH_OFFSET);
    bootloader_metadata_t meta;
    size_t latest_slot = bootloader_metadata_find_latest(metadata_region, &meta);
    if (latest_slot == BOOTLOADER_METADATA_NO_SLOT) {
        // No record in the log validates -- docs/BOOTLOADER.md section 3
        // step 2's "if both copies are bad, enter recovery", generalised to
        // the log scheme (metadata.h's own doc comment on this function).
        enter_recovery();
    }

    // Step 4.
    bootloader_boot_decision_t decision = bootloader_decide_boot(&meta);

    // Step 5. CRC the chosen slot, every boot, with a single retry via
    // bootloader_decide_after_crc_fail() -- only two slots total, so a
    // second failure means neither is usable.
    bool crc_ok = false;
    for (int attempt = 0; attempt < 2 && decision.bootable; attempt++) {
        uint32_t slot_offset = (decision.chosen_slot == BOOTLOADER_SLOT_A)
                                    ? BOOTLOADER_SLOT_A_FLASH_OFFSET
                                    : BOOTLOADER_SLOT_B_FLASH_OFFSET;
        uint32_t length = decision.updated_meta.slots[decision.chosen_slot].length;
        uint32_t expected_crc = decision.updated_meta.slots[decision.chosen_slot].crc32;
        const uint8_t *slot_data = (const uint8_t *)(XIP_BASE + slot_offset);

        if (bootloader_crc32(slot_data, length) == expected_crc) {
            crc_ok = true;
            break;
        }

        decision = bootloader_decide_after_crc_fail(&decision.updated_meta, decision.chosen_slot);
    }

    if (!crc_ok || !decision.bootable) {
        // Neither the initial choice nor (if it was tried) the fallback
        // slot is usable. Persist whatever BAD-marking happened along the
        // way -- it is valuable for a human debugging over SWD later -- then
        // fall into recovery instead of jumping anywhere.
        if (decision.needs_metadata_update) {
            bootloader_persist_metadata(&decision.updated_meta, latest_slot);
        }
        enter_recovery();
    }

    // Step 6.
    if (decision.needs_metadata_update) {
        bootloader_persist_metadata(&decision.updated_meta, latest_slot);
    }

    // Step 7 -- never returns.
    uint32_t chosen_offset = (decision.chosen_slot == BOOTLOADER_SLOT_A)
                                  ? BOOTLOADER_SLOT_A_FLASH_OFFSET
                                  : BOOTLOADER_SLOT_B_FLASH_OFFSET;
    jump_to_app(chosen_offset);

    // Unreachable -- jump_to_app() is noreturn.
    return 0;
}
