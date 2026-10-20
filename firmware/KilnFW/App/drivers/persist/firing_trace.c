// See firing_trace.h for the design, size and power-cut loss statement.
#include "firing_trace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kilnlink/kilnlink_frame.h" /* kilnlink_crc16_ccitt_false() -- CommonFW's CRC, no local CRC code */

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define FT_LOGW(...) ESP_LOGW("firing_trace", __VA_ARGS__)
#else
#define FT_LOGW(...) ((void)0)
#endif

#define FT_VERSION 1u

static firing_trace_io_t s_io;
static uint8_t *s_buf; /* FT_FILE_MAX bytes: 24 B header area + samples */
static firing_trace_entry_t s_idx[FT_SLOTS];
static uint32_t s_next_seq = 1;

static volatile bool s_active;
static volatile bool s_closed; /* end() was called for the active firing */
static volatile bool s_job_busy;
static volatile bool s_flush_due;
static uint32_t s_run_token;
static bool s_have_token;
static uint32_t s_ended_token;
static bool s_have_ended;
static uint32_t s_seq;
static uint32_t s_start_epoch;
static volatile uint16_t s_n;
static volatile uint16_t s_flushed;
static volatile bool s_closed_flushed;
static bool s_truncated;
static uint32_t s_last_uptime;
static bool s_have_last;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static void slot_path(uint32_t seq, char *out, size_t cap)
{
    snprintf(out, cap, "trace/ft%u.bin", (unsigned)(seq % FT_SLOTS));
}

/* Header layout (24 B): magic "FTR1" ver u8 zones u8 rec_size u16 seq u32 start_epoch u32 n u16 flags u16
 * crc_data u16 crc_hdr u16 (crc_hdr covers bytes 0..21). */
static void build_header(uint8_t *h, uint32_t seq, uint32_t start_epoch, uint16_t n, uint16_t flags,
                         const uint8_t *samples)
{
    h[0] = 'F';
    h[1] = 'T';
    h[2] = 'R';
    h[3] = '1';
    h[4] = FT_VERSION;
    h[5] = FT_ZONES;
    put16(h + 6, FT_REC_SIZE);
    put32(h + 8, seq);
    put32(h + 12, start_epoch);
    put16(h + 16, n);
    put16(h + 18, flags);
    put16(h + 20, n ? kilnlink_crc16_ccitt_false(samples, (size_t)n * FT_REC_SIZE) : 0);
    put16(h + 22, kilnlink_crc16_ccitt_false(h, 22));
}

/* True when b[0..len) is a well-formed file; fills *e. */
static bool parse_file(const uint8_t *b, size_t len, firing_trace_entry_t *e)
{
    if (len < FT_HDR_SIZE) return false;
    if (b[0] != 'F' || b[1] != 'T' || b[2] != 'R' || b[3] != '1') return false;
    if (b[4] != FT_VERSION || b[5] != FT_ZONES || get16(b + 6) != FT_REC_SIZE) return false;
    if (get16(b + 22) != kilnlink_crc16_ccitt_false(b, 22)) return false;
    uint16_t n = get16(b + 16);
    if (n > FT_MAX_SAMPLES || len != FT_HDR_SIZE + (size_t)n * FT_REC_SIZE) return false;
    uint16_t crc = n ? kilnlink_crc16_ccitt_false(b + FT_HDR_SIZE, (size_t)n * FT_REC_SIZE) : 0;
    if (get16(b + 20) != crc) return false;
    e->seq = get32(b + 8);
    e->start_epoch = get32(b + 12);
    e->n = n;
    e->flags = get16(b + 18);
    return e->seq != 0;
}

void firing_trace_reset_for_test(void)
{
    free(s_buf);
    s_buf = NULL;
    memset(&s_io, 0, sizeof(s_io));
    memset(s_idx, 0, sizeof(s_idx));
    s_next_seq = 1;
    s_active = false;
    s_closed = false;
    s_job_busy = false;
    s_flush_due = false;
    s_have_token = false;
    s_have_ended = false;
    s_truncated = false;
    s_have_last = false;
    s_n = 0;
    s_flushed = 0;
    s_closed_flushed = false;
}

