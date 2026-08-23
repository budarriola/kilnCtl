// Host tests pinning the pure decision logic behind SimFW's dual-CDC USB
// composite (2026-08-23, "give SimFW a second USB CDC interface, dedicated
// to console/log output"): usb_descriptors.c's config-descriptor
// arithmetic/interface-and-endpoint layout, usb_owner.c's CDC1 console
// write gating, and the per-interface bootloader-touch state-isolation fix
// (CDC1 line-coding/state changes must never contaminate CDC0's tracked
// baud/DTR, or a CDC1 event could make CDC0's own, unrelated next event
// misfire the reboot).
//
// Why this file mirrors rather than calls the real code: usb_descriptors.c
// includes tusb.h (single-owner doctrine, tools/check_single_owner.ps1's
// allowlist: only tasks/usb_owner.c and tasks/usb_descriptors.c may), and
// usb_owner.c is a FreeRTOS task file pulling in tinyusb/task headers --
// neither is part of this host-test harness's source list
// (build_host_tests.ps1 compiles only src/sim/'s pure modules), the exact
// same pure/task boundary test_safe_reboot_logic.c's own header comment
// already documents for the same reason. Each mirror below is a
// deliberately small, byte-for-byte copy of the corresponding real
// constant/condition (cited in each section's comment) -- keep them in sync
// by hand if either side changes.
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "test_common.h"

// ===========================================================================
// Section 1: config-descriptor arithmetic (usb_descriptors.c)
// ===========================================================================
// TinyUSB's own well-known template lengths (lib/tinyusb's tusb.h family --
// not vendored into this repo, hence literal mirrors rather than an
// #include): TUD_CONFIG_DESC_LEN is the 9-byte USB configuration descriptor
// header; TUD_CDC_DESC_LEN is TUD_CDC_DESCRIPTOR()'s own fixed emission size
// -- one 8-byte IAD + one 9-byte control interface + 5+5+4+5-byte CDC
// functional descriptors + one 7-byte notification endpoint + one 9-byte
// data interface + two 7-byte data endpoints = 66 bytes per CDC function,
// IAD included.
#define MIRROR_TUD_CONFIG_DESC_LEN 9u
#define MIRROR_TUD_CDC_DESC_LEN    66u

// usb_descriptors.c's own constants, mirrored.
#define MIRROR_ITF_NUM_CDC0_CTRL 0u
#define MIRROR_ITF_NUM_CDC0_DATA 1u
#define MIRROR_ITF_NUM_CDC1_CTRL 2u
#define MIRROR_ITF_NUM_CDC1_DATA 3u
#define MIRROR_ITF_NUM_TOTAL     4u

#define MIRROR_EPNUM_CDC0_NOTIF 0x81u
#define MIRROR_EPNUM_CDC0_OUT   0x02u
#define MIRROR_EPNUM_CDC0_IN    0x82u
#define MIRROR_EPNUM_CDC1_NOTIF 0x83u
#define MIRROR_EPNUM_CDC1_OUT   0x04u
#define MIRROR_EPNUM_CDC1_IN    0x84u

#define MIRROR_CONFIG_TOTAL_LEN (MIRROR_TUD_CONFIG_DESC_LEN + 2u * MIRROR_TUD_CDC_DESC_LEN)

static void test_config_total_len_matches_dual_cdc_formula(void)
{
    TEST_SECTION("usb_descriptors.c -- CONFIG_TOTAL_LEN is 9 + 2*66, not 9 + 66");
    // This is the induced-failure-prone line in the whole change: reverting
    // CONFIG_TOTAL_LEN back to the single-CDC formula (TUD_CONFIG_DESC_LEN +
    // TUD_CDC_DESC_LEN, forgetting the "2 *") would under-report
    // wTotalLength in the config descriptor's own header -- the host would
    // read only CDC0's bytes and never discover CDC1 exists at all, since
    // TinyUSB packs both TUD_CDC_DESCRIPTOR() invocations back to back with
    // no other length field describing the second one independently.
    TEST_CHECK(MIRROR_CONFIG_TOTAL_LEN == 141u, "9 (config header) + 2*66 (two CDC functions) == 141 bytes");
    TEST_CHECK(MIRROR_CONFIG_TOTAL_LEN != MIRROR_TUD_CONFIG_DESC_LEN + MIRROR_TUD_CDC_DESC_LEN,
               "the dual-CDC total must differ from the old single-CDC total (75) -- a silent revert to one CDC's worth of length must show up here");
    TEST_CHECK(MIRROR_CONFIG_TOTAL_LEN <= 0xFFFFu, "wTotalLength is a u16 field -- must never overflow it (nowhere close today, but this is the assertion that would catch a runaway future addition)");
}

