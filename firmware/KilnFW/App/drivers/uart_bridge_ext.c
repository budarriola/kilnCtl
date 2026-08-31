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
#include "uart_bridge.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "MAX31856.h"
#include "autotune_engine.h"
#include "kiln_io.h"
#include "profile_executor.h"
#include "profiles_builtin.h"
#include "profiles_http.h"
#include "run_state.h"
#include "uart_task_ids.h"
#include "unit_pref.h"
#include "wifi_prov.h"
#include "zones_http.h"

static const char *TAG = "uart_bridge_ext";

#define BRIDGE_INBOX_LEN 4
#define BRIDGE_REPLY_MAX UART_PROTO_MAX_PAYLOAD
#define BRIDGE_REPLY_ACK_TIMEOUT_MS 200u

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
 *     _pause() and run_state_acknowledge().
 *   - control_task:  zones_config_set_pid() and zones_config_set_model(),
 *     both of which end in zones_http.c's nvs_save().
 *   - autotune_task: autotune_engine_accept().
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
static BaseType_t retry_task_create_pinned(TaskFunction_t task_fn, const char *name, uint32_t stack_depth,
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
    ESP_LOGI(TAG, "%s: pre-create heap free=%u largest_dram_block=%u largest_spiram_block=%u", name,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    for (int attempt = 0; attempt < 5; attempt++) {
        BaseType_t created = xTaskCreatePinnedToCoreWithCaps(task_fn, name, stack_depth, param, priority, NULL,
                                                             tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created == pdPASS) {
            if (attempt > 0) {
                ESP_LOGI(TAG, "xTaskCreatePinnedToCoreWithCaps(%s) succeeded on retry %d/5", name, attempt + 1);
            }
            return pdPASS;
        }
        if (attempt < 4) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    ESP_LOGW(TAG, "xTaskCreatePinnedToCoreWithCaps(%s) still failing after 5 attempts (~200ms)", name);
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

#define BX_WORKER_STACK 8192

typedef void (*bx_job_fn)(void *arg);

typedef struct {
    bx_job_fn fn;
    void     *arg;
} bx_job_t;

static QueueHandle_t     s_bx_jobs;
static SemaphoreHandle_t s_bx_done;
static SemaphoreHandle_t s_bx_lock;

static void bx_worker_task(void *arg)
{
    (void)arg;
    while (true) {
        bx_job_t job;
        if (xQueueReceive(s_bx_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.fn) {
            job.fn(job.arg);
        }
        xSemaphoreGive(s_bx_done);
    }
}

/* Called from uart_bridge_ext_start_flash_worker() (the normal path, early in
 * app_main) and as a fallback from the uart_bridge_start_*_task() functions,
 * which app_main calls back-to-back on a single thread -- hence a plain static
 * bool guard with no locking of its own. Idempotent: a second call after a
 * successful create is a no-op returning true. Returns false if the worker
 * could not be created; callers MUST fail the start rather than fall through to
 * running handlers on their PSRAM stack, which is the bug this file is about. */
static bool s_bx_started = false;

static bool bx_worker_ensure_started(void)
{
    if (s_bx_started) {
        return true;
    }

    s_bx_jobs = xQueueCreate(1, sizeof(bx_job_t));
    s_bx_done = xSemaphoreCreateBinary();
    s_bx_lock = xSemaphoreCreateMutex();
    if (!s_bx_jobs || !s_bx_done || !s_bx_lock) {
        ESP_LOGE(TAG, "flash-safe worker: queue/semaphore allocation failed");
        goto fail;
    }

    /* PLAIN xTaskCreatePinnedToCore, NOT the *WithCaps variant used by
     * retry_task_create_pinned(): the entire point is that this stack lives
     * in internal SRAM so the flash cache can be disabled underneath it. */
    if (xTaskCreatePinnedToCore(bx_worker_task, "bx_flash_worker", BX_WORKER_STACK, NULL, 5, NULL,
                                tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "flash-safe worker: task creation failed (internal SRAM)");
        goto fail;
    }

    s_bx_started = true;
    return true;

fail:
    if (s_bx_jobs) { vQueueDelete(s_bx_jobs); s_bx_jobs = NULL; }
    if (s_bx_done) { vSemaphoreDelete(s_bx_done); s_bx_done = NULL; }
    if (s_bx_lock) { vSemaphoreDelete(s_bx_lock); s_bx_lock = NULL; }
    return false;
}

/* Public early-init entry point -- see the "MUST BE CREATED EARLY" note above.
 * Idempotent; app_main calls this once, well before LVGL takes its own internal
 * stack, so the 8192-byte internal allocation lands while there is still ~31 KB
 * of contiguous internal DRAM rather than the 7680 left after lvgl_start. */
esp_err_t uart_bridge_ext_start_flash_worker(void)
{
    if (!bx_worker_ensure_started()) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Runs fn(arg) on the internal-stack worker and blocks until it returns.
 * `arg` may point at the caller's stack -- the caller is blocked for the whole
 * call, so the storage stays live. */
static bool bx_run_on_internal_stack(bx_job_fn fn, void *arg)
{
    if (!s_bx_jobs || !s_bx_done || !s_bx_lock) {
        ESP_LOGE(TAG, "flash-safe worker not started -- job dropped");
        return false;
    }
    xSemaphoreTake(s_bx_lock, portMAX_DELAY);
    bx_job_t job = { .fn = fn, .arg = arg };
    bool ok = (xQueueSend(s_bx_jobs, &job, portMAX_DELAY) == pdTRUE);
    if (ok) {
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
 * exact same bx_run_on_internal_stack() the three bridge handlers below
 * already use; kept `static` there and exported here rather than made
 * non-static directly so every OTHER caller in this file keeps calling the
 * short internal name. */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    return bx_run_on_internal_stack(fn, arg) ? ESP_OK : ESP_FAIL;
}

/* Shared shape for the three refactored handlers: the task's ctx plus the
 * message it just received. Lives on the bridge task's stack across the
 * blocking bx_run_on_internal_stack() call. */
typedef struct {
    void                    *ctx;
    const uart_proto_message_t *msg;
} bx_handler_args_t;

/* --------------------------------------------------------------------------
 * Shared little-endian helpers -- same layout uart_bridge.c uses, duplicated
 * here (rather than exported from there) because they're a handful of
 * one-liners and not worth widening that file's already-large surface for.
 * ------------------------------------------------------------------------ */

static void bx_put_u16_le(uint8_t *o, uint16_t v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); }
static uint32_t bx_u32_le(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static void bx_put_u32_le(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24);
}
static float bx_f32_le(const uint8_t *b) { float v; memcpy(&v, b, sizeof(v)); return v; }
static void bx_put_f32_le(uint8_t *o, float v) { memcpy(o, &v, sizeof(v)); }

/* Same discipline as uart_bridge.c's bridge_args_ok(): reject a truncated
 * frame before touching anything, rather than parse whatever stale bytes
 * are sitting past msg.length in the payload buffer. */
static bool bx_args_ok(const char *who, const uart_proto_message_t *msg, size_t need)
{
    if ((size_t)msg->length >= need) {
        return true;
    }
    ESP_LOGW(TAG, "%s: subcmd 0x%02X truncated (%u byte payload, needs %u) -- rejected", who,
             msg->payload[0], (unsigned)msg->length, (unsigned)need);
    return false;
}

/* Appends a length-prefixed ASCII string to *out at *o, capping to what's
 * left of `cap`; truncates (never overruns) and reports how many bytes of
 * `text` actually fit via the return value. Used for every variable-length
 * name/error-message field below -- the 253-byte cap is real, and a client
 * asking for a 96-byte fault_reason back is exactly the case that would
 * overrun a naively-sized reply buffer otherwise. */
static size_t bx_put_lstring(uint8_t *out, size_t cap, size_t o, const char *text)
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

static void bx_reply(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                     const uint8_t *reply, size_t reply_len)
{
    esp_err_t err = uart_protocol_send(proto, msg->device, msg->task_id, src_task, reply, reply_len,
                                       BRIDGE_REPLY_ACK_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "task%u reply (cmd 0x%02X) to dev%u/task%u failed: %s", src_task, reply[0],
                 msg->device, msg->task_id, esp_err_to_name(err));
    }
}

/* Common shape for every "mutating command with an ok/fail reply" handler
 * below: byte0 = subcmd echoed back, byte1 = ok, [optional] length-prefixed
 * error text. */
static void bx_reply_ok_err(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                            uint8_t subcmd, bool ok, const char *err_msg)
{
    uint8_t reply[BRIDGE_REPLY_MAX];
    size_t o = 0;
    reply[o++] = subcmd;
    reply[o++] = ok ? 1 : 0;
    if (!ok && err_msg && err_msg[0] != '\0') {
        o = bx_put_lstring(reply, sizeof(reply), o, err_msg);
    }
    bx_reply(proto, msg, src_task, reply, o);
}

/* ==========================================================================
 * CONTROL (task 8)
 * ======================================================================== */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} control_ctx_t;

static size_t control_build_get_zones(uint8_t *out)
{
    uint8_t thermo_count = zones_config_get_thermo_count();
    if (thermo_count > MAX31856_CHANNEL_COUNT) {
        thermo_count = MAX31856_CHANNEL_COUNT; /* defensive -- see zones_http.c's own bound */
    }

    size_t o = 0;
    out[o++] = CONTROL_CMD_GET_ZONES;
    out[o++] = thermo_count;
    out[o++] = KILN_IO_RELAY_COUNT;
    out[o++] = thermo_count; /* count field -- see uart_task_ids.h */

    for (uint8_t zi = 0; zi < thermo_count; zi++) {
        uint8_t relay_mask = 0;
        zones_config_get_relay_mask(zi, &relay_mask);
        zone_control_mode_t mode = ZONE_CONTROL_MODE_OFF;
        zones_config_get_control_mode(zi, &mode);
        /* No direct getter for the raw cal offset exists (only
         * zones_config_apply_cal(), which adds it to a caller-supplied raw
         * reading) -- recovering it via apply_cal(zi, 0.0f) is exact and
         * avoids adding a new getter just for this. */
        float cal_offset_c = zones_config_apply_cal(zi, 0.0f);
        float kp = 0.0f, ki = 0.0f, kd = 0.0f;
        zones_config_get_pid(zi, &kp, &ki, &kd);
        float max_ramp = 0.0f;
        zones_config_get_max_ramp(zi, &max_ramp);
        float max_temp = 0.0f, min_temp = 0.0f;
        zones_config_get_temp_limits(zi, &max_temp, &min_temp);

        out[o++] = zi;
        out[o++] = relay_mask;
        out[o++] = (uint8_t)mode;
        bx_put_f32_le(&out[o], cal_offset_c); o += 4;
        bx_put_f32_le(&out[o], kp); o += 4;
        bx_put_f32_le(&out[o], ki); o += 4;
        bx_put_f32_le(&out[o], kd); o += 4;
        bx_put_f32_le(&out[o], max_ramp); o += 4;
        bx_put_f32_le(&out[o], max_temp); o += 4;
        bx_put_f32_le(&out[o], min_temp); o += 4;
    }
    return o;
}

/* Runs on the flash-safe worker (bx_run_on_internal_stack), never on
 * control_task itself: CONTROL_CMD_SET_ZONE_PID/MODEL both end in zones_http.c's
 * nvs_save(). `reply` is a local here on purpose -- that is what moves the
 * BRIDGE_REPLY_MAX buffer onto the worker's internal stack. */
static void control_handle_message(void *vargs)
{
    bx_handler_args_t          *args = (bx_handler_args_t *)vargs;
    control_ctx_t              *ctx  = (control_ctx_t *)args->ctx;
    const uart_proto_message_t  msg  = *args->msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    uint8_t subcmd = msg.payload[0];

    {
        switch (subcmd) {
            case CONTROL_CMD_GET_ZONES: {
                size_t len = control_build_get_zones(reply);
                bx_reply(ctx->proto, &msg, UART_TASK_ID_CONTROL, reply, len);
                break;
            }
            case CONTROL_CMD_SET_ZONE_PID: {
                if (!bx_args_ok("control", &msg, 14)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false, "truncated");
                    break;
                }
                uint8_t zi = msg.payload[1];
                float kp = bx_f32_le(&msg.payload[2]);
                float ki = bx_f32_le(&msg.payload[6]);
                float kd = bx_f32_le(&msg.payload[10]);
                bool ok = zones_config_set_pid(zi, kp, ki, kd);
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, ok, NULL);
                break;
            }
            case CONTROL_CMD_SET_ZONE_MODEL: {
                if (!bx_args_ok("control", &msg, 14)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false, "truncated");
                    break;
                }
                uint8_t zi = msg.payload[1];
                float k_dc = bx_f32_le(&msg.payload[2]);
                float tau_s = bx_f32_le(&msg.payload[6]);
                float dead_time_s = bx_f32_le(&msg.payload[10]);
                bool ok = zones_config_set_model(zi, k_dc, tau_s, dead_time_s);
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, ok, NULL);
                break;
            }
            case CONTROL_CMD_GET_UNIT_PREF: {
                /* 2026-08-21 (ROADMAP.md shared unit preference): additive
                 * QUERY-style subcommand, see uart_task_ids.h's doc comment
                 * for why this needs no protocol version bump. */
                uint8_t reply2[2];
                reply2[0] = CONTROL_CMD_GET_UNIT_PREF;
                reply2[1] = (uint8_t)unit_pref_get();
                bx_reply(ctx->proto, &msg, UART_TASK_ID_CONTROL, reply2, sizeof(reply2));
                break;
            }
            case CONTROL_CMD_SET_UNIT_PREF: {
                if (!bx_args_ok("control", &msg, 2)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false, "truncated");
                    break;
                }
                uint8_t raw = msg.payload[1];
                bool ok = (raw == (uint8_t)UNIT_PREF_CELSIUS || raw == (uint8_t)UNIT_PREF_FAHRENHEIT) &&
                          unit_pref_set((unit_pref_t)raw) == ESP_OK;
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, ok, NULL);
                break;
            }
            default:
                ESP_LOGW(TAG, "control: unknown subcmd 0x%02X -- rejected", subcmd);
                /* Every other reply path here uses bx_reply_ok_err, but an
                 * unrecognized subcmd was falling through unanswered -- the
                 * transport ACK (uart_protocol.c, before this switch ever
                 * runs) already told the host "delivered", and with no reply
                 * that looked identical to "executed". Same gap uart_bridge.c
                 * closes with bridge_reply_unsupported(); see its comment. */
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false,
                                "unknown subcommand");
                break;
        }
    }
}

