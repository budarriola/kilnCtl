# Review: TEST_TRIP WP1 (c2a866b23), kilnlink 17 -> 18 codecs

Date: 2026-10-10. Reviewer: Claude (Opus), adversarial review, no code changed.
Scope: origin/dev commit c2a866b23 ("TEST_TRIP WP1"), reviewed at origin/dev
91e784758 against `docs/TEST_TRIP_PLAN.md`. Files: CommonFW `kilnlink_diag.c`,
`kilnlink_clear_trip.c`, new `kilnlink_test_trip.{c,h}` and
`kilnlink_test_trip_result.{c,h}`, `kilnlink_version.h`, the CommonFW tests,
`wire_protocol_fingerprints.json`, `safety_guards.h` (`SAFETY_TRIP_TEST = 4`),
PcTools `kilnlink_capture.py`, `kilnlink_codec.py`, `devices_safety.py` and
tests, and `LINK_PROTOCOL.md`.

## Summary

The codecs themselves are correct: exact length sets, `has_trip_seq` /
`has_boot_id` derived right, no out-of-bounds read on a hostile length, closed
outcome enum enforced on both encode and decode, magic carried verbatim, no
opcode collision, and no codec path that can clear or mask a trip. The PcTools
mirrors match the C byte for byte.

The real problem is where the version bump lives. `KILNLINK_PROTOCOL_VERSION`
is 18 from WP1 on, so every KilnFW build from here on announces 18, but the
KilnFW consumer code (its own DIAG parser and its CLEAR_TRIP builder) is still
at 17 behaviour until WP3. The plan runs WP2 and WP3 in parallel with no
landing order. A dev tip that has WP2 but not WP3 gives a pair where the ESP
drops every DIAG and every clear is refused (HIGH-1).

| ID | Grade | One line |
|---|---|---|
| HIGH-1 | HIGH | WP1 ESP announces 18 but still parses only 30/31-byte DIAG and builds 4-byte clears; a WP2 Pico answers with V3 DIAG and requires V3 clears |
| MED-1 | MED | Version 18 announced before any 18 behaviour exists; WP3's ">= 18" gate cannot tell a WP1-only Pico from a WP2 one |
| LOW-1 | LOW | LINK_PROTOCOL.md "Version 18 compatibility" claims old peers decode all lengths; older "Both lengths decode" text is stale |
| LOW-2 | LOW | Shared vector JSON not extended to V3; no C<->Python V3 DIAG vector; C V3 DIAG vector pins 3 bytes |
| LOW-3 | LOW | `safety_guards.h` comments still call 4 a reserved WARN-only gap; warn_mask bit 3 (S4) and trip_mask bit 3 (TEST) now mean different things |
| INFO-1..6 | INFO | compat-frames table, word tables, exact-equality update gates, protocol.py ids, absent-trip_seq default, no trip-clearing path |

## Findings

### HIGH-1: ESP announces 18 with 17 consumers; WP2 Pico then breaks DIAG and clear

`kilnlink_version.h` is shared: `safety_link_frames.c:176` announces
`KILNLINK_PROTOCOL_VERSION`, so a KilnFW image built from any commit at or after
c2a866b23 announces 18. But KilnFW does not use the CommonFW DIAG decoder. Its
own `safety_apply_diag()` (`safety_link_frames.c:1084`) accepts only
`SAFETY_LINK_DIAG_FRAME_LEN` (30) and `SAFETY_LINK_DIAG_FRAME_LEN_V2` (31), and
`safety_link_clear_trip()` (`safety_link_commands.c:144-151`) never sets
`has_boot_id`, so it always sends the 3- or 4-byte clear.

Plan WP2 (`TEST_TRIP_PLAN.md` sec 11 and the F6 section, line ~114 and ~355)
makes the Pico send the 32-byte DIAG to any peer that announced >= 18 and
accept only the 5-byte clear from such a peer (`REFUSE_BOOT_ID_REQUIRED`).
Sec 11 says WP2 and WP3 "run in parallel"; nothing orders their landing.

Scenario: WP2 lands on dev before WP3 (both are in flight now). The ESP image
built from that tip embeds the matching Pico image, so the coordinated reflash
or Pico auto-update installs the pair. The ESP announces 18. The Pico sends
32-byte DIAGs; the ESP rejects every one as an "unexpected DIAG frame" and
bumps `frame_errors`. The cached DIAG goes stale, the dashboard loses trip
state from DIAG, and `safety_link_clear_trip()` refuses locally because the
cached DIAG is older than `SAFETY_LINK_STALE_MS`. If the cache were fresh, the
4-byte clear would still be refused by the Pico. A real trip cannot be cleared
from the web, LCD or MCP until WP3 lands and both sides are reflashed. This
fails safe (the trip stays latched), but it is a full loss of trip clearing
and DIAG visibility, and a daily release tag or a dev->main promotion cut in
that window would ship it.

Fix (any one):
- Land WP2 and WP3 together (one promote), and state that in plan sec 11 as a
  hard ordering rule; or