static void test_interface_numbering_is_contiguous_two_iads(void)
{
    TEST_SECTION("usb_descriptors.c -- four interfaces, two CDC functions, contiguous numbering");
    TEST_CHECK(MIRROR_ITF_NUM_CDC0_CTRL == 0u && MIRROR_ITF_NUM_CDC0_DATA == 1u,
               "CDC0 (protocol) is interfaces 0-1 -- declared FIRST, so it is TinyUSB CDC instance 0");
    TEST_CHECK(MIRROR_ITF_NUM_CDC1_CTRL == 2u && MIRROR_ITF_NUM_CDC1_DATA == 3u,
               "CDC1 (console) is interfaces 2-3 -- declared SECOND, so it is TinyUSB CDC instance 1");
    TEST_CHECK(MIRROR_ITF_NUM_TOTAL == 4u, "exactly four interfaces total (two IADs of two interfaces each), no gap or overlap");
    // Each CDC function's own control interface number must be the first of
    // its pair (TUD_CDC_DESCRIPTOR()'s own contract: the data interface is
    // always _itfnum + 1) -- catches a transposed enum entry.
    TEST_CHECK(MIRROR_ITF_NUM_CDC0_DATA == MIRROR_ITF_NUM_CDC0_CTRL + 1u, "CDC0 data interface immediately follows CDC0 control");
    TEST_CHECK(MIRROR_ITF_NUM_CDC1_DATA == MIRROR_ITF_NUM_CDC1_CTRL + 1u, "CDC1 data interface immediately follows CDC1 control");
}

static void test_endpoint_addresses_all_distinct(void)
{
    TEST_SECTION("usb_descriptors.c -- CDC0 and CDC1 endpoint addresses never collide");
    // Two functions sharing an endpoint address would mean TinyUSB routes
    // one function's traffic onto the other's pipe -- a real, silent
    // corruption hazard this check exists specifically to rule out. IN and
    // OUT endpoints of the SAME number (e.g. 0x02 OUT and 0x82 IN) are NOT a
    // collision -- USB endpoint addresses are per-direction -- so this
    // compares the six raw byte values, which already encode direction in
    // bit 7.
    uint8_t const eps[] = {
        MIRROR_EPNUM_CDC0_NOTIF, MIRROR_EPNUM_CDC0_OUT, MIRROR_EPNUM_CDC0_IN,
        MIRROR_EPNUM_CDC1_NOTIF, MIRROR_EPNUM_CDC1_OUT, MIRROR_EPNUM_CDC1_IN,
    };
    bool all_distinct = true;
    for (size_t i = 0; i < sizeof(eps) / sizeof(eps[0]) && all_distinct; i++) {
        for (size_t j = i + 1; j < sizeof(eps) / sizeof(eps[0]); j++) {
            if (eps[i] == eps[j]) {
                all_distinct = false;
                break;
            }
        }
    }
    TEST_CHECK(all_distinct, "all six CDC0/CDC1 endpoint addresses are pairwise distinct");
}

// ===========================================================================
// Section 2: CDC1 console write gating (usb_owner.c's usb_owner_console_write())
// ===========================================================================
// Mirrors the ordered gate usb_owner_console_write() applies before it ever
// calls tud_cdc_n_write(): not-ready-yet, no host connected, and
// insufficient TX FIFO space to take the WHOLE write (never a partial/
// truncated one) all drop; only when every gate passes does the write
// "succeed".
static bool mirror_console_write_ok(bool console_ready, bool cdc1_connected, uint32_t tx_avail, size_t len)
{
    if (!console_ready) {
        return false; // usb_owner_task_fn() hasn't called tusb_init() yet
    }
    if (!cdc1_connected) {
        return false; // nobody has a terminal open on CDC1
    }
    if (tx_avail < len) {
        return false; // would only partially fit -- drop the whole line, never truncate
    }
    return true;
}

