# One-step-at-a-time config migration

**Owner requirement, 2026-09-16, verbatim:** "Each new fw should support
migration of the nearest configuration forward allowing a one way one step at
a time config update path".

**Policy in one sentence.** From the next schema bump onward, a firmware
release that bumps a persisted config version ships exactly one migration step
— the one carrying N-1 to N — and that is the *only* step it carries: a blob
older than N-1 is not readable by that firmware at all.

All four decisions this plan originally left open are settled (§6). Two were
settled against this plan's recommendation and are written here as decisions,
not re-argued.

This document is pending work only. It does not restate the existing converter
except where a decision depends on it.

## 0. Scope and policy

### 0.1 Which stores this governs

Governed (both processors, every store whose on-flash layout carries a version
byte a future release can bump):

| Store | Version symbol | Today |
|---|---|---|
| ESP zones config | `ZONES_CFG_VERSION` (`firmware/KilnFW/App/drivers/persist/zones_config_json.h`) | 26; monolithic converter v1..v25 |
| ESP saved kiln-config slots | `KILN_CFG_STORE_VERSION` (`firmware/KilnFW/App/drivers/persist/kiln_cfg_store_internal.h`) | 3; a real two-step chain — see §1.1 |
| ESP fire profiles | `PROFILE_VERSION` (`firmware/KilnFW/App/drivers/http/profiles_http.c`) | 4; monolithic per-version branches |
| RP2040 safety config | `CONFIG_STORE_FORMAT_VERSION` (`firmware/SaftyFW/src/config_store.h`) | 3 as of the CT-channel-mask pass — corrected from this table's earlier "2, one v1->v2 branch"; `config_store_unpack_ex()` now carries two inline branches (`CONFIG_STORE_FORMAT_VERSION_V1`, `_V2`), not one |

Not governed, and why:

- **Wi-Fi credentials, boot-guard counter, OTA record, crash report, touch
  calibration.** `docs/CONFIG_FILESYSTEM.md` lists these as permanently NVS-
  resident because they are read before any mount or on panic paths. They carry
  no user tuning a migration would preserve; a lost one is re-provisioned, not
  migrated.
- **The `cfg` LittleFS partition itself.** The dual-write bridges
  (`zones_config_cfg_fs.h`, `profiles_cfg_fs.h`) store *the same versioned
  blob* and call `zones_config_json_decode_blob()` unchanged, with no separate
  migration path of their own. The policy therefore lands in the decode path
  and the bridges inherit it — but see §1.5, because under D1 the *second copy*
  becomes a hazard rather than a free win.
- **`KILN_PKG_SCHEMA_VERSION`** (`kiln_package.h`) — a hash-identity schema for
  comparing an ESP/Pico package pair, not a stored layout that migrates.

### 0.2 D1, settled: a firmware carries exactly one step

**Owner decision, 2026-09-16, overriding this plan's earlier recommendation.**
A firmware carries the single step from the immediately preceding version to
its own — not the accumulated historical chain. Read with the original wording
("one way one step at a time config update path"), the intended upgrade path is
sequential, one firmware release at a time; a board that skips a release is not
expected to migrate directly.

Consequences, stated sharply because they are sharp:

- A board more than one config version behind **cannot read its own config**.
  Not "reads it degraded" — the decode fails and the board would otherwise fall
  back to firmware defaults, losing PID gains, the coupling matrix, model fits
  and commissioned limits.
- That silent-default outcome is the *same hazard class* D3 exists to catch,
  arriving from the opposite direction. §1.4 therefore extends the quarantine to
  cover it: **too old to consume is quarantined exactly like too new to
  understand.** A board must never quietly run on defaults because of a version
  gap in either direction.
- The live chain is permanently length one, so §1.2's multi-step driver
  collapses to a single conditional. The step *form* still earns its keep
  (frozen input type, its own CRC gate, its own captured-blob test); the
  table-walking machinery does not, and must not be built speculatively.

### 0.3 Upgrading a board that sat unused across several releases

This is now a documented operator procedure, not an edge case. It must appear
in the release notes for any release that bumps a config version.

