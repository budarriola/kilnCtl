#include "boot_history.h"
#include "esp_attr.h"
#include "persist_scratch.h"
#include <stdlib.h>

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "kiln/kiln_crc32.h"

#include "hal_esp_common.h"
#include "hal_kv.h"
#include "hal_sysinfo.h"
#include "nvs_key_check.h"

/* CRC over the blob with its own crc32 field read as zero. */
#define BLOB_CRC(bp) \
    kiln_crc32_le_zeroed((const uint8_t *)(bp), sizeof(boot_history_blob_t), offsetof(boot_history_blob_t, crc32), sizeof((bp)->crc32))

static const char *TAG = "boot_history";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_BOOT_HIST "boot_hist"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_BOOT_HIST);
#define KILN_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);

typedef char boot_history_entry_size_check[(sizeof(boot_history_entry_t) == 36) ? 1 : -1];
typedef char boot_history_blob_size_check[(sizeof(boot_history_blob_t) == 12 + 36 * BOOT_HISTORY_MAX) ? 1 : -1];

bool boot_history_blob_valid(const boot_history_blob_t *b)
{
    if (b->version != BOOT_HISTORY_VERSION || b->count > BOOT_HISTORY_MAX ||
        b->head >= BOOT_HISTORY_MAX || b->next_seq == 0u) {
        return false;
    }
    return BLOB_CRC(b) == b->crc32;
}

void boot_history_blob_init(boot_history_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->version = BOOT_HISTORY_VERSION;
    b->next_seq = 1u;
}

void boot_history_blob_push(boot_history_blob_t *b, const boot_history_entry_t *e)
{
    boot_history_entry_t ent = *e;
    ent.seq = b->next_seq;
    ent.fw[BOOT_HISTORY_FW_MAX - 1] = '\0';
    b->entries[b->head] = ent;
    b->head = (uint8_t)((b->head + 1u) % BOOT_HISTORY_MAX);
    if (b->count < BOOT_HISTORY_MAX) {
        b->count++;
    }
    b->next_seq = (b->next_seq == 0xFFFFFFFFu) ? 1u : b->next_seq + 1u;
}

bool boot_history_blob_get(const boot_history_blob_t *b, unsigned i, boot_history_entry_t *out)
{
    if (i >= b->count) {
        return false;
    }
    unsigned idx = ((unsigned)b->head + BOOT_HISTORY_MAX - 1u - i) % BOOT_HISTORY_MAX;
    *out = b->entries[idx];
    out->fw[BOOT_HISTORY_FW_MAX - 1] = '\0';
    return true;
}

void boot_history_short_hash(const char *version, char out[BOOT_HISTORY_FW_MAX])
{
    out[0] = '\0';
    if (!version) {
        return;
    }
    const char *src = version;
    const char *g = strstr(version, "-g");
    if (g && isxdigit((unsigned char)g[2])) {
        src = g + 2;
    }
    size_t n = 0;
    while (n < BOOT_HISTORY_FW_MAX - 1 && src[n] != '\0') {
        unsigned char c = (unsigned char)src[n];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            break;
        }
        out[n] = (char)c;
        n++;
    }
    out[n] = '\0';
}

const char *boot_history_reason_name(uint8_t r)
{
    /* Order mirrors hal_reset_reason_t (hal_sysinfo.h). */
    static const char *const names[] = {"UNKNOWN", "POWERON",   "EXT",     "SW",  "PANIC", "INT_WDT", "TASK_WDT",
                                        "WDT",     "DEEPSLEEP", "BROWNOUT", "SDIO", "USB",   "JTAG",    "EFUSE",
                                        "PWR_GLITCH", "CPU_LOCKUP"};
    /* BH-01: the table must cover the whole hal_reset_reason_t enum. */
    typedef char names_cover_enum[(sizeof(names) / sizeof(names[0]) == (size_t)HAL_RESET_CPU_LOCKUP + 1u) ? 1 : -1];
    (void)sizeof(names_cover_enum);
    return (r < sizeof(names) / sizeof(names[0])) ? names[r] : "UNKNOWN";
}

bool boot_history_load(boot_history_blob_t *out)
{
    boot_history_blob_init(out);
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    size_t len = sizeof(*out);
    hal_status_t err = hal_kv_get_blob(&h, NVS_KEY_BOOT_HIST, out, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(*out) || !boot_history_blob_valid(out)) {
        if (err == HAL_OK) {
            ESP_LOGW(TAG, "stored boot history failed size/CRC check -- starting a new ring");
        }
        boot_history_blob_init(out);
        return false;
    }
    return true;
}

/* FW-01: unpersisted coalesced-boot count. RTC_NOINIT survives panic/WDT/software resets (the
 * boot-loop cases) and is invalid after power-on; then the count restarts at 1. */
#define BH_PENDING_MAGIC 0xB007C0A1u
typedef struct {
    uint32_t magic;
    uint32_t seq;     /* entry the pending repeats belong to */
    uint32_t pending; /* repeats not yet in the persisted blob */
} bh_pending_t;
RTC_NOINIT_ATTR static bh_pending_t s_pending;
static bool s_coalesced; /* this boot was folded into the last entry (FW-02: no wall-time backfill) */

EXT_RAM_BSS_ATTR static boot_history_entry_t s_recent[BOOT_HISTORY_RECENT];
static unsigned s_recent_n;

unsigned boot_history_recent(boot_history_entry_t out[BOOT_HISTORY_RECENT])
{
    memcpy(out, s_recent, sizeof(s_recent));
    return s_recent_n;
}

