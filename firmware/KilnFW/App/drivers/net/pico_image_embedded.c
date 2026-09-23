#include "pico_image_embedded.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "pico_img_emb";

/* Symbol names ESP-IDF's EMBED_FILES generates from a source file name: dots
 * become underscores, and the file's basename (with its extension) is
 * sandwiched between "_binary_" and "_start"/"_end". App/drivers/CMakeLists.txt
 * embeds firmware/SaftyFW/build/SaftyFW_slotA.bin and .../SaftyFW_slotB.bin
 * under exactly those basenames -- see that file's own comment for the path
 * resolution and the FATAL_ERROR guard if either is missing at configure
 * time. */
/* MSVC (the host-test toolchain) has no equivalent of GCC's asm("symbol")
 * renaming. Per the established convention (test_zones_http.c and friends),
 * the host test file that #includes this source `#define`s asm(x) away to
 * nothing around the #include, then supplies a real 1-byte placeholder
 * definition of each symbol below the matching #undef -- see
 * test/test_pico_image_embedded.c. Host tests exercise
 * pico_image_embedded_describe_from() directly with synthetic buffers (see
 * its own doc comment), so pico_image_embedded_describe() itself never needs
 * to link against real content on the host -- only against the 1-byte
 * placeholders, to satisfy the linker. */
extern const uint8_t SaftyFW_slotA_bin_start[] asm("_binary_SaftyFW_slotA_bin_start");
extern const uint8_t SaftyFW_slotA_bin_end[] asm("_binary_SaftyFW_slotA_bin_end");
extern const uint8_t SaftyFW_slotB_bin_start[] asm("_binary_SaftyFW_slotB_bin_start");
extern const uint8_t SaftyFW_slotB_bin_end[] asm("_binary_SaftyFW_slotB_bin_end");

static void set_reason(pico_image_embedded_info_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(out->reason, sizeof(out->reason), fmt, ap);
    va_end(ap);
}

static bool idents_agree(const saftyfw_image_identity_t *a, const saftyfw_image_identity_t *b)
{
    return a->dirty == b->dirty && a->commit_len == b->commit_len
           && a->config_format_version == b->config_format_version
           && a->link_protocol_version == b->link_protocol_version
           && memcmp(a->commit, b->commit, a->commit_len) == 0;
}

bool pico_image_embedded_describe(pico_image_embedded_info_t *out)
{
    uint32_t len_a = (uint32_t)(SaftyFW_slotA_bin_end - SaftyFW_slotA_bin_start);
    uint32_t len_b = (uint32_t)(SaftyFW_slotB_bin_end - SaftyFW_slotB_bin_start);
    return pico_image_embedded_describe_from(SaftyFW_slotA_bin_start, len_a, SaftyFW_slotB_bin_start,
                                             len_b, out);
}

bool pico_image_embedded_describe_from(const uint8_t *slot_a, uint32_t slot_a_len,
                                       const uint8_t *slot_b, uint32_t slot_b_len,
                                       pico_image_embedded_info_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    out->slot_data[0] = slot_a;
    out->slot_len[0] = slot_a_len;
    out->slot_data[1] = slot_b;
    out->slot_len[1] = slot_b_len;

    saftyfw_image_identity_t ident_a;
    saftyfw_image_identity_t ident_b;
    out->slot_found[0] = saftyfw_image_identity_find(out->slot_data[0], out->slot_len[0], &ident_a);
    out->slot_found[1] = saftyfw_image_identity_find(out->slot_data[1], out->slot_len[1], &ident_b);

    if (!out->slot_found[0] || !out->slot_found[1]) {
        set_reason(out, "embedded slot %s carries no build-identity record",
                   !out->slot_found[0] ? "A" : "B");
        ESP_LOGE(TAG, "%s -- automatic Pico update cannot use this build's embedded images",
                 out->reason);
        return true;
    }

    if (!idents_agree(&ident_a, &ident_b)) {
        char commit_a[SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX + 1u];
        char commit_b[SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX + 1u];
        memcpy(commit_a, ident_a.commit, ident_a.commit_len);
        commit_a[ident_a.commit_len] = '\0';
        memcpy(commit_b, ident_b.commit, ident_b.commit_len);
        commit_b[ident_b.commit_len] = '\0';
        set_reason(out, "embedded slot A (%s%s) and slot B (%s%s) disagree -- not the same SaftyFW build",
                   commit_a, ident_a.dirty ? "+dirty" : "", commit_b, ident_b.dirty ? "+dirty" : "");
        ESP_LOGE(TAG, "%s -- refusing to use either as an update source (docs/PICO_AUTO_UPDATE_PLAN.md "
                      "sec 9 step 1's fleet-wide-refusal trap: a WRONG expected identity is worse than none)",
                 out->reason);
        return true;
    }

    memcpy(out->commit, ident_a.commit, ident_a.commit_len);
    out->commit[ident_a.commit_len] = '\0';
    out->commit_len = ident_a.commit_len;
    out->dirty = (ident_a.dirty != 0u);
    out->config_format_version = ident_a.config_format_version;
    out->link_protocol_version = ident_a.link_protocol_version;
    out->usable = true;
    ESP_LOGI(TAG, "embedded SaftyFW images usable: slot A %lu B, slot B %lu B, commit %s%s, "
                  "config format v%u, link protocol v%u",
             (unsigned long)out->slot_len[0], (unsigned long)out->slot_len[1], out->commit,
             out->dirty ? " (dirty)" : "", (unsigned)out->config_format_version,
             (unsigned)out->link_protocol_version);
    return true;
}
