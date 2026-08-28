#ifndef KILNLINK_VERSION_H
#define KILNLINK_VERSION_H

/* The single source of truth for the ESP<->Pico isolated safety link's wire
 * protocol version. From 2026-08-17 to 2026-08-24 KilnFW's
 * UART_PROTOCOL_VERSION (App/drivers/uart_task_ids.h) was a plain alias of
 * this number rather than a second, independently-maintained one -- see
 * CommonFW/README.md "Versioning" for that history. It no longer is: three
 * separate incidents of a bump here silently refusing all PC<->ESP traffic
 * (once, 2026-08-17's 5->6, for a change that never touched the PC link at
 * all) proved the two links' contracts diverge often enough that aliasing
 * them was actively dangerous rather than merely redundant. The two numbers
 * are now independent and must be bumped separately, each only when ITS OWN
 * link's contract changes -- see uart_task_ids.h's UART_PROTOCOL_VERSION
 * doc comment for the full rule and the failure it prevents in both
 * directions, and tools/check_uart_version_independence.ps1 for the CI grep
 * that keeps this constant from being re-aliased into that one.
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
 * an unwanted compatibility check on the other.
 *
 * 6 -> 7 (2026-08-24): SAFETY_CMD_GET_CT_CAL, SAFETY_CMD_GET_PARAM, and
 * SAFETY_CMD_GET_CONFIG_PAGE each moved off the command byte they used to
 * share with their own reply (0x1A, 0x1E, 0x1F respectively -- length was
 * the only thing telling request from reply apart) onto their own new ids
 * (0x22, 0x23, 0x24 -- see kilnlink_get_ct_cal.h/kilnlink_get_param.h/
 * kilnlink_get_config_page.h). This is BREAKING, not additive: a peer still
 * sending these requests under the old shared id is not decode-rejected by
 * the new build (there is no longer a dispatch case for the old id in that
 * direction at all), it is silently unmatched by the receiver's dispatch
 * switch and the request is dropped on the floor with no reply and no
 * error -- a failure mode that reads as a hung/dead link, not a version
 * mismatch, unless the two sides refuse to talk in the first place. That is
 * exactly the risk KILNLINK_MIN_COMPATIBLE exists to catch (see its own
 * comment below, which raises the floor alongside this bump). The reply
 * codecs (kilnlink_ct_cal.h/kilnlink_param.h/kilnlink_config_page.h) keep
 * their existing ids (0x1A/0x1E/0x1F) unchanged -- only the GET_* request
 * side moved. See docs/LINK_PROTOCOL.md's new "Request/reply ids must never
 * be shared" rule for why this class of change keeps recurring.
 *
 * 7 -> 8 (2026-08-27): CONFIG_PAGE (0x1F reply) entries gained a `set` bit
 * (KILNLINK_CONFIG_PAGE_UNSET_BIT, bit 7 of the per-entry type byte --
 * kilnlink_config_page.h's own header comment has the full audit trail,
 * "the `set` bit"/"ok cannot fail" commissioning-write defect d). This is
 * BREAKING in one direction only: a NEW Pico's frames still decode cleanly
 * on an OLD ESP as long as no field is actually unset (bit 7 clear reads
 * exactly like the old unconditional-set behavior), but the moment a real
 * no-safe-default field IS unset, the old decoder's closed KILNLINK_PARAM_
 * TYPE_* check rejects the type byte outright (KILNLINK_CONFIG_PAGE_ERR_
 * BAD_TYPE) and the WHOLE page decode fails -- not silently misread, but not
 * silently ignored either; a config fetch that used to succeed (if honestly
 * wrong about `set`) now visibly fails against a too-old peer. The other
 * direction (an OLD Pico's frames, which never set bit 7, read by a NEW ESP)
 * is fully additive and needs no version check at all. Per this file's own
 * rule ("bump if a peer would break"), the version number moves; see
 * KILNLINK_MIN_COMPATIBLE below for whether the floor moves with it. */
#define KILNLINK_PROTOCOL_VERSION 8

/* The oldest peer this build will talk to (docs/LINK_PROTOCOL.md section 4,
 * "What 'compatible' means"). Deliberately NOT bumped alongside the 5 -> 6
 * step above: that step is additive-only (see KILNLINK_PROTOCOL_VERSION's
 * own comment), so a peer built against 5 is still fully compatible with
 * this build and must not be locked out.
 *
 * Bumped 5 -> 7 alongside the 6 -> 7 step above: that step is a REAL
 * compatibility break (GET_CT_CAL/GET_PARAM/GET_CONFIG_PAGE renumbered off
 * their shared ids), and unlike the additive 5->6 step, staying permissive
 * here would trade a loud, diagnosable "incompatible peer" refusal for a
 * silent one -- a pre-7 peer's GET_* requests would simply stop being
 * answered, with nothing in the log pointing at a version skew. The bench
 * cost is real (a board still running 5 or 6 firmware can no longer talk to
 * a 7-built peer at all, not even for the frames that did not change), but
 * it is a known, visible cost paid once at flash time, not an intermittent
 * field failure that looks like a wedged link.
 *
 * NOT bumped alongside the 7 -> 8 step above. Unlike the 6 -> 7 break, an old
 * peer here does not go silently unanswered: the 7 -> 8 comment's own
 * analysis is that a too-old ESP either decodes a new-Pico page correctly
 * (nothing was actually unset yet) or gets a loud, visible per-fetch decode
 * failure it already knows how to retry/back off from (safety_cfg_store.c's
 * existing retry-with-backoff path) -- it is never "the field arrives, but
 * silently wrong" the way this whole audit exists to close. Raising the
 * floor to 8 would instead brick EVERY OTHER frame on the link (GET_STATUS/
 * DIAG/POWER/TRIP_EVENT/...) against a peer whose only actual gap is one
 * reply's honesty about an unset field -- a strictly worse outcome than the
 * targeted, self-diagnosing failure this staying at 7 already produces. */
#define KILNLINK_MIN_COMPATIBLE 7

#endif /* KILNLINK_VERSION_H */
