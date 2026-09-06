#include "crash_report.h"

#include <string.h>

#include "esp_crc.h"
#include "esp_core_dump.h" /* esp_core_dump_summary_t/esp_core_dump_get_summary() -- full-summary parsing stays above hal_sysinfo, see that header's top comment */
#include "esp_log.h"

#include "hal_kv.h"
#include "hal_sysinfo.h" /* hal_sysinfo_coredump_present()/_erase(), hal_sysinfo_reset_reason() */
#include "hal_esp_common.h" /* hal_status_to_esp_err() -- preserve the specific esp_err_t from
                              * hal_sysinfo_coredump_erase()/hal_kv failures rather than collapsing
                              * to ESP_FAIL */

static const char *TAG = "crash_report";

/* Same namespace as run_state.c/ota_record.c/relay_cycles.c, own key -- see
 * run_state.c's header comment for why a shared blob is the wrong move here
 * too: a corrupt/rejected crash record must never be able to take another
 * module's breadcrumb down with it, and vice versa. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_CRASH "crash_rpt"

/* TODO.md 8.1's shared partition, split out of the default NVS partition,
 * managed independently by each module that uses it -- see run_state.c. */
#define KILN_NVS_PARTITION "kiln_nvs"

/* Pins the on-disk layout, same reasoning and same portability trick as
 * ota_record.c's ota_record_t_size_check: this file is ALSO compiled
 * directly into App/test/build_host_tests.ps1's MSVC host-test binary (via
 * test_crash_report.c's #include of this file), and that cl.exe invocation
 * compiles in a C mode old enough that a plain _Static_assert is a hard
 * syntax error. The classic negative-array-size trick is portable C89/C99/
 * C11 alike and checks exactly the same thing. Update this literal whenever
 * crash_report_record_t's layout changes, alongside bumping
 * CRASH_REPORT_RECORD_VERSION. */
typedef char crash_report_record_t_size_check[(sizeof(crash_report_record_t) == 152) ? 1 : -1];

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

    if (!record_valid(&rec)) {
        ESP_LOGW(TAG, "stored crash record failed version/CRC check -- treating as no record");
        return false;
    }
    *out = rec;
    return true;
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
    seal_crc(&rec);

    esp_err_t err = persist(&rec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not persist new crash record: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "crash record captured: task='%s' cause=%lu (%s) pc=0x%08lx addr=0x%08lx frames=%u",
             rec.exc_task, (unsigned long)rec.exc_cause, rec.exc_cause_str,
             (unsigned long)rec.exc_pc, (unsigned long)rec.exc_addr, (unsigned)rec.bt_count);
}

bool crash_report_get(crash_report_record_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    return load(out);
}

bool crash_report_acknowledge(void)
{
    crash_report_record_t rec;
    if (!load(&rec)) {
        return false;
    }
    if (rec.acknowledged) {
        return true; /* already acknowledged -- nothing to do, not a failure */
    }
    rec.acknowledged = 1u;
    seal_crc(&rec);
    esp_err_t err = persist(&rec);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not persist crash-record acknowledgement: %s -- it will reappear after a reboot",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

esp_err_t crash_report_clear(void)
{
    /* Acknowledge first: even if the coredump erase below fails, the operator
     * has already asked to stop being shown this record. */
    crash_report_acknowledge();

    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (kv_err == HAL_OK) {
        kv_err = hal_kv_erase_key(&h, NVS_KEY_CRASH);
        if (kv_err == HAL_OK) {
            kv_err = hal_kv_commit(&h);
        }
        hal_kv_close(&h);
    }
    if (kv_err != HAL_OK && kv_err != HAL_NOT_FOUND) {
        ESP_LOGW(TAG, "could not erase crash record from NVS: %s", hal_status_to_name(kv_err));
    }

    hal_status_t erase_status = hal_sysinfo_coredump_erase();
    if (erase_status != HAL_OK) {
        esp_err_t erase_err = hal_status_to_esp_err(erase_status);
        ESP_LOGW(TAG, "hal_sysinfo_coredump_erase failed: %s", esp_err_to_name(erase_err));
        return erase_err;
    }
    ESP_LOGI(TAG, "crash record and coredump image cleared");
    return ESP_OK;
}
