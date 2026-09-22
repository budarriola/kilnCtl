#include "crash_report.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h" /* RTC_NOINIT_ATTR -- s_uptime_beacon below, same storage class as
                         * boot_guard.c's s_bg_rtc (see that file's header comment) */
#include "esp_crc.h"
#include "esp_core_dump.h" /* esp_core_dump_summary_t/esp_core_dump_get_summary() -- full-summary parsing stays above hal_sysinfo, see that header's top comment */
#include "esp_log.h"

#include "hal_kv.h"
#include "hal_time.h" /* hal_time_now_us() -- crash_report_note_alive()'s RTC-memory beacon */
#include "nvs_key_check.h"
#include "hal_sysinfo.h" /* hal_sysinfo_coredump_present()/_erase(), hal_sysinfo_reset_reason(),
                           * hal_sysinfo_get_build_info() (fw_build, v3) */
#include "hal_esp_common.h" /* hal_status_to_esp_err() -- preserve the specific esp_err_t from
                              * hal_sysinfo_coredump_erase()/hal_kv failures rather than collapsing
                              * to ESP_FAIL */

static const char *TAG = "crash_report";

/* Hand-declared rather than #include "uart_bridge.h" -- same reasoning as
 * relay_cycles.c's/safety_cfg_store.c's identical block: that header pulls
 * in ILI9488.h/screen_idle.h/kiln_io.h for hardware-bridge task
 * declarations this file needs none of. Kept in sync by hand if either
 * signature ever changes.
 *
 * WHY THIS MODULE NEEDS IT (added 2026-09-15,
 * docs/audits/review_crash_report_relay_gate_61765de7_2026-09-15.md, comment
 * corrected 2026-09-15 per docs/audits/review_crash_gate_followups_62e95bbd_
 * 2026-09-15.md LOW-2): once the LCD diagnostics page gained its own
 * Acknowledge control, crash_report_acknowledge()/crash_report_clear()'s NVS
 * writes (persist() below) could be reached from lvgl_task, not just from
 * diagnostics_http.c's httpd-task POST handlers. `lvgl_task`'s stack is
 * actually static internal SRAM (lvgl_port.c's s_lvgl_task_stack, since the
 * 2026-08-21 "REVERTED TO INTERNAL SRAM" fix) -- NOT PSRAM as an earlier
 * version of this comment claimed -- so the cache-disabled-PSRAM-stack abort
 * this comment used to warn about does not actually apply to that caller.
 * The dispatch is kept anyway for two real reasons: (1) it is still needed
 * for any future/other caller whose stack genuinely is PSRAM-backed, and (2)
 * routing every write through the single flash-worker task also SERIALIZES
 * all NVS load-modify-store sequences against each other (see crash_ack_job()
 * below, LOW-3 fix), which a direct write from either caller's own task would
 * not. The re-entrancy check matches relay_cycles_reset()'s. Known callers:
 * diagnostics_http.c's httpd-task POST handlers, and the LCD diagnostics-page
 * Acknowledge control on lvgl_task -- routing both through one worker means
 * neither has to know which task it is running on, and their reads/writes of
 * the crash record can never interleave. */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);
bool uart_bridge_ext_is_on_flash_worker(void);
/* Bounded-wait sibling used ONLY by crash_report_acknowledge_timeout() below
 * (the LCD Acknowledge path) -- see flash_worker.h's own doc comment for the
 * full rationale (MEDIUM 1, docs/audits/review_crash_gate_low_fixes_
 * c534a0df_2026-09-15.md). */
esp_err_t uart_bridge_ext_run_on_flash_worker_timeout(void (*fn)(void *arg), void *arg, uint32_t timeout_ms);

/* Cached mirror of "have_record && !acknowledged" -- see crash_report.h's
 * crash_report_has_unacknowledged() doc comment. Defaults to false (matches
 * crash_report_get()'s own "nothing yet" default) and is only ever written
 * from refresh_unacked_cache()/crash_report_acknowledge()/crash_report_clear()
 * below, each after an NVS operation, never read from inside one -- the
 * point of this flag is that kiln_io_owner.c's relay path can read it
 * without touching NVS at all. */
static bool s_have_unacked_crash = false;

