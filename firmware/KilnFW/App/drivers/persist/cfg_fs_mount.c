#include "cfg_fs_mount.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cfg_fs.h"
#include "cfg_fs_format_gate.h"
#include "boot_guard.h"
#include "flash_worker.h"
#include "flash_worker_wait.h"
#include "zones_config_cfg_fs.h"
#include "pref_cfg_fs.h"
#include "profiles_cfg_fs.h"

static const char *TAG = "cfg_fs";

/* Fails loudly (abort(), via assert()) if any bridge's write function is
 * not the flash-worker-dispatching device writer -- the exact hazard
 * docs/audits/filesystem_migration_review_2026-09-07.md section 1 named:
 * every bridge module defaults its write function to the bare
 * cfg_fs_write_atomic(), which runs flash writes from whatever task calls
 * save() -- the "no flash writes from a PSRAM-stacked task" hazard
 * project_psram_stack_nvs_panic exists to avoid. Called only from
 * finish_mount_after_register() below, right after installing, so the
 * mount and the writer-installation decision can never drift apart (the
 * "reset one side of a pair" bug class this repo keeps re-discovering) --
 * this file is not part of any host test build (App/test/build_host_
 * tests.ps1 never lists cfg_fs_mount.c, since it needs esp_littlefs.h/
 * esp_partition.h, ESP-IDF only), so this assert only ever runs against
 * the real ESP-IDF write functions, never the host fakes
 * *_reset_write_fn_for_test() restores between tests. */
static void cfg_fs_assert_device_write_fns_installed(void)
{
    assert(zones_config_cfg_fs_get_write_fn() == cfg_fs_write_atomic_device);
    assert(pref_cfg_fs_get_write_fn() == cfg_fs_write_atomic_device);
    assert(profiles_cfg_fs_get_write_fn() == cfg_fs_write_atomic_device);
}

/* Review finding (docs/audits/filesystem_migration_review_2026-09-07.md
 * section 1 caveat): installs the flash-worker-dispatching write function
 * on every bridge. Called ONLY from finish_mount_after_register() below --
 * the one function BOTH mount paths (a clean first-try mount, and the
 * confirm-format-then-remount path) funnel through on success, so there is
 * exactly one place that can ever mark the filesystem mounted, and it is
 * the same place that installs the writer. */
static void cfg_fs_install_device_write_fns(void)
{
    zones_config_cfg_fs_set_write_fn(cfg_fs_write_atomic_device);
    pref_cfg_fs_set_write_fn(cfg_fs_write_atomic_device);
    profiles_cfg_fs_set_write_fn(cfg_fs_write_atomic_device);
    cfg_fs_assert_device_write_fns_installed();
}

/* Hand-declared rather than #include "uart_bridge.h" -- same reasoning as
 * factory_reset.c's identical block: that header pulls in hardware-bridge
 * task declarations this file needs none of. Keep in sync with
 * uart_bridge.h by hand if the signature ever changes. */
bool uart_bridge_ext_is_on_flash_worker(void);
bool uart_bridge_ext_flash_worker_started(void);

#define CFG_FS_PARTITION_LABEL "cfg"
#define CFG_FS_SCAN_CHUNK_BYTES 4096

static bool s_format_confirmation_pending = false;
static char s_format_pending_reason[96] = { 0 };

bool cfg_fs_mount_format_confirmation_pending(void)
{
    return s_format_confirmation_pending;
}

const char *cfg_fs_mount_format_pending_reason(void)
{
    return s_format_pending_reason;
}

/* Deferred auto-format state -- see cfg_fs_mount.h's doc comment on the
 * getters below for why this exists and the incident it fixes. Written only
 * by the single background task this module ever creates for this purpose
 * (start_deferred_auto_format() below); read by any task via the getters. Plain (not
 * atomic) reads/writes are acceptable here the same way s_format_confirmation_
 * pending above already is: every field is written by exactly one task, read
 * for observability only (GET /api/cfgfs), and a torn read at worst shows a
 * stale snapshot for one HTTP poll, never a crash or a wrong boot decision. */
