// safety_core.c -- Phase 4/5: assembles a safety_guard_input_t every tick,
// calls the pure evaluator, and on a new trip commands relay_owner and
// latches the reason. The GRACE/ARMED/TRIPPED state machine itself lives in
// relay_owner.c (Phase 5) -- this task only ever issues energize/trip/clear
// commands and never tracks that state on its own, per
// docs/ARCHITECTURE.md's "safety_core commands relay_owner" arrow.
//
// THE ONE RULE THAT MATTERS (docs/ARCHITECTURE.md section 2): no #include of
// any link or uart header, ever, in this file. Nothing below reaches for one,
// and it should stay that way -- a frozen ESP must not be able to hang this
// task, and the only way to make that a structural property rather than a
// promise is for this compilation unit to be physically incapable of calling
// into the link path. tools/check_isolation.ps1 greps for this.
// discrete_task_estop_pressed() and relay_owner.h/boot_reason.h are fine to
// include here -- neither is link-shaped, and check_isolation.ps1 only
// greps for "uart"/"link" in an #include line, not for these. Same for
// current_task.h (its own ADC0/1/2, independent of the link entirely -- see
// SAFETY_MODEL.md section 3) and snapshots.h, which is where link_task.c's
// context/link-liveness getters are (re-)declared specifically so this file
// can call them without a "link"-named #include -- see that header's own
// doc comment on that section, and reboot_announce.h's comment two lines up
// for the precedent this follows.
#include "safety_core.h"

#include <math.h>
#include <stdio.h> // snprintf -- clear-trip outcome log line, see safety_core_task()
#include <string.h> // memcpy -- exact float bit-pattern capture, see s_clear_trip_pre_tc_c_bits

#include "FreeRTOS.h"
#include "queue.h" // s_clear_trip_queue -- see safety_core_request_clear_trip()'s doc
                    // comment in safety_core.h for why this is a queue rather than a
                    // direct call; "queue" is not a link/uart-named header, fine for
                    // check_isolation.ps1
#include "task.h"

#include "pico/time.h" // to_ms_since_boot(get_absolute_time()) -- hardware timing, not
                        // link/uart-shaped, fine for check_isolation.ps1 (same header
                        // link_task.c uses for its own uptime_ms fields, so
                        // safety_core_get_trip_event()'s out_uptime_ms shares that clock base)

#include "task_priorities.h"
#include "watchdog_task.h"

#include "boot_reason.h"
#include "clock_health.h" // 2026-08-23 stalled-get_absolute_time() detector, see its own header comment
#include "clear_trip_diag.h" // 2026-08-23 round 4, CLEAR_TRIP crash checkpoints that survive the reboot -- see its own header comment
#include "config_store.h" // safety_tc_installed (param 0x0211) -- read directly here, every tick,
                           // rather than routed through s_guard_cfg: s_guard_cfg is not yet wired to
                           // config_store at all (see this file's own header comment on that Phase 9
                           // gap), and adding this one field to that pipeline would either have to
                           // fix the whole gap or add a second, inconsistent loading path. config_store_
                           // get_full_record() is the same "read one specific field directly, right where
                           // it's used" pattern config_store_get_tc_type()/link_task.c's own config
                           // reads already establish -- no "link"/"uart" in this header's name, so this
                           // stays legal under this file's own isolation rule above.
#include "current_task.h" // any_current_present (S3/S4/S6b/S11) -- current_task is a real, independent
                           // Phase-6 producer (its own ADC, not link-derived), not link/uart-shaped,
                           // fine for check_isolation.ps1
#include "discrete_task.h"
#include "log_task.h" // clear-trip outcome logging, see safety_core_task()'s queue drain below --
                       // not link/uart-shaped (drains into link_task's TX ring on the OTHER side of
                       // that boundary, same as every task's log_task_log() call), fine for
                       // check_isolation.ps1
#include "reboot_announce.h" // SAFETY_CMD_ANNOUNCE_REBOOT (0x18) grace-window fact for
                              // S6b -- see that header's own doc comment for why this,
                              // and not link_task.h, is the safe way to cross the
                              // link->safety_core boundary tools/check_isolation.ps1 enforces.
#include "relay_grace.h" // relay_energize_allowed_during_update() -- see safety_core_request_enable()
#include "relay_owner.h"
#include "safety_guards.h"
#include "snapshots.h" // context_snapshot_t + link_task_get_context_snapshot()/
                        // link_task_get_degraded_no_context()/link_task_link_up()/
                        // link_task_get_relay_on_continuous_ms() -- declared here, not in
                        // link_task.h, specifically so this file can see them without an
                        // #include naming "link"/"uart"; see snapshots.h's own doc comment
                        // on that section for the full reasoning, and reboot_announce.h's
                        // comment just above for the precedent this follows.
#include "thermo_task.h"
#include "update_task.h" // update_task_transfer_active() -- NOT link/uart-shaped (no "link"/"uart"
                          // substring, tools/check_isolation.ps1's actual grep target), and this
                          // file already has precedent for reaching into a sibling task for one
                          // narrow fact (current_task.h/thermo_task.h above) rather than the link
                          // itself -- see safety_core_request_enable() for the one call site.