static void control_task(void *arg)
{
    control_ctx_t *ctx = (control_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "control: empty payload -- rejected");
            continue;
        }
        bx_handler_args_t args = { .ctx = ctx, .msg = &msg };
        bx_run_on_internal_stack(control_handle_message, &args);
    }
}

esp_err_t uart_bridge_start_control_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Must succeed before the bridge task exists: without the worker,
     * control_handle_message() would never run at all, and silently falling
     * back to running it on this task's PSRAM stack is the exact bug being
     * fixed. Fail the start instead. */
    if (!bx_worker_ensure_started()) {
        return ESP_ERR_NO_MEM;
    }
    static control_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_CONTROL, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }
    BaseType_t created = retry_task_create_pinned(control_task, "control_uart_bridge", 4096, &ctx, 5);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_CONTROL);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ==========================================================================
 * PROFILES (task 9)
 * ======================================================================== */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} profiles_ctx_t;

/* Appends one summary record if it fits. Returns false when the frame is
 * full, which is the signal to stop the page here. */
static bool profiles_list_append(uint8_t *out, size_t *io, uint8_t id, const profile_t *p)
{
    size_t name_len = strlen(p->name);
    /* Bail before overrunning the frame rather than truncate a profile
     * entry mid-record -- a short page is still useful (the client asks for
     * the next one), a mis-parsed record is not. */
    if (*io + 1 + 1 + name_len + 1 + 1 > BRIDGE_REPLY_MAX) {
        return false;
    }
    out[(*io)++] = id;
    *io = bx_put_lstring(out, BRIDGE_REPLY_MAX, *io, p->name);
    out[(*io)++] = p->zone_mask;
    out[(*io)++] = p->segment_count;
    return true;
}