static volatile bool     s_auto_format_ever_started = false;
static volatile bool     s_auto_format_in_progress = false;
static volatile bool     s_auto_format_completed = false;
static volatile esp_err_t s_auto_format_result = ESP_ERR_INVALID_STATE;
static volatile int64_t  s_auto_format_start_us = 0;
static volatile int64_t  s_auto_format_end_us = 0;

bool cfg_fs_mount_format_ever_started(void)
{
    return s_auto_format_ever_started;
}

bool cfg_fs_mount_format_in_progress(void)
{
    return s_auto_format_in_progress;
}

bool cfg_fs_mount_format_completed(void)
{
    return s_auto_format_completed;
}

esp_err_t cfg_fs_mount_format_result(void)
{
    return s_auto_format_result;
}

uint32_t cfg_fs_mount_format_elapsed_ms(void)
{
    if (!s_auto_format_ever_started) {
        return 0;
    }
    int64_t end_us = s_auto_format_completed ? s_auto_format_end_us : esp_timer_get_time();
    int64_t elapsed_us = end_us - s_auto_format_start_us;
    if (elapsed_us < 0) {
        return 0;
    }
    return (uint32_t)(elapsed_us / 1000);
}

/* Reads the WHOLE `cfg` partition back in fixed-size chunks and feeds every
 * byte through cfg_fs_format_gate.h's pure decision function -- never holds
 * more than CFG_FS_SCAN_CHUNK_BYTES in RAM at once, since the partition
 * (512 KiB) is too large to buffer whole on this task's stack/heap
 * budget. Returns the esp_partition_read() error the first time one occurs
 * (verdict is then meaningless -- caller must treat a scan failure as "could
 * not determine safety", never as "looks blank"). */
static esp_err_t scan_partition(const esp_partition_t *part, cfg_fs_format_gate_t *gate,
                                 cfg_fs_format_gate_verdict_t *out_verdict)
{
    cfg_fs_format_gate_reset(gate);
    /* HEAP, never the stack: this runs inline on the `main` task during
     * main_boot_early(), whose stack is CONFIG_ESP_MAIN_TASK_STACK_SIZE
     * (8192 B today). A CFG_FS_SCAN_CHUNK_BYTES array here was half of that
     * in one frame -- see docs/audits/boot_hang_2026-09-08.md, where this
     * plus two 7.5 KiB kiln_cfg_store_blob_t stack copies overflowed the
     * main task and panicked the board at boot with IllegalInstruction.
     * check_main_task_stack_budget.py now measures the whole app_main call
     * tree out of the built ELF and fails the build if it can exceed the
     * configured stack. */
    uint8_t *buf = malloc(CFG_FS_SCAN_CHUNK_BYTES);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    size_t offset = 0;
    while (offset < part->size) {
        size_t len = part->size - offset;
        if (len > CFG_FS_SCAN_CHUNK_BYTES) {
            len = CFG_FS_SCAN_CHUNK_BYTES;
        }
        esp_err_t err = esp_partition_read(part, offset, buf, len);
        if (err != ESP_OK) {
            free(buf);
            return err;
        }
        cfg_fs_format_gate_feed(gate, buf, len);
        offset += len;
    }
    free(buf);
    *out_verdict = cfg_fs_format_gate_conclude(gate);
    return ESP_OK;
}

/* Registers the LittleFS VFS mount for `cfg`, format_if_mount_failed=FALSE
 * always -- the auto-format decision above is deliberate and gated on the
 * content scan, never left to esp_littlefs_register()'s own blunt
 * all-or-nothing flag. */
static esp_err_t register_cfg_vfs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/cfg",
        .partition_label = CFG_FS_PARTITION_LABEL,
        .partition = NULL,
        .format_if_mount_failed = false,
    };
    return esp_vfs_littlefs_register(&conf);
}

