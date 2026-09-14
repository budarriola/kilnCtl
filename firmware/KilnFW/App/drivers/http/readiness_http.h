// readiness_http -- TODO.md 8.3's "is this kiln ready to fire?" status page.
//
// This is a READ-ONLY aggregator, same spirit as nvs_report.c: it owns no
// config of its own and duplicates no storage. Every item on the checklist
// is computed by calling the read-only getters zones_http.h/profiles_http.h/
// wifi_prov.h/nvs_report.h/dashboard_http.h already expose to other
// consumers (profiles_http.c's feasibility check, profile_executor.c,
// autotune_engine.c, /api/status) -- this module just asks the same
// questions those consumers already ask and renders the answers as a
// checklist instead of gating a control decision on them.
//
// Serves GET /readiness (the page) and GET /api/readiness (the JSON the page
// polls). See readiness_http.c's status_get_handler() doc comment for the
// exact JSON shape and the four possible per-item status values.
//
// WHAT "BLOCKING" MEANS HERE -- read this before adding an item or relying
// on one (rewritten 2026-09-09 when the owner made four of these items a
// REAL firing interlock; the full statement lives in docs/SAFETY_CASE.md
// sec 3 item 10):
//
//   An item called "blocking" in this file reports READY_NOT_DONE whenever
//   its condition holds -- never a partial or informational status.
//
//   For FOUR of them that now also stops a firing. `recovery_mode`,
//   `safety_trip`, `crash_report` and `estop_verified` are consumed by
//   App/drivers/safety/readiness_gate.h, which profile_executor_run() calls
//   before anything else -- so every start path (POST
//   /api/profile_exec/start, both LCD start buttons, the benchproto RUN
//   command) refuses while any of the four reads READY_NOT_DONE, naming the
//   item. There is deliberately NO override of any kind.
//
//   For EVERY OTHER item it still means only that the CHECKLIST is
//   incomplete. guard_max_temp, hardware, safety_context, cfg_fs, network,
//   commissioning and the rest do not stop anything, on purpose:
//   guard_max_temp in particular already has a better, profile-aware refusal
//   inside profile_executor_run() (the guard-5 zone-ceiling check).
//
//   ADDING AN ITEM HERE DOES NOT GATE ANYTHING. The gate names its four
//   items explicitly (readiness_gate.h's READINESS_GATE_KEY_*), and
//   promoting an item into that set is an owner decision, not a refactor.
//   Do not add an item here and consider a hazard covered by having added
//   it.
//
//   THE ONE RULE THAT MUST NOT BE BROKEN: the gate does not re-implement any
//   of these decisions. It calls the SAME readiness_*_status() predicate
//   below that readiness_http.c's handler calls to render the item, and
//   blocks iff that predicate says READY_NOT_DONE. If you change a
//   predicate, you have changed what the board will refuse to fire on.
//   check_readiness_gate_display_agreement.ps1 and test_readiness_gate.c
//   exist to keep the page and the interlock from ever disagreeing.
#ifndef READINESS_HTTP_H
#define READINESS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The four per-item status values TODO.md 8.3 requires. Exposed here (rather
 * than staying file-static in readiness_http.c) only so the pure predicate
 * below can be host-tested without dragging in esp_http_server. */
typedef enum {
    READY_OK = 0,
    READY_NOT_DONE,
    READY_CANNOT_YET,
    READY_DELIBERATELY_OFF,
} readiness_status_t;

/* Pure decision for the "Safety processor commissioned" item, split out of
 * the handler so its logic can be tested on the host -- the handler around it
 * is all httpd plumbing and NVS reads that cannot run off-target.
 *
 * `cached_crc` is safety_cfg_store_cached_crc() (0 == never fetched, which is
 * never a real CRC), `unset_count`/`param_count` come from walking
 * safety_cfg_store_get_by_index(). `link_up` gates the never-fetched case:
 * with the safety link down the ESP genuinely cannot distinguish an
 * uncommissioned Pico from one it has simply not talked to yet, so that
 * combination is CANNOT_YET rather than an accusation of unfinished work.
 *
 * static inline (rather than a symbol in readiness_http.c) for the same
 * reason dashboard_safety_ready() is: readiness_http.c itself cannot compile
 * on the host, since it pulls in esp_http_server and the whole NVS stack. */