/* Same namespace as run_state.c/ota_record.c/relay_cycles.c, own key -- see
 * run_state.c's header comment for why a shared blob is the wrong move here
 * too: a corrupt/rejected crash record must never be able to take another
 * module's breadcrumb down with it, and vice versa. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_CRASH "crash_rpt"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_CRASH);

/* TODO.md 8.1's shared partition, split out of the default NVS partition,
 * managed independently by each module that uses it -- see run_state.c. */
#define KILN_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);

/* Pins the on-disk layout, same reasoning and same portability trick as
 * ota_record.c's ota_record_t_size_check: this file is ALSO compiled
 * directly into App/test/build_host_tests.ps1's MSVC host-test binary (via
 * test_crash_report.c's #include of this file), and that cl.exe invocation
 * compiles in a C mode old enough that a plain _Static_assert is a hard
 * syntax error. The classic negative-array-size trick is portable C89/C99/
 * C11 alike and checks exactly the same thing. Update this literal whenever
 * crash_report_record_t's layout changes, alongside bumping
 * CRASH_REPORT_RECORD_VERSION. */
typedef char crash_report_record_t_size_check[(sizeof(crash_report_record_t) == 208) ? 1 : -1];

/* ---------------------------------------------------------------------------
 * Uptime beacon (v3, ROADMAP.md follow-up) -- RTC memory, NOT NVS. Same
 * storage class and same magic-guarded-against-power-on-garbage pattern as
 * boot_guard.c's s_bg_rtc (see that file's header comment for the full
 * "why RTC memory, not NVS" rationale: it survives a software reset/panic/
 * watchdog reset and is only lost on a power cycle, and unlike NVS it can be
 * written at a frequent, fixed cadence with zero flash wear and zero risk to
 * a task with a non-internal-SRAM stack, since RTC_NOINIT_ATTR storage is
 * ordinary memory, not a flash write path).
 *
 * WHY THIS IS ONLY "APPROXIMATE": monitor_task.c's heartbeat calls
 * crash_report_note_alive() once per heartbeat cycle (~300 ms by default,
 * see monitor_task.c), not from the panic handler itself -- a genuine
 * lockup/panic can occur up to one heartbeat period after the last update.
 * That is still far more informative than "unknown", which is what every
 * crash record reported before this field existed.
 * ------------------------------------------------------------------------- */
#define CRASH_UPTIME_BEACON_MAGIC 0x43554231u /* "CUB1" */

typedef struct {
    uint32_t magic;
    uint32_t uptime_s;
} crash_uptime_beacon_t;

/* Deliberately NOT zeroed by startup code (RTC_NOINIT_ATTR), so it carries
 * across a software reset/panic -- garbage after a genuine power-on is
 * rejected by the magic check in crash_report_init() below. */
RTC_NOINIT_ATTR static crash_uptime_beacon_t s_uptime_beacon;

void crash_report_note_alive(void)
{
    s_uptime_beacon.uptime_s = (uint32_t)(hal_time_now_us() / 1000000ull);
    s_uptime_beacon.magic = CRASH_UPTIME_BEACON_MAGIC; /* written last, after uptime_s -- a reset
                                                          * landing between these two writes leaves
                                                          * a bad magic, correctly read as "unknown"
                                                          * rather than a half-updated uptime_s. */
}

/* Brings up KILN_NVS_PARTITION, erasing ONLY that partition if its contents
 * are unusable -- identical to run_state.c's/ota_record.c's own
 * nvs_partition_init(), duplicated rather than shared for the same reason
 * each of those already duplicates it independently. Cheap to call more than
 * once: nvs_flash_init_partition() is a no-op success if already up. */
static esp_err_t nvs_partition_init(const char *partition)
{
    return hal_status_to_esp_err(hal_kv_init_partition(partition));
}

/* ---------------------------------------------------------------------------
 * Pure helpers -- no ESP-IDF I/O, exercised directly by host tests via this
 * file's #include into test_crash_report.c (same convention as
 * test_safety_cfg_store.c's #include of safety_cfg_store.c).
 * ------------------------------------------------------------------------- */

/* esp_crc32_le() over the record with the crc32 field itself zeroed --
 * the OWNER'S EXPLICIT REQUIREMENT. Computed over a local copy so the
 * caller's record (which may have a real, non-zero crc32 already in it, e.g.
 * when re-validating a loaded record) is never mutated by asking. */