/* Runs cfg_fs_init() against "/cfg" once the VFS is registered/mounted
 * (fresh format or already-good), logging the .tmp/ sweep result -- shared
 * by both the plain-mount and auto-formatted-then-mounted paths below. */
static esp_err_t finish_mount_after_register(void)
{
    size_t tmp_reaped = 0;
    esp_err_t init_err = cfg_fs_mount_or_skip(false, "/cfg", &tmp_reaped);
    if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "cfg_fs_init(/cfg) failed: %s", esp_err_to_name(init_err));
        return init_err;
    }
    if (tmp_reaped > 0) {
        ESP_LOGW(TAG, "cfg filesystem mounted: swept %u stale temp file(s) from a prior interrupted write",
                 (unsigned)tmp_reaped);
    } else {
        ESP_LOGI(TAG, "cfg filesystem mounted, no stale temp files");
    }

    /* Mounted successfully -- install the flash-worker-dispatching write
     * function on every bridge NOW, in the same call that made the mount
     * real, so the two can never drift apart (see
     * cfg_fs_install_device_write_fns()'s own comment above). This runs on
     * BOTH callers of this function: the plain first-try mount in
     * cfg_fs_mount_device(), and the confirm-format-then-remount path in
     * cfg_fs_confirm_format_job_run() -- a partition formatted on operator
     * confirmation must end up with the writer installed exactly the same
     * as a partition that just mounted cleanly. */
    cfg_fs_install_device_write_fns();

    return ESP_OK;
}

/* Runs the actual format on the flash worker (uart_bridge_ext_run_on_flash_
 * worker()) -- same call cfg_fs_confirm_format_job_run() uses -- from a
 * dedicated background task, never from the boot task. Blocks THIS task for
 * the real duration of the erase (a few seconds typical, tens of seconds
 * worst case per cfg_fs_status.h's CFG_FS_FORMAT_CEILING_MS comment); boot
 * has already moved on by the time this even starts running. */
typedef struct {
    esp_err_t result;
} cfg_fs_auto_format_job_t;

static void cfg_fs_auto_format_job_run(void *arg)
{
    cfg_fs_auto_format_job_t *job = (cfg_fs_auto_format_job_t *)arg;

    esp_err_t fmt_err = esp_littlefs_format(CFG_FS_PARTITION_LABEL);
    if (fmt_err != ESP_OK) {
        ESP_LOGE(TAG, "deferred auto-format: esp_littlefs_format(cfg) failed: %s -- config filesystem "
                      "UNAVAILABLE this boot", esp_err_to_name(fmt_err));
        job->result = fmt_err;
        return;
    }
    esp_err_t reg_err = register_cfg_vfs();
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "deferred auto-format: register after format failed: %s", esp_err_to_name(reg_err));
        job->result = reg_err;
        return;
    }
    job->result = finish_mount_after_register();
}

/* Deferred format is created during main_boot_early(), at
 * tskIDLE_PRIORITY+1 -- the flash-safe worker task itself is not created
 * until main_control_bringup() calls uart_bridge_ext_start_flash_worker(),
 * several boot stages later. The scheduler is free to run this task before
 * that point (equal/low priority, and the main task yields at various
 * points along the way), and uart_bridge_ext_run_on_flash_worker() fails
 * FAST (~0 ms -- "flash-safe worker not started -- job dropped") rather
 * than waiting, when called before the worker exists. That made every
 * cold-boot auto-format fail in ~16 ms, every single time, regardless of
 * how healthy the partition scan and the eventual worker were: a pure
 * boot-ordering race, not a format/partition defect. Poll for readiness
 * first, bounded, so a slow scheduler is not mistaken for a permanent
 * failure. */
/* Shared with every other boot-time migrate-on-load call site
 * (relay_cycles.c, adaptive_tune.c) via flash_worker_wait.h/.c -- see that
 * header's comment for why this moved out of being a private copy here. */