// 2026-08-23, the CLEAR_TRIP-reboots-the-Pico investigation, final finding:
// this was configMINIMAL_STACK_SIZE (256 words / 1KB) unmultiplied -- the
// tightest stack budget of any task in this firmware, and the one task
// whose call graph includes safety_guards.c's trip(), which vsnprintf()s
// TWO %.1f (float, promoted to double through varargs) conversions plus a
// %u into a 96-byte buffer (safety_guards.h's detail[96]) every time a guard
// newly trips. newlib's (non-nano) floating-point vfprintf path is
// documented as stack-hungry on Cortex-M0+ -- several hundred bytes for its
// own internal frame alone, on top of safety_core_task()'s own already-
// substantial locals (safety_core_build_input() returns a
// safety_guard_input_t by value, held live in safety_core_task()'s frame
// for the whole tick). 1KB total was not enough headroom for that call, and
// vApplicationStackOverflowHook() (main.c) halts with interrupts disabled
// rather than resetting -- so the actual failure was silent: the 1s hardware
// watchdog (unfed, because nothing runs to feed it) reboots the board about
// a second later, indistinguishable from any other reboot from the ESP's
// side, landing exactly at the tick a guard's trip_mask should have flipped.
// S5's blind-thermocouple trip is the one guard whose grace window
// (blind_grace_s, 60s default) is long enough, and reliable enough now that
// the TIMER_DBGPAUSE fix (main.c) actually lets the clock run, to make this
// reproduce on a clean, predictable ~50-60s cycle -- but EVERY guard's
// trip() call is the identical vsnprintf shape (see safety_guards.c's own
// trip() call sites), so this was never S5-specific; S5 was just the guard
// most likely to actually fire first under a normal bench boot with no
// thermocouple fitted.
//
// link_task.c's own LINK_TASK_STACK_WORDS carries the matching lesson from
// this exact codebase already (see that file's own comment, dated the same
// day): configMINIMAL_STACK_SIZE*3 overflowed on real hardware and had to
// go to *6.
//
// 2026-08-23 FOLLOW-UP, *4 was not enough either: *4 fixed the PERIODIC trip
// path (safety_core_task() -> safety_guards_tick() -> trip() -> vsnprintf(),
// proven on hardware: S5 now latches and stays latched) but CLEAR_TRIP still
// rebooted the Pico every time, on a DIFFERENT, DEEPER path through the
// exact same vsnprintf call:
//   safety_core_task()'s clear-trip drain block (own locals: clear_trip_
//   token/was_tripped/try_clear_result/outcome) -> safety_guards_try_clear()
//   -- ONE MORE STACK FRAME than the periodic path ever adds -- which, when
//   guard_condition_still_immediate() does NOT short-circuit it, falls
//   through to safety_guards_clear() + safety_guards_tick() + trip() +
//   vsnprintf(), the identical float-formatting call, now one frame deeper
//   than the path that only just barely fit in *4.
// On top of that, the periodic trip path does NOT call log_task_log() at
// all yet (see safety_core_task()'s own "TODO (Phase 8): step 4... trip
// itself is still not implemented here" comment, a few lines above the
// clear-trip drain block) -- but the CLEAR path does, twice over: this
// file's own outcome snprintf() (char msg[64], no floats) immediately
// followed by log_task_log() itself, which allocates ITS OWN ~100-byte
// log_entry_t (level+len+char msg[96]) plus a second, independent snprintf
// call -- log_task.c:234, `entry.msg`, no floats but a real stack cost on
// WHATEVER task calls log_task_log(), not something this file's earlier
// stack-budget comment accounted for because the periodic trip path had
// literally never reached it.
//
// So the clear path is proven deeper by TWO independent, additive causes,
// not one -- an extra wrapper frame around the same expensive vsnprintf,
// and an entirely separate log_task_log() call the periodic path doesn't
// exercise. *6 (matching link_task's own already-established multiplier for
// this exact class of problem) is the response: comfortable margin over the
// now-understood deepest path, not just the shallowest one that happened to
// get exercised first. The exact right number is still not measured on
// hardware (see this file's own "stack high-water" discussion removed
// 2026-08-23 from watchdog_task.c for distorting checkin timing) --
// test/test_safety_core_stack_budget.c enforces the floor as a source-text
// check so a future edit cannot silently shrink this back toward either
// value that has already caused a live reboot.
#define SAFETY_CORE_STACK_WORDS   (configMINIMAL_STACK_SIZE * 6)

// KilnFW/TODO.md's "SAFETY_CMD_ANNOUNCE_REBOOT sent before the ESP reboots"
// line: how long S6b's trip stays suppressed after the most recent
// ANNOUNCE_REBOOT frame. Chosen the same way SAFETY_LINK_STALE_MS/SAFETY_
// LINK_FIRING_ABORT_SILENCE_MS (firmware/KilnFW/App/drivers/safety_link.h)
// were -- a reasonable software timeout with generous margin, not a measured
// physical constant (unlike S8's rate-of-rise threshold, which SAFETY_MODEL.md
// explicitly forbids guessing because it depends on this kiln's actual mass
// and element power). An ESP32-S3 OTA self-reboot needs to: reset, run the
// ROM/second-stage bootloader, bring up FreeRTOS and Wi-Fi, and reach
// safety_link's first PUSH_CONTEXT poll cycle -- plausibly a few seconds on a
// clean boot. 20s gives roughly 2x margin over that without getting anywhere
// close to link_dead_hard_s's own 120s unconditional backstop (so a genuinely
// stuck reboot still gets caught by the hard backstop shortly after this
// window closes, not held open indefinitely).
#define REBOOT_GRACE_WINDOW_MS 20000u

// SAFETY_MODEL.md section 5 rule 2: "Stale context is no context. Older than
// context_max_age_s (default 5s, i.e. 10 poll periods) and the context-
// consuming guards go inactive, not pessimistic." Not a
// safety_guard_cfg_t field (context_valid is a caller-computed bool, not a
// per-tick raw value the guard module itself ages -- see
// safety_guard_input_t's own doc comment on context_valid), so this lives
// here as a local constant, same status as REBOOT_GRACE_WINDOW_MS just
// above: a documented software default from SAFETY_MODEL.md, not an
// invented one.
#define CONTEXT_MAX_AGE_MS 5000u

// Mirrors safety_guards.c's own CORRELATION_WINDOW_S_DEFAULT (safety_
// guards.h's cfg field doc comment: "0 -> correlation_window_s=150.0s") --
// duplicated here, not exported from safety_guards.c, because this file
// needs the SAME "0 means not configured, substitute the default"
// substitution safety_guards.c already does internally in order to compute
// relay_commanded_continuously as an INPUT fact, one tick before safety_
// guards_tick() itself runs. This is the already-documented SAFETY_MODEL.md
// default, not a new one invented for this file -- if safety_guards.c's own
// default ever changes, this must change with it (same risk any duplicated
// constant carries; there is no third home to put a single copy in without
// violating the isolation boundary this whole file exists to keep, see
// snapshots.h's own doc comment on why the shared pure helpers live there
// instead of in safety_guards.c or link_frame.c).
//
// 2026-08-24: this block used to also define SAFETY_CORE_I_PRESENT_A_
// DEFAULT for the same reason -- current_any_present() took i_present_a as
// a parameter and this file had to substitute the same default safety_
// guards.c's effective_f(cfg->i_present_a, ...) would. Removed: current_
// any_present() now reads current_snapshot_t.present[n], which current_
// sense.c already computed against config_store's i_present_a (with its
// own substitution, current_task_reload_cal()) -- this file no longer
// touches i_present_a at all for that fact.
#define SAFETY_CORE_CORRELATION_WINDOW_S_DEFAULT 150.0f

static TaskHandle_t s_task_handle = NULL;

