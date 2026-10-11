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
#include "trip_seq.h" // trip_seq_next(), F4 (link-free header; link_frame.h is off limits here)
#include "hal_barrier.h" // HAL_DMB(): publish/acquire of the trip-event fields across cores, F5

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
#include "tick_timing.h" // 2026-08-27 audit items 1/2: measured dt_s and snapshot freshness -- see its own header comment
#include "clear_trip_diag.h" // 2026-08-23 round 4, CLEAR_TRIP crash checkpoints that survive the reboot -- see its own header comment
#include "commissioning_gate.h" // ROADMAP.md M12 -- uncommissioned boards refuse heating enable
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
// LINK_FIRING_ABORT_SILENCE_MS (firmware/KilnFW/App/drivers/safety/safety_link.h)
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

// 2026-08-27 audit item 2: ARCHITECTURE.md section 6, "Staleness is checked
// by the consumer, not the producer" -- applied here to thermo_snapshot_t/
// current_snapshot_t the same way CONTEXT_MAX_AGE_MS above already applies it
// to context_snapshot_t. See tick_timing.h's own header comment on
// snapshot_is_fresh() for why the check itself is a separate host-tested
// pure function even though context_valid's identical-shaped check stays
// inline just below.
//
// THERMO_MAX_AGE_MS: thermo_task publishes roughly once per MAX31856
// conversion (max31856_conversion_time_ms(), ~151ms at this board's default
// mode/filter settings -- thermo_task.c's own THERMO_TASK_DRDY_SILENCE_
// MULTIPLIER comment) -- and thermo_task ALREADY declares itself blind
// (tc_valid = false) if no ~DRDY edge arrives within 2x that, ~300ms
// (thermo_task.c). This constant is deliberately independent of, and
// generous relative to, that ~300ms internal detector: it exists for the
// DIFFERENT failure this audit item targets -- thermo_task's own detector
// working fine (correctly declaring itself blind or fresh) but the PUBLISH
// of that verdict getting stuck behind a held mutex, so
// thermo_task_get_snapshot() keeps returning an old, increasingly stale
// struct while thermo_task itself still feeds the watchdog. ~13x the nominal
// publish interval leaves wide margin over ordinary scheduling jitter (this
// is a WORSE, not tighter, bound than the internal ~300ms one -- it is not
// meant to fire before that detector would) while still being caught well
// inside S5's blind_grace_s default of 60.0s (safety_guards.c
// BLIND_GRACE_S_DEFAULT) -- see this file's own comment where tc_valid is
// set for what tripping tc_valid = false actually does.
#define THERMO_MAX_AGE_MS 2000u

// CURRENT_MAX_AGE_MS: current_task publishes every SAFTYFW_PERIOD_CURRENT_
// TASK_MS = 50ms (task_priorities.h). 2000ms mirrors update_task.c's own
// UPDATE_TASK_ADC_FRESH_MS -- the SAME producer, the SAME "generous relative
// to its own period so this is never the flaky part of the check" reasoning
// that constant's own comment already gives (40x its 50ms period) -- reusing
// the already-reviewed number rather than inventing a second one for the
// identical fact. See this file's own comment at the any_current_present
// computation for what a stale current reading is made to mean.
#define CURRENT_MAX_AGE_MS 2000u

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
// exception (now inert: S1's firing-time tightening was
// RETIRED 2026-09-24, owner decision: "the safety limits should be the
// same") -- safety_guards_tick() no longer reads firing_max_valid/
// firing_max_c, so the history below describes plumbing that still runs
// but decides nothing): firing_max_valid/firing_max_c ARE written every
// tick now, by
// safety_core_build_input() below, from link_task's SET_FIRING_CEILING RX
// state. That is deliberately not a config-store gap the same way
// abs_max_temp_c is: safety_guards.h's own doc comment on those two fields is
// explicit they are "context, not config" -- see safety_core_build_input()'s
// own comment at the point they are set for the full reasoning. Note also
// that as long as abs_max_temp_c stays uncommissioned (0), S1 never trips at
// all regardless of firing_max_valid -- the min() clamp in safety_guards.c
// only ever tightens a real abs_max_temp_c, it does not manufacture one.
static safety_guard_cfg_t s_guard_cfg;

// S13's per-zone last-seen sample_counter, indexed by zone_index (0..
// CONTEXT_SNAPSHOT_MAX_ZONES-1, NOT by array position in a given tick's
// zones[] -- see safety_core_build_input()'s sample_counter_advancing block
// for why those two differ). s_borrowed_sample_counter_known[z] is false
// until zone_index z has been seen in a context frame at least once this
// boot, so the very first sighting of a zone can never manufacture a false
// "advancing" (or "stalled") verdict by comparing against zero-initialized
// state that was never a real reading.
static uint8_t s_borrowed_last_sample_counter[CONTEXT_SNAPSHOT_MAX_ZONES];
static bool    s_borrowed_sample_counter_known[CONTEXT_SNAPSHOT_MAX_ZONES];

// Edge-latch for the borrowed-channel tc_type mismatch diagnostic
// (context_borrowed_type_mismatch(), snapshots.h) -- docs/THERMOCOUPLE.md
// "Type checking a borrowed channel". Not per-zone like the sample_counter
// state above: only one zone can be the commissioned borrowed_zone_index at
// a time, so one flag is enough. Logged once on the false->true transition,
// not every 100ms tick the mismatch persists -- log_task_log() is not free
// (this file's own comments elsewhere on log_task_log() cost), and a
// standing mismatch is exactly as true on tick 2 as it was on tick 1.
static bool s_borrowed_type_mismatch_warned;
// T2: last-good-config bookkeeping for a failed config_store read (see the
// cfg_read_ok block in the tick). Reset with the guards at task start.
static bool s_have_good_guard_cfg;
static bool s_last_good_tc_not_installed_declared;

// KILN_PROFILES_PLAN.md item 16 -- same "log once on the transition, not
// every tick" idiom as s_borrowed_type_mismatch_warned just above, for the
// ARMED-while-unconfigured backstop below.
static bool s_unconfigured_armed_warned;

// Cached alongside s_guard_cfg by apply_config_to_guard_cfg(), for the same
// reason: recomputed only when the commissioned record changes, read every
// tick. See that function's comment for why S9 needs it. False until a
// record with all three k_ct_v_per_a channels calibrated is loaded, which is
// the safe direction -- it downgrades S9 to a warning rather than latching an
// unclearable trip off a reading nothing has calibrated.
static bool s_current_sensing_commissioned = false;
static safety_guard_state_t s_guard_state;

