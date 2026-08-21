// main.c -- virtual_dut's "guard core": a tiny stdio batch harness around
// the REAL, UNMODIFIED SaftyFW guard/relay-state-machine code:
//
//   firmware/SaftyFW/src/safety_guards.c   (safety_guards_reset/_tick/
//                                            _deciding_threshold_c)
//   firmware/SaftyFW/src/tasks/relay_grace.c (relay_grace_tick/
//                                              relay_trip_transition)
//   firmware/SaftyFW/src/snapshots.h        (context_reduce_zones/
//                                              current_any_present -- the two
//                                              pure reduction helpers
//                                              safety_core_build_input()
//                                              itself calls, compiled here
//                                              from the real header rather
//                                              than reimplemented; see
//                                              "Faithfulness" below)
//
// compiled verbatim (see build_host.ps1) exactly the way
// firmware/SaftyFW/test/build_host_tests.ps1 already proves safety_guards.c
// is pure, portable C (~549 checks). None of those files is modified,
// wrapped, or reimplemented by anything in this directory -- see this
// directory's README.md for the precise file-by-file accounting.
//
// What this file is NOT: safety_core.c (firmware/SaftyFW/src/tasks/
// safety_core.c), the real FreeRTOS task that normally drives these calls,
// cannot be host-compiled (FreeRTOS.h, pico/time.h, thermo_task.h,
// discrete_task.h, relay_owner.h's task, reboot_announce.h, watchdog_task.h
// are all RTOS/hardware-shaped). This file is a single-threaded, stdio-
// driven REPLACEMENT for that task's *outer loop* only -- it builds the
// same safety_guard_input_t safety_core_build_input() builds, using the same
// pure helpers and the same arithmetic where that arithmetic is
// FreeRTOS-free, calls the same two library functions safety_core_task()'s
// loop calls, and mirrors relay_owner_task()'s GRACE-timer/TRIP-latch
// bookkeeping via relay_grace.c the same way relay_owner.c itself does.
//
// == Faithfulness to safety_core_build_input() (rewritten 2026-08-20) ==
//
// Commit f304392 wired safety_core_build_input() to link_task's published
// context, link liveness and relay-correlation facts, and to current_task's
// ADC snapshot. Before that commit, this file's hardcoded zero/false values
// for context_valid/link_up/any_current_present/relay_commanded_* WERE the
// real function's behavior (its C99 struct literal simply never named those
// fields). After it, they are not -- so this file is now driven from real
// fixture data instead, one field per real producer:
//
//   context_snapshot_t   <- the orchestrator's per-poll context frame,
//                            derived from virtual_simfw telemetry (the same
//                            physical facts a real ESP's SAFETY_CMD_PUSH_
//                            CONTEXT would carry: per-zone measured_c,
//                            active/relay-on flags, relay masks). See
//                            ../run_dut_scenarios.py for how each field is
//                            sourced, including the one field the fixture
//                            genuinely has no producer for (setpoint_c,
//                            sent as NaN -- "unknown", never a guessed
//                            number; NaN correctly makes S2's own
//                            `tc_c > max_setpoint + margin` false rather
//                            than inventing a ceiling for it to trip on).
//   current_snapshot_t   <- the fixture's per-zone CT amps.
//   main_fault_asserted  <- the TICK line's optional trailing <main_fault>
//                            field, standing in for discrete_task_main_
//                            fault(): safety_core_build_input() now names
//                            this field (`.main_fault_asserted =
//                            discrete_task_main_fault()`, immediately after
//                            `.estop_pressed`), so leaving it hardcoded
//                            false here would no longer mirror the real
//                            function. The fixture SENSES the Fault line
//                            rather than driving the Pico's GPIO10, so no
//                            current scenario asserts it -- but the input is
//                            now reachable from the wire, exactly like
//                            <estop>, instead of being unreachable in C.
//   relay_deenergized    <- this file's own s_energized, inverted:
//                            safety_core_build_input() now names the field as
//                            `!relay_owner_is_energized()`, and s_energized
//                            here already IS relay_owner.c's s_energized,
//                            modelled by apply_energize_request() /
//                            relay_trip_transition() below. So this needs no
//                            new TICK field and no protocol change -- the
//                            producer was already mirrored, only its inverse
//                            was never consumed. Note the consequence: with
//                            no scenario issuing ENABLE, s_energized is false
//                            for every run, so this now reads TRUE
//                            throughout, which is truthful (K4 really is open
//                            in these runs) and still cannot escalate S9 on
//                            its own -- S9 also needs a latched trip AND
//                            any_current_present past trip_verify_s.
//   link_up              <- the fixture telemetry stream's own liveness,
//                            standing in for link_task_link_up()'s
//                            "CRC-valid frame within LINK_UP_RECENCY_MS".
//   context age /        <- carried on the wire from the orchestrator, which
//   degraded_no_context     owns the only clock here; this file applies the
//                            same CONTEXT_MAX_AGE_MS test safety_core.c does.
//   heat_commanded        <- safety_core_build_input() now names this field
//                            (`.heat_commanded = any_current_present`), so
//                            this file sets it from the SAME already-computed
//                            in.any_current_present value, right after that
//                            field is assigned -- one producer, two
//                            consumers, matching the real function exactly.
//                            Deliberately NOT derived from context (relay_
//                            commanded_recently/_continuously): safety_
//                            guards.h's own header comment requires this
//                            field stay link-independent, and current_
//                            any_present already is (its own ADC snapshot,
//                            no context_valid gate either in this file or
//                            in safety_core.c).
//
// Fields still fixed here, each matching real, current safety_core.c
// exactly (NOT a harness simplification -- re-read safety_core.c before
// changing any of these):
//   sample_counter_advancing = false (safety_core.c deliberately leaves it
//                                      false: S13 needs a commissioned
//                                      borrowed_zone_index that does not
//                                      exist anywhere in the codebase, and
//                                      cfg->tc_source's OWN_J7 default keeps
//                                      S13 dormant regardless)
//   reboot_grace_active      = false (no SAFETY_CMD_ANNOUNCE_REBOOT source in
//                                      this fixture -- there is no ESP here)
//   energized                = only ever set by an explicit ENABLE command
//                                      (see below); nothing in this fixture
//                                      issues one today, mirroring the fact
//                                      that SAFETY_CMD_REQUEST_ENABLE is an
//                                      operator/PC action (KilnFW's
//                                      uart_bridge.c) that no scenario models
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
//   TICK <regs_hex32> <estop> <link_up> <ctx_present> <ctx_degraded>
//        <ctx_age_ms> <relay_now_mask> <relay_recent_mask>
//        <relay_on_continuous_ms> <amps0> <amps1> <amps2> <zone_count>
//        [<zone_flags> <setpoint_c> <measured_c> <sample_counter>] * zone_count
//        [<main_fault> [<dt_ms>]]
//
//        <main_fault> (S6a, discrete_task_main_fault()'s already-debounced
//        active-low GPIO10 level: 1 == asserted) is OPTIONAL and trailing,
//        defaulting to 0 when absent, so an orchestrator written against the
//        pre-S6a line format keeps working unchanged. It is last because the
//        zone block ahead of it is variable-length.
//
//        <dt_ms> (OPTIONAL, after <main_fault>; defaults to
//        SAFTYFW_PERIOD_SAFETY_CORE_MS = 100) is how much simulated time this
//        ONE tick represents, i.e. safety_guard_input_t.dt_s * 1000. It exists
//        because this harness's caller observes the fixture at the fixture's
//        own telemetry rate, not at the real 100 ms safety-core rate, and the
//        two are not the same number (see ../run_dut_scenarios.py's
//        "one tick per observed sample" comment and README.md Finding 0/8).
//        Real safety_core_task() calls safety_guards_tick() once per REAL
//        sample with dt_s = its own period; this field keeps that invariant --
//        one call per observed sample -- while letting dt_s carry the true
//        elapsed time, instead of the old scheme where the caller replayed a
//        single sample across many 100 ms ticks and thereby manufactured
//        consecutive-read streaks (S1/S5 count *reads*, not seconds) that the
//        fixture never produced. Nothing in safety_guards.c treats dt_s as
//        anything other than an additive time increment (every use is
//        `state->..._elapsed_s += in->dt_s`), so a larger dt_s is exact for
//        the time-based bars and simply honest about how many reads were
//        actually seen.
//
//        -> decodes the 16-byte MAX31856 register image (32 hex chars,
//        firmware/SimFW/docs/PROTOCOL.md sec 5.2's TC_GET_REGS `regs` field)
//        via max31856_decode_regs() (see that file's own header comment for
//        why this decode step exists and is not itself guard logic), builds
//        one tick's safety_guard_input_t exactly as documented above, calls
//        safety_guards_tick() once (dt_s = <dt_ms>/1000, defaulting to
//        SAFTYFW_PERIOD_SAFETY_CORE_MS/1000 = 0.1s, safety_core.c's own
//        #define, when the caller does not say otherwise), then
//        applies relay_grace_tick()'s GRACE->ARMED timer check and, if this
//        tick just tripped, relay_trip_transition()'s unconditional latch --
//        the same two calls relay_owner_task()'s loop body makes every
//        iteration. Reply is one space-separated line:
//
//          <is_tripped:0|1> <reason:int> <s5_warn:0|1> <s12_warn:0|1>
//          <s4_warn:0|1> <s10_warn:0|1> <s13_warn:0|1> <trip_ineffective:0|1>
//          <relay_state:0..3> <energized:0|1> <deciding_threshold_c:float>
//          <tc_valid:0|1> <tc_c:float> <cj_c:float> <fault_bits:int>
//          <context_valid:0|1> <eff_zone_count:int> <any_current_present:0|1>
//          <link_up:0|1>
//
//        `reason` is safety_trip_t's numeric value (safety_guards.h) --
//        0 == SAFETY_TRIP_NONE. `relay_state` is relay_owner_state_t's
//        numeric value (0 INIT / 1 GRACE / 2 ARMED / 3 TRIPPED) --
//        INIT never actually appears here (RESET jumps straight to GRACE,
//        matching relay_owner_task()'s own behavior). The last four fields
//        are the derived context/current/link facts, echoed back so the
//        orchestrator can report guard reachability without re-deriving
//        them in Python (they are decided here, by the real helpers).
//
//   ENABLE <0|1>
//     -> mirrors link_task.c's SAFETY_CMD_REQUEST_ENABLE (0x02) decoder ->
//        safety_core_request_enable() -> relay_owner_command_energize():
//        refused outright while TRIPPED, accepted-but-never-applied during
//        GRACE, and only actually energizes while ARMED (relay_owner.c's
//        own command loop). Reply: "OK <energized:0|1>". Provided so a
//        future scenario CAN model the operator/PC enable step; nothing in
//        the current fixture sends it -- see the header note above.
//
// Any unrecognized line gets "ERR unknown command" and is otherwise
// ignored (does not crash, does not tick).
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "safety_guards.h"
#include "relay_grace.h" // relay_owner_state_t, relay_grace_tick, relay_trip_transition
#include "snapshots.h"   // context_snapshot_t / current_snapshot_t and the two REAL,
                          // unmodified pure helpers safety_core_build_input() calls:
                          // context_reduce_zones() and current_any_present(). Compiled
                          // from the real SaftyFW header, not copied -- that is the
                          // entire point of this harness.
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

