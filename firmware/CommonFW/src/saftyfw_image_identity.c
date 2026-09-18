#include "kilnlink/saftyfw_image_identity.h"

#include <string.h>

bool saftyfw_image_identity_is_valid(const saftyfw_image_identity_t *rec)
{
    if (rec == NULL) {
        return false;
    }
    if (rec->magic0 != SAFTYFW_IMAGE_IDENTITY_MAGIC0) {
        return false;
    }
    if (rec->magic1 != SAFTYFW_IMAGE_IDENTITY_MAGIC1) {
        return false;
    }
    if (rec->magic_end != SAFTYFW_IMAGE_IDENTITY_MAGIC_END) {
        return false;
    }
    if (rec->record_version != SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION) {
        return false;
    }
    if (rec->commit_len == 0u || rec->commit_len > SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX) {
        return false;
    }
    if (rec->dirty > 1u) {
        return false;
    }
    return true;
}

bool saftyfw_image_identity_find(const uint8_t *buf, size_t len, saftyfw_image_identity_t *out)
{
    if (buf == NULL || out == NULL || len < SAFTYFW_IMAGE_IDENTITY_SIZE) {
        return false;
    }

    /* Cheap pre-filter on the first magic word before paying for a 60-byte
     * copy at every one of the ~200k aligned offsets in a full slot image.
     * Reassembled byte-by-byte rather than read through a uint32_t* so this
     * stays correct for an unaligned `buf` and free of strict-aliasing
     * assumptions -- the scanner is also host-compiled under MSVC /W4 /WX. */
    const size_t last = len - SAFTYFW_IMAGE_IDENTITY_SIZE;
    for (size_t off = 0; off <= last; off += 4u) {
        uint32_t m0 = (uint32_t)buf[off] | ((uint32_t)buf[off + 1u] << 8) |
                      ((uint32_t)buf[off + 2u] << 16) | ((uint32_t)buf[off + 3u] << 24);
        if (m0 != SAFTYFW_IMAGE_IDENTITY_MAGIC0) {
            continue;
        }
        saftyfw_image_identity_t rec;
        memcpy(&rec, buf + off, sizeof(rec));
        if (!saftyfw_image_identity_is_valid(&rec)) {
            continue;
        }
        *out = rec;
        return true;
    }
    return false;
}