/* LIST is PAGED, because the catalogue does not fit in one frame.
 *
 * A summary record is 1 (id) + 1 (name len) + <=15 (name) + 1 (zone_mask) +
 * 1 (segment_count) = up to 19 bytes, and BRIDGE_REPLY_MAX is 253 with 2
 * bytes of header, so ~13 records per frame against 8 user slots plus 28
 * shipped schedules. Rather than invent a second command, LIST takes an
 * optional `start_id` byte and enumerates every EXISTING profile with
 * id >= start_id in ascending id order -- user slots 0..7 first, then the
 * builtin catalogue at PROFILE_BUILTIN_ID_BASE.. -- stopping when the frame
 * fills. The client pages by re-asking with (last id + 1) until a reply
 * comes back with count == 0. The reply layout is unchanged, so an old
 * client that sends no argument still parses this fine; it simply sees the
 * first page (start_id 0) instead of "all of them", which was already the
 * documented behaviour of the break above.
 *
 * Hidden builtins are skipped here, matching GET /api/profiles. They stay
 * reachable by direct GET, same as on the HTTP side. */
static size_t profiles_build_list(uint8_t *out, uint8_t start_id)
{
    size_t o = 0;
    out[o++] = PROFILES_CMD_LIST;
    size_t count_pos = o++;
    uint8_t count = 0;

    for (uint16_t id = start_id; id < PROFILES_MAX_COUNT; id++) {
        profile_t p;
        if (!profiles_http_get((uint8_t)id, &p)) {
            continue;
        }
        if (!profiles_list_append(out, &o, (uint8_t)id, &p)) {
            goto done;
        }
        count++;
    }

    for (size_t i = 0; i < g_builtin_profile_count; i++) {
        uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
        if (id < start_id || profiles_builtin_is_hidden(id)) {
            continue;
        }
        profile_t p;
        if (!profiles_http_get(id, &p)) {
            continue;
        }
        if (!profiles_list_append(out, &o, id, &p)) {
            goto done;
        }
        count++;
    }

done:
    out[count_pos] = count;
    return o;
}

