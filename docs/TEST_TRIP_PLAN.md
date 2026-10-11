# TEST_TRIP link command plan

Owner decision 2026-10-10 ("Add, fold into bump"): add an admin-only Pico
`TEST_TRIP` command on the ESP-Pico link. It latches a dedicated trip reason
through the real trip path, and that trip clears only by the normal trip-clear
rules. It needs a new kilnlink opcode, so the link protocol bumps by one. F6
from `docs/audits/SAFETY_LINK_REVIEW_2026-10-09.md` (bind the clear to the
Pico boot_id) rides in the same bump
(`docs/audits/REVIEW_SAFETY_LINK_FIX_2026-10-10.md`, "F6 recommendation").

Why: AX-T02 (a safety trip drops spare relay 4) and other trip-path bench cases
need a real Pico trip. Today the only ways to get one are physical (E-stop,
pulling the link) or OT-B01's dual-reflash S6a, so these cases are
operator-only. With TEST_TRIP they can run unattended.

Status: plan only, nothing built. Line numbers below are at dev `68e3b4966`.

## 0. Rules this design must keep

1. **The command can only cause a trip.** It never clears, masks, delays or
   blinds one. A test trip that is already latched is cleared by
   `SAFETY_CMD_CLEAR_TRIP` and nothing else. No new clear path is added
   (owner rule: no software path clears a trip outside the documented rules;
   memory `project_owner_decision_no_guard_disable`).
2. **The real latch path.** The Pico latches the trip in
   `safety_guards_tick()`, so `safety_core_task()`'s `newly_tripped` branch
   runs unchanged (`tasks/safety_core.c:1447-1520`). That branch sets
   `s_trip_command_owed`, which makes `relay_owner` de-energise K4 and drop the
   heat enable. It calls `boot_reason_latch_trip()`, writes the Frame D
   snapshot, bumps `trip_seq` and sends `TRIP_EVENT`. No simulated flag,
   no ESP-only state, no shortcut around `relay_owner`.
3. **No guard is disabled while the test trip is latched.** S9 keeps
   evaluating after any trip (`safety_guards.c:403+`). A test trip is
   therefore also a real check that K4 opened.
4. **Abuse is fail-safe.** Someone with admin access could keep a kiln tripped
   by sending TEST_TRIP repeatedly. That is a denial of heat, which is the
   safe direction, and is accepted. An admin can already stop a firing.

## 1. Version: 17 -> 18

The memory note says kilnlink is 16. That is stale. The current value is
**17** (`CommonFW/include/kilnlink/kilnlink_version.h:358`, the 2026-10-09 M4
bump that bound CLEAR_TRIP to `trip_seq`). This plan bumps it **17 -> 18**.

`KILNLINK_MIN_COMPATIBLE` stays at **7**. Every change below is additive and
gated both ways, the same shape as 16 -> 17:

- a frame or trailing byte is sent only to a peer known to be >= 18;
- an 18 build still accepts every 17-form frame from a 17 peer;
- a 7..17 peer sees exactly the frames it already knows.

The `kilnlink_version.h` comment gets a `17 -> 18` paragraph, and the
`MIN_COMPATIBLE` comment gets a "NOT bumped alongside 17 -> 18" line.
`UART_PROTOCOL_VERSION` (PC<->ESP) does not change
(`check_uart_version_independence.ps1`).

## 2. Wire format

### 2.1 Opcodes

The free ids after `APPLY_CONFIG_VOLATILE` (0x2D) are used. Request and reply
get separate ids, per LINK_PROTOCOL.md's "request/reply ids must never be
shared" rule.

| Id | Name | Direction | Length |
|---|---|---|---|
| `0x2E` | `SAFETY_CMD_TEST_TRIP` | ESP -> Pico | 4 |
| `0x2F` | `SAFETY_CMD_TEST_TRIP_RESULT` | Pico -> ESP | 4 |

Before taking the ids, WP1 greps all three trees and PcTools
(`protocol.py`, `kilnlink_codec.py`) to confirm 0x2E/0x2F are unused.

`TEST_TRIP` (ESP -> Pico), fixed length 4, little endian, serialised field by
field (CommonFW README rules 1-6):