// Thresholds. Zero-initialized (static storage, so this is implicit, but
// spelled out for the reader): abs_max_temp_c has no default at all
// (safety_guards.h's own doc comment, SAFETY_MODEL.md section 4, S1) so 0
// here correctly means "not commissioned, S1 never trips" rather than a
// silently-substituted ceiling. S5/S11/S12's thresholds DO have real
// defaults per that same doc convention, and safety_guards.c substitutes
// them itself whenever the corresponding cfg field is 0 -- confirmed by
// reading safety_guards.c's effective_f()/effective_u16() call sites, not
// guessed. Nothing here needs to set them.
// TODO (Phase 9, config_store): load real commissioned values (abs_max_temp_c
// above all) from flash once config_store.c exists. Until then this cfg is
// permanently the conservative "nothing commissioned" state -- with ONE
// exception: firing_max_valid/firing_max_c ARE written every tick now, by
// safety_core_build_input() below, from link_task's SET_FIRING_CEILING RX
// state. That is deliberately not a config-store gap the same way
// abs_max_temp_c is: safety_guards.h's own doc comment on those two fields is
// explicit they are "context, not config" -- see safety_core_build_input()'s
// own comment at the point they are set for the full reasoning. Note also
// that as long as abs_max_temp_c stays uncommissioned (0), S1 never trips at
// all regardless of firing_max_valid -- the min() clamp in safety_guards.c
// only ever tightens a real abs_max_temp_c, it does not manufacture one.
static safety_guard_cfg_t s_guard_cfg;
static safety_guard_state_t s_guard_state;

// CLEAR_TRIP queue -- see safety_core_request_clear_trip()'s doc comment in
// safety_core.h for the full "why a queue, not a direct call" reasoning.
// Small and non-blocking, same convention as relay_owner.h's own command
// queue: a backlog here means safety_core_task's 100ms tick is not draining
// it, which is a bug worth surfacing as a dropped request (xQueueSend(...,
// 0) returning false), never a reason to block the caller. 2, not 1: a
// second CLEAR_TRIP arriving in the same 100ms window as the first (e.g. a
// GUI double-click) is a real, if unlikely, case worth not silently
// dropping; there is no legitimate reason for a third to queue up before
// the tick drains the first two. The queue holds a one-byte token with no
// payload -- link_task has already validated trip_mask
// (link_frame_decide_clear_trip()) before ever calling
// safety_core_request_clear_trip(), so there is nothing left to carry.
#define SAFETY_CORE_CLEAR_TRIP_QUEUE_LEN 2
static QueueHandle_t s_clear_trip_queue = NULL;

// Single-writer statics backing safety_core_get_clear_trip_stats() --
// s_clear_trip_requested written only inside safety_core_request_clear_trip()
// (called only from link_task, so effectively single-core-writer too, same
// as link_task.c's own s_context_frames_ok/bad); s_clear_trip_processed/
// s_clear_trip_last_outcome written only inside safety_core_task() below.
static uint32_t s_clear_trip_requested = 0;
static uint32_t s_clear_trip_processed = 0;
static safety_clear_trip_outcome_t s_clear_trip_last_outcome = SAFETY_CLEAR_TRIP_OUTCOME_NONE;

// Trip-event capture (Frame D, SAFETY_CMD_TRIP_EVENT) -- written only from
// safety_core_task() the instant safety_guards_tick() reports newly_tripped
// (this task's own thread), read from any task via
// safety_core_get_trip_event(). Same single-writer/plain-read reasoning
// link_task.c already documents for its own s_context_frames_ok/bad and
// s_status_tx_ok_count: one task ever writes these, so no lock is needed for
// a handful of scalars this small, and a torn read here is no worse than
// this codebase's existing pattern for the same class of counter.
// s_trip_seq == 0 means "no trip yet this boot" (matches
// safety_core_get_trip_event()'s documented return-false contract); the
// first real trip makes it 1, wrapping uint8_t thereafter -- 255 trips in one
// boot without a reboot in between is not a case this needs to handle
// specially, wrapping back to 0 just means the 256th event reports the same
// sequence number the "never tripped" state would have, which is an
// acceptable, undocumented edge this shares with any other wrapping counter
// in this codebase (e.g. link_task's own s_msg_index).
static uint8_t s_trip_seq = 0;
static safety_trip_t s_trip_reason = SAFETY_TRIP_NONE;
static uint32_t s_trip_uptime_ms = 0;
static float s_trip_tc_c = 0.0f; // meaningless while s_trip_seq == 0 -- the getter
static float s_trip_deciding_threshold = 0.0f; // never reports these until a real trip sets them

// 2026-08-23, the DIAG-content-frozen investigation: single-writer state for
// clock_health_observe(), touched only from safety_core_build_input(), which
// itself has exactly one call site (safety_core_task()'s own loop, paced by
// vTaskDelayUntil() -- see that function below) -- same single-writer/no-lock
// reasoning as every other plain static in this file. Zero-initialised,
// matching clock_health.h's own "zero-initialise before first use" contract.
static clock_health_state_t s_clock_health;

// 2026-08-23, round 3 of the CLEAR_TRIP investigation: the differential
// (S6a clear accepted and survives; S5 clear refused and crashes) rules out
// stack depth for THIS branch specifically -- the refusal path
// (safety_guards_try_clear() -> guard_condition_still_immediate() ->
// s5_bad_read_now()) is structurally SHALLOWER than the accept path
// (safety_guards_try_clear() -> safety_guards_clear() +
// safety_guards_tick(), the same big function the periodic trip path
// already proved survives at *6), yet it is the one that crashes. Read
// exhaustively; found no C-level bug (no null deref, no array indexing, no
// uninitialised field -- s5_bad_read_now()'s in->spi_failed/tc_valid/tc_c/
// fault_bits are all plain, valid reads on a pointer alive for the whole
// tick). One fact worth having pinned down precisely, though: once a guard
// LATCHES, safety_guards_tick()'s very first line (`if (state->is_tripped)`)
// short-circuits to the S9-only branch and never reaches S5's own
// s5_bad_read_now() call again -- so guard_condition_still_immediate()'s S5
// case is not just "the same check running one frame deeper", it is the
// FIRST time s5_bad_read_now()/isnan(in->tc_c) has run via THIS call chain
// since before the trip latched. These SWD-readable statics latch every
// input guard_condition_still_immediate() is about to be handed, and a
// before/after call counter, so the coordinator can read exactly what `in`
// contained and confirm whether the call ever returned -- written here
// (safety_core.c, hardware-linked) rather than in safety_guards.c (pure,
// host-tested, deliberately free of anything hardware-facing) even though
// the values describe safety_guards.c's own call, same split link_task.c's
// diagnostic checkpoints already use for calls into CommonFW's codecs.
static volatile uint32_t s_clear_trip_pre_call_count = 0;  // incremented immediately BEFORE safety_guards_try_clear()
static volatile uint32_t s_clear_trip_post_call_count = 0; // incremented immediately AFTER it returns -- pre > post after a crash means it never came back
static volatile uint8_t  s_clear_trip_pre_reason = 0;       // state->reason at the moment of the call (safety_trip_t)
static volatile uint8_t  s_clear_trip_pre_tc_valid = 0;
static volatile uint32_t s_clear_trip_pre_tc_c_bits = 0;    // raw IEEE-754 bit pattern of in->tc_c (memcpy, not a cast) -- exact NaN payload visible, not just "is it NaN"
static volatile uint8_t  s_clear_trip_pre_fault_bits = 0;
static volatile uint8_t  s_clear_trip_pre_spi_failed = 0;

