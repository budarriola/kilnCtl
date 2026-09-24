#include "ota_record.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
/* ESP_ERR_NVS_NOT_FOUND only -- no nvs_*()/nvs_flash_*() function call in
 * this file reaches NVS directly any more (see hal_kv_* below). Reinstated
 * per ota_record.h's doc contract: ota_record_load()'s "no record yet" case
 * must keep returning this SPECIFIC code, not a generic ESP_FAIL, because
 * that is the value this header has always promised and nothing about the
 * hal_kv.h migration changes what "no record" means to a caller. */
#include "nvs.h"

static const char *TAG = "ota_record";

/* Same namespace as run_state.c/relay_cycles.c/zones_http.c, own key -- see
 * run_state.c's header comment for why a shared blob is the wrong move (a
 * corrupt/rejected record here must never be able to take zone config down
 * with it, and vice versa). */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_OTA_RECORD "ota_record"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_OTA_RECORD);

/* TODO.md 8.1's shared partition, split out of the default NVS partition. */
#define KILN_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);

/* Pins the on-disk layout the same way run_state.c's own _Static_assert
 * does: a silent field add/reorder would otherwise look like corruption to
 * anything that later tries to load this record, rather than fail the
 * build. There is no loader in this first pass (see ota_record.h -- only
 * the most recent record is kept and nothing currently reads it back), but
 * pinning the size now costs nothing and pays off the day something does.
 *
 * A plain _Static_assert() here builds fine under ESP-IDF's GCC, but this
 * file is ALSO compiled into App/test/build_host_tests.ps1's MSVC host-test
 * binary (added 2026-08-21 alongside the image_sha256_hex field, so
 * ota_record_fill() could get a real host test) -- and the cl.exe invocation
 * that script uses compiles .c files in a C mode old enough that
 * _Static_assert is a hard syntax error, not just unavailable. The classic
 * negative-array-size trick below is portable C89/C99/C11 alike and checks
 * exactly the same thing. */
typedef char ota_record_t_size_check[(sizeof(ota_record_t) == 216) ? 1 : -1];

/* Brings up KILN_NVS_PARTITION, erasing ONLY that partition if its contents
 * are unusable -- identical to run_state.c's/relay_cycles.c's own
 * nvs_partition_init(), duplicated rather than shared because each of those
 * modules already duplicates it independently (see run_state.c's header
 * comment: "each of which manages its own init/migration independently").
 * Cheap to call on every append: nvs_flash_init_partition() is a no-op
 * success if the partition is already initialized. */
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
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

// One side of ota_version_compare(), split out of an esp_app_desc_t.version
// string of the shape `git describe --tags --always --dirty` produces:
// "<tag>[-<count>-g<hash>][-dirty]" -- see ota_record.h's header comment on
// ota_version_compare() for the shapes this tree really emits (including the
// 31-char truncation that leaves "...-3140-" with no hash at all).
enum { OTA_VER_MAX_NUMS = 4 };
typedef struct {
    char tag[OTA_RECORD_VERSION_STR_MAX];
    long nums[OTA_VER_MAX_NUMS]; // numeric runs inside the tag, zero-padded
    int n_nums;
    long count;                  // commits since the tag, 0 if absent
    char hash[OTA_RECORD_VERSION_STR_MAX]; // hex after "-g", "" if absent/truncated off
    bool bare_hash;              // no tag at all: `--always` fell back to a hash
} ota_ver_parts_t;

