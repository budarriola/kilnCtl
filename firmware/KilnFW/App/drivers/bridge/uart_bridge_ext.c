// uart_bridge_ext -- CONTROL/PROFILES/AUTOTUNE/WIFI UART tasks (task_ids
// 8-11, uart_task_ids.h). Same shape as every bridge in uart_bridge.c: one
// task_id registered, one task, block on the inbox, dispatch on payload
// byte0, answer queries with a DATA frame back to whoever asked. Split into
// its own file rather than appended to uart_bridge.c purely for size --
// uart_bridge.c was already 1700+ lines before this.
//
// Unlike THERMO/IO's silent SET_* commands, every mutating subcommand here
// answers with an explicit ok/fail reply (see uart_task_ids.h's per-task
// doc comments for why): these wrap operations whose HTTP counterparts
// return a JSON {"ok":...,"error":...} body for a reason a GUI needs right
// away (a feasibility check, "no such profile", "nothing paused to
// resume"), not something to discover on the next poll.
//
// None of these four tasks own hardware directly -- they route entirely
// through the same public getters/setters the HTTP handlers call
// (zones_http.h, profiles_http.h, profile_executor.h, autotune_engine.h,
// wifi_prov.h), so they are safe to start unconditionally regardless of
// which boards are attached, exactly like dashboard_http_start() et al.
//
// SPLIT 2026-09-04 (ROADMAP.md M15, 1500-line rule): this file was 1727
// lines and hosted four separate bridge task families under one roof. Now
// holds only the shared infrastructure -- the flash-safe executor and the
// shared little-endian/reply-framing helpers -- plus the retry-task-create
// helper every start function needs. CONTROL/PROFILES moved to
// uart_bridge_ext_control.c, AUTOTUNE to uart_bridge_ext_autotune.c, WIFI to
// uart_bridge_ext_wifi.c. See uart_bridge_ext_internal.h for the full file
// map and the symbol-widening audit this move required.
#include "uart_bridge.h"
#include "uart_bridge_ext_internal.h"

#include "bx_worker_reentrancy.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "stack_margin.h"

const char *UART_BRIDGE_EXT_TAG = "uart_bridge_ext";