#define CFG_FS_AUTOFMT_WORKER_WAIT_POLL_MS FLASH_WORKER_WAIT_POLL_MS_DEFAULT
#define CFG_FS_AUTOFMT_WORKER_WAIT_CEILING_MS FLASH_WORKER_WAIT_CEILING_MS_DEFAULT

static bool wait_for_flash_worker(void)
{
    return flash_worker_wait_until_started(uart_bridge_ext_flash_worker_started,
                                            CFG_FS_AUTOFMT_WORKER_WAIT_POLL_MS,
                                            CFG_FS_AUTOFMT_WORKER_WAIT_CEILING_MS);
}

static void cfg_fs_auto_format_task(void *arg)
{
    (void)arg;
    s_auto_format_start_us = esp_timer_get_time();
    ESP_LOGW(TAG, "cfg auto-format: background format starting (boot has already continued; poll GET "
                  "/api/cfgfs for progress)");

    cfg_fs_auto_format_job_t job = { .result = ESP_ERR_INVALID_STATE };
    esp_err_t dispatch_err;
    if (!wait_for_flash_worker()) {
        ESP_LOGE(TAG, "cfg auto-format: flash-safe worker still not started after %u ms -- giving up",
                 (unsigned)CFG_FS_AUTOFMT_WORKER_WAIT_CEILING_MS);
        dispatch_err = ESP_ERR_TIMEOUT;
    } else {
        /* Not reachable on-worker: this is a brand-new task
         * (cfg_fs_auto_format_task, created once by start_deferred_auto_format())
         * whose entire body is this function -- it has no other caller and never
         * runs on the flash worker itself, so the reentrancy guard other
         * dispatchers in this file use is unnecessary here, but dispatching
         * unconditionally (never inline) keeps this task's own tiny stack out of
         * the flash-write path regardless. */
        dispatch_err = uart_bridge_ext_run_on_flash_worker(cfg_fs_auto_format_job_run, &job);
    }
    esp_err_t final_result = (dispatch_err != ESP_OK) ? dispatch_err : job.result;

    s_auto_format_end_us = esp_timer_get_time();
    s_auto_format_result = final_result;
    s_auto_format_completed = true;
    s_auto_format_in_progress = false;

    uint32_t dur_ms = (uint32_t)((s_auto_format_end_us - s_auto_format_start_us) / 1000);
    if (final_result == ESP_OK) {
        ESP_LOGW(TAG, "cfg auto-format: completed OK in %u ms -- cfg filesystem now available", (unsigned)dur_ms);
    } else {
        ESP_LOGE(TAG, "cfg auto-format: FAILED after %u ms: %s -- cfg filesystem remains unavailable this boot",
                 (unsigned)dur_ms, esp_err_to_name(final_result));
    }
    vTaskDelete(NULL);
}

/* Starts the deferred background format. Returns false (logs its own error)
 * if the task could not even be created -- caller must treat that exactly
 * like any other "could not format" outcome. */
static bool start_deferred_auto_format(void)
{
    s_auto_format_ever_started = true;
    s_auto_format_in_progress = true;
    s_auto_format_completed = false;
    s_auto_format_result = ESP_ERR_INVALID_STATE;
    s_auto_format_start_us = 0;
    s_auto_format_end_us = 0;

    /* Plain internal-RAM stack (xTaskCreate(), never the PSRAM-capable
     * xTaskCreateWithCaps/EXT variant) -- this task itself never touches
     * flash directly (it dispatches to the flash worker, which owns its own
     * safe stack), but keeping it internal-RAM is the same standing rule as
     * every other flash-adjacent task in this codebase
     * (project_psram_stack_nvs_panic). 3072 words is generous for a function
     * whose own frames are a handful of locals plus one dispatch call. */
    BaseType_t created = xTaskCreate(cfg_fs_auto_format_task, "cfg_autofmt", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "cfg auto-format: xTaskCreate() failed -- cfg filesystem UNAVAILABLE this boot");
        s_auto_format_in_progress = false;
        s_auto_format_completed = true;
        s_auto_format_result = ESP_ERR_NO_MEM;
        return false;
    }
    return true;
}