**The procedure:** install each intervening release in version order, letting
the board **boot and persist** between each (§1.6 — persisting is not
automatic today, and that is a defect this plan must close before D1 is safe).
Each boot consumes exactly one step. There is no supported way to jump.

**If an intervening release is unavailable or skipped**, the config cannot be
recovered by any firmware, and the operator's recourse is a pre-upgrade backup:
`kiln_cfg_store`'s saved slots and the `/api/backup` export are the only paths
that carry settings across a gap, and a backup blob is itself versioned and
subject to the same one-step rule. **Take a backup before upgrading, and
restore it by re-entering values, not by importing an unreadably-old blob.**

**Does the single-slot OTA design actually permit stepping?** Checked against
`docs/OTA_SINGLE_SLOT_PLAN.md` and the build config rather than assumed.
**Yes, mechanically — with three frictions worth knowing, none prohibitive:**

1. **No anti-rollback bars an older or intermediate image.**
   `firmware/KilnFW/sdkconfig.defaults` sets `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
   and `CONFIG_APP_ROLLBACK_ENABLE` — the confirm-or-revert mechanism — but
   **not** `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, the monotonic secure-version
   refusal. Those are different features. Nothing in the bootloader refuses an
   image whose version is lower than the running one, so a sequence of
   intermediate images can be pushed in order. **If anti-rollback is ever
   enabled, stepping breaks and D1 becomes unimplementable as written** — that
   is a standing constraint this plan places on any future secure-boot work.
2. **A failed intermediate lands in recovery, not in the previous app.** With
   one application slot, an image that fails to confirm falls back to the
   recovery image. That is recoverable over Wi-Fi by pushing the next image, but
   it means each intermediate hop is a real (if small) risk, taken N times
   instead of once.
3. **Passing through recovery is migration-neutral.** The recovery image does
   not mount `cfg` and never reads or repairs config. It cannot consume a step
   and cannot corrupt one.

**Assessment: not a collision between the two owner decisions.** Single-slot
OTA permits the sequential path D1 requires. The genuine blocker was §1.6's
write-back defect, internal to the config store — CLOSED 2026-09-17
(`d3f74d67`/`6985c89b`; see §1.6).

### 0.4 One-way

No downgrade step is written, ever. Consistent with
`docs/OTA_SINGLE_SLOT_PLAN.md`, where the rollback target is a recovery image
that does not fire the kiln, not an older application.

## 1. What a step is

### 1.1 Signature and location

The precedent to copy is `kiln_cfg_store.c`'s pair: a file-local function
taking a `const` pointer to the frozen source struct and a pointer to the next
version's struct, with no return value, because a shape-to-shape copy cannot
fail once the length gate has passed. (That store carries two chained steps
today; under D1 it stops accumulating and keeps only its newest — see §4.)

Each step lands in
`firmware/KilnFW/App/drivers/persist/zones_config_migrate.c` as:

```c
/* vN-1 -> vN. Frozen input type, current output type. */
static void zones_cfg_step_v26_to_v27(const zones_cfg_v26_t *src, zones_cfg_t *dst);
```

Because the chain is length one, the destination *is* the current struct and no
dispatch table is needed — one `if (version == ZONES_CFG_VERSION - 1)` branch
calls it. Three properties each step must hold:

1. **Its input type is frozen.** `zones_config_json.h` already freezes each
   historical struct with `_Static_assert` on `sizeof` and on the offset of
   every field a converter reads (`zone_cfg_v24_t`, `zone_cfg_v25_t` today). A
   step's input type gets the same treatment when created, and is never edited.
2. **It never reads the current struct as its source.** The bug the step form
   prevents is the one `zones_cfg_expected_len_for_version()`'s comment records:
   returning the *current* struct's size for an old version, which rejected
   every profile on the owner's board and was caught only by a hardware flash.
3. **It does not carry the source CRC forward.** Structural, because the
   destination's `crc32` is not written by the step at all.

### 1.2 Selection and application

`zones_config_json_decode_blob()` keeps its current front matter unchanged —
version byte read, newer-than-known refusal, `zones_cfg_expected_len_for_version()`
length gate, current-version CRC path. The `else` branch becomes:

1. If `version == ZONES_CFG_VERSION - 1`: verify the blob's CRC against its own
   claimed version's layout (§1.3), run the one step, then the existing
   post-migration fixups (`zones_config_json_apply_model_fit_defaults()`,
   `raise_heater_timing_to_floors()`) and `zones_config_json_validate()`,
   unchanged. These are deliberately not steps: they apply on every load
   whatever version the blob claimed, which is the property
   `raise_heater_timing_to_floors()`'s comment says it needs.
2. Else if the version is within the pre-v26 tail's range: the existing
   `convert_versioned_blob_to_current()`, unchanged (§4).
3. Else: **too old to consume** — a distinct outcome from corruption, carrying
   its own reason string and triggering §1.4's quarantine. It must not be
   reported as `ZONES_DECODE_CORRUPT`, because the operator's remedy is
   completely different (step through releases or restore a backup, versus
   discard a damaged blob).

Since the chain is length one there is no ping-pong buffering, and the ~1.8 KB
of stack two `zones_cfg_t` buffers would have cost does not arise. The single
step still writes into a file-static destination under the store's existing
lock rather than a stack local: `zones_config_json_compute_crc()`'s comment
records a real 2026-09-09 panic from a single ~900 B copy of this struct on
`profile_executor`'s 4096 B stack.

### 1.3 What "verify the CRC of an old blob" means

Unchanged by D1 and still the rule: **the CRC is verified once, against the
blob's own layout, before the step runs — never after.** The post-step struct
is in RAM and never crossed a flash boundary, so re-CRCing would check the
step's own arithmetic rather than storage integrity, against a synthetic value
no writer ever produced.

Today's N-1 gate (v25, and v24 as N-2, added by `49772fa5`) is exactly the
right shape and becomes the permanent pattern: each new step brings its own
six-line gate plus a `_Static_assert` that `crc32` is the last field of its
input struct.

The N-2 gate is a casualty of D1 worth naming: once only N-1 is consumable, a
v(N-2) blob is refused before any CRC question arises, so that gate becomes
dead code at the next bump and should be deleted with the step that obsoletes
it, not left to rot. No retroactive coverage is created for v1..v23 (v1..v6
predate `crc32` entirely); claiming otherwise in a log line or test name would
be a vacuous gate.

### 1.4 Quarantine, in both directions

Extends D3's newer-than-known quarantine to cover D1's older-than-consumable
case. **A separate agent is implementing the quarantine mechanism; this section
is the contract it must satisfy, not a second implementation.**

| Blob state | Today | Required |
|---|---|---|
| Newer than firmware knows | refused, `ESP_LOGW`, runs on defaults | quarantine: refuse firing, state the reason |
| Older than the one step can consume | would decode-fail and fall back to defaults | **same quarantine, distinct reason string** |

Both set a sticky flag surfaced on `/api/zones/config` and honoured by
`capability_preflight`, which **refuses to start a firing** until the operator
acts. Flash is left untouched in both cases: the bytes are the operator's only
copy and a board that cannot read them must not overwrite them.
`kiln_cfg_store.c`'s `set_quarantine()` is the working precedent — same posture,
and its message names the version it found and the version it expected, which
is exactly what an operator needs to know which intermediate release to install.

The two reasons must remain distinguishable in the operator-facing text. "Too
new: install newer firmware or discard" and "too old: step through the
intervening releases, or restore a backup" are different instructions, and
collapsing them into one "config unreadable" message would destroy the only
actionable information the board has.

### 1.5 The `cfg`/NVS second copy

Under D1 the dual-write bridge needs explicit handling rather than inheriting
the policy for free. `nvs_load()` calls `zones_config_cfg_fs_resolve()`, which
picks a winner between the NVS blob and the `cfg` file by revision and may
write a resync copy to the loser. Both copies are independently versioned.

The hazard: if one copy is migrated and rewritten while the other keeps its
old-version bytes, a later release sees a blob two versions behind on whichever
side lost, and D1 makes that unreadable. **Requirement: a migration write-back
(§1.6) must update both copies, or deliberately invalidate the stale one.**
This is inert on boards with no `cfg` partition, which is all of them today,
and must be settled before `cfg` goes live rather than after. **Superseded
2026-09-21:** the bench board now has `cfg` mounted and populated (per
`GET /api/cfgfs`), so this requirement is now live there, not purely
theoretical.