// Builds one tick's worth of safety_guard_input_t from the current live
// snapshots -- factored out of safety_core_task()'s loop so
// safety_core_request_clear_trip() below can build the exact same fresh
// input for its re-evaluation retick (safety_guards_try_clear()) instead of
// duplicating this construction.
// Defined below, next to safety_core.h's outcome enum -- forward-declared
// here because safety_core_task()'s log line above uses it before that point.
static const char *clear_trip_outcome_str(safety_clear_trip_outcome_t outcome);

static safety_guard_input_t safety_core_build_input(void)
{
    // Phase 3: thermo_task now owns a real MAX31856 (max31856.c, ported
    // from KilnFW) and publishes a thermo_snapshot_t every ~DRDY edge
    // (or marks itself invalid on DRDY silence -- see thermo_task.c).
    // Pulled here, never pushed -- thermo_task_get_snapshot() is a
    // mutex-guarded copy, matching ARCHITECTURE.md section 2's
    // "safety_core pulls, nothing pushes into it" rule. A snapshot that
    // was never published (thermo_task not started, or called before its
    // first read) comes back tc_valid = false / NaN via
    // thermo_task_get_snapshot()'s own documented failure contract, so
    // the fallback here needs no special-casing beyond the struct's
    // default field values.
    //
    // heat_commanded is now wired to any_current_present (computed a few
    // lines below, alongside S3/S4/S6b's own use of it) -- SAFETY_MODEL.md
    // section 4's S11 formula spells this out literally: "current flowing OR
    // heat commanded during that window", and section 4's prose adds "a
    // genuinely static value ... for ten minutes, while energy is going in,
    // does not happen in a real thermal system." Current actually flowing
    // IS energy going in, so it alone honestly answers "do you know heat is
    // happening?" with "yes" -- no ESP claim required.
    //
    // Deliberately NOT relay_commanded_recently/_continuously, even though
    // those also mean roughly "heat commanded": both are context-derived
    // (ctx.relay_recent_mask / link_task_get_relay_on_continuous_ms()), and
    // safety_guards.h's own header comment (line ~53) is explicit that this
    // struct "has no link-derived field of any kind -- heat_commanded is a
    // plain caller-supplied fact, not a context read." Wiring a context
    // field here would make S11 silently go dormant under
    // DEGRADED_NO_CONTEXT, contradicting SAFETY_MODEL.md's own S11/S13 audit
    // note (section 6, "S11 reads neither [link_up nor context_valid] --
    // three disjoint facts with no cross-reads") and the guard's place on
    // the "keeps running with authority over K4 even when the main
    // controller is an unknown quantity" list (section 6). any_current_
    // present is a real, independent Phase 6 producer (its own ADC0/1/2),
    // exactly like S6b's own unconditional (non-context-gated) use of it
    // just below.
    //
    // Nuisance-trip check: does NOT reintroduce the S6b-class failure
    // (tripping ~120s into every idle boot, fixed by f304392) because
    // current_any_present is false on an idle kiln -- current only flows
    // when an element is actually being driven, so a cold, idle kiln still
    // satisfies "heat_commanded == false" and S11 stays dormant exactly as
    // SAFETY_MODEL.md section 4 requires ("a cold, idle kiln legitimately
    // sits at a constant reading for hours"). A real soak with the SSR duty-
    // cycling also does not nuisance-trip: S11 compares in->tc_c bit-for-bit
    // across ticks (state->s11_last_c), and a real 19-bit MAX31856 reading
    // on a live thermal system does not repeat exactly, soak or not -- the
    // window resets on the very next tick's noise. The only way to hold
    // in->tc_c bit-identical for the full 600s default while current flows
    // is a genuinely stuck reading, which is exactly the fault this guard
    // exists to catch.
    thermo_snapshot_t thermo;
    (void)thermo_task_get_snapshot(&thermo);

    // 2026-08-23, the DIAG-content-frozen investigation: ONE read of the
    // hardware clock per tick, shared by both staleness checks below (was
    // two independent to_ms_since_boot(get_absolute_time()) calls) and fed
    // to clock_health_observe() so a stalled clock is detected from this
    // exact read, not inferred. main.c now clears TIMER_DBGPAUSE at boot so
    // this should not happen on this hardware any more (see that fix's own
    // comment for the full mechanism) -- this check does not trust that it
    // took. dt_s is the same fixed, tick-paced constant used elsewhere in
    // this function (this function is single-call-site, see s_clock_health's
    // own declaration comment), so clock_health_observe() is comparing this
    // read against a genuinely reliable "how much wall-clock time really
    // passed."
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool clock_stalled =
        clock_health_observe(&s_clock_health, now_ms, (float)SAFTYFW_PERIOD_SAFETY_CORE_MS / 1000.0f);

    // SAFETY_CMD_ANNOUNCE_REBOOT grace window: this is the ONE place that
    // does the announced-timestamp-to-now arithmetic -- safety_guards.c
    // receives only the already-computed bool, per that struct field's own
    // doc comment. reboot_announce_get() returning false ("never announced
    // this boot") correctly collapses to reboot_grace_active = false, the
    // same "unknown means not-suppressed" default every other guard input
    // in this function already uses -- clock_stalled collapses to that same
    // default now too: a stalled clock cannot honestly answer "still within
    // the grace window", and the safe answer to an unanswerable question
    // here is "not suppressed", exactly like a boot that was never announced
    // at all.
    uint32_t announced_at_ms = 0;
    bool reboot_grace_active = false;
    if (!clock_stalled && reboot_announce_get(&announced_at_ms)) {
        // Unsigned subtraction is deliberately safe here even across a
        // pico/time wraparound: both operands come from the same
        // to_ms_since_boot() clock, so (now_ms - announced_at_ms) wraps
        // correctly in uint32_t arithmetic regardless of which side is
        // numerically larger, identical to how link_task.c already reasons
        // about its own tick-based age calculations.
        uint32_t age_ms = now_ms - announced_at_ms;
        reboot_grace_active = age_ms < REBOOT_GRACE_WINDOW_MS;
    }

    // --- Context from link_task (SAFETY_MODEL.md section 5) ----------------
    // Pulled here, never pushed -- same "safety_core pulls, link_task never
    // pushes into it" discipline as the thermo snapshot above. This was the
    // TODO (Phase 7) this function carried for every prior pass: context_
    // snapshot_t is now actually read, not just declared reachable.
    context_snapshot_t ctx;
    bool ctx_published = link_task_get_context_snapshot(&ctx);

    // "Stale context is no context" -- collapses never-received, stale, a
    // version mismatch (DEGRADED_NO_CONTEXT, SAFETY_MODEL.md section 6a:
    // "the context-dependent guards report as disabled"), AND now a stalled
    // local clock into the single context_valid fact safety_guards.c's own
    // doc comment expects: it does not, and must not, need to re-derive any
    // of these four conditions itself. The clock_stalled case matters on its
    // own merits, not just as belt-and-suspenders against main.c's fix: a
    // dead ESP-side context read as fresh forever (age_ms pinned near zero
    // because both operands come from the same stalled clock) is exactly the
    // failure "stale context is no context" exists to prevent, and it is
    // silent -- no counter, no log line, nothing -- without this check.
    bool context_valid = false;
    if (!clock_stalled && ctx_published && ctx.valid && !link_task_get_degraded_no_context()) {
        // Same wraparound-safe unsigned subtraction reasoning as
        // reboot_grace_active above -- both ctx.timestamp_ms and now_ms come
        // from the same to_ms_since_boot() clock.
        uint32_t age_ms = now_ms - ctx.timestamp_ms;
        context_valid = age_ms < CONTEXT_MAX_AGE_MS;
    }

    // S1's firing ceiling (SAFETY_CMD_SET_FIRING_CEILING, SAFETY_MODEL.md
    // section 4) -- pulled here, same "pulled, never pushed" discipline as
    // the context snapshot just above. link_task_get_firing_ceiling() already
    // applied its own bounds check at RX time (link_frame_ceiling_is_active(),
    // src/tasks/link_frame.c); this file only decides whether that fact is
    // still fresh enough to act on, via link_firing_ceiling_should_apply()
    // (snapshots.h) against the SAME context_valid every other context-
    // dependent input on this tick already uses -- link loss (or a stale/
    // never-received context) reverts S1 to abs_max_temp_c alone rather than
    // honouring a ceiling from a firing that may no longer be running. Written
    // into s_guard_cfg directly (not safety_guard_input_t) because that is
    // where safety_guards.h's own S1 implementation already reads
    // firing_max_valid/firing_max_c from -- see that struct's field comment
    // for why this context-shaped value lives in the config struct at all.
    float firing_max_c = 0.0f;
    bool have_firing_ceiling = link_task_get_firing_ceiling(&firing_max_c);
    s_guard_cfg.firing_max_valid =
        link_firing_ceiling_should_apply(have_firing_ceiling, context_valid);
    s_guard_cfg.firing_max_c = s_guard_cfg.firing_max_valid ? firing_max_c : 0.0f;

    // S2/S10's zone reduction -- context_reduce_zones() (snapshots.h) is the
    // pure, host-tested function that turns the raw per-zone array into the
    // two scalars those guards actually consume; see its own doc comment for
    // why *_zone_count is a count of ELIGIBLE (active + measured-valid)
    // zones, not ctx.zone_count itself. Only meaningful (and only computed
    // against a real tc_c) when context is valid -- when it is not, zeroing
    // zone_count below is what tells safety_guards.c "no active zones this
    // tick", the same inactive treatment as context_valid == false itself
    // (safety_guards.c resets S2/S10's own accumulators either way).
    uint8_t zone_count = 0;
    float   max_zone_setpoint_c = 0.0f;
    float   nearest_zone_measured_c = 0.0f;
    if (context_valid) {
        context_reduce_zones(&ctx, thermo.tc_c, &zone_count, &max_zone_setpoint_c,
                              &nearest_zone_measured_c);
    }

    // S3/S4/S6b/S11's current-presence fact -- current_task is a real, already-
    // running Phase 6 producer (its own ADC0/1/2, independent of the link;
    // SAFETY_MODEL.md section 3 -- "not an over/under-current guard", so this
    // is presence/absence only). current_any_present() (snapshots.h) applies
    // the SAME threshold substitution safety_guards.c's own effective_f()
    // does internally (see SAFETY_CORE_I_PRESENT_A_DEFAULT's declaration
    // comment) -- unconditional, not gated on context_valid, matching
    // safety_guards.c's own S6b block which reads any_current_present
    // regardless of context (SAFETY_MODEL.md section 2: "a quiet link with
    // current flowing" needs current sensing to work even with no ESP
    // context at all).
    current_snapshot_t current;
    current_task_get_snapshot(&current);
    // 2026-08-24: current_any_present() used to take i_present_a here and
    // recompute amps[n] > i_present_a itself (this file kept its own
    // SAFETY_CORE_I_PRESENT_A_DEFAULT copy of the substitution, removed
    // above). It now reads current.present[n], which current_sense.c
    // already computed -- via current_presence_policy.h, decoupled from
    // k_ct_v_per_a -- using config_store's i_present_a internally (see
    // current_task_reload_cal(), current_task.c).
    bool any_current_present = current_any_present(&current);

    // S3/S4's relay-correlation facts (SAFETY_MODEL.md section 4) -- both
    // derived from context, so both collapse to false whenever context_valid
    // is false, matching safety_guards.c's own "reset the accumulator, don't
    // just skip the check" discipline for the whole context-dependent block.
    bool relay_commanded_recently = false;
    bool relay_commanded_continuously = false;
    if (context_valid) {
        relay_commanded_recently = ctx.relay_recent_mask != 0u;
        float correlation_window_s = (s_guard_cfg.correlation_window_s > 0.0f)
                                          ? s_guard_cfg.correlation_window_s
                                          : SAFETY_CORE_CORRELATION_WINDOW_S_DEFAULT;
        uint32_t continuous_ms = link_task_get_relay_on_continuous_ms();
        relay_commanded_continuously = (float)continuous_ms >= correlation_window_s * 1000.0f;
    }

    // S13's sample_counter_advancing: deliberately left false. SAFETY_MODEL.md
    // section 3 requires a commissioned `borrowed_zone_index` (0..2) to say
    // WHICH context zone is "the" borrowed channel before this fact means
    // anything at all -- that field is documented (docs/CONFIG_REFERENCE.md,
    // SAFETY_MODEL.md section 3) but does not exist anywhere in this
    // codebase yet (grep-confirmed: no config_store field, no
    // safety_guard_cfg_t field), the same "Phase 9, not commissioned" gap
    // abs_max_temp_c is in for S1. Guessing a zone index (e.g. hardcoding
    // zone 0) would be inventing a commissioning decision this pass is not
    // authorised to make, and it would be silently WRONG the moment a real
    // installation's borrowed zone is not zone 0. This is harmless today
    // regardless: cfg.tc_source has no default and stays SAFETY_TC_SOURCE_
    // OWN_J7 (0) absent Phase 9 commissioning, and safety_guards.c's own S13
    // block is gated on `cfg->tc_source == BORROWED_ZONE || BOTH` before it
    // ever reads this field -- so S13 stays correctly dormant either way,
    // the same "config gap, not a missing-producer gap" category
    // GUARD_TEST_MATRIX.md section 6 already documents for S1. Revisit once
    // borrowed_zone_index is real.
    bool sample_counter_advancing = false;

    // safety_tc_installed (config param 0x0211) -- config_store_get_full_
    // record() is a cheap cached-copy read (config_store_flash.c), safe to
    // call every 100ms tick, same as config_store_get_tc_type()'s own use
    // elsewhere. Inverted into safety_tc_not_installed_declared here (see
    // that field's own doc comment in safety_guards.h for why the pure
    // module's input struct deliberately uses the opposite polarity from
    // the config field it is derived from).
    config_store_record_t cfg_rec;
    config_store_get_full_record(&cfg_rec);
    bool safety_tc_not_installed_declared = (cfg_rec.safety_tc_installed == 0u);

    return (safety_guard_input_t){
        .tc_valid = thermo.valid,
        .tc_c = thermo.tc_c,
        .cj_c = thermo.cj_c,
        .fault_bits = thermo.fault_bits,
        .spi_failed = thermo.spi_failed,
        .safety_tc_not_installed_declared = safety_tc_not_installed_declared,
        .estop_pressed = discrete_task_estop_pressed(),
        // S6a, the sibling of estop_pressed above and read exactly the same
        // way: discrete_task samples GPIO10 as `!gpio_get(...)` (active low)
        // and debounces it over 200ms, so a true here already MEANS
        // "mainFault is asserted", which is the sense safety_guards.c's S6a
        // block trips on directly. No inversion, and no extra conditioning,
        // belongs at this call site -- same division of labour as S7's.
        .main_fault_asserted = discrete_task_main_fault(),
        .heat_commanded = any_current_present,
        .context_valid = context_valid,
        .zone_count = zone_count,
        .max_zone_setpoint_c = max_zone_setpoint_c,
        .nearest_zone_measured_c = nearest_zone_measured_c,
        .any_current_present = any_current_present,
        .relay_commanded_recently = relay_commanded_recently,
        .relay_commanded_continuously = relay_commanded_continuously,
        .sample_counter_advancing = sample_counter_advancing,
        .link_up = link_task_link_up(),
        .reboot_grace_active = reboot_grace_active,
        // S9's "did the hardware actually respond". The INVERSE of
        // relay_owner_is_energized(), and the inversion belongs here rather
        // than in the producer: relay_owner.h publishes the affirmative
        // ("true only while GPIO6 is actually being driven high right now"),
        // safety_guards.h asks for the negative ("set by the caller once
        // relay_owner has actually de-energized K4"). safety_core_get_output_
        // status() below reads the SAME getter with NO `!` for its
        // out_relay_energized field, which is the cross-check that this `!`
        // is the right way round: the two call sites disagree by exactly one
        // negation because the two field names are exact opposites.
        //
        // Not gated on relay_owner's state, and deliberately so. K4 does read
        // de-energized for the whole 60s GRACE window at every boot (GRACE
        // refuses to drive GPIO6 high at all), but S9 cannot nuisance-trip on
        // that: safety_guards_tick() only evaluates S9 inside its
        // `if (state->is_tripped)` branch, so with no trip latched this field
        // is never even read -- it is not the S6b class of unconditional
        // free-running timer that f304392 had to undo. And if a trip DOES
        // land during GRACE, "K4 open yet current still flowing for
        // trip_verify_s" is a genuine welded contactor whether or not GRACE
        // is still running; suppressing it there would blind the one guard
        // whose entire job is to distrust the trip that just happened.
        .relay_deenergized = !relay_owner_is_energized(),
        .dt_s = (float)SAFTYFW_PERIOD_SAFETY_CORE_MS / 1000.0f,
    };
}