void firing_trace_init(const firing_trace_io_t *io)
{
    if (io) s_io = *io;
    if (!s_buf) {
        s_buf = s_io.alloc ? (uint8_t *)s_io.alloc(FT_FILE_MAX) : NULL; /* no alloc seam => tracing disabled */
    }
    memset(s_idx, 0, sizeof(s_idx));
    s_next_seq = 1;
    if (!s_buf || !s_io.read) return;
    for (uint32_t slot = 0; slot < FT_SLOTS; slot++) {
        char path[24];
        slot_path(slot, path, sizeof(path));
        size_t len = 0;
        firing_trace_entry_t e;
        if (s_io.read(path, s_buf, FT_FILE_MAX, &len) != ESP_OK || !parse_file(s_buf, len, &e)) continue;
        if ((e.seq % FT_SLOTS) != slot) continue;
        s_idx[slot] = e;
        if (e.seq >= s_next_seq) s_next_seq = e.seq + 1;
    }
}

static int16_t temp_to_dc(float t)
{
    if (isnan(t)) return FT_TEMP_INVALID;
    float d = t * 10.0f;
    if (d > 32000.0f) d = 32000.0f;
    if (d < -32000.0f) d = -32000.0f;
    return (int16_t)lroundf(d);
}

static uint8_t duty_to_u8(float d)
{
    if (isnan(d)) return FT_DUTY_INACTIVE;
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    return (uint8_t)lroundf(d * 200.0f);
}

bool firing_trace_note(uint32_t run_token, uint32_t uptime_s, uint32_t epoch, float setpoint_c,
                       const float temp_c[FT_ZONES], const float duty[FT_ZONES], uint8_t segment)
{
    if (!s_buf || !s_io.write) return false;
    if (s_have_ended && s_ended_token == run_token) return false; /* firing already closed */
    if (!s_active || !s_have_token || s_run_token != run_token) {
        if (s_job_busy) return false; /* previous flush still reading the buffer: retry next sample */
        s_run_token = run_token;
        s_have_token = true;
        s_active = true;
        s_closed = false;
        s_closed_flushed = false;
        s_have_ended = false;
        s_truncated = false;
        s_have_last = false;
        s_seq = s_next_seq++;
        s_start_epoch = epoch;
        s_n = 0;
        s_flushed = 0;
        s_flush_due = false;
    }
    if (s_have_last && (uint32_t)(uptime_s - s_last_uptime) < FT_SAMPLE_PERIOD_S) return false;
    if (s_n >= FT_MAX_SAMPLES) {
        s_truncated = true;
        return false;
    }
    if (s_start_epoch == 0 && epoch != 0) s_start_epoch = epoch;
    uint8_t *r = s_buf + FT_HDR_SIZE + (size_t)s_n * FT_REC_SIZE;
    put32(r, uptime_s);
    put32(r + 4, epoch);
    put16(r + 8, (uint16_t)temp_to_dc(setpoint_c));
    for (uint8_t z = 0; z < FT_ZONES; z++) {
        put16(r + 10 + 2 * z, (uint16_t)temp_to_dc(temp_c[z]));
        r[16 + z] = duty_to_u8(duty[z]);
    }
    r[19] = segment;
    s_last_uptime = uptime_s;
    s_have_last = true;
    uint16_t n = (uint16_t)(s_n + 1);
    s_n = n; /* publish after the record is complete */
    if (n == 1 || (uint16_t)(n - s_flushed) >= FT_FLUSH_EVERY) s_flush_due = true;
    return true;
}

void firing_trace_flush_job(void *arg)
{
    (void)arg;
    if (s_buf && s_io.write && s_active) {
        uint16_t n = s_n;
        bool closed = s_closed;
        if (n != 0 && (n != s_flushed || closed != s_closed_flushed)) {
            uint16_t flags = (uint16_t)((s_truncated ? FT_FLAG_TRUNCATED : 0) | (closed ? FT_FLAG_CLOSED : 0));
            build_header(s_buf, s_seq, s_start_epoch, n, flags, s_buf + FT_HDR_SIZE);
            char path[24];
            slot_path(s_seq, path, sizeof(path));
            esp_err_t err = s_io.write(path, s_buf, FT_HDR_SIZE + (size_t)n * FT_REC_SIZE);
            if (err == ESP_OK) {
                s_flushed = n;
                s_closed_flushed = closed;
                firing_trace_entry_t *e = &s_idx[s_seq % FT_SLOTS];
                e->seq = s_seq;
                e->start_epoch = s_start_epoch;
                e->n = n;
                e->flags = flags;
            } else {
                FT_LOGW("trace flush failed (%d); %u samples stay in RAM", (int)err, (unsigned)n);
            }
        }
    }
    s_job_busy = false;
}