static uint32_t compute_crc(const crash_report_record_t *rec)
{
    crash_report_record_t tmp = *rec;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

/* True only if `rec` is a version this build recognizes AND its stored CRC
 * matches a fresh recomputation. A version mismatch is treated identically
 * to a CRC mismatch -- both mean "do not trust these bytes as a current
 * record" -- but is checked first since it also protects a future version's
 * differently-shaped struct from having compute_crc() applied to fields at
 * the wrong offsets. */
static bool record_valid(const crash_report_record_t *rec)
{
    if (rec->version != CRASH_REPORT_RECORD_VERSION) {
        return false;
    }
    return compute_crc(rec) == rec->crc32;
}

/* Stamps rec->crc32 from compute_crc() -- the one and only place a record is
 * ever made ready to persist. */
static void seal_crc(crash_report_record_t *rec)
{
    rec->crc32 = compute_crc(rec);
}

/* espcoredump stores exc_pc as esp_cpu_process_stack_pc(raw_pc), which is
 * `raw_pc - 3` (components/xtensa/include/esp_cpu_utils.h). A raw PC of 0
 * therefore lands in the record as 0 - 3 == 0xfffffffd. That is not a code
 * address on any ESP32-S3 image, so a record carrying it describes a frame
 * whose PC field was never a PC -- see this file's public doc comment. */
#define CRASH_REPORT_PC_OF_ZERO 0xfffffffdu

bool crash_report_frame_trustworthy(const crash_report_record_t *rec)
{
    if (!rec) {
        return false;
    }
    if (rec->bt_corrupted) {
        return false;
    }
    if (rec->exc_pc == CRASH_REPORT_PC_OF_ZERO) {
        return false;
    }
    return true;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/* Xtensa EXCCAUSE mnemonics this board can actually hit (Xtensa ISA table
 * 4-64), decoded for the web page so "exc_cause=9" doesn't have to be looked
 * up by hand. Deliberately short: an unrecognised cause (a future core
 * variant, or a value this table hasn't been extended for) falls through to
 * "" rather than guessing, same "never invent a confident-looking answer"
 * discipline as run_state.h's no-wall-clock rule. */
static const char *decode_exccause(uint32_t cause)
{
    switch (cause) {
    case 0:  return "IllegalInstruction";
    case 1:  return "Syscall";
    case 2:  return "InstructionFetchError";
    case 3:  return "LoadStoreError";
    case 4:  return "Level1Interrupt";
    case 5:  return "Alloca";
    case 6:  return "IntegerDivideByZero";
    case 8:  return "Privileged";
    case 9:  return "LoadStoreAlignment";
    case 12: return "InstrPIFDataError";
    case 13: return "LoadStorePIFDataError";
    case 14: return "InstrPIFAddrError";
    case 15: return "LoadStorePIFAddrError";
    case 16: return "InstTLBMiss";
    case 17: return "InstTLBMultiHit";
    case 18: return "InstFetchPrivilege";
    case 20: return "InstFetchProhibited";
    case 24: return "LoadStoreTLBMiss";
    case 25: return "LoadStoreTLBMultiHit";
    case 26: return "LoadStorePrivilege";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "";
    }
}

/* Must be called with a fully-zeroed *out (caller's job -- see the one call
 * site in crash_report_init()). Pure struct-fill: no ESP-IDF I/O, but does
 * take live ESP-IDF types (esp_core_dump_summary_t) as input, which is why
 * this function itself is NOT split out for a host test the way compute_crc/
 * record_valid are -- esp_core_dump.h drags in xtensa-specific headers this
 * project's host-test stubs do not (and should not) attempt to shadow. */
static void fill_from_summary(crash_report_record_t *out, const esp_core_dump_summary_t *summary,
                               const char *reset_reason_name)
{
    out->version = CRASH_REPORT_RECORD_VERSION;
    out->acknowledged = 0;
    out->exc_cause = summary->ex_info.exc_cause;
    out->exc_pc = summary->exc_pc;
    out->exc_addr = summary->ex_info.exc_vaddr;
    /* a0/a1 of the crashing frame -- see crash_report.h's field comments for
     * why a1 (the stack pointer) is the field that disambiguates a NULL
     * struct-pointer dereference from a NULL/garbage stack pointer. */
    out->exc_a0 = summary->ex_info.exc_a[0];
    out->exc_a1 = summary->ex_info.exc_a[1];

    uint32_t depth = summary->exc_bt_info.depth;
    if (depth > CRASH_REPORT_BT_MAX) {
        depth = CRASH_REPORT_BT_MAX;
    }
    out->bt_count = (uint8_t)depth;
    out->bt_corrupted = summary->exc_bt_info.corrupted ? 1u : 0u;
    for (uint32_t i = 0; i < depth; i++) {
        out->backtrace_pc[i] = summary->exc_bt_info.bt[i];
    }

    copy_str(out->exc_task, sizeof(out->exc_task), summary->exc_task);
    copy_str(out->exc_cause_str, sizeof(out->exc_cause_str), decode_exccause(summary->ex_info.exc_cause));
    copy_str(out->reset_reason, sizeof(out->reset_reason), reset_reason_name);
}

/* v3 fields -- pure, no ESP-IDF I/O, exercised directly by host tests (same
 * "pure helper, no live IDF types" convention as compute_crc/record_valid
 * above, unlike fill_from_summary() which takes the live esp_core_dump_
 * summary_t and is therefore NOT host-testable -- see this file's own
 * comment on fill_from_summary()).
 *
 * `beacon`/`build_info` are read-only inputs, not this module's globals
 * directly, so a test can construct both by hand without touching
 * s_uptime_beacon or a real hal_sysinfo backend. */
static void fill_v3_fields(crash_report_record_t *out, const crash_uptime_beacon_t *beacon,
                            const hal_sysinfo_build_info_t *build_info)
{
    if (beacon->magic == CRASH_UPTIME_BEACON_MAGIC) {
        out->crash_uptime_s = beacon->uptime_s;
        out->crash_uptime_known = 1u;
    } else {
        out->crash_uptime_s = 0u;
        out->crash_uptime_known = 0u;
    }

    if (build_info->valid) {
        snprintf(out->fw_build, sizeof(out->fw_build), "%s %s", build_info->date, build_info->time);
    } else {
        out->fw_build[0] = '\0';
    }
}

/* Identity hash for "which coredump is this" (crash_report_record_t.dump_id).
 *
 * Deliberately NOT a CRC over the raw esp_core_dump_summary_t: that struct is
 * stack-allocated by the caller and esp_core_dump_get_summary() only writes
 * the fields it knows about, so any compiler padding between them (real IDF
 * layout, e.g. esp_core_dump_bt_info_t's trailing `bool corrupted` followed
 * by 3 padding bytes, and app_elf_sha256[66] followed by 2 padding bytes
 * before the next 4-byte field) is whatever garbage happened to be on the
 * stack that boot. Hashing the whole struct therefore hashed that garbage
 * too, so an identical crash produced a different dump_id on every boot --
 * observed on hardware: an acknowledged record read back as unacknowledged,
 * with identical PC/backtrace, on three consecutive boots after an OTA.
 * Hashing only the fields that actually identify a crash (exception PC, the
 * faulting task's name, and the backtrace bounded to its own depth) is both
 * deterministic across boots AND immune to a future esp_core_dump_summary_t
 * layout change adding/reordering fields -- no explicit memset of caller
 * memory can protect against that, only pinning down what "identity" means. */
static uint32_t crash_report_dump_id(const esp_core_dump_summary_t *summary)
{
    uint32_t crc = 0;
    crc = esp_crc32_le(crc, (const uint8_t *)&summary->exc_pc, sizeof(summary->exc_pc));

    /* exc_task is itself a fixed-size buffer holding a NUL-terminated task
     * name -- only strnlen() bytes of it are meaningful. Bytes after the NUL
     * are exactly the same kind of "never explicitly written" filler as the
     * struct padding this function exists to ignore, so normalize them to a
     * fixed value (0) before hashing rather than hashing the buffer verbatim. */
    char task_buf[sizeof(summary->exc_task)];
    memset(task_buf, 0, sizeof(task_buf));
    size_t task_len = strnlen(summary->exc_task, sizeof(summary->exc_task));
    memcpy(task_buf, summary->exc_task, task_len);
    crc = esp_crc32_le(crc, (const uint8_t *)task_buf, sizeof(task_buf));

    uint32_t depth = summary->exc_bt_info.depth;
    uint32_t bt_max = (uint32_t)(sizeof(summary->exc_bt_info.bt) / sizeof(summary->exc_bt_info.bt[0]));
    if (depth > bt_max) {
        depth = bt_max;
    }
    crc = esp_crc32_le(crc, (const uint8_t *)&depth, sizeof(depth));
    crc = esp_crc32_le(crc, (const uint8_t *)summary->exc_bt_info.bt, depth * sizeof(summary->exc_bt_info.bt[0]));
    return crc;
}

/* ---------------------------------------------------------------------------
 * NVS I/O -- same shape as run_state.c's persist_locked()/loader, minus the
 * lock (this module has no periodic-refresh writer to race with: it writes
 * at most once per boot, from crash_report_init(), and otherwise only from
 * an HTTP handler acknowledging/clearing -- both single-shot, not a
 * high-frequency writer needing run_state.c's mutex).
 * ------------------------------------------------------------------------- */

static esp_err_t persist(const crash_report_record_t *rec)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_CRASH, rec, sizeof(*rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

/* Loads the stored record. Returns true (and fills *out) only if a blob is
 * present, the right size, and passes record_valid() (version + CRC) --
 * anything else (not found, wrong size, corrupted) is treated as "no
 * record", never as a reason to hand back partially-trusted bytes. */
static bool load(crash_report_record_t *out)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false;
    }
    crash_report_record_t rec;
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_CRASH, &rec, &len);
    hal_kv_close(&h);

    if (err != HAL_OK || len != sizeof(rec)) {
        return false;
    }
    /* Strings come out of flash and are about to be logged/emitted as JSON --
     * terminate defensively, same reasoning as run_state.c's loader. */
    rec.exc_task[sizeof(rec.exc_task) - 1] = '\0';
    rec.exc_cause_str[sizeof(rec.exc_cause_str) - 1] = '\0';
    rec.reset_reason[sizeof(rec.reset_reason) - 1] = '\0';
    rec.fw_build[sizeof(rec.fw_build) - 1] = '\0';

    if (!record_valid(&rec)) {
        ESP_LOGW(TAG, "stored crash record failed version/CRC check -- treating as no record");
        return false;
    }
    *out = rec;
    return true;
}

