# Explaining the unexplained safety-processor config CRC change (2026-09-10)

Follow-up to `docs/audits/esp_head_flash_2026-09-09.md`'s "Pico commissioning /
config CRC — flagged for follow-up, not resolved" section (commit `7d4c3c89`),
which observed `config_crc` move `42374 -> 63771` on an ESP reflash to HEAD
`0dddd435`, with `config_version` moving `134 -> 135` (expected) but every
inspected value unchanged, and guessed the cause was commit `2900db99`'s
`ct_channel_map`/`ct_topology` gating fix changing how the config page is
*encoded*. That guess is wrong, and the real mechanism is simpler and, for
this codebase's design, not actually a defect.

## 1. What `config_crc` is computed over, on both sides

The Pico is the only side that computes this CRC. `config_store_pack()`
(`firmware/SaftyFW/src/config_store.c:359`) serializes the whole 512-byte
record (magic, format_version, **seq** at offset 8, fields_set, every guard
field, 269 bytes of `0xFF` reserved padding), and `config_store_record_crc()`
(`config_store.c:1126-1129`) runs `bootloader_crc32()` over bytes `[0, 504)`
(`REC_OFF_CRC`, see the byte-layout comment at `config_store.c:14-121` and the
CRC write at `config_store.c:452`). `config_store_get_config_crc()`
(declared `config_store.h:1211`) exposes "the low 16 bits of
`config_store_record_crc()` applied to the cached record" (comment at
`safety_cfg_store.c:1186-1187`) — that 16-bit value is what
`SAFETY_CMD_FW_VERSION` (Frame C, `kilnlink_fw_version.h:11-31`) carries as
`config_crc` to the ESP.

The ESP does **not** independently compute a CRC over its own copy of the
config values. `safety_cfg_store.c`'s `config_crc` field (struct field at
`safety_cfg_store.c:270`) is just a cached copy of the last CRC the Pico
reported; `safety_cfg_store_maybe_refetch()` (`safety_cfg_store.c:1281-1301`)
compares that cache against each new FW_VERSION frame's `live_config_crc` and
triggers a full config-page refetch only when they differ (this is a
**change-detection latch**, not a bilateral integrity check — the ESP never
independently re-derives the checksum to confirm the Pico computed it
correctly). So "both sides compute it identically" is not the right frame:
only one side computes it at all.

## 2. What actually moves the CRC without moving a value: `seq`

Offset 8 of the packed record is `seq`, a 4-byte monotonic write counter
(`config_store.c:21` layout comment, `REC_OFF_SEQ` at `config_store.c:124`,
packed at `config_store.c:366`, unpacked at `config_store.c:464`). It is
**inside** the CRC-covered `[0, REC_OFF_CRC)` span. `config_store_write()`
"always assigns `s_cached_record.seq + 1u`" (documented at
`config_store.h:1233`, restated at `config_store.h:1157-1168` for
`config_store_seq_to_version()`, which folds `seq` into the reported
`config_version` via `(seq - 1) % 255 + 1`).

This means **every** write to the Pico's config store — including a
byte-for-byte idempotent resend of the exact same guard values — advances
`seq`, which changes 4 of the 504 CRC-covered bytes, which (CRC-32's avalanche
property) changes the reported 16-bit `config_crc` by an amount with no
relationship to whether any guard value changed. A `config_version` bump
(134→135) and an accompanying `config_crc` jump are not two coincidental
symptoms of the same root cause — the CRC bump is a **direct, unavoidable
consequence** of the version bump, because `seq` is inside both.

Checked whether `2900db99` is actually responsible, per the task's
instruction not to assume it: `git log --oneline 316967b7..0dddd435 --
firmware/KilnFW/App/drivers/safety firmware/CommonFW/include/kilnlink
firmware/CommonFW/src` returns **zero commits**. No commit between the
pre-flash and post-flash builds touched the SET_CONFIG payload builder, the
kilnlink frame codecs, or any safety-link code at all. `2900db99` itself only
touched `firmware/KilnFW/App/drivers/http/readiness_http.c`/`.h` — the ESP's
*readiness-page display logic* for whether `ct_channel_map` is required —
and never touches `config_store.c`'s pack/CRC path or what bytes get sent to
the Pico. The wire-encoded SET_CONFIG payload was byte-identical before and
after this flash. The 2026-09-09 audit's guess is therefore not just
unconfirmed but actively ruled out: nothing about the encoding changed. The
sole cause is `safety_sync_tc_type()` (`firmware/KilnFW/App/drivers/safety/
safety_link_poll.c:186-199`) unconditionally resending `SET_CONFIG` on every
link down/up transition (`tc_type_last_sent` reset to the `0xFF` sentinel at
the reconnect edge, per the comment at `safety_link_poll.c:159-172`) — exactly
the "expected" resend the original audit already correctly attributed the
`config_version` bump to. It just did not realize the CRC bump was the same
event, not a second, unexplained one.

## 3. Does the CRC cover things it arguably should not? Tested both directions