- Move the 17 -> 18 bump out of WP1 into the commit that lands the second of
  WP2/WP3, so neither side announces 18 before both consume 18; or
- Make WP3's ESP DIAG parser and V3 clear builder land before WP2 (WP3 depends
  only on WP1), and add "WP2 depends on WP3's DIAG/clear part" to the table.
Also worth adding: a KilnFW host test that feeds a 32-byte DIAG to
`safety_apply_diag()` and expects it accepted whenever the build announces
>= 18, so this mismatch fails a test instead of a bench run.

### MED-1: version 18 announced before any 18 behaviour exists

Same root as HIGH-1, the other direction. A Pico image built from WP1-only code
announces 18 but has no 0x2E handler (link_task.c's default dispatch drops
unknown opcodes silently), no DIAG V3 emit and no F6 clear rule. WP3's ESP gate
("a frame is sent only to a peer known to be >= 18", plan line 48) treats it as
TEST_TRIP-capable: `POST /api/safety/test_trip` then times out with no
`TEST_TRIP_RESULT`, and `REFUSED_PEER_VERSION` is never reported because the
peer did announce 18. The exact protocol-equality gates
(`pico_image_embedded.h:160`, `ota_http_pico.c:313`) do not help, because both
sides say 18.

Scenario: a bench or release Pico flashed from any dev commit between WP1 and
WP2, paired with a WP3 ESP.

Fix: same as HIGH-1 (bump the version where behaviour lands), or have WP3
report a no-reply TEST_TRIP as "peer did not answer (pre-WP2 firmware?)"
rather than a generic timeout. Document in LINK_PROTOCOL.md that 18 is only
meaningful from the WP2/WP3 landing commit on.

### LOW-1: LINK_PROTOCOL.md compatibility text is inaccurate

`LINK_PROTOCOL.md:847-849` says "An old peer ignores an unknown opcode, and
DIAG/CLEAR_TRIP decode all lengths, so `KILNLINK_MIN_COMPATIBLE` stays 7."
A 17 peer rejects the 32-byte DIAG and the 5-byte CLEAR_TRIP (exact length
sets). What keeps 7 correct is that every new length and opcode is sent only to
a peer that announced >= 18, not tolerant decoding. `LINK_PROTOCOL.md:539`
("Both lengths decode on both sides") predates the third length and now reads
as if only two exist.

Scenario: someone adds a 19 field trusting "old peers decode all lengths" and
sends it ungated.

Fix: reword 847-849 to "an old peer ignores an unknown opcode, and the
extended DIAG/CLEAR_TRIP lengths are sent only to a peer that announced >= 18,
so MIN_COMPATIBLE stays 7"; change 539 to "all three lengths decode on an 18
build". MIN_COMPATIBLE = 7 itself is still right.

### LOW-2: V3 forms missing from the shared vector JSON; weak C DIAG vector

`firmware/CommonFW/test/vectors/clear_trip_vectors.json` holds only the 3- and
4-byte vectors and `diag_vectors.json` only V1/V2. These are not just docs:
`tools/PcTools/tests/test_kilnlink_commonfw_vectors.py` re-encodes them with
`kilnlink_codec.py`, so they are the one C<->Python cross-check. The V3 forms
have none:
- CLEAR_TRIP V3 is pinned in Python by a literal (`0a08000511`) that matches
  the C vector, so it is covered in practice, but by hand-copy.
- DIAG V3: the C test `test_vector_boot_id` checks only bytes 0, 30 and 31 of
  32; the Python test checks length and bytes 30/31. Bytes 1..29 are shared
  with the V1/V2 path and pinned there, so this is a gap in form, not a live
  bug.
- Negtest (below): the Python codecs' default for an absent `trip_seq` when
  `pico_boot_id` is present (writes 0) is unpinned; changing it to 0x55 in
  either `encode_diag` or `encode_clear_trip` passed every test (P1, P3 MISSED).

Fix: add one V3 vector to each JSON (e.g. the existing C vectors) so the
PcTools vector test covers them, and pin the absent-trip_seq case (encode
`{"trip_mask":8,"pico_boot_id":0x11}` -> `0a08000011`).

### LOW-3: stale `safety_guards.h` comments for reason 4

`safety_guards.h:87-90` still says "4 and 11 are reserved gaps (S4 and S10 are
WARN-only, never produce a trip code)" and that the enum is "verbatim from
ARCHITECTURE.md section 9"; around lines 898-901 the comment says the S4/S10
codes stay free so a WARN-only guard "has a slot without colliding with any
TRIP guard's bit". With `SAFETY_TRIP_TEST = 4`, trip_mask bit 3 is now TEST
while warn_mask bit 3 is still S4. Anything that assumed "bit N means the same
guard in both masks" (the safety page's "per-guard bitmasks" note, any UI that
shares a bit-to-guard table between the two masks) is now wrong for bit 3.

Scenario: a future UI change reuses the warn-mask table to label trip_mask and
shows a test trip as "S4 warn".

