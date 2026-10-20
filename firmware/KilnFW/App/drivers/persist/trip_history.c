#include "trip_history.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "kiln/kiln_crc32.h"

#include "flash_hop.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

/* CRC over the blob with its own crc32 field read as zero. */
#define BLOB_CRC(bp) \
    kiln_crc32_le_zeroed((const uint8_t *)(bp), sizeof(trip_history_blob_t), offsetof(trip_history_blob_t, crc32), sizeof((bp)->crc32))

static const char *TAG = "trip_history";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_TRIP_HIST "trip_hist"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_TRIP_HIST);
#define KILN_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);

typedef char trip_history_entry_size_check[(sizeof(trip_history_entry_t) == 36) ? 1 : -1];
typedef char trip_history_blob_size_check[(sizeof(trip_history_blob_t) == 12 + 36 * TRIP_HISTORY_MAX) ? 1 : -1];

bool trip_history_blob_valid(const trip_history_blob_t *b)
{
    if (b->version != TRIP_HISTORY_VERSION || b->count > TRIP_HISTORY_MAX || b->head >= TRIP_HISTORY_MAX ||
        b->next_seq == 0u) {
        return false;
    }
    return BLOB_CRC(b) == b->crc32;
}

void trip_history_blob_init(trip_history_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->version = TRIP_HISTORY_VERSION;
    b->next_seq = 1u;
}

uint16_t trip_history_mask_for_reason(uint8_t reason)
{
    return (reason >= 1u && reason <= 16u) ? (uint16_t)(1u << (reason - 1u)) : 0u;
}

bool trip_history_blob_has(const trip_history_blob_t *b, const trip_history_entry_t *e)
{
    for (unsigned i = 0; i < b->count; i++) {
        const trip_history_entry_t *x = &b->entries[i];
        if (x->pico_trip_seq == e->pico_trip_seq && x->reason == e->reason &&
            x->pico_uptime_ms == e->pico_uptime_ms) {
            return true;
        }
    }
    return false;
}

bool trip_history_blob_push(trip_history_blob_t *b, const trip_history_entry_t *e)
{
    if (trip_history_blob_has(b, e)) {
        return false;
    }
    trip_history_entry_t ent = *e;
    ent.seq = b->next_seq;
    ent.mask = trip_history_mask_for_reason(ent.reason);
    memset(ent.pad, 0, sizeof(ent.pad));
    b->entries[b->head] = ent;
    b->head = (uint8_t)((b->head + 1u) % TRIP_HISTORY_MAX);
    if (b->count < TRIP_HISTORY_MAX) {
        b->count++;
    }
    b->next_seq = (b->next_seq == 0xFFFFFFFFu) ? 1u : b->next_seq + 1u;
    return true;
}

bool trip_history_blob_get(const trip_history_blob_t *b, unsigned i, trip_history_entry_t *out)
{
    if (i >= b->count) {
        return false;
    }
    unsigned idx = ((unsigned)b->head + TRIP_HISTORY_MAX - 1u - i) % TRIP_HISTORY_MAX;
    *out = b->entries[idx];
    return true;
}

bool trip_history_load(trip_history_blob_t *out)
{
    trip_history_blob_init(out);
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    size_t len = sizeof(*out);
    hal_status_t err = hal_kv_get_blob(&h, NVS_KEY_TRIP_HIST, out, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(*out) || !trip_history_blob_valid(out)) {
        if (err == HAL_OK) {
            ESP_LOGW(TAG, "stored trip history failed size/CRC check -- starting a new ring");
        }
        trip_history_blob_init(out);
        return false;
    }
    return true;
}

/* ---- pending queue: single producer (link RX task), single consumer (telemetry_log task) ---- */
static trip_history_entry_t s_pending[TRIP_HISTORY_PENDING_MAX];
static volatile uint8_t s_pend_head; /* written by the producer */
static volatile uint8_t s_pend_tail; /* written by the consumer */
static volatile unsigned s_dropped;

/* TH-01: wire floats are printed with %.1f into a fixed JSON buffer; NaN/inf/huge values would
 * overflow a row. Non-finite -> 0, others clamped to a width-bounded range. */
static float th_clamp(float v)
{
    if (!(v == v) || v > 1.0e5f || v < -1.0e5f) { /* NaN fails v == v; huge/inf out of range */
        return (v > 1.0e5f) ? 1.0e5f : ((v < -1.0e5f) ? -1.0e5f : 0.0f);
    }
    return v;
}

bool trip_history_note(const trip_history_entry_t *e_in)
{
    trip_history_entry_t e_c = *e_in;
    e_c.safety_tc_c = th_clamp(e_c.safety_tc_c);
    e_c.threshold = th_clamp(e_c.threshold);
    const trip_history_entry_t *e = &e_c;
    uint8_t head = s_pend_head;
    uint8_t next = (uint8_t)((head + 1u) % TRIP_HISTORY_PENDING_MAX);
    if (next == s_pend_tail) {
        s_dropped++;
        return false;
    }
    s_pending[head] = *e;
    s_pend_head = next; /* publish after the slot is written */
    return true;
}

unsigned trip_history_dropped(void)
{
    return s_dropped;
}

typedef struct {
    trip_history_entry_t ents[TRIP_HISTORY_PENDING_MAX];
    unsigned n;
    unsigned recorded;
    bool ok; /* TH-02: false when the partition/open/write failed => queue entries must be kept */
} flush_job_t;

/* Runs on a flash-safe (internal-stack) context via flash_hop_run(). */
static void flush_job(void *arg)
{
    flush_job_t *j = (flush_job_t *)arg;
    j->recorded = 0;
    j->ok = false;
    if (hal_kv_init_partition(KILN_NVS_PARTITION) != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition init failed -- trip not recorded");
        return;
    }
    trip_history_blob_t b; /* 588 B: bx_flash_worker has a 10 KB internal stack */
    trip_history_load(&b);
    for (unsigned i = 0; i < j->n; i++) {
        if (trip_history_blob_push(&b, &j->ents[i])) {
            j->recorded++;
        }
    }
    if (j->recorded == 0u) {
        j->ok = true;
        return; /* all duplicates: no flash write */
    }
    b.crc32 = BLOB_CRC(&b);
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "hal_kv_open failed: %s -- trip not recorded", hal_status_to_name(err));
        j->recorded = 0;
        return;
    }
    err = hal_kv_set_blob(&h, NVS_KEY_TRIP_HIST, &b, sizeof(b));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "trip history persist failed: %s", hal_status_to_name(err));
        j->recorded = 0;
        return;
    }
    j->ok = true;
}

unsigned trip_history_flush(void)
{
    if (s_pend_tail == s_pend_head) {
        return 0;
    }
    flush_job_t j;
    memset(&j, 0, sizeof(j));
    uint8_t tail = s_pend_tail;
    uint8_t head = s_pend_head;
    while (tail != head && j.n < TRIP_HISTORY_PENDING_MAX) {
        j.ents[j.n++] = s_pending[tail];
        tail = (uint8_t)((tail + 1u) % TRIP_HISTORY_PENDING_MAX);
    }
    esp_err_t r = flash_hop_run(flush_job, &j);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "flash worker unavailable -- keeping %u pending trip(s)", j.n);
        return 0; /* queue left intact; retried on the next flush */
    }
    if (!j.ok) {
        ESP_LOGE(TAG, "trip history write failed -- keeping %u pending trip(s) for retry", j.n);
        return 0; /* TH-02: tail not advanced; dedupe makes the retry safe */
    }
    s_pend_tail = tail;
    return j.recorded;
}
