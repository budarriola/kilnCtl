#ifndef KILNLINK_CONFIG_PAGE_H
#define KILNLINK_CONFIG_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kilnlink/kilnlink_param_value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_CONFIG_PAGE = 0x1F -- sent in reply to
 * SAFETY_CMD_GET_CONFIG_PAGE (kilnlink_get_config_page.h, its own id 0x24
 * since KILNLINK_PROTOCOL_VERSION 7 -- see that header's comment). Before
 * version 7 this reply shared 0x1F with its own request, distinguished only
 * by direction and length; that scheme structurally blocked a
 * length-different refusal reply, which is why the request moved off this
 * id. 0x1F itself is unchanged and is now used ONLY by this reply.
 *
 * Wire layout:
 *   0      u8   cmd (0x1F)
 *   1      u8   page_index -- echoes the request
 *   2      u8   entry_count (N) -- how many (id, value) pairs follow
 *   3      u8   more -- 1 if further pages remain beyond this one, else 0
 *   4..    N * entry, each:
 *            0..1  u16 LE  param_id
 *            2     u8      type (KILNLINK_PARAM_TYPE_*)
 *            3..   value, kilnlink_param_value_len(type) bytes (1, 1, 2, or 4)
 *
 * **Paging scheme.** The 253-byte payload cap (LINK_PROTOCOL.md sec 3) is
 * far too small for CONFIG_REFERENCE.md secs 1-5's whole surface in one
 * frame once every field carries its own type tag, so this is a bulk read
 * split across as many frames as it takes: kilnlink_config_page_pack()
 * greedily packs entries, in the order given, into one frame until the next
 * entry would not fit in `out_cap` (normally 253) -- **as many pairs as fit
 * and no more** -- then stops and reports `more = 1` if entries remain
 * unpacked. The caller (SaftyFW's link_task.c) drives this by re-invoking
 * with the leftover slice of its parameter table and the next page_index;
 * the ESP drives its side by requesting page 0, 1, 2, ... until a reply
 * comes back with `more == 0`. Packing is greedy and stateless per call --
 * there is no server-side cursor to go stale, which matters because
 * GET_CONFIG_PAGE requests, like every frame on this link, can be lost or
 * repeated with no ACK (LINK_PROTOCOL.md sec 2): re-asking for the same
 * page_index after a lost reply always reproduces the same bytes.
 *
 * `entry_count` is a real, physical bound: at minimum-size entries (BOOL/
 * U8, 4 bytes each) up to 62 fit in a 253-byte frame, but
 * KILNLINK_CONFIG_PAGE_MAX_ENTRIES (32) is the array capacity this codec
 * actually allocates for -- comfortably above what any config page needs
 * (CONFIG_REFERENCE.md secs 1-5 is well under 32 fields per logical group)
 * while keeping kilnlink_config_page_t a fixed, modest size. A page that
 * would need more than 32 entries to stay within the byte cap is a caller
 * bug (too many entries handed to one pack() call), not a wire condition --
 * see kilnlink_config_page_pack()'s own comment.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Decoding walks the entry
 * list checking remaining-length before every field read, so a truncated
 * frame, a bad type tag partway through, or entry_count disagreeing with
 * the frame's actual length are all caught without ever reading past
 * `payload[len - 1]`. */

#define KILNLINK_CONFIG_PAGE_CMD 0x1Fu
#define KILNLINK_CONFIG_PAGE_HDR_LEN 4u /* cmd(1) + page_index(1) + entry_count(1) + more(1) */
#define KILNLINK_CONFIG_PAGE_ENTRY_HDR_LEN 3u /* param_id u16(2) + type(1), before the value */
#define KILNLINK_CONFIG_PAGE_ENTRY_MAX_LEN (KILNLINK_CONFIG_PAGE_ENTRY_HDR_LEN + 4u) /* + f32 value */
#define KILNLINK_CONFIG_PAGE_MAX_ENTRIES 32u

typedef enum {
    KILNLINK_CONFIG_PAGE_OK = 0,
    KILNLINK_CONFIG_PAGE_ERR_BUFFER_TOO_SMALL,  /* out_cap too small to hold even the 4-byte header */
    KILNLINK_CONFIG_PAGE_ERR_LENGTH_MISMATCH,   /* frame too short/long, or truncated mid-entry/mid-value */
    KILNLINK_CONFIG_PAGE_ERR_WRONG_CMD,         /* byte 0 isn't KILNLINK_CONFIG_PAGE_CMD */
    KILNLINK_CONFIG_PAGE_ERR_BAD_TYPE,          /* an entry's type byte isn't a KILNLINK_PARAM_TYPE_* tag */
    KILNLINK_CONFIG_PAGE_ERR_TOO_MANY_ENTRIES,  /* entry_count exceeds KILNLINK_CONFIG_PAGE_MAX_ENTRIES */
} kilnlink_config_page_status_t;

typedef struct {
    uint16_t param_id;
    uint8_t  type; /* KILNLINK_PARAM_TYPE_* */
    kilnlink_param_value_t value;
} kilnlink_config_page_entry_t;

typedef struct {
    uint8_t page_index;
    uint8_t entry_count;
    uint8_t more;
    kilnlink_config_page_entry_t entries[KILNLINK_CONFIG_PAGE_MAX_ENTRIES];
} kilnlink_config_page_t;

/* Greedily packs entries[0 .. entries_avail) into one CONFIG_PAGE frame in
 * `out`, stopping as soon as the next entry would not fit within `out_cap`
 * (the 253-byte payload cap in practice) or KILNLINK_CONFIG_PAGE_MAX_ENTRIES
 * entries have been packed, whichever comes first -- "as many pairs as fit
 * and no more". `*entries_packed` receives how many of `entries_avail` were
 * actually written; the `more` byte in the frame is set to 1 iff
 * `*entries_packed < entries_avail`, telling the reader to ask for
 * page_index + 1 with the leftover slice. Returns the total frame length
 * (KILNLINK_CONFIG_PAGE_HDR_LEN or more), or 0 on error:
 *   - KILNLINK_CONFIG_PAGE_ERR_BUFFER_TOO_SMALL if out_cap can't even hold
 *     the 4-byte header,
 *   - KILNLINK_CONFIG_PAGE_ERR_BAD_TYPE if any entry passed in carries a
 *     type this build does not recognise (a caller bug -- every entry
 *     handed in is assumed to already be validated data, not wire input). */
size_t kilnlink_config_page_pack(uint8_t page_index, const kilnlink_config_page_entry_t *entries,
                                  size_t entries_avail, uint8_t *out, size_t out_cap,
                                  size_t *entries_packed, kilnlink_config_page_status_t *status);

/* Parses a CONFIG_PAGE payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. Walks the entry list checking remaining length before every
 * read: rejects a frame shorter than the header, an entry_count above
 * KILNLINK_CONFIG_PAGE_MAX_ENTRIES, a bad type tag on any entry, a frame
 * truncated partway through an entry's id/type or its value, and a frame
 * with trailing bytes after the last entry (entry_count says fewer entries
 * than the frame's actual length implies) -- all without ever reading past
 * `payload[len - 1]`. Untrusted input from another processor across an
 * isolated link (CommonFW/README.md rule 6). */
kilnlink_config_page_status_t kilnlink_config_page_decode(const uint8_t *payload, size_t len,
                                                            kilnlink_config_page_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_CONFIG_PAGE_H */
