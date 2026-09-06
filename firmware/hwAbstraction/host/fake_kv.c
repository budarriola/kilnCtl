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
static bool                   s_lossy_uncommitted = false;

static const char *norm_partition(const char *partition)
{
    return partition ? partition : "";
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

void fake_kv_reset_all(void)
{
    memset(s_partitions, 0, sizeof(s_partitions));
    memset(s_handles, 0, sizeof(s_handles));
    s_write_safe_here = true;
    s_next_write_fail_armed = false;
    s_next_write_fail_status = HAL_OK;
    s_lossy_uncommitted = false;
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
    s_next_write_fail_status = status;
}

void fake_kv_set_write_safe_here(bool safe)
{
    s_write_safe_here = safe;
}

/* --- hal_kv.h implementation --- */

hal_status_t hal_kv_open(hal_kv_handle_t *h, const char *namespace_name,
                          hal_kv_mode_t mode, const char *partition)
{
    if (h == NULL || namespace_name == NULL) return HAL_INVALID_ARG;
    if (strlen(namespace_name) >= FAKE_KV_MAX_NAME_LEN) return HAL_INVALID_ARG;

    int p = find_partition_slot(partition);
    if (p < 0) return HAL_NOT_READY; /* hal_kv_init_partition() not called yet */
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

    if (s_next_write_fail_armed) {
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

    if (want_str && !is_str) return HAL_INVALID_ARG; /* wrong-type injection case */

    if (buf == NULL) { /* size probe */
        *out_len = len;
        return HAL_OK;
    }
    if (*out_len < len) return HAL_INVALID_SIZE;
    if (len) memcpy(buf, data, len);
    *out_len = len;
    return HAL_OK;
}

hal_status_t hal_kv_get_blob(hal_kv_handle_t *h, const char *key, void *buf, size_t *out_len)
{
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

    if (s_next_write_fail_armed) {
        s_next_write_fail_armed = false;
        return s_next_write_fail_status;
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

hal_status_t hal_kv_erase_key(hal_kv_handle_t *h, const char *key)
{
    fake_kv_handle_slot_t *hs = get_handle(h);
    if (!hs) return HAL_NOT_READY;
    if (hs->mode != HAL_KV_MODE_READ_WRITE) return HAL_INVALID_ARG;
    if (key == NULL) return HAL_INVALID_ARG;
    if (strlen(key) >= FAKE_KV_MAX_KEY_LEN) return HAL_IO; /* see do_get()'s comment */

    if (s_next_write_fail_armed) {
        s_next_write_fail_armed = false;
        return s_next_write_fail_status;
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
