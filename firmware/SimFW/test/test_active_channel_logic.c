// Host test pinning the pure channel-selection rule in
// max31856_pio_engine.c's active_channel(), fixed 2026-08-25 for the bug
// where handle_load_done() silently dropped transactions on the safety bus
// (bus B): spi_transactions stuck at 0 (occasionally +1) while
// spi_protocol_errors climbed by hundreds, and a 4-write configure() burst
// landed as CR0=0x10 CR1=0x81 MASK=0x23 (write 2's own address byte applied
// as write 1's DATA, one register late) instead of CR0=0x90 CR1=0x23
// MASK=0xFC.
//
// Why this file mirrors rather than calls the real code: max31856_pio_engine.c
// includes hardware/dma.h, hardware/gpio.h, hardware/irq.h (pico-sdk headers)
// and is part of a FreeRTOS/PIO/DMA driver with no meaning off-target, so it
// is not in build_host_tests.ps1's source list -- the same pure/task boundary
// every existing test_*_logic.c file in this directory already respects (see
// test_gap_closure_logic.c's header comment for the established pattern).
// This is a deliberately small, byte-for-byte mirror of active_channel()'s
// decision rule in max31856_pio_engine.c, with `bus->cs_low[i]` and
// `gpio_get(bus->cs_gpio[i]) == 0` replaced by plain bool arrays the test
// controls directly -- keep the two in sync by hand if either changes.
//
// THE BUG BEING PINNED: the OLD active_channel() asked only "which CS pin
// reads low RIGHT NOW". handle_load_done() runs on DMA_IRQ_0, which cannot
// preempt the CS-rise GPIO handler (same NVIC priority, per that handler's
// own ORDERING comment) -- so if handle_load_done() is still queued behind a
// slow CS-rise teardown when the NEXT transaction's CS has already both
// fallen AND risen again (a MAX31856 single-byte write is ~4us at 4MHz, well
// within that teardown's busy-wait DMA abort + PIO register writes), the live
// pin reads HIGH for a transaction whose address word the sniff/load DMA
// chain already captured correctly -- and the old code silently returned -1,
// dropping it. The NEW rule instead trusts bus->cs_low[], softwar's own
// "is this channel's transaction still open" bookkeeping, which is set
// promptly at the CS-falling edge and cleared only after that SAME channel's
// own handle_load_done() call would already have run.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#define MAX_CHANNELS 3

// Mirrors max31856_pio_engine.c's active_channel(), current (fixed) version.
static int active_channel_new(const bool cs_low[MAX_CHANNELS], const bool gpio_low[MAX_CHANNELS],
                               uint8_t channel_count)
{
    int fallback = -1;
    for (uint8_t i = 0; i < channel_count; i++) {
        if (cs_low[i]) {
            if (fallback < 0) {
                fallback = (int)i;
            }
            if (gpio_low[i]) {
                return (int)i;
            }
        }
    }
    if (fallback >= 0) {
        return fallback;
    }
    for (uint8_t i = 0; i < channel_count; i++) {
        if (gpio_low[i]) {
            return (int)i;
        }
    }
    return -1;
}

// Mirrors max31856_pio_engine.c's active_channel(), PRE-FIX version -- kept
// here ONLY so the tests below can demonstrate the old rule actually fails
// the delayed-handler scenario (i.e. this negative test would have failed
// against the code as it shipped before 2026-08-25's fix, proving the new
// test is not vacuous).
static int active_channel_old(const bool gpio_low[MAX_CHANNELS], uint8_t channel_count)
{
    for (uint8_t i = 0; i < channel_count; i++) {
        if (gpio_low[i]) {
            return (int)i;
        }
    }
    return -1;
}

