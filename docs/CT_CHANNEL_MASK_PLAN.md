# Zone-to-CT channel mapping — supporting one, two, or three shared CTs

Status: opened 2026-09-18. Owner request: kilns with one OR TWO shared current
transformers covering all zones (partial population), not just the two cases
`ct_topology` can express today (three per-zone CTs, or exactly one on
channel 2). This plan replaces an earlier per-channel-installed-mask draft:
the owner specified the data model directly (see decision below), so this
document designs that model rather than the mask.

Landing alongside this plan, a separate in-flight change makes
`s_current_sensing_commissioned` (`firmware/SaftyFW/src/tasks/safety_core.c:302`)
topology-aware and introduces a helper answering "does channel `ch` have a CT
fitted." This plan's field is that helper's generalised data source — see
step 1 of the sequence below.

## The model (owner-specified)

Each **zone** selects which CT **channel** it reads: `zone_ct_channel[z]` for
`z` in 0..2, value 0..2. Any zones that select the same channel are, for that
channel, summed together. This is the semantic inverse of the existing
`ct_channel_map[ch] -> relay_id`, which is one-to-one and is exactly why two
shared CTs cannot be expressed today — a many-to-one mapping subsumes it:

| Case               | `zone_ct_channel[0..2]` |
|---------------------|-------------------------|
| Per-zone (today)     | `{0, 1, 2}`             |
| Summed (today)       | `{2, 2, 2}`             |
| Two shared CTs, e.g. z0+z1 on ch0, z2 on ch1 | `{0, 0, 1}` |
| Two shared CTs, e.g. z0 on ch0, z1+z2 on ch1 | `{0, 1, 1}` |

"Does channel `ch` have a CT fitted" becomes: `exists z: zone_ct_channel[z] ==
ch`. This is the generalisation the in-flight commissioning-helper work
should be built on — a channel is fitted iff at least one zone claims it,
full stop, regardless of topology.

### `ct_topology` — derive it, do not remove it, do not keep writing it

- **Retain the byte on the wire and in the record**, for backward
  compatibility (see next section), but stop treating it as a
  commissioning input once `zone_ct_channel` exists. New commissioning
  writes `zone_ct_channel` only.
- **Report it as a derived value** wherever it is surfaced today
  (`GET_CONFIG_PAGE`, `HARDWARE.md`/commissioning UI text, log lines): compute
  `ct_topology_derived` from `zone_ct_channel` — `PER_ZONE` iff the map is the
  identity `{0,1,2}` in some order with all three distinct, `SUMMED` iff all
  three equal channel 2, and a new third label (`SPLIT` — informational only,
  no wire encoding needed since existing consumers already treat "not
  per_zone, not summed" as an unrecognised state to warn on, see the
  ESP-side `snprintf` call at
  `firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c:411`) for
  everything else.
- **Do not remove the stored byte.** `ct_topology` at record offset 228 keeps
  its wire slot; a board running old firmware still needs it to mean what it
  always meant when a new-firmware-written record round-trips through it (see
  the fresh-write compatibility rule below). Removing it would need a
  format_version bump for no behavioural gain, since it easily coexists with
  the new field.

### Old-record → new-field, and new-record → old-firmware

- **Old record read by new firmware** (no `zone_ct_channel` present, i.e. the
  new fields_set bit below is unset): derive `zone_ct_channel` from the
  existing `ct_topology` byte at load time — `PER_ZONE` (0) becomes `{0,1,2}`,
  `SUMMED` (1) or anything else becomes `{2,2,2}`, exactly today's default.
  This is the same "unrecognised decodes to the safe/current behaviour"
  convention `ct_topology` itself already uses for its own out-of-range
  bytes.