/* Wire ids for the six current-transformer params (config_params.c:
 * ct_channel_map[0..2] = 0x0106-0x0108, i_normal_a[0..2] = 0x031A-0x031C).
 * These are the ONLY safety-config params whose "required for commissioning"
 * status depends on the board's hardware configuration rather than being
 * unconditionally required -- see
 * readiness_param_required_for_commissioning()'s comment below. */
#define READINESS_PARAM_ID_CT_CHANNEL_MAP_0 0x0106u
#define READINESS_PARAM_ID_CT_CHANNEL_MAP_1 0x0107u
#define READINESS_PARAM_ID_CT_CHANNEL_MAP_2 0x0108u
#define READINESS_PARAM_ID_I_NORMAL_A_0     0x031Au
#define READINESS_PARAM_ID_I_NORMAL_A_1     0x031Bu
#define READINESS_PARAM_ID_I_NORMAL_A_2     0x031Cu

/* Whether an unset `param_id` counts as "commissioning incomplete", given
 * this board's `ct_installed` setting.
 *
 * ct_channel_map[0..2] is required for commissioning ONLY when ct_installed
 * != 0 -- this is not a guess, it is the SAME rule
 * firmware/SaftyFW/src/config_params.c's config_params_all_required_set()
 * already applies to decide calibration_missing (and therefore
 * commissioned:false on the wire): "which relay does each CT watch" is a
 * question with no answer on a board that has no CTs, so requiring it there
 * made a CT-less board permanently uncommissionable (that function's own
 * comment on CONFIG_STORE_SET_CT_INSTALLED joining its required mask).
 *
 * i_normal_a[0..2] is not in that Pico-side required mask at all -- S14, the
 * guard that consumes it (safety_guards.c), treats a channel with no
 * measured normal as simply inactive, never faulted, so the Pico never
 * blocks commissioning on it either way. This item nonetheless gates it the
 * same way as ct_channel_map (required only when ct_installed != 0) rather
 * than never requiring it: a board that DOES have CTs fitted should still be
 * nudged to record the per-channel normal so S14 can actually do its job,
 * and a board with no CTs can never produce a real measurement for it in the
 * first place.
 *
 * Before this existed, a board with ct_installed=0 (no current transformers
 * fitted -- a real, correctly-reported hardware configuration, not a fault)
 * permanently failed this readiness item: these six params can never be set
 * on such a board (there is nothing to map, nothing to measure), so the item
 * counted 6 "unset" params forever and reported NOT COMMISSIONED even once
 * every field the safety processor actually requires was filled in and the
 * Pico itself reported commissioned:true.
 *
 * `ct_installed_value` follows config_store.c's own default-safe-direction
 * rule: when the ct_installed param is itself unset, the record defaults to
 * 1 (installed) rather than 0, so an unanswered "are CTs fitted?" question
 * never relaxes these six params' requirement. Callers that have not yet
 * determined ct_installed's cached value should therefore pass 1, not 0.
 *
 * `ct_topology_value` mirrors firmware/SaftyFW/src/config_params.c's
 * config_params_all_required_set() (0x031F, CONFIG_STORE_CT_TOPOLOGY_SUMMED
 * = 1u there -- this header has no shared include with the Pico build, so
 * the value is compared by literal, same as zone_cfg_committed_ct_topology()
 * and dashboard_ct_topology_is_summed() already do against this same param
 * id elsewhere in this driver). Pico-side, 2026-09-09 (commit b5cb83a4) made
 * ct_channel_map[0..2] required ONLY when ct_installed != 0 AND
 * ct_topology != SUMMED: in summed mode the one shared CT is read by zone id
 * straight into S14/S15, never through ct_channel_map, so a fully-configured
 * summed board has no meaningful answer for "which relay does each CT
 * watch" and was left permanently uncommissionable by the Pico's OLD rule
 * (docs/audits/commissioning_gap_and_no_heat_2026-09-09.md). This ESP-side
 * copy of the same rule was not updated in that pass -- fixed here so a
 * summed board's readiness item 10a agrees with the Pico's own
 * commissioned:true instead of counting ct_channel_map[0..2] as
 * unset-forever. i_normal_a[0..2] is NOT re-gated on topology: unlike the
 * map, it still requires a live current measurement to fill in regardless
 * of topology, and the Pico's own required mask never included it either
 * way -- see the paragraph above.
 *
 * Callers that have not yet determined ct_topology's cached value should
 * pass 0 (PER_ZONE): the unconditional branch, so an unknown topology never
 * relaxes the requirement, matching the ct_installed default-safe rule. */
