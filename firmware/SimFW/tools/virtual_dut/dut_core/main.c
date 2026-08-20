// main.c -- virtual_dut's "guard core": a tiny stdio batch harness around
// the REAL, UNMODIFIED SaftyFW guard/relay-state-machine code:
//
//   firmware/SaftyFW/src/safety_guards.c   (safety_guards_reset/_tick/
//                                            _deciding_threshold_c)
//   firmware/SaftyFW/src/tasks/relay_grace.c (relay_grace_tick/
//                                              relay_trip_transition)
//
// compiled verbatim (see build_host.ps1) exactly the way
// firmware/SaftyFW/test/build_host_tests.ps1 already proves safety_guards.c
// is pure, portable C (~452 checks). Neither of those two .c files is
// modified, wrapped, or reimplemented by anything in this directory --
// see this directory's README.md for the precise file-by-file accounting.
//
// What this file is NOT: safety_core.c (firmware/SaftyFW/src/tasks/
// safety_core.c), the real FreeRTOS task that normally drives these calls,
// cannot be host-compiled (FreeRTOS.h, pico/time.h, thermo_task.h,
// discrete_task.h, relay_owner.h's task, reboot_announce.h, watchdog_task.h
// are all RTOS/hardware-shaped). This file is a single-threaded, stdio-
// driven REPLACEMENT for that task's *outer loop* only -- it builds the
// same safety_guard_input_t shape safety_core_build_input() builds, calls
// the same two library functions safety_core_task()'s loop calls, and
// mirrors relay_owner_task()'s GRACE-timer/TRIP-latch bookkeeping via
// relay_grace.c the same way relay_owner.c itself does. See this
// directory's README.md, "Faithfulness to safety_core_build_input()", for
// the field-by-field justification of every value set below -- in
// particular, several fields below are left permanently false/zero not out
// of laziness but because that is EXACTLY what safety_core_build_input()'s
// own C99 struct literal does today (fields it never names are
// zero-initialized): context_valid, main_fault_asserted, link_up,
// relay_deenergized, any_current_present, relay_commanded_recently/
// _continuously, sample_counter_advancing, zone_count are every one of them
// omitted from that literal, hence false/0 in real, current SaftyFW --
// this file reproduces that, not a "safe stub" of our own invention.
//
// Line protocol on stdin/stdout (text, one line in -> one line out; chosen
// so a Python orchestrator that already owns the real benchproto/TCP
// conversation with virtual_simfw does not also have to speak benchproto in
// C -- see ../run_dut_scenarios.py):
//
//   RESET
//     -> resets guard state (safety_guards_reset), resets relay_owner's
//        state machine to GRACE with a fresh grace-elapsed counter of 0
//        (relay_owner_task()'s own "GRACE begins the instant the task
//        starts running" -- this is that instant). Reply: "OK".
//
//   TICK <regs_hex32> <estop:0|1>
//     -> decodes the 16-byte MAX31856 register image (32 hex chars,
//        firmware/SimFW/docs/PROTOCOL.md sec 5.2's TC_GET_REGS `regs` field)
//        via max31856_decode_regs() (see that file's own header comment for
//        why this decode step exists and is not itself guard logic), builds
//        one tick's safety_guard_input_t exactly as documented above, calls
//        safety_guards_tick() once (dt_s fixed at SAFTYFW_PERIOD_SAFETY_
//        CORE_MS/1000 = 0.1s, matching safety_core.c's own #define), then
//        applies relay_grace_tick()'s GRACE->ARMED timer check and, if this
//        tick just tripped, relay_trip_transition()'s unconditional latch --
//        the same two calls relay_owner_task()'s loop body makes every
//        iteration. Reply is one space-separated line:
//
//          <is_tripped:0|1> <reason:int> <s5_warn:0|1> <s12_warn:0|1>
//          <s4_warn:0|1> <s10_warn:0|1> <s13_warn:0|1> <trip_ineffective:0|1>
//          <relay_state:0..3> <energized:0|1> <deciding_threshold_c:float>
//          <tc_valid:0|1> <tc_c:float> <cj_c:float> <fault_bits:int>
//
//        `reason` is safety_trip_t's numeric value (safety_guards.h) --
//        0 == SAFETY_TRIP_NONE. `relay_state` is relay_owner_state_t's
//        numeric value (0 INIT / 1 GRACE / 2 ARMED / 3 TRIPPED) --
//        INIT never actually appears here (RESET jumps straight to GRACE,
//        matching relay_owner_task()'s own behavior).
//
// Any unrecognized line gets "ERR unknown command" and is otherwise
// ignored (does not crash, does not tick).
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "safety_guards.h"
#include "relay_grace.h" // relay_owner_state_t, relay_grace_tick, relay_trip_transition
#include "max31856_decode.h"

// safety_core.c's own #define (firmware/SaftyFW/src/tasks/safety_core.c):
// SAFTYFW_PERIOD_SAFETY_CORE_MS 100 -- see this file's header comment and
// this directory's README for why the value is hand-copied here rather than
// #included: task_priorities.h (the real header that defines it) also
// defines FreeRTOS-priority macros this host build has no FreeRTOS to
// resolve against, so it cannot be #included as-is. The numeric value
// itself is trivially re-verified with one grep any time SaftyFW changes it
// (see README.md's "keep in sync" note); it is not a guessed number.
#define SAFTYFW_PERIOD_SAFETY_CORE_MS 100u

// relay_owner.c's own #define (firmware/SaftyFW/src/tasks/relay_owner.c):
// SAFTYFW_STARTUP_GRACE_MS 60000u. Same "hand-copied, not included" reasoning
// as above -- relay_owner.c itself is not host-compilable (FreeRTOS/gpio.h).
#define SAFTYFW_STARTUP_GRACE_MS 60000u