// Copies every commissioned threshold out of config_store into s_guard_cfg.
//
// THIS EXISTS BECAUSE IT DID NOT (audit 2026-08-27). Before this function,
// the ONLY fields ever written to s_guard_cfg anywhere in the firmware were
// firing_max_valid/firing_max_c (below, from the link). Every other member --
// abs_max_temp_c above all -- sat at its zero-initialised value for the life
// of the board. S1's own gate is `if (cfg->abs_max_temp_c > 0.0f)`, so the
// absolute over-temperature guard, the one SAFETY_MODEL.md section 4 says
// would "alone justify the board", could not fire at ANY temperature. Worse,
// commissioning did not help: SET_PARAM/COMMIT_CONFIG wrote the operator's
// value to flash and to the config cache, and nothing ever carried it the
// last few inches into the struct the guards actually read.
//
// Called every tick rather than once at start or on a commit hook. The
// record read is a cheap cached-copy (config_store_flash.c) that this task
// already performs each tick for safety_tc_installed, so the added cost is a
// handful of float copies; in exchange there is no way for a COMMIT_CONFIG
// to leave the guards running on a stale threshold, and no hook to forget to
// call. firing_max_valid/firing_max_c are deliberately NOT touched here --
// they are context from the link, written in safety_core_build_input().
//
// fields_set gating is honoured exactly as config_store.h documents it: a
// field whose CONFIG_STORE_SET_* bit is clear is NOT copied, so it keeps the
// "never commissioned" value the guards already treat as "stay off". That is
// the difference between a guard that is off because nobody set it and a
// guard that is off because someone set it to zero, and SAFETY_MODEL.md
// section 1 is explicit that those must not be conflated.
static void safety_core_load_guard_cfg(const config_store_record_t *rec)
{
    // S1. Left at 0 (never trips) unless explicitly commissioned -- see
    // SAFETY_MODEL.md section 4, S1: "has no default and must be
    // commissioned". A substituted ceiling here would be a missed-trip risk
    // if it were ever too high, which is why this stays gated.
    s_guard_cfg.abs_max_temp_c =
        (rec->fields_set & CONFIG_STORE_SET_ABS_MAX_TEMP_C) ? rec->abs_max_temp_c : 0.0f;
    s_guard_cfg.firing_margin_c = rec->firing_margin_c;

    // tc_placement_mode gates S1's chamber ceiling, S2 and S10. The enum
    // cannot express "unset" (CHAMBER_AGREED is 0, which is also the
    // zero-init value AND the value that arms S2/S10), so the validity flag
    // carries that -- see safety_guards.h's tc_placement_valid comment.
    s_guard_cfg.tc_placement_valid = (rec->fields_set & CONFIG_STORE_SET_TC_PLACEMENT_MODE) != 0u;
    s_guard_cfg.tc_placement_mode = (rec->tc_placement_mode == CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT)
                                         ? SAFETY_TC_EXTERNAL_OVERHEAT
                                         : SAFETY_TC_CHAMBER_AGREED;

    // tc_source gates S13. OWN_J7 is the conservative uncommissioned reading
    // (safety_guards.h). sample_counter_advancing (safety_core_build_input())
    // now has a real producer -- see that call site -- so BORROWED_ZONE/BOTH
    // is safe to commission once borrowed_zone_index is also set.
    s_guard_cfg.tc_source = SAFETY_TC_SOURCE_OWN_J7;
    if (rec->fields_set & CONFIG_STORE_SET_TC_SOURCE) {
        if (rec->tc_source == CONFIG_STORE_TC_SOURCE_BORROWED_ZONE) {
            s_guard_cfg.tc_source = SAFETY_TC_SOURCE_BORROWED_ZONE;
        } else if (rec->tc_source == CONFIG_STORE_TC_SOURCE_BOTH) {
            s_guard_cfg.tc_source = SAFETY_TC_SOURCE_BOTH;
        }
    }

    // S5 / S11 / S12 / S13 / S2 / S3 / S6b / S9 / S10 windows and thresholds.
    // Each of these is a "0 -> documented default" field in safety_guards.c's
    // own effective_f()/effective_u16(), so copying a genuine 0 through is
    // correct and keeps that single source of defaults authoritative here.
    s_guard_cfg.blind_grace_s     = (float)rec->blind_grace_s;
    s_guard_cfg.frozen_window_s   = (float)rec->frozen_window_s;
    s_guard_cfg.cj_warn_c         = rec->cj_warn_c;
    s_guard_cfg.cj_max_c          = rec->cj_max_c;
    s_guard_cfg.cj_time_s         = (float)rec->cj_time_s;
    s_guard_cfg.borrowed_stale_s      = (float)rec->borrowed_stale_s;
    s_guard_cfg.borrowed_stale_trip_s = (float)rec->borrowed_stale_trip_s;
    s_guard_cfg.overshoot_margin_c    = rec->overshoot_margin_c;
    s_guard_cfg.overshoot_time_s      = (float)rec->overshoot_time_s;
    s_guard_cfg.i_present_a           = rec->i_present_a;
    s_guard_cfg.correlation_window_s  = (float)rec->correlation_window_s;
    s_guard_cfg.stuck_on_time_s       = (float)rec->stuck_on_time_s;
    s_guard_cfg.link_timeout_s        = (float)rec->link_timeout_s;
    s_guard_cfg.link_dead_hard_s      = (float)rec->link_dead_hard_s;
    s_guard_cfg.trip_verify_s         = (float)rec->trip_verify_s;
    s_guard_cfg.tc_disagreement_c      = rec->tc_disagreement_c;
    s_guard_cfg.tc_disagreement_time_s = (float)rec->tc_disagreement_time_s;

    // S8. max_rate_c_per_min follows the exact abs_max_temp_c idiom above:
    // fields_set-gated, no substituted default, because CONFIG_REFERENCE.md
    // section 7 says it ships genuinely UNSET (not merely zero) and 0.0f is
    // also the value that means "never trips" (safety_guards.c's own gate,
    // `if (cfg->max_rate_c_per_min > 0.0f)`). Copying a genuine 0 through
    // when the bit IS set is still correct -- that is an operator explicitly
    // leaving S8 off, same distinction SAFETY_MODEL.md section 1 draws for
    // S1. rate_window_s is NOT fields_set-gated, same as blind_grace_s/
    // frozen_window_s/etc above: it is a "0 -> documented default" field per
    // safety_guards.c's own effective_f(RATE_WINDOW_S_DEFAULT), so copying a
    // genuine 0 through is correct and keeps that single source of defaults
    // authoritative here.
    s_guard_cfg.max_rate_c_per_min =
        (rec->fields_set & CONFIG_STORE_SET_MAX_RATE_C_PER_MIN) ? rec->max_rate_c_per_min : 0.0f;
    s_guard_cfg.rate_window_s = (float)rec->rate_window_s;

    // S14 (COMMISSIONING_UX.md section 3.3). Per-channel i_normal_a is
    // fields_set-gated INDIVIDUALLY (three separate bits, not one group
    // bit -- config_store.h's own comment on why) -- a channel whose bit is
    // clear must reach safety_guards.c with i_normal_valid[ch] == false so
    // it is skipped entirely, never evaluated against a stale/zero value.
    // This is the exact wiring step the 2026-08-27 audit found missing for
    // S1 ("a host test cannot see a value that never arrives") applied to a
    // brand-new guard instead of an existing one.
    s_guard_cfg.i_normal_valid[0] = (rec->fields_set & CONFIG_STORE_SET_I_NORMAL_A_0) != 0u;
    s_guard_cfg.i_normal_valid[1] = (rec->fields_set & CONFIG_STORE_SET_I_NORMAL_A_1) != 0u;
    s_guard_cfg.i_normal_valid[2] = (rec->fields_set & CONFIG_STORE_SET_I_NORMAL_A_2) != 0u;
    s_guard_cfg.i_normal_a[0] = s_guard_cfg.i_normal_valid[0] ? rec->i_normal_a[0] : 0.0f;
    s_guard_cfg.i_normal_a[1] = s_guard_cfg.i_normal_valid[1] ? rec->i_normal_a[1] : 0.0f;
    s_guard_cfg.i_normal_a[2] = s_guard_cfg.i_normal_valid[2] ? rec->i_normal_a[2] : 0.0f;
    s_guard_cfg.overcurrent_pct    = (uint16_t)rec->overcurrent_pct;
    s_guard_cfg.overcurrent_time_s = (float)rec->overcurrent_time_s;

    // ct_topology (param 0x031F, CT_COMMISSIONING_PLAN.md step 3). rec->
    // ct_topology is already the DECODED value (config_store.c's unpack does
    // the marker-byte defaulting for a legacy/erased record) -- 0 = per_zone,
    // 1 = summed. No fields_set gate: unlike ct_installed this is not an
    // ASKED question with two equally-plausible answers, it is "which
    // hardware topology is fitted", and per_zone (today's only shipped
    // topology) is the only safe silent default for a record that never
    // answers.
    s_guard_cfg.ct_topology_summed = (rec->ct_topology == CONFIG_STORE_CT_TOPOLOGY_SUMMED);

    // zone_ct_channel (params 0x0320-0x0322, CT_CHANNEL_MASK.md step 4).
    // config_store_effective_zone_ct_channel() is THE single place that knows
    // whether the stored bytes are an operator answer or the ct_topology-
    // derived fallback, so this is a straight projection of it -- do not
    // re-derive the rule here. The _valid flag mirrors the same group bit,
    // and is what makes safety_guards.c keep running its legacy
    // ct_topology_summed branches for every board that has not committed a
    // map: an effective map that is merely DERIVED carries no information
    // the topology boolean did not already carry, so feeding it into the
    // generalised arm could only add risk, never accuracy.
    config_store_effective_zone_ct_channel(rec, s_guard_cfg.zone_ct_channel);
    s_guard_cfg.zone_ct_channel_valid =
        (rec->fields_set & CONFIG_STORE_SET_ZONE_CT_CHANNEL) != 0u;

    // S9's gate on whether the current reading is a MEASUREMENT or a
    // heuristic. 2026-08-27: safety_guards.c gained
    // in->current_sensing_commissioned so an uncommissioned board cannot latch
    // the unclearable TRIP_INEFFECTIVE off a phantom reading -- with
    // zero_counts shipping as 0 (config_store_default()), the op-amp's DC
    // offset floor is subtracted against nothing and reads as real current on
    // every tick, forever. That field arrived with no producer, which is the
    // same shape as S13's sample_counter_advancing sitting hardcoded false:
    // a guard input that looks wired and is not. Without this line the field
    // would be false on EVERY board, silently downgrading S9 to a warning
    // even after commissioning -- the failure that would have been found
    // months later, by a welded contactor.
    //
    // 2026-09-18 (owner-directed fix, summed-CT-topology support): this used
    // to require ALL THREE channels calibrated, unconditionally -- correct
    // for PER_ZONE (every channel has its own CT and an uncalibrated one is
    // a real gap), but permanently, silently WRONG for SUMMED topology,
    // where channels 0/1 have no CT behind them at all and can never be
    // calibrated. That made this flag permanently false on every summed-CT
    // board, permanently downgrading S9 (TRIP_INEFFECTIVE, the welded-
    // contactor guard) to a warning -- the live bench board's actual state.
    //
    // Now: require k_ct_v_per_a > 0 on every FITTED channel (config_store_ct_
    // channel_fitted() -- THE single place "is this channel fitted" is
    // decided, see its doc comment) and require at least one channel be
    // fitted at all. A board with ct_installed == 0 (no channel fitted)
    // must not report commissioned -- there is no measurement to certify.
    // PER_ZONE with all three calibrated is bit-for-bit the same answer as
    // before (all three are fitted, so this is still "all three
    // calibrated"); SUMMED with channel 2 calibrated now correctly reports
    // commissioned, since channel 2 is the only fitted channel.
    //
    // This relaxation is safe ONLY because it lands together with this same
    // commit's presence-masking fix (safety_core_build_input()'s
    // any_current_present derivation and current_task_any_current_present(),
    // both now masked through the identical config_store_ct_channel_fitted()
    // predicate): before that companion fix, relaxing this gate alone would
    // let S9 (unclearable once latched -- safety_guards_try_clear() and
    // link_frame_decide_clear_trip() both refuse it unconditionally) arm
    // while it could still latch off an UNFITTED channel's raw ADC noise
    // floor (current_presence_policy.c's fallback-margin branch, taken
    // whenever k_ct_v_per_a <= 0 -- measured ~16-17 counts idle on this
    // bench board's unfitted channels, against a 25-count margin). With
    // presence masked to fitted channels only, an unfitted channel's noise
    // can no longer contribute to the current_any_present fact S9 reads, so
    // there is nothing left for a relaxed commissioned gate to arm against.
    //
    // The actual decision -- ct_installed-effective resolution, the per-
    // channel fitted/calibrated loop, and the "at least one fitted" rule --
    // lives in config_store_current_sensing_commissioned() (config_store.h),
    // not here, so this is the ONLY place that reads it: host-testable
    // directly, and the one thing this codebase's own reset-one-side-of-a-
    // pair bug class says never to re-derive inline a second time.
    s_current_sensing_commissioned = config_store_current_sensing_commissioned(rec);
}