- **New record read by old firmware**: old firmware never looks at the new
  field or its fields_set bit, and keeps reading `ct_topology` exactly as
  before. New firmware **must therefore keep `ct_topology` in sync with
  whatever `zone_ct_channel` says** every time it writes a record, using the
  same derivation as the reporting rule above, collapsing `SPLIT` states to
  `SUMMED` (the more conservative of the two existing values — see guard
  analysis below for why SUMMED, not PER_ZONE, is the safe collapse). A board
  downgraded from new to old firmware after being commissioned with a genuine
  split then behaves as summed-with-all-three-normals-pooled, not as three
  independent per-zone thresholds it can no longer represent — never as
  per-zone, which would silently divide one shared CT's reading by channel
  and arm each channel against a threshold sized for the whole CT's current.

## Encoding and record layout

- **New field**: `uint8_t zone_ct_channel[3]` — one byte per zone, value 0-2.
  Same "channel index" domain `ct_channel_map` already uses, just addressed
  by zone instead of by channel.
- **New fields_set bit**: `CONFIG_STORE_SET_CT_INSTALLED` (bit 15) was
  already noted in `firmware/SaftyFW/src/config_store.h` as "the last free
  bit" of the `uint16_t fields_set` — confirmed still true (bits 0-15 fully
  allocated). `zone_ct_channel` therefore needs the same widening that
  comment already flagged as pending: **`fields_set` grows from `uint16_t` to
  `uint32_t`**, adding `CONFIG_STORE_SET_ZONE_CT_CHANNEL` at bit 16. This is
  the one unavoidable record-shape change this plan introduces (2 bytes
  longer at `REC_OFF_FIELDS_SET` onward, shifting every fixed offset after it
  by +2) — see the schema-version question below for whether that forces a
  `format_version` bump on its own.
- **New param id**: next unallocated id is **`0x0320`** (0x031F/`ct_topology`
  is the last entry in the section-3 current-sense group in
  `firmware/SaftyFW/src/config_params.c`; `0x0401` starts the next group, so
  `0x0320` is free and keeps the new field adjacent to the rest of the
  current-sense commissioning block, matching the existing convention of
  slotting new related ids next to their group rather than at the numeric
  end of the table). Three sub-ids, one per zone (`0x0320`/`0x0321`/`0x0322`,
  `KILNLINK_PARAM_TYPE_U8`), mirroring how `ct_channel_map[0..2]` already
  gets three ids (`0x0106`-`0x0108`) rather than one packed value — consistent
  with every other fixed-length-array field in this table, and it lets a
  partial `SET_PARAM` sequence commission one zone at a time the same way the
  channel map does today.
- **Byte offset**: append after the current tail-of-live-fields region,
  reusing the 269-byte reserved block (`REC_OFF` 235-503) already carved out
  for exactly this kind of addition rather than growing the record past
  `CONFIG_STORE_RECORD_LEN` (512). Three bytes for `zone_ct_channel[3]`, plus
  the 2-byte `fields_set` widening above, is far inside that headroom.
- **Compiled default**: `zone_ct_channel = {0, 1, 2}` (identity — per-zone),
  matching `ct_topology`'s existing default of `PER_ZONE`, so a record that
  has never set the new bit behaves exactly as an unset-field record does
  today: falls through to the `ct_topology`-derived value above, which for an
  all-zeroes/never-set record is `PER_ZONE`. **This is the safety property
  the owner flagged**: an existing commissioned PER_ZONE or SUMMED board's
  guard behaviour must not change on a firmware upgrade that merely adds this
  field but is never explicitly re-commissioned with it.

### Storage decision: bump `CONFIG_STORE_FORMAT_VERSION` (owner decision, 2026-09-18)

