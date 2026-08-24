#ifndef KILNLINK_VERSION_H
#define KILNLINK_VERSION_H

/* The single source of truth for the wire protocol version. KilnFW's
 * UART_PROTOCOL_VERSION (App/drivers/uart_task_ids.h) becomes an alias of
 * this rather than a second number -- see CommonFW/README.md "Versioning".
 *
 * Bump when a change would break a peer running the old value: renumbering
 * an id, changing a payload layout or length, or changing the envelope. Do
 * not bump for comments or internal refactors -- a human judgement call,
 * deliberately not a hash of the file (uart_task_ids.h:8-20 has the full
 * reasoning, carried over unchanged).
 *
 * 5 -> 6 (2026-08-23): Frame A (SAFETY_CMD_GET_STATUS, isolated Pico<->ESP
 * link only) grew an optional 24th byte (tx_dropped_sat, saturating TX-ring-
 * drop counter -- link_frame.h's LINK_FRAME_STATUS_LEN_V2). This is
 * ADDITIVE, not breaking: a receiver on either side that only knows the old
 * 23-byte layout keeps working unmodified (KILNLINK_MIN_COMPATIBLE stays at
 * 5, deliberately not bumped alongside this -- see its own comment below),
 * and SaftyFW's sender only emits the 24th byte once it has positively
 * learned, via this same protocol_version field on an ANNOUNCE_VERSION it
 * received, that the peer ESP is built against 6+ (link_task.c's
 * LINK_FRAME_STATUS_V2_MIN_PROTOCOL) -- see link_frame.h's own doc comment
 * on link_frame_pack_status() for the exact skew-safety argument in both
 * directions. This is also the first time this number has been bumped for
 * an isolated-link-only change while the PC link's own contract stayed
 * untouched -- the "or vice versa" case this file's own comment above
 * already anticipated. It stays the SAME number as KilnFW's PC-link
 * UART_PROTOCOL_VERSION for now (not split into two independent numbers,
 * per the paragraph above) because nothing about the PC link's own frames
 * changed and link_frame_versions_compatible()'s range check (peer >= my
 * min_compatible && I >= peer's min_compatible) means a PC tool still on
 * protocol 5 remains fully compatible with an ESP now announcing 6 -- this
 * bump only ever WIDENS what a 6-speaking peer can additionally do, it never
 * narrows what a 5-speaking one could already do. Revisit the "split into
 * two numbers" question the day a change to ONE link's contract would force
 * an unwanted compatibility check on the other. */
#define KILNLINK_PROTOCOL_VERSION 6

/* The oldest peer this build will talk to (docs/LINK_PROTOCOL.md section 4,
 * "What 'compatible' means"). Deliberately NOT bumped alongside the 5 -> 6
 * step above: that step is additive-only (see KILNLINK_PROTOCOL_VERSION's
 * own comment), so a peer built against 5 is still fully compatible with
 * this build and must not be locked out. Bump this one only alongside a
 * future REAL compatibility-breaking change, in the same commit as that
 * change, same discipline as before. */
#define KILNLINK_MIN_COMPATIBLE 5

#endif /* KILNLINK_VERSION_H */
