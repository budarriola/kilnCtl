#include "log_store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define LOG_STORE_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define LOG_STORE_MKDIR(path) mkdir((path), 0755)
#endif

/* No dirent.h anywhere in this file on purpose: the host test build is MSVC,
 * which has no dirent.h, and this module needs to compile identically on
 * host and on-device. Instead of scanning the directory, each kind's
 * [oldest_idx, newest_idx] segment range is tracked explicitly and persisted
 * to a tiny manifest file -- see this file's header comment in log_store.h
 * for the rotation policy this implements. */

typedef struct {
    bool   has_data;
    size_t oldest_idx;
    size_t newest_idx;
} kind_state_t;

static char         s_base_dir[512];
static bool         s_init = false;
static kind_state_t s_state[LOG_STORE_KIND_COUNT];

static const char *kind_prefix(log_store_kind_t kind)
{
    return (kind == LOG_STORE_KIND_AUTOTUNE) ? "autotune" : "firing";
}

static void segment_path(log_store_kind_t kind, size_t idx, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s_%010zu.log", s_base_dir, kind_prefix(kind), idx);
}

static void manifest_path(log_store_kind_t kind, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.manifest", s_base_dir, kind_prefix(kind));
}

static long file_size_or_zero(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long sz = ftell(f);
    fclose(f);
    return sz < 0 ? 0 : sz;
}