/* Refreshes s_have_unacked_crash from whatever is currently persisted.
 * Called once from crash_report_init() (covers the "no new coredump this
 * boot, but an old unacknowledged record still exists" case, not just a
 * freshly captured one) and is the only place that reads NVS to decide the
 * flag -- crash_report_acknowledge()/crash_report_clear() below update it
 * directly from their own known outcome instead of re-loading. */
static void refresh_unacked_cache(void)
{
    crash_report_record_t rec;
    s_have_unacked_crash = load(&rec) && !rec.acknowledged;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void crash_report_init(void)
{
    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- crash record cannot be captured/read",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
        return;
    }

    /* Reflects whatever is ALREADY persisted before deciding whether there is
     * a NEW coredump to capture this boot -- an old unacknowledged record
     * from a previous boot must show up in the cache even on a boot with no
     * fresh coredump at all (the branch just below returns early in that
     * case). Recomputed again below only if a new record actually gets
     * captured. */
    refresh_unacked_cache();

    if (!hal_sysinfo_coredump_present()) {
        /* No coredump present (the ordinary case) or unreadable -- nothing
         * for this module to capture. main.c's own coredump-check block
         * already logs the outcome; this module does not repeat it. */
        return;
    }

    /* Zeroed before the fill, not just for cleanliness: esp_core_dump_get_summary()
     * only writes the fields it knows about, leaving any struct padding (and
     * any field it doesn't populate) as whatever was already on the stack.
     * See crash_report_dump_id()'s comment for why that matters. */
    esp_core_dump_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    esp_err_t sum_err = esp_core_dump_get_summary(&summary);
    if (sum_err != ESP_OK) {
        ESP_LOGW(TAG, "coredump present but esp_core_dump_get_summary failed: %s -- no crash record captured",
                 esp_err_to_name(sum_err));
        return;
    }

    uint32_t dump_id = crash_report_dump_id(&summary);

    crash_report_record_t existing;
    if (load(&existing) && existing.dump_id == dump_id) {
        /* Same coredump already captured on an earlier boot -- do not
         * recapture/overwrite (this is what keeps the record ONE-per-crash,
         * not rewritten every boot until the operator clears it). */
        ESP_LOGI(TAG, "crash record for this coredump already captured -- not recapturing");
        return;
    }

    hal_reset_reason_t rr = hal_sysinfo_reset_reason();
    const char *rr_name;
    switch (rr) {
    case HAL_RESET_UNKNOWN:    rr_name = "UNKNOWN"; break;
    case HAL_RESET_POWERON:    rr_name = "POWERON"; break;
    case HAL_RESET_EXT:        rr_name = "EXT"; break;
    case HAL_RESET_SW:         rr_name = "SW"; break;
    case HAL_RESET_PANIC:      rr_name = "PANIC"; break;
    case HAL_RESET_INT_WDT:    rr_name = "INT_WDT"; break;
    case HAL_RESET_TASK_WDT:   rr_name = "TASK_WDT"; break;
    case HAL_RESET_WDT:        rr_name = "WDT"; break;
    case HAL_RESET_DEEPSLEEP:  rr_name = "DEEPSLEEP"; break;
    case HAL_RESET_BROWNOUT:   rr_name = "BROWNOUT"; break;
    case HAL_RESET_SDIO:       rr_name = "SDIO"; break;
    default:                   rr_name = "UNKNOWN"; break;
    }

    crash_report_record_t rec;
    memset(&rec, 0, sizeof(rec));
    fill_from_summary(&rec, &summary, rr_name);
    rec.dump_id = dump_id;

    hal_sysinfo_build_info_t build_info;
    hal_sysinfo_get_build_info(&build_info);
    fill_v3_fields(&rec, &s_uptime_beacon, &build_info);

    seal_crc(&rec);

    esp_err_t err = persist(&rec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not persist new crash record: %s", esp_err_to_name(err));
        return;
    }
    /* Freshly captured record is always acknowledged=0 -- recompute from
     * disk anyway (rather than just setting the flag true directly) so this
     * stays the one code path that decides the cache, not two. */
    refresh_unacked_cache();
    ESP_LOGW(TAG, "crash record captured: task='%s' cause=%lu (%s) pc=0x%08lx addr=0x%08lx a0=0x%08lx "
                  "a1(sp)=0x%08lx frames=%u",
             rec.exc_task, (unsigned long)rec.exc_cause, rec.exc_cause_str,
             (unsigned long)rec.exc_pc, (unsigned long)rec.exc_addr, (unsigned long)rec.exc_a0,
             (unsigned long)rec.exc_a1, (unsigned)rec.bt_count);
    if (!crash_report_frame_trustworthy(&rec)) {
        ESP_LOGE(TAG, "crash record's exception frame is NOT self-consistent (pc=0x%08lx%s, backtrace "
                      "corrupted=%u) -- do NOT read exc_addr as a struct field offset or exc_pc as a "
                      "code address from this record; symbolize the coredump instead",
                 (unsigned long)rec.exc_pc,
                 (rec.exc_pc == CRASH_REPORT_PC_OF_ZERO) ? " == esp_cpu_process_stack_pc(0), i.e. the saved PC was 0" : "",
                 (unsigned)rec.bt_corrupted);
    }
}

