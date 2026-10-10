/* fake_kv.c -- host fake backend for hal_kv.h. See fake_kv.h. */
#include "fake_kv.h"

#include <string.h>

#define FAKE_KV_HANDLE_MAGIC 0x4B564831u /* "KVH1" */

typedef struct {
    uint32_t magic;
    int      slot;
} fake_kv_tag_t;

_Static_assert(sizeof(fake_kv_tag_t) <= HAL_KV_HANDLE_STORAGE_BYTES,
               "fake_kv_tag_t must fit hal_kv_handle_t storage");

/* The known worst-case key count for profiles_http.c/profiles_favorites.c's
 * shared "kiln_cfg" namespace: PROFILES_MAX_COUNT (100, firmware/KilnFW/App/
 * drivers/http/profiles_http.c) "profN" keys, + "prof_used", +
 * "prof_favusr"/"prof_favbi" (profiles_favorites.c) = 103. Hand-pinned here
 * rather than computed from profiles_types.h's PROFILES_MAX_COUNT -- this
 * file is a shared, portable HAL fake (SaftyFW's host tests link it too),
 * and including an ESP32 app header just to derive one constant would be a
 * worse dependency than a hand-pinned number with a compile-time tripwire.
 * If FAKE_KV_MAX_KEYS_PER_NS (fake_kv.h) is ever lowered, or this number
 * needs to grow because profiles_http.c's namespace grows again, this
 * assertion fails the BUILD rather than letting a host test silently strand
 * partway through staging its keys (find_key()'s "table full" -> HAL_NO_MEM
 * on writes past the cap, easy to miss if a caller loop doesn't check every
 * return). See fake_kv.h's FAKE_KV_MAX_KEYS_PER_NS comment for the history. */
#define FAKE_KV_KNOWN_WORST_CASE_KILN_CFG_KEYS 103
_Static_assert(FAKE_KV_MAX_KEYS_PER_NS >= FAKE_KV_KNOWN_WORST_CASE_KILN_CFG_KEYS,
               "FAKE_KV_MAX_KEYS_PER_NS must cover kiln_cfg's known worst case (103 keys) -- see the comment above");

typedef struct {
    bool     in_use;
    char     name[FAKE_KV_MAX_KEY_LEN];

    bool     committed_valid;
    bool     committed_is_str;
    bool     committed_corrupted;
    uint8_t  committed_data[FAKE_KV_MAX_VALUE_BYTES];
    size_t   committed_len;

    bool     pending_set;      /* a pending write (or tombstone) exists */
    bool     pending_tombstone;
    bool     pending_is_str;
    uint8_t  pending_data[FAKE_KV_MAX_VALUE_BYTES];
    size_t   pending_len;
} fake_kv_key_slot_t;

typedef struct {
    bool                in_use;
    char                name[FAKE_KV_MAX_NAME_LEN];
    fake_kv_key_slot_t  keys[FAKE_KV_MAX_KEYS_PER_NS];
} fake_kv_namespace_t;

typedef struct {
    bool                 initialized;
    char                 name[FAKE_KV_MAX_NAME_LEN]; /* "" for the default partition */
    fake_kv_namespace_t  namespaces[FAKE_KV_MAX_NAMESPACES_PER_PART];
} fake_kv_partition_t;

typedef struct {
    bool          in_use;
    int           partition_slot;
    int           ns_slot;
    hal_kv_mode_t mode;
} fake_kv_handle_slot_t;

static fake_kv_partition_t    s_partitions[FAKE_KV_MAX_PARTITIONS];
static fake_kv_handle_slot_t  s_handles[FAKE_KV_MAX_HANDLES];
static bool                   s_write_safe_here = true;
static bool                   s_next_write_fail_armed = false;
static hal_status_t           s_next_write_fail_status = HAL_OK;
static unsigned                s_next_write_fail_skip = 0u;
/* True when the armed write failure fires on THIS call: skips the first
 * s_next_write_fail_skip write-class calls, then fires once. */