static void load_manifest(log_store_kind_t kind)
{
    kind_state_t *st = &s_state[kind];
    st->has_data = false;
    st->oldest_idx = 0;
    st->newest_idx = 0;

    char path[600];
    manifest_path(kind, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    unsigned long oldest = 0, newest = 0;
    int has = 0;
    if (fscanf(f, "%lu %lu %d", &oldest, &newest, &has) == 3) {
        st->oldest_idx = (size_t)oldest;
        st->newest_idx = (size_t)newest;
        st->has_data = (has != 0);
    }
    fclose(f);
}

static esp_err_t save_manifest(log_store_kind_t kind)
{
    kind_state_t *st = &s_state[kind];
    char path[600];
    manifest_path(kind, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        return ESP_FAIL;
    }
    int n = fprintf(f, "%zu %zu %d\n", st->oldest_idx, st->newest_idx, st->has_data ? 1 : 0);
    int close_rc = fclose(f);
    if (n < 0 || close_rc != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t log_store_init(const char *base_dir)
{
    if (!base_dir || base_dir[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(base_dir) >= sizeof(s_base_dir)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Best-effort: if it already exists this fails harmlessly (EEXIST on
     * POSIX, similar on Windows) and is not treated as an error -- the
     * directory being there already is the success case, not a failure. */
    LOG_STORE_MKDIR(base_dir);

    strncpy(s_base_dir, base_dir, sizeof(s_base_dir) - 1);
    s_base_dir[sizeof(s_base_dir) - 1] = '\0';
    s_init = true;

    for (int k = 0; k < LOG_STORE_KIND_COUNT; k++) {
        load_manifest((log_store_kind_t)k);
    }
    return ESP_OK;
}

bool log_store_is_init(void)
{
    return s_init;
}

static esp_err_t rotate_if_needed(log_store_kind_t kind, size_t record_len)
{
    kind_state_t *st = &s_state[kind];

    if (!st->has_data) {
        st->oldest_idx = 0;
        st->newest_idx = 0;
        st->has_data = true;
        return ESP_OK;
    }

    char path[600];
    segment_path(kind, st->newest_idx, path, sizeof(path));
    long cur_size = file_size_or_zero(path);
    size_t needed = (size_t)cur_size + 2u + record_len; /* +2 for the uint16 length prefix */

    if (cur_size > 0 && needed > LOG_STORE_SEGMENT_MAX_BYTES) {
        st->newest_idx++;
    }
    return ESP_OK;
}

static void trim_to_cap(log_store_kind_t kind)
{
    kind_state_t *st = &s_state[kind];
    if (!st->has_data) {
        return;
    }
    while ((st->newest_idx - st->oldest_idx + 1) > LOG_STORE_MAX_SEGMENTS) {
        char old_path[600];
        segment_path(kind, st->oldest_idx, old_path, sizeof(old_path));
        remove(old_path); /* best-effort -- an already-missing file is fine */
        st->oldest_idx++;
    }
}

esp_err_t log_store_append(log_store_kind_t kind, const void *data, size_t len)
{
    if (!s_init) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((int)kind < 0 || kind >= LOG_STORE_KIND_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!data || len == 0 || len > LOG_STORE_MAX_RECORD_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = rotate_if_needed(kind, len);
    if (rc != ESP_OK) {
        return rc;
    }

    char path[600];
    segment_path(kind, s_state[kind].newest_idx, path, sizeof(path));

    /* DEGRADE, DON'T WEDGE: a failed fopen/fwrite/fclose here (full or
     * failing filesystem, a directory sitting where the segment file should
     * be, anything) drops this one record and returns ESP_FAIL. It never
     * retries and never touches any state that would leave the store
     * inconsistent for the NEXT call -- s_state[kind] is only advanced
     * further below, after a successful write. Binary mode ("ab") on
     * purpose: a record's bytes may be any value, including 0x0A/0x00, so
     * text-mode translation (CRLF rewriting on Windows host tests) must
     * never touch them. */
    FILE *f = fopen(path, "ab");
    if (!f) {
        return ESP_FAIL;
    }
    uint8_t len_prefix[2] = { (uint8_t)(len & 0xFFu), (uint8_t)((len >> 8) & 0xFFu) };
    int wrote_ok = (fwrite(len_prefix, 1, sizeof(len_prefix), f) == sizeof(len_prefix)) &&
                   (fwrite(data, 1, len, f) == len);
    int close_ok = (fclose(f) == 0);
    if (!wrote_ok || !close_ok) {
        return ESP_FAIL;
    }

    trim_to_cap(kind);

    if (save_manifest(kind) != ESP_OK) {
        /* The record itself is safely on disk; losing the manifest update
         * only risks re-scanning/rotation drift on the next boot, not data
         * loss or a wedge -- still reported so a caller/test can see it. */
        return ESP_FAIL;
    }
    return ESP_OK;
}

size_t log_store_total_bytes(log_store_kind_t kind)
{
    if (!s_init || (int)kind < 0 || kind >= LOG_STORE_KIND_COUNT) {
        return 0;
    }
    kind_state_t *st = &s_state[kind];
    if (!st->has_data) {
        return 0;
    }
    size_t total = 0;
    for (size_t idx = st->oldest_idx; idx <= st->newest_idx; idx++) {
        char path[600];
        segment_path(kind, idx, path, sizeof(path));
        total += (size_t)file_size_or_zero(path);
    }
    return total;
}

size_t log_store_segment_count(log_store_kind_t kind)
{
    if (!s_init || (int)kind < 0 || kind >= LOG_STORE_KIND_COUNT) {
        return 0;
    }
    kind_state_t *st = &s_state[kind];
    if (!st->has_data) {
        return 0;
    }
    return st->newest_idx - st->oldest_idx + 1;
}

struct log_store_reader {
    log_store_kind_t kind;
    size_t           idx;
    size_t           newest_idx;
    bool             has_data;
    FILE            *fp;
};

log_store_reader_t *log_store_reader_open(log_store_kind_t kind)
{
    log_store_reader_t *rd = (log_store_reader_t *)calloc(1, sizeof(*rd));
    if (!rd) {
        return NULL;
    }
    rd->kind = kind;
    rd->fp = NULL;

    if (!s_init || (int)kind < 0 || kind >= LOG_STORE_KIND_COUNT) {
        rd->has_data = false;
        return rd;
    }
    kind_state_t *st = &s_state[kind];
    rd->has_data = st->has_data;
    rd->idx = st->oldest_idx;
    rd->newest_idx = st->newest_idx;
    return rd;
}

static bool reader_open_current(log_store_reader_t *rd)
{
    char path[600];
    segment_path(rd->kind, rd->idx, path, sizeof(path));
    rd->fp = fopen(path, "rb"); /* binary mode -- see log_store_append()'s "ab" comment */
    return rd->fp != NULL;
}

bool log_store_reader_next(log_store_reader_t *rd, void *out, size_t cap, size_t *out_len)
{
    if (!rd || !out || cap == 0 || !rd->has_data) {
        return false;
    }

    for (;;) {
        if (!rd->fp) {
            if (rd->idx > rd->newest_idx) {
                return false;
            }
            if (!reader_open_current(rd)) {
                /* Missing/unreadable segment -- skip it rather than wedge
                 * the reader; this is the read-side half of "degrades
                 * safely", matching log_store_append()'s write-side
                 * behaviour. */
                rd->idx++;
                continue;
            }
        }

        uint8_t len_prefix[2];
        size_t got = fread(len_prefix, 1, sizeof(len_prefix), rd->fp);
        if (got != sizeof(len_prefix)) {
            /* End of this segment (clean EOF or a short/corrupt trailing
             * prefix) -- either way, move on to the next segment rather
             * than wedge the reader. */
            fclose(rd->fp);
            rd->fp = NULL;
            rd->idx++;
            continue;
        }
        size_t rec_len = (size_t)len_prefix[0] | ((size_t)len_prefix[1] << 8);

        /* Read the full record into a bounded scratch buffer regardless of
         * `cap` so a short caller buffer truncates safely instead of
         * leaving the file position mid-record for the next call. */
        uint8_t scratch[LOG_STORE_MAX_RECORD_LEN];
        size_t to_read = rec_len > sizeof(scratch) ? sizeof(scratch) : rec_len;
        size_t read_now = fread(scratch, 1, to_read, rd->fp);
        if (rec_len > sizeof(scratch)) {
            /* Corrupt/oversized length prefix -- skip the excess bytes we
             * cannot buffer so the stream resyncs at the next record. */
            fseek(rd->fp, (long)(rec_len - sizeof(scratch)), SEEK_CUR);
        }
        if (read_now != to_read) {
            fclose(rd->fp);
            rd->fp = NULL;
            rd->idx++;
            continue;
        }

        size_t n = rec_len < cap ? rec_len : cap;
        memcpy(out, scratch, n);
        if (out_len) {
            *out_len = rec_len;
        }
        return true;
    }
}

void log_store_reader_close(log_store_reader_t *rd)
{
    if (!rd) {
        return;
    }
    if (rd->fp) {
        fclose(rd->fp);
    }
    free(rd);
}