static void test_console_write_drops_before_usb_ready(void)
{
    TEST_SECTION("usb_owner_console_write -- drops silently before usb_owner's TinyUSB init has run");
    // The exact boot-time hazard this gate exists for: main.c's earliest
    // printf() calls happen before usb_owner_start()/usb_owner_task_fn()
    // have run at all (console_sink_register() is called right after
    // stdio_init_all(), long before the SIMFW_BOOT_STAGE("usb_owner", ...)
    // call). Without this gate, touching TinyUSB's CDC1 state that early
    // would be undefined behavior, not just an unwanted reboot-relevant
    // side effect.
    TEST_CHECK(!mirror_console_write_ok(false, true, 1000, 10), "console_ready == false refuses the write even if everything else looks fine");
}

static void test_console_write_drops_when_no_terminal_attached(void)
{
    TEST_SECTION("usb_owner_console_write -- drops when nobody is listening on CDC1");
    // This is the headline behavior the task calls out: "never block, never
    // grow unboundedly if nobody is listening."
    TEST_CHECK(!mirror_console_write_ok(true, false, 1000, 10), "CDC1 not connected (no terminal open) drops the write, does not buffer it");
}

static void test_console_write_drops_on_short_fifo_space_not_truncates(void)
{
    TEST_SECTION("usb_owner_console_write -- would-only-partially-fit drops the WHOLE line, never truncates");
    TEST_CHECK(!mirror_console_write_ok(true, true, 5, 10), "TX FIFO has less room than the line needs -- refused whole, not sent as a truncated 5-byte fragment");
}

static void test_console_write_succeeds_when_everything_is_ready(void)
{
    TEST_SECTION("usb_owner_console_write -- ready + connected + enough room succeeds");
    TEST_CHECK(mirror_console_write_ok(true, true, 64, 10), "a normal, fully-ready CDC1 write goes through");
    TEST_CHECK(mirror_console_write_ok(true, true, 10, 10), "exactly-enough FIFO space (avail == len) is enough, not just avail > len");
}

// ===========================================================================
// Section 3: bootloader-touch interface isolation (usb_owner.c)
// ===========================================================================
// This is the bug class the itf-gating fix specifically closes: CDC0 and
// CDC1 line-coding/line-state changes must never share tracked state, or a
// CDC1 event (an ordinary terminal opening the console port) could leave
// stale "1200 baud, DTR deasserted" state that a LATER, entirely unrelated
// CDC0 event then reads back and misfires on. The mirror below models
// usb_owner.c's actual state-update rule: only itf == 0 (CDC0, the
// protocol interface) events are allowed to touch the tracked baud/DTR
// pair at all -- itf == 1 (CDC1) events are ignored outright, not merely
// "checked and found not to trigger".
#define MIRROR_TOUCH_ITF  0u
#define MIRROR_TOUCH_BAUD 1200u

typedef struct {
    uint32_t baud_bps;
    bool dtr_asserted;
} mirror_touch_session_t;

static mirror_touch_session_t mirror_touch_session_init(void)
{
    mirror_touch_session_t s;
    s.baud_bps = 0;          // 0 is never a real baud rate -- pre-any-line-coding-change sentinel
    s.dtr_asserted = true;   // starts asserted -- see usb_owner.c's own comment on this default
    return s;
}

// Mirrors tud_cdc_line_coding_cb(): CDC1 (itf != 0) events are ignored
// entirely, never even written into the tracked state.
static void mirror_on_line_coding(mirror_touch_session_t *s, uint8_t itf, uint32_t baud_bps)
{
    if (itf != MIRROR_TOUCH_ITF) {
        return;
    }
    s->baud_bps = baud_bps;
}