// safety_core.c's own three local constants, hand-copied for the same reason
// (that file cannot be included at all). CONTEXT_MAX_AGE_MS is
// SAFETY_MODEL.md sec 5 rule 2's "stale context is no context" default;
// the other two mirror safety_guards.c's own effective_f() substitutions,
// which safety_core.c has to duplicate too (see its own comment on why).
#define CONTEXT_MAX_AGE_MS                        5000u
#define SAFETY_CORE_I_PRESENT_A_DEFAULT           2.0f
#define SAFETY_CORE_CORRELATION_WINDOW_S_DEFAULT  150.0f

static safety_guard_cfg_t s_cfg; // zero-initialized: "nothing commissioned"
                                  // -- see README.md, this is real current
                                  // safety_core.c's own state, not a
                                  // simplification.
static safety_guard_state_t s_state;
static relay_owner_state_t s_relay_state;
static uint32_t s_grace_elapsed_ms;   // GRACE timer, in simulated ms (see <dt_ms>)
static bool s_energized;      // relay_owner.c's s_energized: only ENABLE can set it
static bool s_enable_wanted;  // relay_owner's "command accepted during GRACE but not
                               // applied until ARMED" behavior needs the request kept

static void do_reset(void)
{
    safety_guards_reset(&s_state);
    s_relay_state = RELAY_OWNER_STATE_GRACE; // relay_owner_task()'s first statement
    s_grace_elapsed_ms = 0;
    s_energized = false;
    s_enable_wanted = false;
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

// --- TICK argument tokenizer -------------------------------------------------
// The TICK line is variable-arity (zone blocks), so it is tokenized rather
// than sscanf'd against a fixed format string. Tokens are whitespace-
// separated; the caller pulls them positionally.
#define MAX_TICK_TOKENS 64

typedef struct {
    char *tok[MAX_TICK_TOKENS];
    int   count;
    int   next;
    bool  bad;
} tokens_t;

static void tokens_split(tokens_t *t, char *s)
{
    memset(t, 0, sizeof(*t));
    char *p = s;
    while (*p != '\0' && t->count < MAX_TICK_TOKENS) {
        while (*p == ' ' || *p == '\t') { p++; }
        if (*p == '\0') { break; }
        t->tok[t->count++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') { p++; }
        if (*p != '\0') { *p++ = '\0'; }
    }
}

static const char *tok_str(tokens_t *t)
{
    if (t->next >= t->count) { t->bad = true; return ""; }
    return t->tok[t->next++];
}

static long tok_long(tokens_t *t)
{
    const char *s = tok_str(t);
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || (end != NULL && *end != '\0')) { t->bad = true; }
    return v;
}

static float tok_float(tokens_t *t)
{
    const char *s = tok_str(t);
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || (end != NULL && *end != '\0')) { t->bad = true; }
    return (float)v;
}

