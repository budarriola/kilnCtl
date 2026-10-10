#ifndef KILNLINK_VERSION_H
#define KILNLINK_VERSION_H

/* The single source of truth for the ESP<->Pico isolated safety link's wire
 * protocol version. From 2026-08-17 to 2026-08-24 KilnFW's
 * UART_PROTOCOL_VERSION (App/drivers/common/uart_task_ids.h) was a plain alias of
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
 * KILNLINK_MIN_COMPATIBLE below for whether the floor moves with it.
 *
 * 8 -> 9 (2026-08-30): new Pico -> ESP frame, SAFETY_CMD_ROLLBACK_RESULT
 * (0x25, kilnlink_rollback_result.h) -- the missing wire-visible reply for
 * a refused SAFETY_CMD_ROLLBACK (0x17). Before this, a rollback refusal
 * (relay ARMED, or the other bootloader slot not VALID/PENDING_VERIFY) was
 * logged only to SaftyFW's own local log; the ESP had no way to distinguish
 * "refused" from "accepted, rebooting" from "link was down the whole time."
 * This follows the EXACT 5->6 precedent: ADDITIVE, not breaking, same as
 * that entry's own reasoning applied to a whole new frame instead of a
 * growing field --
 *   - An ESP still on protocol 8 or older simply never sees this frame: it
 *     keeps working exactly as it does today (a refusal stays silent on the
 *     wire, same as before this change). Nothing about the OLD frame set
 *     (STATUS/ROLLBACK/etc.) changed shape, so an old ESP decoding those
 *     is completely unaffected.
 *   - SaftyFW's sender (link_task_handle_rollback(), src/tasks/link_task.c)
 *     only emits 0x25 once it has positively learned, via this same
 *     protocol_version field on an ANNOUNCE_VERSION it received, that the
 *     peer ESP is built against 9+ (kilnlink_rollback_result.h's own
 *     KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL gate, mirroring link_frame.h's
 *     LINK_FRAME_STATUS_V2_MIN_PROTOCOL exactly) -- so an old ESP is never
 *     sent a frame its dispatch switch has no case for.
 *   - The reverse skew direction (a NEW ESP talking to an OLD Pico that
 *     predates this frame entirely) is the one this bump does NOT protect
 *     against by itself, because it cannot: an old Pico simply never sends
 *     0x25, bump or no bump. This is why safety_link_send_rollback()
 *     (KilnFW's safety_link.c) is written to ALWAYS bound its wait for this
 *     reply with a timeout and treat "no frame arrived" as an UNKNOWN
 *     outcome the caller must not report as success -- only a subsequent
 *     link drop-and-reconnect with a new boot_id is treated as proof of
 *     acceptance. A protocol-version bump cannot make an old peer speak a
 *     frame it was never built to send; only the receiver's own timeout
 *     discipline can make that safe, and this codebase's discipline for it
 *     already assumes "no reply" is not evidence of anything.
 * KILNLINK_MIN_COMPATIBLE is NOT raised alongside this bump -- see its own
 * comment below.
 *
 * 9 -> 10 (2026-09-03): Frame A (SAFETY_CMD_GET_STATUS, isolated Pico<->ESP
 * link only) grew two optional bytes (24/25, link_frame.h's LINK_FRAME_
 * STATUS_LEN_V3 -- 26 bytes total) carrying the new
 * BORROWED status flag (flags2 byte 24, bit 0) plus its borrowed_zone_index
 * (byte 25, 0xFF sentinel = unknown/uncommissioned) -- surfacing whether the
 * safety processor's reading is (at least partly) sourced from another
 * zone's probe (tc_source == BORROWED_ZONE or BOTH), which guard S13
 * polices and which an operator looking at an unlabelled reading has no way
 * to tell from an honest own-sensor reading otherwise. EXACT same shape as
 * the 5->6 step: ADDITIVE, not breaking. A receiver that only knows the old
 * 24-byte V2 layout keeps working unmodified (KILNLINK_MIN_COMPATIBLE stays
 * at 7, not bumped alongside this), and SaftyFW's sender only emits bytes
 * 24/25 once it has positively learned, via ANNOUNCE_VERSION, that the peer
 * ESP is built against 10+ (link_frame.h's LINK_FRAME_STATUS_V3_MIN_
 * PROTOCOL) -- see link_frame_pack_status()'s own doc comment for the full
 * skew-safety argument, identical in shape to the 5->6 step's, applied a
 * second time to the same frame. A newer Pico talking to an ESP it has not
 * yet confirmed as v10+ sends the 24-byte V2 frame it already knows that
 * peer accepts -- never silently upgrades the length and risks silencing
 * Frame A itself. An older Pico (predates V3 entirely) simply never sets
 * peer_supports_status_v3 true, bump or no bump -- same "a version bump
 * cannot make an old peer speak a frame it was never built to send" limit
 * the 8->9 entry's own comment already states for ROLLBACK_RESULT. This is
 * why safety_link.c's absent-byte handling on the ESP side treats "V1/V2
 * frame, no byte 24/25" as UNKNOWN and fails closed (never assumes
 * not-borrowed) -- see safety_link.h's SAFETY_LINK_STATUS_FRAME_LEN_V3
 * comment for that reasoning, mirroring the `min_compatible` precedent that
 * a too-short frame must read as UNKNOWN, never as a zero that silently
 * means "fine".
 * KILNLINK_MIN_COMPATIBLE is NOT raised alongside this bump -- see its own
 * comment below.
 *
 * 10 -> 11 (2026-09-06): Frame E (SAFETY_CMD_POWER) grew 6 bytes, 3 x u16
 * counts_avg -- the raw 16x-oversampled ADC counts per channel, published
 * independent of calibration state (CURRENT_SENSE.md sec 4's "Tooling gap"
 * option 1/2: there was previously no way to see the ADC's actual value
 * when a channel is uncommissioned, since amps[] honestly reads 0.0f in
 * that case). Unlike every other length-grown frame on this link (Frame A's
 * 5->6 and 9->10 steps, both ADDITIVE with a length-tolerant decoder from
 * day one), kilnlink_power_decode() had NO length-tolerant path before this
 * change -- it hard-rejected (KILNLINK_POWER_ERR_LENGTH_MISMATCH) any
 * length other than the old fixed KILNLINK_POWER_LEN (55). This bump adds
 * that tolerance AT THE SAME TIME as the new field: kilnlink_power.h now
 * defines KILNLINK_POWER_LEN_V1 (55, still accepted) and _V2 (61, current),
 * with a new KILNLINK_POWER_FLAG_COUNTS_VALID flag bit distinguishing "this
 * frame carries real counts" from "legacy peer, counts_avg is zero-filled
 * padding" for a decoder that cannot infer that from length alone once the
 * length check has already passed both ways. Net effect once both sides
 * are rebuilt: fully additive, same as the 5->6/9->10 precedent -- an old
 * (protocol <= 10) Pico keeps sending 55-byte frames a new ESP decodes with
 * counts_avg=0/flag clear, and a new (protocol 11) Pico's 61-byte frames
 * decode cleanly on an old ESP built against this same commit's decoder
 * (which already accepts both lengths) but would have been REJECTED
 * outright by any ESP build that predates this commit -- that boundary is
 * exactly what the version bump exists to make visible rather than an
 * unexplained "POWER frame stopped decoding" symptom.
 * KILNLINK_MIN_COMPATIBLE is NOT raised alongside this bump, same shape of
 * reasoning as 5->6/8->9/9->10: no other frame's shape changed, and the
 * decoder change is itself the thing that makes staying permissive safe --
 * a pre-11 peer's 55-byte POWER frames still decode correctly under this
 * commit's own decoder.
 *
 * 11 -> 12 (2026-09-06): three brand-new frames, CT_COMMISSIONING_PLAN.md
 * step 2's auto idle-offset measurement -- SAFETY_CMD_CT_AUTO_ZERO_BEGIN
 * (0x26, ESP->Pico, kilnlink_ct_auto_zero_begin.h), SAFETY_CMD_GET_CT_
 * AUTO_ZERO (0x27, ESP->Pico request-only, kilnlink_get_ct_auto_zero.h),
 * and its reply SAFETY_CMD_CT_AUTO_ZERO_STATUS (0x28, Pico->ESP,
 * kilnlink_ct_auto_zero_status.h). EXACT same shape as the 8->9
 * (ROLLBACK_RESULT) precedent: fully additive, three ids nothing on this
 * link used before. An ESP still on protocol 11 or older simply never
 * sends CT_AUTO_ZERO_BEGIN/GET_CT_AUTO_ZERO (its commissioning page has no
 * "Auto-zero" button yet) and would silently drop a CT_AUTO_ZERO_STATUS
 * reply it has no dispatch case for -- but no old-ESP build ever receives
 * one, because a pre-12 Pico never sends it either (this pass changes both
 * sides together). Nothing about the existing frame set (STATUS/DIAG/
 * POWER/CT_CAL/etc) changed shape.
 * KILNLINK_MIN_COMPATIBLE is NOT raised alongside this bump, same
 * reasoning as 8->9/9->10/10->11: a peer built against 7 through 11
 * remains fully compatible with a 12-built peer for every frame that
 * existed before this pass.
 *
 * 12 -> 12, DELIBERATELY NOT BUMPED (2026-09-09): SAFETY_CMD_REBOOT (0x29,
 * ESP->Pico, kilnlink_reboot.h) and its reply SAFETY_CMD_REBOOT_RESULT
 * (0x2A, Pico->ESP, kilnlink_reboot_result.h) -- the genuine "reboot
 * yourself in place, same firmware slot" command behind KilnFW's POST
 * /api/sw_reset. Two brand-new ids, nothing existing changed shape. This
 * entry exists because the question "does an additive opcode need a bump?"
 * was asked explicitly, and the answer here is NO -- a narrower argument
 * than the 11->12 step's, resting on one specific structural property:
 *   - This pair is REQUEST-TRIGGERED in both directions. Only a build that
 *     has this feature ever sends 0x29, and a Pico only ever emits 0x2A in
 *     direct reply to a 0x29 it just decoded. There is therefore no path by
 *     which an OLD peer, in either direction, ever receives a byte it does
 *     not know: an old Pico never sends 0x2A (it never understood a 0x29 to
 *     reply to), and an old ESP never receives 0x2A (it never sent a 0x29).
 *     Contrast SAFETY_CMD_ROLLBACK_RESULT (8->9) and the Frame A length
 *     growths (5->6, 9->10), all emitted on the PICO's own initiative and
 *     therefore genuinely needing both a bump and a peer_protocol_version
 *     gate to know when it was safe to speak.
 *   - This file's stated rule is "bump when a change would break a peer
 *     running the old value." Nothing here can: an old Pico drops the
 *     unrecognized 0x29 into link_task.c's dispatch default (counted, never
 *     acted on), safety_link_send_reboot() sees no reply within its bounded
 *     window and reports NO_REPLY, and sw_reset_http.c reports honestly
 *     that the safety processor was NOT confirmed to reboot. Degraded,
 *     visible, never misreported as success -- exactly the property a bump
 *     would have been protecting.
 * If a FUTURE change makes either frame Pico-initiated (an unsolicited "I
 * am about to reboot myself" notice, say), that change DOES need a bump
 * plus a link_frame_*_supported() gate like ROLLBACK_RESULT's -- the
 * argument above depends entirely on the request-triggered shape and does
 * not survive without it.
 *
 * 12 -> 13 (2026-09-14): SAFETY_CMD_GET_STACK_MARGIN (0x2B, ESP->Pico,
 * kilnlink_get_stack_margin.h) and its reply SAFETY_CMD_STACK_MARGIN (0x2C,
 * Pico->ESP, kilnlink_stack_margin.h) -- live per-task SaftyFW stack
 * high-water marks, per docs/audits/saftyfw_live_stack_reporting_design_
 * 2026-09-11.md and its impl audit
 * docs/audits/saftyfw_live_stack_reporting_impl_2026-09-14.md.
 *
 * This pair is BUMPED, unlike the 12 (unbumped) REBOOT/REBOOT_RESULT
 * precedent immediately above, because it fails that precedent's own test:
 * "does an old peer, in either direction, ever receive a byte it does not
 * know?" REBOOT/REBOOT_RESULT is request-triggered in both directions (an
 * old Pico never emits 0x2A because it never understood a 0x29 to reply
 * to). STACK_MARGIN is ALSO request-triggered (0x2C is only ever sent in
 * reply to a 0x2B), so that half is fine on its own -- an old Pico simply
 * never sends 0x2C. The break is the other direction: nothing here changed
 * an EXISTING frame's shape, so a genuinely additive-only reading might
 * suggest no bump is needed at all. But the actual hazard this project
 * cares about (docs/LINK_PROTOCOL.md's "a peer would break" test) is not
 * about frame length collisions here -- 0x2B/0x2C are brand-new ids an old
 * peer's dispatch switch drops into its unrecognized-command default,
 * exactly like an unbumped REBOOT would. It is bumped anyway, deliberately
 * more conservative than the letter of that test, because this feature's
 * whole purpose is safety-relevant stack-margin visibility during exactly
 * the dual-reflash window (docs/MCP_SERVERS.md's flash section) where a
 * skewed pair is already the single most common real-world failure mode in
 * this codebase (project memory: "+121 crc/framing errors in 3 seconds" on
 * a mismatched pair) -- a version-visible bump gives both
 * flash_firmware()'s verification step and a human reading /api/status a
 * cheap, explicit signal that the two processors were flashed together,
 * rather than relying solely on absence-of-symptom (an old Pico silently
 * never answering 0x2B) to notice the skew.
 * KILNLINK_MIN_COMPATIBLE is NOT raised alongside this bump, same
 * reasoning as every additive step above: a peer built against 7 through 12
 * remains fully compatible with a 13-built peer for every frame that
 * existed before this pass; it simply never receives the new stack-margin
 * data (get_saftyfw_stack_margin()-style tooling on a 13 ESP talking to a
 * pre-13 Pico gets NO_REPLY on 0x2B, exactly like any other unsupported
 * request against an old peer, and must report absence honestly rather
 * than a zero/fabricated reading).
 * The separate UART LINK version (kilnlink_get_fw_version.h /
 * KILNLINK_MIN_COMPATIBLE's sibling constant, currently 11) is UNCHANGED --
 * this pass adds an application-level command pair; it does not touch the
 * transport framing (start/length/CRC) at all.
 *
 * 13 -> 14 (2026-09-14): SAFETY_CMD_STACK_MARGIN (0x2C)'s payload grows a
 * `last_tick_ms` (u32 LE) field -- KILNLINK_STACK_MARGIN_LEN 47 -> 51 --
 * per docs/audits/saftyfw_live_stack_reporting_impl_2026-09-14.md's review
 * (defect 1): `rounds_completed` alone saturates at 255 within ~2 minutes of
 * uptime and cannot serve as a freshness signal, so a stalled poller (log_task
 * hung) was indistinguishable on the wire from a live one. `last_tick_ms` is
 * a free-running tick count stamped on every poller tick (~500ms), letting a
 * consumer detect "this cache stopped advancing" independent of
 * `rounds_completed`.
 *
 * This is a REAL layout break of an EXISTING frame -- unlike the 12->13 step
 * (brand-new ids), an old 47-byte-shaped decoder reading a new 51-byte frame,
 * or a new decoder reading an old 47-byte frame, would misalign every entry
 * field if length were not checked. It is NOT actually unsafe in practice
 * only because `kilnlink_stack_margin_decode()` rejects any `len !=
 * KILNLINK_STACK_MARGIN_LEN` before reading a single field (docs/audits/
 * saftyfw_live_stack_reporting_impl_2026-09-14.md review sec 5) -- a skewed
 * pair fails closed with ERR_LENGTH_MISMATCH, never a misparse, exactly the
 * same safety property the Frame A growths (5->6, 9->10) relied on. Bumped
 * anyway for the same visibility reason as 12->13: a version-visible signal
 * beats relying on a length-mismatch log line to notice a dual-reflash skew.
 * KILNLINK_MIN_COMPATIBLE is NOT raised: the length check alone is
 * sufficient to prevent misparsing, and an old (13) Pico paired with a new
 * (14) ESP, or vice versa, degrades to a clean, logged ERR_LENGTH_MISMATCH
 * on this one frame -- every other frame on the link is completely
 * unaffected, so refusing the whole link over this one payload's growth
 * would be strictly worse than the status quo.
 *
 * 14 -> 15 (2026-09-14): SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D) added --
 * docs/KILN_PROFILES_PLAN.md item 15's RAM-only sibling to COMMIT_CONFIG
 * (0x1D). Purely additive: a whole new frame neither side is required to
 * send or understand to keep every existing frame working, same shape as
 * the 8 -> 9 and 11 -> 12 steps above, so KILNLINK_MIN_COMPATIBLE is NOT
 * raised alongside it (see that constant's own comment).
 *
 * 15 -> 16 (2026-09-20): SAFETY_CMD_DIAG (0x08, Frame B)'s payload grows a
 * `log_frames_dropped` (u32 LE) field -- KILNLINK_DIAG_LEN 26 -> 30 --
 * TODO.md's "Dropped-log-frame counter surfaced from the diagnostic frame".
 * log_task.c's own drop counter (log_task_get_dropped()) existed already --
 * log emission on this side has always been best-effort/non-blocking, never
 * able to stall a safety task -- but was never carried past this boot's own
 * RAM onto the wire, so a Pico silently dropping log lines under load (its
 * 16-deep queue, log_task.c) looked, from the ESP/PC side, identical to one
 * that simply had nothing to say.
 *
 * This is a REAL layout break of an EXISTING frame, same class as the
 * 13 -> 14 stack-margin growth: an old 26-byte-shaped decoder reading a new
 * 30-byte frame, or a new decoder reading an old 26-byte frame, would
 * misalign nothing (the field is strictly appended) but would MISS the new
 * counter entirely without a length check -- kilnlink_diag_decode() rejects
 * any len != KILNLINK_DIAG_LEN before reading a single field, so a skewed
 * pair fails closed with ERR_LENGTH_MISMATCH rather than silently reading a
 * stale/zero value. Bumped for the same visibility reason as 12->13/13->14:
 * a version-visible signal beats relying on a length-mismatch log line to
 * notice a dual-reflash skew. KILNLINK_MIN_COMPATIBLE is NOT raised: the
 * length check alone is sufficient, and an old (15) Pico paired with a new
 * (16) ESP, or vice versa, degrades to a clean ERR_LENGTH_MISMATCH on this
 * one frame while every other frame on the link is unaffected.
 *
 * 16 -> 17 (2026-10-09, kilnlink robustness audit M4): CLEAR_TRIP binds to a
 * trip occurrence. SAFETY_CMD_DIAG gains a trailing trip_seq byte
 * (KILNLINK_DIAG_LEN_V2 = 31) and SAFETY_CMD_CLEAR_TRIP an optional trailing
 * trip_seq byte (KILNLINK_CLEAR_TRIP_LEN_V2 = 4). Before this a duplicated or
 * replayed `0A mask` frame, or one built from a DIAG cached up to
 * SAFETY_LINK_STALE_MS old, could clear a LATER occurrence of the same
 * reason once its condition had gone. Additive and gated both ways, so
 * KILNLINK_MIN_COMPATIBLE is NOT raised:
 *   - the Pico sends the 31-byte DIAG only to a peer that announced >= 17
 *     (a 16 ESP rejects any DIAG length but 30);
 *   - the ESP sends the 4-byte CLEAR_TRIP only when its cached DIAG was the
 *     31-byte form, i.e. the Pico is >= 17 (a 16 Pico rejects any CLEAR_TRIP
 *     length but 3);
 *   - a 17 Pico refuses a 3-byte (unbound) CLEAR_TRIP from a peer that
 *     announced >= 17, and refuses a 4-byte one whose trip_seq is not the
 *     occurrence latched when safety_core dequeues it. A 3-byte frame from
 *     a 16 peer, or before any ANNOUNCE_VERSION (peer version 0), keeps the
 *     legacy mask-only behaviour, so the boot-time S6a clear and a mixed
 *     pair still work. */
