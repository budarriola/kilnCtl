# One-step-at-a-time config migration

**Owner requirement, 2026-09-16, verbatim:** "Each new fw should support
migration of the nearest configuration forward allowing a one way one step at
a time config update path".

**Policy in one sentence.** From the next schema bump onward, a firmware
release that bumps a persisted config version ships exactly one new migration
step — the one carrying N-1 to N — and a blob older than N-1 is brought current
by applying steps in sequence, never by a converter that knows every historical
shape at once.

This document is pending work only. It does not restate the existing converter
except where a decision depends on it.

## 0. Scope, and the two readings of "one step at a time"

### 0.1 Which stores this governs

Governed (both processors, every store whose on-flash layout carries a version
byte a future release can bump):

| Store | Version symbol | Today |
|---|---|---|
| ESP zones config | `ZONES_CFG_VERSION` (`firmware/KilnFW/App/drivers/persist/zones_config_json.h`) | 26; monolithic converter v1..v25 |
| ESP saved kiln-config slots | `KILN_CFG_STORE_VERSION` (`firmware/KilnFW/App/drivers/persist/kiln_cfg_store_internal.h`) | 3; **already a real step chain** — see §1.1 |
| ESP fire profiles | `PROFILE_VERSION` (`firmware/KilnFW/App/drivers/http/profiles_http.c`) | 4; monolithic per-version branches |
| RP2040 safety config | `CONFIG_STORE_FORMAT_VERSION` (`firmware/SaftyFW/src/config_store.h`) | 2; one v1->v2 branch, which IS a single step |

Not governed, and why:

- **Wi-Fi credentials, boot-guard counter, OTA record, crash report, touch
  calibration.** `docs/CONFIG_FILESYSTEM.md` lists these as permanently NVS-
  resident because they are read before any mount or on panic paths. They carry
  no user tuning a migration would preserve; a lost one is re-provisioned, not
  migrated.
- **The `cfg` LittleFS partition itself.** The dual-write bridges
  (`zones_config_cfg_fs.h`, `profiles_cfg_fs.h`) deliberately store *the same
  versioned blob* and call `zones_config_json_decode_blob()` unchanged, with no
  separate migration path of their own. The file is a second copy of the blob,
  not a second schema — so this policy lands entirely in the decode path and
  the `cfg` bridges inherit it for free. Nothing in this plan touches
  `cfg_fs*.c`.
- **`KILN_PKG_SCHEMA_VERSION`** (`kiln_package.h`) — a hash-identity schema for
  comparing an ESP/Pico package pair, not a stored layout that migrates.

### 0.2 The ambiguity, stated rather than assumed

The requirement is readable two ways:

- **(A) Whole chain retained.** Each firmware carries every step from the
  oldest supported version up to its own, and applies as many as needed. "One
  step at a time" describes how the code is *written and tested*, not how much
  history a build understands.
- **(B) Only the newest step retained.** A firmware carries only N-1 -> N;
  anything older is unreadable and falls back to firmware defaults.

**This plan assumes (A).** Two pieces of evidence, not just preference. First,
(B) makes an upgrade destructive for any board that skips a release: the blob
decodes as corrupt and the board runs on firmware-default PID gains — the exact
hazard `CLAUDE.md` records for the rollback case, converted from a rare
rollback event into a routine upgrade event. Second, the codebase already
implements (A) at `kiln_cfg_store.c`: `migrate_store_v1_to_v2()` then
`migrate_store_v2_to_v3()` are chained for a v1 blob, and the comment at the
first of them says so explicitly — "the old `migrate_store_v1_to_current()` now
that 'current' is v3, one step". Nothing in the tree implements (B).

**If the owner meant (B), the cost is:** every step's code and test can be
deleted one release after it lands (a small, ongoing maintenance saving and
some flash), and in exchange any board more than one release behind loses all
zone tuning — PID gains, coupling matrix, model fits, commissioned limits — on
upgrade, silently, with only a log line. On the Pico half it would additionally
force a CT recalibration on every second bump, which §2 shows is the thing the
owner has just asked to avoid. Recorded as an open decision in §6 (D1) because
it is a policy call, but the recommendation is (A).