static inline bool readiness_param_required_for_commissioning(uint16_t param_id, uint8_t ct_installed_value,
                                                                uint8_t ct_topology_value)
{
    switch (param_id) {
    case READINESS_PARAM_ID_CT_CHANNEL_MAP_0:
    case READINESS_PARAM_ID_CT_CHANNEL_MAP_1:
    case READINESS_PARAM_ID_CT_CHANNEL_MAP_2:
        return ct_installed_value != 0u && ct_topology_value == 0u;
    case READINESS_PARAM_ID_I_NORMAL_A_0:
    case READINESS_PARAM_ID_I_NORMAL_A_1:
    case READINESS_PARAM_ID_I_NORMAL_A_2:
        return ct_installed_value != 0u;
    default:
        return true;
    }
}

static inline readiness_status_t readiness_commissioning_status(bool link_up, uint16_t cached_crc,
                                                                size_t unset_count, size_t param_count)
{
    if (cached_crc == 0) {
        return link_up ? READY_NOT_DONE : READY_CANNOT_YET;
    }
    /* A zero param_count would make "all parameters have values" vacuously
     * true; treat it as not-done rather than showing a green light for a
     * table that does not exist. */
    if (param_count == 0) {
        return READY_NOT_DONE;
    }
    return (unset_count == 0) ? READY_OK : READY_NOT_DONE;
}

/* Pure decision for the "Guard limits (max_temp_c)" item (TODO.md 96's
 * fail-open/fail-closed reconciliation, 2026-09-06). `thermo_count` is the
 * number of configured zones; `heating_unset_count` is how many of them can
 * heat (control_mode != OFF) yet have max_temp_c == 0; `set_count` is how
 * many have an explicit (>0) ceiling.
 *
 * A zone that can heat with max_temp_c == 0 is "uncommissioned", and
 * profile_executor_run()'s guard-5 refusal (zones_config_accessors.h's doc
 * comment on zones_config_get_temp_limits) already refuses to start ANY
 * firing while such a zone is active. Reporting that state as ok or
 * deliberately_off, as this item did before the reconciliation, told the
 * operator the board was ready when starting a firing would be refused
 * immediately -- so any such zone forces NOT_DONE regardless of the other
 * zones' state. Only once every heating zone has an explicit ceiling (or
 * every zone with max_temp_c == 0 is OFF and therefore can never trip
 * guard 5's refusal) is the item OK/DELIBERATELY_OFF. */
static inline readiness_status_t readiness_guard_max_temp_status(uint8_t thermo_count, uint8_t set_count,
                                                                  uint8_t heating_unset_count)
{
    if (thermo_count == 0) {
        return READY_CANNOT_YET;
    }
    if (heating_unset_count > 0) {
        return READY_NOT_DONE;
    }
    return (set_count == thermo_count) ? READY_OK : READY_DELIBERATELY_OFF;
}

/* 2026-09-14 owner decision: "the Pico ceiling must ALWAYS equal the ESP's,
 * there should never be a way that the pico is not armed" and "if a config
 * doesn't land and match on both sides then alarm and dissable heaters."
 * Supersedes the older "Pico must never be TIGHTER than the ESP" standing
 * invariant (safety_ceiling_policy.h, docs/audits/safety_ceiling_sync_
 * 2026-09-10.md): equality (format version + hash match, config_
 * divergence.h) is now the required state, not merely "wide enough".
 *
 * `diverged` is safety_ceiling_sync_is_diverged()'s own live verdict --
 * deliberately NOT recomputed here from raw float values a second, parallel
 * way (that would be exactly the "reset one side of a pair" class CLAUDE.md
 * warns about: two independent implementations of "do they match" that can
 * silently drift apart). The ENFORCEMENT (heaters actively disabled) and
 * this DISPLAY read the exact same boolean, computed once, in one place.
 * Already folds in every sub-case: no zone configured yet (not diverged,
 * nothing to compare), the Pico never having confirmed a value (diverged --
 * "an unarmed Pico is a divergence by definition"), and an outright hash
 * mismatch. Link-down is reported CANNOT_YET rather than NOT_DONE (same
 * "cannot tell uncommissioned from unread" reasoning readiness_
 * commissioning_status() already uses) precisely because a bench operator
 * cannot fix a dead link by taking a commissioning action. */
static inline readiness_status_t readiness_ceiling_match_status(bool link_up, bool diverged)
{
    if (!diverged) {
        return READY_OK;
    }
    return link_up ? READY_NOT_DONE : READY_CANNOT_YET;
}

