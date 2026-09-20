#include "pico_img_stage.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ota_image_crc.h"
#include "ota_pico_relay.h" /* ota_pico_img_partition() */
#include "pico_image_manifest.h"

static const char *TAG = "pico_img_stage";

static void set_fail(char *buf, size_t len, const char *fmt, ...)
{
    if (buf == NULL || len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

bool pico_img_stage_begin(pico_img_stage_ctx_t *ctx, size_t content_len, char *fail_reason,
                          size_t fail_reason_len)
{
    if (ctx == NULL) {
        return false;
    }
    memset(ctx, 0, sizeof(*ctx));

    const esp_partition_t *part = ota_pico_img_partition();
    if (part == NULL) {
        set_fail(fail_reason, fail_reason_len, "pico_img staging partition not found");
        ESP_LOGE(TAG, "pico_img staging partition not found");
        return false;
    }
    if (content_len > part->size) {
        set_fail(fail_reason, fail_reason_len, "image (%u B) larger than the pico_img partition (%u B)",
                 (unsigned)content_len, (unsigned)part->size);
        ESP_LOGE(TAG, "image (%u B) larger than pico_img (%u B)", (unsigned)content_len,
                 (unsigned)part->size);
        return false;
    }

    uint32_t sector = esp_partition_get_main_flash_sector_size();
    size_t erase_len = ((content_len + sector - 1u) / sector) * sector;
    esp_err_t erc = esp_partition_erase_range(part, 0, erase_len);
    if (erc != ESP_OK) {
        set_fail(fail_reason, fail_reason_len, "pico_img erase failed: %s", esp_err_to_name(erc));
        ESP_LOGE(TAG, "pico_img erase failed: %s", esp_err_to_name(erc));
        return false;
    }

    ctx->part = part;
    ctx->crc = OTA_IMAGE_CRC32_INIT;
    ctx->written = 0;
    ctx->total_len = content_len;
    return true;
}

bool pico_img_stage_write_chunk(pico_img_stage_ctx_t *ctx, const uint8_t *data, size_t len,
                                char *fail_reason, size_t fail_reason_len)
{
    if (ctx == NULL || ctx->part == NULL || (data == NULL && len > 0)) {
        set_fail(fail_reason, fail_reason_len, "pico_img_stage_write_chunk: invalid argument");
        return false;
    }
    esp_err_t werr = esp_partition_write(ctx->part, ctx->written, data, len);
    if (werr != ESP_OK) {
        set_fail(fail_reason, fail_reason_len, "pico_img write failed at %u bytes: %s",
                 (unsigned)ctx->written, esp_err_to_name(werr));
        ESP_LOGE(TAG, "pico_img write failed at %u bytes: %s", (unsigned)ctx->written,
                 esp_err_to_name(werr));
        return false;
    }
    ctx->crc = ota_image_crc32_update(ctx->crc, data, len);
    ctx->written += len;
    return true;
}

bool pico_img_stage_finish(pico_img_stage_ctx_t *ctx, uint32_t *out_crc)
{
    if (ctx == NULL) {
        return false;
    }
    if (out_crc != NULL) {
        *out_crc = ctx->crc;
    }
    /* Written BEFORE the caller starts the relay -- same reasoning as
     * ota_http_pico.c's original inline version: if the relay attempt that
     * follows fails, the staged image is still good and a LATER boot should
     * be able to retry with it (or an operator should be able to see what
     * was staged). Non-fatal to the caller: a false return means the bytes
     * are staged and usable this boot, but a later boot cannot re-discover
     * them via the manifest path -- pico_image_manifest_store() already logs
     * loudly on its own. */
    return pico_image_manifest_store((uint32_t)ctx->written, ctx->crc);
}