/* Owner decision 2026-09-07 (docs/FILESYSTEM_USER_DATA_PLAN.md section 5
 * step 1): "Auto format, dont require all ff, search for valid files/
 * partitions ask the user if it is ok to overwright if partitions/files
 * found." Called only after esp_vfs_littlefs_register() has already failed
 * once above -- never runs against a partition that mounted cleanly.
 *
 * Never formats blind: finds the `cfg` partition, scans every byte through
 * cfg_fs_format_gate.h, and only formats when that scan concludes there is
 * no evidence of real content. Any other outcome -- content found, or the
 * scan itself failing -- leaves the partition untouched and sets the
 * awaiting-confirmation flag so the web UI and boot log can ask instead of
 * guessing.
 *
 * The scan above is fast and bounded (a few ms: 512 KiB read back in 4 KiB
 * chunks) and stays inline. The format itself is NOT run here any more --
 * see cfg_fs_mount.h's doc comment on the deferred-format getters for why
 * (docs/audits/boot_hang_2026-09-08.md: an inline format could run long
 * enough to starve the RTC watchdog before monitor_task.c ever starts
 * feeding it, turning a slow-but-honest format into a boot-time reset loop).
 * When the scan says SAFE_TO_FORMAT, this function only STARTS the deferred
 * background task and returns immediately -- it always returns the caller's
 * original mount-failure error in that case, same as the "awaiting
 * confirmation" outcome, because cfg_fs is genuinely still unavailable at
 * the moment this function returns; the deferred task is what eventually
 * makes it available, asynchronously, and GET /api/cfgfs's "format" section
 * is how that gets observed instead of guessed at. */
static esp_err_t maybe_auto_format_and_remount(esp_err_t original_mount_err)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                                            CFG_FS_PARTITION_LABEL);
    if (!part) {
        /* Genuinely absent from partitions.csv -- nothing to scan, nothing
         * to format. Unchanged from the pre-auto-format behaviour: report
         * loudly, degrade to defaults, never touch flash. */
        ESP_LOGE(TAG, "cfg partition not found -- cannot scan for auto-format (partitions.csv missing the "
                      "`cfg` entry?)");
        return original_mount_err;
    }

    /* HEAP, never the stack: cfg_fs_format_gate_t carries an 8192 B verbatim
     * copy of the partition's two metadata blocks (see cfg_fs_format_gate.h's
     * `header[CFG_FS_FORMAT_GATE_HEADER_BYTES]`). A stack-local `gate` here
     * used to combine with scan_partition() (inlined into this function by
     * the compiler) to put ~8.4 KiB in this function's own frame -- measured
     * by check_main_task_stack_budget.py as the deepest app_main path,
     * 8704 B against the 6144 B budget on an 8192 B main-task stack. Same
     * "malloc failure degrades, never panics" convention as scan_partition()'s
     * own chunk buffer just above: if the allocation fails, this treats the
     * scan as unreadable and asks for operator confirmation instead of
     * auto-formatting -- never a crash. */
    cfg_fs_format_gate_t *gate = malloc(sizeof(*gate));
    if (!gate) {
        ESP_LOGE(TAG, "could not allocate cfg_fs_format_gate_t (%u B) to judge auto-format safety -- refusing "
                      "to format, treating as awaiting confirmation", (unsigned)sizeof(*gate));
        s_format_confirmation_pending = true;
        snprintf(s_format_pending_reason, sizeof(s_format_pending_reason),
                 "partition could not be scanned: out of memory");
        return original_mount_err;
    }

    cfg_fs_format_gate_verdict_t verdict;
    esp_err_t scan_err = scan_partition(part, gate, &verdict);
    if (scan_err != ESP_OK) {
        ESP_LOGE(TAG, "could not read back cfg partition to judge auto-format safety: %s -- refusing to "
                      "format, treating as awaiting confirmation", esp_err_to_name(scan_err));
        s_format_confirmation_pending = true;
        snprintf(s_format_pending_reason, sizeof(s_format_pending_reason),
                 "partition could not be read back: %s", esp_err_to_name(scan_err));
        free(gate);
        return original_mount_err;
    }

    char reason[sizeof(s_format_pending_reason)];
    cfg_fs_format_gate_describe(gate, verdict, reason, sizeof(reason));
    free(gate);
    gate = NULL;

    if (verdict != CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT) {
        s_format_confirmation_pending = true;
        strncpy(s_format_pending_reason, reason, sizeof(s_format_pending_reason) - 1);
        s_format_pending_reason[sizeof(s_format_pending_reason) - 1] = '\0';
        ESP_LOGE(TAG, "cfg partition failed to mount and APPEARS TO CONTAIN DATA (%s) -- refusing to "
                      "auto-format; an operator must explicitly confirm via POST /api/cfgfs/format_confirm "
                      "before this partition is ever erased", reason);
        return original_mount_err;
    }

    ESP_LOGW(TAG, "cfg partition failed to mount (%s) and the content scan found %s -- deferring format to a "
                  "background task so boot never blocks on it", esp_err_to_name(original_mount_err), reason);
    if (!start_deferred_auto_format()) {
        return ESP_ERR_NO_MEM;
    }
    /* cfg_fs stays UNAVAILABLE for THIS boot's return from cfg_fs_mount_device()
     * -- the deferred task installs the mount and write functions later, off
     * this call stack entirely. */
    return original_mount_err;
}

