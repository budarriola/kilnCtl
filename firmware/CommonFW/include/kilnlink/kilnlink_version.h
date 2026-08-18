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
 * Currently tracks KilnFW's PC-link UART_PROTOCOL_VERSION (5) because the two
 * have not diverged yet -- kilnlink's framing layer (kilnlink_frame.{c,h}) is
 * byte-for-byte the same envelope uart_protocol.c already uses for the PC
 * link. It becomes its own independent number the day the isolated link's
 * contract (kilnlink_context/kilnlink_status, not yet written) changes
 * without the PC link changing, or vice versa. */
#define KILNLINK_PROTOCOL_VERSION 5

/* The oldest peer this build will talk to (docs/LINK_PROTOCOL.md section 4,
 * "What 'compatible' means"). Nothing has shipped against a different
 * protocol version yet, so the oldest peer this build can talk to is simply
 * itself -- KILNLINK_MIN_COMPATIBLE tracks KILNLINK_PROTOCOL_VERSION until a
 * real compatibility-breaking change ships and this is deliberately bumped
 * apart from it in the same commit as that change. */
#define KILNLINK_MIN_COMPATIBLE 5

#endif /* KILNLINK_VERSION_H */