| Offset | Field | Meaning |
|---|---|---|
| 0 | cmd | `0x2E` |
| 1 | `pico_boot_id` u8 | The Pico boot_id the ESP read from the same DIAG it checked (sec 6). The Pico refuses a mismatch, so a request queued against a previous Pico boot never fires. |
| 2 | `request_id` u8 | Chosen by the ESP. The Pico echoes it, and it is the dedup key for retries. |
| 3 | `magic` u8 | `0xA5`. A cheap check against a corrupted or mis-dispatched frame. The CRC already covers this, so it is defence in depth only. |

`TEST_TRIP_RESULT` (Pico -> ESP), fixed length 4:

| Offset | Field | Meaning |
|---|---|---|
| 0 | cmd | `0x2F` |
| 1 | `request_id` u8 | Echoed from the request. |
| 2 | `outcome` u8 | See below. |
| 3 | `trip_seq` u8 | The `trip_seq` of the latched test trip when outcome is ACCEPTED, else 0. |

Outcomes (`kilnlink_test_trip_result.h`, closed enum; any other value fails
decode):

| Value | Name | When |
|---|---|---|
| 0 | `ACCEPTED` | Queued to safety_core. The trip latches on the next safety_core tick (100 ms period). |
| 1 | `REFUSED_ALREADY_TRIPPED` | Some trip is already latched. The command never replaces or re-labels an existing trip. |
| 2 | `REFUSED_BOOT_ID` | `pico_boot_id` is not this boot's. |
| 3 | `REFUSED_PEER_VERSION` | The peer has not announced >= 18 this session. |
| 4 | `REFUSED_RATE_LIMIT` | Less than `TEST_TRIP_MIN_INTERVAL_MS` since the last accepted test trip. |
| 5 | `REFUSED_UPDATING` | `update_task` has a Pico flash in progress. |
| 6 | `REFUSED_BAD_FRAME` | Wrong magic. |
| 7 | `DUPLICATE` | Same `request_id` as the last accepted one within the dedup window. The original outcome and `trip_seq` are re-sent and nothing new latches. |

The reply is sent as a short repeated burst, the same as `REBOOT_RESULT`
(`kilnlink_reboot_result.h`). The Pico never waits for the ESP
(LINK_PROTOCOL.md sec 2). A lost reply is harmless: the ESP sees the trip in
the next DIAG.

### 2.2 DIAG and CLEAR_TRIP (F6, sec 6)

- `SAFETY_CMD_DIAG` (0x08) gains a trailing `pico_boot_id` u8:
  `KILNLINK_DIAG_LEN_V3 = 32`. The Pico sends it only to a peer that announced
  >= 18. A 17 peer keeps getting the 31-byte frame, and a 16 peer the 30-byte
  frame.
- `SAFETY_CMD_CLEAR_TRIP` (0x0A) gains a trailing `pico_boot_id` u8:
  `KILNLINK_CLEAR_TRIP_LEN_V3 = 5` (cmd, mask u16, trip_seq, boot_id). The ESP
  sends it only when its cached DIAG was the 32-byte form. The decoder accepts
  3, 4 and 5.

## 3. Trip reason and mask

`safety_trip_t` is `SAFETY_TRIP_NONE = 0` through `SAFETY_TRIP_SELF_TEST = 16`,
with 4 and 11 reserved (`SaftyFW/src/safety_guards.h:91-109`). The mask is
`1 << (reason - 1)` (`link_frame_trip_mask_for_reason()`,
`tasks/link_frame.c:248-254`), and it is a **u16** on the wire (DIAG
`trip_mask`, CLEAR_TRIP `trip_mask`).

**Value 17 cannot be used.** `1 << 16` does not fit the u16 mask, and
widening the mask would change DIAG and CLEAR_TRIP on all three trees plus
every PC decoder. That cost is out of proportion to a test facility.

**Recommendation: `SAFETY_TRIP_TEST = 4`, mask `0x0008`.** Value 4 is the
reserved gap for S4, which is WARN-only by design and never produces a trip
code (`SaftyFW/docs/ARCHITECTURE.md` sec 9, lines 576 and 593). No log,
screenshot or record has ever carried reason 4, so giving it a meaning
reinterprets no history. That history is the only reason the enum comment
forbids renumbering. A pre-18 ESP never receives reason 4, because the Pico
latches it only on a TEST_TRIP, which only an 18 ESP sends. If such an ESP
did see it, it would show "unknown guard", which is still fail-safe.