static void safety_core_task(void *arg)
{
    (void)arg;

    safety_guards_reset(&s_guard_state);

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFTYFW_PERIOD_SAFETY_CORE_MS));

        safety_guard_input_t input = safety_core_build_input();

        bool newly_tripped = safety_guards_tick(&s_guard_state, &s_guard_cfg, &input);
        if (newly_tripped) {
            // SAFETY_MODEL.md section 6's 4-step trip order: (1) relay_owner
            // de-energizes K4 first, before anything else, before logging;
            // (2) the trip reason (and a snapshot of the deciding inputs --
            // s_guard_state already holds one) is latched.
            // relay_owner_command_trip() posts to relay_owner's queue;
            // relay_owner runs at priority 7 (this task is priority 5), so
            // FreeRTOS preempts this task to service that command before
            // execution returns to the line below -- see relay_owner.h's
            // doc comment on relay_owner_command_trip() for why that
            // ordering is real and not just hoped for.
            (void)relay_owner_command_trip(s_guard_state.reason);
            boot_reason_latch_trip((uint32_t)s_guard_state.reason);

            // Capture Frame D's "at the instant of the trip" values right
            // here, same tick, before anything else runs -- LINK_PROTOCOL.md
            // sec 6: "half a second later the temperature has changed... the
            // evidence is gone." s_trip_seq incrementing LAST is deliberate:
            // safety_core_get_trip_event() is lock-free, so a reader must
            // never be able to observe the new seq before the fields it
            // describes are already written.
            s_trip_reason = s_guard_state.reason;
            s_trip_uptime_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
            s_trip_tc_c = input.tc_valid ? input.tc_c : NAN;
            s_trip_deciding_threshold = safety_guards_deciding_threshold_c(s_guard_state.reason,
                                                                            &s_guard_cfg);
            s_trip_seq++; // wraps uint8_t -- see the variable's own doc comment

            // TODO (Phase 8): step 4 of SAFETY_MODEL.md section 6's 4-step
            // trip order (logging the trip itself) is still not implemented
            // here -- log_task now exists (see the CLEAR_TRIP drain right
            // below, which does use it), but wiring the ORIGINAL trip event
            // to it is separate, later work, not this pass's.
        }

        // CLEAR_TRIP: drain at most one queued request per tick. Must run
        // here, inside safety_core_task, using THIS tick's own `input` --
        // see safety_core_request_clear_trip()'s doc comment in
        // safety_core.h for why it may no longer be called directly from
        // link_task's context. Non-blocking receive: a request sitting in
        // the queue is picked up on the very next 100ms tick, never held up
        // by, and never itself holding up, anything link_task or the link
        // is doing.
        uint8_t clear_trip_token;
        if (xQueueReceive(s_clear_trip_queue, &clear_trip_token, 0) == pdTRUE) {
            (void)clear_trip_token; // no payload, see s_clear_trip_queue's doc comment

            // link_task already screened the "nothing tripped" case before
            // enqueuing (link_frame_decide_clear_trip(), against a
            // slightly-older read of trip_reason) -- was_tripped can still
            // be false here if is_tripped changed in the window between
            // that check and this dequeue (e.g. an E-stop-cycle clear, or a
            // second queued request already resolved it). Not an error,
            // just stale; safety_guards_try_clear() is skipped entirely in
            // that case (nothing to retest against) --
            // safety_guards_decide_clear_trip_outcome() (safety_guards.c,
            // host-tested) is the pure classification of the two facts
            // below into one of the three outcomes.
            bool was_tripped = s_guard_state.is_tripped;
            bool try_clear_result = false;
            // 2026-08-23 round 3 diagnostic (SWD-readable BSS statics, kept
            // for a live non-crash quick check) -- see statics' own comment
            // above. SUPERSEDED as the authoritative record by
            // clear_trip_diag_mark() below (round 4): the crash under
            // investigation reboots the chip, and a watchdog reset zeroes
            // .bss before anyone can read what these held -- see
            // clear_trip_diag.h's own header comment for the full reasoning.
            // Kept anyway because they are still readable for a manual,
            // non-crashing single-step over SWD, which the scratch-register
            // version does not need to bother offering.
            uint8_t reason_u8 = 0;
            uint8_t fault_bits_u8 = 0;
            bool tc_valid_snapshot = false;
            bool spi_failed_snapshot = false;
            bool tc_c_is_nan_snapshot = false;
            if (was_tripped) {
                reason_u8 = (uint8_t)s_guard_state.reason;
                fault_bits_u8 = input.fault_bits;
                tc_valid_snapshot = input.tc_valid;
                spi_failed_snapshot = input.spi_failed;
                tc_c_is_nan_snapshot = isnan(input.tc_c) != 0;

                s_clear_trip_pre_reason = reason_u8;
                s_clear_trip_pre_tc_valid = tc_valid_snapshot ? 1u : 0u;
                uint32_t tc_c_bits = 0;
                memcpy(&tc_c_bits, &input.tc_c, sizeof(tc_c_bits)); // exact bit pattern, not a cast -- see static's own comment
                s_clear_trip_pre_tc_c_bits = tc_c_bits;
                s_clear_trip_pre_fault_bits = fault_bits_u8;
                s_clear_trip_pre_spi_failed = spi_failed_snapshot ? 1u : 0u;
                s_clear_trip_pre_call_count++;

                // 2026-08-23 round 4: the reset-surviving checkpoint. Written
                // immediately before the suspect call, with outcome not yet
                // known (0/NONE) -- if the board reboots between here and
                // the next mark(), THIS is the last stage that will read
                // back after reset, which alone tells the coordinator the
                // fault is inside (or entered by) safety_guards_try_clear().
                clear_trip_diag_mark(CLEAR_TRIP_DIAG_STAGE_PRE_TRY_CLEAR, reason_u8, fault_bits_u8,
                                      tc_valid_snapshot, spi_failed_snapshot, tc_c_is_nan_snapshot,
                                      0u);

                try_clear_result = safety_guards_try_clear(&s_guard_state, &s_guard_cfg, &input);

                s_clear_trip_post_call_count++; // reached iff the call above actually returned
                clear_trip_diag_mark(CLEAR_TRIP_DIAG_STAGE_POST_TRY_CLEAR, reason_u8, fault_bits_u8,
                                      tc_valid_snapshot, spi_failed_snapshot, tc_c_is_nan_snapshot,
                                      0u);
            }
            safety_clear_trip_outcome_t outcome =
                safety_guards_decide_clear_trip_outcome(was_tripped, try_clear_result);
            if (outcome == SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED) {
                (void)relay_owner_clear_trip();
            }

            s_clear_trip_processed++;
            s_clear_trip_last_outcome = outcome;

            clear_trip_diag_mark(CLEAR_TRIP_DIAG_STAGE_PRE_OUTCOME_LOG, reason_u8, fault_bits_u8,
                                  tc_valid_snapshot, spi_failed_snapshot, tc_c_is_nan_snapshot,
                                  (uint8_t)outcome);

            char msg[64];
            snprintf(msg, sizeof(msg), "req=%lu proc=%lu outcome=%s",
                     (unsigned long)s_clear_trip_requested, (unsigned long)s_clear_trip_processed,
                     clear_trip_outcome_str(outcome));
            log_task_log(outcome == SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED ? LOG_LEVEL_INFO : LOG_LEVEL_WARN,
                         "clear_trip", msg);

            clear_trip_diag_mark(CLEAR_TRIP_DIAG_STAGE_POST_LOG_TASK, reason_u8, fault_bits_u8,
                                  tc_valid_snapshot, spi_failed_snapshot, tc_c_is_nan_snapshot,
                                  (uint8_t)outcome);
        }

        watchdog_task_checkin(WATCHDOG_CHECKIN_SAFETY_CORE);
    }
}