static bool all_hex(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (; *s; s++) {
        if (!isxdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

static bool all_digits(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

static void strip_trailing_dashes(char *buf)
{
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] == '-') {
        buf[--len] = '\0';
    }
}

// Returns a pointer to the token after the last '-' in buf, or NULL if there
// is no '-' (the whole string is one token).
static char *last_token(char *buf)
{
    char *dash = strrchr(buf, '-');
    return dash ? dash + 1 : NULL;
}

static void parse_version_parts(const char *s, ota_ver_parts_t *out)
{
    memset(out, 0, sizeof(*out));
    char buf[OTA_RECORD_VERSION_STR_MAX];
    copy_str(buf, sizeof(buf), s);
    strip_trailing_dashes(buf); // truncation can leave "...-3140-"

    // "-dirty", or a truncated prefix of it ("-d", "-dir", ...) that can only
    // follow a hash token.
    char *tok = last_token(buf);
    if (tok && tok[0] != '\0' && strncmp(tok, "dirty", strlen(tok)) == 0) {
        tok[-1] = '\0';
        strip_trailing_dashes(buf);
        tok = last_token(buf);
    }
    // "-g<hex>" (possibly truncated, even down to a bare "-g").
    if (tok && tok[0] == 'g' && (tok[1] == '\0' || all_hex(tok + 1))) {
        copy_str(out->hash, sizeof(out->hash), tok + 1);
        tok[-1] = '\0';
        strip_trailing_dashes(buf);
        tok = last_token(buf);
    }
    // "-<count>" -- only when something precedes it (a tag).
    if (tok && tok != buf + 1 && all_digits(tok)) {
        out->count = strtol(tok, NULL, 10);
        tok[-1] = '\0';
        strip_trailing_dashes(buf);
    }

    copy_str(out->tag, sizeof(out->tag), buf);
    // No tag reachable: `git describe --always` emits just the abbreviated
    // hash (e.g. "9ca22048"), whose digits mean nothing numerically.
    if (strchr(out->tag, '.') == NULL && strlen(out->tag) >= 7 && all_hex(out->tag)) {
        out->bare_hash = true;
        return;
    }

    const char *p = out->tag;
    while (*p != '\0' && out->n_nums < OTA_VER_MAX_NUMS) {
        while (*p != '\0' && !isdigit((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *end = NULL;
        out->nums[out->n_nums++] = strtol(p, &end, 10);
        p = end;
    }
}

ota_version_cmp_t ota_version_compare(const char *version_before, const char *version_after)
{
    if (!version_before || !version_after || version_before[0] == '\0' || version_after[0] == '\0') {
        return OTA_VERSION_CMP_UNKNOWN;
    }

    ota_ver_parts_t b, a;
    parse_version_parts(version_before, &b);
    parse_version_parts(version_after, &a);
    if (b.bare_hash || a.bare_hash) {
        return OTA_VERSION_CMP_UNKNOWN;
    }

    if (strcmp(b.tag, a.tag) != 0) {
        // Different tags: order by the tag's own numbers only (a commit
        // count is relative to its own tag, so it says nothing across tags).
        if (b.n_nums == 0 || a.n_nums == 0) {
            return OTA_VERSION_CMP_UNKNOWN;
        }
        for (int i = 0; i < OTA_VER_MAX_NUMS; i++) {
            if (b.nums[i] != a.nums[i]) {
                return (a.nums[i] < b.nums[i]) ? OTA_VERSION_CMP_OLDER : OTA_VERSION_CMP_NEWER;
            }
        }
        return OTA_VERSION_CMP_UNKNOWN; // same numbers, differently named tags
    }

    // Same tag: the commit count orders them.
    if (b.count != a.count) {
        return (a.count < b.count) ? OTA_VERSION_CMP_OLDER : OTA_VERSION_CMP_NEWER;
    }
    // Same tag and count but two different commits (two branches the same
    // distance from the tag): no order exists. A hash truncated off either
    // string cannot be compared, so equal counts then read as SAME -- the
    // "-dirty" flag never affects the answer.
    if (b.hash[0] != '\0' && a.hash[0] != '\0') {
        size_t nb = strlen(b.hash), na = strlen(a.hash);
        size_t n = (nb < na) ? nb : na;
        if (strncmp(b.hash, a.hash, n) != 0) {
            return OTA_VERSION_CMP_UNKNOWN;
        }
    }
    return OTA_VERSION_CMP_SAME;
}

void ota_record_fill(ota_record_t *out, uint32_t uptime_s, const char *processor,
                      const char *version_before, const char *version_after, bool success,
                      const char *reason, const char *image_sha256_hex_or_null)
{
    memset(out, 0, sizeof(*out));
    out->version = OTA_RECORD_VERSION;
    out->uptime_s = uptime_s;
    copy_str(out->processor, sizeof(out->processor), processor);
    copy_str(out->version_before, sizeof(out->version_before), version_before);
    copy_str(out->version_after, sizeof(out->version_after), version_after);
    out->success = success ? 1u : 0u;
    copy_str(out->reason, sizeof(out->reason), reason);
    copy_str(out->image_sha256_hex, sizeof(out->image_sha256_hex), image_sha256_hex_or_null);

    ota_version_cmp_t cmp = ota_version_compare(out->version_before, out->version_after);
    out->version_compare_known = (cmp != OTA_VERSION_CMP_UNKNOWN) ? 1u : 0u;
    out->is_downgrade = (cmp == OTA_VERSION_CMP_OLDER) ? 1u : 0u;
}

esp_err_t ota_record_append(const ota_record_t *rec)
{
    if (!rec) {
        return ESP_ERR_INVALID_ARG;
    }

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- update record not saved",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return hal_status_to_esp_err(part_err);
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "hal_kv_open failed: %s -- update record not saved",
                 hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_OTA_RECORD, rec, sizeof(*rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);

    if (err != HAL_OK) {
        /* Best-effort, like run_state.c's own persist failures -- an update
         * record that failed to save is not a reason to have refused (or
         * un-refused) the update itself, but it must not be silent: this is
         * the only place the outcome would otherwise be recorded at all. */
        ESP_LOGE(TAG, "could not persist OTA update record: %s -- this update's outcome will not "
                      "survive a reboot",
                 hal_status_to_name(err));
    } else {
        ESP_LOGI(TAG, "OTA update record saved: processor=%s success=%d reason=\"%s\" "
                      "version %s -> %s%s sha256=%s",
                 rec->processor, (int)rec->success, rec->reason, rec->version_before,
                 rec->version_after,
                 rec->version_compare_known ? (rec->is_downgrade ? " (DOWNGRADE)" : " (upgrade/same)")
                                             : " (version comparison unknown)",
                 rec->image_sha256_hex[0] ? rec->image_sha256_hex : "(none)");
    }
    return hal_status_to_esp_err(err);
}

esp_err_t ota_record_load(ota_record_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- cannot read update record",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return hal_status_to_esp_err(part_err);
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        /* HAL_NOT_FOUND here means the namespace itself has never been
         * written -- same "no record yet" case as the blob-not-found path
         * below, not a real failure. Reported as the SPECIFIC
         * ESP_ERR_NVS_NOT_FOUND, per ota_record.h's doc contract -- unlike
         * hal_status_to_esp_err()'s generic HAL_NOT_FOUND -> ESP_ERR_NOT_FOUND
         * mapping (a different, generic-driver code), this preserves the
         * exact value callers have always been able to rely on for "no
         * record yet" specifically. */
        if (err == HAL_NOT_FOUND) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        ESP_LOGE(TAG, "hal_kv_open (read) failed: %s", hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }

    size_t len = sizeof(*out);
    err = hal_kv_get_blob(&h, NVS_KEY_OTA_RECORD, out, &len);
    hal_kv_close(&h);

    if (err == HAL_OK && len != sizeof(*out)) {
        /* Same "a size-mismatched blob is not a current record" tolerance
         * ota_record.h's doc comment on this function promises -- treat it
         * as absent rather than hand back a partially-filled struct. */
        ESP_LOGW(TAG, "stored OTA record is %u bytes, expected %u -- treating as absent",
                 (unsigned)len, (unsigned)sizeof(*out));
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err == HAL_OK) {
        return ESP_OK;
    }
    if (err == HAL_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    ESP_LOGE(TAG, "hal_kv_get_blob(ota_record) failed: %s", hal_status_to_name(err));
    return hal_status_to_esp_err(err);
}
