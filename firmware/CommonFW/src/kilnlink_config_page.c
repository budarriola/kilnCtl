#include "kilnlink/kilnlink_config_page.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per this file's header comment:
 *   0      u8  cmd (0x1F)
 *   1      u8  page_index
 *   2      u8  entry_count
 *   3      u8  more
 *   4..    entries
 */
#define OFF_PAGE_INDEX  1u
#define OFF_ENTRY_COUNT 2u
#define OFF_MORE        3u
#define OFF_ENTRIES     4u

size_t kilnlink_config_page_pack(uint8_t page_index, const kilnlink_config_page_entry_t *entries,
                                  size_t entries_avail, uint8_t *out, size_t out_cap,
                                  size_t *entries_packed, kilnlink_config_page_status_t *status)
{
    kilnlink_config_page_status_t local_status = KILNLINK_CONFIG_PAGE_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_CONFIG_PAGE_OK;

    size_t packed = 0;

    if (out_cap < KILNLINK_CONFIG_PAGE_HDR_LEN) {
        *status = KILNLINK_CONFIG_PAGE_ERR_BUFFER_TOO_SMALL;
        if (entries_packed) {
            *entries_packed = 0;
        }
        return 0;
    }

    size_t off = OFF_ENTRIES;
    while (packed < entries_avail && packed < KILNLINK_CONFIG_PAGE_MAX_ENTRIES) {
        uint8_t type = entries[packed].type;
        size_t vlen = kilnlink_param_value_len(type);
        if (vlen == 0) {
            /* Every entry handed to pack() is assumed to be already-valid
             * data the caller built (e.g. read out of config_store), not
             * untrusted wire input -- so a bad tag here is the caller's bug,
             * and packing stops rather than silently skipping the entry. */
            *status = KILNLINK_CONFIG_PAGE_ERR_BAD_TYPE;
            if (entries_packed) {
                *entries_packed = 0;
            }
            return 0;
        }

        size_t entry_len = KILNLINK_CONFIG_PAGE_ENTRY_HDR_LEN + vlen;
        if (off + entry_len > out_cap) {
            break; /* this entry (and everything after it) waits for the next page */
        }

        kilnlink_put_u16le(out, off, entries[packed].param_id);
        out[off + 2u] = type;
        /* vlen was already derived from `type`, which is already known-good
         * here, so this cannot fail. */
        (void)kilnlink_param_value_encode(type, &entries[packed].value, out, off + 3u);

        off += entry_len;
        packed++;
    }

    out[0] = KILNLINK_CONFIG_PAGE_CMD;
    out[OFF_PAGE_INDEX] = page_index;
    out[OFF_ENTRY_COUNT] = (uint8_t)packed;
    out[OFF_MORE] = (packed < entries_avail) ? 1u : 0u;

    if (entries_packed) {
        *entries_packed = packed;
    }
    return off;
}

kilnlink_config_page_status_t kilnlink_config_page_decode(const uint8_t *payload, size_t len,
                                                            kilnlink_config_page_t *out)
{
    if (len < KILNLINK_CONFIG_PAGE_HDR_LEN) {
        return KILNLINK_CONFIG_PAGE_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_CONFIG_PAGE_CMD) {
        return KILNLINK_CONFIG_PAGE_ERR_WRONG_CMD;
    }

    uint8_t entry_count = payload[OFF_ENTRY_COUNT];
    if (entry_count > KILNLINK_CONFIG_PAGE_MAX_ENTRIES) {
        return KILNLINK_CONFIG_PAGE_ERR_TOO_MANY_ENTRIES;
    }

    size_t off = OFF_ENTRIES;
    for (uint8_t i = 0; i < entry_count; i++) {
        /* Bounds-check the id+type header of this entry BEFORE reading any
         * of it -- a frame that claims more entries than it has room for
         * must be rejected, never read past. */
        if (off + KILNLINK_CONFIG_PAGE_ENTRY_HDR_LEN > len) {
            return KILNLINK_CONFIG_PAGE_ERR_LENGTH_MISMATCH;
        }

        uint16_t param_id = kilnlink_get_u16le(payload, off);
        uint8_t type = payload[off + 2u];
        size_t vlen = kilnlink_param_value_len(type);
        if (vlen == 0) {
            return KILNLINK_CONFIG_PAGE_ERR_BAD_TYPE;
        }

        /* Bounds-check the value BEFORE reading it -- catches a frame
         * truncated partway through an entry's value (the type tag said
         * more bytes should follow than the frame actually has). */
        if (off + KILNLINK_CONFIG_PAGE_ENTRY_HDR_LEN + vlen > len) {
            return KILNLINK_CONFIG_PAGE_ERR_LENGTH_MISMATCH;
        }

        out->entries[i].param_id = param_id;
        out->entries[i].type = type;
        /* type was already validated above, so this cannot fail. */
        (void)kilnlink_param_value_decode(type, payload, off + 3u, &out->entries[i].value);

        off += KILNLINK_CONFIG_PAGE_ENTRY_HDR_LEN + vlen;
    }

    /* No trailing bytes after the last (or zeroth) entry -- entry_count
     * disagreeing with the frame's actual length in either direction is
     * rejected, not silently tolerated. */
    if (off != len) {
        return KILNLINK_CONFIG_PAGE_ERR_LENGTH_MISMATCH;
    }

    out->page_index = payload[OFF_PAGE_INDEX];
    out->entry_count = entry_count;
    out->more = payload[OFF_MORE];

    return KILNLINK_CONFIG_PAGE_OK;
}