void firing_trace_service(void)
{
    if (!s_flush_due || s_job_busy || !s_io.post) return;
    s_flush_due = false;
    s_job_busy = true;
    if (s_io.post(firing_trace_flush_job) != ESP_OK) {
        s_job_busy = false;
        s_flush_due = true; /* worker busy: retry on a later tick */
    }
}

void firing_trace_end(void)
{
    if (!s_active) return;
    s_closed = true;
    s_flush_due = false;
    s_job_busy = true;
    esp_err_t err = s_io.run ? s_io.run(firing_trace_flush_job, NULL) : ESP_FAIL;
    if (err != ESP_OK) {
        s_job_busy = false;
        FT_LOGW("final trace flush did not run (%d)", (int)err);
    }
    s_ended_token = s_run_token;
    s_have_ended = true;
    s_active = false;
}

size_t firing_trace_list(firing_trace_entry_t out[FT_SLOTS])
{
    size_t k = 0;
    for (uint32_t i = 0; i < FT_SLOTS; i++) {
        if (s_idx[i].seq != 0) out[k++] = s_idx[i];
    }
    for (size_t a = 0; a < k; a++) { /* newest (highest seq) first */
        for (size_t b = a + 1; b < k; b++) {
            if (out[b].seq > out[a].seq) {
                firing_trace_entry_t t = out[a];
                out[a] = out[b];
                out[b] = t;
            }
        }
    }
    return k;
}

esp_err_t firing_trace_open(uint32_t id, uint8_t *scratch, size_t cap, firing_trace_view_t *out)
{
    if (!scratch || !out || cap < FT_FILE_MAX || id == 0) return ESP_ERR_INVALID_ARG;
    if (!s_io.read) return ESP_ERR_INVALID_STATE;
    char path[24];
    slot_path(id, path, sizeof(path));
    size_t len = 0;
    esp_err_t err = s_io.read(path, scratch, cap, &len);
    if (err != ESP_OK) return err;
    if (!parse_file(scratch, len, &out->entry)) return ESP_ERR_INVALID_CRC;
    if (out->entry.seq != id) return ESP_ERR_NOT_FOUND;
    out->samples = scratch + FT_HDR_SIZE;
    return ESP_OK;
}

size_t firing_trace_csv_header(char *out, size_t cap)
{
    int n = snprintf(out, cap,
                     "uptime_s,utc_epoch,setpoint_c,z0_temp_c,z1_temp_c,z2_temp_c,"
                     "z0_duty_pct,z1_duty_pct,z2_duty_pct,segment\n");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static size_t put_dc(char *o, size_t cap, int16_t dc)
{
    if (dc == FT_TEMP_INVALID) return 0;
    int v = dc;
    int a = v < 0 ? -v : v;
    int n = snprintf(o, cap, "%s%d.%d", v < 0 ? "-" : "", a / 10, a % 10);
    return n > 0 ? (size_t)n : 0;
}

size_t firing_trace_csv_row(const firing_trace_view_t *v, uint16_t idx, char *out, size_t cap)
{
    if (!v || idx >= v->entry.n || cap < FT_CSV_LINE_CAP) return 0;
    const uint8_t *r = v->samples + (size_t)idx * FT_REC_SIZE;
    size_t o = (size_t)snprintf(out, cap, "%lu,%lu,", (unsigned long)get32(r), (unsigned long)get32(r + 4));
    o += put_dc(out + o, cap - o, (int16_t)get16(r + 8));
    for (uint8_t z = 0; z < FT_ZONES; z++) {
        out[o++] = ',';
        o += put_dc(out + o, cap - o, (int16_t)get16(r + 10 + 2 * z));
    }
    for (uint8_t z = 0; z < FT_ZONES; z++) {
        out[o++] = ',';
        uint8_t d = r[16 + z];
        if (d != FT_DUTY_INACTIVE) {
            o += (size_t)snprintf(out + o, cap - o, "%u.%u", (unsigned)(d / 2), (unsigned)(d % 2) * 5u);
        }
    }
    o += (size_t)snprintf(out + o, cap - o, ",%u\n", (unsigned)r[19]);
    return o;
}