static bool take_write_fail(void)
{
    if (!s_next_write_fail_armed) {
        return false;
    }
    if (s_next_write_fail_skip > 0u) {
        s_next_write_fail_skip--;
        return false;
    }
    return true;
}
static bool                   s_lossy_uncommitted = false;
static unsigned               s_silent_erase_noops = 0u;
static unsigned               s_silent_set_noops = 0u;
static bool                   s_blob_get_misses_size1 = false;
/* One-shot hal_kv_open() failure injection, scoped to a single namespace
 * name so scripting a failure for one namespace (e.g. the boot_guard legacy
 * namespace) can't accidentally also fail an unrelated open the same test
 * performs first (e.g. the primary namespace). Added for
 * load_count_strict()'s "legacy namespace open fails with something other
 * than HAL_NOT_FOUND" case, which no existing fake_kv script could reach:
 * a real namespace-name-length/HAL_INVALID_ARG failure isn't test-triggerable
 * for a fixed, short, hardcoded namespace string. */
static bool                   s_next_open_fail_armed = false;
static hal_status_t           s_next_open_fail_status = HAL_OK;
static char                   s_next_open_fail_namespace[FAKE_KV_MAX_NAME_LEN];

static const char *norm_partition(const char *partition)
{
    /* The real backend selects the default partition with either NULL or the
     * literal NVS_DEFAULT_PART_NAME ("nvs"); fold both to one slot here so a
     * test that seeds via one spelling and a module that reads via the other
     * (wifi_prov_nvs.c uses "nvs", most migrations use NULL) sees one store. */
    if (partition == NULL || strcmp(partition, "nvs") == 0) return "";
    return partition;
}

/* Bounded copy, always NUL-terminated within dst_size bytes -- avoids MSVC's
 * C4996 on strncpy (which would need _CRT_SECURE_NO_WARNINGS to silence
 * under /WX) while keeping strncpy's truncate-and-terminate semantics. Every
 * caller's dst buffer is already zero-filled (memset on the containing
 * struct), so strncpy's zero-pad-the-remainder behavior is not needed here. */
static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len > dst_size - 1) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int find_partition_slot(const char *partition)
{
    const char *name = norm_partition(partition);
    for (int i = 0; i < FAKE_KV_MAX_PARTITIONS; i++) {
        if (s_partitions[i].initialized && strcmp(s_partitions[i].name, name) == 0) return i;
    }
    return -1;
}

static fake_kv_handle_slot_t *get_handle(const hal_kv_handle_t *h)
{
    if (h == NULL) return NULL;
    fake_kv_tag_t tag;
    memcpy(&tag, h->storage, sizeof(tag));
    if (tag.magic != FAKE_KV_HANDLE_MAGIC) return NULL;
    if (tag.slot < 0 || tag.slot >= FAKE_KV_MAX_HANDLES) return NULL;
    if (!s_handles[tag.slot].in_use) return NULL;
    return &s_handles[tag.slot];
}