// Mirrors relay_owner_task()'s RELAY_OWNER_CMD_ENERGIZE case plus
// relay_owner_command_energize()'s own TRIPPED fast-refusal. Called from the
// ENABLE command and re-applied every tick, because relay_owner's GRACE
// branch keeps the request without applying it until the GRACE timer expires.
static bool apply_energize_request(void)
{
    if (s_relay_state == RELAY_OWNER_STATE_TRIPPED) {
        s_enable_wanted = false; // refused entirely -- latched
        s_energized = false;
        return false;
    }
    if (s_relay_state == RELAY_OWNER_STATE_ARMED) {
        s_energized = s_enable_wanted;
    } else {
        s_energized = false; // GRACE: accepted/tracked, never actually energizes
    }
    return s_energized;
}

static void do_tick(tokens_t *t)
{
    uint8_t regs[16];
    const char *regs_hex = tok_str(t);
    if (t->bad || !parse_regs_hex(regs_hex, regs)) {
        printf("ERR bad regs hex\n");
        fflush(stdout);
        return;
    }

    int      estop        = (int)tok_long(t);
    int      link_up      = (int)tok_long(t);
    int      ctx_present  = (int)tok_long(t);
    int      ctx_degraded = (int)tok_long(t);
    uint32_t ctx_age_ms   = (uint32_t)tok_long(t);
    uint32_t now_mask     = (uint32_t)tok_long(t);
    uint32_t recent_mask  = (uint32_t)tok_long(t);
    uint32_t relay_on_ms  = (uint32_t)tok_long(t);

    current_snapshot_t current;
    memset(&current, 0, sizeof(current));
    current.amps[0] = tok_float(t);
    current.amps[1] = tok_float(t);
    current.amps[2] = tok_float(t);

    context_snapshot_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.valid           = (ctx_present != 0);
    ctx.relay_now_mask  = (uint8_t)(now_mask & 0xFFu);
    ctx.relay_recent_mask = (uint8_t)(recent_mask & 0xFFu);

    long nz = tok_long(t);
    if (nz < 0) { nz = 0; }
    if (nz > (long)CONTEXT_SNAPSHOT_MAX_ZONES) { nz = (long)CONTEXT_SNAPSHOT_MAX_ZONES; }
    ctx.zone_count = (uint8_t)nz;
    for (long i = 0; i < nz; i++) {
        ctx.zones[i].zone_index     = (uint8_t)i;
        ctx.zones[i].flags          = (uint8_t)tok_long(t);
        ctx.zones[i].setpoint_c     = tok_float(t);
        ctx.zones[i].measured_c     = tok_float(t);
        ctx.zones[i].sample_counter = (uint8_t)tok_long(t);
    }

    // Optional trailing <main_fault> (see the TICK line format above): read
    // only when the caller actually supplied it, so an orchestrator still
    // sending the pre-S6a line shape does not fall into "ERR bad TICK args".
    int main_fault = 0;
    if (t->next < t->count) {
        main_fault = (int)tok_long(t);
    }

    // Optional trailing <dt_ms> (see the TICK line format above). Absent ==
    // the real safety-core period, which is what every caller written before
    // the "one tick per observed sample" change sent implicitly.
    long dt_ms = (long)SAFTYFW_PERIOD_SAFETY_CORE_MS;
    if (t->next < t->count) {
        dt_ms = tok_long(t);
        if (dt_ms <= 0) { t->bad = true; }
    }

    if (t->bad) {
        printf("ERR bad TICK args\n");
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
    memset(&in, 0, sizeof(in)); // every field safety_core_build_input()'s own
                                  // C99 struct literal names is assigned
                                  // explicitly below; this only covers the
                                  // ones it deliberately leaves at zero.
    in.tc_valid = true;
    in.tc_c = decoded.tc_c;
    in.cj_c = decoded.cj_c;
    in.fault_bits = decoded.fault_bits;
    in.spi_failed = false;
    in.estop_pressed = (estop != 0);
    // S6a, sitting immediately after .estop_pressed exactly as it now does in
    // safety_core_build_input(). discrete_task already inverted the active-low
    // GPIO10 and debounced it 200ms, so the wire carries the same "1 ==
    // asserted" level this field expects -- no inversion here either.
    in.main_fault_asserted = (main_fault != 0);
    // heat_commanded is set below, right after any_current_present is
    // computed -- safety_core_build_input() now wires it from that same
    // value (`.heat_commanded = any_current_present`), so this file follows
    // suit rather than assigning it here ahead of its producer existing.
    in.reboot_grace_active = false; // no ANNOUNCE_REBOOT source in this fixture
    in.dt_s = (float)dt_ms / 1000.0f;

    // "Stale context is no context" -- safety_core_build_input()'s own
    // three-way collapse (never received / stale / DEGRADED_NO_CONTEXT).
    bool context_valid = false;
    if (ctx_present && ctx.valid && !ctx_degraded) {
        context_valid = ctx_age_ms < CONTEXT_MAX_AGE_MS;
    }
    in.context_valid = context_valid;

    // S2/S10's zone reduction, by the REAL helper (snapshots.h) -- same
    // call, same arguments, same context_valid gate as safety_core.c.
    uint8_t zone_count = 0;
    float   max_zone_setpoint_c = 0.0f;
    float   nearest_zone_measured_c = 0.0f;
    if (context_valid) {
        context_reduce_zones(&ctx, in.tc_c, &zone_count, &max_zone_setpoint_c,
                              &nearest_zone_measured_c);
    }
    in.zone_count = zone_count;
    in.max_zone_setpoint_c = max_zone_setpoint_c;
    in.nearest_zone_measured_c = nearest_zone_measured_c;

    // S3/S4/S6b's current-presence fact, by the REAL helper (snapshots.h) --
    // unconditional, not context-gated, exactly as in safety_core.c.
    float i_present_a = (s_cfg.i_present_a > 0.0f) ? s_cfg.i_present_a
                                                    : SAFETY_CORE_I_PRESENT_A_DEFAULT;
    in.any_current_present = current_any_present(&current, i_present_a);

    // S11's heat_commanded qualifier, mirroring safety_core_build_input()'s
    // `.heat_commanded = any_current_present` -- the SAME value, computed
    // once, fed to both fields, exactly as the real function now does. See
    // this file's header comment for why this and not a context-derived
    // fact is the right producer (link-independence).
    in.heat_commanded = in.any_current_present;

    // S3/S4's relay-correlation facts -- both collapse to false whenever
    // context_valid is false, same as safety_core.c.
    if (context_valid) {
        in.relay_commanded_recently = ctx.relay_recent_mask != 0u;
        float correlation_window_s = (s_cfg.correlation_window_s > 0.0f)
                                          ? s_cfg.correlation_window_s
                                          : SAFETY_CORE_CORRELATION_WINDOW_S_DEFAULT;
        in.relay_commanded_continuously =
            (float)relay_on_ms >= correlation_window_s * 1000.0f;
    }

    // S13: deliberately false, exactly as safety_core.c leaves it -- a
    // commissioned borrowed_zone_index does not exist anywhere in the
    // codebase, and guessing one here would invent a commissioning
    // decision. cfg->tc_source's OWN_J7 default keeps S13 dormant anyway.
    in.sample_counter_advancing = false;

    in.link_up = (link_up != 0);

    // S9, mirroring safety_core_build_input()'s `.relay_deenergized =
    // !relay_owner_is_energized()`. Read HERE, before safety_guards_tick(),
    // from the value left by the previous tick's apply_energize_request() --
    // the real safety_core reads relay_owner's published level at the top of
    // its own tick the same way. Same single negation, same reason: this
    // file's s_energized is the affirmative, the guard field is the negative.
    in.relay_deenergized = !s_energized;

    bool newly_tripped = safety_guards_tick(&s_state, &s_cfg, &in);

    if (newly_tripped) {
        s_relay_state = relay_trip_transition(s_relay_state);
        s_energized = false; // relay_owner_task()'s CMD_TRIP case drives GPIO6 low unconditionally
        s_enable_wanted = false;
    } else {
        // relay_owner_task() counts its GRACE timer in whole
        // SAFTYFW_PERIOD_SAFETY_CORE_MS ticks because that is exactly how
        // often it runs. Here one call can represent more (or less) than one
        // period of simulated time -- see <dt_ms> above -- so the elapsed
        // TIME is accumulated and converted to that same tick unit right at
        // the call, rather than counting calls. relay_grace_tick()'s own
        // comparison is untouched (it still gets "ticks elapsed" vs "ticks
        // required"); this only stops a call from meaning a fixed 100 ms when
        // it did not.
        s_grace_elapsed_ms += (uint32_t)dt_ms;
        uint32_t grace_ticks = SAFTYFW_STARTUP_GRACE_MS / SAFTYFW_PERIOD_SAFETY_CORE_MS;
        uint32_t elapsed_ticks = s_grace_elapsed_ms / SAFTYFW_PERIOD_SAFETY_CORE_MS;
        s_relay_state = relay_grace_tick(s_relay_state, elapsed_ticks, grace_ticks);
        (void)apply_energize_request(); // GRACE -> ARMED makes a held request take effect
    }

    float deciding = safety_guards_deciding_threshold_c(s_state.reason, &s_cfg);

    printf("%d %d %d %d %d %d %d %d %d %d %.6f %d %.6f %.6f %d %d %d %d %d\n",
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
           (int)in.fault_bits,
           in.context_valid ? 1 : 0,
           (int)in.zone_count,
           in.any_current_present ? 1 : 0,
           in.link_up ? 1 : 0);
    fflush(stdout);
}

int main(void)
{
    char line[1024];
    do_reset(); // boot state: relay_owner_task()'s GRACE-entry-on-start
                 // happens the instant this process is ready, same reasoning
                 // relay_owner.c's own header comment gives for real hardware.

    while (fgets(line, sizeof(line), stdin) != NULL) {
        // strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (strcmp(line, "RESET") == 0) {
            do_reset();
        } else if (strncmp(line, "TICK ", 5) == 0) {
            tokens_t t;
            tokens_split(&t, line + 5);
            do_tick(&t);
        } else if (strncmp(line, "ENABLE ", 7) == 0) {
            tokens_t t;
            tokens_split(&t, line + 7);
            long want = tok_long(&t);
            if (t.bad) {
                printf("ERR bad ENABLE args\n");
            } else {
                s_enable_wanted = (want != 0);
                printf("OK %d\n", apply_energize_request() ? 1 : 0);
            }
            fflush(stdout);
        } else if (strlen(line) == 0) {
            // ignore blank lines
        } else {
            printf("ERR unknown command\n");
            fflush(stdout);
        }
    }
    return 0;
}
