#include "saftyfw_image_identity_record.h"

#include "config_store.h"
#include "saftyfw_build_info.h"

/* Populated entirely from generated/compile-time constants: no runtime
 * initialisation, so the bytes sit in .rodata inside the slot image exactly
 * as the ESP will read them back out of the `pico_img` staging partition.
 *
 * A CRC over the record would be the obvious integrity field, but it cannot
 * be computed in a C initialiser -- hence the magic0/magic1/magic_end
 * triple the shared header describes instead. The image's real integrity
 * check is the end-to-end CRC-32 the update path already performs over the
 * whole image. */
static const char k_commit[] = SAFTYFW_GIT_COMMIT;

typedef char saftyfw_image_identity_commit_fits
    [((sizeof(k_commit) - 1u) <= SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX) ? 1 : -1];

static const saftyfw_image_identity_t k_identity = {
    .magic0 = SAFTYFW_IMAGE_IDENTITY_MAGIC0,
    .magic1 = SAFTYFW_IMAGE_IDENTITY_MAGIC1,
    .record_version = SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION,
    .dirty = (SAFTYFW_GIT_DIRTY) ? 1u : 0u,
    .commit_len = (uint8_t)(sizeof(k_commit) - 1u),
    .commit = SAFTYFW_GIT_COMMIT,
    .config_format_version = (uint16_t)CONFIG_STORE_FORMAT_VERSION,
    .reserved = 0u,
    .magic_end = SAFTYFW_IMAGE_IDENTITY_MAGIC_END,
};

const saftyfw_image_identity_t *saftyfw_image_identity_get(void)
{
    return &k_identity;
}
