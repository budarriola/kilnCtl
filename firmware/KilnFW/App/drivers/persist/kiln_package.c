#include "kiln_package.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crc.h" /* esp_crc32_le() -- same primitive zones_config_store.c/crash_report.c
                       * already use for their own CRC32s (host-testable via
                       * test/stubs/esp_crc.h, a real CRC32 not a fake -- see that stub's
                       * header comment). */

/* Reinterprets `value` (tagged by `type`) as a plain uint32_t bit pattern --
 * never a numeric conversion, always a byte-for-byte reinterpretation via a
 * union, so a float's IEEE-754 bits round-trip exactly and a bool/u8/u16
 * value_bits comparison is bit-for-bit too (plan section 3.2's "compare f32
 * params bit-for-bit on the stored representation, not with an epsilon" --
 * this is what makes that comparison possible one layer up). Unknown `type`
 * (should be unreachable -- safety_cfg_store.c's own table only ever reports
 * one of the four KILNLINK_PARAM_TYPE_* tags) reads as 0. */
static uint32_t value_to_bits(uint8_t type, const kilnlink_param_value_t *value)
{
    union {
        float f;
        uint32_t bits;
    } conv;
    switch (type) {
    case KILNLINK_PARAM_TYPE_BOOL:
        return (uint32_t)value->bool_val;
    case KILNLINK_PARAM_TYPE_U8:
        return (uint32_t)value->u8_val;
    case KILNLINK_PARAM_TYPE_U16:
        return (uint32_t)value->u16_val;
    case KILNLINK_PARAM_TYPE_F32:
        conv.f = value->f32_val;
        return conv.bits;
    default:
        return 0;
    }
}

kiln_pkg_pico_source_t kiln_pkg_pico_source_default(void)
{
    kiln_pkg_pico_source_t src;
    src.param_count = safety_cfg_store_param_count;
    src.get_by_index = safety_cfg_store_get_by_index;
    return src;
}

bool kiln_package_capture_pico_half(const kiln_pkg_pico_source_t *source, kiln_pkg_safety_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t n = source->param_count();
    if (n > KILN_PKG_SAFETY_PARAM_CAP) {
        /* Never truncate -- section 6's "no partial write" rule generalises
         * to capture, not only to apply. A caller must bump
         * KILN_PKG_SAFETY_PARAM_CAP (and, in practice, KILN_CFG_STORE_VERSION
         * alongside it) before this can succeed again. */
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        safety_cfg_param_t row;
        if (!source->get_by_index(i, &row)) {
            /* Table says `n` rows exist but this one could not be read --
             * treat exactly like "too many to fit": refuse the whole
             * capture rather than package a hole. */
            memset(out, 0, sizeof(*out));
            return false;
        }
        kiln_pkg_pico_param_t *dst = &out->entries[i];
        dst->param_id = row.param_id;
        dst->type = row.type;
        dst->flags = row.set ? KILN_PKG_PARAM_FLAG_SET : 0;
        dst->value_bits = row.set ? value_to_bits(row.type, &row.value) : 0;
    }
    out->count = (uint16_t)n;

    /* Ascending-param_id sort (plan section 3.1.3 rule 2) -- CONFIG_PARAM_
     * TABLE's own iteration order is editorial, not numeric (config_params.c's
     * 0x0211 comment). Insertion sort: at most 96 elements, run once per
     * save, never a hot path -- clarity over cleverness here. */
    for (size_t i = 1; i < out->count; i++) {
        kiln_pkg_pico_param_t key = out->entries[i];
        size_t j = i;
        while (j > 0 && out->entries[j - 1].param_id > key.param_id) {
            out->entries[j] = out->entries[j - 1];
            j--;
        }
        out->entries[j] = key;
    }
    return true;
}

/* Scratch buffer ceiling for kiln_package_compute_hash()'s canonical
 * serialization: 2 (pkg_schema) + 2 (esp_blob_len) + ZONES_CONFIG_BLOB_MAX_
 * SIZE-worst-case (bounded by the uint16_t param itself, so use its full
 * range ceiling actually enforced by the caller) + 2 (pico count) +
 * KILN_PKG_SAFETY_PARAM_CAP * 8 (each packed entry). Sized generously
 * against today's real ceilings (896 + 96*8 = 1664) with headroom, and
 * heap-allocated (never a stack buffer -- this runs from kiln_cfg_store.c's
 * save/apply paths, which the httpd worker can reach, and this codebase's
 * standing rule is "no stack buffer grows for this feature",
 * project_httpd_stack_blob_class). */
#define KILN_PKG_HASH_SCRATCH_CAP 4096u

bool kiln_package_compute_hash(uint16_t pkg_schema, const uint8_t *esp_blob, uint16_t esp_blob_len,
                                const kiln_pkg_safety_t *pico, uint32_t *out_hash)
{
    /* H2 fix: *out_hash is written ONLY on success, and never zeroed on
     * failure -- an untouched caller-owned variable is a clearer failure
     * signal than a value this function chose, and the PRIMARY return
     * value (bool) is what callers must check; see this header's own
     * comment for why a discardable uint32_t sentinel was the defect. */
    if (!pico || (esp_blob_len > 0 && !esp_blob) || !out_hash) {
        return false;
    }
    size_t needed = 2u /* pkg_schema */
                    + 2u /* esp_blob_len */
                    + (size_t)esp_blob_len
                    + 2u /* pico->count */
                    + (size_t)pico->count * 8u;
    if (needed > KILN_PKG_HASH_SCRATCH_CAP) {
        return false; /* defensive -- unreachable with today's ceilings; never hash a truncated buffer */
    }

    uint8_t *buf = (uint8_t *)malloc(KILN_PKG_HASH_SCRATCH_CAP);
    if (!buf) {
        return false;
    }
    size_t off = 0;

    buf[off++] = (uint8_t)(pkg_schema & 0xFFu);
    buf[off++] = (uint8_t)((pkg_schema >> 8) & 0xFFu);

    buf[off++] = (uint8_t)(esp_blob_len & 0xFFu);
    buf[off++] = (uint8_t)((esp_blob_len >> 8) & 0xFFu);
    if (esp_blob_len > 0) {
        memcpy(buf + off, esp_blob, esp_blob_len);
        off += esp_blob_len;
    }

    buf[off++] = (uint8_t)(pico->count & 0xFFu);
    buf[off++] = (uint8_t)((pico->count >> 8) & 0xFFu);
    for (uint16_t i = 0; i < pico->count; i++) {
        const kiln_pkg_pico_param_t *e = &pico->entries[i];
        buf[off++] = (uint8_t)(e->param_id & 0xFFu);
        buf[off++] = (uint8_t)((e->param_id >> 8) & 0xFFu);
        buf[off++] = e->type;
        buf[off++] = e->flags;
        buf[off++] = (uint8_t)(e->value_bits & 0xFFu);
        buf[off++] = (uint8_t)((e->value_bits >> 8) & 0xFFu);
        buf[off++] = (uint8_t)((e->value_bits >> 16) & 0xFFu);
        buf[off++] = (uint8_t)((e->value_bits >> 24) & 0xFFu);
    }

    uint32_t crc = esp_crc32_le(0, buf, (uint32_t)off);
    free(buf);
    *out_hash = crc;
    return true;
}