### 1.6 Migration must persist — the blocking defect (CLOSED 2026-09-17)

**Found by inspection during this revision, and it blocked D1. Now fixed and
landed: `d3f74d67` persists a migrated blob immediately on the ordinary
`nvs_load()` path, read-back verified, and `6985c89b` surfaces a write-back
verify failure to the operator (`zones_cfg_migration_persist_fault_t`, wired
through `dashboard_http.c` -> `/api/status` -> the LCD trip strip) rather than
silently retrying forever. All four numbered requirements below are
implemented; kept here as the record of what was required and why.**

A migrated blob is **never written back on the ordinary load path**.
`nvs_load()` decodes into `s_zones.cfg` and returns; flash still holds the
old-version bytes. The only save-back on any migrated path is inside
`migrate_from_default_partition()`, a one-time legacy-partition move that calls
`nvs_save()` — not the general case. Under the previous whole-chain reading
this was harmless, because a later firmware could still read the old blob
whenever it eventually got rewritten.

**Under D1 it defeats the entire upgrade path.** An operator who does exactly
the right thing — install vN+1, boot it, install vN+2 — still arrives with vN
bytes on flash, because nothing during the vN+1 boot necessarily wrote the
migrated config back. vN+2 can only consume vN+1. The config is lost despite
correct operator behaviour, which is the worst possible failure shape: it
punishes compliance.

Required work, and it is a prerequisite for the first step, not a follow-up:

1. `zones_config_json_decode_blob()` reports whether it migrated (a new out-flag
   or a distinct `ZONES_DECODE_OK_MIGRATED` result). `out_valid` cannot serve —
   it is true for both current and migrated blobs.
2. `nvs_load()` persists immediately on that signal, through the existing flash
   worker, before the boot proceeds to anything that can fire.
3. The write-back is **read-back verified**. `CLAUDE.md`'s standing rule from the
   boot-guard episode applies directly: an NVS write reporting success proves
   nothing about flash. A migration whose write-back is unverified is a
   migration that may silently need doing again next boot — and under D1 the
   next boot may be the one that can no longer do it.
4. If the write-back cannot be verified, **quarantine rather than proceed**
   (§1.4). Running a firing on a config that could not be persisted forward is
   precisely the situation the operator must be told about.

## 2. The Pico half, and the CT normals

`docs/PICO_AUTO_UPDATE_PLAN.md` (referenced, not edited here — another session
owns it) records that the Pico's config store lives outside both application
slots, so an automatic update preserves `abs_max_temp_c`, the arming state and
the CT normals `i_normal_a` by construction, with one residual: a
`CONFIG_STORE_FORMAT_VERSION` bump, deliberately excluded from automatic
update. The settled decision is that such a bump must carry the CT normals
(`i_normal_a`, param ids `0x031A`/`0x031B`/`0x031C`, i.e. 794-796 decimal,
`firmware/SaftyFW/src/config_params.c`) forward automatically rather than
forcing recalibration.

**Does the step form make that implementable? Yes, and the Pico is already
most of the way there.** `config_store_unpack()`'s v1 branch is a correctly-
shaped single step: it checks the v1 CRC against *v1's own shorter range*
(`REC_V1_OFF_CRC`), starts from `config_store_default()`, then overlays exactly
the fields v1 held.

The thing that actually blocks carrying `i_normal_a` forward is **not** the
migration shape — it is `calibration_missing`, which the v1 step forces `true`
unconditionally, with a documented rationale: a migrated record "was never
commissioned against the fields this pass added". That is right for v1, whose
additions were commissioning fields. It is wrong as a blanket rule, and must
become per-step:

- Each step declares which `fields_set` bits it carries forward and whether its
  own additions require recommissioning. A bump adding no commissioning-relevant
  field carries `calibration_missing` through unchanged and preserves
  `i_normal_a` plus its three `CONFIG_STORE_SET_I_NORMAL_A_*` bits.
- A bump that does add one sets `calibration_missing` — a deliberate,
  reviewable, per-step statement rather than an artefact of the migration path.