### 0.3 One-way

No downgrade step is written, ever. This is consistent with
`docs/OTA_SINGLE_SLOT_PLAN.md`, where the rollback target is a recovery image
that does not fire the kiln, not an older application. Today a newer-than-known
blob is already refused rather than guessed at:
`zones_config_json_decode_blob()` returns `ZONES_DECODE_NEWER`, and
`zones_config_store.c`'s loader leaves flash untouched, sets `*out_found = true`
so the legacy-partition migration cannot overwrite it, and runs that boot on
defaults. **That behaviour is correct and this plan keeps it** — but it is also
the unannounced-defaults hazard `CLAUDE.md` describes, so §1.4 adds the missing
annunciation rather than changing the refusal.

## 1. What a step is

### 1.1 Signature and location

The precedent to copy is `kiln_cfg_store.c`'s pair, whose shape is already
right: a file-local function taking a `const` pointer to the frozen source
struct and a pointer to the next version's struct, total, with no return value
because a shape-to-shape copy cannot fail once the length gate has passed.

Generalised for the zones store, each step lands in
`firmware/KilnFW/App/drivers/persist/zones_config_migrate.c` as:

```c
/* vN-1 -> vN. Frozen input type, next-version output type. */
static void zones_cfg_step_v27_to_v28(const zones_cfg_v27_t *src, zones_cfg_v28_t *dst);
```

with a table the driver walks:

```c
typedef void (*zones_cfg_step_fn)(const void *src, void *dst);
struct zones_cfg_step { uint8_t from; size_t src_size, dst_size; zones_cfg_step_fn fn; };
```

Three properties each step must hold, all of which the existing per-version
cases already demonstrate and which the step form makes enforceable:

1. **Its input type is frozen.** `zones_config_json.h` already freezes each
   historical struct with `_Static_assert` on `sizeof` and on the offset of
   every field a converter reads (`zone_cfg_v24_t` and `zone_cfg_v25_t` carry
   exactly this today). A step's input type gets the same treatment the moment
   it is created, and is never edited again.
2. **It never reads the current struct.** The bug that a step form exists to
   prevent is the one `zones_cfg_expected_len_for_version()`'s comment records:
   returning the *current* struct's size for an old version, which rejected
   every profile on the owner's board and was caught only by a hardware flash.
   A step whose output type is `zones_cfg_v28_t` rather than `zones_cfg_t`
   cannot drift when v29 lands.
3. **It does not carry the source CRC forward.** Each existing case says this
   in prose; in the step form it is structural, because the destination type's
   `crc32` is not written by the step at all.

### 1.2 Chain selection and application

`zones_config_json_decode_blob()` keeps its current front matter unchanged —
version byte read, newer-than-known refusal, `zones_cfg_expected_len_for_version()`
length gate, current-version CRC path. The `else` branch gains a driver:

1. Verify the blob's CRC **against its own claimed version's layout** (§1.3).
2. Walk the step table from the blob's version to `ZONES_CFG_VERSION`, each
   step's output buffer becoming the next's input.
3. If no contiguous run of steps reaches the current version, fall through to
   `convert_versioned_blob_to_current()` (§4) and, if that also declines,
   return `ZONES_DECODE_CORRUPT` exactly as today.
4. Stamp `out->version = ZONES_CFG_VERSION`, leave `out->crc32` zero as today,
   then run the existing post-chain fixups (`zones_config_json_apply_model_fit_defaults()`,
   `raise_heater_timing_to_floors()`) and `zones_config_json_validate()`
   unchanged. These are deliberately *not* steps: they apply on every load
   whatever version the blob claimed, which is the property
   `raise_heater_timing_to_floors()`'s comment says it needs to also catch a
   restored old backup.

