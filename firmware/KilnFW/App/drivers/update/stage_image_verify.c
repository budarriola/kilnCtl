// RX-11: see stage_image_verify.h for why this exists instead of esp_image_verify() over the stage.
// Mirrors IDF v6.0.2 esp_image_format.c image_load() for a non-secure-boot app in verify mode.
#include "stage_image_verify.h"

#include <stdbool.h>
#include <string.h>

#define IMG_HDR_LEN 24u
#define SEG_HDR_LEN 8u
#define HASH_LEN 32u
#define CHUNK 512u
#define IMAGE_MAGIC 0xE9u
#define CHECKSUM_INITIAL 0xEFu

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// IDF should_map(): IROM / DROM ranges on the ESP32-S3 (soc.h SOC_IROM_*, SOC_DROM_*).
static bool should_map(uint32_t a)
{
    return (a >= 0x42000000u && a < 0x44000000u) || (a >= 0x3C000000u && a < 0x3E000000u);
}

// Reads [off, off+len), feeds the hash; when xor != NULL folds the bytes into it.
static int feed(const stage_image_io_t *io, uint32_t off, uint32_t len, uint8_t *xor_acc, uint8_t *last)
{
    uint8_t buf[CHUNK];
    while (len > 0) {
        uint32_t n = len < CHUNK ? len : CHUNK;
        if (io->read(io->ctx, off, buf, n) != 0 || io->sha_update(io->ctx, buf, n) != 0) {
            return -1;
        }
        if (xor_acc != NULL) {
            for (uint32_t i = 0; i < n; i++) {
                *xor_acc ^= buf[i];
            }
        }
        if (last != NULL) {
            *last = buf[n - 1];
        }
        off += n;
        len -= n;
    }
    return 0;
}

static stage_image_result_t walk(const stage_image_io_t *io, uint32_t avail, uint32_t *out_len, bool *finished, int chip_check)
{
    uint8_t hdr[IMG_HDR_LEN];
    if (avail < IMG_HDR_LEN + HASH_LEN + 16u) {
        return STAGE_IMG_TOO_SHORT;
    }
    if (io->read(io->ctx, 0, hdr, IMG_HDR_LEN) != 0) {
        return STAGE_IMG_IO;
    }
    const uint32_t nseg = hdr[1];
    const uint32_t chip = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8);
    if (hdr[0] != IMAGE_MAGIC || nseg == 0 || nseg > STAGE_IMG_MAX_SEGMENTS || chip != STAGE_IMG_CHIP_ID_ESP32S3) {
        return STAGE_IMG_BAD_HEADER;
    }
    if (chip_check && io->chip_ok(io->ctx, hdr) != 0) {
        return STAGE_IMG_BAD_CHIP_REV; // before any erase: bootloader and cb_app_verify only run after it
    }
    if (hdr[23] != 1u) {
        return STAGE_IMG_NO_HASH; // IDF app builds always append it; require it for corruption detection
    }
    if (io->sha_update(io->ctx, hdr, IMG_HDR_LEN) != 0) {
        return STAGE_IMG_IO;
    }
    uint32_t off = IMG_HDR_LEN;
    uint8_t x = CHECKSUM_INITIAL;
    for (uint32_t i = 0; i < nseg; i++) {
        uint8_t sh[SEG_HDR_LEN];
        if (off > avail - SEG_HDR_LEN) {
            return STAGE_IMG_TOO_SHORT;
        }
        if (io->read(io->ctx, off, sh, SEG_HDR_LEN) != 0 || io->sha_update(io->ctx, sh, SEG_HDR_LEN) != 0) {
            return STAGE_IMG_IO;
        }
        const uint32_t load = rd32(sh);
        const uint32_t dlen = rd32(sh + 4);
        off += SEG_HDR_LEN;
        if ((dlen & 3u) != 0 || dlen >= 0x1000000u) {
            return STAGE_IMG_BAD_SEGMENT;
        }
        // IDF verify_segment_header(), but against where the image RUNS: the 64 KB aligned app
        // partition, so the image-relative offset is the one that matters.
        if (should_map(load) && (off % STAGE_IMG_MMU_PAGE) != (load % STAGE_IMG_MMU_PAGE)) {
            return STAGE_IMG_MISALIGNED;
        }
        if (dlen > avail - off) {
            return STAGE_IMG_TOO_SHORT;
        }
        if (feed(io, off, dlen, &x, NULL) != 0) {
            return STAGE_IMG_IO;
        }
        off += dlen;
    }
    // IDF process_checksum(): pad so the checksum byte ends a 16-byte block.
    const uint32_t pad = ((off + 1u + 15u) & ~15u) - off;
    if (pad > avail - off) {
        return STAGE_IMG_TOO_SHORT;
    }
    uint8_t ck = 0;
    if (feed(io, off, pad, NULL, &ck) != 0) {
        return STAGE_IMG_IO;
    }
    off += pad;
    if (ck != x) {
        return STAGE_IMG_BAD_CHECKSUM;
    }
    if (HASH_LEN > avail - off) {
        return STAGE_IMG_TOO_SHORT;
    }
    uint8_t want[HASH_LEN];
    uint8_t got[HASH_LEN];
    if (io->read(io->ctx, off, want, HASH_LEN) != 0) {
        return STAGE_IMG_IO;
    }
    *finished = true;
    if (io->sha_finish(io->ctx, got) != 0) {
        return STAGE_IMG_IO;
    }
    if (memcmp(want, got, HASH_LEN) != 0) {
        return STAGE_IMG_BAD_HASH;
    }
    if (out_len != NULL) {
        *out_len = off + HASH_LEN;
    }
    return STAGE_IMG_OK;
}

stage_image_result_t stage_image_verify(const stage_image_io_t *io, uint32_t avail, uint32_t *out_len)
{
    return stage_image_verify_opt(io, avail, out_len, STAGE_IMG_CHIP_REV_CHECK);
}

stage_image_result_t stage_image_verify_opt(const stage_image_io_t *io, uint32_t avail, uint32_t *out_len, int chip_check)
{
    if (io == NULL || io->read == NULL || io->sha_begin == NULL || io->sha_update == NULL || io->sha_finish == NULL || io->chip_ok == NULL) {
        return STAGE_IMG_IO;
    }
    if (io->sha_begin(io->ctx) != 0) {
        return STAGE_IMG_IO;
    }
    bool finished = false;
    stage_image_result_t r = walk(io, avail, out_len, &finished, chip_check);
    if (!finished) {
        uint8_t discard[HASH_LEN];
        (void)io->sha_finish(io->ctx, discard); // release the hash on every failure path
    }
    return r;
}
