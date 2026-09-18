#include "pico_image_source.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

#include "ota_image_crc.h"
#include "ota_pico_relay.h" /* ota_pico_img_partition() */
#include "pico_image_manifest.h"

static const char *TAG = "pico_img_src";

/* Both multiples of 4, which is what keeps every scan window's start at a
 * 4-byte-aligned IMAGE offset -- saftyfw_image_identity_find()'s documented
 * precondition. The chunk is modest on purpose: this buffer is static (see
 * the header's note on why it cannot be a local), and 1 KB is plenty to
 * amortise esp_partition_read() over a ~200 KB image. */
#define SCAN_CHUNK 1024u
#define SCAN_CARRY SAFTYFW_IMAGE_IDENTITY_SCAN_OVERLAP /* 56 */

typedef char pico_image_source_align_check
    [((SCAN_CHUNK % 4u) == 0u && (SCAN_CARRY % 4u) == 0u) ? 1 : -1];

/* Carry region first, then the freshly-read chunk: the scan window is the
 * tail of the previous chunk followed by this one, so a record straddling a
 * chunk boundary is still seen whole exactly once. */
static uint8_t s_scan_buf[SCAN_CARRY + SCAN_CHUNK];

static void set_reason(pico_image_source_info_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(out->reason, sizeof(out->reason), fmt, ap);
    va_end(ap);
}

bool pico_image_source_describe(pico_image_source_info_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    uint32_t length = 0;
    uint32_t expect_crc = 0;
    if (!pico_image_manifest_load(&length, &expect_crc)) {
        /* No image was ever staged on this board. Not a fault, and
         * deliberately NOT a reason string: there is nothing wrong to
         * report. */
        return false;
    }
    out->manifest_present = true;
    out->image_length = length;
    out->image_crc32 = expect_crc;

    const esp_partition_t *part = ota_pico_img_partition();
    if (part == NULL) {
        set_reason(out, "no pico_img partition on this board");
        ESP_LOGE(TAG, "%s", out->reason);
        return true;
    }
    if (length > part->size) {
        set_reason(out, "manifest length %lu exceeds pico_img (%lu)", (unsigned long)length,
                   (unsigned long)part->size);
        ESP_LOGE(TAG, "%s", out->reason);
        return true;
    }

    /* One pass: CRC over the image's real byte stream (each byte exactly
     * once, chunk boundaries irrelevant -- ota_image_crc.h's contract) while
     * scanning an overlapping window for the identity record. */
    uint32_t crc = OTA_IMAGE_CRC32_INIT;
    saftyfw_image_identity_t ident;
    bool found = false;
    size_t carry = 0; /* bytes of the previous chunk currently at s_scan_buf[0..carry) */
    uint32_t pos = 0;

    while (pos < length) {
        size_t want = length - pos;
        if (want > SCAN_CHUNK) {
            want = SCAN_CHUNK;
        }
        esp_err_t err = esp_partition_read(part, pos, s_scan_buf + carry, want);
        if (err != ESP_OK) {
            set_reason(out, "pico_img read failed at %lu: %s", (unsigned long)pos,
                       esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", out->reason);
            return true;
        }
        crc = ota_image_crc32_update(crc, s_scan_buf + carry, want);
        if (!found) {
            found = saftyfw_image_identity_find(s_scan_buf, carry + want, &ident);
        }
        pos += (uint32_t)want;

        /* Keep the tail for the next window. A chunk shorter than the carry
         * (only possible on a tiny image) simply carries everything it has;
         * memmove, not memcpy -- the regions overlap whenever
         * carry + want < 2 * SCAN_CARRY. */
        size_t have = carry + want;
        size_t keep = (have < SCAN_CARRY) ? have : SCAN_CARRY;
        memmove(s_scan_buf, s_scan_buf + (have - keep), keep);
        carry = keep;
    }

    if (crc != expect_crc) {
        set_reason(out, "staged image CRC 0x%08lx != recorded 0x%08lx",
                   (unsigned long)crc, (unsigned long)expect_crc);
        ESP_LOGW(TAG, "%s -- the pico_img partition no longer holds the image that was staged",
                 out->reason);
        return true;
    }
    if (!found) {
        /* An image built before this record existed, or one built by a
         * different project. Refusing it is the only safe answer: without an
         * identity there is no way to tell whether pushing it would be an
         * upgrade, a downgrade, or a no-op, and a needless Pico reflash costs
         * a window with the safety processor rebooting. */
        set_reason(out, "staged image carries no build-identity record");
        ESP_LOGW(TAG, "%s -- it cannot be used for an automatic update", out->reason);
        return true;
    }

    memcpy(out->commit, ident.commit, ident.commit_len);
    out->commit[ident.commit_len] = '\0';
    out->commit_len = ident.commit_len;
    out->dirty = (ident.dirty != 0u);
    out->config_format_version = ident.config_format_version;
    out->usable = true;
    ESP_LOGI(TAG, "staged SaftyFW image usable: %lu bytes, commit %s%s, config format v%u",
             (unsigned long)length, out->commit, out->dirty ? " (dirty)" : "",
             (unsigned)out->config_format_version);
    return true;
}