**Direction A — noise: does the CRC move on pure bookkeeping with no value
change?** Yes, confirmed above: `seq` (a write counter, not a configuration
value) is CRC-covered, so a resend always moves it. This is genuine CRC
"noise" relative to its use as an at-a-glance "did anything change" signal
for a human reading `safety_get_commissioning`'s "config CRC ... matches
live, not stale" line (`tools/PcTools/src/kilnctrl/mcp_server_safety.py:551`)
— that phrase is technically accurate (it means "the ESP's cached copy is not
older than what the Pico just reported") but is easy to misread as "the
configuration is unchanged," which it does not promise and does not attempt
to promise.

**Direction B — danger: would the CRC fail to move if a real value changed?**
No evidence of this. The CRC covers the entire 504-byte record including
every guard field (`abs_max_temp_c`, `max_rate_c_per_min`, `ct_channel_map`,
`estop_active_level`, etc. — full list in the `config_store.c:14-121` layout
comment) and `fields_set` (offset 12, the commissioned/uncommitted bitmask).
Any real value change, or any change to which fields are considered "set"
(exactly the kind of change `2900db99`'s sibling fix `b5cb83a4` made to
`config_params_all_required_set()`), would move the CRC just as reliably.
Nothing found suggests a real change could hide "under" the CRC. Padding
(`0xFF`-filled, offsets 235-503 minus the fields above) is included in the
CRC span but is never written to by any commissioning path found in
`config_store.c`, so it contributes zero variance in practice, not a hiding
place.

**Conclusion:** the CRC is not dangerous (direction B), but it is noisier
than its display text suggests (direction A) — it conflates "the record's
write-history advanced" with "a guard value changed."

## 4. Live board read-back during the running capture (read-only, no changes)

Read now, ~19 hours after the audited flash, while the 3.3-hour heating
capture continues (no writes performed):

| Field | Audit (`7d4c3c89`, post-flash) | Now |
|---|---|---|
| `boot_id` | 246 | **1** |
| Pico firmware commit | (running the pre-existing build, unflashed) | **`a87672ab`**, built `2026-09-10 02:49:20Z` |
| `config_version` | 135 | 136 |
| `config_crc` | 63771 (0xF91B) | 20244 (0x4F14) |
| `abs_max_temp_c` (S1) | 80 C | 80 C — unchanged |
| `max_rate_c_per_min` (S8) | 20 C/min | 20 C/min — unchanged |
| `tc_type` | 3 | 3 — unchanged |
| `tc_placement_mode` | CHAMBER_AGREED | CHAMBER_AGREED — unchanged |
| `estop_active_level` | ACTIVE_HIGH (0) | ACTIVE_HIGH (0) — unchanged |
| `mains_voltage_v` | 240 | 240 — unchanged |
| `commissioned` | True | True — unchanged |
| `trip_mask` / `state` | 0x0000 / armed | 0x0000 / armed — unchanged |

`boot_id` changed from 246 to 1. This is **not** an anomaly or a wraparound:
`s_boot_id = (uint8_t)(time_us_64() ^ (time_us_64() >> 8))`
(`firmware/SaftyFW/src/tasks/link_task.c:2648`) is a pseudo-random value
re-derived fresh on every boot, not a monotonic counter, so any two boots'
`boot_id` values are expected to be unrelated. Combined with the changed
Pico firmware commit (the audited session never touched SaftyFW — only
KilnFW was reflashed on 2026-09-09), this shows the **Pico itself was
reflashed and rebooted** at some point between the audit and this read,
almost certainly by a separate concurrent session (this repo runs multiple
agents in parallel; see `docs/audits/` for other same-day sessions). That is
a second, independent, and entirely expected source of `config_version`/
`config_crc` movement — a real Pico boot, not a resend — and it is further
confirmation that config CRC movement alone is a poor signal of "something
changed in the guard values" as opposed to "a write or a reboot happened."

Despite the Pico reflash in between, every guard value and the commissioned
state read back identical to the audited values above — **no drift found**.
This is not a finding that requires action; it is stated for the record per
task instruction 4.

## 5. Can an operator or tool currently tell "CRC moved, nothing meaningful
changed" from "CRC moved, something changed"?

No. Today's surfaces (`safety_get_commissioning`'s "matches live, not stale"
line, `mcp_server_safety.py:545-551` and `:878-879`) report only CRC identity
or staleness, never a diff. There is no tool call in the `kiln_*` surface
that decodes two config records (or a record before/after a resend) and
reports which named fields actually differ — the operator has to fall back to
"pull `safety_get_commissioning`'s full field dump before and after, and
diff it by eye," which is exactly the arithmetic-by-hand class the
`safety_get_diag()` trip-mask decode was recently fixed to remove (per
CLAUDE.md's flash section on `get_heap_status`/`trip_mask` decoding).

**Recommended follow-up** (not implemented this pass, per the "small and
unambiguous only" instruction — this is neither): add a
`safety_get_commissioning`-adjacent tool, or extend it, to keep the
previously-fetched config page cached client-side (the PC tooling already
holds `cached_config_crc`, see `mcp_server_safety.py:546`) and, whenever the
live CRC differs from the last-seen CRC, decode and diff the two full
records field-by-field, reporting either "no field differs (write-only:
`seq`/`config_version` advanced from a resend or reboot)" or naming exactly
which fields changed. That would convert the current binary "CRC differs"
signal into the same kind of decoded, no-arithmetic-required report the
trip-mask fix already provides for `trip_mask`, and would have answered the
2026-09-09 audit's question immediately instead of leaving it open for a
day.

## Bottom line

- `config_crc` moved because `seq`, a write counter, is inside the CRC's
  input span, and a `SET_CONFIG` resend (link down/up, `safety_sync_tc_type()`
  at `safety_link_poll.c:186-199`) always advances `seq` regardless of
  whether any value changed. `2900db99` is not implicated — no code in the
  SET_CONFIG payload/encoding path changed between the two flashed builds.
- No real configuration value changed, either across the audited flash or
  across the further Pico reflash discovered during this pass (Section 4).
- The CRC's *coverage* (whole record including `seq`) is correct and not
  dangerous — no direction was found in which a real value change could move
  through it undetected. It is just a noisier signal than its display text
  implies, because it does not distinguish "value changed" from "write
  happened." A decoded-diff tool, analogous to the trip-mask decode, is the
  concrete fix for that gap; the CRC computation itself needs no change.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