void run_test_active_channel_logic(void)
{
    TEST_SECTION("active_channel selection logic (max31856_pio_engine.c)");

    // --- The exact bug scenario: single-channel bus B, handle_load_done()
    // delayed until AFTER the transaction's CS has already risen again.
    // Software still considers the channel open (cs_low[0] = true, set
    // promptly at the falling edge); the live pin has already gone high
    // (gpio_low[0] = false) because the whole 2-byte write completed while
    // this handler was queued behind the previous transaction's teardown.
    {
        bool cs_low[MAX_CHANNELS] = { true, false, false };
        bool gpio_low[MAX_CHANNELS] = { false, false, false }; // CS already rose on the wire
        TEST_CHECK(active_channel_new(cs_low, gpio_low, 1) == 0,
                   "delayed handler: new rule finds channel 0 via cs_low[] even though the live pin already reads high");
        // Negative test: prove this scenario is not vacuous by showing the
        // OLD (pre-fix) rule actually fails it -- this is the bug that shipped.
        TEST_CHECK(active_channel_old(gpio_low, 1) == -1,
                   "negative test: the pre-fix gpio-only rule drops this exact transaction (returns -1)");
    }

    // --- Ordinary case: transaction genuinely in flight, live pin agrees
    // with cs_low[]. Both old and new rules must still find it -- the fix
    // must not regress the common case.
    {
        bool cs_low[MAX_CHANNELS] = { true, false, false };
        bool gpio_low[MAX_CHANNELS] = { true, false, false };
        TEST_CHECK(active_channel_new(cs_low, gpio_low, 1) == 0,
                   "ordinary in-flight transaction: new rule still finds channel 0");
        TEST_CHECK(active_channel_old(gpio_low, 1) == 0,
                   "ordinary in-flight transaction: old rule also finds channel 0 (sanity check, not the regression under test)");
    }

    // --- Multi-channel bus (bus A shape): the channel that just ended is
    // still stale-true in cs_low[] (its own teardown hasn't reached step 5
    // yet) while a DIFFERENT channel's CS has genuinely fallen for a new
    // transaction. The new rule must prefer the one the live pin confirms is
    // actually active now, not the stale leftover.
    {
        bool cs_low[MAX_CHANNELS] = { true, true, false };   // ch0 stale-open, ch1 newly open
        bool gpio_low[MAX_CHANNELS] = { false, true, false }; // only ch1 is live-low right now
        TEST_CHECK(active_channel_new(cs_low, gpio_low, 3) == 1,
                   "multi-channel disambiguation: prefer the candidate the live pin confirms, not the stale one");
    }

    // --- Multi-channel bus, delayed case with no live-pin confirmation for
    // either stale-open candidate (both already risen): falls back to the
    // first cs_low[] candidate rather than silently returning -1.
    {
        bool cs_low[MAX_CHANNELS] = { false, true, false };
        bool gpio_low[MAX_CHANNELS] = { false, false, false };
        TEST_CHECK(active_channel_new(cs_low, gpio_low, 3) == 1,
                   "delayed multi-channel case with no live match: falls back to the cs_low[] candidate instead of -1");
    }

    // --- Nobody has cs_low[] set at all (the pre-existing "missed the
    // falling edge" case the original code's own comment already
    // anticipated) but the live pin shows a channel low: last-resort
    // fallback to the live pin must still work, so this belt-and-braces
    // behaviour is not lost by the fix.
    {
        bool cs_low[MAX_CHANNELS] = { false, false, false };
        bool gpio_low[MAX_CHANNELS] = { false, true, false };
        TEST_CHECK(active_channel_new(cs_low, gpio_low, 3) == 1,
                   "missed-falling-edge fallback: live pin is still consulted when cs_low[] has nothing");
    }

    // --- Truly nothing active: must still return -1, not fabricate a
    // channel. This is the check that proves the function can still fail
    // when it should -- a version that always returned 0 would pass every
    // check above except this one.
    {
        bool cs_low[MAX_CHANNELS] = { false, false, false };
        bool gpio_low[MAX_CHANNELS] = { false, false, false };
        TEST_CHECK(active_channel_new(cs_low, gpio_low, 3) == -1,
                   "nothing open anywhere: must return -1, not a fabricated channel");
    }
}