void safety_core_get_output_status(bool *out_relay_energized, bool *out_heating_enabled)
{
    if (out_relay_energized) {
        *out_relay_energized = relay_owner_is_energized();
    }
    if (out_heating_enabled) {
        *out_heating_enabled = (relay_owner_get_state() == RELAY_OWNER_STATE_ARMED);
    }
}

void safety_core_get_diag_status(safety_trip_t *out_trip_reason, bool *out_warn_active,
                                  uint8_t *out_diag_state)
{
    // s_guard_state is this task's own local static -- safe to read from any
    // task the same way relay_owner's state is (single-word/small-struct
    // reads of fields only this task's own tick ever writes; no torn-read
    // hazard worse than the volatile-read pattern relay_owner.h already
    // documents for the same reason, and a stale-by-one-tick (100ms) read is
    // immaterial for a diagnostic frame).
    bool warn_active = s_guard_state.s5_warn || s_guard_state.s12_warn;

    if (out_trip_reason) {
        *out_trip_reason = s_guard_state.is_tripped ? s_guard_state.reason : SAFETY_TRIP_NONE;
    }
    if (out_warn_active) {
        *out_warn_active = warn_active;
    }
    if (out_diag_state) {
        relay_owner_state_t relay_state = relay_owner_get_state();
        uint8_t state;
        if (s_guard_state.is_tripped) {
            state = 4; // tripped -- takes priority over everything else
        } else if (warn_active) {
            state = 3; // warn -- a guard is warning but hasn't tripped; not a
                       // relay_owner_state_t value at all (see this
                       // function's header comment), so it must be decided
                       // here rather than by mapping relay_state alone
        } else if (relay_state == RELAY_OWNER_STATE_ARMED) {
            state = 2;
        } else if (relay_state == RELAY_OWNER_STATE_GRACE) {
            state = 1;
        } else {
            state = 0; // RELAY_OWNER_STATE_INIT, or any future value --
                       // conservative default rather than guessing
        }
        *out_diag_state = state;
    }
}

