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
 *            2     u8      type (KILNLINK_PARAM_TYPE_* in bits 0-6) |
 *                           KILNLINK_CONFIG_PAGE_UNSET_BIT (bit 7)
 *            3..   value, kilnlink_param_value_len(type) bytes (1, 1, 2, or 4)
 *                           -- present and encoded even when UNSET (as the
 *                           field's compiled-in/zero default), but MUST NOT
 *                           be treated as a real value by the reader; see
 *                           KILNLINK_CONFIG_PAGE_UNSET_BIT below.
 *
 * **The `set` bit (2026-08-27 audit fix, "ok cannot fail" commissioning-write
 * defect d).** Every KILNLINK_PARAM_TYPE_* tag this codec knows about
 * (BOOL/U8/U16/F32) is 0x00-0x03, so bit 7 of the type byte was always free;
 * it now carries whether the SENDER considers this entry SET (per
 * CONFIG_REFERENCE.md's no-safe-default fields, e.g. abs_max_temp_c before
 * commissioning) or reporting only a placeholder default. Before this bit
 * existed, an entry's `set`-ness was invented independently by whichever end
 * read the frame -- SaftyFW/src/tasks/link_task.c's link_task_send_config_
 * page() emitted every field's raw value with no way to say "this one is
 * still unset", and KilnFW/App/drivers/safety/safety_cfg_store.c's refetch loop
 * then set `.set = 1` for every entry it received, unconditionally. The
 * result: an operator-facing page showed abs_max_temp_c "{set:true,
 * value:0}" for a field the Pico itself considered UNSET, and 0 on that
 * specific field means the overtemperature guard NEVER TRIPS -- the single
 * worst value that field can silently carry.
 *
 * WIRE COMPATIBILITY: this is NOT wire-compatible with a pre-fix peer. A
 * pre-fix encoder never sets bit 7 (every entry it sends reads as `set =
 * true`, which is exactly its old unconditional behavior -- so a NEW decoder
 * talking to an OLD Pico degrades gracefully to the old, less-honest
 * behavior, never a decode failure). But a pre-fix DECODER validates the
 * whole type byte against its closed KILNLINK_PARAM_TYPE_* set and rejects
 * anything else as KILNLINK_CONFIG_PAGE_ERR_BAD_TYPE -- so an OLD ESP talking
 * to a NEW Pico that sends an unset field with bit 7 set would have that
 * entry's type byte read as 0x80/0x81/0x82/0x83, none of which match any
 * known tag, and reject the WHOLE PAGE. Per LINK_PROTOCOL.md's "commit as
 * the code and bump KILNLINK_PROTOCOL_VERSION if a peer would break" rule,
 * this change bumps KILNLINK_PROTOCOL_VERSION (see kilnlink_protocol_
 * version.h) -- both ends of this specific link ship together in one
 * firmware release, so the compatibility floor (KILNLINK_MIN_COMPATIBLE)
 * does not need to move, but the version number itself must, so a genuine
 * cross-version mismatch is still detected and logged rather than silently
 * misdecoded.
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
/* Bit 7 of the on-wire type byte -- see this header's top comment ("The `set`
 * bit"). Never combine with a KILNLINK_PARAM_TYPE_* tag directly; always go
 * through kilnlink_config_page_pack()/_decode(), which mask it in/out. */
#define KILNLINK_CONFIG_PAGE_UNSET_BIT 0x80u

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
    uint8_t  type; /* KILNLINK_PARAM_TYPE_* -- never carries KILNLINK_CONFIG_PAGE_UNSET_BIT; that bit lives
                    * only on the wire and in this struct's own separate `set` field below. */
    kilnlink_param_value_t value; /* meaningless when !set -- caller's job to check `set` first, same
                                    * "never treat an unset field's value as real" rule
                                    * safety_cfg_store.h's safety_cfg_param_t documents on the ESP side. */
    bool set; /* false: the sender considers this field UNSET (CONFIG_REFERENCE.md's no-safe-default
               * fields before commissioning) -- `value` is a placeholder, not a real reading. true:
               * a pre-KILNLINK_CONFIG_PAGE_UNSET_BIT peer's frames always decode this true, matching
               * that era's "every field is set" behavior (see this header's WIRE COMPATIBILITY note). */
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