uint32_t boot_history_latest_seq(void)
{
    return s_recent_n > 0u ? s_recent[0].seq : 0u;
}

/* FV-01: coalesced boots share one seq, so per-boot keys also need the entry's repeat count, which
 * differs for every consecutive boot (persisted repeat + RTC pending, folded into s_recent). Low 8 bits
 * are enough: consecutive boots differ by 1. */
uint8_t boot_history_latest_rep(void)
{
    return s_recent_n > 0u ? (uint8_t)(s_recent[0].repeat & 0xFFu) : 0u;
}

esp_err_t boot_history_record_boot(uint8_t hal_reset_reason, bool prev_uptime_known,
                                   uint32_t prev_uptime_s, uint32_t dump_id, bool has_crash,
                                   const char *fw_version, uint32_t wall_time)
{
    hal_status_t perr = hal_kv_init_partition(KILN_NVS_PARTITION);
    if (perr != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition init failed: %s -- boot not recorded", hal_status_to_name(perr));
        return hal_status_to_esp_err(perr);
    }
    boot_history_blob_t b;
    bool had_blob = boot_history_load(&b);

    boot_history_entry_t e;
    memset(&e, 0, sizeof(e));
    e.reset_reason = hal_reset_reason;
    e.prev_uptime_known = prev_uptime_known ? 1u : 0u;
    e.prev_uptime_s = prev_uptime_known ? prev_uptime_s : 0u;
    e.dump_id = has_crash ? dump_id : 0u;
    e.has_crash = has_crash ? 1u : 0u;
    e.wall_time = wall_time;
    boot_history_short_hash(fw_version, e.fw);

    unsigned lidx = ((unsigned)b.head + BOOT_HISTORY_MAX - 1u) % BOOT_HISTORY_MAX;
    bool pend_ok = s_pending.magic == BH_PENDING_MAGIC && b.count > 0u && s_pending.seq == b.entries[lidx].seq;
    uint32_t pending = pend_ok ? s_pending.pending : 0u;
    s_coalesced = false;
    if (had_blob && b.count > 0u && b.entries[lidx].reset_reason == e.reset_reason &&
        b.entries[lidx].has_crash == e.has_crash && /* FV-03: a crash-record boot always gets its own entry */
        strcmp(b.entries[lidx].fw, e.fw) == 0) {
        /* FW-01: identical boot -- fold into the last entry, persist only every Nth repeat. */
        pending++;
        uint32_t total = (uint32_t)b.entries[lidx].repeat + pending;
        b.entries[lidx].repeat = (uint16_t)(total > 0xFFFFu ? 0xFFFFu : total);
        s_coalesced = true;
        s_pending.magic = BH_PENDING_MAGIC;
        s_pending.seq = b.entries[lidx].seq;
        s_pending.pending = pending;
        s_recent_n = 0;
        for (unsigned i = 0; i < BOOT_HISTORY_RECENT && boot_history_blob_get(&b, i, &s_recent[i]); i++) {
            s_recent_n++;
        }
        if (pending < BOOT_HISTORY_FLUSH_EVERY) {
            return ESP_OK;
        }
        s_pending.pending = 0u;
        b.crc32 = BLOB_CRC(&b);
        goto persist;
    }
    if (pending > 0u) { /* fold the unpersisted repeats into the write this boot needs anyway */
        uint32_t total = (uint32_t)b.entries[lidx].repeat + pending;
        b.entries[lidx].repeat = (uint16_t)(total > 0xFFFFu ? 0xFFFFu : total);
    }
    s_pending.magic = 0u;
    boot_history_blob_push(&b, &e);
    b.crc32 = BLOB_CRC(&b);
    s_recent_n = 0;
    for (unsigned i = 0; i < BOOT_HISTORY_RECENT && boot_history_blob_get(&b, i, &s_recent[i]); i++) {
        s_recent_n++;
    }
persist:;
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "hal_kv_open failed: %s -- boot not recorded", hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_BOOT_HIST, &b, sizeof(b));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "boot history persist failed: %s", hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    return ESP_OK;
}

static esp_err_t backfill_with_blob(boot_history_blob_t *b, uint32_t wall_time)
{
    if (!boot_history_load(b)) {
        return ESP_ERR_INVALID_STATE;
    }
    unsigned idx = ((unsigned)b->head + BOOT_HISTORY_MAX - 1u) % BOOT_HISTORY_MAX;
    if (b->count == 0u || b->entries[idx].seq != s_recent[0].seq || b->entries[idx].wall_time != 0u) {
        return ESP_ERR_INVALID_STATE;
    }
    b->entries[idx].wall_time = wall_time;
    b->crc32 = BLOB_CRC(b);
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_BOOT_HIST, b, sizeof(*b));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    s_recent[0].wall_time = wall_time;
    return ESP_OK;
}

/* The blob is on the internal heap, not the stack: this runs on the telemetry_log task
 * (check_all_task_stack_budgets). */
esp_err_t boot_history_backfill_wall_time(uint32_t wall_time)
{
    if (s_recent_n == 0u || wall_time == 0u || s_coalesced) { /* FW-02: coalesced boot: no extra write */
        return ESP_ERR_INVALID_STATE;
    }
    boot_history_blob_t *b = persist_scratch_alloc(sizeof(*b));
    if (!b) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t r = backfill_with_blob(b, wall_time);
    free(b);
    return r;
}