bool safety_core_get_trip_event(uint8_t *out_trip_seq, safety_trip_t *out_trip_reason,
                                 uint32_t *out_uptime_ms, float *out_tc_c,
                                 float *out_deciding_threshold)
{
    // Read s_trip_seq FIRST, matching the write-order comment at the capture
    // site in safety_core_task() above -- if a trip lands between this read
    // and the field reads below, the caller sees either the old, fully
    // consistent set (this seq, these fields) or -- at worst -- reports one
    // trip event a poll cycle later than it happened. It can never observe a
    // torn mix of one trip's seq with another trip's fields, because the
    // writer always finishes writing every field before bumping the seq.
    uint8_t seq = s_trip_seq;

    if (out_trip_seq) {
        *out_trip_seq = seq;
    }
    if (seq == 0) {
        // No trip yet this boot -- every other output is meaningless
        // (matches the header comment's documented "false, everything
        // zeroed" contract) rather than whatever s_trip_* happen to hold
        // (their static-storage zero-init, in this case, but that is an
        // implementation detail the caller must not rely on).
        if (out_trip_reason) {
            *out_trip_reason = SAFETY_TRIP_NONE;
        }
        if (out_uptime_ms) {
            *out_uptime_ms = 0;
        }
        if (out_tc_c) {
            *out_tc_c = NAN;
        }
        if (out_deciding_threshold) {
            *out_deciding_threshold = NAN;
        }
        return false;
    }

    if (out_trip_reason) {
        *out_trip_reason = s_trip_reason;
    }
    if (out_uptime_ms) {
        *out_uptime_ms = s_trip_uptime_ms;
    }
    if (out_tc_c) {
        *out_tc_c = s_trip_tc_c;
    }
    if (out_deciding_threshold) {
        *out_deciding_threshold = s_trip_deciding_threshold;
    }
    return true;
}

