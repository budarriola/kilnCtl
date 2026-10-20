#include "boot_history.h"

#include <ctype.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_log.h"

#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

static const char *TAG = "boot_history";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_BOOT_HIST "boot_hist"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_BOOT_HIST);
#define KILN_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);

typedef char boot_history_entry_size_check[(sizeof(boot_history_entry_t) == 32) ? 1 : -1];
typedef char boot_history_blob_size_check[(sizeof(boot_history_blob_t) == 12 + 32 * BOOT_HISTORY_MAX) ? 1 : -1];

static uint32_t blob_crc(const boot_history_blob_t *b)
{
    boot_history_blob_t tmp = *b;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

bool boot_history_blob_valid(const boot_history_blob_t *b)
{
    if (b->version != BOOT_HISTORY_VERSION || b->count > BOOT_HISTORY_MAX ||
        b->head >= BOOT_HISTORY_MAX || b->next_seq == 0u) {
        return false;
    }
    return blob_crc(b) == b->crc32;
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
                                        "WDT",     "DEEPSLEEP", "BROWNOUT", "SDIO", "USB",   "JTAG",    "EFUSE"};
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

static boot_history_entry_t s_recent[BOOT_HISTORY_RECENT];
static unsigned s_recent_n;

unsigned boot_history_recent(boot_history_entry_t out[BOOT_HISTORY_RECENT])
{
    memcpy(out, s_recent, sizeof(s_recent));
    return s_recent_n;
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
    boot_history_load(&b);

    boot_history_entry_t e;
    memset(&e, 0, sizeof(e));
    e.reset_reason = hal_reset_reason;
    e.prev_uptime_known = prev_uptime_known ? 1u : 0u;
    e.prev_uptime_s = prev_uptime_known ? prev_uptime_s : 0u;
    e.dump_id = has_crash ? dump_id : 0u;
    e.has_crash = has_crash ? 1u : 0u;
    e.wall_time = wall_time;
    boot_history_short_hash(fw_version, e.fw);
    boot_history_blob_push(&b, &e);
    b.crc32 = blob_crc(&b);
    s_recent_n = 0;
    for (unsigned i = 0; i < BOOT_HISTORY_RECENT && boot_history_blob_get(&b, i, &s_recent[i]); i++) {
        s_recent_n++;
    }

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