bool crash_report_get(crash_report_record_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    return load(out);
}

bool crash_report_has_unacknowledged(void)
{
    return s_have_unacked_crash;
}

/* The job run ON the flash worker's own internal-SRAM stack -- see
 * relay_cycles.c's reset_persist_job()/safety_cfg_store.c's nvs_save_
 * store_job() for the identical shape. `arg` points at a small struct owned
 * by the calling task's own stack frame, safe because uart_bridge_ext_run_
 * on_flash_worker() blocks the caller for the whole call.
 *
 * LOW-3 fix (docs/audits/review_crash_gate_followups_62e95bbd_2026-09-15.md):
 * the load() used to happen in the CALLER's task, before dispatch -- so an
 * LCD Acknowledge could load() a record, then a concurrent web /clear could
 * erase it, and the LCD's stale, already-loaded copy would still get written
 * back by its persist job, resurrecting an acknowledged record after the
 * operator had just cleared it. Doing the load INSIDE the job means the
 * whole read-modify-write happens on the flash worker's single serial task,
 * so it can never interleave with a concurrent crash_clear_job() (or another
 * crash_ack_job()) -- whichever job actually runs later simply sees whatever
 * the previous one left on disk. */
typedef struct {
    esp_err_t err;
    bool had_record;
    bool already_acked;
} crash_ack_job_ctx_t;