// CLEAR_TRIP queue -- see safety_core_request_clear_trip()'s doc comment in
// safety_core.h for the full "why a queue, not a direct call" reasoning.
// Small and non-blocking, same convention as relay_owner.h's own command
// queue: a backlog here means safety_core_task's 100ms tick is not draining
// it, which is a bug worth surfacing as a dropped request (xQueueSend(...,
// 0) returning false), never a reason to block the caller. 2, not 1: a
// second CLEAR_TRIP arriving in the same 100ms window as the first (e.g. a
// GUI double-click) is a real, if unlikely, case worth not silently
// dropping; there is no legitimate reason for a third to queue up before
// the tick drains the first two. link_task has already validated trip_mask
// (link_frame_decide_clear_trip()) before ever calling
// safety_core_request_clear_trip(). Since kilnlink audit 2026-10-09 M4 each
// item is a uint16_t carrying the request's trip occurrence:
// CLEAR_TRIP_TOKEN_BOUND | trip_seq for a 4-byte (protocol >= 17) frame, 0
// for a legacy unbound one.
#define SAFETY_CORE_CLEAR_TRIP_QUEUE_LEN 2
#define CLEAR_TRIP_TOKEN_BOUND 0x100u
static QueueHandle_t s_clear_trip_queue = NULL;

// Single-writer statics backing safety_core_get_clear_trip_stats() --
// s_clear_trip_requested written only inside safety_core_request_clear_trip()
// (called only from link_task, so effectively single-core-writer too, same
// as link_task.c's own s_context_frames_ok/bad); s_clear_trip_processed/
// s_clear_trip_last_outcome written only inside safety_core_task() below.
static uint32_t s_clear_trip_requested = 0;

// 2026-09-15 Opus re-review N1: link_task's tc-type heat-safety gate needs
// to know "did the Pico recently receive/act on a REQUEST_ENABLE(true)",
// not just "is GPIO6 high right now" -- relay_owner_command_energize() is
// a queued command (relay_owner.c drains it on its own tick), so there is
// a real window, right after this function forwards an accepted enable
// request, where relay_owner_is_energized() can still read false even
// though the Pico has committed to energizing. single-writer: only this
// function (safety_core_request_enable(), always called from link_task's
// own task context per its header comment) ever writes this.
static uint32_t s_last_enable_true_request_ms = 0;
static bool     s_last_enable_true_request_seen = false;

// 2026-09-15 Opus re-review N2: set true by link_task.c for the whole
// span from "the tc-type heat-safety gate just passed" through "the new
// tc_type has actually been reconfigured into the MAX31856 (or the
// reconfigure was correctly skipped/retried)" -- see link_task.c's own
// comment on this window. safety_core_request_enable() below refuses the
// ON direction while this is true, same "ON direction only, de-energizing
// always reachable" shape as every other refusal in that function, so a
// REQUEST_ENABLE landing mid-apply can never race the flash-write-then-
// reconfigure sequence: the flash record and the physical chip cannot
// diverge because heat is never granted during the one window where they
// could. single-writer: only link_task.c ever calls the setter, always
// from its own task context (same as s_last_enable_true_request_ms above).
static bool s_tc_type_apply_in_progress = false;
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
// first real trip makes it 1, wrapping uint8_t 255 -> 1 thereafter (never back
// to 0: trip_seq_next(), safety link review 2026-10-09 F4 -- a wrap
// to 0 made the 256th trip read as "never tripped" and go unreported).
// 2026-08-27 audit: relay_owner_command_trip() posts to a 4-deep queue and
// never blocks (relay_owner must never block -- ARCHITECTURE.md section 1),
// so a full queue silently drops the trip command: K4 stays energized and
// S9 (driven by relay_owner_is_energized(), the software mirror of the
// INTENDED state) has no way to notice its own de-energize path failed.
// safety_guards_tick() only reports "newly tripped" for the ONE tick the
// trip is decided -- without this latch a dropped command would simply
// never be retried. Set the instant a trip is decided, cleared only once
// relay_owner_command_trip() actually enqueues (checked every tick,
// independent of newly_tripped, in the retry loop in safety_core_task()
// below) -- "retry until it actually succeeds" is the shape that fits this
// codebase's non-blocking producer/consumer pattern, not a bigger queue or
// a blocking send.
static bool s_trip_command_owed = false;

// 2026-08-27 audit: the identical hazard as s_trip_command_owed above, one
// line away and one direction over. relay_owner_clear_trip() posts to the
// SAME non-blocking 4-deep queue relay_owner_command_trip() does, and
// commit 4962421 only latched/retried the TRIP direction -- the CLEAR
// direction was left as a bare (void)-discarded single attempt. A dropped
// clear send left relay_owner latched in RELAY_OWNER_STATE_TRIPPED forever:
// s_guard_state had already gone back to "not tripped" and the clear_trip
// log line had already reported ACCEPTED, so nothing anywhere -- not the
// guard state, not telemetry, not the log -- ever indicated K4 would refuse
// every future energize. Set the instant safety_guards_decide_clear_trip_
// outcome() returns ACCEPTED, cleared only once relay_owner_clear_trip()
// actually enqueues (checked every tick in the retry loop at the bottom of
// safety_core_task(), same "retry until it actually succeeds" shape as the
// trip side, not a bigger queue or a blocking send). See relay_clear_
// command_still_owed()'s doc comment (relay_grace.h) for how this flag and
// s_trip_command_owed interact when both are owed at once -- the SAFE
// (trip) direction always wins, and it is s_guard_state.is_tripped itself
// (not this flag inspecting s_trip_command_owed directly) that decides it,
// which is what keeps the two flags from ever being able to deadlock or
// oscillate against each other.
static bool s_clear_command_owed = false;
// LOW-6 (safety-link fix batch 2): both are written by safety_core_task (core 1)
// and read lock-free by the link task on the other core, so they are volatile:
// without it the compiler may hoist/merge the reader's loads. s_trip_gen is a
// real seqlock generation (odd while a capture is in flight); s_trip_seq is the
// protocol-visible trip number (wraps 255->1, never usable as the generation).
static volatile uint8_t s_trip_seq = 0;
static volatile uint32_t s_trip_gen = 0;
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

// 2026-08-27 audit item 1: previous tick's clock reading, single-writer
// (safety_core_build_input()'s one call site, same status as s_clock_health
// just above), used to MEASURE dt_s via tick_dt_compute_s() instead of
// trusting the compile-time SAFTYFW_PERIOD_SAFETY_CORE_MS constant. Zero-
// initialised: s_dt_have_prev_now_ms starts false, matching tick_dt_
// compute_s()'s own "first tick" fallback contract -- there is no previous
// reading to diff against yet, so the very first call correctly falls back
// to nominal_dt_s rather than measuring against a garbage 0.
static uint32_t s_dt_prev_now_ms;
static bool     s_dt_have_prev_now_ms;

// SWD-readable diagnostics for safety_core_get_dt_diag() (safety_core.h) --
// same status/pattern as s_clear_trip_pre_*/s_clear_trip_post_call_count
// below: not on any link frame (those are frozen), single-writer from this
// file's one call site, read through a getter. See that function's own doc
// comment for what each field means.
static volatile float    s_dt_last_measured_s;
static volatile uint32_t s_dt_clamped_high_count;
static volatile uint32_t s_dt_clamped_low_count;

// dt_s clamp bounds -- see tick_timing.h's tick_dt_compute_s() doc comment
// for the full nuisance-trip-storm-vs-under-integration reasoning behind
// each bound; this is only where the two numbers are chosen for THIS tick
// rate (SAFTYFW_PERIOD_SAFETY_CORE_MS = 100ms).
//   - Upper: 10x nominal (1.0s). Caps the worst single-tick credit any one
//     guard's accumulator can receive to 1.0s -- below even S5's bad_read_
//     time_s default of 5.0s, the SHORTEST documented graduated-guard
//     duration bar in this codebase (safety_guards.c BAD_READ_TIME_S_
//     DEFAULT), so one maximally-clamped tick can never by itself satisfy
//     any trip's duration requirement.
//   - Lower: 0.5x nominal (50ms), the same "half of nominal" fraction
//     clock_health.c's own CLOCK_HEALTH_STALL_DEBOUNCE healthy/stalled
//     threshold already uses for the identical clock, reused rather than
//     inventing a second arbitrary fraction.
#define SAFETY_CORE_DT_NOMINAL_S ((float)SAFTYFW_PERIOD_SAFETY_CORE_MS / 1000.0f)
#define SAFETY_CORE_DT_MIN_S     (SAFETY_CORE_DT_NOMINAL_S * 0.5f)
#define SAFETY_CORE_DT_MAX_S     (SAFETY_CORE_DT_NOMINAL_S * 10.0f)