/* Pure decision for the "Safety processor trip status" item (2026-09-08 live
 * dry run, docs/audits/setup_wizard_live_dryrun_2026-09-08.md): the old
 * "Hardware present and answering" item (dashboard_http_get_hw_ready()) only
 * asks whether the safety link is UP, never whether the processor on the
 * other end has an ACTIVE trip latched. A board with S5 tripped
 * (diag_trip_mask nonzero) answered link_up=true and safety_ready=true, so
 * every readiness item read ok/not_done for unrelated reasons and the
 * wizard's completeness gate -- which only refuses on an outstanding
 * not_done/cannot_yet item -- saw nothing to refuse on. complete=true,
 * reasons=[] on a kiln that would immediately refuse to fire.
 *
 * A live trip always reads READY_NOT_DONE whenever it is known (blocking in
 * BOTH senses since 2026-09-09: it makes the checklist incomplete AND the
 * firing interlock refuses a start on it -- see the top comment. The Pico's
 * own refusal to enable heat, and profile_executor_run()'s
 * relay_authority_on_blocked() check, already stopped a tripped board from
 * heating; the interlock is what makes the REFUSAL say so): this is not a "some zones, some not" partial-credit item like
 * max_temp_c above, because ANY tripped guard means the safety processor is
 * currently refusing to let the main board command heat at all -- there is
 * no such thing as a kiln that is "partially ready to fire" while tripped.
 *
 * `link_up` gates the same way the commissioning item's cached_crc==0 case
 * does: with the link down the ESP has no current diag_trip_mask to trust
 * (safety_link_get_status()'s link_up field already carries the staleness
 * gate, per dashboard_safety_ready()'s doc comment), so it cannot tell
 * "not tripped" from "do not know" -- reporting not_done here would be a
 * confident answer to a question the board cannot currently answer, exactly
 * backwards from the bug being fixed. That combination is CANNOT_YET, not
 * a green light -- the separate "hardware present and answering" item is
 * what already reports "safety=down" as not_done, so a dead link is never a
 * bare pass, it just isn't double-reported as this item's fault too. */
static inline readiness_status_t readiness_safety_trip_status(bool link_up, uint16_t diag_trip_mask)
{
    if (!link_up) {
        return READY_CANNOT_YET;
    }
    return (diag_trip_mask != 0u) ? READY_NOT_DONE : READY_OK;
}

/* Pure decision for the "Unacknowledged crash report" item, 2026-09-08's
 * follow-on to the safety_trip fix above -- that pass named four remaining
 * blind spots where readiness disagreed with what another layer (here,
 * capability_preflight, tools/PcTools/src/kilnctrl/capability_preflight.py)
 * already knows and already refuses on. `capability_preflight` refuses to
 * start a run on a board with an unacknowledged crash regardless of what the
 * run needs; readiness had no item for it at all, so a board could show
 * every light green while a different layer was already blocking it -- the
 * exact split that let a live trip through before safety_trip existed.
 *
 * Unconditionally blocking (READY_NOT_DONE) whenever an unacknowledged
 * record exists: an unreviewed panic is not a "some zones ready" partial
 * state, and the operator has an unambiguous, always-available action
 * (POST /api/crash_report/ack, or /clear) to resolve it -- there is no
 * "cannot tell yet" case here the way a Pico-side fact can be unknown while
 * the link is down. `have_record` false (crash_report_get() found nothing,
 * or has never been asked) reads ok, same as "no trip" reads ok above. */
static inline readiness_status_t readiness_crash_report_status(bool have_record, bool acknowledged)
{
    if (have_record && !acknowledged) {
        return READY_NOT_DONE;
    }
    return READY_OK;
}

/* Pure decision for the "Recovery-mode boot" item (boot_guard.h,
 * RECOVERY_MODE_ENABLED): a board that booted into recovery mode has
 * deliberately skipped starting profile_executor/autotune_engine/rules_task
 * (main_boot_early.c's boot_guard_is_recovery_mode() gate) -- it cannot fire
 * a profile, run autotune, or serve most of the cfg mount's normal
 * consumers no matter what every other readiness item says, so this is
 * always READY_NOT_DONE whenever true (blocking in both senses since
 * 2026-09-09 -- the firing interlock refuses on it, and the underlying
 * protection is still main_boot_early.c not starting those subsystems). There is no "cannot tell yet"
 * case: boot_guard_is_recovery_mode() is a local, always-answerable fact
 * about THIS boot (stable for its lifetime, per that function's own doc
 * comment), never dependent on a link or a peer that could be down. */