/* GET serves a user slot and a builtin catalogue id alike -- profiles_http_get()
 * resolves both (and fills a builtin's zone_mask from the configured zones), so
 * there is deliberately no id-range test here.
 *
 * Reply budget, worst case: 1 (subcmd) + 1 (ok) + 1 (id) + 1 (name len) + 15
 * (PROFILE_NAME_MAX_LEN) + 1 (zone_mask) + 1 (segment_count) + 12 segments x
 * 12 bytes = 165 bytes, against BRIDGE_REPLY_MAX = UART_PROTO_MAX_PAYLOAD =
 * 253. A full 12-segment builtin fits with 88 bytes to spare; the clamp below
 * never binds today. */
static size_t profiles_build_get(uint8_t *out, uint8_t id)
{
    size_t o = 0;
    out[o++] = PROFILES_CMD_GET;
    profile_t p;
    if (!profiles_http_get(id, &p)) {
        out[o++] = 0; /* ok = 0 */
        return o;
    }
    out[o++] = 1; /* ok */
    out[o++] = id;
    o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, p.name);
    out[o++] = p.zone_mask;
    uint8_t seg_count = p.segment_count;
    /* Clamp to what's left of the frame -- 12 bytes/segment, so this can
     * only bind in a build where PROFILE_MAX_SEGMENTS grows well past 12,
     * which it hasn't; kept as a hard backstop rather than an assumption. */
    size_t max_segs_that_fit = (BRIDGE_REPLY_MAX - o - 1) / 12u;
    if (seg_count > max_segs_that_fit) {
        seg_count = (uint8_t)max_segs_that_fit;
    }
    out[o++] = seg_count;
    for (uint8_t i = 0; i < seg_count; i++) {
        bx_put_f32_le(&out[o], p.segments[i].target_c); o += 4;
        bx_put_f32_le(&out[o], p.segments[i].ramp_c_per_hr); o += 4;
        bx_put_u32_le(&out[o], p.segments[i].dwell_min); o += 4;
    }
    return o;
}

/* GET_EXEC_STATUS mirrors /api/control's per-zone shape (control_mode/
 * actual/duty/PID-adjacent fields), not /api/profile_exec's fuller one --
 * see uart_task_ids.h for why. */
static size_t profiles_build_exec_status(uint8_t *out)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);

    size_t o = 0;
    out[o++] = PROFILES_CMD_GET_EXEC_STATUS;
    out[o++] = (uint8_t)st.state;
    out[o++] = st.profile_id;
    o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, st.profile_name);
    out[o++] = st.zone_mask;
    out[o++] = st.segment_index;
    out[o++] = st.segment_count;
    out[o++] = st.dwelling ? 1 : 0;
    bx_put_f32_le(&out[o], st.target_c); o += 4;
    bx_put_u32_le(&out[o], st.segment_elapsed_s); o += 4;
    bx_put_u32_le(&out[o], st.dwell_remaining_s); o += 4;
    out[o++] = st.ramp_lock_held ? 1 : 0;
    out[o++] = st.ramp_lock_lagging_mask;
    out[o++] = st.fault_guard;

    size_t zone_count_pos = o++;
    uint8_t zone_count = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const profile_exec_zone_status_t *z = &st.zones[zi];
        if (!z->active) {
            continue;
        }
        if (o + 14 > BRIDGE_REPLY_MAX) {
            break; /* frame full -- report what fit rather than overrun */
        }
        out[o++] = zi;
        out[o++] = z->control_mode;
        bx_put_f32_le(&out[o], z->actual_c); o += 4;
        out[o++] = z->actual_valid ? 1 : 0;
        bx_put_f32_le(&out[o], z->duty); o += 4;
        out[o++] = z->relay_commanded_on ? 1 : 0;
        out[o++] = z->faulted ? 1 : 0;
        out[o++] = z->fault_guard;
        zone_count++;
    }
    out[zone_count_pos] = zone_count;
    return o;
}

/* Runs on the flash-safe worker. This is the handler whose old in-task form
 * produced the captured coredump: SAVE -> profiles_http_save() -> nvs_save_slot()
 * -> esp_flash_write() -> cache disable -> assert. DELETE, START, STOP, PAUSE
 * and ACK_LAST_RUN reach flash too (run_state / profiles_http). */