static fake_kv_key_slot_t *find_key(fake_kv_namespace_t *ns, const char *key, bool create)
{
    int free_slot = -1;
    for (int i = 0; i < FAKE_KV_MAX_KEYS_PER_NS; i++) {
        if (ns->keys[i].in_use && strcmp(ns->keys[i].name, key) == 0) return &ns->keys[i];
        if (!ns->keys[i].in_use && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return NULL;
    memset(&ns->keys[free_slot], 0, sizeof(ns->keys[free_slot]));
    ns->keys[free_slot].in_use = true;
    copy_bounded(ns->keys[free_slot].name, sizeof(ns->keys[free_slot].name), key);
    return &ns->keys[free_slot];
}

/* key existing "logically" means it has a committed value or a pending
 * (non-tombstone) write -- used by erase_key/get_* to decide HAL_NOT_FOUND. */
static bool key_logically_present(const fake_kv_key_slot_t *k)
{
    if (k->pending_set) return !k->pending_tombstone;
    return k->committed_valid;
}

/* Counts every do_get() call regardless of outcome -- see fake_kv_get_call_
 * count()'s doc comment. Reset by fake_kv_reset_all(). Declared here (ahead
 * of do_get() and fake_kv_reset_all(), both below) since this file has no
 * header of its own for internal statics and forward declarations. */
static unsigned s_get_call_count = 0;

void fake_kv_reset_all(void)
{
    hal_kv_set_write_refuse_hook(NULL);
    memset(s_partitions, 0, sizeof(s_partitions));
    memset(s_handles, 0, sizeof(s_handles));
    s_write_safe_here = true;
    s_next_write_fail_armed = false;
    s_next_write_fail_skip = 0u;
    s_next_write_fail_status = HAL_OK;
    s_lossy_uncommitted = false;
    s_silent_erase_noops = 0u;
    s_silent_set_noops = 0u;
    s_blob_get_misses_size1 = false;
    s_get_call_count = 0u;
    s_next_open_fail_armed = false;
    s_next_open_fail_status = HAL_OK;
    s_next_open_fail_namespace[0] = '\0';
}

/* Total hal_kv_get_blob()/hal_kv_get_str() calls since the last reset,
 * regardless of outcome (found/not-found/error all count). Added 2026-09-15
 * (INFO finding, docs/audits/review_crash_gate_low_fixes_c534a0df_2026-09-15.md)
 * so a test can snapshot this around a call under test and assert NO read
 * happened outside a specific place -- e.g. proving crash_report_acknowledge()
 * performs no load() in the caller's own task before dispatching onto the
 * flash worker, which the dispatch-count assertion alone cannot catch (a
 * caller-side load() plus a dispatched job would both count as "one
 * dispatch" and give the same answer). */
unsigned fake_kv_get_call_count(void)
{
    return s_get_call_count;
}

bool fake_kv_handle_is_live(const hal_kv_handle_t *h)
{
    return get_handle(h) != NULL;
}

bool fake_kv_has_uncommitted_writes(const char *partition)
{
    int p = find_partition_slot(partition);
    if (p < 0) return false;
    fake_kv_partition_t *part = &s_partitions[p];
    for (int n = 0; n < FAKE_KV_MAX_NAMESPACES_PER_PART; n++) {
        if (!part->namespaces[n].in_use) continue;
        for (int k = 0; k < FAKE_KV_MAX_KEYS_PER_NS; k++) {
            if (part->namespaces[n].keys[k].in_use && part->namespaces[n].keys[k].pending_set) {
                return true;
            }
        }
    }
    return false;
}

void fake_kv_set_lossy_uncommitted(bool lossy)
{
    s_lossy_uncommitted = lossy;
}

void fake_kv_simulate_power_loss(void)
{
    for (int p = 0; p < FAKE_KV_MAX_PARTITIONS; p++) {
        if (!s_partitions[p].initialized) continue;
        for (int n = 0; n < FAKE_KV_MAX_NAMESPACES_PER_PART; n++) {
            if (!s_partitions[p].namespaces[n].in_use) continue;
            for (int k = 0; k < FAKE_KV_MAX_KEYS_PER_NS; k++) {
                fake_kv_key_slot_t *ks = &s_partitions[p].namespaces[n].keys[k];
                if (!ks->pending_set) continue;
                if (s_lossy_uncommitted) {
                    /* OVER-approximation, opt-in: discard the pending write
                     * as if it never reached flash -- see fake_kv.h's
                     * durability-model comment. */
                    ks->pending_set = false;
                    ks->pending_tombstone = false;
                    ks->pending_len = 0;
                    continue;
                }
                /* DEFAULT: keep it -- models real NVS's immediate write to
                 * flash, which nvs_commit() merely acknowledges rather than
                 * performs. Same merge hal_kv_commit() does. */
                if (ks->pending_tombstone) {
                    ks->committed_valid = false;
                    ks->committed_len = 0;
                    ks->committed_corrupted = false;
                } else {
                    ks->committed_valid = true;
                    ks->committed_is_str = ks->pending_is_str;
                    ks->committed_len = ks->pending_len;
                    memcpy(ks->committed_data, ks->pending_data, ks->pending_len);
                    ks->committed_corrupted = false;
                }
                ks->pending_set = false;
                ks->pending_tombstone = false;
                ks->pending_len = 0;
            }
        }
    }
}

bool fake_kv_script_corrupt_key(const char *partition, const char *namespace_name, const char *key)
{
    int p = find_partition_slot(partition);
    if (p < 0 || namespace_name == NULL || key == NULL) return false;
    fake_kv_partition_t *part = &s_partitions[p];
    for (int n = 0; n < FAKE_KV_MAX_NAMESPACES_PER_PART; n++) {
        if (!part->namespaces[n].in_use || strcmp(part->namespaces[n].name, namespace_name) != 0) continue;
        fake_kv_key_slot_t *k = find_key(&part->namespaces[n], key, false);
        if (!k || !k->committed_valid) return false;
        k->committed_corrupted = true;
        return true;
    }
    return false;
}

void fake_kv_script_next_write_status(hal_status_t status)
{
    s_next_write_fail_armed = true;
    s_next_write_fail_skip = 0u;
    s_next_write_fail_status = status;
}

bool fake_kv_script_next_open_status(const char *namespace_name, hal_status_t status)
{
    if (namespace_name == NULL || strlen(namespace_name) >= FAKE_KV_MAX_NAME_LEN) return false;
    s_next_open_fail_armed = true;
    s_next_open_fail_status = status;
    copy_bounded(s_next_open_fail_namespace, sizeof(s_next_open_fail_namespace), namespace_name);
    return true;
}

void fake_kv_script_silent_erase_noops(unsigned count)
{
    s_silent_erase_noops = count;
}

void fake_kv_script_silent_set_noops(unsigned count)
{
    s_silent_set_noops = count;
}

void fake_kv_set_write_safe_here(bool safe)
{
    s_write_safe_here = safe;
}

/* --- hal_kv.h implementation --- */

/* Mutation fence mirror of hal_kv_esp.c (hal_kv.h "Mutation fence"). */
static hal_kv_write_refuse_fn_t s_write_refuse_fn = NULL;

void hal_kv_set_write_refuse_hook(hal_kv_write_refuse_fn_t fn)
{
    s_write_refuse_fn = fn;
}

static bool write_refused(const fake_kv_handle_slot_t *hs)
{
    if (!s_write_refuse_fn) return false;
    const char *name = s_partitions[hs->partition_slot].name;
    return s_write_refuse_fn(name[0] ? name : NULL);
}


hal_status_t hal_kv_open(hal_kv_handle_t *h, const char *namespace_name,
                          hal_kv_mode_t mode, const char *partition)
{
    if (h == NULL || namespace_name == NULL) return HAL_INVALID_ARG;
    if (strlen(namespace_name) >= FAKE_KV_MAX_NAME_LEN) return HAL_INVALID_ARG;

    if (s_next_open_fail_armed && strcmp(s_next_open_fail_namespace, namespace_name) == 0) {
        s_next_open_fail_armed = false;
        return s_next_open_fail_status;
    }

    int p = find_partition_slot(partition);
    /* HAL_NOT_READY, NOT HAL_NOT_FOUND -- deliberately NOT mirroring
     * hal_kv_esp.c's ESP_ERR_NVS_PART_NOT_FOUND -> HAL_NOT_FOUND mapping
     * here. The two "partition missing" cases are different concepts: on
     * target it means the partition table itself never had this partition
     * flashed; here it means this test/harness simply has not called
     * hal_kv_init_partition() yet for this fake run -- a usage-discipline
     * signal, not a "board was never provisioned" signal, and existing
     * tests (test_fake_kv.c's "open before init_partition -> HAL_NOT_READY"
     * and its uninitialized-handle checks, test_safety_cfg_store.c's
     * "hal_kv_open() fails closed with HAL_NOT_READY") already lock this
     * return value in. Changing it would fix nothing a real board could
     * ever observe (a partition that's missing from a fake's in-RAM model
     * is always because a test forgot to init it, never because of a
     * misflashed partition table) and would break those call sites. */
    if (p < 0) return HAL_NOT_READY;
    fake_kv_partition_t *part = &s_partitions[p];

    int ns_slot = -1, free_ns = -1;
    for (int n = 0; n < FAKE_KV_MAX_NAMESPACES_PER_PART; n++) {
        if (part->namespaces[n].in_use && strcmp(part->namespaces[n].name, namespace_name) == 0) {
            ns_slot = n;
            break;
        }
        if (!part->namespaces[n].in_use && free_ns < 0) free_ns = n;
    }
    if (ns_slot < 0) {
        if (mode == HAL_KV_MODE_READ_ONLY) return HAL_NOT_FOUND;
        if (free_ns < 0) return HAL_NO_MEM;
        memset(&part->namespaces[free_ns], 0, sizeof(part->namespaces[free_ns]));
        part->namespaces[free_ns].in_use = true;
        copy_bounded(part->namespaces[free_ns].name, sizeof(part->namespaces[free_ns].name), namespace_name);
        ns_slot = free_ns;
    }

    int free_handle = -1;
    for (int i = 0; i < FAKE_KV_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) { free_handle = i; break; }
    }
    if (free_handle < 0) return HAL_NO_MEM;

    fake_kv_handle_slot_t *hs = &s_handles[free_handle];
    memset(hs, 0, sizeof(*hs));
    hs->in_use = true;
    hs->partition_slot = p;
    hs->ns_slot = ns_slot;
    hs->mode = mode;

    fake_kv_tag_t tag;
    tag.magic = FAKE_KV_HANDLE_MAGIC;
    tag.slot = free_handle;
    memset(h->storage, 0, sizeof(h->storage));
    memcpy(h->storage, &tag, sizeof(tag));
    return HAL_OK;
}

hal_status_t hal_kv_close(hal_kv_handle_t *h)
{
    fake_kv_handle_slot_t *hs = get_handle(h);
    if (!hs) return HAL_NOT_READY;
    hs->in_use = false;
    memset(h->storage, 0, sizeof(h->storage));
    return HAL_OK;
}

hal_status_t hal_kv_commit(hal_kv_handle_t *h)
{
    fake_kv_handle_slot_t *hs = get_handle(h);
    if (!hs) return HAL_NOT_READY;

    if (write_refused(hs)) return HAL_NOT_READY;

    if (take_write_fail()) {
        s_next_write_fail_armed = false;
        return s_next_write_fail_status;
    }

    /* Commit flushes the WHOLE partition's page in real NVS, not just this
     * handle's namespace -- merge every namespace's pending writes. */
    fake_kv_partition_t *part = &s_partitions[hs->partition_slot];
    for (int n = 0; n < FAKE_KV_MAX_NAMESPACES_PER_PART; n++) {
        if (!part->namespaces[n].in_use) continue;
        for (int k = 0; k < FAKE_KV_MAX_KEYS_PER_NS; k++) {
            fake_kv_key_slot_t *ks = &part->namespaces[n].keys[k];
            if (!ks->in_use || !ks->pending_set) continue;
            if (ks->pending_tombstone) {
                ks->committed_valid = false;
                ks->committed_len = 0;
                ks->committed_corrupted = false;
            } else {
                ks->committed_valid = true;
                ks->committed_is_str = ks->pending_is_str;
                ks->committed_len = ks->pending_len;
                memcpy(ks->committed_data, ks->pending_data, ks->pending_len);
                ks->committed_corrupted = false;
            }
            ks->pending_set = false;
            ks->pending_tombstone = false;
            ks->pending_len = 0;
        }
    }
    return HAL_OK;
}

static hal_status_t do_get(fake_kv_handle_slot_t *hs, const char *key, bool want_str,
                            void *buf, size_t *out_len)
{
    s_get_call_count++;
    if (!hs) return HAL_NOT_READY;
    if (key == NULL || out_len == NULL) return HAL_INVALID_ARG;
    /* Matches the real backend: ESP_ERR_NVS_KEY_TOO_LONG has no explicit
     * case in hal_kv_esp_err_to_status(), so it falls through to
     * hal_esp_err_to_status()'s default -> HAL_IO. Reject up front rather
     * than silently truncating (copy_bounded) and comparing the truncated
     * name against the full key in find_key(), which used to report
     * HAL_NOT_FOUND for a key that was, from the caller's point of view,
     * already set. */
    if (strlen(key) >= FAKE_KV_MAX_KEY_LEN) return HAL_IO;

    fake_kv_namespace_t *ns = &s_partitions[hs->partition_slot].namespaces[hs->ns_slot];
    fake_kv_key_slot_t *k = find_key(ns, key, false);
    if (!k || !key_logically_present(k)) return HAL_NOT_FOUND;

    bool is_str;
    const uint8_t *data;
    size_t len;
    if (k->pending_set) {
        is_str = k->pending_is_str;
        data = k->pending_data;
        len = k->pending_len;
    } else {
        if (k->committed_corrupted) return HAL_IO;
        is_str = k->committed_is_str;
        data = k->committed_data;
        len = k->committed_len;
    }

    /* Matches ESP-IDF 6.0.2: Storage::findItem keeps scanning past a page
     * TYPE_MISMATCH and ends in NOT_FOUND, so a typed read of a key stored
     * with another type is NOT_FOUND on target, not a type error. */
    if (want_str != is_str) return HAL_NOT_FOUND;

    if (buf == NULL) { /* size probe */
        *out_len = len;
        return HAL_OK;
    }
    if (*out_len < len) return HAL_INVALID_SIZE;
    if (len) memcpy(buf, data, len);
    *out_len = len;
    return HAL_OK;
}

void fake_kv_script_blob_get_misses_size1(bool on)
{
    s_blob_get_misses_size1 = on;
}

hal_status_t hal_kv_get_blob(hal_kv_handle_t *h, const char *key, void *buf, size_t *out_len)
{
    if (s_blob_get_misses_size1) {
        size_t probe = 0;
        hal_status_t pe = do_get(get_handle(h), key, false, NULL, &probe);
        if (pe == HAL_OK && probe == 1u) {
            return HAL_NOT_FOUND;
        }
    }
    return do_get(get_handle(h), key, false, buf, out_len);
}

hal_status_t hal_kv_get_str(hal_kv_handle_t *h, const char *key, char *buf, size_t *out_len)
{
    return do_get(get_handle(h), key, true, buf, out_len);
}

static hal_status_t do_set(fake_kv_handle_slot_t *hs, const char *key, bool is_str,
                            const void *buf, size_t len)
{
    if (!hs) return HAL_NOT_READY;
    if (hs->mode != HAL_KV_MODE_READ_WRITE) return HAL_INVALID_ARG;
    if (key == NULL) return HAL_INVALID_ARG;
    if (strlen(key) >= FAKE_KV_MAX_KEY_LEN) return HAL_IO; /* see do_get()'s comment */
    if (buf == NULL && len > 0) return HAL_INVALID_ARG;
    if (len > FAKE_KV_MAX_VALUE_BYTES) return HAL_INVALID_SIZE;
    if (write_refused(hs)) return HAL_NOT_READY;

    if (take_write_fail()) {
        s_next_write_fail_armed = false;
        return s_next_write_fail_status;
    }

    if (s_silent_set_noops > 0u) {
        /* The lie: report success, stage nothing -- the set_blob/set_str/
         * set_u32 analogue of fake_kv_script_silent_erase_noops(). Added
         * for boot_guard_reset_counter()'s host tests
         * (docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md):
         * boot_guard.c's persist_count() writes via hal_kv_set_blob(), not
         * erase_key(), so the erase-only noop above cannot model a lying
         * write on that path at all -- this is the only way to reach
         * clear_persisted_counter_verified_locked()'s read-back check with
         * a *_set_blob*-based writer instead of an honest failure, which
         * would exit on the return code long before the read-back runs. */
        s_silent_set_noops--;
        return HAL_OK;
    }

    fake_kv_namespace_t *ns = &s_partitions[hs->partition_slot].namespaces[hs->ns_slot];
    fake_kv_key_slot_t *k = find_key(ns, key, true);
    if (!k) return HAL_NO_MEM; /* namespace's key table is full */

    k->pending_set = true;
    k->pending_tombstone = false;
    k->pending_is_str = is_str;
    k->pending_len = len;
    if (len) memcpy(k->pending_data, buf, len);
    return HAL_OK;
}

hal_status_t hal_kv_set_blob(hal_kv_handle_t *h, const char *key, const void *buf, size_t len)
{
    return do_set(get_handle(h), key, false, buf, len);
}

hal_status_t hal_kv_set_str(hal_kv_handle_t *h, const char *key, const char *value)
{
    if (value == NULL) return HAL_INVALID_ARG;
    return do_set(get_handle(h), key, true, value, strlen(value) + 1);
}

hal_status_t hal_kv_get_u32(hal_kv_handle_t *h, const char *key, uint32_t *out)
{
    if (out == NULL) return HAL_INVALID_ARG;
    size_t len = sizeof(*out);
    /* Stored as a plain 4-byte blob -- same underlying key-slot storage as
     * hal_kv_get_blob(), just fixed-size. A key whose stored value is a
     * different size (written via set_blob/set_str with other content, or
     * genuinely corrupt) is caught by do_get()'s own `*out_len < len` check
     * (HAL_INVALID_SIZE) when the stored value is larger than 4 bytes; the
     * length re-check below additionally rejects a stored value SMALLER
     * than 4 bytes, which do_get() would otherwise report as a successful
     * short read. */
    hal_status_t err = do_get(get_handle(h), key, false, out, &len);
    if (err == HAL_OK && len != sizeof(*out)) return HAL_NOT_FOUND; /* wrong type: NOT_FOUND on target */
    return err;
}

hal_status_t hal_kv_set_u32(hal_kv_handle_t *h, const char *key, uint32_t value)
{
    return do_set(get_handle(h), key, false, &value, sizeof(value));
}

hal_status_t hal_kv_get_u8(hal_kv_handle_t *h, const char *key, uint8_t *out)
{
    if (out == NULL) return HAL_INVALID_ARG;
    size_t len = sizeof(*out);
    hal_status_t err = do_get(get_handle(h), key, false, out, &len);
    if (err == HAL_OK && len != sizeof(*out)) return HAL_NOT_FOUND; /* wrong type: NOT_FOUND on target */
    return err;
}

hal_status_t hal_kv_set_u8(hal_kv_handle_t *h, const char *key, uint8_t value)
{
    return do_set(get_handle(h), key, false, &value, sizeof(value));
}

hal_status_t hal_kv_key_exists(hal_kv_handle_t *h, const char *key)
{
    fake_kv_handle_slot_t *hs = get_handle(h);
    if (!hs) return HAL_NOT_READY;
    if (key == NULL) return HAL_INVALID_ARG;
    if (strlen(key) >= FAKE_KV_MAX_KEY_LEN) return HAL_IO;
    fake_kv_namespace_t *ns = &s_partitions[hs->partition_slot].namespaces[hs->ns_slot];
    fake_kv_key_slot_t *k = find_key(ns, key, false);
    return (k && key_logically_present(k)) ? HAL_OK : HAL_NOT_FOUND;
}

hal_status_t hal_kv_erase_key(hal_kv_handle_t *h, const char *key)
{
    fake_kv_handle_slot_t *hs = get_handle(h);
    if (!hs) return HAL_NOT_READY;
    if (hs->mode != HAL_KV_MODE_READ_WRITE) return HAL_INVALID_ARG;
    if (key == NULL) return HAL_INVALID_ARG;
    if (strlen(key) >= FAKE_KV_MAX_KEY_LEN) return HAL_IO; /* see do_get()'s comment */

    if (write_refused(hs)) return HAL_NOT_READY;

    if (take_write_fail()) {
        s_next_write_fail_armed = false;
        return s_next_write_fail_status;
    }

    if (s_silent_erase_noops > 0u) {
        /* The lie: report success, change nothing. See
         * fake_kv_script_silent_erase_noops()'s header comment. */
        s_silent_erase_noops--;
        return HAL_OK;
    }

    fake_kv_namespace_t *ns = &s_partitions[hs->partition_slot].namespaces[hs->ns_slot];
    fake_kv_key_slot_t *k = find_key(ns, key, false);
    if (!k || !key_logically_present(k)) return HAL_NOT_FOUND;

    k->pending_set = true;
    k->pending_tombstone = true;
    k->pending_len = 0;
    return HAL_OK;
}

hal_status_t hal_kv_init_partition(const char *partition)
{
    const char *name = norm_partition(partition);
    if (strlen(name) >= FAKE_KV_MAX_NAME_LEN) return HAL_INVALID_ARG;

    int existing = find_partition_slot(partition);
    if (existing >= 0) return HAL_OK; /* idempotent */

    int free_slot = -1;
    for (int i = 0; i < FAKE_KV_MAX_PARTITIONS; i++) {
        if (!s_partitions[i].initialized) { free_slot = i; break; }
    }
    if (free_slot < 0) return HAL_NO_MEM;

    memset(&s_partitions[free_slot], 0, sizeof(s_partitions[free_slot]));
    s_partitions[free_slot].initialized = true;
    copy_bounded(s_partitions[free_slot].name, sizeof(s_partitions[free_slot].name), name);
    return HAL_OK;
}

hal_status_t hal_kv_mount_probe(const char *partition)
{
    /* The fake has no NO_FREE_PAGES/NEW_VERSION_FOUND concept -- an in-RAM
     * partition is either initialized (mounted) or not, with nothing
     * in-between an erase could fix. Mirrors hal_kv_init_partition()'s own
     * idempotent-if-initialized check, but never initializes: matches
     * hal_kv_esp.c's hal_kv_mount_probe(), which never erases either. */
    if (strlen(norm_partition(partition)) >= FAKE_KV_MAX_NAME_LEN) return HAL_INVALID_ARG;
    return (find_partition_slot(partition) >= 0) ? HAL_OK : HAL_NOT_READY;
}

hal_status_t hal_kv_erase_partition(const char *partition)
{
    int p = find_partition_slot(partition);
    if (p < 0) return HAL_NOT_FOUND;
    char saved_name[FAKE_KV_MAX_NAME_LEN];
    memcpy(saved_name, s_partitions[p].name, sizeof(saved_name));
    memset(&s_partitions[p], 0, sizeof(s_partitions[p]));
    s_partitions[p].initialized = true; /* erase, not de-init -- open() still works after */
    memcpy(s_partitions[p].name, saved_name, sizeof(saved_name));
    return HAL_OK;
}

hal_status_t hal_kv_stats(const char *partition, hal_kv_stats_t *out)
{
    if (out == NULL) return HAL_INVALID_ARG;
    int p = find_partition_slot(partition);
    if (p < 0) return HAL_NOT_FOUND;

    size_t used = 0;
    const size_t total = (size_t)FAKE_KV_MAX_NAMESPACES_PER_PART * FAKE_KV_MAX_KEYS_PER_NS;
    fake_kv_partition_t *part = &s_partitions[p];
    for (int n = 0; n < FAKE_KV_MAX_NAMESPACES_PER_PART; n++) {
        if (!part->namespaces[n].in_use) continue;
        for (int k = 0; k < FAKE_KV_MAX_KEYS_PER_NS; k++) {
            if (part->namespaces[n].keys[k].in_use && key_logically_present(&part->namespaces[n].keys[k])) {
                used++;
            }
        }
    }
    out->used_entries = used;
    out->total_entries = total;
    out->free_entries = total - used;
    return HAL_OK;
}

bool hal_kv_write_safe_here(void)
{
    return s_write_safe_here;
}

void fake_kv_script_write_status_after(unsigned skip, hal_status_t status)
{
    s_next_write_fail_armed = true;
    s_next_write_fail_skip = skip;
    s_next_write_fail_status = status;
}