void safety_core_get_dt_diag(float *out_last_measured_dt_s, uint32_t *out_clamped_high_count,
                              uint32_t *out_clamped_low_count)
{
    if (out_last_measured_dt_s) {
        *out_last_measured_dt_s = s_dt_last_measured_s;
    }
    if (out_clamped_high_count) {
        *out_clamped_high_count = s_dt_clamped_high_count;
    }
    if (out_clamped_low_count) {
        *out_clamped_low_count = s_dt_clamped_low_count;
    }
}

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
    // took. clock_health_observe() is deliberately called with the NOMINAL
    // constant here, not the measured dt_s computed just below: it needs an
    // independent, reliable "how much time SHOULD have passed" to compare
    // the clock's own advancement against, and the whole point of dt_s below
    // is that it is derived FROM this same clock -- feeding a clock-derived
    // number back into its own stall detector would make a stalled clock
    // measure itself as healthy (dt_s would stall right alongside now_ms).
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool clock_stalled =
        clock_health_observe(&s_clock_health, now_ms, SAFETY_CORE_DT_NOMINAL_S);

    // 2026-08-27 audit item 1: measured, clamped dt_s -- see tick_timing.h's
    // tick_dt_compute_s() doc comment for the full fallback/clamp reasoning,
    // and this file's SAFETY_CORE_DT_MIN_S/MAX_S doc comment for why THESE
    // two bounds. prev_now_ms/have_prev_now_ms are snapshotted into locals
    // BEFORE either static is updated below, both because tick_dt_compute_s()
    // needs the PREVIOUS tick's values (not this tick's, which do not exist
    // yet) and because the diagnostics block just below needs the same
    // pre-update values to compute the raw (pre-clamp) measurement for the
    // clamp-hit counters -- reading the statics twice, before and after their
    // own update, would have silently used the just-written new value the
    // second time.
    uint32_t prev_now_ms      = s_dt_prev_now_ms;
    bool     have_prev_now_ms = s_dt_have_prev_now_ms;
    float dt_s = tick_dt_compute_s(now_ms, prev_now_ms, have_prev_now_ms, clock_stalled,
                                    SAFETY_CORE_DT_NOMINAL_S, SAFETY_CORE_DT_MIN_S,
                                    SAFETY_CORE_DT_MAX_S);

    // State update for the NEXT call. clock_stalled == true deliberately
    // clears s_dt_have_prev_now_ms rather than storing now_ms as the new
    // baseline: now_ms is exactly the frozen/unreliable reading clock_
    // health_observe() just distrusted, and s_dt_prev_now_ms must never be
    // set from a reading already known to be bad. This is what makes "any
    // tick after the clock-stalled fallback has no valid previous
    // timestamp" (tick_timing.h's own requirement) true -- the tick right
    // after recovery sees have_prev_now_ms == false and falls back to
    // nominal_dt_s too, same as the very first tick after boot; only the
    // tick after THAT resumes measuring real dt against a freshly-
    // established baseline, never against a timestamp spanning the stall
    // itself.
    if (clock_stalled) {
        s_dt_have_prev_now_ms = false;
    } else {
        s_dt_prev_now_ms = now_ms;
        s_dt_have_prev_now_ms = true;
    }

    // Diagnostics (safety_core_get_dt_diag(), safety_core.h) -- SWD-visible
    // record of what just happened, not link-frame content. Clamp-hit
    // counters only increment when a real measurement (not a nominal_dt_s
    // fallback tick -- have_prev_now_ms/clock_stalled guard that the same way
    // tick_dt_compute_s() itself does) needed clamping; recomputing the raw
    // pre-clamp value here from the SAME pre-update locals used for the call
    // above, rather than threading a third output out of tick_dt_compute_s(),
    // keeps that function's signature to exactly the inputs/output this
    // file's own negative tests need to prove.
    s_dt_last_measured_s = dt_s;
    if (have_prev_now_ms && !clock_stalled) {
        uint32_t raw_elapsed_ms = now_ms - prev_now_ms; // wraparound-safe, same reasoning as above
        float    raw_measured_s = (float)raw_elapsed_ms / 1000.0f;
        if (raw_measured_s > SAFETY_CORE_DT_MAX_S) {
            s_dt_clamped_high_count++;
        } else if (raw_measured_s < SAFETY_CORE_DT_MIN_S) {
            s_dt_clamped_low_count++;
        }
    }

    // 2026-08-27 audit item 2: "stale reading looks fresh" -- ARCHITECTURE.md
    // section 6, "Staleness is checked by the consumer, not the producer."
    // Same treatment context_valid already gets a few lines below, applied
    // here to the thermo snapshot pulled above. Gated on !clock_stalled for
    // the identical reason context_valid is: an age computed against a
    // frozen now_ms reads as ~0 forever, which is the exact "dead producer
    // reads as fresh" failure this check exists to catch, not a case it is
    // allowed to fall silent on.
    bool thermo_fresh =
        !clock_stalled && snapshot_is_fresh(now_ms, thermo.timestamp_ms, THERMO_MAX_AGE_MS);

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
        // F7 (guard review 2026-10-09): expire-once so the 32-bit tick wrap
        // cannot re-arm an old announcement's grace window.
        static bool     s_grace_expired_valid = false;
        static uint32_t s_grace_expired_at_ms = 0u;
        reboot_grace_active = reboot_grace_evaluate(true, announced_at_ms, now_ms,
                                                    REBOOT_GRACE_WINDOW_MS,
                                                    &s_grace_expired_valid,
                                                    &s_grace_expired_at_ms);
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

    // INERT since 2026-09-24: S1 is unconditionally abs_max_temp_c (owner
    // decision -- the Pico's limits must equal the ESP's, never tighter).
    // These two fields are still written but no guard reads them; the
    // original rationale is kept below as history.
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
    //
    // 2026-08-27 audit item 2: same "stale reading looks fresh" treatment as
    // thermo above, applied here too. current_snapshot_t has no per-channel
    // `valid` bit the way thermo_snapshot_t does (ARCHITECTURE.md section 6),
    // so unlike thermo there is no separate producer-honesty field being
    // combined with freshness -- current_fresh alone gates whether this tick
    // trusts the reading at all.
    //
    // DELIBERATE CHOICE (there is no S-current-invalid trip the way S5 exists
    // for thermo, so this needed its own answer, not a copy of thermo's):
    // a stale current snapshot makes any_current_present read FALSE, the
    // same "unknown collapses to the input's own safe-inactive default"
    // treatment context_valid, zone_count, and firing_max_valid already get a
    // few lines above and below in this same function -- SAFETY_MODEL.md
    // section 5's documented language for exactly this shape of unknown is
    // "goes inactive, not pessimistic," which is "reads as absent," not
    // "reads as present."
    //   - S3 (load stuck on, TRIP-capable): any_current_present && !relay_
    //     commanded_recently. Forcing false during staleness means a stale
    //     current reading can NEVER by itself cause an S3 TRIP -- the
    //     dangerous direction (an under-detected weld) would be forcing
    //     TRUE instead, which risks the opposite hazard of accumulating
    //     s3_stuck_elapsed_s against data that is not actually evidence of
    //     anything happening right now. Bounded exposure: this only matters
    //     for the CURRENT_MAX_AGE_MS (2s) window itself -- current_task
    //     resuming normal publishes immediately resumes real S3 coverage,
    //     and a current sensor that is stale for LONGER than that (the
    //     thermo-style repeated-mutex-loss failure) leaves any_current_
    //     present pinned false, which S4's existing WARN
    //     ("relay_commanded_continuously && !any_current_present") already
    //     surfaces as an operator-visible symptom rather than a silent gap
    //     -- a permanently "absent" current reading on a relay that is
    //     genuinely, continuously commanded on is exactly S4's trigger
    //     condition, so this failure does not go unnoticed even though it
    //     produces no NEW trip path of its own.
    //   - S9 (contactor welded, the post-trip "K4 open but current still
    //     flowing" check): also reads any_current_present, so a stale
    //     reading cannot manufacture evidence of a weld it does not actually
    //     have -- correct, since "current_sensing_commissioned == false"
    //     already downgrades S9 to WARN whenever the fact cannot be trusted
    //     (see safety_core_load_guard_cfg()'s own comment on that field);
    //     staleness is one more way the fact cannot be trusted this tick,
    //     and collapsing to false is consistent with that existing gate
    //     rather than inventing a second one.
    //   - S4 (WARN only, never TRIP by SAFETY_MODEL.md section 4's own
    //     design) is the one guard this pushes toward FIRING more readily
    //     during staleness (relay commanded, no current seen) -- accepted:
    //     S4 cannot trip, and per the paragraph above, a current sensor
    //     stuck stale long enough to matter is a real, distinct fault worth
    //     that WARN surfacing anyway.
    bool current_fresh =
        !clock_stalled && snapshot_is_fresh(now_ms, current.timestamp_ms, CURRENT_MAX_AGE_MS);
    bool any_current_present = current_any_present(&current) && current_fresh;

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

    // safety_tc_installed (config param 0x0211) -- config_store_get_full_
    // record() is a cheap cached-copy read (config_store_flash.c), safe to
    // call every 100ms tick, same as config_store_get_tc_type()'s own use
    // elsewhere. Inverted into safety_tc_not_installed_declared here (see
    // that field's own doc comment in safety_guards.h for why the pure
    // module's input struct deliberately uses the opposite polarity from
    // the config field it is derived from). Pulled up here (was previously
    // read AFTER sample_counter_advancing below) because that producer now
    // needs cfg_rec.borrowed_zone_index too.
    config_store_record_t cfg_rec;
    // Return value used ONLY by the item-16 backstop below (cfg_read_ok) --
    // every other reader of cfg_rec in this tick keeps the pre-existing
    // "a missing part must not abort a guard tick" behavior of falling back
    // to config_store_default() either way, so ignoring it for them is
    // correct, not an oversight.
    bool cfg_read_ok = config_store_get_full_record(&cfg_rec);
    // T2 (REVIEW_SAFTYFW_TRIP_PATH_2026-10-10): a failed read yields the default
    // record (fields_set == 0), which would zero S1/S8/S2/S14 config and flip
    // the S5 "declared not installed" suppression for one tick. Keep the last
    // GOOD values instead. Until a first good read exists (s_have_good_guard_cfg
    // false) behaviour is unchanged: the default record is used, as at boot.
    bool safety_tc_not_installed_declared = (cfg_rec.safety_tc_installed == 0u);
    if (cfg_read_ok) {
        s_have_good_guard_cfg = true;
        s_last_good_tc_not_installed_declared = safety_tc_not_installed_declared;
    } else if (s_have_good_guard_cfg) {
        safety_tc_not_installed_declared = s_last_good_tc_not_installed_declared;
    }

    // S13's sample_counter_advancing. GUARD_TEST_MATRIX.md section 6 (row
    // S13) found the actual gap: `borrowed_zone_index` IS a real config_store
    // field (config_store.h/.c, config_params.c param 0x0102) -- the comment
    // that used to sit here, claiming it "does not exist anywhere in this
    // codebase yet", was written before that field landed and went stale.
    // The real gap was always the one-step plumbing GUARD_TEST_MATRIX.md
    // describes: nothing read it back out to decide whether the context
    // frame's per-zone sample_counter was moving. Fixed here by calling
    // snapshots.h's context_borrowed_sample_counter_advancing() -- see that
    // function's own doc comment for the zone_index-vs-array-position
    // subtlety and the "missing zone doesn't reset the remembered counter"
    // rule. Indexed into the file-static last-seen arrays by
    // cfg_rec.borrowed_zone_index itself (always 0..2, config_params.c's
    // CHECK_U8_MAX(2u) on param 0x0102), which is safe to do even when the
    // index is uncommissioned (defaults to 0, config_store.c's zero-record) --
    // have_index below is what actually gates whether the result means
    // anything, not the array indexing.
    bool sample_counter_advancing = context_borrowed_sample_counter_advancing(
        &ctx, context_valid, (cfg_rec.fields_set & CONFIG_STORE_SET_BORROWED_ZONE_INDEX) != 0u,
        cfg_rec.borrowed_zone_index, &s_borrowed_sample_counter_known[cfg_rec.borrowed_zone_index],
        &s_borrowed_last_sample_counter[cfg_rec.borrowed_zone_index]);

    // docs/THERMOCOUPLE.md "Type checking a borrowed channel" / THERMOCOUPLE.md's
    // own open checklist item -- same have_index gate as sample_counter_advancing
    // above (a borrowed zone must be commissioned before its reported tc_type
    // means anything). Logged once on the transition into mismatch, and the
    // latch clears the instant it stops being true so a later, different
    // mismatch (or the same one recurring after a fix-and-break) logs again
    // rather than being silenced forever by the first sighting.
    bool borrowed_type_mismatch = context_borrowed_type_mismatch(
        &ctx, context_valid, (cfg_rec.fields_set & CONFIG_STORE_SET_BORROWED_ZONE_INDEX) != 0u,
        cfg_rec.borrowed_zone_index, cfg_rec.borrowed_type_expected);
    if (borrowed_type_mismatch && !s_borrowed_type_mismatch_warned) {
        log_task_log(LOG_LEVEL_WARN, "safety_core",
                     "borrowed zone tc_type mismatch: main board now reports a different type than "
                     "borrowed_type_expected -- reconfigured underneath us since commissioning");
    }
    s_borrowed_type_mismatch_warned = borrowed_type_mismatch;

    // Carry every commissioned threshold into s_guard_cfg from the SAME
    // record read above, on the same tick the guards are about to run
    // against. Before this call existed, none of them ever arrived and S1
    // could not fire at any temperature -- see
    // safety_core_load_guard_cfg()'s own doc comment.
    if (cfg_read_ok || !s_have_good_guard_cfg) {
        safety_core_load_guard_cfg(&cfg_rec);
    }

    // KILN_PROFILES_PLAN.md item 16, defence in depth -- NOT the primary
    // interlock. The primary refusal already lives in safety_core_request_
    // enable() (commissioning_gate_energize_allowed(), checked BEFORE
    // relay_owner_command_energize(true) is ever called) and in relay_owner
    // itself (opus review d22431d0: an uncommissioned Pico fails the
    // commissioning gate, so the ON direction is refused before ARMED is
    // ever reachable -- confirmed not exploitable today). This is a second,
    // independent backstop at the one place every tick already passes
    // through regardless of how ARMED was reached, in case a future change
    // to relay_owner or the commissioning gate ever defeats that primary
    // path without anyone noticing here.
    //
    // Do NOT "fix" this by inventing a default for abs_max_temp_c --
    // CONFIG_REFERENCE.md section 7's "no default may be a guess dressed as
    // a value" is exactly why S1 itself stays at "0 = never trips" for an
    // unconfigured board (safety_guards.c/.h, deliberate). This check does
    // not touch S1 or safety_guards.c at all: it is a separate, blunt
    // circuit breaker that de-energizes directly through relay_owner (the
    // one module that owns the safety relay -- never a raw GPIO write, same
    // as every other call site here) the instant it observes the specific
    // combination the owner said must never exist: ARMED while the ceiling
    // that is supposed to be enforced was never actually commissioned.
    // "Unconfigured" here uses the SAME fields_set bit safety_core_load_
    // guard_cfg() just used two lines above, so this cannot drift from what
    // S1 itself considers unconfigured (project_reset_one_side_bug_class --
    // two places deriving the same fact independently is exactly how that
    // class of bug starts; this reads the identical bit instead).
    //
    // CORRECTION (2026-09-14 review, Finding C): cfg_read_ok (above) gates
    // this whole block now. config_store_get_full_record() fails CLOSED to
    // config_store_default() -- fields_set == 0, byte-identical to a
    // genuinely never-commissioned record -- whenever config_store_
    // seqlock_read() exhausts both its primary and fallback retries. Proven
    // by execution (standalone harness against the real sources, scratchpad
    // only): a COMMISSIONED, ARMED board driven into that degraded-read
    // state presents this backstop's exact trigger and would have been
    // spuriously force-de-energized mid-firing -- a real availability
    // regression, always in the fail-safe direction, but not the "dead code
    // by construction" this backstop was believed to be. `cfg_read_ok ==
    // false` means "I could not confirm what is commissioned this tick," not
    // "nothing is commissioned" -- those are different claims, and only the
    // second one justifies pulling the relay. On a failed read this backstop
    // now DECLINES to act (skips both the de-energize and the warn-latch
    // update) rather than guessing either way; a genuinely stuck ARMED-and-
    // unconfigured board keeps re-triggering on every tick that DOES get a
    // real snapshot, so this does not weaken the backstop's actual target
    // case, only stops it from misfiring on a transient it was never meant
    // to interpret.
    bool abs_max_temp_c_unconfigured = (cfg_rec.fields_set & CONFIG_STORE_SET_ABS_MAX_TEMP_C) == 0u;
    if (!cfg_read_ok) {
        // Declined: see the correction above. Leave s_unconfigured_armed_
        // warned exactly as it was so a real, already-latched condition is
        // not silently cleared by one bad read, and so a real transition is
        // still reported once a stable snapshot resumes.
    } else if (abs_max_temp_c_unconfigured && relay_owner_get_state() == RELAY_OWNER_STATE_ARMED) {
        if (!s_unconfigured_armed_warned) {
            log_task_log(LOG_LEVEL_ERROR, "safety_core",
                         "unreachable state observed: ARMED with abs_max_temp_c unconfigured -- "
                         "forcing de-energize (KILN_PROFILES_PLAN.md item 16 backstop)");
        }
        s_unconfigured_armed_warned = true;
        (void)relay_owner_command_energize(false);
    } else {
        s_unconfigured_armed_warned = false;
    }

    // "CTs are optional hardware" pass. cts_disabled is true ONLY on the
    // explicit answer -- the CONFIG_STORE_SET_CT_INSTALLED bit present AND
    // the value 0. An unanswered question leaves every CT-fed guard armed,
    // exactly as before this field existed.
    //
    // Read from cfg_rec every tick rather than cached in the config-applied
    // path above, for the same reason safety_tc_installed is read that way
    // (this file's comment at that read): it is a structural fact about the
    // board that must not be able to sit stale across a COMMIT_CONFIG.
    //
    // Everything downstream of the sensor is forced to its no-information
    // state here, at the producer, rather than each guard being asked to
    // remember to ignore its input: any_current_present false, amps_valid
    // false. safety_guards.c ADDITIONALLY branches on the flag itself, which
    // is deliberate redundancy -- forcing the inputs alone would make S3/S9
    // silently "pass" and S4 permanently warn, and the flag alone would leave
    // the raw floor reading reachable. Both halves are required; neither is
    // sufficient.
    bool cts_disabled = ((cfg_rec.fields_set & CONFIG_STORE_SET_CT_INSTALLED) != 0u) &&
                        (cfg_rec.ct_installed == 0u);
    if (cts_disabled) {
        any_current_present = false;
    }

    // 2026-09-18 (owner-directed fix, summed-CT-topology support): mask
    // any_current_present down to FITTED channels only, via config_store_
    // mask_current_present_to_fitted() (config_store.h -- THE single place
    // this masking lives; see its doc comment for the full fail-safe-
    // direction argument, and this file's comment on s_current_sensing_
    // commissioned's assignment for why this half is mandatory alongside
    // that one). Recomputed here from `current.present[]` rather than the
    // earlier current_any_present(&current)-derived value above, because
    // that call already collapsed the three channels into one bool before
    // cfg_rec (read further up this same tick) was available -- same
    // "compute early, mask once config is known" shape the cts_disabled
    // block just above already uses.
    //
    // AND'd into the existing value rather than overwritten: current_fresh/
    // cts_disabled already gate any_current_present above (a stale reading
    // or a fully-disabled board must stay false regardless of this mask),
    // so this can only ever narrow the fact, never widen it back past either
    // existing gate.
    any_current_present =
        any_current_present &&
        config_store_mask_current_present_to_fitted(current.present, !cts_disabled,
                                                      cfg_rec.ct_topology);

    // S14 (COMMISSIONING_UX.md section 3.3). amps[ch] comes straight from
    // current_snapshot_t, gated by the SAME freshness fact any_current_
    // present already uses (current_fresh) -- a stale amps[] reading must
    // not be allowed to arm or clear S14 any more than it may arm/clear
    // S3/S4/S9 above. relay_commanded_now_for_ct[ch] answers "is THIS
    // channel's own mapped relay commanded on right now", read from
    // ctx.relay_now_mask at bit ct_channel_map[ch] -- zone/relay ids and
    // relay_now_mask bit positions are both 0-2 for the three heated zones
    // (config_store.h's own "zone/relay id" comment on ct_channel_map;
    // relay_now_mask's bits 0-3 cover relays 1-4, of which relays 1-3 are
    // the three zone heaters this guard cares about). Requires the whole
    // ct_channel_map group to be commissioned (CONFIG_STORE_SET_CT_CHANNEL_
    // MAP) as well as context_valid -- an uncommissioned map has nothing
    // meaningful to index relay_now_mask with.
    float amps_for_ct[3] = { 0.0f, 0.0f, 0.0f };
    bool  amps_valid_for_ct[3] = { false, false, false };
    bool  relay_commanded_now_for_ct[3] = { false, false, false };
    if (current_fresh && !cts_disabled) {
        for (unsigned ch = 0; ch < 3; ch++) {
            // Summed topology: only channel 2 (index 2, "channel 3"/GPIO28
            // per HARDWARE.md) has a CT behind it at all -- channels 0/1
            // must report "not fitted", never a plausible-looking 0.00 A,
            // same discipline as ct_installed==0's per-channel treatment.
            // Routed through config_store_ct_channel_fitted() (config_store.h)
            // rather than a second inline `ct_topology == SUMMED && ch != 2`
            // check -- this file used to carry exactly that duplicate, and
            // this codebase has a documented history of two sites encoding
            // the same contract drifting apart ("reset one side of a pair").
            // !cts_disabled is already established by the loop guard above,
            // so this is the topology half of the predicate only.
            if (!config_store_ct_channel_fitted((uint8_t)ch, true, cfg_rec.ct_topology)) {
                continue;
            }
            // Opus review finding 1: cs_counts_to_amps() (current_sense.c)
            // returns 0.0f -- a plausible-looking reading, not an error --
            // for any channel whose k_ct_v_per_a has never been
            // commissioned. Without this gate amps_valid_for_ct[ch] went
            // true off freshness alone, so S14/S15 could arm against a
            // fabricated "0.00 A, always below expected" reading the
            // instant a sweep on the OTHER side (the ESP) happened to
            // record a nonzero i_normal_a for this channel -- exactly the
            // "arm on an unvalidated threshold" hazard the owner's standing
            // instruction forbids. Per-channel, not the all-three-AND
            // s_current_sensing_commissioned (that flag answers a
            // different question -- S9's "is the whole set a measurement
            // at all" -- and is deliberately strict across all three
            // channels even in summed topology, where only channel 2 has a
            // CT wired up at all).
            if (!(cfg_rec.k_ct_v_per_a[ch] > 0.0f)) {
                continue;
            }
            amps_for_ct[ch] = current.amps[ch];
            amps_valid_for_ct[ch] = true;
        }
    }
    if (context_valid && (cfg_rec.fields_set & CONFIG_STORE_SET_CT_CHANNEL_MAP) != 0u) {
        for (unsigned ch = 0; ch < 3; ch++) {
            uint8_t relay_id = cfg_rec.ct_channel_map[ch];
            if (relay_id < 3u) {
                relay_commanded_now_for_ct[ch] = (ctx.relay_now_mask & (1u << relay_id)) != 0u;
            }
        }
    }

    // S14 (summed topology)/S15. Per-ZONE commanded-now, straight off
    // ctx.relay_now_mask by zone id (0-2) -- unlike relay_commanded_now_
    // for_ct above this does NOT go through ct_channel_map, because in
    // summed topology every channel maps to the one shared CT, so the map
    // cannot answer "which zones are on" for zones other than whichever one
    // it happens to name. Gated on context_valid alone (not on ct_channel_
    // map being commissioned) since summed mode's S14/S15 do not consult
    // that map at all -- see safety_guards.c's summed-topology block.
    bool relay_commanded_now_for_zone[3] = { false, false, false };
    if (context_valid) {
        for (unsigned z = 0; z < 3; z++) {
            relay_commanded_now_for_zone[z] = (ctx.relay_now_mask & (1u << z)) != 0u;
        }
    }

    // S2/S3/S4 SIM_PLANT disable (TODO.md "Honour the SIM_PLANT flag"). See
    // safety_guard_input_t::sim_plant_disable_active's own comment
    // (safety_guards.h) for the full reasoning -- in short, this ANDs the
    // ESP's own runtime claim (CONTEXT_FLAG_SIM_PLANT, only meaningful while
    // context_valid) with a LOCAL compile-time fact this Pico firmware was
    // built with, so an ESP claiming SIM_PLANT can never disable this
    // board's own guards unless this build was deliberately compiled with
    // SAFTYFW_HONOR_SIM_PLANT (CMakeLists.txt, default OFF). In every
    // production/target build the #if branch below is simply never
    // compiled, so this is `false` unconditionally -- there is no runtime
    // toggle and nothing here can ever weaken a guard in that build.