The alternatives are open question Q1: 11 (mask `0x0400`, the S10 gap), or
widening the mask to u32.

The update touches every copy of the enum and its words:

- `safety_guards.h` enum, `ARCHITECTURE.md` sec 9 enum and the "bit position"
  text (lines 576, 593, 626-633), `SAFETY_MODEL.md` trip table.
- KilnFW word tables, which must change together
  (`safety_trip_words.h` short/cause/action,
  `profile_executor.h:184` `profile_executor_safety_trip_words()`,
  `main_page.html` `SAFETY_TRIP_WORDS`).
- PcTools `devices_safety.py` / `kilnlink_codec.py` reason names.

Wording:

- short: `"TEST trip (admin)"`
- cause: `"An administrator triggered a test trip from the web or a tool. No
  guard detected a fault."`
- action: `"Clear the trip from the Safety page when the test is done."`
- the fault sentence: `"safety test trip requested by administrator (TEST)"`

## 4. Pico side (SaftyFW)

### 4.1 Acceptance (link_task, core 0)

The decision is a pure function, `link_frame_decide_test_trip()`, in
`link_frame.c` next to `link_frame_decide_clear_trip()` so it can be
host-tested. Its inputs are the current reason, the frame's boot_id and
magic, the peer protocol version, the ms since the last accepted test trip,
`update_task_busy` and the dedup state. The checks run in this order:

1. Bad magic: `REFUSED_BAD_FRAME`.
2. Peer version < 18 or unknown (0): `REFUSED_PEER_VERSION`. Only an 18 ESP
   can send this frame legitimately, so an unknown version means the
   ANNOUNCE has not arrived yet this session.
3. `pico_boot_id` != own boot_id: `REFUSED_BOOT_ID`.
4. Same `request_id` as the last accepted, within 10 s: `DUPLICATE`.
5. `update_task` busy: `REFUSED_UPDATING`.
6. Any trip latched (`current_trip_reason != NONE`): `REFUSED_ALREADY_TRIPPED`.
7. Less than `TEST_TRIP_MIN_INTERVAL_MS` (recommend 10 000 ms) since the last
   accepted: `REFUSED_RATE_LIMIT`.
8. Otherwise `ACCEPTED`.

The link is wired traces with CRC and sequence dedup but no authentication
(owner decision, memory `project_owner_decision_abort_stopwatch_pico_halt`).
"Healthy" therefore means checks 2 and 3: an announced >= 18 peer in this
session, talking to this boot. Authorisation lives on the ESP (sec 5).

GRACE and ARMED do not matter. A trip during GRACE is already a real case
(`safety_core.c:1400-1410`), and an accepted test trip latches in either
state. Accepting while the context is stale is fine too: it can only trip.

### 4.2 Latching

- link_task posts a request to a new depth-1 queue `s_test_trip_queue`, using
  a 0-tick send, the same pattern as `s_clear_trip_queue`. If the send fails,
  the reply is `REFUSED_ALREADY_TRIPPED`. That cannot happen while check 6
  holds, but the code must not claim acceptance it did not get.
- `safety_core_build_input()` drains the queue into a new input field,
  `safety_guard_input_t.test_trip_requested` (bool, one tick only).
- `safety_guards_tick()` treats `test_trip_requested` as a guard condition
  with no debounce. It is evaluated in the not-tripped branch after every
  real guard, so a real guard that fires on the same tick wins the reason.
  It sets `is_tripped = true` and `reason = SAFETY_TRIP_TEST`.
- Nothing else changes. `newly_tripped` is true and the existing branch does
  steps 1-4 of the trip order in SAFETY_MODEL.md sec 6.
- link_task builds the ACCEPTED reply's `trip_seq` from
  `safety_core_get_trip_event()` after the latch. If the latch has not
  happened by the time the reply is due, the reply carries 0 and the ESP
  reads the seq from DIAG. Recommendation: reply after the latch, polling up
  to 300 ms. The Pico still never blocks on the ESP.

### 4.3 Clearing (normal rules, no new path)