- `config_params_validate_ranges()` continues to run on the migrated record, as
  both existing branches do. A carried-forward `i_normal_a` failing
  `RANGE_F32_NONNEG` still invalidates the slot, which is correct.

D1 applies here too, and the Pico is where it bites hardest: a safety processor
two format versions behind loses its commissioned ceiling and CT normals. The
Pico's own store already refuses an unknown version and falls back to
`config_store_default()` **with `calibration_missing` forced true**, which is
the correct posture — the guards then know they are uncommissioned rather than
trusting defaults. That is the behaviour §1.4 is asking the ESP side to match.

## 3. Testing

**Two halves, two standards. A later reader must not mistake the first for the
second.**

**The pre-v26 tail keeps whatever coverage it has, and this work owes it
nothing more.** `test_zones_http.c` exercises v6..v23 forward-conversion by
constructing a frozen historical struct in C and staging it
(`stage_zones_blob()`), each such test setting `src.crc32 = 0` with a comment
recording that the old-version path does not check it. That is real coverage of
the conversion arithmetic and it stays. It is **not** the standard for new work
and no new test should be modelled on it.

**Every new step owes, from the day it lands:**

1. **A real blob at its input version, byte-exact, with a real CRC** — a
   captured byte array, not a C struct assembled in the test. A test that builds
   its input from the struct definition shares the very assumption the migration
   can get wrong, and passes when on-flash reality differs. This is the
   `expected_len_for_version()` failure already in this tree's history.
2. **Fixtures live in `firmware/KilnFW/App/test/cfg_blobs/`**, one
   `zones_v<N>.bin` per version with a sidecar `.md` naming its provenance
   (which board, which firmware build, captured or synthesized).
3. **A round-trip test through the write-back path** (§1.6): decode the old
   blob, persist, re-read, and assert the stored version advanced. Without this
   the §1.6 defect can regress silently, and under D1 a silent regression costs
   the config.
4. **A negative test per step**: break the step's field mapping, confirm the
   test fails, restore by hand, and **force a full rebuild** before re-measuring
   — `CLAUDE.md` records a poisoned `.exe` surviving an otherwise-correct revert
   and reaching a committed verdict.

Same standard for each new `CONFIG_STORE_FORMAT_VERSION` step on the Pico,
whose host tests live in `firmware/SaftyFW/test/test_config_store.c`.

### 3.1 D4, settled: the capture is a mandatory release step

**Owner decision, as recommended.** Capturing a real config blob before each
version bump is a required part of the release procedure, not best-effort.

- **When:** before flashing the firmware that bumps the version. The window
  closes permanently at that moment — once the bumped build has run and
  persisted (§1.6), the pre-bump bytes are gone from the only board that had
  them.
- **What:** the raw `kiln_nvs` zones blob (and, once `cfg` is live, the
  corresponding file), plus the Pico's config record for a
  `CONFIG_STORE_FORMAT_VERSION` bump.
- **Where:** committed to `firmware/KilnFW/App/test/cfg_blobs/` with its
  sidecar, in the same commit as the step that consumes it.
- **How a test consumes one:** the step's test reads the `.bin` verbatim,
  hands those exact bytes to `zones_config_json_decode_blob()`, and asserts both
  the decoded field values and that the CRC gate accepted the real stored CRC.
  A test that recomputes the CRC over the fixture instead of using the stored
  one has disarmed the gate it exists to prove.
- **Synthesized fallback:** permitted only where a capture window was genuinely
  missed, marked as such in the sidecar, and never counted as satisfying (1)
  when a capture was possible. §5's check cannot tell the two apart, so this one
  rests on review.

## 4. The existing monolithic converter, and D2's expiry

### 4.1 Settled: forward-only, not retroactive

**Owner decision, 2026-09-16:** "Start that from here on out no I need to do it
historically". `convert_versioned_blob_to_current()` is **not** decomposed
retroactively; it stays as the pre-v26 tail, and single-version steps begin at
the next bump above 26.

The handoff is the part that can go wrong, so it is pinned rather than left to
implementation:

| Blob version | Owner of that blob | Notes |
|---|---|---|
| > `ZONES_CFG_VERSION` | neither — refused | quarantined, §1.4 |
| == `ZONES_CFG_VERSION` | the current-version branch | full CRC check, unchanged |
| == `ZONES_CFG_VERSION - 1` | **the one step** | its own CRC gate, §1.3 |
| 1 .. 25, while the tail survives | **the tail converter**, unchanged | direct-to-current, exactly as today |
| everything else | nobody — **too old to consume** | quarantined, §1.4, distinct reason |

At `ZONES_CFG_VERSION` 27 the step's input is v26 and the tail still covers
1..25, leaving **no gap**. At 28 the step's input is v27 and v26 belongs to
nobody — the first version to fall into "too old to consume". Every version is
claimed by exactly one owner or explicitly by none, enforced by §5's check
rather than by reading.

### 4.2 D2, settled: steps expire past a fixed age

**Owner decision, overriding this plan's earlier "no expiry" recommendation.**

Under D1 the live chain is already length one, so expiry does not shorten a
chain — **it governs the pre-v26 tail**, which is the only accumulated history
in the tree. Concretely, "drop past a fixed age" means: the tail converter's
oldest cases are deleted once they pass the floor, and the versions they covered
join "too old to consume".

Proposed concrete policy, for confirmation at implementation time:

- **Measured in config versions, not releases or dates.** Versions are what the
  code and the blobs actually carry; release count is not recorded on flash, and
  a date is not knowable from a blob. A floor expressed in versions is checkable
  by §5's check; one expressed in dates is not.
- **Floor: `ZONES_CFG_VERSION - 8`**, a named constant beside the version
  itself, with a comment stating that lowering it is a data-loss decision. Eight
  is chosen as roughly a season of this project's bump rate — long enough that
  the bench board's own blob is never orphaned between sessions, short enough
  that the tail actually shrinks. It is a starting value, not a derived one.
- **A board below the floor quarantines** (§1.4) — refuses firing, states the
  version it found, leaves flash untouched. It does **not** silently reset.
- **Deletions happen at bump time**, in the same commit as the new step, so the
  tail shrinks by exactly the versions that crossed the floor and the change is
  reviewable alongside the thing that caused it.

**Named plainly: dropping the tail is what strands an old board permanently.**
Today a v7 blob still migrates. Once the floor passes 7, that same board's
config is unreadable by every future firmware, forever, with no recovery path
but re-entering values by hand. That is the accepted cost of the decision, and
the reason §1.4's quarantine must name the version it found — for a stranded
board, that message is the only remaining evidence of what the config was.

## 5. Mechanical enforcement

A config version bump must fail the build if it does not bring its step and its
test. `check_no_orphaned_checks.ps1` exists because "someone remembers" failed
at least three times.

Landed 2026-09-17: `tools/check_config_migration_steps.ps1`, picked up
automatically by `run_all_checks.ps1`'s `check_*.ps1` glob, originally scoped
to the ESP zones config store only (the sole governed store with an empty,
D1-shaped step table at the time). **Extended 2026-09-19** to also cover the
other three governed stores — see the new subsection after the negative-test
paragraph below for what that extension actually checks, and why it is
narrower than the zones rule set rather than an identical copy of it.

The full zones rule set (unchanged by the extension):

1. A step exists whose input is `CURRENT_VERSION - 1`, unless the current
   version is still within the tail's range (26 for zones).
2. **Exactly one** step exists — under D1 a second is a defect, not a bonus.
   This is the check that keeps D1 from silently decaying back into an
   accumulated chain.