static void crash_ack_job(void *arg)
{
    crash_ack_job_ctx_t *ctx = (crash_ack_job_ctx_t *)arg;
    crash_report_record_t rec;
    if (!load(&rec)) {
        ctx->had_record = false;
        ctx->err = ESP_OK;
        return;
    }
    ctx->had_record = true;
    if (rec.acknowledged) {
        ctx->already_acked = true;
        ctx->err = ESP_OK;
        return;
    }
    rec.acknowledged = 1u;
    seal_crc(&rec);
    ctx->err = persist(&rec);
}

/* Shared tail for crash_report_acknowledge()/crash_report_acknowledge_
 * timeout() below -- both dispatch crash_ack_job() the same way and only
 * differ in HOW they dispatch (unbounded vs. bounded wait). `submit_err` is
 * the dispatch call's own return value (ESP_OK if the job ran at all,
 * whatever it reports otherwise if it never ran).
 *
 * LOW 1 fix (docs/audits/review_crash_gate_low_fixes_c534a0df_2026-09-15.md):
 * a failed dispatch (submit_err != ESP_OK) now logs its OWN distinct warning
 * before the `!had_record` check below can return silently -- pre-fix, a
 * dispatch failure left ctx.had_record at its false initializer and fell
 * straight into that check with no log at all, indistinguishable from the
 * genuine "nothing to acknowledge" case. Both are diagnosability fixes, not
 * safety ones: the gate stays blocking (s_have_unacked_crash untouched)
 * either way. */