esp_err_t cfg_fs_mount_device(void)
{
    if (cfg_fs_get_status() == CFG_FS_STATUS_MOUNTED) {
        return ESP_OK; /* idempotent, same convention as log_store_mount() */
    }

    bool recovery = boot_guard_is_recovery_mode();
    if (recovery) {
        ESP_LOGI(TAG, "recovery mode: skipping cfg filesystem mount entirely");
        return cfg_fs_mount_or_skip(true, "/cfg", NULL);
    }

    /* format_if_mount_failed = FALSE, deliberately the opposite of
     * log_store_mount()'s `logs` policy -- see cfg_fs.h's file banner.
     * Losing user tuning/profile data to a silent reformat is the worst
     * possible outcome here; the auto-format decision below is a
     * deliberate, content-scanned choice, never this flag's blunt
     * all-or-nothing one. */
    esp_err_t err = register_cfg_vfs();
    if (err != ESP_OK) {
        esp_err_t auto_fmt_err = maybe_auto_format_and_remount(err);
        if (auto_fmt_err != ESP_OK) {
            /* Every cfg_fs_*() call fails clean with ESP_ERR_INVALID_STATE
             * from here on this boot; every caller degrades to firmware
             * defaults (docs/FILESYSTEM_USER_DATA_PLAN.md's mount-failure
             * contract). maybe_auto_format_and_remount() has already logged
             * the specific reason (absent partition, refused-pending-
             * confirmation, or a format/re-register failure). */
            return auto_fmt_err;
        }
        /* Auto-formatted and freshly registered -- fall through to the
         * normal cfg_fs_init() finish below, same as a clean first-try
         * mount. */
    }

    return finish_mount_after_register();
}

typedef struct {
    const char *rel_path;
    const void *data;
    size_t      len;
    esp_err_t   result;
} cfg_fs_write_job_t;

static void cfg_fs_write_job_run(void *arg)
{
    cfg_fs_write_job_t *job = (cfg_fs_write_job_t *)arg;
    job->result = cfg_fs_write_atomic(job->rel_path, job->data, job->len);
}