A test trip is cleared by `SAFETY_CMD_CLEAR_TRIP` through the existing chain:
`link_frame_decide_clear_trip()` (mask match, occurrence-bound seq, plus the
F6 boot_id), the queue, `safety_guards_clear_trip_occurrence_matches()`,
`safety_guards_try_clear()`, and the `relay_owner` clear.

`guard_condition_still_immediate()` gets a `SAFETY_TRIP_TEST` case. The
condition stays "still true" until the trip has been latched for
`trip_verify_s` (the S9 verify window; `safety_guard_state_t` already tracks
`s9_verify_elapsed_s`). This adds no new way to clear. It makes the test trip
harder to clear than "immediately", so a clear cannot cut S9's check that K4
opened short. After the window, the clear is accepted like any other.

S9 escalation applies unchanged. If current still flows after a test trip,
the reason becomes `SAFETY_TRIP_INEFFECTIVE` and cannot be cleared, exactly
as for a real trip. On this bench S9 is uncommissioned and only warns
(`safety_guards.c`, `s9_uncommissioned_warn`).

### 4.4 Boot reason and reboot

`boot_reason_latch_trip(4)` records the test trip like any other. If the Pico
reboots while a test trip is latched, the ESP's benign/fatal reboot classifier
(memory `project_owner_decision_pico_reboot_resume`) must treat a pre-reboot
test trip exactly as it treats any other pre-reboot latched trip, no more
leniently. WP2 adds a host test that pins this.

## 5. ESP side (KilnFW)

### 5.1 Link send

`safety_link_send_test_trip(SafetyLinkClass *link, uint8_t request_id,
safety_test_trip_outcome_t *out, uint8_t *out_trip_seq)` follows the
`safety_link_send_reboot()` shape: a send burst, then a bounded reply window
(recommend 1500 ms) under the link's existing transaction lock. It refuses
locally, without sending, when:

- the cached DIAG is stale (> `SAFETY_LINK_STALE_MS`), or is not the 32-byte
  form, meaning the Pico is < 18 (`ESP_ERR_NOT_SUPPORTED`);
- the cached DIAG already reports TRIPPED (`ESP_ERR_INVALID_STATE`);
- the ESP is in recovery mode (`boot_guard_is_recovery_mode()`).

`pico_boot_id` comes from the same cached DIAG. The result is reported from
`TEST_TRIP_RESULT` when one arrives. With no reply, the result is a timeout,
and the caller decides from the next DIAG.

### 5.2 HTTP route

`POST /api/safety/test_trip`, `ROUTE_TIER_ADMIN`, registered in
`dashboard_http.c` next to `/api/safety/clear_trip`, with a row in
`route_tier_table.h`.

- Body: `confirm=1` is required, otherwise 400 `confirm_required`.
- CSRF: the existing cross-origin refusal for state-changing requests applies
  automatically.
- Responses:
  - 200 `{"ok":true,"outcome":"accepted","trip_seq":N,"trip_reason":4,"trip_mask":8}`
  - 409 `{"ok":false,"outcome":"already_tripped"|"rate_limited"|"updating"}`
  - 501 `{"ok":false,"outcome":"pico_protocol_too_old","pico_protocol":17}`
  - 503 for a stale link or a reply timeout. The body says "check
    /api/safety status", because a timeout does not mean nothing tripped.
- Not mode-gated. A test trip mid-firing is a supported case (sec 5.4), and
  the system-mode gate exists to stop writes that change heat behaviour while
  running. A trip only removes heat.
- Route cap: `check_uri_handler_cap.ps1` today reports 175 routes against
  `max_uri_handlers = 184`, so 9 slots are spare, and
  `KILN_HTTP_MAX_ROUTES = 192`. One new route leaves 8. No cap bump is needed,
  but WP3 re-runs the check.
- The recovery image (`firmware/KilnFW_recovery/`) gets **no** test trip
  route. Its `recovery_pico_proto.*` copy only needs to compile against the
  new `kilnlink_version.h`.

### 5.3 MCP tool

`safety_test_trip(confirm=False)` in `mcp_server_safety.py`:

- Refuses unless `confirm is True` exactly.
- Reads `safety_get_status()` first and refuses if a trip is already latched.
- POSTs through `http_auth.urlopen()`, which needs an admin session.
- Re-reads status after the POST and fails loudly unless it shows
  `trip_reason == 4` and `trip_mask == 0x0008`, computed with the formula,
  not a constant.