static inline readiness_status_t readiness_recovery_mode_status(bool recovery_mode)
{
    return recovery_mode ? READY_NOT_DONE : READY_OK;
}

/* Pure decision for the "Config filesystem (cfg_fs)" item (cfg_fs.h,
 * docs/FILESYSTEM_USER_DATA_PLAN.md): user config (zones, kiln_cfg_store,
 * profiles, and the pref-backed items) is now file-backed on the `cfg`
 * LittleFS partition with NVS as the dual-write mirror. A mount failure
 * (CFG_FS_STATUS_UNAVAILABLE) or an init that was simply never reached
 * (CFG_FS_STATUS_UNMOUNTED, at the point this item is evaluated -- boot has
 * long since called cfg_fs_init()) means every *_cfg_fs.c bridge falls back
 * to reading/writing its NVS copy alone, per each bridge's own documented
 * fallback contract -- degraded, not broken: the board keeps loading and
 * saving config, just without the file-backed copy or the divergence
 * checking that copy enables.
 *
 * Chosen NON-BLOCKING (READY_DELIBERATELY_OFF rather than READY_NOT_DONE):
 * unlike a live trip or recovery mode, there is nothing here that stops a
 * firing, and the operator has no action that fixes it from the readiness
 * page (a failed LittleFS mount is not cleared by re-clicking anything --
 * see cfg_fs_format_http.h for the one recovery path, a deliberate reformat,
 * which is a destructive action this page should never nudge anyone toward
 * as if it were a routine checklist step). Following the same idiom the
 * "Thermocouple calibration offsets" item above already uses for "nothing
 * is missing and no red cross is warranted, but the operator should still be
 * able to see it": DELIBERATELY_OFF, not a bare OK, so the detail string
 * stays visible on the page instead of the item disappearing into an
 * indistinguishable green light. */
static inline readiness_status_t readiness_cfg_fs_status(bool mounted)
{
    return mounted ? READY_OK : READY_DELIBERATELY_OFF;
}

/* SAFETY_POLL_PERIOD_MS default (settings.h) is 500 ms, and
 * safety_link_poll.c sends SAFETY_CMD_PUSH_CONTEXT on that same cadence
 * (its own comment: "same cadence as the GET_STATUS poll above"), so a
 * healthy diag_context_age_100ms should never drift far past one or two
 * poll periods plus scheduling jitter -- a few hundred ms, not seconds. This
 * threshold (10 s = 100 in these units) is an order of magnitude past that,
 * comfortably below the field's own 254 (25.4 s) saturation ceiling
 * (link_task_context_age_100ms() in SaftyFW, which clamps rather than wraps
 * specifically so a genuinely stale age is never misread as the 255 "never
 * received" sentinel) -- so a value at or beyond it cannot be ordinary
 * jitter, only a sustained failure to land PUSH_CONTEXT despite GET_STATUS
 * still answering. */
#define READINESS_CONTEXT_STALE_100MS 100u

/* Pure decision for the "Safety link command delivery" item -- the fourth
 * 2026-09-08 blind spot: "a wedged-but-technically-up link (frames flowing
 * but commands ignored) is indistinguishable from healthy." GET_STATUS
 * replies (what link_up measures) and PUSH_CONTEXT delivery (what actually
 * carries setpoints/relay commands TO the Pico) are two different frame
 * exchanges on the same wire -- a Pico that keeps answering GET_STATUS while
 * silently failing to parse/apply PUSH_CONTEXT (a firmware bug on either
 * side, a framing issue specific to the longer context payload, etc.) reads
 * link_up=true and every other item that only checks link_up would read
 * clean, exactly the split this pass exists to close.
 *
 * diag_context_age_100ms (Frame B, byte 11) is the one honest signal
 * available for this: it is the Pico's OWN report of how long since it last
 * parsed a well-formed PUSH_CONTEXT, not something this ESP infers from its
 * own send-side counters (which would only prove the ESP attempted to send,
 * never that the Pico did anything with it). No round-trip command-ack or
 * sequence-counter equivalent exists for PUSH_CONTEXT today -- it is sent as
 * a broadcast, not a request/reply exchange (safety_link_poll.c's own
 * comment: "independent of whether the exchange above got a reply: this is
 * a broadcast, not part of that request/reply pairing") -- so this is the
 * real signal, not an invented proxy.
 *
 * `link_up` gates it the same way every other Pico-dependent item above
 * does: a down link cannot distinguish "context wedged" from "nothing has
 * been exchanged at all," so that combination reads CANNOT_YET.
 * `diag_ever_received` false means no DIAG frame (Frame B) has arrived this
 * boot at all -- an older Pico build that predates Frame B, or one this ESP
 * has not yet heard from -- and diag_context_age_100ms is meaningless in
 * that case, so it also reads CANNOT_YET rather than trusting a
 * zero-initialized field as a false "healthy." Only once both a link and a
 * real DIAG frame exist does the age value mean anything to gate on. */