/* 2026-08-20, ROOT-CAUSED on the bench once the uart_log_bridge queue-overflow
 * bug (separate fix, see TODO.md) stopped eating the early boot log: this
 * file's four uart_bridge_start_*_task() functions (CONTROL/PROFILES/
 * AUTOTUNE/WIFI) run back-to-back in main.c right after wifi_prov starts the
 * softAP (main.c ~2.9-3.6s in), which is exactly when the WiFi driver is
 * itself tearing through internal SRAM for its own buffers (32 dynamic tx,
 * 10 static rx @1600B, 5 static mgmt, etc. -- all logged immediately before,
 * see "wifi_init: Init ... buffer num" lines). xTaskCreatePinnedToCore()
 * (plain, no *WithCaps suffix) always allocates BOTH the TCB and the stack
 * from internal SRAM -- CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY does
 * *not* change that; it only permits PSRAM stacks for tasks created through
 * the *WithCaps API below, which nothing here was using. So these four
 * tasks were racing the WiFi driver for the same shrinking internal-SRAM
 * pool at the worst possible moment. The proof this was a *race*, not a
 * fixed-size shortfall: which task(s) failed varied boot to boot with no
 * source change -- one capture showed AUTOTUNE+WIFI (3rd/4th in line)
 * failing while CONTROL+PROFILES (1st/2nd, both still stack=4096 then)
 * came up clean; a later capture with AUTOTUNE/WIFI's stack already shrunk
 * to 3072 instead showed PROFILES+WIFI failing while CONTROL+AUTOTUNE came
 * up clean -- i.e. PROFILES failed at 4096 in one boot and AUTOTUNE
 * succeeded at 3072 in another. A fixed-size problem would fail the same
 * task(s) every time; a shrinking-race problem doesn't. Shrinking the stack
 * request (the earlier attempt) only nudged the odds, it didn't remove the
 * race.
 *
 * FIX: request the stack from PSRAM via xTaskCreatePinnedToCoreWithCaps()
 * (freertos/idf_additions.h) with MALLOC_CAP_SPIRAM -- these four bridge
 * tasks are not latency-critical (they block on their inbox and answer
 * queries; see the file banner) and PSRAM access is more than fast enough
 * for that. Per idf_additions.h's own doc comment, the caps only apply to
 * the stack -- the TCB (a few hundred bytes, not the multi-KB stack that
 * was actually contending with WiFi's buffers) still comes from internal
 * SRAM, which is fine; a TCB-sized allocation was never the problem. The
 * retry loop stays: it's still valid insurance against genuine transient
 * contention (e.g. two of these racing each other, or PSRAM itself
 * momentarily fragmented), just no longer the primary defense against the
 * WiFi-driver race, which moving off internal SRAM removes at the source.
 *
 * HAZARD -- A PSRAM STACK MUST NOT BE LIVE WHILE THE FLASH CACHE IS DOWN.
 * PSRAM is reached THROUGH the flash cache, so any task with its stack there
 * must never be the one running when the cache is disabled -- its stack
 * vanishes mid-call. ESP-IDF asserts on it:
 *
 *   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
 *                  cache_utils.c:126 (esp_task_stack_is_sane_cache_disabled())
 *
 * Confirmed on hardware 2026-08-20: the LVGL task had a PSRAM stack for the
 * same reason these do, and rebooted the board the moment a UI callback wrote
 * touch-calibration data to NVS. Its stack is now internal again; see
 * lvgl_port.c.
 *
 * REPRODUCED ON THESE BRIDGE TASKS TOO -- 2026-08-20, coredump captured.
 * An earlier revision of this comment said `profiles_uart_bridge` was "NOT
 * VERIFIED SAFE" and "not reproduced yet". It is reproduced now. Saving a
 * profile over the UART bridge aborts with exactly the assert above:
 *
 *   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
 *                  cache_utils.c:126 (esp_task_stack_is_sane_cache_disabled())
 *
 * Backtrace (condensed):
 *   profiles_task (uart_bridge_ext.c:514)
 *     -> profiles_http_save (profiles_http.c:448)
 *       -> nvs_save_slot (profiles_http.c:230)
 *         -> nvs -> esp_flash_write -> cache disable -> assert
 *
 * And profiles was NOT the only exposed task. The audit that followed found:
 *   - profiles_task: profiles_http_save(), profiles_http_delete(), plus the
 *     run_state writes reachable through profile_executor_run() / _halt() /
 *     _pause() and run_state_acknowledge(). profile_executor_halt() also
 *     reaches adaptive_tune_run_end() (profile_executor_status.c), which
 *     dispatches a save onto this same worker whenever a Ki baseline was
 *     newly latched this run -- but on THIS call site that can never
 *     happen: profile_executor_status.c passes a hardcoded `clean=false`,
 *     which forces every zone through the skip-and-continue branch before
 *     baseline_newly_latched is ever set. Defensive only; the guard is kept
 *     because it is cheap and correct if `clean` ever stops being a
 *     constant -- see adaptive_tune_run_end()'s own comment in
 *     adaptive_tune.c for the uart_bridge_ext_is_on_flash_worker() guard,
 *     matching the accept path below.
 *   - control_task:  zones_config_set_pid() and zones_config_set_model(),
 *     both of which end in zones_http.c's nvs_save().
 *   - autotune_task: autotune_engine_accept() -- which, since it now also
 *     calls adaptive_tune_clear_ki_baseline(), is itself a RE-ENTRANT caller
 *     of this same executor once its job is already running on bx_worker_
 *     task; see bx_run_on_internal_stack()'s own comment below for the
 *     deadlock that caused and the task-identity check that fixes it. It is
 *     NOT the only such caller -- see profile_executor_halt() above.
 *   - wifi_task: NOT exposed -- wifi_prov_* post to the wifi_prov owner task,
 *     which has an ordinary internal stack, and the write happens there.
 *
 * NVS *READS* ARE EQUALLY UNSAFE. The assert is in the cache-disable path,
 * not in the write path: any partition/NVS read that misses the cache
 * disables it the same way. So "audit the writes" is not a sufficient fix.
 *
 * FIX (implemented below): a single shared flash-safe executor --
 * bx_run_on_internal_stack(). One worker task created with plain
 * xTaskCreatePinnedToCore(), i.e. an INTERNAL-SRAM stack, plus a job queue, a
 * mutex serializing callers and a completion semaphore. control_task,
 * profiles_task and autotune_task now run their ENTIRE per-message switch
 * body on that worker rather than wrapping individual flash calls: there are
 * many flash-reaching call sites, more will be added, and reads count too, so
 * per-call wrapping is fragile. wifi_task is unchanged.
 *
 * Moving these tasks' own stacks back to internal SRAM is NOT the fix -- that
 * reintroduces the WiFi-driver internal-DRAM race documented at the top of
 * this comment. The bridge tasks keep their PSRAM stacks; only the worker's
 * stack is internal, and it is created once, long after the WiFi buffer storm.
 *
 * STACK SIZING FOR TASKS CREATED HERE -- read before shrinking one.
 * Because the stack comes from PSRAM (~8MB free) and not internal SRAM, its
 * size costs nothing scarce. Shrinking one of these to save memory saves
 * memory that was never under pressure, while spending real safety margin.
 * On 2026-08-20 wifi_uart_bridge was trimmed 4096 -> 3072 on an estimate of
 * its "modest" depth; it later overflowed at a measured 3440 bytes and
 * rebooted the board, which is the long-unexplained "spontaneous reboot"
 * this project chased for days. Size these generously, and size them from
 * the STACK USED figure in a coredump rather than from reading the
 * function's own locals -- the ESP-IDF driver call chain underneath a
 * handler is invisible in the source here and is what dominates. */