Fix: update both comments (WP2 owns ARCHITECTURE.md sec 9), and state in
LINK_PROTOCOL.md that trip_mask and warn_mask bit 3 differ. The checks
`check_safety_trip_mask_docs.ps1` and `check_safety_trip_words_sync.ps1` both
PASS, so neither catches this.

### INFO

- INFO-1: `test_safety_link_compile.c` (~2167) `s_compat_frames` has no 0x2F
  row at min_version 18. WP3 must add it or a WP1-era ESP counts 0x2F as
  unmatched. (0x2A REBOOT_RESULT and CT_AUTO_ZERO_STATUS were already missing.)
- INFO-2: `safety_trip_words.h` has no case 4, so reason 4 shows "unknown
  guard" until WP3. `check_safety_trip_words_sync.ps1` does not require a word
  per enum value.
- INFO-3: `pico_image_embedded.h:160` and `ota_http_pico.c:313` require exact
  protocol equality, so a 17/18 mixed pair refuses Pico auto-update and forces
  the coordinated dual reflash (plan sec 7). Expected, worth a line in sec 7.
- INFO-4: the plan's WP1 item "PcTools `protocol.py` ids" was skipped on
  purpose (it would force a UART bump); the commit note and plan row say so.
- INFO-5: the C encoders write `dg->trip_seq` / `msg->trip_seq` as given when
  only `has_boot_id` is set; Python writes 0 when `trip_seq` is absent. Same
  bytes for any zero-initialised caller; differs only for a caller that sets a
  non-zero `trip_seq` with `has_trip_seq=false`.
- INFO-6: no codec path can clear or mask a trip. TEST_TRIP decode only fills a
  struct; TEST_TRIP_RESULT refuses outcome > 7 on encode and decode; 0x2E/0x2F
  collide with no existing request or reply id. `SAFETY_TRIP_TEST = 4` is used
  only by the enum, the PcTools name table and the mask-doc rows at this commit.

## Questions answered

- 17 <-> 18 wire compatibility: safe only because of peer gating, both ways.
  A 17 Pico drops 0x2E silently; a 17 ESP never sees 32/5-byte frames as long
  as the Pico gates on the announce. MIN_COMPATIBLE = 7 stays right. The live
  risk is an 18-announcing build with 17 consumers (HIGH-1, MED-1).
- DIAG V3 / CLEAR_TRIP V3 lengths: exact sets {30,31,32} and {3,4,5};
  `has_trip_seq = len >= V2`, `has_boot_id = len == V3`; offsets read only after
  the length check, so no OOB read. NULL payload is not checked, same as the
  older codecs.
- Outcome enum closed (0..7), magic verbatim, no opcode collision: confirmed.
- Frozen vectors and negtests: meaningful for the C codecs (all four C
  mutations below CAUGHT). Python V3 coverage is thinner (LOW-2).
- PcTools mirrors: match the C exactly (INFO-5 aside).
- Vector JSON left unchanged / `check_commonfw_diag_vectors.ps1` not run by the
  commit: the check passes now; the JSON gap is LOW-2.
- Other users of 4: none at this commit beyond the enum, PcTools names and the
  mask docs; see LOW-3 for the comment and warn-mask bit meaning.

## Tests run (worktree at 91e784758)

- `firmware/CommonFW/test/check_commonfw_ctest.ps1`: PASS, 44/44.
- `firmware/CommonFW/test/check_commonfw_diag_vectors.ps1`: PASS.
- `firmware/KilnFW/App/test/check_wire_protocol_fingerprint.ps1`: PASS.
- PcTools pytest `test_kilnlink_capture.py test_kilnlink_codec.py
  test_kilnlink_commonfw_vectors.py test_safety_diag.py`: 110 passed.
- `check_safety_trip_mask_docs.ps1`, `check_safety_trip_words_sync.ps1`: PASS.

Negtests (`tools\negtest.ps1`):

Preset check over `firmware/CommonFW/test/check_commonfw_ctest.ps1`, all CAUGHT:
- C1 DIAG encode writes trip_seq only when `has_trip_seq` (V3 with only `has_boot_id`): CAUGHT (test_diag.c:374, 381, 411).
- C2 CLEAR_TRIP encode, same mutation: CAUGHT (test_clear_trip.c:231, 245).
- C3 CLEAR_TRIP decode sets `has_boot_id` on the 4-byte form: CAUGHT (test_clear_trip.c:254).
- C4 DIAG decode sets `has_boot_id` on the 31-byte form: CAUGHT (test_diag.c:392).

Python, preset pytest over the four files above:
- P1 `encode_diag` absent trip_seq writes 0x55: MISSED (LOW-2).
- P2 DIAG V3 tail bytes swapped in both codec and capture: CAUGHT
  (`test_v18_diag_pico_boot_id_round_trip` pins raw bytes 30/31).
- P3 `encode_clear_trip` absent trip_seq writes 0x55: MISSED (LOW-2).