- Reports `trip_seq`.

No `safety_test_trip_clear` is added. The test clears with the existing
`safety_clear_trip()`, which keeps a single clear path. The facade count goes
231 -> 232, and CLAUDE.md's count paragraph and `docs/MCP_SERVERS.md` are
updated.

### 5.4 Run state: identical to a real trip

Nothing on the ESP branches on "test" for control purposes.

- A fresh DIAG with TRIPPED goes into `profile_executor_wd_decide()`
  (`profile_executor.h:225`). A RUNNING or PAUSED run goes to **FAULTED**
  with `"safety processor tripped: safety test trip requested by
  administrator (TEST), firing aborted"`. All relays go off and the relay
  claim is released.
- Idle: `LOG_IDLE_TRIP`, same as now.
- `safety_trip_decision.c` dedups the `TRIP_EVENT` the same way.
- heat_enable drops the grant through the existing reconcile against the
  Pico-reported K4 (review F1).
- Aux outputs drop on any safety fault (`aux_outputs`, review 4 M1). This is
  what AX-T02 checks.
- The run log, firing history and `fault_reason` carry the words above, so a
  test trip is never mistaken for a real one afterwards.

The only allowed difference is wording. WP3 adds a host test asserting that
`profile_executor_wd_decide()` gives the same action for reason 4 as for
reason 1.

### 5.5 Operator visibility

- LCD home and Safety page show `safety_trip_words_short(4)`, "TEST trip
  (admin)". The clear button follows the 2026-10-07 rule: the LCD clear needs
  admin login and follows the web clear.
- The web Safety page banner shows the cause and action sentences, plus a
  "test" badge so a screenshot cannot be read as a real fault.
- `GET /api/status` / `/api/safety` already expose `trip_reason` and
  `trip_mask`. Add `"trip_is_test": true` as a convenience, derived from
  reason == 4, so it adds no state.
- The readiness `safety_trip` item reads not-ok while the trip is latched,
  unchanged.

## 6. F6: bind the clear to the Pico boot_id

Today the bound token is `0x100 | seq` (`link_frame.c:263+`,
`safety_guards.c:295`). Both Pico boots number their first trip 1. A delayed
CLEAR_TRIP from the previous boot, with the same seq and mask, can therefore
clear the new boot's trip once its condition has gone. Separately, an unbound
3-byte clear is accepted while the Pico's peer version is unknown.

The design, for 18 peers:

1. **Clear binds to boot_id.** DIAG V3 carries the Pico's `boot_id`
   (sec 2.2), so mask, seq and boot_id come from one frame and stay
   consistent. CLEAR_TRIP V3 echoes all three.
   `link_frame_decide_clear_trip()` gains `frame_has_boot_id`/`wire_boot_id`,
   and a new refusal `LINK_CLEAR_TRIP_REFUSE_BOOT_ID` when
   `wire_boot_id != own boot_id`.
2. **The form required depends on the announced peer version:**
   - 18 or later: only the 5-byte form is accepted. A 3- or 4-byte frame gets
     `REFUSE_BOOT_ID_REQUIRED`.
   - 17: the 4-byte form is required, as today.
   - 7..16: the 3-byte form is accepted, as today.
3. **Unknown peer version (second F6 bullet).** The Pico refuses a 3-byte
   clear while the peer version is unknown (`REFUSE_PEER_VERSION_UNKNOWN`),
   with a fallback: after `CLEAR_UNKNOWN_PEER_FALLBACK_MS` (recommend 30 s)
   from the start of the ESP session with no ANNOUNCE, the legacy path opens
   again. Every ESP since protocol 6 sends ANNOUNCE, so the refusal only bites
   while ANNOUNCE is in flight or lost. The fallback keeps a pre-17 ESP after
   a rollback from being locked out by one lost ANNOUNCE. An 18 ESP re-sends
   ANNOUNCE when DIAG reports this refusal. Whether to have the fallback at
   all is open question Q2.
4. **The ESP side** already forgets its cached `diag_trip_seq` on a Pico
   boot_id change (`safety_link_frames.c:275-281`). It must forget the cached
   boot_id-bearing DIAG on the same event (pairs with the reset-one-side
   checklist in CLAUDE.md). It never builds a V3 clear from a DIAG older than
   `SAFETY_LINK_STALE_MS`, as today.