**Decided: `CONFIG_STORE_FORMAT_VERSION` (`firmware/SaftyFW/src/config_store.h:157`,
currently 2) bumps to `CONFIG_STORE_FORMAT_VERSION + 1` (3). `fields_set`
widens to `uint32_t` in place, at its existing offset
(`REC_OFF_FIELDS_SET`, `config_store.c:126`, 12), adding
`CONFIG_STORE_SET_ZONE_CT_CHANNEL` at bit 16 — the widening the header
comment at `config_store.h:373-376` already flagged as needing exactly this.
The earlier `fields_set2`-second-bitmask idea (a same-length widening
avoiding a version bump) was shown to the owner alongside this option,
including the rollback hazard the bump carries (below), and was rejected in
favor of the bump. This is a decision, not a correction — the analysis that
produced `fields_set2` was sound; the owner weighed the accepted cost
differently. Do not implement `fields_set2`.**

This record format has no `ZONES_CFG_VERSION`-style separate blob-length
table: `CONFIG_STORE_RECORD_LEN` is a fixed 512 bytes for every format
version (`config_store.h:143`), and version discrimination is by
`format_version` field plus a per-version CRC region
(`REC_OFF_CRC`/`REC_V1_OFF_CRC`), not by exact decoded length. The 5 bytes
this change needs (2 for the `fields_set` widening, 3 for
`zone_ct_channel[3]`) come out of the 269-byte reserved block
(`REC_OFF_RESERVED`/`REC_RESERVED_LEN`, `config_store.c:222-223`), so
`CONFIG_STORE_RECORD_LEN` itself does not change.

**What the bump obliges**, verified against `config_store.c`'s one existing
migration case (format_version 1 to 2, the only precedent this store has —
there is no `zones_cfg_vN_t`-style historical struct anywhere in this file;
migration here works directly off byte-offset constants, and the same style
carries forward):

- A new pair of frozen offset constants for the layout being superseded,
  mirroring `REC_V1_OFF_*` (`config_store.c:252-259`): `REC_V2_OFF_*` for
  every field from `REC_OFF_FIELDS_SET` onward, at their **current** (pre-bump)
  offsets, plus `REC_V2_OFF_CRC` at the current `REC_OFF_CRC` (504) — the v2
  CRC region is exactly today's whole record, unchanged. The live `REC_OFF_*`
  table then moves to the new v3 offsets: unchanged through
  `REC_OFF_FIELDS_SET` (12, still the start of the field), every offset from
  `REC_OFF_TC_SOURCE` (today 14) onward shifted by +2, and `zone_ct_channel[3]`
  and `REC_OFF_CRC` still land inside the (now 264-byte) reserved block, at
  504, same as today.