static void profiles_handle_message(void *vargs)
{
    bx_handler_args_t          *args = (bx_handler_args_t *)vargs;
    profiles_ctx_t             *ctx  = (profiles_ctx_t *)args->ctx;
    const uart_proto_message_t  msg  = *args->msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    uint8_t subcmd = msg.payload[0];

    {
        switch (subcmd) {
            case PROFILES_CMD_LIST: {
                /* Optional start_id byte -- absent means "from the top". */
                uint8_t start_id = (msg.length >= 2) ? msg.payload[1] : 0u;
                size_t len = profiles_build_list(reply, start_id);
                bx_reply(ctx->proto, &msg, UART_TASK_ID_PROFILES, reply, len);
                break;
            }
            case PROFILES_CMD_GET: {
                if (!bx_args_ok("profiles", &msg, 2)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                size_t len = profiles_build_get(reply, msg.payload[1]);
                bx_reply(ctx->proto, &msg, UART_TASK_ID_PROFILES, reply, len);
                break;
            }
            case PROFILES_CMD_SAVE: {
                if (!bx_args_ok("profiles", &msg, 5)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                uint8_t requested_id = msg.payload[1];
                uint8_t name_len = msg.payload[2];
                if (!bx_args_ok("profiles", &msg, (size_t)3 + name_len + 2)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                if (name_len > PROFILE_NAME_MAX_LEN) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false,
                                    "name too long");
                    break;
                }
                size_t p = 3;
                profile_t candidate;
                memset(&candidate, 0, sizeof(candidate));
                memcpy(candidate.name, &msg.payload[p], name_len);
                candidate.name[name_len] = '\0';
                p += name_len;
                candidate.zone_mask = msg.payload[p++];
                uint8_t seg_count = msg.payload[p++];
                if (seg_count < 1 || seg_count > PROFILE_MAX_SEGMENTS ||
                    !bx_args_ok("profiles", &msg, p + (size_t)seg_count * 12)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false,
                                    "seg_count missing or out of range");
                    break;
                }
                candidate.segment_count = seg_count;
                for (uint8_t i = 0; i < seg_count; i++) {
                    candidate.segments[i].target_c = bx_f32_le(&msg.payload[p]); p += 4;
                    candidate.segments[i].ramp_c_per_hr = bx_f32_le(&msg.payload[p]); p += 4;
                    candidate.segments[i].dwell_min = bx_u32_le(&msg.payload[p]); p += 4;
                }

                uint8_t out_id = 0, warn_count = 0;
                char err_msg[96] = "";
                bool ok = profiles_http_save(requested_id, &candidate, &out_id, &warn_count, err_msg,
                                             sizeof(err_msg));
                uint8_t rep[BRIDGE_REPLY_MAX];
                size_t o = 0;
                rep[o++] = subcmd;
                rep[o++] = ok ? 1 : 0;
                if (ok) {
                    rep[o++] = out_id;
                    rep[o++] = warn_count;
                } else {
                    o = bx_put_lstring(rep, sizeof(rep), o, err_msg);
                }
                bx_reply(ctx->proto, &msg, UART_TASK_ID_PROFILES, rep, o);
                break;
            }
            case PROFILES_CMD_DELETE: {
                if (!bx_args_ok("profiles", &msg, 2)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                uint8_t del_id = msg.payload[1];
                /* Same refusal profile_delete_post_handler() gives: a builtin
                 * is a const table in flash and cannot be erased. Say so and
                 * point at hide, rather than at "no such profile" -- which
                 * would be a lie about an id GET and START both accept. */
                if (profiles_builtin_id_valid(del_id)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false,
                                    "built-in schedule is read-only; hide it instead "
                                    "(POST /api/profile/builtin/hide)");
                    break;
                }
                bool ok = profiles_http_delete(del_id);
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, ok,
                                ok ? NULL : "no such profile");
                break;
            }
            case PROFILES_CMD_GET_EXEC_STATUS: {
                size_t len = profiles_build_exec_status(reply);
                bx_reply(ctx->proto, &msg, UART_TASK_ID_PROFILES, reply, len);
                break;
            }
            case PROFILES_CMD_START: {
                if (!bx_args_ok("profiles", &msg, 2)) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                char err_msg[96] = "";
                bool ok = profile_executor_run(msg.payload[1], err_msg, sizeof(err_msg));
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, ok, err_msg);
                break;
            }
            case PROFILES_CMD_STOP: {
                profile_executor_halt();
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, true, NULL);
                break;
            }
            case PROFILES_CMD_PAUSE: {
                bool ok = profile_executor_pause();
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, ok,
                                ok ? NULL : "nothing running to pause");
                break;
            }
            case PROFILES_CMD_RESUME: {
                bool ok = profile_executor_resume();
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, ok,
                                ok ? NULL : "nothing paused to resume");
                break;
            }
            case PROFILES_CMD_ACK_LAST_RUN: {
                bool ok = run_state_acknowledge();
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, ok,
                                ok ? NULL : "no previous-run record to acknowledge");
                break;
            }
            default:
                ESP_LOGW(TAG, "profiles: unknown subcmd 0x%02X -- rejected", subcmd);
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false,
                                "unknown subcommand");
                break;
        }
    }
}

static void profiles_task(void *arg)
{
    profiles_ctx_t *ctx = (profiles_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "profiles: empty payload -- rejected");
            continue;
        }
        bx_handler_args_t args = { .ctx = ctx, .msg = &msg };
        bx_run_on_internal_stack(profiles_handle_message, &args);
    }
}

esp_err_t uart_bridge_start_profiles_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bx_worker_ensure_started()) {
        return ESP_ERR_NO_MEM; /* see uart_bridge_start_control_task() */
    }
    static profiles_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_PROFILES, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }
    BaseType_t created = retry_task_create_pinned(profiles_task, "profiles_uart_bridge", 4096, &ctx, 5);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_PROFILES);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ==========================================================================
 * AUTOTUNE (task 10)
 * ======================================================================== */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} autotune_ctx_t;

static size_t autotune_build_status(uint8_t *out)
{
    autotune_engine_status_t st;
    autotune_engine_get_status(&st);

    size_t o = 0;
    out[o++] = AUTOTUNE_CMD_GET_STATUS;
    out[o++] = (uint8_t)st.state;
    out[o++] = (uint8_t)st.method;
    out[o++] = st.zone_index;
    bx_put_u32_le(&out[o], st.elapsed_s); o += 4;
    bx_put_u16_le(&out[o], st.sample_count); o += 2;
    bx_put_f32_le(&out[o], st.actual_c); o += 4;
    out[o++] = st.actual_valid ? 1 : 0;
    bx_put_f32_le(&out[o], st.duty); o += 4;
    out[o++] = st.model.valid ? 1 : 0;
    bx_put_f32_le(&out[o], st.model.k_gain_c_per_duty); o += 4;
    bx_put_f32_le(&out[o], st.model.tau_s); o += 4;
    bx_put_f32_le(&out[o], st.model.dead_time_s); o += 4;
    bx_put_f32_le(&out[o], st.proposed_gains.kp); o += 4;
    bx_put_f32_le(&out[o], st.proposed_gains.ki); o += 4;
    bx_put_f32_le(&out[o], st.proposed_gains.kd); o += 4;
    out[o++] = (uint8_t)st.proposed_gains.rule;
    bx_put_f32_le(&out[o], st.predicted_max_ramp_c_per_hr); o += 4;
    out[o++] = st.relay.valid ? 1 : 0;
    bx_put_f32_le(&out[o], st.relay.ku); o += 4;
    bx_put_f32_le(&out[o], st.relay.tu_s); o += 4;
    bx_put_f32_le(&out[o], st.relay.amplitude_c); o += 4;
    o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, st.state == AUTOTUNE_ENGINE_ABORTED ? st.abort_reason : "");
    return o;
}

/* Runs on the flash-safe worker: AUTOTUNE_CMD_ACCEPT writes the tuned gains
 * through to NVS via autotune_engine_accept(). */