5. **Boot-time S6a clear.** The ESP announces at link-up before it sends any
   clear, so an 18 pair uses the bound V3 clear for the dual-reflash S6a too.
   The 18 ESP must wait for the first 32-byte DIAG before clearing. The
   `flash_firmware()`/bench procedure clears by `safety_clear_trip()`, which
   already reads DIAG first.

Residual risk: `boot_id` is 8 random bits (`link_task_start()`,
`get_rand_32`), so two consecutive boots collide with probability 1/256, and
F6's replay then still needs the same seq, the same mask and a gone
condition. This is accepted, as in the review's recommendation.

TEST_TRIP's own `pico_boot_id` check (sec 4.1, check 3) uses the same field,
so the two features share one source of truth.

## 7. Version skew and the coordinated dual reflash

Behaviour of a mixed pair (none of these is an unsafe state):

| ESP | Pico | TEST_TRIP | CLEAR_TRIP form |
|---|---|---|---|
| 18 | 18 | available | 5-byte, boot_id bound |
| 18 | 17 | route answers 501 `pico_protocol_too_old` | 4-byte (DIAG is 31 bytes) |
| 17 | 18 | not sent (no route) | 4-byte; the Pico sends 31-byte DIAG |
| 16 or older | 18 | not sent | 3-byte; legacy path |

Procedure (the bench is pre-authorised; memory
`project_owner_decision_bench_free_use_no_usb_recovery`):

1. Land WP1-WP4 on dev. `build_kilnfw` builds SaftyFW first and embeds both
   slot images, so the ESP image carries the 18 Pico image
   (`docs/PICO_AUTO_UPDATE_PLAN.md`).
2. Flash the Pico with `debug_program(peer="pico")`. Under the 2026-10-10
   decision, an unreadable ESP state flashes with a warning. A 17 ESP keeps
   talking to the 18 Pico through the skew rows above.
3. Flash the ESP with `flash_firmware()` (never `debug_program(peer="esp")`,
   never esptool), with verify on and the default boot_guard reset.
4. Expect S6a: `trip_reason == 6`, `trip_mask == 1 << 5 == 0x0020`. Confirm
   link-up with `safety_get_status()`, that the reason is only S6a, and that
   the cached DIAG is the 32-byte form (`safety_get_diag()` shows
   `pico_boot_id`). Then `safety_clear_trip()`, which must go out as a V3
   clear.
5. Confirm both sides report protocol 18 (`safety_get_fw_version`,
   `get_heap_status`), that there is no unacknowledged crash, and that
   `check_task_liveness` is clean.
6. Smoke-test: `safety_test_trip(confirm=True)` while idle. Check K4 open,
   reason 4, mask 0x0008, LCD capture shows "TEST trip", and a clear before
   `trip_verify_s` is refused. Wait out the window, clear, and confirm
   ARMED/idle again.
7. If the ESP boots the auto-update path instead, it pushes the embedded 18
   Pico image itself. Step 2 then becomes a no-op check.

Rollback: an ESP rollback to 17 against an 18 Pico is a supported skew row,
with no TEST_TRIP and a 4-byte clear. The `zones_cfg` rollback hazard in
CLAUDE.md is unrelated, but still read back `control_get_zones` before
heating.

## 8. Tests

**CommonFW / host codecs** (`firmware/CommonFW/test`):

- TEST_TRIP and TEST_TRIP_RESULT round-trip.
- Wrong length, wrong cmd and an unknown outcome are rejected.
- DIAG 30/31/32 decode; CLEAR_TRIP 3/4/5 decode.
- A frozen byte vector for each new frame.

**SaftyFW host tests:**

- `link_frame_decide_test_trip()`: each refusal, the order of checks, and
  DUPLICATE.
- `safety_guards_tick()` with `test_trip_requested`:
  - latches reason 4 with no debounce;
  - a real guard on the same tick wins the reason;
  - ignored while already tripped.
- `guard_condition_still_immediate(TEST)`: refuses inside `trip_verify_s`,
  allows after it.