BaseType_t uart_bridge_ext_retry_task_create_pinned(TaskFunction_t task_fn, const char *name, uint32_t stack_depth,
                                                     void *param, UBaseType_t priority)
{
    /* DIAGNOSTIC, left in deliberately: confirms at each boot that the fix
     * is actually working (internal SRAM should no longer visibly dip
     * during this window) and gives headroom numbers if a future task
     * created here ever needs more stack than expected. */
    /* MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT, not MALLOC_CAP_INTERNAL alone:
     * the latter includes 32-bit-only IRAM, which cannot back a task stack,
     * so it overstates what is actually available here. See main.c's
     * boot-time heap line for the same correction and why it matters. */
    ESP_LOGI(UART_BRIDGE_EXT_TAG, "%s: pre-create heap free=%u largest_dram_block=%u largest_spiram_block=%u", name,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    for (int attempt = 0; attempt < 5; attempt++) {
        BaseType_t created = xTaskCreatePinnedToCoreWithCaps(task_fn, name, stack_depth, param, priority, NULL,
                                                             tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created == pdPASS) {
            if (attempt > 0) {
                ESP_LOGI(UART_BRIDGE_EXT_TAG, "xTaskCreatePinnedToCoreWithCaps(%s) succeeded on retry %d/5", name,
                         attempt + 1);
            }
            return pdPASS;
        }
        if (attempt < 4) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    ESP_LOGW(UART_BRIDGE_EXT_TAG, "xTaskCreatePinnedToCoreWithCaps(%s) still failing after 5 attempts (~200ms)",
             name);
    return pdFAIL;
}

/* ==========================================================================
 * FLASH-SAFE EXECUTOR
 *
 * See the HAZARD block above for why this exists. The three bridge tasks that
 * can reach flash (control/profiles/autotune) do not run their message
 * handlers themselves; they hand the whole handler to this one worker, whose
 * stack is ordinary internal SRAM, and block until it returns.
 *
 * Deliberately whole-message granularity, not per-flash-call: NVS reads trip
 * the same assert as NVS writes, the reachable call sites are numerous
 * (profiles_http_*, zones_config_set_*, profile_executor_*, run_state_*,
 * autotune_engine_accept) and the set will grow. Wrapping individual calls
 * means re-auditing on every future edit; wrapping the switch body does not.
 *
 * Serialized by a mutex, so the queue only ever holds one job and a single
 * completion semaphore is unambiguous. That also means one bridge task's slow
 * handler blocks the other two -- acceptable: these are low-rate request/reply
 * bridges (see the file banner), and NVS access was already serialized inside
 * NVS itself.
 *
 * 8192, internal. Larger than the 4096 the bridge tasks themselves use
 * because the reply buffers that used to be locals in those switch bodies
 * (uint8_t reply[BRIDGE_REPLY_MAX], plus PROFILES_CMD_SAVE's second
 * BRIDGE_REPLY_MAX rep[] and a profile_t candidate) now live on THIS stack,
 * on top of the NVS/esp_flash call chain underneath them. Do not trim this
 * from reading the visible locals -- that is precisely the mistake that
 * overflowed wifi_uart_bridge; size it from a coredump's STACK USED figure.
 *
 * THE WORKER MUST BE CREATED EARLY IN app_main -- DO NOT MAKE THIS LAZY AGAIN.
 * 2026-08-20, on hardware, with the three MAX31856 thermocouple ICs fitted (an
 * extra internal-DRAM consumer at boot): the worker was created lazily from the
 * first of uart_bridge_start_control/_profiles/_autotune_task(), which app_main
 * calls AFTER lvgl_port_start(). That is the single tightest moment of the whole
 * boot -- LVGL takes its own 8192-byte internal stack right there:
 *
 *   W app_main: heap stage uart_bridges_1  largest= 15360 delta= -8192 dram_free= 27227
 *   W app_main: heap stage lvgl_start      largest=  7680 delta= -7680 dram_free= 15519
 *   E uart_bridge_ext: flash-safe worker: task creation failed (internal SRAM)
 *
 * 7680 largest free internal block, 8192 wanted. The worker failed, all three
 * start functions correctly returned ESP_ERR_NO_MEM, and tasks 8/9/10 were
 * never registered: control_get_zones / profiles_list / autotune_get_status all
 * answered "destination task not registered on the peer" for the rest of the
 * boot. The board otherwise ran fine, which is exactly what made it easy to
 * miss.
 *
 * Fix: app_main now calls uart_bridge_ext_start_flash_worker() explicitly at
 * the "executor+autotune" heap stage (largest free internal block ~31744),
 * before display/LVGL bring-up. Shrinking BX_WORKER_STACK to fit the 7680 hole
 * is NOT an alternative -- see the sizing note above; this stack carries
 * profiles_http_save()'s whole NVS chain plus the reply buffers. Move the
 * allocation, not the size. bx_worker_ensure_started() stays as an idempotent
 * fallback so the lazy path still works if the explicit call is ever skipped.
 * ======================================================================== */

/* 8192 -> 10240, 2026-09-21: a REAL hardware stack overflow in this task
 * (docs/audits/bx_flash_worker_panic_after_cfgfs_format_2026-09-21.md) --
 * CONTROL_CMD_SET_ZONE_PID runs ON this worker, and the first cfg-LittleFS
 * write after a format took control_handle_message -> zones nvs_save ->
 * cfg_fs_write_atomic -> esp_vfs -> lfs mkdir/create/rename past 8192 B.
 * cfg_fs_write_atomic gave ~2 kB back in the same pass (see
 * cfg_fs_write_scratch_t), but the LittleFS half of that chain recurses
 * (lfs_dir_traverse) and cannot be bounded statically -- see
 * check_all_task_stack_budgets.py, which grades this task at a
 * self-declared LOWER BOUND, not a measurement. Costs 2048 B of permanent
 * internal DRAM; the allocation still fits the ~31744 B largest free
 * internal block at the explicit start point described above. */
#define BX_WORKER_STACK 10240

typedef void (*bx_job_fn)(void *arg);

typedef struct {
    bx_job_fn fn;
    void     *arg;
} bx_job_t;

static QueueHandle_t     s_bx_jobs;
static SemaphoreHandle_t s_bx_done;
static SemaphoreHandle_t s_bx_lock;
static TaskHandle_t      s_bx_worker_task_handle;

/* ---- POSTED (fire-and-forget) JOB SLOT ---------------------------------
 * 2026-09-16 (HIGH 1, docs/audits/review_divergence_wiring_60d6552f_
 * 2026-09-15.md's own follow-up review 49c9beb7): a SECOND way to get work
 * onto this task, for the one caller shape neither dispatch function above
 * can serve -- a task that must never block at all.
 *
 * The problem this exists for: both bx_run_on_internal_stack() and its
 * *_timeout() sibling end in xSemaphoreTake(s_bx_done, portMAX_DELAY), so
 * the CALLER is blocked for the entire duration of its own job. The
 * timeout bounds only the wait to ACQUIRE the worker, never the job (see
 * flash_worker.h and that function's own comment). safety_poll_task is the
 * sole sender of the ESP->Pico GET_STATUS liveness heartbeat the Pico's S6b
 * LINK_DEAD guard watches, and it dispatches a config autosave -- an NVS
 * write, not the "short job by inspection" those functions require. A slow
 * or stuck write therefore stalls the heartbeat for as long as it takes,
 * and past link_timeout_s (10.0 s default) the Pico trips S6b and ruins a
 * firing. Fails safe, but spuriously.
 *
 * Why a separate slot rather than a bound on s_bx_done: bx_run_on_internal_
 * stack()'s comment already explains why bounding that take is unsafe (the
 * job stays queued and runs later against a caller stack frame that has
 * already unwound, which needs a cancellation/ownership scheme, not a raw
 * timeout). And it cannot share s_bx_jobs either: the worker gives s_bx_
 * done after every queued job, so a job that snuck in without holding
 * s_bx_lock would hand a spurious completion to the NEXT awaiting caller,
 * which would then return before its own job had run. This slot is
 * therefore entirely disjoint from the queue/s_bx_done/s_bx_lock
 * machinery -- it changes nothing about either dispatch path's semantics.
 *
 * `arg` is deliberately NOT part of this API: nobody awaits a posted job,
 * so a pointer into the poster's stack frame would dangle by the time the
 * worker ran it. A posted fn takes its inputs from module state instead.
 *
 * Serialization comes free: posted jobs run on bx_worker_task, the same
 * single task that drains the queue, so a posted job and a queued job can
 * never overlap. Latency is up to BX_POST_POLL_MS plus however long the
 * job currently in flight takes -- fine for the housekeeping writes this
 * is for, and the poster pays none of it. */
#define BX_POST_POLL_MS 100u

static bx_job_fn         s_bx_posted_fn;
static SemaphoreHandle_t s_bx_post_lock;

/* ONE-POSTER INVARIANT (2026-09-16, deliberately omitted from c616391d only
 * to keep that commit byte-identical with its pre-rebase original).
 *
 * Coalescing this slot to a single outstanding post is content-safe TODAY
 * only by accident of who uses it: the one poster's job carries no content
 * (flash_worker.h: "a posted fn takes its inputs from module state") and
 * re-reads that state when it finally runs, so collapsing two posts of the
 * SAME fn loses nothing.
 *
 * The failure mode that is NOT guarded by that argument, and which this
 * repo has lost data to before: a SECOND poster whose pending work is
 * encoded in module state that a coalesced post overwrites -- or, more
 * simply, two posters whose jobs are different functions, where the second
 * post is refused with ESP_ERR_INVALID_STATE and the first poster's job is
 * the only one that ever runs. Each poster's own retry loop then looks
 * healthy while one of them silently never executes.
 *
 * So the slot remembers the fn of the first accepted post and REFUSES a
 * different one loudly rather than silently coalescing across posters. This
 * is a tripwire, not a feature: a genuine second poster is not forbidden
 * forever, it just has to come here, read the paragraph above, and give the
 * slot a real per-poster identity (or a small queue) first. Refusing while
 * logging every attempt is the failure shape this codebase prefers -- loud
 * and repeated, never silent. */
static bx_job_fn         s_bx_post_owner_fn;

/* Claims the posted job, if any, and clears the slot. s_bx_post_lock is
 * held ONLY across the pointer swap, never across the job itself -- this
 * repo's standing "never hold a lock across a producer or blocking call"
 * rule, and also what keeps uart_bridge_ext_post_on_flash_worker()'s
 * zero-tick take below from ever finding the lock held for a meaningful
 * length of time. */
static bx_job_fn bx_take_posted_job(void)
{
    if (!s_bx_post_lock) {
        return NULL;
    }
    if (xSemaphoreTake(s_bx_post_lock, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    bx_job_fn fn = s_bx_posted_fn;
    s_bx_posted_fn = NULL;
    xSemaphoreGive(s_bx_post_lock);
    return fn;
}

static void bx_worker_task(void *arg)
{
    (void)arg;
    while (true) {
        /* Bounded receive rather than portMAX_DELAY so the posted slot is
         * serviced on an otherwise idle worker. A timeout is the ordinary
         * idle case here, not an error -- it just means "no queued job;
         * check the posted slot and go back to waiting". */
        bx_job_t job;
        if (xQueueReceive(s_bx_jobs, &job, pdMS_TO_TICKS(BX_POST_POLL_MS)) == pdTRUE) {
            if (job.fn) {
                job.fn(job.arg);
            }
            xSemaphoreGive(s_bx_done);
        }
        bx_job_fn posted = bx_take_posted_job();
        if (posted) {
            posted(NULL);
        }
    }
}

/* Called from uart_bridge_ext_start_flash_worker() (the normal path, early in
 * app_main) and as a fallback from the uart_bridge_start_*_task() functions
 * (uart_bridge_ext_control.c, uart_bridge_ext_autotune.c), which app_main
 * calls back-to-back on a single thread -- hence a plain static bool guard
 * with no locking of its own. Idempotent: a second call after a successful
 * create is a no-op returning true. Returns false if the worker could not be
 * created; callers MUST fail the start rather than fall through to running
 * handlers on their PSRAM stack, which is the bug this file is about. */
static bool s_bx_started = false;

bool uart_bridge_ext_worker_ensure_started(void)
{
    if (s_bx_started) {
        return true;
    }

    // Ordering note (2026-09-01 audit of ae5905f, flagged as a non-defect):
    // s_bx_jobs is created here before the task-create below writes
    // s_bx_worker_task_handle, so there is a window where s_bx_jobs is
    // non-NULL while the handle is still NULL. bx_run_on_internal_stack()'s
    // NULL check on s_bx_jobs (not the handle) at its own top would let a
    // job through in that window and correctly fail bx_caller_is_worker_
    // task()'s NULL-handle check (never matches), landing on the ordinary
    // dispatch path -- unreachable in practice anyway, since this function
    // runs single-threaded in app_main before any bridge task exists to
    // race it, and it fails in the safe direction if that ever changed.
    s_bx_jobs = xQueueCreate(1, sizeof(bx_job_t));
    s_bx_done = xSemaphoreCreateBinary();
    s_bx_lock = xSemaphoreCreateMutex();
    s_bx_post_lock = xSemaphoreCreateMutex();
    if (!s_bx_jobs || !s_bx_done || !s_bx_lock || !s_bx_post_lock) {
        ESP_LOGE(UART_BRIDGE_EXT_TAG, "flash-safe worker: queue/semaphore allocation failed");
        goto fail;
    }

    /* PLAIN xTaskCreatePinnedToCore, NOT the *WithCaps variant used by
     * uart_bridge_ext_retry_task_create_pinned(): the entire point is that
     * this stack lives in internal SRAM so the flash cache can be disabled
     * underneath it. */
    if (xTaskCreatePinnedToCore(bx_worker_task, "bx_flash_worker", BX_WORKER_STACK, NULL, 5,
                                &s_bx_worker_task_handle, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(UART_BRIDGE_EXT_TAG, "flash-safe worker: task creation failed (internal SRAM)");
        goto fail;
    }
    /* S5 (2026-09-01 audit of ae5905f): registered only on the pdPASS-only
     * path (creation failure already returned above, so this is only ever
     * reached with a real handle) -- same convention as every other
     * stack_margin_register() call site (profile_executor_start.c,
     * safety_link.c, uart_bridge_system.c, wifi_provision_http.c). This
     * task carries the DEEPEST flash chain in the firmware -- every
     * CONTROL/PROFILES/AUTOTUNE mutating command, safety_cfg_store's
     * deferred NVS flush, AND (since R1/S1) now-nested inline jobs
     * (adaptive_tune_clear_ki_baseline()/adaptive_tune_run_end() running
     * fn() directly on this same stack when already dispatched here) --
     * and it was the one significant task in the firmware with no margin
     * visibility at all. BX_WORKER_STACK must match the literal
     * xTaskCreatePinnedToCore() argument three lines up exactly -- see
     * stack_margin.h's own doc comment on why this number is never assumed
     * equal to another task's. */
    stack_margin_register("bx_flash_worker", &s_bx_worker_task_handle, BX_WORKER_STACK);

    s_bx_started = true;
    return true;

fail:
    if (s_bx_jobs) { vQueueDelete(s_bx_jobs); s_bx_jobs = NULL; }
    if (s_bx_done) { vSemaphoreDelete(s_bx_done); s_bx_done = NULL; }
    if (s_bx_lock) { vSemaphoreDelete(s_bx_lock); s_bx_lock = NULL; }
    if (s_bx_post_lock) { vSemaphoreDelete(s_bx_post_lock); s_bx_post_lock = NULL; }
    s_bx_worker_task_handle = NULL;
    return false;
}

/* Public early-init entry point -- see the "MUST BE CREATED EARLY" note above.
 * Idempotent; main_control_bringup() calls this once from its new, earlier
 * call site (1f741635 moved it ahead of relay_cycles_init() and
 * profile_executor_start()) -- LVGL already started in main_boot_early.c by
 * this point, same as at the old call site, so the ordering claim is not
 * "before LVGL" (docs/audits/unreviewed_changes_review_2026-09-08.md finding
 * D5). What actually changed, and still holds: the new site runs strictly
 * earlier in main_control_bringup() than the old one did, so MORE
 * contiguous internal DRAM is available for the 8192-byte allocation here
 * than at the old site, which itself succeeded. */
esp_err_t uart_bridge_ext_start_flash_worker(void)
{
    if (!uart_bridge_ext_worker_ensure_started()) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Runs fn(arg) on the internal-stack worker and blocks until it returns.
 * `arg` may point at the caller's stack -- the caller is blocked for the whole
 * call, so the storage stays live.
 *
 * RE-ENTRANCY -- a job already running ON the worker (e.g. autotune_handle_
 * message() -> AUTOTUNE_CMD_ACCEPT -> autotune_engine_accept() ->
 * adaptive_tune_clear_ki_baseline()) can legitimately need to dispatch
 * ANOTHER flash-safe call. Going through the normal path here would deadlock
 * permanently: s_bx_lock is a non-recursive mutex already held by the
 * ORIGINAL caller (e.g. autotune_task), which is blocked on s_bx_done waiting
 * for THIS job to finish, so xSemaphoreTake(s_bx_lock, ...) below would block
 * forever; a recursive mutex would not save it either, since the 1-deep queue
 * is only ever drained by bx_worker_task, which is the very task now stuck
 * trying to enqueue into it. Confirmed as a real, reproduced deadlock (opus
 * review of commit 7c47683) that hung the flash worker for the whole board --
 * control/profiles/autotune bridges and safety_cfg_store's deferred NVS flush
 * all wedge with it, recoverable only by reboot.
 *
 * Detect the re-entrant case by task identity and run fn(arg) INLINE instead
 * of dispatching: bx_worker_task's own stack is already the internal-SRAM
 * stack this whole executor exists to provide, so the PSRAM/flash-cache
 * hazard this file documents up top is already satisfied without going
 * through the queue at all.
 *
 * Kept `static` and reached by CONTROL/PROFILES/AUTOTUNE (now in their own
 * split files) only through the public uart_bridge_ext_run_on_flash_worker()
 * wrapper below -- there is no reason to widen this one too. */
static bool bx_run_on_internal_stack(bx_job_fn fn, void *arg)
{
    if (!s_bx_jobs || !s_bx_done || !s_bx_lock) {
        ESP_LOGE(UART_BRIDGE_EXT_TAG, "flash-safe worker not started -- job dropped");
        return false;
    }
    if (bx_caller_is_worker_task(s_bx_worker_task_handle, xTaskGetCurrentTaskHandle())) {
        if (fn) {
            fn(arg);
        }
        return true;
    }
    /* Blocks portMAX_DELAY on s_bx_lock and then s_bx_done, with no timeout.
     * Since 2026-09-15 this can be reached from lvgl_task (crash_report.c's
     * LCD Acknowledge control) -- if the worker is busy with a long job (a
     * profile/autotune save, or a CONTROL message), the whole LCD stalls for
     * that long with no bound and no operator feedback (MEDIUM 1,
     * docs/audits/review_crash_gate_low_fixes_c534a0df_2026-09-15.md). This
     * is the same shape relay_cycles_reset() (Relay Life Reset, also called
     * from lvgl_task) has already accepted as a bounded-by-"how long the
     * current job takes" freeze rather than fixed.
     *
     * The REAL invariant this relies on -- corrected 2026-09-15, the
     * previous wording named a symbol (`lvgl_port_lock`) that exists nowhere
     * in this tree: no flash-worker job may ever block on anything lvgl_task
     * itself must produce (an LVGL-serviced queue, a UI-thread callback, a
     * future LVGL mutex). Today nothing under drivers/bridge, control/
     * profile*, or control/autotune* does that, so this function's stall
     * ends when the current job's own flash I/O finishes, never sooner and
     * never later -- not a deadlock, but also not proven safe against a
     * FUTURE flash-worker job that waits on something lvgl_task produces;
     * that combination WOULD deadlock the whole board the same way the
     * re-entrancy case above does. Not enforced by anything mechanical yet
     * (a candidate: teach flash_worker_lint.py this pattern).
     *
     * A caller that cannot accept an unbounded wait here (lvgl_task via
     * crash_report.c) now goes through uart_bridge_ext_run_on_flash_worker_
     * timeout() below instead of this path -- see that function's own
     * comment for why a bound on s_bx_lock (not on s_bx_done) is the safe
     * place to put a timeout: a timed-out xSemaphoreTake on s_bx_done would
     * leave `job` still sitting in the queue for the worker to run later
     * against a stack frame the caller has already unwound. */
    xSemaphoreTake(s_bx_lock, portMAX_DELAY);
    bx_job_t job = { .fn = fn, .arg = arg };
    bool ok = (xQueueSend(s_bx_jobs, &job, portMAX_DELAY) == pdTRUE);
    if (ok) {
        xSemaphoreTake(s_bx_done, portMAX_DELAY);
    }
    xSemaphoreGive(s_bx_lock);
    return ok;
}

/* Bounded-wait sibling of bx_run_on_internal_stack() above -- see flash_
 * worker.h's uart_bridge_ext_run_on_flash_worker_timeout() doc comment for
 * the full rationale (MEDIUM 1, docs/audits/review_crash_gate_low_fixes_
 * c534a0df_2026-09-15.md). Only the wait to ACQUIRE the worker (s_bx_lock)
 * is bounded; once acquired, `fn` is this caller's own job and is dispatched
 * and awaited unbounded, exactly like the sibling above -- `job` is only
 * ever placed on the queue after the bounded wait already succeeded, so
 * there is no window where a timed-out caller leaves a stale stack-frame
 * pointer sitting in the queue. */
static bool bx_run_on_internal_stack_timeout(bx_job_fn fn, void *arg, TickType_t wait_ticks, bool *out_timed_out)
{
    *out_timed_out = false;
    if (!s_bx_jobs || !s_bx_done || !s_bx_lock) {
        ESP_LOGE(UART_BRIDGE_EXT_TAG, "flash-safe worker not started -- job dropped");
        return false;
    }
    if (bx_caller_is_worker_task(s_bx_worker_task_handle, xTaskGetCurrentTaskHandle())) {
        if (fn) {
            fn(arg);
        }
        return true;
    }
    if (xSemaphoreTake(s_bx_lock, wait_ticks) != pdTRUE) {
        *out_timed_out = true;
        return false;
    }
    bx_job_t job = { .fn = fn, .arg = arg };
    bool ok = (xQueueSend(s_bx_jobs, &job, portMAX_DELAY) == pdTRUE);
    if (ok) {
        /* UNBOUNDED wait -- LOW finding (docs/audits/review_crash_gate_
         * medium_fixes_aa2c484d_2026-09-15.md): the bound above applies only
         * to ACQUIRING s_bx_lock (waiting behind some OTHER caller's job);
         * once THIS caller's own `fn` is accepted, it is awaited to
         * completion no matter how long it takes, same as the unbounded
         * bx_run_on_internal_stack() above. That is safe today ONLY because
         * every known caller of the *_timeout() entry point dispatches a
         * short, bounded-in-practice job -- crash_ack_job() (one NVS load
         * plus one persist) and relay_cycles.c's reset_persist_job()
         * (identical shape) -- never a long-running one. Nothing here
         * mechanically enforces that: a future *_timeout() caller that
         * dispatches something slow (a large import, a multi-key migration)
         * would silently turn its "bounded" wait back into an unbounded one
         * from the caller's perspective, defeating the whole point of this
         * function. If a genuinely long job ever needs a bounded caller,
         * this wait needs its own timeout -- which reintroduces the hazard
         * bx_run_on_internal_stack()'s comment above already describes (a
         * timed-out xSemaphoreTake here would leave the job queued against a
         * caller stack frame that has already unwound) and would need a
         * cancellation/ownership scheme, not just a raw timeout, to fix
         * safely. Keep every *_timeout() caller's job short by inspection
         * until that exists. */
        xSemaphoreTake(s_bx_done, portMAX_DELAY);
    }
    xSemaphoreGive(s_bx_lock);
    return ok;
}

/* Public entry point for OTHER modules that need this same worker --
 * 2026-08-23, safety_cfg_store.c's deferred NVS flush is the first outside
 * caller (see that file's own comment for the incident: safety_poll_task's
 * stack is PSRAM, same hazard as this file's own "HAZARD" block above
 * describes for CONTROL/PROFILES/AUTOTUNE, so its NVS write is routed
 * through here rather than a second internal-RAM-stack task being invented
 * for it -- one worker doing this job is enough). Thin wrapper over the
 * exact same bx_run_on_internal_stack() the three bridge handlers already
 * use (now split across uart_bridge_ext_control.c and uart_bridge_ext_
 * autotune.c); kept `static` there and exported here rather than made
 * non-static directly so every OTHER caller keeps calling this short public
 * name. */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    return bx_run_on_internal_stack(fn, arg) ? ESP_OK : ESP_FAIL;
}

/* See flash_worker.h's doc comment, and the POSTED JOB SLOT block above for
 * the full rationale. Never blocks: the only wait here is a ZERO-tick take
 * on s_bx_post_lock, which is held by anyone else only for a single pointer
 * assignment. That is the whole point -- the caller this exists for
 * (safety_poll_task, sole sender of the safety-link heartbeat) must not be
 * stalled by ANY duration of flash work, bounded or not.
 *
 * Safe to call from the worker itself: nothing here blocks or waits on the
 * worker, so a posted job dispatched from a job already running on
 * bx_flash_worker simply runs on a later loop iteration. That is strictly
 * weaker than the re-entrancy hazard the two run_on_flash_worker()
 * functions carry (flash_worker.h's RE-ENTRANCY HAZARD block) -- there is
 * no lock to deadlock on and no completion to await. */
esp_err_t uart_bridge_ext_post_on_flash_worker(void (*fn)(void *arg))
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_bx_post_lock || !s_bx_started) {
        ESP_LOGE(UART_BRIDGE_EXT_TAG, "flash-safe worker not started -- posted job dropped");
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_bx_post_lock, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    if (s_bx_posted_fn) {
        /* A previous post has not been picked up yet. Deliberately NOT a
         * queue: the callers this serves all re-derive their own pending
         * state and retry on a later tick, so coalescing to one outstanding
         * post is correct and keeps this slot O(1). */
        err = ESP_ERR_INVALID_STATE;
    } else if (s_bx_post_owner_fn && s_bx_post_owner_fn != fn) {
        /* ONE-POSTER INVARIANT above. Not a coalescing refusal -- a second
         * distinct poster, which this slot cannot serve safely. */
        ESP_LOGE(UART_BRIDGE_EXT_TAG,
                 "posted-job slot has exactly one poster (%p); a SECOND poster (%p) was refused -- see the "
                 "ONE-POSTER INVARIANT comment in uart_bridge_ext.c before adding one",
                 (void *)s_bx_post_owner_fn, (void *)fn);
        err = ESP_ERR_NOT_SUPPORTED;
    } else {
        s_bx_post_owner_fn = fn;
        s_bx_posted_fn = fn;
        err = ESP_OK;
    }
    xSemaphoreGive(s_bx_post_lock);
    return err;
}

/* See flash_worker.h's doc comment. */
esp_err_t uart_bridge_ext_run_on_flash_worker_timeout(void (*fn)(void *arg), void *arg, uint32_t timeout_ms)
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    bool timed_out = false;
    bool ok = bx_run_on_internal_stack_timeout(fn, arg, pdMS_TO_TICKS(timeout_ms), &timed_out);
    if (timed_out) {
        return ESP_ERR_TIMEOUT;
    }
    return ok ? ESP_OK : ESP_FAIL;
}

/* Lets a caller that is ABOUT to dispatch onto this worker check first
 * whether it is already running there -- so it can do the flash-safe work
 * inline instead of calling uart_bridge_ext_run_on_flash_worker() a second
 * time. bx_run_on_internal_stack() above now protects every caller of this
 * file's own dispatcher regardless (see its "RE-ENTRANCY" comment), but a
 * caller that KNOWS it can be reached from an already-on-worker context --
 * adaptive_tune_clear_ki_baseline(), reached from autotune_engine_accept()
 * when that itself runs as part of AUTOTUNE_CMD_ACCEPT on this worker -- is
 * better off skipping the dispatch machinery entirely and just doing the
 * work: cheaper, and it keeps the "one job in flight at a time" invariant
 * simple for anyone reading uart_bridge_ext.c in isolation, rather than
 * relying on a second module to know this file's internals fixed themselves
 * to tolerate a redundant dispatch. */
bool uart_bridge_ext_is_on_flash_worker(void)
{
    return bx_caller_is_worker_task(s_bx_worker_task_handle, xTaskGetCurrentTaskHandle());
}

/* True once the flash-safe worker task has actually been created --
 * i.e. iff uart_bridge_ext_run_on_flash_worker() would dispatch onto a real
 * task instead of hitting bx_run_on_internal_stack()'s "flash-safe worker
 * not started -- job dropped" fast-fail path. Exists for callers that can
 * run BEFORE main_control_bringup() calls uart_bridge_ext_start_flash_
 * worker() -- cfg_fs_mount.c's deferred auto-format task is the first: it is
 * created during main_boot_early(), at tskIDLE_PRIORITY+1, which the
 * scheduler is free to run before the main task ever reaches
 * main_control_bringup() (docs/audits: the dispatch used to fail in 16 ms,
 * every single boot, for exactly this reason). A caller in that position
 * should poll this rather than dispatch blind and fail fast. */
bool uart_bridge_ext_flash_worker_started(void)
{
    return s_bx_started;
}

/* --------------------------------------------------------------------------
 * Shared little-endian helpers -- same layout uart_bridge.c uses, duplicated
 * here (rather than exported from there) because they're a handful of
 * one-liners and not worth widening that file's already-large surface for.
 * ------------------------------------------------------------------------ */

void uart_bridge_ext_put_u16_le(uint8_t *o, uint16_t v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); }
uint32_t uart_bridge_ext_u32_le(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
void uart_bridge_ext_put_u32_le(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24);
}
float uart_bridge_ext_f32_le(const uint8_t *b) { float v; memcpy(&v, b, sizeof(v)); return v; }
void uart_bridge_ext_put_f32_le(uint8_t *o, float v) { memcpy(o, &v, sizeof(v)); }

/* Same discipline as uart_bridge.c's bridge_args_ok(): reject a truncated
 * frame before touching anything, rather than parse whatever stale bytes
 * are sitting past msg.length in the payload buffer. */
bool uart_bridge_ext_args_ok(const char *who, const uart_proto_message_t *msg, size_t need)
{
    if ((size_t)msg->length >= need) {
        return true;
    }
    ESP_LOGW(UART_BRIDGE_EXT_TAG, "%s: subcmd 0x%02X truncated (%u byte payload, needs %u) -- rejected", who,
             msg->payload[0], (unsigned)msg->length, (unsigned)need);
    return false;
}

/* Appends a length-prefixed ASCII string to *out at *o, capping to what's
 * left of `cap`; truncates (never overruns) and reports how many bytes of
 * `text` actually fit via the return value. Used for every variable-length
 * name/error-message field below -- the 253-byte cap is real, and a client
 * asking for a 96-byte fault_reason back is exactly the case that would
 * overrun a naively-sized reply buffer otherwise. */
size_t uart_bridge_ext_put_lstring(uint8_t *out, size_t cap, size_t o, const char *text)
{
    if (o >= cap) {
        return o;
    }
    size_t room = cap - o - 1; /* -1 for the length byte itself */
    size_t len = text ? strlen(text) : 0;
    if (len > room) {
        len = room;
    }
    if (len > 255) {
        len = 255;
    }
    out[o++] = (uint8_t)len;
    if (len > 0) {
        memcpy(&out[o], text, len);
        o += len;
    }
    return o;
}

void uart_bridge_ext_reply(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                            const uint8_t *reply, size_t reply_len)
{
    esp_err_t err = uart_protocol_send(proto, msg->device, msg->task_id, src_task, reply, reply_len,
                                       BRIDGE_REPLY_ACK_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(UART_BRIDGE_EXT_TAG, "task%u reply (cmd 0x%02X) to dev%u/task%u failed: %s", src_task, reply[0],
                 msg->device, msg->task_id, esp_err_to_name(err));
    }
}

/* Common shape for every "mutating command with an ok/fail reply" handler
 * below: byte0 = subcmd echoed back, byte1 = ok, [optional] length-prefixed
 * error text. */
void uart_bridge_ext_reply_ok_err(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                                   uint8_t subcmd, bool ok, const char *err_msg)
{
    uint8_t reply[BRIDGE_REPLY_MAX];
    size_t o = 0;
    reply[o++] = subcmd;
    reply[o++] = ok ? 1 : 0;
    if (!ok && err_msg && err_msg[0] != '\0') {
        o = uart_bridge_ext_put_lstring(reply, sizeof(reply), o, err_msg);
    }
    uart_bridge_ext_reply(proto, msg, src_task, reply, o);
}