static bool crash_ack_finish(crash_ack_job_ctx_t *ctx, esp_err_t submit_err)
{
    if (submit_err != ESP_OK) {
        ESP_LOGW(TAG, "crash-record acknowledge: could not dispatch to the flash worker: %s -- "
                      "nothing was read or written, try again",
                 esp_err_to_name(submit_err));
        return false;
    }
    if (!ctx->had_record) {
        return false;
    }
    if (ctx->already_acked) {
        s_have_unacked_crash = false; /* defensive -- keep the cache honest even if it had drifted */
        return true; /* already acknowledged -- nothing to do, not a failure */
    }
    if (ctx->err != ESP_OK) {
        ESP_LOGW(TAG, "could not persist crash-record acknowledgement: %s -- it will reappear after a reboot",
                 esp_err_to_name(ctx->err));
        return false;
    }
    s_have_unacked_crash = false;
    return true;
}

bool crash_report_acknowledge(void)
{
    /* RE-ENTRANCY (flash_worker_lint.py's pattern 1, same guard relay_
     * cycles_reset() uses): check whether we are already ON the flash worker
     * before dispatching a second job onto it -- dispatching from inside an
     * already-dispatched job deadlocks the real board. Both known callers
     * (diagnostics_http.c's httpd-task POST handler, and 2026-09-15's LCD
     * diagnostics-page Acknowledge control on lvgl_task) are not expected to
     * already be on the worker, but the check is cheap and this is exactly
     * the class of bug that stays invisible until a caller changes. */
    crash_ack_job_ctx_t ctx = { .err = ESP_FAIL, .had_record = false, .already_acked = false };
    esp_err_t submit_err = ESP_OK;
    if (uart_bridge_ext_is_on_flash_worker()) {
        crash_ack_job(&ctx);
    } else {
        submit_err = uart_bridge_ext_run_on_flash_worker(crash_ack_job, &ctx);
    }
    return crash_ack_finish(&ctx, submit_err);
}

/* Bounded-wait sibling of crash_report_acknowledge() -- for the LCD
 * Acknowledge control (lvgl_task), which must not block the whole UI
 * indefinitely behind some OTHER caller's long flash-worker job (MEDIUM 1,
 * docs/audits/review_crash_gate_low_fixes_c534a0df_2026-09-15.md). Every
 * other caller (diagnostics_http.c's httpd-task POST handler) keeps using
 * the unbounded crash_report_acknowledge() above -- an HTTP request has no
 * equivalent "freeze the whole UI" hazard.
 *
 * `*out_timed_out` (if non-NULL) is set true only when the worker could not
 * be acquired within timeout_ms -- i.e. nothing was read or written at all,
 * as distinct from every other false-returning outcome (no record, or a
 * dispatched write that failed), which are reported the normal way through
 * the return value and crash_ack_finish()'s own logging. The caller (ui_
 * page_diagnostics.c) uses this to show a distinct "busy, try again" result
 * rather than the generic failure text. */