- `safety_guards_try_clear()`: a test trip clears only through the normal
  path, and S9 escalation to INEFFECTIVE still blocks the clear.
- `link_frame_decide_clear_trip()` boot_id cases:
  - V3 match is accepted, V3 mismatch is refused;
  - an 18 peer sending V2 is refused;
  - unknown peer: refused, then the fallback opens.
- `link_frame_trip_mask_for_reason(4) == 0x0008`.
- Negative tests with `tools\negtest.ps1 -Preset saftyfw-host`: drop the
  boot_id compare, drop the already-tripped refusal, make the TEST guard
  condition always false. Each must be CAUGHT.

**KilnFW host tests:**

- `safety_link_send_test_trip()` local refusals (stale, < 18, tripped,
  recovery).
- V3 clear built only from a 32-byte DIAG; the boot_id cache is cleared on a
  Pico boot_id change.
- `profile_executor_wd_decide()` with reason 4 equals reason 1 (FAULT).
- Word tables have an entry for 4 in all three C/JS copies (extend the
  existing sync check if there is one, else add `check_trip_words_sync.ps1`).
- Route handler: confirm gate, outcome-to-status mapping.
- Negative tests with the `kilnfw-host` preset and `-ExpectPattern`, per
  `rules.txt`.

**PcTools:**

- `safety_test_trip` tool against a fake board: the confirm gate, the
  pre-tripped refusal, read-back failure.
- `kilnlink_codec.py` decodes the new frames for `saleae_decode_kilnlink`.

**Checks:** `check_uri_handler_cap.ps1`, the route tier table/registration
check, `check_uart_version_independence.ps1`, and the recovery
`check_recovery_pico_proto.ps1`.

## 9. Bench cases to convert or add

All of them use `safety_test_trip()`. None needs `--attended` any more. They
stay under the existing write gates (`KILNCTL_AUX_BENCH_CONFIRM` for AX, the
ctx-suite gate for SP/HP). Each case clears its own trip, verifies the clear
by read-back, and taints the run if the clear fails.

| Case | Change |
|---|---|
| **AX-T02** | No longer operator-only. Hold the rule ON, call `safety_test_trip`, and require: relay 4 OFF within 2 s (relay shadow), the run FAULTED with the test wording, `trip_mask == 1 << (trip_reason - 1)` with reason 4. Wait `trip_verify_s`, clear, verify. Update the `operator_only` flag in `registry.py:330`, and update `cases_aux.py:24` and BENCH_TEST_SYSTEM_PLAN sec AX. |
| **SP-12 (new)** | Test trip latch/clear while idle: K4 de-energised, reason/mask, a clear inside `trip_verify_s` is refused, accepted after it, back to ARMED. `TRIP_EVENT` seen once (dedup). |
| **SP-13 (new)** | TEST_TRIP refusals: a second request while tripped gets 409 `already_tripped` and the latched reason/seq are unchanged; one inside 10 s gets `rate_limited`. Proves the command cannot re-label or mask an existing trip. |
| **SP-14 (new)** | F6 on the bench: dual reset, then the S6a clear goes out in V3 form (`safety_get_link_stats` clear-outcome counters). Replaces nothing; extends SP-04. |
| **HP-09 (new)** | Test trip mid-firing (HP-01 profile, at 60 s): run FAULTED within one watchdog period, all duties 0, relays off, heat enable dropped, aux off. Then the clear, and a new start is allowed. This is the unattended version of SP-09's downstream half. |
| SP-08, SP-09 | Stay operator-only. They test guard detection (S7 input, S6b liveness), which TEST_TRIP does not touch. Their downstream checks can cite SP-12/HP-09. |
| HP-07 | Unchanged (a zone-limit fault, not a Pico trip). |

The `registry.py` judge-less ratchet test needs the new ids registered with
judges in the same change.

## 10. Doc updates

- `firmware/CommonFW/docs/LINK_PROTOCOL.md`:
  - new sections for `SAFETY_CMD_TEST_TRIP` (0x2E) and
    `SAFETY_CMD_TEST_TRIP_RESULT` (0x2F);
  - DIAG V3 and CLEAR_TRIP V3 in the existing sections;
  - the version-compatibility table gains an 18 row.