**Buffer cost is the one real implementation constraint.** Two `zones_cfg_t`-
sized buffers ping-ponged is roughly 1.8 KB of stack, and
`zones_config_json_compute_crc()`'s comment records a real 2026-09-09 panic
caused by a single ~900 B copy of this struct on `profile_executor`'s 4096 B
stack. Decode runs at boot and from the httpd worker, not from
`profile_executor`, but the step driver must still use two file-static buffers
under the store's existing lock, never stack locals. This is a blocking design
constraint on the implementation, not an optimisation.

### 1.3 What "verify the CRC of an old blob" means under a chain

Today's two old-version CRC gates (v25 as N-1, v24 as N-2, added by
`49772fa5`) are hand-written copies of the same six lines, each with a
`_Static_assert` that `crc32` is the last field of that version's struct. Under
the chain they stop being special cases and become **a property of the step
table**: each entry already knows its source size, so one generic

```c
crc_ok = crc32_over(blob, src_size - 4) == read_u32(blob + src_size - 4);
```

covers every version that has a step, with the same `_Static_assert` per
frozen type. That is the honest answer to "what does verifying an old blob's
CRC mean": **the CRC is verified once, against the blob's own layout, before
the first step runs — never again between steps**, because the intermediate
buffers are in RAM and never crossed a flash boundary. Re-CRCing between steps
would check the step's own arithmetic, not storage integrity, and would need a
synthetic CRC per intermediate version that no writer ever produced.

Consequence worth naming: the chain does **not** retroactively give v1..v23
CRC coverage, and must not pretend to. v1..v6 predate the `crc32` field
entirely; v7..v23 have a real CRC that the tail converter (§4) still discards,
exactly as today. The step-table gate covers each version from the first one
that gets a step onward — which under §4's decision means v26 onward. Anything
implying broader coverage in a log line or a test name is a vacuous gate.

### 1.4 Newer-than-known: keep the refusal, add the annunciation

Unchanged: refuse, leave flash untouched, run on defaults for that boot. What
is missing is that the operator is told only by a `ESP_LOGW` nobody reads
during a rollback — `CLAUDE.md` records a firing started on firmware-default
PID gains after exactly this. Pending work, small and independent of the rest
of this plan:

- A sticky flag, set on `ZONES_DECODE_NEWER`, surfaced on `/api/zones/config`
  and in `capability_preflight`, that **refuses to start a firing** until the
  operator either flashes firmware that understands the blob or explicitly
  discards it. `kiln_cfg_store.c`'s `set_quarantine()` is the working
  precedent — same posture (flash untouched, writes refused until the operator
  acts), applied to the zones store, which currently has no equivalent.

## 2. The Pico half, and the CT normals

`docs/PICO_AUTO_UPDATE_PLAN.md` (referenced, not edited here — another session
owns it) records that the Pico's config store lives outside both application
slots, so an automatic update preserves `abs_max_temp_c`, the arming state and
the CT normals `i_normal_a` by construction, with one residual: a
`CONFIG_STORE_FORMAT_VERSION` bump, deliberately excluded from automatic
update. The just-settled decision is that such a bump must carry the CT normals
(`i_normal_a`, param ids `0x031A`/`0x031B`/`0x031C`, i.e. 794-796 decimal,
`firmware/SaftyFW/src/config_params.c`) forward automatically rather than
forcing recalibration.

**Is a step chain the mechanism that makes that implementable? Yes, and the
Pico is already 90% of the way there.** `config_store_unpack()`'s v1 branch is
already a correctly-shaped single step: it checks the v1 CRC against *v1's own
shorter range* (`REC_V1_OFF_CRC`), starts from `config_store_default()`, then
overlays exactly the fields v1 held. Formalising it as a table entry and
requiring each future bump to add one is a small change.

The thing that actually blocks carrying `i_normal_a` forward is **not** the
migration shape — it is `calibration_missing`, which the v1 step forces `true`
unconditionally with a documented rationale: a migrated record "was never
commissioned against the fields this pass added". That rationale is right for
v1, where the added fields were commissioning fields. It is wrong as a blanket
rule, and under the owner's decision it must become per-step:

- Each step declares which `fields_set` bits it carries forward and whether its
  own additions require recommissioning. A bump that adds no commissioning-
  relevant field carries `calibration_missing` through unchanged and preserves
  `i_normal_a` plus its three `CONFIG_STORE_SET_I_NORMAL_A_*` bits.
- A bump that *does* add a commissioning-relevant field sets
  `calibration_missing` — but that is then a deliberate, per-step,
  reviewable statement, not an artefact of the migration path.
- `config_params_validate_ranges()` continues to run on the migrated record, as
  both existing branches do. A carried-forward `i_normal_a` that fails
  `RANGE_F32_NONNEG` still invalidates the slot, which is the correct outcome.

That per-step `calibration_missing` policy is the whole mechanism. Without it,
carrying the normals forward is impossible however the migration is shaped;
with it, the chain form makes the declaration a required field of each new
step rather than something to remember.

## 3. Testing, with a deliberate asymmetry

**Two halves, two standards. A later reader must not mistake the first for the
second.**

**The pre-v26 tail keeps whatever coverage it has, and this work owes it
nothing more.** `test_zones_http.c` today exercises v6..v23 forward-conversion
by constructing a frozen historical struct in C, field by field, and staging it
(`stage_zones_blob()`), with each such test setting `src.crc32 = 0` and a
comment recording that the old-version path does not check it. That is real
coverage of the conversion arithmetic and it stays. It is not the standard for
new work, and no new test should be modelled on it.

**Every new step owes, from the day it lands:**

1. **A real blob at its input version, byte-exact, with a real CRC.** Not a
   C struct assembled in the test — a captured byte array. The reason is the
   `expected_len_for_version()` failure already in this tree's history: a test
   that builds its input from a struct definition shares the very assumption
   the migration can get wrong, and passes when the on-flash reality differs.
2. **Where the blobs live.** A new `firmware/KilnFW/App/test/cfg_blobs/`
   directory, one `zones_v<N>.bin` per version, each with a sidecar `.md`
   naming its provenance. Both sources are legitimate and both are needed:
   - **Captured**, preferred: dump the bench board's `kiln_nvs` blob (or the
     `cfg` partition file, which holds the same bytes) *before* flashing the
     firmware that bumps the version. This is a step in the release procedure,
     not an afterthought — once the bump is flashed the pre-bump blob is gone.
   - **Synthesized**, fallback: produced by a host program from the frozen
     struct plus a real computed CRC, for a version whose capture window was
     missed. A synthesized blob is marked as such in its sidecar and does not
     count as satisfying (1) on its own if a capture was possible.
3. **Chain-level tests, not just step-level.** At least one test that decodes
   the oldest blob with a step and asserts it reaches the current version
   through every intervening step, and one that asserts a corrupted byte
   anywhere in an old blob is rejected by §1.3's gate rather than migrated.
4. **A negative test per step**, per this repo's standing rule: break the
   step's field mapping, confirm the test fails, restore by hand, and force a
   full rebuild before re-measuring — `CLAUDE.md` records a poisoned `.exe`
   surviving an otherwise-correct revert and reaching a committed verdict.

Same standard applies to each new `CONFIG_STORE_FORMAT_VERSION` step on the
Pico, whose host tests already live in `firmware/SaftyFW/test/test_config_store.c`.

## 4. The existing monolithic converter — settled, not a recommendation

**Owner decision, 2026-09-16:** "Start that from here on out no I need to do it
historically". The policy is forward-only. `convert_versioned_blob_to_current()`
is **not** decomposed retroactively; it stays exactly as it is, as the pre-v26
tail, and the chain of single-version steps begins at the next bump above 26.
A blob older than the tail's coverage is handled by the existing converter
exactly as today.

The part that can actually go wrong is the handoff, so it is pinned here
rather than left to the implementation:

| Blob version | Owner of that blob | Notes |
|---|---|---|
| > `ZONES_CFG_VERSION` | neither — refused | `ZONES_DECODE_NEWER`, unchanged (§1.4) |
| == `ZONES_CFG_VERSION` | the current-version branch | full CRC check, unchanged |
| 26 .. `ZONES_CFG_VERSION - 1` | **the step chain** | v26 is the first version with a step, added by the bump to v27 |
| 1 .. 25 | **the tail converter**, unchanged | direct-to-current, exactly as today |

Read the middle row carefully: v26 is the chain's *input floor*, not its first
output. The step table is empty until `ZONES_CFG_VERSION` becomes 27, at which
point exactly one entry exists, `from = 26`. Every version is claimed by exactly
one owner and none by both, enforced mechanically by §5's check: the step
table's lowest `from` must equal 26 and its entries must be contiguous up to
`ZONES_CFG_VERSION - 1`, while the tail converter's `switch` must cover 1..25
and no more. A gap or an overlap fails the build rather than producing a blob
that silently falls through to `ZONES_DECODE_CORRUPT`.

One consequence of the tail staying: its known CRC coverage gap stays too.
v24/v25 keep their hand-written gates from `49772fa5`; v7..v23 remain
unchecked; v1..v6 have no CRC to check. §1.3's generic gate applies to the
chain only. Documenting that boundary honestly is part of this work; closing it
is not.

## 5. Mechanical enforcement

Yes — a config version bump should fail the build if it does not bring its step
and its test. This repo's own history is the argument: `check_no_orphaned_checks.ps1`
exists because "someone remembers" failed at least three times, and a check
that is never negative-tested ships as a vacuous pass.

Proposed `tools/check_config_migration_steps.ps1`, run by
`run_all_checks.ps1`'s existing `check_*.ps1` glob (so it needs no
registration), asserting, for each governed store in §0.1:

1. A step exists whose `from` equals `CURRENT_VERSION - 1`, unless the current
   version is at or below that store's tail boundary (26 for zones).
2. The step table is contiguous from the store's chain floor to
   `CURRENT_VERSION - 1`, with no duplicate `from` and no overlap with the tail
   converter's covered range (§4's table, enforced literally).
3. Every step's input type has its `sizeof` and `crc32`-is-last `_Static_assert`.
4. A fixture `cfg_blobs/zones_v<from>.bin` exists for every step, with a sidecar.
5. That fixture's filename appears in a test source, so a blob cannot sit in the
   directory unread — the same failure `check_no_orphaned_checks.ps1` guards.

The check must itself be negative-tested before it lands: bump the version in a
scratch tree without adding a step and confirm a red run, per this repo's rule
that eight checks shipped as vacuous passes without that step.

## 6. Owner decisions left open

**D1 — Whole chain or newest step only (§0.2).** *Recommendation: whole chain
(reading A).* It is the only reading under which a board that skips a release
keeps its tuning, and it is what `kiln_cfg_store.c` already does. Reading B
would make every skipped release silently reset zone tuning to firmware
defaults and, on the Pico, force a CT recalibration the owner has just asked to
avoid.

**D2 — How far back the chain is supported once it is long.** *Recommendation:
no expiry for now; revisit at ten steps.* Each step is small and flash is not
the constraint (`docs/OTA_SINGLE_SLOT_PLAN.md` gives the application 8 MiB).
Set a policy when there is evidence of a cost, not before.

**D3 — Does a firing-blocking quarantine on a newer-than-known blob (§1.4) go
in now or with the OTA single-slot work?** *Recommendation: now, and
independently.* It is the live hazard `CLAUDE.md` already records against the
current `ota_rollback_esp()` path, it does not depend on any step existing, and
it is a few hours' work.

**D4 — Is a pre-bump blob capture (§3.2) a mandatory release step?**
*Recommendation: yes, mandatory, as part of the bump itself.* The capture
window closes the moment the bumped firmware is flashed, and a synthesized
fallback shares the assumption the test is meant to check.

Item 4 (the tail converter) is settled, not open — see §4.