#if SAFTYFW_HONOR_SIM_PLANT
    bool sim_plant_disable_active = context_valid && (ctx.flags & CONTEXT_FLAG_SIM_PLANT) != 0u;
#else
    bool sim_plant_disable_active = false;
#endif

    return (safety_guard_input_t){
        // 2026-08-27 audit item 2: thermo.valid alone is not enough -- a
        // thermo_task that keeps losing its publish mutex leaves this struct
        // permanently valid == true off the last successful read.
        // thermo_fresh (computed above, alongside clock_stalled) closes that:
        // a stale-but-was-valid reading now collapses to tc_valid == false
        // here, which s5_bad_read_now() (safety_guards.c) already treats as
        // a bad read -- CONFIRMED by reading that function directly (`!in->
        // tc_valid` is one of its four OR'd terms) and by this file's own
        // negative test (test_tick_timing.c) exercising the same s5_bad_
        // read_now() logic against a stale-vs-fresh input. This is the
        // correct fail-safe direction: S5 is graduated (BAD_READ_TIME_S_
        // DEFAULT = 5.0s consecutive-bad AND BLIND_GRACE_S_DEFAULT = 60.0s
        // accumulated before it TRIPS, WARN only before that), so a single
        // merely-slow tick (or even several) cannot nuisance-trip it -- only
        // a genuinely stuck publish, sustained for tens of seconds, does,
        // which is exactly the hazard ("safety processor blind and does not
        // know it") this item exists to make visible instead of silent.
        .tc_valid = thermo.valid && thermo_fresh,
        .tc_c = thermo.tc_c,
        .cj_c = thermo.cj_c,
        .cj_invalid = !thermo.cj_valid || !thermo_fresh, // review saftyfx6 F5
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
        .sim_plant_disable_active = sim_plant_disable_active,
        .zone_count = zone_count,
        .max_zone_setpoint_c = max_zone_setpoint_c,
        .nearest_zone_measured_c = nearest_zone_measured_c,
        .any_current_present = any_current_present,
        .current_sensing_commissioned = s_current_sensing_commissioned,
        .current_sensing_disabled = cts_disabled,
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
        // S14 -- see this function's own comment above the array prep for
        // what these come from and why current_fresh/context_valid gate them.
        .amps = { amps_for_ct[0], amps_for_ct[1], amps_for_ct[2] },
        .amps_valid = { amps_valid_for_ct[0], amps_valid_for_ct[1], amps_valid_for_ct[2] },
        .relay_commanded_now_for_ct = { relay_commanded_now_for_ct[0], relay_commanded_now_for_ct[1],
                                         relay_commanded_now_for_ct[2] },
        .relay_commanded_now_for_zone = { relay_commanded_now_for_zone[0], relay_commanded_now_for_zone[1],
                                           relay_commanded_now_for_zone[2] },
        // S16. config_store_flash.c (core 0, link_task) latches this the
        // moment a SECOND RAM-corruption is found this boot; this is a
        // plain read of that cross-core flag, no debounce or gating of its
        // own needed here -- see safety_guards.h's field comment.
        .config_integrity_trip = config_store_ram_integrity_recurrence_pending(),
        // 2026-08-27 audit item 1: measured (clamped, fallback-safe) dt_s,
        // computed above via tick_dt_compute_s() -- no longer the raw
        // compile-time SAFTYFW_PERIOD_SAFETY_CORE_MS constant. See that call
        // site's own comment block for the full reasoning.
        .dt_s = dt_s,
    };
}