- `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`:
  - a host-test row block for TEST_TRIP and F6;
  - sec 3.4 gains a "TEST (reason 4)" row: provoke with
    `safety_test_trip`, expected K4 open, clear refused inside
    `trip_verify_s`, then accepted;
  - a note that it does not exercise any guard's detection.
- `ARCHITECTURE.md` sec 9 and `SAFETY_MODEL.md`: the enum value and the mask
  text.
- `docs/BENCH_TEST_SYSTEM_PLAN.md`: AX-T02 and the new SP/HP rows.
- `CLAUDE.md`: the MCP count paragraph (231 -> 232); kilnlink is 18. Also
  correct the memory note that says 16.
- `docs/MCP_SERVERS.md`: the tool entry.
- The audit docs: mark F6 as fixed with the SHA once it lands.
- `ROADMAP.md`: the row added with this plan; update it as WPs land.

## 11. Work packages

Each WP is sized for one implementer. WP1 lands first; WP2 and WP3 can then
run in parallel; WP4 needs WP3's tool; WP5 runs last and needs the bench.

| WP | Tree | Content | Depends |
|---|---|---|---|
| **WP1** CommonFW wire | `firmware/CommonFW` | Version 18 and comments. New `kilnlink_test_trip.h/.c` and `kilnlink_test_trip_result.h/.c`. DIAG V3 (32 B) and CLEAR_TRIP V3 (5 B) codecs. Codec host tests with frozen vectors. PcTools `protocol.py` ids and `kilnlink_codec.py` decode. Recovery image compile check. LINK_PROTOCOL.md sections. | none |
| **WP2** SaftyFW | `firmware/SaftyFW` | `SAFETY_TRIP_TEST = 4` (or the Q1 outcome). `link_frame_decide_test_trip()`. link_task dispatch, reply burst and dedup. `s_test_trip_queue`, input field, guard evaluation, `guard_condition_still_immediate` case. F6: DIAG V3 emit, the CLEAR_TRIP boot_id decision, the unknown-peer refusal and fallback. Host tests and negtests. GUARD_TEST_MATRIX, ARCHITECTURE sec 9, SAFETY_MODEL. Stack check if link_task/safety_core grow (stack bumps are allowed). | WP1 |
| **WP3** KilnFW link, HTTP, MCP | `firmware/KilnFW`, `tools/PcTools` | `safety_link_send_test_trip()`. DIAG V3 cache with the boot_id reset on a Pico boot_id change. V3 clear build. Word tables (3 copies) and the sync check. `POST /api/safety/test_trip` and the route-tier row. Web banner badge and `trip_is_test`. LCD words. Executor parity test. `safety_test_trip` MCP tool and tests. CLAUDE.md and MCP_SERVERS.md counts. | WP1 |
| **WP4** Bench cases | `tools/PcTools/.../bench_test` | AX-T02 conversion; SP-12, SP-13, SP-14, HP-09 with judges; registry and ratchet; fake-board tests; BENCH_TEST_SYSTEM_PLAN rows. | WP3 (tool name and route contract) |
| **WP5** Coordinated reflash and bench run | bench | The sec 7 procedure, the smoke test, then `bench_test_run` for suites aux and safety plus HP-09. Record in `docs/BENCH_TEST_LOG.md`, mark F6 fixed in the audits, update ROADMAP. | WP2, WP3, WP4 on dev |

## 12. Open questions (owner)

- **Q1: the reason value.** Recommend **4** (the S4 WARN-only gap, mask
  0x0008). The alternatives are 11 (the S10 gap, 0x0400), or 17 with the mask
  widened to u32 on DIAG and CLEAR_TRIP (a breaking change on all three
  trees; not recommended).
- **Q2: F6's unknown-peer fallback.** Recommend refusing unbound clears while
  the peer version is unknown, opening the legacy path after 30 s with no
  ANNOUNCE. The alternative is a strict refusal with no fallback, which can
  lock out clears from a pre-17 ESP whose single ANNOUNCE was lost, until one
  side reboots.
- **Q3: the minimum hold before a test trip clears.** Recommend
  `trip_verify_s`, so S9's K4-opened check always completes. The alternative
  is no hold, which clears as soon as the clear arrives.
- **Q4: the rate limit.** Recommend 10 s between accepted test trips on the
  Pico. A tighter limit adds no safety, because the command only trips.
