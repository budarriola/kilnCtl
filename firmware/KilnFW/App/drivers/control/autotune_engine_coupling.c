#include "autotune_engine_internal.h"

/* Cross-zone coupling-matrix persistence job, the coupling matrix getter,
 * RGA computation. See autotune_engine_internal.h's top comment for the full
 * five-way split this file is one piece of. */

/* autotune_finalize_fit()'s persist step (autotune_engine_step_identify.c) gathers
 * every cell to write into one of these (coupling_persist_job_t -- declared
 * in autotune_engine_internal.h, shared with that file) and hands it to
 * bx_flash_worker in a single job -- see that function's own comment for why
 * a direct zones_config_set_coupling_cell() call from task_entry()'s task is
 * a hard panic, not just a bug.
 *
 * The struct itself may live on the caller's PSRAM stack (task_entry()'s),
 * but that is fine: coupling_persist_job() reads every field it needs BEFORE
 * making its first NVS call, i.e. strictly before the flash cache is ever
 * disabled on the worker's own (internal-RAM) stack. Nothing here is
 * touched again after that point. */
void coupling_persist_job(void *arg)
{
    coupling_persist_job_t *job = (coupling_persist_job_t *)arg;
    for (uint8_t i = 0; i < job->count; i++) {
        if (!zones_config_set_coupling_cell(job->affected_zone[i], job->stepped_zone, job->coeff[i],
                                             job->tau_s[i], job->dead_time_s[i])) {
            job->fail_count++;
        }
    }
}

/* Caller holds s_at.lock. Moves the coupling persist autotune_finalize_fit()
 * parked in s_at.pending_coupling into *out and clears it. Returns false when
 * nothing is pending. */
bool autotune_take_pending_coupling_locked(coupling_persist_job_t *out)
{
    if (!s_at.pending_coupling_valid) {
        return false;
    }
    *out = s_at.pending_coupling;
    s_at.pending_coupling_valid = false;
    memset(&s_at.pending_coupling, 0, sizeof(s_at.pending_coupling));
    return true;
}

/* Flash-worker lock-inversion audit 2026-10-09 F2. MUST be called with
 * s_at.lock NOT held. uart_bridge_ext_run_on_flash_worker() waits
 * portMAX_DELAY for bx_flash_worker, and every UART autotune command
 * (GET_STATUS, ABORT, ACCEPT, START) runs on that worker and takes s_at.lock.
 * Dispatching with the lock held deadlocked the autotune task and the worker
 * whenever a PC status poll was in flight as a step-test fit finished.
 *
 * Dispatch (not inline) is still required: task_entry()'s stack is in PSRAM
 * and zones_config_set_coupling_cell() ends in an NVS commit that disables
 * the flash cache -- see coupling_persist_job()'s comment. */
void autotune_dispatch_coupling_persist(coupling_persist_job_t *job)
{
    if (job == NULL || job->count == 0) {
        return;
    }
    // Not reachable on-worker today; covered by bx_run_on_internal_
    // stack()'s generic backstop if that ever changes.
    esp_err_t submit_err = uart_bridge_ext_run_on_flash_worker(coupling_persist_job, job);
    if (submit_err != ESP_OK) {
        ESP_LOGW(AT_TAG, "autotune zone %u: could not submit %u coupling cell(s) to the flash worker: %s",
                 job->stepped_zone, (unsigned)job->count, esp_err_to_name(submit_err));
    } else if (job->fail_count > 0) {
        ESP_LOGW(AT_TAG, "autotune zone %u: %u of %u coupling cell(s) failed to persist", job->stepped_zone,
                 (unsigned)job->fail_count, (unsigned)job->count);
    }
}

void autotune_engine_get_coupling_matrix(autotune_coupling_matrix_t *out)
{
    if (!out) return;
    /* See autotune_begin_run_locked()'s guard comment above. Zeroing here matches
     * every cell's own "unmeasured" representation (autotune_coupling_cell_t
     * ::valid == false), same as a never-run engine's real coupling matrix. */
    if (s_at.lock == NULL) {
        LOG_PRESTART_ONCE("autotune_engine_get_coupling_matrix() called before autotune_engine_start() -- reporting empty");
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    *out = s_at.coupling;
    xSemaphoreGive(s_at.lock);
}

void autotune_engine_compute_rga(const autotune_coupling_matrix_t *m, autotune_rga_t *out)
{
    if (!out) return;
    if (!m) {
        memset(out, 0, sizeof(*out));
        return;
    }

    /* Only the steady-state gain enters the RGA. tau and L are deliberately
     * discarded here: the RGA is a *steady-state* interaction measure, and
     * the dynamic version (the RGA evaluated at a frequency rather than at
     * DC) needs a full transfer-function model per pair, which nothing in
     * this firmware identifies. Mixing the two would be an upgrade in
     * apparent rigour and a downgrade in correctness. */
    float k[MAX31856_CHANNEL_COUNT * MAX31856_CHANNEL_COUNT];
    bool k_valid[MAX31856_CHANNEL_COUNT * MAX31856_CHANNEL_COUNT];
    for (int i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            const autotune_coupling_cell_t *c = &m->cell[i][j];
            /* A cell is usable only if the *fit* converged too -- cell.valid
             * alone can be true for a row that was written with a model the
             * fitter rejected, and an unfitted gain is a hole, not a zero. */
            bool ok = c->valid && c->model.valid;
            k[i * MAX31856_CHANNEL_COUNT + j] = ok ? c->model.k_gain_c_per_duty : 0.0f;
            k_valid[i * MAX31856_CHANNEL_COUNT + j] = ok;
        }
    }
    *out = pid_autotune_rga(k, k_valid, MAX31856_CHANNEL_COUNT);
}