static void safety_core_task(void *arg)
{
    (void)arg;

    safety_guards_reset(&s_guard_state);
    s_have_good_guard_cfg = false;
    s_last_good_tc_not_installed_declared = false;

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
            // ordering is real and not just hoped for. That ordering says
            // nothing about whether the SEND itself lands, though: latch
            // "a trip command is owed" first, unconditionally, then let the
            // retry loop below (which also runs this same tick, before
            // anything else touches K4) make the actual attempt -- see
            // s_trip_command_owed's own doc comment for why a dropped send
            // must not be a one-shot fire-and-forget.
            s_trip_command_owed = true;
            boot_reason_latch_trip((uint32_t)s_guard_state.reason);

            // Capture Frame D's "at the instant of the trip" values right
            // here, same tick, before anything else runs -- LINK_PROTOCOL.md
            // sec 6: "half a second later the temperature has changed... the
            // evidence is gone." s_trip_seq incrementing LAST is deliberate:
            // safety_core_get_trip_event() is lock-free, so a reader must
            // never be able to observe the new seq before the fields it
            // describes are already written.
            // Seqlock write side: odd generation while the fields change, so a
            // reader that overlaps a SECOND trip's capture (old seq, half-new
            // fields) sees the generation move and retries instead of
            // returning a torn mix. The seq itself is bumped after the fields.
            s_trip_gen++;
            HAL_DMB();
            s_trip_reason = s_guard_state.reason;
            s_trip_uptime_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
            s_trip_tc_c = input.tc_valid ? input.tc_c : NAN;
            s_trip_deciding_threshold = safety_guards_deciding_threshold_c(s_guard_state.reason,
                                                                            &s_guard_cfg);
            // F5: release -- every field above must be visible to the other core before the seq is.
            HAL_DMB();
            s_trip_seq = trip_seq_next(s_trip_seq); // wraps 255 -> 1, never 0 (F4); INSIDE the odd window
            HAL_DMB();
            s_trip_gen++; // even again: capture complete

            // Step 4 of SAFETY_MODEL.md section 6's 4-step trip order:
            // log the trip itself. The log_task call below is a 0-tick
            // xQueueSend (see log_task.h's own doc comment) -- exactly the
            // same non-blocking property the CLEAR_TRIP drain below relies
            // on -- so this cannot extend this tick or block behind a full
            // log queue; a full queue just drops the entry (log_task's own
            // dropped-count tracks that) rather than stalling
            // safety_core_task. Deliberately placed AFTER the trip is
            // fully latched above (K4 already commanded via
            // s_trip_command_owed, state and Frame D snapshot already
            // written) so this audit-trail write can never precede or gate
            // the actual trip action.
            char trip_msg[64];
            snprintf(trip_msg, sizeof(trip_msg), "reason=%u tc_c=%.1f uptime_ms=%lu",
                     (unsigned)s_trip_reason, (double)s_trip_tc_c,
                     (unsigned long)s_trip_uptime_ms);
            log_task_log(LOG_LEVEL_ERROR, "trip", trip_msg);
        }

        // Retry a still-owed trip command every tick, independent of
        // newly_tripped (which is only ever true the ONE tick the trip is
        // decided -- see s_trip_command_owed's doc comment). relay_owner_
        // command_trip() itself never blocks (it is a 0-tick xQueueSend),
        // so retrying here costs nothing when the queue is healthy and
        // s_trip_command_owed is already false; when the first send was
        // dropped, this is what keeps re-issuing it every 100ms until
        // relay_owner actually drains its queue and de-energizes K4 --
        // K4 and S9's `relay_owner_is_energized()` mirror stay wrong
        // together for as long as this retry keeps failing, but never
        // silently forever the way a single fire-and-forget send would.
        // Gated on s_guard_state.is_tripped too: if a CLEAR_TRIP lands
        // (below) while the very first send is still stuck in the queue,
        // the guard state has already gone back to "not tripped" and
        // relay_owner itself was never actually commanded into TRIPPED (the
        // send never landed) -- re-issuing a now-stale trip command after a
        // legitimate clear would re-trip a relay that was correctly cleared,
        // which is exactly backwards. relay_trip_command_still_owed()
        // (relay_grace.c, host-tested) is the pure classification of
        // "is_tripped + did this attempt succeed" into "still owed or not".
        if (s_trip_command_owed) {
            bool send_succeeded =
                s_guard_state.is_tripped ? relay_owner_command_trip(s_guard_state.reason) : false;
            s_trip_command_owed =
                relay_trip_command_still_owed(s_guard_state.is_tripped, send_succeeded);
        }

        // CLEAR_TRIP: drain at most one queued request per tick. Must run
        // here, inside safety_core_task, using THIS tick's own `input` --
        // see safety_core_request_clear_trip()'s doc comment in
        // safety_core.h for why it may no longer be called directly from
        // link_task's context. Non-blocking receive: a request sitting in
        // the queue is picked up on the very next 100ms tick, never held up
        // by, and never itself holding up, anything link_task or the link
        // is doing.
        uint16_t clear_trip_token;
        if (xQueueReceive(s_clear_trip_queue, &clear_trip_token, 0) == pdTRUE) {
            // kilnlink audit 2026-10-09 M4: the request's trip occurrence,
            // compared against s_trip_seq HERE, on the task that bumps it,
            // so a trip that latched after the ESP read DIAG (or between
            // link_task's screen and this dequeue) cannot be cleared by a
            // request for the earlier occurrence.
            bool clear_trip_bound = (clear_trip_token & CLEAR_TRIP_TOKEN_BOUND) != 0u;
            uint8_t clear_trip_seq = (uint8_t)(clear_trip_token & 0xFFu);
            bool occurrence_matches = safety_guards_clear_trip_occurrence_matches(
                clear_trip_bound, clear_trip_seq, s_trip_seq);

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
            if (was_tripped && occurrence_matches) {
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
            safety_clear_trip_outcome_t outcome = safety_guards_decide_clear_trip_outcome(
                was_tripped, occurrence_matches, try_clear_result);
            if (outcome == SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED) {
                // Latch "a clear command is owed" unconditionally, the same
                // instant the acceptance is decided -- see s_clear_command_
                // owed's own doc comment for why a bare discarded send here
                // is exactly the bug 4962421 already fixed on the trip side.
                // The retry loop at the bottom of this function (which also
                // runs this same tick, right after this block) makes the
                // actual attempt.
                s_clear_command_owed = true;
            }

            s_clear_trip_processed++;
            s_clear_trip_last_outcome = outcome;

            clear_trip_diag_mark(CLEAR_TRIP_DIAG_STAGE_PRE_OUTCOME_LOG, reason_u8, fault_bits_u8,
                                  tc_valid_snapshot, spi_failed_snapshot, tc_c_is_nan_snapshot,
                                  (uint8_t)outcome);

            char msg[96];
            if (clear_trip_bound) {
                snprintf(msg, sizeof(msg), "req=%lu proc=%lu outcome=%s seq=%u cur=%u",
                         (unsigned long)s_clear_trip_requested,
                         (unsigned long)s_clear_trip_processed, clear_trip_outcome_str(outcome),
                         (unsigned)clear_trip_seq, (unsigned)s_trip_seq);
            } else {
                snprintf(msg, sizeof(msg), "req=%lu proc=%lu outcome=%s unbound",
                         (unsigned long)s_clear_trip_requested,
                         (unsigned long)s_clear_trip_processed, clear_trip_outcome_str(outcome));
            }
            log_task_log(outcome == SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED ? LOG_LEVEL_INFO : LOG_LEVEL_WARN,
                         "clear_trip", msg);

            clear_trip_diag_mark(CLEAR_TRIP_DIAG_STAGE_POST_LOG_TASK, reason_u8, fault_bits_u8,
                                  tc_valid_snapshot, spi_failed_snapshot, tc_c_is_nan_snapshot,
                                  (uint8_t)outcome);
        }

        // Retry a still-owed clear command every tick -- mirrors the trip
        // retry loop above exactly, one direction over. relay_owner_clear_
        // trip() itself never blocks (0-tick xQueueSend, same as the trip
        // side), so retrying here costs nothing when the queue is healthy
        // and s_clear_command_owed is already false; when the send was
        // dropped, this is what keeps re-issuing it every 100ms until
        // relay_owner actually drains its queue and leaves TRIPPED.
        //
        // Gated on !s_guard_state.is_tripped: if a NEW trip has (re-)latched
        // since this clear was decided ACCEPTED -- either the trip retry
        // block above just landed one this same tick, or a fresh guard trip
        // fired on a later tick while this clear was still stuck behind a
        // full queue -- then re-issuing the now-stale clear would let
        // relay_owner_command_energize() re-energize K4 out from under a
        // guard that just said it must stay open, which is exactly
        // backwards. relay_clear_command_still_owed() (relay_grace.c,
        // host-tested) is the pure classification of "is_tripped + did this
        // attempt succeed" into "still owed or not" -- see its doc comment
        // in relay_grace.h for the full trip-beats-clear reasoning, and for
        // why reading s_guard_state.is_tripped fresh every tick (rather than
        // this function consulting s_trip_command_owed directly) is what
        // keeps the two owed-flags from being able to deadlock or oscillate
        // against each other.
        if (s_clear_command_owed) {
            bool send_succeeded =
                !s_guard_state.is_tripped ? relay_owner_clear_trip() : false;
            s_clear_command_owed =
                relay_clear_command_still_owed(s_guard_state.is_tripped, send_succeeded);
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
                                  uint8_t *out_diag_state, uint16_t *out_warn_mask)
{
    // s_guard_state is this task's own local static -- safe to read from any
    // task the same way relay_owner's state is (single-word/small-struct
    // reads of fields only this task's own tick ever writes; no torn-read
    // hazard worse than the volatile-read pattern relay_owner.h already
    // documents for the same reason, and a stale-by-one-tick (100ms) read is
    // immaterial for a diagnostic frame).
    //
    // Opus review of 51c084f/c49bb0e, finding 1: warn_active used to OR only
    // s5_warn/s12_warn -- the two guards this function's own header comment
    // used to (incorrectly, by the time S14/S15 existed) call "the only two
    // guards in this build with a WARN concept". Now derived from the real
    // per-guard mask (safety_guards_warn_mask(), safety_guards.h) so adding
    // a ninth WARN-capable guard later cannot silently leave this OR behind
    // the same way it already had.
    uint16_t warn_mask = safety_guards_warn_mask(&s_guard_state);
    bool warn_active = (warn_mask != 0u);

    if (out_trip_reason) {
        *out_trip_reason = s_guard_state.is_tripped ? s_guard_state.reason : SAFETY_TRIP_NONE;
    }
    if (out_warn_active) {
        *out_warn_active = warn_active;
    }
    if (out_warn_mask) {
        *out_warn_mask = warn_mask;
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
    // Lock-free reader of the trip-event snapshot written by safety_core_task().
    // The seq/fields consistency rule is the seqlock below (s_trip_gen); the
    // writer also bumps s_trip_seq LAST so a new seq implies complete fields.
    // LOW-6: a real seqlock read. The writer holds s_trip_gen odd while it
    // rewrites the fields; re-read the generation after the fields and retry if
    // it was odd or moved. Bounded (the capture is a few stores), so a wedged
    // writer cannot hang the link task: after the bound the last snapshot is
    // returned as-is (at worst one poll sees a torn second trip; the next poll
    // sees the new seq and a consistent set).
    uint8_t seq = 0;
    safety_trip_t reason = SAFETY_TRIP_NONE;
    uint32_t uptime_ms = 0;
    float tc_c = NAN;
    float threshold = NAN;
    for (int attempt = 0; attempt < 64; attempt++) {
        uint32_t g1 = s_trip_gen;
        HAL_DMB(); // F5: acquire -- the fields below are read only after the generation/seq
        seq = s_trip_seq;
        reason = s_trip_reason;
        uptime_ms = s_trip_uptime_ms;
        tc_c = s_trip_tc_c;
        threshold = s_trip_deciding_threshold;
        HAL_DMB();
        uint32_t g2 = s_trip_gen;
        if ((g1 & 1u) == 0u && g1 == g2) {
            break;
        }
    }

    if (out_trip_seq) {
        *out_trip_seq = seq;
    }
    if (seq == 0) {
        // No trip yet this boot -- every other output is meaningless
        // (the documented "false, everything zeroed" contract).
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
        *out_trip_reason = reason;
    }
    if (out_uptime_ms) {
        *out_uptime_ms = uptime_ms;
    }
    if (out_tc_c) {
        *out_tc_c = tc_c;
    }
    if (out_deciding_threshold) {
        *out_deciding_threshold = threshold;
    }
    return true;
}

// See safety_core.h's doc comment on this function for the full "why a
// queue, not a direct call" history -- this is now purely an enqueue, never
// a direct touch of s_guard_state. safety_core_task() (below) is the only
// code that actually runs safety_guards_try_clear().
bool safety_core_request_clear_trip(bool bound, uint8_t trip_seq)
{
    if (s_clear_trip_queue == NULL) {
        return false;
    }

    // See s_clear_trip_queue's own doc comment: link_task already validated
    // trip_mask; the token carries only the request's trip occurrence
    // (kilnlink audit 2026-10-09 M4), checked at dequeue.
    uint16_t token = bound ? (uint16_t)(CLEAR_TRIP_TOKEN_BOUND | trip_seq) : 0u;
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
    case SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STALE_OCCURRENCE:
        return "refused-stale-occurrence";
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

    // 2026-09-15 Opus re-review N2: refuse the ON direction while a tc_type
    // apply (config_store flash write + MAX31856 reconfigure) is in
    // progress -- see s_tc_type_apply_in_progress's own doc comment. Same
    // "ON direction only" shape as the update-transfer interlock just
    // above.
    if (enable && s_tc_type_apply_in_progress) {
        log_task_log(LOG_LEVEL_WARN, "request_enable",
                     "refused: tc_type apply in progress");
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

        // ROADMAP.md M12, "An uncommissioned safety processor refuses
        // heating enable" -- the owner's answer was an unqualified NO, and
        // this is that refusal. Same ON-direction-only shape as the two
        // interlocks above: a board that cannot honestly claim to have been
        // commissioned must still be able to DE-energize on command.
        //
        // "Commissioned" is commissioning_gate.h's two-part definition (the
        // stored calibration_missing verdict AND config_params_all_required_
        // set() recomputed from fields_set, which must agree) -- not "the
        // numbers look plausible". An uncommissioned board's abs_max_temp_c
        // is 0, which S1 correctly reads as "never trip" (safety_guards.h);
        // granting heat under a ceiling that can never fire is precisely
        // the state this refusal exists to make unreachable.
        //
        // The TODO that used to sit here warned this must land LAST, after
        // a real commissioning pass had succeeded on the board -- it has
        // (the four-question commissioning page, 64d0a8e), so the refusal
        // is now sequenced in. A bench board that has never been
        // commissioned will refuse heat until it is; that is the intent.
        if (!commissioning_gate_energize_allowed(enable, &cfg_rec)) {
            log_task_log(LOG_LEVEL_WARN, "request_enable",
                         "refused: safety processor not commissioned");
            return false;
        }
    }

    // Record "the Pico just received and is about to act on an enable
    // request" BEFORE forwarding -- see s_last_enable_true_request_ms's own
    // doc comment. Recorded on every accepted-past-the-refusals enable=true
    // call regardless of relay_owner_command_energize()'s own return (even
    // a dropped-queue command means the ESP believes it holds a grant, and
    // this timestamp exists precisely to distrust "not energized yet" for a
    // short window after that).
    if (enable) {
        s_last_enable_true_request_ms = to_ms_since_boot(get_absolute_time());
        s_last_enable_true_request_seen = true;
    }

    // See safety_core.h's doc comment: deliberately a thin forward beyond
    // the check above, no second policy layer duplicating relay_owner's
    // own state machine. relay_owner_command_energize() already refuses
    // while TRIPPED, accepts-but-never-applies during GRACE, and only
    // actually drives GPIO6 high while ARMED.
    return relay_owner_command_energize(enable);
}

// See safety_core.h's doc comment.
void safety_core_set_tc_type_apply_in_progress(bool in_progress)
{
    s_tc_type_apply_in_progress = in_progress;
}

// See safety_core.h's doc comment.
uint32_t safety_core_ms_since_last_enable_true_request(bool *out_ever_seen)
{
    if (out_ever_seen) {
        *out_ever_seen = s_last_enable_true_request_seen;
    }
    if (!s_last_enable_true_request_seen) {
        return UINT32_MAX;
    }
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    return now_ms - s_last_enable_true_request_ms; // wraparound-safe unsigned subtraction
}

bool safety_core_start(void)
{
    // Created before the task itself -- safety_core_request_clear_trip()
    // (callable from link_task the instant this returns true) must never
    // observe a NULL queue that safety_core_task would have created for
    // itself a moment later.
    s_clear_trip_queue = xQueueCreate(SAFETY_CORE_CLEAR_TRIP_QUEUE_LEN, sizeof(uint16_t));
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