bool crash_report_acknowledge_timeout(uint32_t timeout_ms, bool *out_timed_out)
{
    if (out_timed_out) {
        *out_timed_out = false;
    }
    crash_ack_job_ctx_t ctx = { .err = ESP_FAIL, .had_record = false, .already_acked = false };
    if (uart_bridge_ext_is_on_flash_worker()) {
        crash_ack_job(&ctx);
        return crash_ack_finish(&ctx, ESP_OK);
    }
    esp_err_t submit_err = uart_bridge_ext_run_on_flash_worker_timeout(crash_ack_job, &ctx, timeout_ms);
    if (submit_err == ESP_ERR_TIMEOUT) {
        if (out_timed_out) {
            *out_timed_out = true;
        }
        ESP_LOGW(TAG, "crash-record acknowledge: flash worker busy, timed out after %lu ms -- try again",
                 (unsigned long)timeout_ms);
        return false;
    }
    return crash_ack_finish(&ctx, submit_err);
}

/* Same shape as crash_ack_persist_job() above -- the erase half of
 * crash_report_clear() also touches flash (hal_kv_erase_key()/commit() and
 * hal_sysinfo_coredump_erase()), so it needs the identical flash-worker
 * dispatch. Bundled into one job so both erases happen in a single
 * dispatch/wait round trip rather than two. */
typedef struct {
    hal_status_t kv_err;
    hal_status_t coredump_err;
} crash_clear_job_ctx_t;

static void crash_clear_job(void *arg)
{
    crash_clear_job_ctx_t *ctx = (crash_clear_job_ctx_t *)arg;

    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (kv_err == HAL_OK) {
        kv_err = hal_kv_erase_key(&h, NVS_KEY_CRASH);
        if (kv_err == HAL_OK) {
            kv_err = hal_kv_commit(&h);
        }
        hal_kv_close(&h);
    }
    ctx->kv_err = kv_err;
    ctx->coredump_err = hal_sysinfo_coredump_erase();
}

esp_err_t crash_report_clear(void)
{
    /* Acknowledge first: even if the coredump erase below fails, the operator
     * has already asked to stop being shown this record. crash_report_
     * acknowledge() does its own flash-worker dispatch (including the
     * re-entrancy check), so this call is safe from any task. */
    crash_report_acknowledge();

    crash_clear_job_ctx_t ctx = { .kv_err = HAL_IO, .coredump_err = HAL_IO };
    if (uart_bridge_ext_is_on_flash_worker()) {
        crash_clear_job(&ctx);
    } else {
        esp_err_t submit_err = uart_bridge_ext_run_on_flash_worker(crash_clear_job, &ctx);
        if (submit_err != ESP_OK) {
            ctx.kv_err = HAL_IO;
            ctx.coredump_err = HAL_IO;
        }
    }

    if (ctx.kv_err != HAL_OK && ctx.kv_err != HAL_NOT_FOUND) {
        ESP_LOGW(TAG, "could not erase crash record from NVS: %s", hal_status_to_name(ctx.kv_err));
    }

    /* LOW fix (2026-09-15 audit): re-derive s_have_unacked_crash from disk
     * rather than trusting crash_report_acknowledge()'s own outcome above --
     * if THAT write failed but the NVS erase just above still succeeded
     * (ctx.kv_err is HAL_OK/HAL_NOT_FOUND, i.e. the record is genuinely
     * gone), the old code left s_have_unacked_crash stuck true until the
     * next reboot even though load() now correctly reports "no record".
     * refresh_unacked_cache() is the one function that is allowed to read
     * NVS to decide the flag (see its own doc comment) and a plain read is
     * safe to call from any task (no cache-disabling flash operation),
     * unlike the writes above. */
    refresh_unacked_cache();

    if (ctx.coredump_err != HAL_OK) {
        esp_err_t erase_err = hal_status_to_esp_err(ctx.coredump_err);
        ESP_LOGW(TAG, "hal_sysinfo_coredump_erase failed: %s", esp_err_to_name(erase_err));
        return erase_err;
    }
    ESP_LOGI(TAG, "crash record and coredump image cleared");
    return ESP_OK;
}
