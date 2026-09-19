/* readiness_gate_collect() -- the one part of the firing interlock that
 * touches the live board. Everything that DECIDES anything lives in
 * readiness_gate.h as pure code; this file is deliberately nothing but seven
 * reads and a struct fill, so there is no logic here for a host test to be
 * unable to reach.
 *
 * Split out for exactly that reason: dashboard_http.h pulls in
 * esp_http_server, and crash_report.c/estop_verification.c pull in the NVS
 * stack, none of which compile on the host. Keeping them on this side of the
 * line is what lets profile_executor_run.c call the gate and lets
 * test_readiness_gate.c / test_profile_executor_prestart.c drive the real
 * rule with a fake body for this one function.
 *
 * FAIL-SAFE DIRECTION of each read, since this runs at firing-start time and
 * may run before the module it reads has been started:
 *   - boot_guard_is_recovery_mode(): a local fact about this boot, fixed for
 *     its lifetime, always answerable.
 *   - dashboard_http_get_safety_trip(): documented safe before
 *     dashboard_http_start(); reads link_up=false, which
 *     readiness_safety_trip_status() maps to CANNOT_YET, not NOT_DONE. That
 *     is deliberately NOT a block -- see that predicate's own doc comment:
 *     a down link is already refused, in better words, by
 *     profile_executor_run()'s relay_authority_on_blocked() check, and
 *     blocking here too would report the wrong item.
 *   - crash_report_get(): false (no record) when the store was never opened,
 *     which reads OK. The independent capability_preflight refusal still
 *     covers a board whose HTTP API is up.
 *   - estop_verification_is_verified(): false unless a record loaded
 *     cleanly, i.e. a failed load reads exactly like "never verified" --
 *     that module's own documented fail-safe direction, and the reason this
 *     item can never be accidentally satisfied by a storage fault.
 *   - pico_auto_update_state_is_blocking(): false (never blocks) until
 *     docs/PICO_AUTO_UPDATE_PLAN.md's boot-time glue is wired -- see that
 *     module's own doc comment. */

#include "readiness_gate.h"

#include <string.h>

#include "boot_guard.h"
#include "ct_verify_store.h"
#include "crash_report.h"
#include "dashboard_http.h"
#include "estop_verification.h"
#include "pico_auto_update_state.h"
#include "safety_ceiling_sync.h"

void readiness_gate_collect(readiness_gate_facts_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->recovery_mode = boot_guard_is_recovery_mode();

    bool link_up = false;
    uint16_t trip_mask = 0;
    dashboard_http_get_safety_trip(&link_up, &trip_mask);
    out->safety_link_up = link_up;
    out->safety_trip_mask = trip_mask;

    crash_report_record_t rec;
    memset(&rec, 0, sizeof(rec));
    out->crash_have_record = crash_report_get(&rec);
    out->crash_acknowledged = out->crash_have_record && (rec.acknowledged != 0u);

    out->estop_verified = estop_verification_is_verified();

    /* The SAME live verdict safety_ceiling_sync.c's enforcement is already
     * acting on -- never recomputed here from raw values a second way (see
     * readiness_ceiling_match_status()'s own doc comment on why). */
    out->ceiling_diverged = safety_ceiling_sync_is_diverged(NULL, 0);

    /* The SAME live verdict readiness_pico_update_status() displays --
     * see that predicate's own doc comment. pico_auto_update_state_
     * is_blocking() reads false (never blocks) until docs/PICO_AUTO_
     * UPDATE_PLAN.md's boot-time glue lands in a later commit; this call
     * is stable, standalone and safe to add now. */
    out->pico_update_blocked = pico_auto_update_state_is_blocking();

    /* The SAME resolved fact /api/readiness renders -- one producer, so the
     * page and the interlock cannot drift apart, and the stored verdict is
     * never read without its configuration fingerprint being checked in the
     * same expression (ct_verify_store.h). Fail-safe direction: a store that
     * was never started, a missing key or a blob that fails validation all
     * read NEVER_RUN, which displays as CANNOT_YET and blocks only the
     * wizard step -- never a start, per the owner's rule that only a FAIL
     * blocks firing. */
    out->ct_attribution = (readiness_ct_attribution_fact_t)ct_verify_current_fact();
}