static void autotune_handle_message(void *vargs)
{
    bx_handler_args_t          *args = (bx_handler_args_t *)vargs;
    autotune_ctx_t             *ctx  = (autotune_ctx_t *)args->ctx;
    const uart_proto_message_t  msg  = *args->msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    uint8_t subcmd = msg.payload[0];

    {
        switch (subcmd) {
            case AUTOTUNE_CMD_GET_STATUS: {
                size_t len = autotune_build_status(reply);
                bx_reply(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, reply, len);
                break;
            }
            case AUTOTUNE_CMD_START: {
                if (!bx_args_ok("autotune", &msg, 16)) break;
                uint8_t zone = msg.payload[1];
                uint8_t method = msg.payload[2];
                float arg_a = bx_f32_le(&msg.payload[3]);   /* step_duty or setpoint_c */
                float relay_d = bx_f32_le(&msg.payload[7]);
                float relay_h = bx_f32_le(&msg.payload[11]);
                uint8_t rule_byte = msg.payload[15];

                char err_msg[96] = "";
                bool ok;
                if (method == AUTOTUNE_METHOD_WIRE_RELAY) {
                    autotune_rule_t rule = (rule_byte == AUTOTUNE_RULE_WIRE_ZN) ? AUTOTUNE_RULE_ZIEGLER_NICHOLS
                                                                                : AUTOTUNE_RULE_TYREUS_LUYBEN;
                    ok = autotune_engine_run_relay(zone, arg_a, relay_d, relay_h, rule, err_msg, sizeof(err_msg));
                } else if (method == AUTOTUNE_METHOD_WIRE_STEP) {
                    /* Wire protocol has no step-rule byte of its own yet -- SIMC only,
                     * same default the HTTP path uses when no rule is given. */
                    ok = autotune_engine_run(zone, arg_a, AUTOTUNE_RULE_SIMC, err_msg, sizeof(err_msg));
                } else {
                    ok = false;
                    snprintf(err_msg, sizeof(err_msg), "method must be 0 (step) or 1 (relay)");
                }
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, subcmd, ok, err_msg);
                break;
            }
            case AUTOTUNE_CMD_ABORT: {
                autotune_engine_abort("aborted from UART GUI");
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, subcmd, true, NULL);
                break;
            }
            case AUTOTUNE_CMD_ACCEPT: {
                bool ok = autotune_engine_accept();
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, subcmd, ok,
                                ok ? NULL : "no completed autotune result to accept");
                break;
            }
            default:
                ESP_LOGW(TAG, "autotune: unknown subcmd 0x%02X -- rejected", subcmd);
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, subcmd, false,
                                "unknown subcommand");
                break;
        }
    }
}

static void autotune_task(void *arg)
{
    autotune_ctx_t *ctx = (autotune_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "autotune: empty payload -- rejected");
            continue;
        }
        bx_handler_args_t args = { .ctx = ctx, .msg = &msg };
        bx_run_on_internal_stack(autotune_handle_message, &args);
    }
}

esp_err_t uart_bridge_start_autotune_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bx_worker_ensure_started()) {
        return ESP_ERR_NO_MEM; /* see uart_bridge_start_control_task() */
    }
    static autotune_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_AUTOTUNE, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }
    /* Restored to 4096 on 2026-08-20, having been shrunk to 3072 earlier the
     * same day. The shrink was a workaround for internal-SRAM exhaustion at
     * task-creation time; that shortfall is fixed at its source now (LVGL's
     * allocator and the Wi-Fi/lwIP pools moved to PSRAM), and more to the
     * point THIS STACK IS NOT IN INTERNAL SRAM AT ALL -- retry_task_create_
     * pinned() allocates it from PSRAM, of which ~8MB is free. Shrinking it
     * therefore bought nothing and cost margin.
     *
     * Not hypothetical: the identical shrink applied to wifi_uart_bridge
     * below overflowed its stack and rebooted the board. See that call. */
    BaseType_t created = retry_task_create_pinned(autotune_task, "autotune_uart_bridge", 4096, &ctx, 5);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_AUTOTUNE);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ==========================================================================
 * WIFI (task 11)
 * ======================================================================== */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} wifi_ctx_t;

static size_t wifi_build_status(uint8_t *out)
{
    size_t o = 0;
    out[o++] = WIFI_CMD_GET_STATUS;
    out[o++] = (uint8_t)wifi_prov_get_mode();
    out[o++] = (uint8_t)wifi_prov_get_state();
    bool connected = wifi_prov_is_sta_connected();
    out[o++] = connected ? 1 : 0;
    o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, wifi_prov_get_saved_ssid());
    o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, wifi_prov_get_ap_ssid());
    o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, wifi_prov_get_ap_password());
    char sta_ip[16] = {0};
    if (connected) {
        wifi_prov_get_sta_ip(sta_ip, sizeof(sta_ip));
    }
    o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, sta_ip);
    out[o++] = (uint8_t)wifi_prov_get_sta_rssi();
    out[o++] = wifi_prov_get_ap_client_count();
    return o;
}

static size_t wifi_build_scan(uint8_t *out)
{
    wifi_prov_scan_result_t results[WIFI_WIRE_MAX_SCAN_ENTRIES + 4];
    size_t count = 0;
    esp_err_t err = wifi_prov_scan(results, sizeof(results) / sizeof(results[0]), &count);
    size_t o = 0;
    out[o++] = WIFI_CMD_SCAN;
    size_t count_pos = o++;
    size_t trunc_pos = o++;
    uint8_t emitted = 0;
    bool truncated = false;
    if (err == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            if (emitted >= WIFI_WIRE_MAX_SCAN_ENTRIES) {
                truncated = true;
                break;
            }
            size_t before = o;
            o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, results[i].ssid);
            if (o + 2 > BRIDGE_REPLY_MAX) {
                o = before;
                truncated = true;
                break;
            }
            out[o++] = (uint8_t)results[i].rssi;
            out[o++] = results[i].secure ? 1 : 0;
            emitted++;
        }
        if (count > emitted) {
            truncated = true;
        }
    }
    out[count_pos] = emitted;
    out[trunc_pos] = truncated ? 1 : 0;
    return o;
}