#define KILNLINK_PROTOCOL_VERSION 17

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
 * targeted, self-diagnosing failure this staying at 7 already produces.
 *
 * NOT bumped alongside the 14 -> 15 step below either, same shape as
 * 8 -> 9 / 11 -> 12: one brand-new, wholly additive frame (SAFETY_CMD_
 * APPLY_CONFIG_VOLATILE, 0x2D, docs/KILN_PROFILES_PLAN.md item 15), nothing
 * existing changed shape or meaning, so a peer built against 7 through 14
 * remains fully compatible with a 15-built peer for everything it already
 * knew how to speak -- it simply has no caller for the one new command yet,
 * same as any other peer that predates a purely additive frame.
 *
 * NOT bumped alongside the 8 -> 9 step above either, same shape of reason
 * as the 5 -> 6 step: that step is purely additive (a whole new frame
 * neither side is required to send or understand to keep every existing
 * frame working -- see KILNLINK_PROTOCOL_VERSION's own 8->9 comment for the
 * full two-direction skew argument), so a peer built against 7 or 8 remains
 * fully compatible with a 9-built peer and must not be locked out over a
 * feature it simply predates.
 *
 * NOT bumped alongside the 9 -> 10 step above either, same reasoning again:
 * that step is purely additive (two new optional bytes on an existing frame,
 * gated the same way the 5->6 step's byte 23 was), so a peer built against
 * 7, 8, or 9 remains fully compatible with a 10-built peer.
 *
 * NOT bumped alongside the 10 -> 11 step above either: that step's own
 * decoder change is what makes staying permissive safe (see its own
 * comment) -- a peer built against 7 through 10 sends/reads the 55-byte V1
 * POWER layout, which this build's decoder still accepts.
 *
 * NOT bumped alongside the 11 -> 12 step above either, same shape as
 * 8 -> 9: three brand-new frames, nothing existing changed shape, so a
 * peer built against 7 through 11 remains fully compatible with a
 * 12-built peer for everything it already knew how to speak.
 *
 * NOT bumped alongside the 16 -> 17 step either: both new trailing bytes are
 * sent only to a peer known to be >= 17 (see that step's comment), so a 7
 * through 16 peer still sees exactly the frames it already knew. */
#define KILNLINK_MIN_COMPATIBLE 7

#endif /* KILNLINK_VERSION_H */
