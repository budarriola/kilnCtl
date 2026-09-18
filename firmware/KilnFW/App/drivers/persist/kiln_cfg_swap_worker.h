/* kiln_cfg_swap_worker.h -- the dedicated task that runs kiln_cfg_swap_apply()
 *
 * docs/KILN_PROFILES_PLAN.md section 8 item 5's "TASK PLACEMENT" acceptance
 * criterion. kiln_cfg_swap_apply() is synchronous and blocking by design
 * (see kiln_cfg_swap.h's own header comment on why): it performs a 60+
 * round-trip UART exchange with the safety processor plus at least one full
 * flash write, and stack-allocates a kiln_cfg_swap_pending_t (~1.7 kB:
 * ZONES_CONFIG_BLOB_MAX_SIZE 896 B + a kiln_pkg_safety_t) on its own frame.
 *
 * It therefore must not run on either of the two tasks a caller would
 * naively reach for:
 *
 *   - NOT the httpd worker. That stack is 8192 B and already measured at a
 *     4832 B ceiling (check_httpd_task_stack_budget.ps1, cfgfs_status_get_
 *     handler), leaving ~2 kB of honest headroom -- less than this
 *     transaction's own frame, before counting the minutes-long blocking
 *     wait that would wedge every other page on the board and trip the task
 *     watchdog. This repo has had two panics from big locals on that stack
 *     (project_httpd_stack_blob_class).
 *
 *   - NOT the flash worker. zones_config_import_blob(), which this
 *     transaction calls, itself dispatches its NVS write to the flash
 *     worker; dispatching to the flash worker from a caller already running
 *     on it deadlocks the board (project_flash_worker_reentrancy).
 *
 * So: its own task, its own stack, created once at bring-up and registered
 * with stack_margin_register() like every other long-lived task here.
 *
 * CONCURRENCY: exactly one swap may be in flight at a time. submit() refuses
 * a second one rather than queueing it -- two interleaved swaps would race
 * on kiln_cfg_swap's single persisted pending record, and "your swap was
 * queued behind another one" is not a state this feature's operator-facing
 * story has any way to explain. The module lock is NEVER held across
 * kiln_cfg_swap_apply() itself (that call blocks for minutes on the UART
 * link); it is taken only to publish state transitions, per this repo's
 * standing "never hold a module lock across a producer or blocking call"
 * rule (project_screen_idle_brick_real_cause).
 */
#ifndef KILN_CFG_SWAP_WORKER_H
#define KILN_CFG_SWAP_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "kiln_cfg_swap.h" /* KILN_CFG_SWAP_REASON_MAX */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* No swap has been submitted since boot. */
    KILN_CFG_SWAP_JOB_IDLE = 0,
    /* Submitted and either queued or executing. */
    KILN_CFG_SWAP_JOB_RUNNING = 1,
    /* kiln_cfg_swap_apply() returned true -- both halves committed, read
     * back and verified (section 4.2 step 12 actually ran). */
    KILN_CFG_SWAP_JOB_DONE_OK = 2,
    /* kiln_cfg_swap_apply() returned false. `reason` carries its verbatim
     * refusal text and `diverged` distinguishes "refused, nothing changed"
     * from "partly landed, board is now alarmed" -- see kiln_cfg_swap.h's
     * doc comment on out_diverged for why an operator message must not
     * collapse those two. */
    KILN_CFG_SWAP_JOB_DONE_FAILED = 3,
} kiln_cfg_swap_job_state_t;

/* Creates the worker task and its single-slot job queue, and registers the
 * task for stack-margin reporting under the name "kiln_cfg_swap". Call once
 * at bring-up, AFTER kiln_cfg_store_init() (this module dispatches into
 * kiln_cfg_swap, which reads the slot store) and after
 * kiln_cfg_swap_set_link(). Idempotent: a second call with the task already
 * up returns ESP_OK without creating anything.
 *
 * On failure nothing is created and submit() refuses every request with a
 * reason -- a board that could not start this task can still read and save
 * kiln configs, it just cannot apply one. */
esp_err_t kiln_cfg_swap_worker_start(void);

/* Queues a swap of the active kiln config to `target_id` and returns
 * immediately. Returns false -- with a caller-facing reason in `reason_out`
 * -- if the worker never started, or if a swap is already in flight.
 *
 * `ack_no_safety_processor` is forwarded verbatim to kiln_cfg_swap_apply(),
 * which forwards it to ota_http_check_interlocks(). NOTE that the interlock
 * check therefore happens on the WORKER, after this function has already
 * returned success to the caller: an HTTP caller that wants to answer a
 * refused interlock synchronously (with a 409/428 and its reason) must run
 * its own ota_http_check_interlocks() pre-check first, exactly as
 * kiln_cfg_http.c's apply handler does. That pre-check is not redundant
 * belt-and-braces here the way it was against the old synchronous path --
 * it is the only thing that can turn an interlock refusal into a real HTTP
 * status code. */
bool kiln_cfg_swap_worker_submit(int32_t target_id, bool ack_no_safety_processor, char *reason_out,
                                 size_t reason_cap);

/* Reads the current/last job's outcome. Any out-parameter may be NULL.
 * `reason_out` receives the empty string unless the state is
 * KILN_CFG_SWAP_JOB_DONE_FAILED. Safe to call before _start(). */
void kiln_cfg_swap_worker_get_status(kiln_cfg_swap_job_state_t *out_state, int32_t *out_target_id,
                                     bool *out_diverged, char *reason_out, size_t reason_cap);

/* true iff a swap is queued or executing right now. */
bool kiln_cfg_swap_worker_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* KILN_CFG_SWAP_WORKER_H */