/* Scan cache for wifi_build_networks() only.
 *
 * WIFI_CMD_GET_NETWORKS lists the SAVED networks; the scan exists solely to
 * annotate each with in-range/RSSI. It was doing a full blocking radio scan on
 * EVERY call, which measured 2.7s per request against 0.2s for every other
 * bridge command on this link -- the scan was the entire difference. Repeating
 * a 2.7s radio scan to refresh a decoration on a list that changes rarely is
 * not a good trade, especially for a UI that polls.
 *
 * Cached for WIFI_NETWORKS_SCAN_CACHE_US. The cost is that in-range/RSSI can
 * be up to that stale; the saved-network list itself is always live, since
 * only the annotation comes from here. An explicit WIFI_CMD_SCAN still forces
 * a fresh scan -- when the user asks to scan, they get a real scan.
 *
 * `static` also deliberately moves this 20-entry array (~720 bytes) OFF the
 * stack. This function runs on wifi_uart_bridge, whose stack overflowed and
 * rebooted the board (see uart_bridge_start_wifi_task); this array was one of
 * the larger contributors.
 *
 * No locking: touched only from wifi_task(), which is single-threaded. Do not
 * call wifi_build_networks() from anywhere else without revisiting that. */
#define WIFI_NETWORKS_SCAN_CACHE_US (30 * 1000 * 1000)

static wifi_prov_scan_result_t s_networks_scan[20];
static size_t                  s_networks_scan_count;
static int64_t                 s_networks_scan_us; /* 0 = never scanned */

static size_t wifi_build_networks(uint8_t *out)
{
    wifi_prov_saved_network_t saved[8];
    size_t saved_count = 0;
    wifi_prov_get_saved_networks(saved, sizeof(saved) / sizeof(saved[0]), &saved_count);

    int64_t now_us = esp_timer_get_time();
    if (s_networks_scan_us == 0 || (now_us - s_networks_scan_us) >= WIFI_NETWORKS_SCAN_CACHE_US) {
        size_t    fresh_count = 0;
        esp_err_t scan_err = wifi_prov_scan(s_networks_scan,
                                            sizeof(s_networks_scan) / sizeof(s_networks_scan[0]),
                                            &fresh_count);
        /* On failure keep whatever the previous scan found rather than
         * dropping every annotation: a stale RSSI is more useful than none,
         * and a failed scan is usually transient (radio busy). The timestamp
         * is still advanced so a persistently failing scan cannot turn this
         * back into a scan-on-every-call path. */
        if (scan_err == ESP_OK) {
            s_networks_scan_count = fresh_count;
        }
        s_networks_scan_us = now_us;
    }

    const wifi_prov_scan_result_t *scanned = s_networks_scan;
    size_t                         scan_count = s_networks_scan_count;

    const char *active_ssid = wifi_prov_get_saved_ssid();
    bool sta_connected = wifi_prov_is_sta_connected();

    size_t o = 0;
    out[o++] = WIFI_CMD_GET_NETWORKS;
    size_t count_pos = o++;
    size_t trunc_pos = o++;
    uint8_t emitted = 0;
    bool truncated = false;

    for (size_t i = 0; i < saved_count && !truncated; i++) {
        const char *ssid = saved[i].ssid;
        bool in_range = false;
        int8_t rssi = 0;
        bool secure = false;
        for (size_t j = 0; j < scan_count; j++) {
            if (strcmp(ssid, scanned[j].ssid) == 0) {
                in_range = true;
                rssi = scanned[j].rssi;
                secure = scanned[j].secure;
                break;
            }
        }
        bool connected = sta_connected && strcmp(ssid, active_ssid) == 0;
        if (emitted >= WIFI_WIRE_MAX_NETWORK_ENTRIES) {
            truncated = true;
            break;
        }
        size_t before = o;
        o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, ssid);
        if (o + 5 > BRIDGE_REPLY_MAX) {
            o = before;
            truncated = true;
            break;
        }
        out[o++] = 1; /* saved */
        out[o++] = in_range ? 1 : 0;
        out[o++] = (uint8_t)rssi;
        out[o++] = secure ? 1 : 0;
        out[o++] = connected ? 1 : 0;
        emitted++;
    }
    if (saved_count > emitted) {
        truncated = true;
    }

    for (size_t j = 0; j < scan_count && !truncated; j++) {
        bool already_saved = false;
        for (size_t i = 0; i < saved_count; i++) {
            if (strcmp(scanned[j].ssid, saved[i].ssid) == 0) {
                already_saved = true;
                break;
            }
        }
        if (already_saved) {
            continue;
        }
        if (emitted >= WIFI_WIRE_MAX_NETWORK_ENTRIES) {
            truncated = true;
            break;
        }
        bool connected = sta_connected && strcmp(scanned[j].ssid, active_ssid) == 0;
        size_t before = o;
        o = bx_put_lstring(out, BRIDGE_REPLY_MAX, o, scanned[j].ssid);
        if (o + 5 > BRIDGE_REPLY_MAX) {
            o = before;
            truncated = true;
            break;
        }
        out[o++] = 0; /* not saved */
        out[o++] = 1; /* in_range */
        out[o++] = (uint8_t)scanned[j].rssi;
        out[o++] = scanned[j].secure ? 1 : 0;
        out[o++] = connected ? 1 : 0;
        emitted++;
    }

    out[count_pos] = emitted;
    out[trunc_pos] = truncated ? 1 : 0;
    return o;
}