3. No gap and no overlap between the step, the tail's covered range, and the
   expiry floor (§4.1's table, enforced literally).
4. Every step's input type carries its `sizeof` and `crc32`-is-last
   `_Static_assert`.
5. A fixture `cfg_blobs/zones_v<input>.bin` plus sidecar exists for the step,
   and its filename appears in a test source — so a blob cannot sit unread, the
   failure `check_no_orphaned_checks.ps1` guards.
6. The expiry floor constant exists and the tail's oldest case matches it.

Negative-tested per that requirement:
`firmware/KilnFW/App/test/test_check_config_migration_steps.ps1` bumps the
version in synthetic scratch text (never the real tree) without adding a
step, and separately with a step missing each individual required property
(frozen-input assert, crc32-last-field, fixture presence/reference, expiry
floor), confirming a red run named to the specific broken rule each time,
plus a green run against a synthetic fully-correct step and against today's
real (pre-bump) tree. Wired into `run_all_checks.ps1` alongside the other
`test_check_*.ps1` negative tests. Eight checks in this repo previously
shipped as vacuous passes without that discipline.

### 5.1 Extension to the other three governed stores (2026-09-19)

Each of the other three stores has a genuinely different on-disk migration
shape from zones', so the extension enforces the part of D1 that generalizes
to that shape rather than forcing an identical rule set onto code it doesn't
fit:

- **ESP kiln-config slots** (`Test-KilnCfgStoreMigrationStep`): already
  carries two pre-D1 GRANDFATHERED steps (`migrate_store_v1_to_v2`,
  `migrate_store_v2_to_v3`) — exactly the case this section's earlier
  revision warned a naive "exactly one step total" rule would immediately
  flag. Enforced instead: `migrate_store_v<CURRENT-1>_to_v<CURRENT>(...)`
  must exist in `kiln_cfg_store.c` for the live `KILN_CFG_STORE_VERSION`.
  This is a real, negative-tested check (bumping the version in scratch
  text without adding the matching function fails, naming it) but it does
  not re-enforce D1's "exactly one" accumulation rule, a frozen-input
  `_Static_assert`/`crc32`-last-field convention (this store has neither —
  it uses a length-based migration detection and one aggregate BSS/heap
  budget assert covering all historical structs together, not a per-step
  one), a fixture-must-be-referenced rule, or an expiry floor. None of
  those conventions exist in this store's own design to check against
  today; inventing one here would be a check enforcing a policy this store
  never adopted, not a check catching a real regression.
- **ESP fire profiles** (`Test-ProfilesMigrationStep`): its converters
  (`convert_profile_v1/v2/v3`) each convert DIRECTLY from a historical
  version to the current in-memory `profile_t`, not `N -> N+1` — a
  monolithic-tail shape like zones' pre-v26 converter, not a chain. This
  store DOES already carry the frozen-input `_Static_assert`/`crc32`-last-
  field discipline per historical struct (`profile_persisted_v3_t`, sized
  and offset-asserted). Enforced today: `convert_profile_v<PROFILE_VERSION-1>
  (...)` must exist, **and (2026-09-19)** its frozen input type
  `profile_persisted_v<PROFILE_VERSION-1>_t` must carry a `_Static_assert`
  pinning its `sizeof` and must have `crc32` as its structurally last field
  — the same rule 3 zones already enforced, taught to this store's naming
  convention (`profile_persisted_v<N>_t`, found by name rather than via
  `expected_len_for_version()`'s `switch`, which the check does not need to
  parse to check this). Still deliberately not enforced for this store: D1's
  "exactly one" accumulation rule (this store is a monolithic tail like
  zones' pre-v26 converter, not a chain, so it does not apply the same way),
  a fixture-must-be-referenced rule (no `cfg_blobs` captured for this store
  yet), and D2's expiry floor (no tail-eviction policy of its own).
- **RP2040 safety config** (`Test-SaftyConfigStoreMigrationStep`): has no
  per-transition function at all — migration is two inline
  `if (version == CONFIG_STORE_FORMAT_VERSION_V<N>)` branches inside
  `config_store_unpack_ex()`. Enforced: a
  `CONFIG_STORE_FORMAT_VERSION_V<CURRENT-1>` macro must be defined in
  `config_store.h` AND actually referenced by a matching branch in
  `config_store.c` (an orphaned macro is caught). Depth beyond that
  (frozen-struct/fixture/D2) needs the same kind of scaffolding this store
  doesn't have yet — its migration inputs are raw byte offsets
  (`REC_V1_OFF_*`/`REC_V2_OFF_*`), not typed frozen structs — and building
  that scaffold is a real firmware change, not a check-only change, so it
  is left as follow-up rather than attempted here.

All three additions are negative-tested the same way as the original zones
rule: `test_check_config_migration_steps.ps1` assertions 10-15 bump each
store's version in synthetic scratch text with no matching step/macro (FAIL,
naming the store) and separately run the function against today's real,
already-compliant production files (PASS) — 15 assertions total, up from 9.
The fire-profiles frozen-input-struct extension above is separately
negative-tested by assertions 19-22 (missing `sizeof` assert; `crc32` not
the last field; a commented-out `sizeof` assert; a trailing comment naming
`crc32` after the true last member), 22 assertions total as of 2026-09-19.

**Still explicitly follow-up, not assumed done:** D1's "exactly one NEW step
per bump" defect-catching rule and the fixture-must-be-referenced rule, for
all three of these stores; D2's expiry floor, for all three; and the
frozen-input assert/`crc32`-last-field discipline for kiln-config slots and
RP2040 safety config specifically (fire profiles' copy of that one rule
landed 2026-09-19, above — the "check taught its existing scaffolding" case
this paragraph used to name as still open). Kiln-config slots and RP2040
safety config would need new scaffolding that does not exist in their
current design to go further; that is not silently assumed covered by this
pass.

## 6. Decisions — all settled

**D1 — one step only, not the whole chain.** Settled 2026-09-16, against this
plan's recommendation. Sequential upgrade is the intended path; a skipped
release is not expected to migrate. §0.2, §0.3, and the §1.6 write-back defect
are the consequences.

**D2 — steps expire past a fixed age.** Settled, against this plan's
recommendation. Governs the pre-v26 tail; concrete policy proposed in §4.2,
including that dropping the tail strands an old board permanently.

**D3 — quarantine firing, land now.** Settled as recommended, and extended by
§1.4 to cover the older-than-consumable direction as well as newer-than-known.
Implemented by a separate agent; §1.4 is the contract, not a second
implementation.

**D4 — pre-bump blob capture is mandatory.** Settled as recommended. §3.1.

The tail-converter question (originally item 4) was settled earlier the same
day and is recorded in §4.1.

### 6.1 Residual risks carried by these decisions

Not open questions — accepted consequences, recorded so they are not
rediscovered as surprises:

1. **§1.6 is a prerequisite, not a follow-up.** D1 is unsafe until a migrated
   config is persisted and read-back verified on the ordinary load path. The
   first step must not land before it.
2. **Enabling anti-rollback would break the upgrade path** (§0.3). Any future
   secure-boot work must account for this or D1 becomes unimplementable.
3. **Each intermediate hop carries single-slot OTA's failure mode** — a failed
   image lands in recovery, N times instead of once. Recoverable over Wi-Fi, but
   the exposure scales with how far behind the board is.
4. **`cfg`/NVS divergence** (§1.5) must be settled before the `cfg` partition
   goes live, not after.

## 7. PC-side arbitrary-jump converter (2026-09-17)

D1 keeps firmware single-step only; the owner separately asked for a PC-side
tool that jumps any version to any other, best-effort, without ever touching
a board. Landed as `tools/PcTools/src/kilnctrl/config_convert.py` (CLI:
`tools/PcTools/scripts/config_convert.py`; MCP: `convert_config`). Scope
today: the `kilnctl_backup` document (delegates to the existing
`cfg_convert.py`) and a new `kilnctl_profile_blob` wrapper for the raw
`profile_persisted_t` NVS record (v1-v4, mirroring `PROFILE_VERSION`).
Forward steps mirror firmware's migration exactly; backward steps drop what
the older layout cannot express and name every drop in a per-field report
(`report.lossy` is true only when something is actually dropped, never
merely defaulted). `kiln_cfg_store`'s package format and SaftyFW's raw
`config_store_record_t` are deliberately NOT implemented yet -- refused with
a named reason, not attempted. A regex-based mirror-drift check
(`tools/check_config_convert_mirror.py`) fails if `PROFILE_VERSION` or its
sibling constants are bumped in firmware without a matching update here;
negative-tested by bumping `PROFILE_VERSION` in a scratch copy of
`profiles_http.c` and confirming failure, then restoring byte-exact via
`git cat-file blob`. CRC32 uses Python's `zlib.crc32()` as an unverified
stand-in for `esp_crc32_le()` -- believed equivalent, not confirmed against a
real captured NVS blob.