/* Reachable while already running ON bx_flash_worker -- concrete deadlock
 * path: CONTROL_CMD_SET_UNIT_PREF already runs on that worker ->
 * unit_pref_set() -> pref_cfg_fs_save() -> this function -> the dispatch
 * call below, again from inside itself. Dispatching onto a worker task
 * from a callback already running on that same worker deadlocks the board
 * (CLAUDE.md's flash-worker-reentrancy note; project_flash_worker_
 * reentrancy). The host stub models no lock, so no existing test could see
 * this -- found by check_flash_worker_lint.ps1's reentrancy-guard rule
 * (bac50dbc). Fixed the same way every other dispatcher in this codebase
 * (e.g. cfg_fs_confirm_format_device() below) handles the same hazard:
 * check the re-entrancy guard immediately below first and run the write
 * inline when already on the worker, dispatch only when not. */
esp_err_t cfg_fs_write_atomic_device(const char *rel_path, const void *data, size_t len)
{
    if (!rel_path) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg_fs_write_job_t job = { .rel_path = rel_path, .data = data, .len = len, .result = ESP_ERR_INVALID_STATE };
    if (uart_bridge_ext_is_on_flash_worker()) {
        cfg_fs_write_job_run(&job);
        return job.result;
    }
    esp_err_t dispatch_err = uart_bridge_ext_run_on_flash_worker(cfg_fs_write_job_run, &job);
    if (dispatch_err != ESP_OK) {
        return dispatch_err;
    }
    return job.result;
}

typedef struct {
    esp_err_t result;
} cfg_fs_confirm_format_job_t;

/* Runs entirely on the flash worker task: unmounts (best-effort -- it may
 * not be mounted at all, that is the common case this exists for),
 * unconditionally formats, re-registers, and re-runs cfg_fs_init(). Calling
 * this function at all IS the explicit confirmation -- unlike
 * maybe_auto_format_and_remount() above, it never scans first. */
static void cfg_fs_confirm_format_job_run(void *arg)
{
    cfg_fs_confirm_format_job_t *job = (cfg_fs_confirm_format_job_t *)arg;

    if (cfg_fs_get_status() == CFG_FS_STATUS_MOUNTED) {
        esp_vfs_littlefs_unregister(CFG_FS_PARTITION_LABEL);
        cfg_fs_deinit();
    }

    esp_err_t fmt_err = esp_littlefs_format(CFG_FS_PARTITION_LABEL);
    if (fmt_err != ESP_OK) {
        ESP_LOGE(TAG, "cfg_fs_confirm_format_device: esp_littlefs_format(cfg) failed: %s",
                 esp_err_to_name(fmt_err));
        job->result = fmt_err;
        return;
    }

    esp_err_t reg_err = register_cfg_vfs();
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "cfg_fs_confirm_format_device: register after format failed: %s",
                 esp_err_to_name(reg_err));
        job->result = reg_err;
        return;
    }

    job->result = finish_mount_after_register();
    if (job->result == ESP_OK) {
        s_format_confirmation_pending = false;
        s_format_pending_reason[0] = '\0';
        ESP_LOGW(TAG, "cfg partition formatted on explicit operator confirmation and mounted fresh");
    }
}

esp_err_t cfg_fs_confirm_format_device(void)
{
    cfg_fs_confirm_format_job_t job = { .result = ESP_ERR_INVALID_STATE };

    /* Same re-entrancy guard as factory_reset.c's execute_scope() and
     * cfg_fs_write_atomic_device()'s own doc comment: no caller today is
     * expected to already be on the flash worker, but the check is cheap
     * and this is exactly the bug class that stays invisible until a caller
     * changes. */
    if (uart_bridge_ext_is_on_flash_worker()) {
        cfg_fs_confirm_format_job_run(&job);
    } else {
        esp_err_t dispatch_err = uart_bridge_ext_run_on_flash_worker(cfg_fs_confirm_format_job_run, &job);
        if (dispatch_err != ESP_OK) {
            return dispatch_err;
        }
    }
    return job.result;
}