static void wifi_task(void *arg)
{
    wifi_ctx_t *ctx = (wifi_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(TAG, "wifi: empty payload -- rejected");
            continue;
        }
        uint8_t subcmd = msg.payload[0];

        switch (subcmd) {
            case WIFI_CMD_GET_STATUS: {
                size_t len = wifi_build_status(reply);
                bx_reply(ctx->proto, &msg, UART_TASK_ID_WIFI, reply, len);
                break;
            }
            case WIFI_CMD_SCAN: {
                size_t len = wifi_build_scan(reply);
                bx_reply(ctx->proto, &msg, UART_TASK_ID_WIFI, reply, len);
                break;
            }
            case WIFI_CMD_ADD_NETWORK: {
                if (!bx_args_ok("wifi", &msg, 2)) break;
                uint8_t ssid_len = msg.payload[1];
                if (!bx_args_ok("wifi", &msg, (size_t)2 + ssid_len + 1)) break;
                char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
                if (ssid_len > WIFI_PROV_SSID_MAX_LEN) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false, "ssid too long");
                    break;
                }
                memcpy(ssid, &msg.payload[2], ssid_len);
                ssid[ssid_len] = '\0';
                size_t p = 2 + ssid_len;
                uint8_t pass_len = msg.payload[p++];
                if (!bx_args_ok("wifi", &msg, p + pass_len)) break;
                char password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
                if (pass_len > WIFI_PROV_PASSWORD_MAX_LEN) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false, "password too long");
                    break;
                }
                memcpy(password, &msg.payload[p], pass_len);
                password[pass_len] = '\0';
                esp_err_t err = wifi_prov_add_network(ssid, ssid_len, password, pass_len);
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, err == ESP_OK,
                                err == ESP_ERR_NO_MEM ? "saved network list is full"
                                                       : (err == ESP_OK ? NULL : "could not save credentials"));
                break;
            }
            case WIFI_CMD_SET_MODE: {
                if (!bx_args_ok("wifi", &msg, 2)) break;
                wifi_prov_mode_t mode = (msg.payload[1] == 1) ? WIFI_PROV_MODE_AP : WIFI_PROV_MODE_HOME;
                esp_err_t err = wifi_prov_set_mode(mode);
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, err == ESP_OK,
                                err == ESP_OK ? NULL : "could not change mode");
                break;
            }
            case WIFI_CMD_SET_AP_IDENTITY: {
                if (!bx_args_ok("wifi", &msg, 2)) break;
                size_t p = 1;
                bool has_ssid = msg.payload[p++] != 0;
                char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
                size_t ssid_len = 0;
                if (has_ssid) {
                    if (!bx_args_ok("wifi", &msg, p + 1)) break;
                    ssid_len = msg.payload[p++];
                    if (ssid_len > WIFI_PROV_SSID_MAX_LEN || !bx_args_ok("wifi", &msg, p + ssid_len)) {
                        bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false,
                                        "ap_ssid too long");
                        break;
                    }
                    memcpy(ssid, &msg.payload[p], ssid_len);
                    ssid[ssid_len] = '\0';
                    p += ssid_len;
                }
                if (!bx_args_ok("wifi", &msg, p + 1)) break;
                bool has_password = msg.payload[p++] != 0;
                char password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
                size_t pass_len = 0;
                if (has_password) {
                    if (!bx_args_ok("wifi", &msg, p + 1)) break;
                    pass_len = msg.payload[p++];
                    if (pass_len > WIFI_PROV_PASSWORD_MAX_LEN || !bx_args_ok("wifi", &msg, p + pass_len)) {
                        bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false,
                                        "ap_password too long");
                        break;
                    }
                    memcpy(password, &msg.payload[p], pass_len);
                    password[pass_len] = '\0';
                }
                esp_err_t err = ESP_OK;
                const char *fail_msg = NULL;
                if (has_ssid) {
                    err = wifi_prov_set_ap_ssid(ssid, ssid_len);
                    if (err != ESP_OK) fail_msg = "ap_ssid must be 1-32 characters";
                }
                if (err == ESP_OK && has_password) {
                    err = wifi_prov_set_ap_password(password, pass_len);
                    if (err != ESP_OK) fail_msg = "ap_password must be empty or 8-63 characters";
                }
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, err == ESP_OK, fail_msg);
                break;
            }
            case WIFI_CMD_GET_NETWORKS: {
                size_t len = wifi_build_networks(reply);
                bx_reply(ctx->proto, &msg, UART_TASK_ID_WIFI, reply, len);
                break;
            }
            case WIFI_CMD_FORGET: {
                if (!bx_args_ok("wifi", &msg, 2)) break;
                uint8_t ssid_len = msg.payload[1];
                if (!bx_args_ok("wifi", &msg, (size_t)2 + ssid_len) || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
                    bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false,
                                    "ssid missing or too long");
                    break;
                }
                char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
                memcpy(ssid, &msg.payload[2], ssid_len);
                ssid[ssid_len] = '\0';
                esp_err_t err = wifi_prov_forget_network(ssid, ssid_len);
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, err == ESP_OK,
                                err == ESP_OK ? NULL : "could not forget network");
                break;
            }
            default:
                ESP_LOGW(TAG, "wifi: unknown subcmd 0x%02X -- rejected", subcmd);
                bx_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false,
                                "unknown subcommand");
                break;
        }
    }
}

esp_err_t uart_bridge_start_wifi_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }
    static wifi_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_WIFI, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }
    /* 8192, raised from 3072. THIS TASK'S STACK OVERFLOW REBOOTED THE BOARD.
     *
     * It is the "spontaneous reboot" this project chased for days. Caught
     * 2026-08-20 by the first coredump the firmware ever successfully wrote:
     *
     *   Panic reason: ***ERROR*** A stack overflow in task wifi_uart_bridg
     *                 has been detected.
     *          TCB             NAME PRIO C/B  STACK USED/FREE
     *   0x3fcc0a7c  wifi_uart_bridg      7/5         3440/376
     *
     * 3440 bytes used against a 3072 request. The earlier estimate quoted
     * here -- "real stack depth is modest (BRIDGE_REPLY_MAX reply buffer
     * plus small fixed SSID/password copies)" -- counted only THIS file's
     * own locals and missed the ESP-IDF Wi-Fi driver call chain underneath
     * WIFI_CMD_SCAN / status queries, which dominates. Reasoning about stack
     * depth from the visible frame is how this got sized wrong; the figure
     * above is measured.
     *
     * Sized at 8192, not 4096: 4096 is only ~19% above the observed
     * high-water, and that high-water came from an ordinary bench session,
     * not a worst case (a scan returning the full WIFI_WIRE_MAX_SCAN_ENTRIES
     * set with a deeper driver path can only be larger). The stack comes
     * from PSRAM via retry_task_create_pinned(), so the extra 4KB costs no
     * internal SRAM whatsoever -- there is no reason to be thrifty here, and
     * being thrifty is exactly what rebooted the kiln controller. */
    BaseType_t created = retry_task_create_pinned(wifi_task, "wifi_uart_bridge", 8192, &ctx, 5);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_WIFI);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