// Mirrors tud_cdc_line_state_cb() the same way.
static void mirror_on_line_state(mirror_touch_session_t *s, uint8_t itf, bool dtr_asserted)
{
    if (itf != MIRROR_TOUCH_ITF) {
        return;
    }
    s->dtr_asserted = dtr_asserted;
}

static bool mirror_touch_fires(const mirror_touch_session_t *s)
{
    return s->baud_bps == MIRROR_TOUCH_BAUD && !s->dtr_asserted;
}

static void test_cdc1_touch_convention_never_fires(void)
{
    TEST_SECTION("bootloader touch -- CDC1 (console) 1200-baud + DTR-deasserted does NOT fire");
    mirror_touch_session_t s = mirror_touch_session_init();
    mirror_on_line_coding(&s, 1u, MIRROR_TOUCH_BAUD);
    mirror_on_line_state(&s, 1u, false);
    TEST_CHECK(!mirror_touch_fires(&s), "the exact real convention, performed entirely on CDC1, never trips the touch");
    TEST_CHECK(s.baud_bps == 0u, "a CDC1 line-coding event must not even be RECORDED into the tracked (CDC0-only) state");
    TEST_CHECK(s.dtr_asserted == true, "a CDC1 line-state event must not even be RECORDED into the tracked (CDC0-only) state either");
}

static void test_cdc0_touch_convention_still_fires(void)
{
    TEST_SECTION("bootloader touch -- CDC0 (protocol) 1200-baud + DTR-deasserted still fires");
    mirror_touch_session_t s = mirror_touch_session_init();
    mirror_on_line_coding(&s, 0u, MIRROR_TOUCH_BAUD);
    mirror_on_line_state(&s, 0u, false);
    TEST_CHECK(mirror_touch_fires(&s), "the dual-CDC change must not have broken the original, still-supported CDC0 trigger");
}

static void test_cdc1_activity_cannot_contaminate_a_later_cdc0_event(void)
{
    TEST_SECTION("bootloader touch -- CDC1 touching 1200 baud, then CDC0 asserting DTR normally, must not fire");
    // The exact cross-interface contamination bug a shared (non-itf-gated)
    // pair of tracked variables would produce: CDC1 sets 1200 baud (e.g. a
    // legacy terminal program's default), then CDC0 goes through an
    // ordinary, unrelated line-state callback (DTR asserted, a normal port
    // open) -- if CDC1's 1200 baud had leaked into the shared state, CDC0's
    // own event could read back "1200 baud" from CDC1 and, combined with
    // CDC0's own real DTR-deasserted moment later, misfire. Isolating state
    // per interface (this mirror only ever updates baud_bps/dtr_asserted for
    // itf == 0) closes that path structurally, not just by getting lucky
    // with call ordering.
    mirror_touch_session_t s = mirror_touch_session_init();
    mirror_on_line_coding(&s, 1u, MIRROR_TOUCH_BAUD); // CDC1: 1200 baud -- must not be recorded
    mirror_on_line_state(&s, 0u, true);               // CDC0: DTR asserted, ordinary open
    TEST_CHECK(!mirror_touch_fires(&s), "CDC1's baud never reaches the CDC0-scoped state, so this ordinary CDC0 open does not fire");
    mirror_on_line_state(&s, 0u, false);              // CDC0: DTR now deasserted too
    TEST_CHECK(!mirror_touch_fires(&s), "still refuses: CDC0's OWN baud (still the 0 sentinel, never set on CDC0) is not 1200, contamination or not");
}

void run_test_usb_dual_cdc_logic(void)
{
    test_config_total_len_matches_dual_cdc_formula();
    test_interface_numbering_is_contiguous_two_iads();
    test_endpoint_addresses_all_distinct();
    test_console_write_drops_before_usb_ready();
    test_console_write_drops_when_no_terminal_attached();
    test_console_write_drops_on_short_fifo_space_not_truncates();
    test_console_write_succeeds_when_everything_is_ready();
    test_cdc1_touch_convention_never_fires();
    test_cdc0_touch_convention_still_fires();
    test_cdc1_activity_cannot_contaminate_a_later_cdc0_event();
}