- A new `#define CONFIG_STORE_FORMAT_VERSION_V2 2u`, alongside the existing
  `CONFIG_STORE_FORMAT_VERSION_V1` (`config_store.h:165`), and a new
  `if (version == CONFIG_STORE_FORMAT_VERSION_V2)` branch in
  `config_store_unpack_ex()` (`config_store.c:729` is today's `V1` branch;
  the new branch is its sibling), same shape: verify the CRC against the OLD
  (`REC_V2_OFF_CRC`) region, then read every field at its `REC_V2_OFF_*`
  offset into a scratch record seeded by `config_store_default()` (so
  `zone_ct_channel` starts at the compiled identity default, `fields_set`'s
  new bit starts clear), zero-extend the old 16-bit `fields_set` into the new
  32-bit field (bits 16-31 correctly read as unset — a v2 record never had
  them), derive `zone_ct_channel` from the migrated `ct_topology` byte using
  this plan's own old-record rule above, set `scratch.format_version =
  CONFIG_STORE_FORMAT_VERSION`, then run `config_params_validate_ranges()`
  exactly as the v1 branch does before accepting. Unlike the v1-to-v2
  migration, this one must **not** force `calibration_missing = true` — v2's
  own commissioning surface (current-sense calibration, `i_normal_a`, etc.)
  is untouched by this plan, so a migrated record is exactly as commissioned
  as it always was; only `zone_ct_channel` is new, and it has its own
  documented safe default.
- The existing `CONFIG_STORE_FORMAT_VERSION_V1` branch needs no change beyond
  what already happens automatically: it seeds from `config_store_default()`
  (already giving `zone_ct_channel` the identity default) and stamps
  `format_version = CONFIG_STORE_FORMAT_VERSION`, which becomes 3 the moment
  the constant is bumped — a v1 record migrates straight through to v3
  shape, skipping v2, exactly as today's v1 branch already skips no
  intermediate version.
- The static assertions bracketing the offset tables
  (`config_store.c:267`, checking the current CRC region fits;
  `config_store.c:277`, the equivalent for `REC_V1_*`) need a third instance
  for `REC_V2_*`, and the existing one at line 267 needs to keep checking the
  **new** (v3) `REC_OFF_RESERVED + REC_RESERVED_LEN <= REC_OFF_CRC` bound
  after the reserved length shrinks from 269 to 264.
- `config_store_pack()` and `unpack_v2_fields()` (today's names for the
  current-version pack/unpack helpers) move to the new v3 offsets as part of
  the same edit that updates the `REC_OFF_*` table — they read the table
  symbolically, not a hardcoded literal, so this is confirming they need no
  independent change, not adding one.

## S3/S4/S9/S11/S14/S15 walk

- **S3 (load stuck on), S9 (ineffective heat / escalation), S11 (frozen
  sensor)**: all three key off `current_any_present()`
  (`firmware/SaftyFW/src/snapshots.h:247`), which is presence-only, ORed
  across all three raw channels, and does not consult `ct_channel_map`,
  `ct_topology`, or zone attribution at all. **Unaffected** by this change —
  they already work correctly for any subset of populated channels, since
  presence-per-channel is independent of which zone(s) that channel is
  attributed to. The in-flight masking work (making channels with no CT
  fitted unable to contribute a false "present") is exactly the piece these
  three guards need, and `zone_ct_channel`'s "does any zone claim channel
  ch" answers that fitted-ness question directly, generalised over any
  split.
- **S4** (WARN-only overshoot-adjacent guard, not itself current-sensing per
  `ARCHITECTURE.md`'s enum comment) does not read current at all in the code
  surveyed. **Unaffected.**
- **S14 (per-channel/summed overcurrent WARN) and S15 (open-heater deficit
  WARN)** are the guards this plan actually changes, in
  `firmware/SaftyFW/src/safety_guards.c` around lines 970-1050. Today's code
  branches on `cfg->ct_topology_summed` (a bool) into two disjoint
  implementations. **This plan replaces that boolean branch with one
  implementation, generalised over channels 0-2, using `zone_ct_channel` to
  determine each channel's zone membership**:

  For each CT channel `ch` (0-2):
  1. Compute `member(ch) = { z : zone_ct_channel[z] == ch }` — the set of
     zones this channel reads (empty, one, two, or three zones).
  2. `active(ch)` iff channel `ch` has a fresh, calibrated reading
     (`amps_valid[ch]`) and at least one zone in `member(ch)` is commanded on
     right now.
  3. `expected_sum_a(ch) = sum over z in member(ch) where relay_commanded_now_for_zone[z] of i_normal_a[z]`,
     with the existing "any commanded zone lacking a measured normal makes
     the whole sum unknowable, skip entirely" rule (today's `commanded_normals_known`
     logic) applied per-channel instead of globally.
  4. S14 evaluates exactly as today's summed branch does, substituting
     `expected_sum_a(ch)` for the old whole-kiln sum, per channel instead of
     only channel 2.
  5. S15 evaluates per zone as today, but `deficit_a` becomes **per-channel**
     (`expected_sum_a(ch) - amps[ch]` for the channel that zone `z` selects,
     i.e. `zone_ct_channel[z]`) rather than one global scalar — a deficit on
     one channel must not implicate a zone that channel does not even read.

  **Collapse check (the backward-compatibility argument the owner asked to
  verify explicitly):**
  - Per-zone topology (`zone_ct_channel = {0,1,2}`): `member(ch) = {ch}` for
    each channel, one zone each. `expected_sum_a(ch)` reduces to exactly
    `i_normal_a[ch]` when that single zone is commanded (today's per-channel
    S14 formula, unchanged), or the "skip, no accumulation" path when it is
    not (unchanged). S15's per-channel deficit for zone `z` reduces to
    `i_normal_a[z] - amps[z]`... but today's PER_ZONE branch runs **no S15 at
    all** (it forces `s15_warn[z] = false` unconditionally, per the comment
    "no shared CT in this topology"). This is the one place the collapse is
    NOT automatic: a single-zone-per-channel deficit computed this way is
    mathematically S15-shaped even in per-zone topology, but S15 has never
    fired there because a dedicated per-zone open-heater deficit was never
    the same guard as the summed one (per_zone already has a stronger,
    single-zone-vs-its-own-normal signal available via S14's own
    over/under comparison — S15 exists specifically because a *shared* CT
    loses per-zone visibility). **Explicit design choice: keep S15 gated to
    `member(ch).size >= 2`** (a genuinely shared channel), so per-zone
    topology's `member(ch) = {ch}` singleton case continues to produce
    `s15_warn[z] = false` unconditionally, exactly reproducing today's
    behaviour, and S15 only activates for channels two or more zones
    actually share — which is what "S15 is the *shared-CT* open-heater
    guard" already means conceptually.
  - Summed topology (`zone_ct_channel = {2,2,2}`): `member(2) = {0,1,2}`,
    channels 0/1 have empty membership and stay inert (`active(0)` and
    `active(1)` are false since `member` is empty, matching today's
    unconditional `s14_warn[0]=false, s14_warn[1]=false`). `expected_sum_a(2)`
    is the sum over all commanded zones — identical to today's
    `expected_sum_a` computation verbatim. S15's `member(2).size == 3 >= 2`,
    so S15 activates exactly as today, with `deficit_a` for channel 2
    identical to today's single global scalar (there is only one non-empty
    channel to compute it for). **Collapse verified for both existing
    topologies.**
  - Two-CT split (new): each channel's `member` has one or two zones. A
    2-zone channel gets real S14/S15 coverage for the first time (previously
    inexpressible); a 1-zone channel on a split board behaves like per-zone
    for that channel (S15 inert on it, S14 exact-normal comparison) — which
    is correct, since a channel one zone alone occupies really is
    indistinguishable from a dedicated per-zone CT from the guards'
    perspective.

## ESP-side sweep (`zones_current_sweep_task.c` / `_engine.c`)

Today's sweep branches the same way safety_guards.c does:
`zone_sweep_plan_k_ct()` dispatches to `zone_sweep_plan_k_ct_summed()` when
`s_ct_topology_summed`, else derives `k_ct` per channel independently. Under
the zone-to-channel model, `k_ct` derivation generalises the same way S14/S15
does: **for each channel, sum the sweep's measured contributions over exactly
the zones in `member(ch)`**, instead of either "this one zone" (today's
per-channel path) or "every zone" (today's summed path hardcoded to channel
2). The sweep must also derive its own `zone_ct_channel` reads from
`safety_get_status`'s commissioned config (mirroring how it already reads
`zone_cfg_committed_ct_topology()`, `firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c:54`)
rather than re-deriving membership locally, so a change committed on the
Pico is picked up by the very next sweep the same way a topology change is
today. `zone_sweep_task_record_ct_channels()`'s per-zone "relay on, CT idle
subtraction" logic is unaffected in shape — it already isolates one zone at a
time regardless of topology (see the file's own settle/sample-window
comment); only the *attribution of the result to a channel* changes, from
"whichever channel `ct_channel_map` names" / "always channel 2" to
"`zone_ct_channel[zone]`". `summed_unmeasured_mask`'s completeness check
generalises the same way: a channel's derived `k_ct` is only committed once
every zone in that channel's `member()` has a valid measured contribution,
not just once all three are measured (today's summed check) or just the one
(today's per-channel check).

`ct_channel_map`'s existing role (deriving which relay a channel watches, for
`relay_commanded_now_for_ct` in `safety_core.c:1187-1192`) becomes **fully
subsumed** by `zone_ct_channel`: `relay_commanded_now_for_ct[ch]` is exactly
`OR over z in member(ch) of relay_commanded_now_for_zone[z]`. Recommend
`ct_channel_map` becomes read-only/derived from `zone_ct_channel` the same
way `ct_topology` does (never a second independent commissioning surface for
the same fact), for the same backward-compatibility reasons — old firmware
still reads `ct_channel_map` directly, so new firmware must keep writing a
value there consistent with `zone_ct_channel` on every commit. For a genuine
two-zone-share-one-channel record, `ct_channel_map[ch]` cannot name two relay
ids in one byte; write **the lowest zone id in `member(ch)`** (arbitrary but
deterministic) so old firmware's `relay_commanded_now_for_ct[ch]` at least
reflects one of the true member zones rather than an undefined byte — this is
already a degraded (not exact) fallback for old firmware, which is expected
and acceptable given old firmware has no way to represent the concept at all.

## Commissioning surface

`firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md` step 3 currently asks the
operator to pick one of two radio choices ("per-zone" / "summed on channel
3"). This plan changes that surface to: **for each zone, pick which physical
CT channel (1/2/3) it is wired to** — a 3-way selector per zone rather than a
single kiln-wide radio choice, on `safety_commissioning_page.html` alongside
the existing per-channel `k_ct_v_per_a`/`zero_counts` fields. The UI can still
offer the two common presets ("one CT per zone" / "one CT for the whole
kiln") as one-click shortcuts that fill in `{0,1,2}` or `{2,2,2}`
respectively, but the underlying commissioning write is always the
three-zone selection, never a topology enum. `ct_topology`'s reporting
(GET_CONFIG_PAGE, any status text) becomes the derived label described above,
so existing operator-facing text asking "what topology is this kiln" barely
changes in the two already-supported cases and gains a third, correctly
generic label for a genuine split.

**No readiness item may be marked satisfied by a hand-typed
`zone_ct_channel` value standing in for a measured `i_normal_a`** — this
field only says which channel a zone's current is attributed to; the actual
normal-current numbers each channel's S14/S15 threshold uses still come
exclusively from the zone-sweep measurement path, unchanged by this plan.
Selecting `zone_ct_channel` wrong (e.g. claiming a zone shares a channel it
does not physically share) is a genuine hardware-truth question, same
category as `ct_installed` — the operator must get it right, and no
readiness check can substitute for that.

`abs_max_temp_c` parity between ESP and Pico is unaffected by this plan
(current-sense-only change); no interaction to call out.

## Staged, independently-safe sequence

1. Land the in-flight `s_current_sensing_commissioned`/channel-fitted helper
   (already in progress elsewhere) unchanged in behaviour for PER_ZONE/SUMMED
   — no dependency on this plan yet.
2. Bump `CONFIG_STORE_FORMAT_VERSION` to 3, widen `fields_set` to `uint32_t`
   in place and add its new bit, add `zone_ct_channel[3]` storage, param ids
   `0x0320`-`0x0322`, the v2-to-v3 migration branch, and the old-record
   derivation (`ct_topology` → `zone_ct_channel`) with the compiled default.
   No guard or sweep behaviour changes yet — this step is purely additive
   storage, verifiable by round-tripping every existing config-store host
   test unmodified plus new ones for the migration and the derivation. Safe
   to land alone: nothing reads the new field yet. This is also the step
   that incurs the rollback cost recorded below — that cost lands with this
   step, not with any later one.
3. Wire the new-record → `ct_topology`/`ct_channel_map` back-fill (new
   firmware keeps old fields in sync on every write) and the collapse-check
   host tests proving PER_ZONE/SUMMED byte-for-byte unchanged. Still no guard
   behaviour change — `ct_topology_summed` is still what S14/S15 branch on.
   Safe to land alone.
4. Replace S14/S15's boolean branch with the generalised per-channel
   `member()` algorithm, gated so that for any record without the new
   `CONFIG_STORE_SET_ZONE_CT_CHANNEL` bit it is mathematically identical to
   today's two branches
   (proven by the collapse argument above, exercised as negative/positive
   host tests per `SAFETY_MODEL.md`'s conventions — this is the step where a
   forced-wrong-membership negative test matters most, per this repo's
   negative-test discipline). This is the first step with real guard-logic
   risk; land it in isolation from the sweep change below so a regression is
   attributable to one commit.
5. **Done.** The ESP-side sweep's `k_ct`/normal derivation uses `member(ch)`
   (`zone_sweep_plan_k_ct_mapped()`) instead of the two hardcoded branches,
   with collapse tests against both existing topologies in
   `firmware/KilnFW/App/test/test_zones_http.c`: identity map ≡ per_zone
   channel-for-channel and value-for-value, all-2 map ≡ summed (one channel,
   the other two left inert), a genuine `{0,0,1}` split, a forced-wrong-
   membership negative test, and a vacuity guard recording that the planner's
   completeness gate (`measured_zone_mask` vs `s_sweep.zones_total`) is
   satisfied by an empty run when `zones_total` is 0 — which is why the
   fixture sets both sides explicitly rather than reusing the bare
   clean-run fixture.
6. Update `safety_commissioning_page.html` and
   `CT_COMMISSIONING_PLAN.md` to the per-zone selector surface; this is the
   only step requiring an actual operator workflow change and should land
   last, once every guard/sweep behaviour underneath it is already correct
   and tested independently of the UI.

**Risks**: step 4 is the one place a subtle off-by-membership error could
silently disarm S14/S15 for a real split kiln (never for the two existing
topologies, per the collapse proof) — the negative test for that step should
specifically construct a two-CT-split fixture and confirm a deficit on one
channel does NOT warn a zone attributed to the other channel. Step 2 carries
the accepted rollback cost recorded below — the format_version bump was
already decided (see the storage-decision section above), so this is a
known, accepted cost of that step, not an open question.

## Accepted cost: rolling back the Pico firmware after this lands

Once a board has been commissioned with `zone_ct_channel` on `format_version`
3 firmware, flashing a pre-bump SaftyFW build (`format_version` 2 or
earlier) back onto the RP2040 makes that build's `config_store_unpack_ex()`
see a `format_version` (3) greater than its own `CONFIG_STORE_FORMAT_VERSION`
(2) and **refuse the record outright** (`config_store.c`'s final fallthrough,
"anything else ... is refused, not reinterpreted"). The caller
(`config_store_find_latest()`) then falls back to `config_store_default()`
with `calibration_missing` forced true — every current-sense guard
(S3/S9/S11/S14/S15) and every other `fields_set`-gated field on that board
runs on **compiled defaults**, not the values it was actually commissioned
with, with no separate warning beyond the pre-existing "calibration missing"
state that condition already produces. This is the same class of hazard
`CLAUDE.md` records for `ota_rollback_esp()` past a `zones_cfg` schema bump
on the ESP side, applied here to the Pico's own, separate config store — the
two are different subsystems on different processors, and this bump does not
touch the ESP's `ZONES_CFG_VERSION` or its PID gains at all.

The remedy is the same shape: after any rollback of the RP2040 firmware,
read back the board's commissioned state (`GET_CONFIG_PAGE` / the
commissioning page's own report of `calibration_missing` and per-channel
values) before relying on any guard that depends on it, and recommission if
the readback shows defaults. Flash is untouched by a rollback — reflashing
the newer SaftyFW build restores the record exactly as it was, since
`config_store_find_latest()` picks the newest valid slot and the old,
now-unreadable v3 record is still sitting in flash, unmodified, waiting for
firmware that understands it again.
