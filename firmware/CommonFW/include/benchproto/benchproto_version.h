#ifndef BENCHPROTO_VERSION_H
#define BENCHPROTO_VERSION_H

/* The single source of truth for benchproto's wire protocol version.
 * Consumers (first: SimFW's usb_owner task and the fresh `kilnsim` PC
 * tools, per firmware/SimFW/docs/DESIGN_NOTES.md sec 4.4/5.1) should alias their
 * own "protocol version" constant to this one rather than keeping a second
 * number -- see CommonFW/README.md's kilnlink "Versioning" section, the
 * same discipline applies here.
 *
 * Bump when a change would break a peer running the old value: renumbering
 * a task_id, changing a payload layout or length, or changing the envelope
 * (benchproto_frame.h). Do not bump for comments or internal refactors --
 * a human judgement call, deliberately not a hash of the file. See
 * firmware/KilnFW/App/drivers/common/uart_task_ids.h:8-20 for the full reasoning
 * behind that policy; it applies unchanged to this constant. */
#define BENCHPROTO_PROTOCOL_VERSION 1

/* The oldest peer this build will talk to. Nothing has shipped against a
 * different benchproto version yet, so the oldest compatible peer is
 * simply itself -- BENCHPROTO_MIN_COMPATIBLE tracks
 * BENCHPROTO_PROTOCOL_VERSION until a real compatibility-breaking change
 * ships and this is deliberately bumped apart from it in the same commit
 * as that change. */
#define BENCHPROTO_MIN_COMPATIBLE 1

#endif /* BENCHPROTO_VERSION_H */