// See safety_core.h's doc comment on this function for the full "why a
// queue, not a direct call" history -- this is now purely an enqueue, never
// a direct touch of s_guard_state. safety_core_task() (below) is the only
// code that actually runs safety_guards_try_clear().
bool safety_core_request_clear_trip(void)
{
    if (s_clear_trip_queue == NULL) {
        return false;
    }

    // Content unused -- see s_clear_trip_queue's own doc comment: this is a
    // pure "a request is pending" signal, link_task already validated
    // trip_mask before ever calling this function.
    uint8_t token = 0;
    if (xQueueSend(s_clear_trip_queue, &token, 0) != pdTRUE) {
        return false;
    }
    s_clear_trip_requested++;
    return true;
}

// String form of safety_clear_trip_outcome_t for the log line
// safety_core_task() emits once it dequeues and resolves a request --
// factored out only so that log line and this comment stay next to the enum
// definition's own doc comment in safety_core.h rather than drifting apart.
static const char *clear_trip_outcome_str(safety_clear_trip_outcome_t outcome)
{
    switch (outcome) {
    case SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED:
        return "accepted";
    case SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STILL_TRIPPED:
        return "refused-still-tripped";
    case SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_NOTHING_LATCHED:
        return "refused-nothing-latched";
    case SAFETY_CLEAR_TRIP_OUTCOME_NONE:
    default:
        return "none";
    }
}

void safety_core_get_clear_trip_stats(uint32_t *out_requested, uint32_t *out_processed,
                                       safety_clear_trip_outcome_t *out_last_outcome)
{
    if (out_requested) {
        *out_requested = s_clear_trip_requested;
    }
    if (out_processed) {
        *out_processed = s_clear_trip_processed;
    }
    if (out_last_outcome) {
        *out_last_outcome = s_clear_trip_last_outcome;
    }
}

bool safety_core_request_enable(bool enable)
{
    // The Pico's OWN half of the mutual "heating is not allowed during
    // updates" interlock (ROADMAP.md M8) -- checked BEFORE forwarding to
    // relay_owner, and only when `enable` is requesting ON: de-energizing
    // is never refused, same "safe direction always reachable" rule
    // relay_energize_allowed_during_update()'s own doc comment states. This
    // does not consult the ESP at all -- update_task_transfer_active()
    // reads THIS processor's own s_transfer_active, so a Pico mid-transfer
    // refuses a new energize request even if the ESP's own interlock
    // (KilnFW's ota_interlock.c) somehow disagreed or was bypassed.
    if (enable && !relay_energize_allowed_during_update(update_task_transfer_active())) {
        return false;
    }

    // safety_tc_installed (config param 0x0211) refusal -- the other half
    // of the S5 trade documented in safety_guards.c's grace-exceeded block
    // and in config_store.h's own field comment. When the operator has
    // declared the safety thermocouple not installed, S5 stops promoting a
    // persistent bad-read streak to TRIP (it stays a permanent WARN
    // instead) -- and that downgrade is only safe BECAUSE this function
    // refuses the ON direction unconditionally whenever the same field is
    // 0, with no time box, no accumulator, and no way for any other code
    // path to re-grant heat while it is declared absent. Same "only the ON
    // direction, de-energizing is always reachable" shape as the update-
    // interlock check just above: a board correctly reporting itself
    // untestable for heat must still be able to de-energize on command
    // (e.g. an operator flipping a clear latched trip, or the normal
    // GRACE-to-IDLE path), never refused in that direction.
    if (enable) {
        config_store_record_t cfg_rec;
        config_store_get_full_record(&cfg_rec);
        if (cfg_rec.safety_tc_installed == 0u) {
            return false;
        }
    }

    // See safety_core.h's doc comment: deliberately a thin forward beyond
    // the check above, no second policy layer duplicating relay_owner's
    // own state machine. relay_owner_command_energize() already refuses
    // while TRIPPED, accepts-but-never-applies during GRACE, and only
    // actually drives GPIO6 high while ARMED.
    return relay_owner_command_energize(enable);
}

bool safety_core_start(void)
{
    // Created before the task itself -- safety_core_request_clear_trip()
    // (callable from link_task the instant this returns true) must never
    // observe a NULL queue that safety_core_task would have created for
    // itself a moment later.
    s_clear_trip_queue = xQueueCreate(SAFETY_CORE_CLEAR_TRIP_QUEUE_LEN, sizeof(uint8_t));
    if (s_clear_trip_queue == NULL) {
        return false;
    }

    BaseType_t ok = xTaskCreate(safety_core_task, "safety_core", SAFETY_CORE_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_SAFETY_CORE, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_TRIP_PATH);
    return true;
}
