#include "cfg_fs_format_gate.h"

#include <stdio.h>
#include <string.h>

static const uint8_t kLittleFsMagic[8] = { 'l', 'i', 't', 't', 'l', 'e', 'f', 's' };

void cfg_fs_format_gate_reset(cfg_fs_format_gate_t *gate)
{
    if (!gate) {
        return;
    }
    memset(gate, 0, sizeof(*gate));
}

/* Scans `buf[0..len)` (which may include up to 7 carried-over bytes from the
 * previous chunk, prepended by the caller into a small scratch array) for
 * the LittleFS magic. Kept separate from cfg_fs_format_gate_feed() only for
 * clarity. */
static bool contains_magic(const uint8_t *buf, size_t len)
{
    if (len < sizeof(kLittleFsMagic)) {
        return false;
    }
    for (size_t i = 0; i + sizeof(kLittleFsMagic) <= len; i++) {
        if (memcmp(buf + i, kLittleFsMagic, sizeof(kLittleFsMagic)) == 0) {
            return true;
        }
    }
    return false;
}

void cfg_fs_format_gate_feed(cfg_fs_format_gate_t *gate, const uint8_t *chunk, size_t len)
{
    if (!gate || !chunk || len == 0) {
        return;
    }

    /* Stitch the boundary: previous chunk's trailing <=7 bytes plus this
     * chunk's leading bytes, so a magic string split across two feed() calls
     * is still found. */
    if (gate->tail_len > 0) {
        uint8_t stitched[sizeof(gate->tail) + sizeof(kLittleFsMagic) - 1];
        size_t lead = len < (sizeof(kLittleFsMagic) - 1) ? len : (sizeof(kLittleFsMagic) - 1);
        memcpy(stitched, gate->tail, gate->tail_len);
        memcpy(stitched + gate->tail_len, chunk, lead);
        if (contains_magic(stitched, gate->tail_len + lead)) {
            gate->magic_found = true;
        }
    }
    if (!gate->magic_found && contains_magic(chunk, len)) {
        gate->magic_found = true;
    }

    for (size_t i = 0; i < len; i++) {
        if (chunk[i] != 0xFF) {
            gate->nonerased_bytes++;
        }
    }
    gate->total_bytes += len;

    size_t carry = len < sizeof(gate->tail) ? len : sizeof(gate->tail);
    memcpy(gate->tail, chunk + (len - carry), carry);
    gate->tail_len = carry;
}

cfg_fs_format_gate_verdict_t cfg_fs_format_gate_conclude(const cfg_fs_format_gate_t *gate)
{
    if (!gate || gate->total_bytes == 0) {
        /* Nothing was scanned at all -- refuse rather than guess "blank".
         * A caller that failed to feed any bytes (e.g. the partition could
         * not be read back for some reason) must not have that silently
         * treated as evidence of nothing being there. */
        return CFG_FS_FORMAT_GATE_HAS_CONTENT;
    }
    if (gate->magic_found) {
        return CFG_FS_FORMAT_GATE_HAS_CONTENT;
    }
    /* nonerased_bytes * 1000 / total_bytes, without overflowing size_t for a
     * 512 KiB partition (well within range, but written defensively). */
    uint64_t per_mille = ((uint64_t)gate->nonerased_bytes * 1000ULL) / (uint64_t)gate->total_bytes;
    if (per_mille > CFG_FS_FORMAT_GATE_NONERASED_PER_MILLE_THRESHOLD) {
        return CFG_FS_FORMAT_GATE_HAS_CONTENT;
    }
    return CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT;
}

void cfg_fs_format_gate_describe(const cfg_fs_format_gate_t *gate, cfg_fs_format_gate_verdict_t verdict,
                                  char *buf, size_t buf_cap)
{
    if (!buf || buf_cap == 0) {
        return;
    }
    if (verdict == CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT) {
        snprintf(buf, buf_cap, "no evidence of existing data (region reads as erased)");
        return;
    }
    if (!gate || gate->total_bytes == 0) {
        snprintf(buf, buf_cap, "partition could not be scanned");
        return;
    }
    if (gate->magic_found) {
        snprintf(buf, buf_cap, "LittleFS superblock signature found");
        return;
    }
    /* Report to one decimal place without pulling in floating-point
     * formatting guarantees -- integer tenths-of-a-percent math only. */
    uint64_t per_mille = ((uint64_t)gate->nonerased_bytes * 1000ULL) / (uint64_t)gate->total_bytes;
    snprintf(buf, buf_cap, "%u.%u%% of the partition is non-erased data",
             (unsigned)(per_mille / 10), (unsigned)(per_mille % 10));
}