static inline readiness_status_t readiness_safety_context_status(bool link_up, bool diag_ever_received,
                                                                  uint8_t diag_context_age_100ms)
{
    if (!link_up || !diag_ever_received) {
        return READY_CANNOT_YET;
    }
    return (diag_context_age_100ms >= READINESS_CONTEXT_STALE_100MS) ? READY_NOT_DONE : READY_OK;
}

/* Pure decision for the "E-stop interlock verified" item, 2026-09-08's
 * follow-on to 3b5ced00: that pass made the FIRMWARE side of E-stop
 * test-locked (relay_owner de-energises on TRIP, negative-tested), but the
 * board only provides POLE 2 (GPIO9/R10/C3/J1) -- POLE 1, in series with the
 * external line contactor's coil, is wiring the OWNER adds and firmware
 * structurally cannot observe. No amount of host testing or GPIO reading can
 * ever cover that pole, so the honest coverage is a documented bench
 * procedure (firmware/SaftyFW/README.md) plus a durable, DELIBERATE record
 * that a human ran it -- see estop_verification.h for the full record
 * lifecycle (set only by POST /api/estop/verify, cleared by a polarity
 * commit or a kiln/all-scope factory reset).
 *
 * UNCONDITIONALLY BLOCKING (READY_NOT_DONE) whenever unverified -- considered
 * and rejected: gating this on whether a firing has ever been attempted (so
 * a bench that has never fired reads merely informational) sounds appealing,
 * but the only readily-available "has this board ever fired" signal in this
 * codebase is relay_cycles.c's per-relay cycle counts, and those are
 * DELIBERATELY operator-resettable (relay_cycles_reset(), "a contact was
 * replaced"). Building a safety-blocking/informational split on top of a
 * counter the operator is expected to zero for routine maintenance would let
 * a legitimate maintenance action silently downgrade this item back to
 * informational after real firings had already happened -- exactly the
 * "reset one side of a pair" bug class this codebase has been burned by four
 * times already (see CLAUDE.md's standing note on that class). Inventing a
 * SEPARATE, non-resettable "has ever fired" bit just to support a grace
 * period would be new state solely in service of relaxing a safety item,
 * which is the wrong direction to add complexity in. This readiness page's
 * entire stated purpose (this header's own top comment: "is this kiln ready
 * to fire?") already implies "before you fire" for every other item on it
 * that reads NOT_DONE unconditionally (guard_max_temp, safety_trip,
 * crash_report, recovery_mode). As of 2026-09-09 this item is no longer
 * advisory: it is one of the four the firing interlock refuses on
 * (readiness_gate.h), and it is the only one of the four with no OTHER
 * enforcement anywhere -- which is exactly why the owner asked for the
 * interlock. There is no reason for the E-stop item alone to
 * carry a bench exemption the rest of the page does not offer, and the cost
 * of one extra confirmation click on a board that has never seen line
 * voltage is far smaller than the cost of this item going quiet exactly when
 * it starts to matter. */
static inline readiness_status_t readiness_estop_verification_status(bool verified)
{
    return verified ? READY_OK : READY_NOT_DONE;
}

/* Registers /readiness + GET /api/readiness on the server
 * wifi_provision_http.c already started. No hardware pointers needed --
 * every hardware-adjacent fact (io_ready/thermo_ready/safety_ready) is read
 * through dashboard_http_get_hw_ready() rather than owned here. Call after
 * zones_http_start()/profiles_http_start()/wifi_prov_start()/
 * dashboard_http_start()/nvs_report_capture() -- this only reads what those
 * have already established, same ordering rule nvs_report_capture() itself
 * documents. */
esp_err_t readiness_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // READINESS_HTTP_H
