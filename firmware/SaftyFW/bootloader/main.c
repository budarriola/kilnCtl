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

#include "board_pins.h"
#include "crc32.h"
#include "flash_layout.h"
#include "metadata.h"

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
// NOT independently verified against real pico-sdk headers or hardware in
// this pass -- no toolchain/hardware was available to build and confirm it
// against a real flash datasheet or a pico-sdk example (e.g. the SDK's own
// flash_id-style examples). Double-check this encoding before trusting it on
// real hardware; this codebase's own discipline is to never claim more
// confidence than has actually been verified.
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
// Scoped down per the coordinator's explicit decision this session: UART1
// bring-up plus a periodic status beacon ONLY.
//
// NOT implemented this pass: UPDATE_BEGIN(0x10)/UPDATE_DATA(0x11)/
// UPDATE_END(0x12)/UPDATE_ABORT(0x13) frame handling (receiving and writing
// image data into flash), and the beacon below is a raw, distinctive marker
// byte sequence, NOT a real framed UPDATE_STATUS(0x14) kilnlink frame. Both
// are real streaming-flash-write / wire-framing logic that deserve their own
// dedicated, carefully-reviewed pass -- see
// firmware/CommonFW/docs/UPDATE_PROTOCOL.md section 4 and
// docs/BOOTLOADER.md section 4 for exactly what that follow-on needs to
// build.
//
// GPIO6 was already driven low as this file's first statement and is never
// touched again below -- this function does not reference it, by design.
static void enter_recovery(void) __attribute__((noreturn));

static void enter_recovery(void)
{
    uart_init(uart1, 115200u);
    gpio_set_function(SAFTYFW_PIN_UART1_TX, GPIO_FUNC_UART);
    gpio_set_function(SAFTYFW_PIN_UART1_RX, GPIO_FUNC_UART);
    // Plain hardware UART, no inversion, no PIO -- same as
    // src/tasks/uart_owner.c's uart_owner_init(); the ESP inverts on its
    // side.
    uart_set_hw_flow(uart1, false, false);
    uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart1, true);

    // Distinctive raw marker, not a kilnlink frame -- 0x7E is kilnlink's own
    // delimiter (CommonFW/docs/LINK_PROTOCOL.md section 3), reused here only
    // so a byte-level capture visually brackets the marker; a real listener
    // must not attempt to parse this as a frame.
    static const uint8_t recovery_beacon[] = {
        0x7Eu, 'R', 'E', 'C', 'O', 'V', 0x7Eu,
    };

    for (;;) {
        for (size_t i = 0; i < sizeof(recovery_beacon); i++) {
            uart_putc_raw(uart1, (char)recovery_beacon[i]);
        }
        // No timeout out of recovery -- loop forever. "There is nothing
        // safe to time out into" (docs/BOOTLOADER.md section 4).
        sleep_ms(1000);
    }
}

// --- Step 6: persist metadata --------------------------------------------
//
// Writes `*meta` to the next free log slot in the metadata sector,
// incrementing seq exactly once first -- bootloader_decide_boot() and
// bootloader_decide_after_crc_fail() are pure and deliberately do not touch
// seq themselves (metadata.h's own doc comments; confirmed against
// metadata.c: neither decision_boot() nor decision_not_bootable() assigns
// updated_meta.seq).
static void persist_metadata(bootloader_metadata_t *meta, size_t latest_slot_index)
{
    meta->seq = meta->seq + 1u;

    size_t next_write_slot = bootloader_metadata_next_write_slot(latest_slot_index);
    bool needs_erase = bootloader_metadata_next_write_needs_erase(latest_slot_index);

    uint8_t record[BOOTLOADER_METADATA_RECORD_LEN];
    bootloader_metadata_pack(meta, record); // pure byte packing, no flash I/O

    // flash_range_erase()/flash_range_program() must not race a flash read --
    // including this very code's own subsequent instruction fetches, since
    // this bootloader executes from flash (XIP). This is a single-core,
    // bare-metal image with no ISRs registered, so plain
    // save_and_disable_interrupts()/restore_interrupts() is sufficient;
    // flash_safe_execute()'s multicore lockout exists for when core 1 might
    // be running unrelated code concurrently, which cannot happen here.
    uint32_t ints = save_and_disable_interrupts();
    if (needs_erase) {
        // Whole-sector erase -- 4K is the only erase granularity available
        // (metadata.h's header comment: this is why the log scheme exists
        // in the first place).
        flash_range_erase(BOOTLOADER_METADATA_FLASH_OFFSET, BOOTLOADER_METADATA_FLASH_SIZE);
    }
    flash_range_program(BOOTLOADER_METADATA_FLASH_OFFSET +
                             (uint32_t)next_write_slot * BOOTLOADER_METADATA_RECORD_LEN,
                         record, BOOTLOADER_METADATA_RECORD_LEN);
    restore_interrupts(ints);
}

// --- Step 7: jump to the application -------------------------------------
//
// Standard RP2040 vector-table relocation. `slot_offset` is the chosen
// slot's flash-relative offset (BOOTLOADER_SLOT_A_FLASH_OFFSET or _B); the
// application's own vector table lives at the very start of its slot.
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
            persist_metadata(&decision.updated_meta, latest_slot);
        }
        enter_recovery();
    }

    // Step 6.
    if (decision.needs_metadata_update) {
        persist_metadata(&decision.updated_meta, latest_slot);
    }

    // Step 7 -- never returns.
    uint32_t chosen_offset = (decision.chosen_slot == BOOTLOADER_SLOT_A)
                                  ? BOOTLOADER_SLOT_A_FLASH_OFFSET
                                  : BOOTLOADER_SLOT_B_FLASH_OFFSET;
    jump_to_app(chosen_offset);

    // Unreachable -- jump_to_app() is noreturn.
    return 0;
}