static safety_guard_cfg_t s_cfg; // zero-initialized: "nothing commissioned"
                                  // -- see README.md, this is real current
                                  // safety_core.c's own state, not a
                                  // simplification.
static safety_guard_state_t s_state;
static relay_owner_state_t s_relay_state;
static uint32_t s_grace_elapsed_ticks;
static bool s_energized; // never set true anywhere in this file -- see
                          // README.md's "K4 is never energized in current
                          // SaftyFW" finding: no real caller of
                          // relay_owner_command_energize() exists yet
                          // (Phase 7, link_task/GUI), so this stays false
                          // for the same reason it stays false on real
                          // hardware today.

static void do_reset(void)
{
    safety_guards_reset(&s_state);
    s_relay_state = RELAY_OWNER_STATE_GRACE; // relay_owner_task()'s first statement
    s_grace_elapsed_ticks = 0;
    s_energized = false;
    printf("OK\n");
    fflush(stdout);
}

static bool hex_nibble(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}

static bool parse_regs_hex(const char *hex, uint8_t out[16])
{
    if (strlen(hex) != 32) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        uint8_t hi, lo;
        if (!hex_nibble(hex[i * 2], &hi) || !hex_nibble(hex[i * 2 + 1], &lo)) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void do_tick(const char *regs_hex, int estop)
{
    uint8_t regs[16];
    if (!parse_regs_hex(regs_hex, regs)) {
        printf("ERR bad regs hex\n");
        fflush(stdout);
        return;
    }

    max31856_decoded_t decoded;
    max31856_decode_regs(regs, &decoded);

    // safety_core_build_input(), field for field (see this file's header
    // comment). tc_valid is unconditionally true here: this harness has no
    // real SPI transaction to fail (virtual_simfw's own documented "no real
    // SPI bytes ever flow" -- TC_GET_REGS always succeeds), matching
    // thermo_task_get_snapshot()'s contract that a successful transaction
    // sets valid=true and lets the SR fault bits speak for themselves
    // (already applied by max31856_decode_regs() above).
    safety_guard_input_t in;
    memset(&in, 0, sizeof(in)); // every field this literal doesn't name below
                                  // stays false/0/NaN-never -- exactly what
                                  // safety_core_build_input()'s own C99
                                  // struct-literal semantics do for the
                                  // fields IT doesn't name (context_valid,
                                  // main_fault_asserted, link_up,
                                  // relay_deenergized, any_current_present,
                                  // relay_commanded_recently/_continuously,
                                  // sample_counter_advancing, zone_count).
    in.tc_valid = true;
    in.tc_c = decoded.tc_c;
    in.cj_c = decoded.cj_c;
    in.fault_bits = decoded.fault_bits;
    in.spi_failed = false;
    in.estop_pressed = (estop != 0);
    in.heat_commanded = false; // real safety_core.c's own hardcoded value (Phase 6 TODO)
    in.reboot_grace_active = false; // no ANNOUNCE_REBOOT source in this fixture
    in.dt_s = (float)SAFTYFW_PERIOD_SAFETY_CORE_MS / 1000.0f;

    bool newly_tripped = safety_guards_tick(&s_state, &s_cfg, &in);

    if (newly_tripped) {
        s_relay_state = relay_trip_transition(s_relay_state);
        s_energized = false; // relay_owner_task()'s CMD_TRIP case drives GPIO6 low unconditionally
    } else {
        s_grace_elapsed_ticks++;
        uint32_t grace_ticks = SAFTYFW_STARTUP_GRACE_MS / SAFTYFW_PERIOD_SAFETY_CORE_MS;
        s_relay_state = relay_grace_tick(s_relay_state, s_grace_elapsed_ticks, grace_ticks);
    }

    float deciding = safety_guards_deciding_threshold_c(s_state.reason, &s_cfg);

    printf("%d %d %d %d %d %d %d %d %d %d %.6f %d %.6f %.6f %d\n",
           s_state.is_tripped ? 1 : 0,
           (int)s_state.reason,
           s_state.s5_warn ? 1 : 0,
           s_state.s12_warn ? 1 : 0,
           s_state.s4_warn ? 1 : 0,
           s_state.s10_warn ? 1 : 0,
           s_state.s13_warn ? 1 : 0,
           s_state.trip_ineffective ? 1 : 0,
           (int)s_relay_state,
           s_energized ? 1 : 0,
           isnan(deciding) ? 0.0 : (double)deciding,
           in.tc_valid ? 1 : 0,
           isnan(in.tc_c) ? 0.0 : (double)in.tc_c,
           isnan(in.cj_c) ? 0.0 : (double)in.cj_c,
           (int)in.fault_bits);
    fflush(stdout);
}

int main(void)
{
    char line[256];
    do_reset(); // boot state: relay_owner_task()'s GRACE-entry-on-start
                 // happens the instant this process is ready, same reasoning
                 // relay_owner.c's own header comment gives for real hardware.

    while (fgets(line, sizeof(line), stdin) != NULL) {
        // strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (strncmp(line, "RESET", 5) == 0) {
            do_reset();
        } else if (strncmp(line, "TICK ", 5) == 0) {
            char regs_hex[64];
            int estop = 0;
            if (sscanf(line + 5, "%63s %d", regs_hex, &estop) == 2) {
                do_tick(regs_hex, estop);
            } else {
                printf("ERR bad TICK args\n");
                fflush(stdout);
            }
        } else if (strlen(line) == 0) {
            // ignore blank lines
        } else {
            printf("ERR unknown command\n");
            fflush(stdout);
        }
    }
    return 0;
}
