# Pico ceiling mirror, config divergence, and S8 rate guard, 2026-09-14

Two owner decisions this session, both on the safety-ceiling mirror between
the ESP32-S3 (KilnFW) and the RP2040 safety processor (SaftyFW), plus a
standing S8 rate-guard correction.

## Decision 1 -- S8 rate guard: 20 -> 33.3 C/min

The doc/compiled default (`CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN`,
`firmware/SaftyFW/src/config_store.h:205`) was already `33.3f` -- an earlier
session's retune (`docs/audits/s8_rate_guard_retune_2026-09-09.md`) had
already corrected the FIRMWARE default. What was stale was the LIVE BOARD's
already-*commissioned* value: `max_rate_c_per_min` is one of config_store's
"no safe default" fields (`CONFIG_STORE_SET_MAX_RATE_C_PER_MIN`-gated), so a
compiled-default change never retroactively updates a board that was
commissioned before the change shipped. `safety_get_commissioning()` read
back `S8 max_rate_c_per_min=20C/min ARMED` at the start of this session.

Fix: no code change needed. Re-commissioned the live board via
`safety_set_rate_guard(max_rate_c_per_min=33.3, rate_window_s=60,
confirm=true)` and verified with `safety_get_rate_guard()`. See "Board
verification" below for the actual readback.

## Decision 2 -- the Pico ceiling must ALWAYS equal the ESP's, and 0 must
## never be a resting state that allows heat

### What already existed (2026-09-10 session)

`safety_ceiling_policy.{h,c}` / `safety_ceiling_sync.{h,c}`
(`firmware/KilnFW/App/drivers/safety/`) already computed the Pico's target
ceiling as the ESP's own configured maximum EXACTLY (no headroom -- a
2026-09-10 owner correction already removed a +5 C headroom this file used
to add), wrote it via `safety_cfg_http_set_and_confirm_f32()` (stage +
commit + confirm-by-readback, never a bare ACK), and reconciled on every
`safety_poll_task` tick while the link is up. Separately,
`commissioning_gate.h` (SaftyFW) already refused to grant heat (the ON
direction of `safety_core_request_enable()`) on any board where
`abs_max_temp_c` (and the other no-safe-default fields) had never been
explicitly commissioned -- so "0 as the resting state that allows heat" was
already structurally impossible on the Pico side before this session:
`abs_max_temp_c == 0` means uncommissioned, and an uncommissioned board
already refuses heating enable (ROADMAP.md M12, 2026-09-09). Live board
state confirmed this: `abs_max_temp_c=80C ARMED`, matching the ESP's zone
ceiling exactly, at the start of this session -- not the "0, never trips"
state the initial brief assumed.

### What the 2026-09-10 design did NOT cover, and what changed

The standing invariant as written was "never TIGHTER than the ESP", which
treated a Pico left WIDER than the ESP's target (e.g. a LOWERING reconcile
that could not confirm -- `safety_ceiling_policy_apply_lower()` is
deliberately best-effort/non-blocking) as a fully compliant resting state.
The owner's 2026-09-14 decision is strictly narrower: **"They should always
be the same, there should never be a way that the pico is not armed."**
Wider-than-necessary is no longer compliant. A second, stronger statement
followed: **"if a config doesn't land and match on both sides then alarm
and dissable heaters"** -- divergence must be checked continuously (not
just gate a future firing start) and must ACTIVELY disable heat, not merely
log or block.

**Supersession recorded here, per the owner's own instruction, since this
is where the old rule is written down**: `safety_ceiling_policy.h`'s own
top comment now carries a SUPERSEDED notice pointing at this document
before its original "never TIGHTER" text (kept verbatim, not deleted, since
the write-ORDERING logic it describes is still correct and unchanged -- only
the definition of "compliant rest state" changed).

### Design: format-version + hash identity, not a field-by-field comparator

Owner's own definition, verbatim: **"Matching config meens format version
number and a hash that identifys the set."**

`firmware/KilnFW/App/drivers/safety/config_divergence.{h,c}` (new, pure,
host-tested `test_config_divergence.c`, 20/20 checks):

- `CONFIG_IDENTITY_FORMAT_VERSION` (currently `1`) is this mechanism's OWN
  version tag for "which fields, in which order, are in the set" -- NOT
  `ZONES_CFG_VERSION` (unchanged, still 26) and NOT `KILNLINK_PROTOCOL_VERSION`
  (unchanged, still 14). Bumping it changes nothing on flash or on the wire,
  only what this in-memory comparator considers comparable. **Two identities
  computed under different format_version numbers never match, regardless of
  hash** -- comparing hashes across differing field sets/normalisation would
  be meaningless, so the version is checked first and gates the whole
  comparison.
- `config_identity_compute(format_version, fields[], n)` folds each field's
  NORMALISED value (see below), its name, and the format version into one
  FNV-1a 32-bit hash, in a fixed, documented field order. An `.known` flag
  travels with the result: an identity with any unknown field never reports
  as matching another identity, even one with an identical (sentinel) hash --
  this is the literal implementation of **"an unarmed Pico is a divergence
  by definition"**: a field the ESP has an opinion on that the Pico has never
  confirmed (boot before push, or any field gated the same way
  `abs_max_temp_c` is by `commissioning_gate.h`) fails the identity check the
  same way a numeric mismatch does.
- **Float normalisation (the owner's own named trap)**: every float is put
  through the SAME `%.9g` stringify -> `strtof()` reparse round trip
  `safety_cfg_http.c` already performs on every wire write, before hashing.
  `test_config_divergence.c`'s `test_wire_round_trip_normalises_identically()`
  proves a value that actually made that trip (via `snprintf`/`strtof`, not
  an idealised assumption) hashes IDENTICALLY to the value before the trip,
  and `test_normalize_collapses_different_bit_patterns()` proves two
  different bit patterns for the same decimal quantity collapse together.
  Without this, a value that merely travelled the wire and back could
  spuriously "diverge" and trip the alarm -- exactly the nuisance-check
  hazard the owner warned would get the check switched off.
- **Who computes what, independently** (the owner's stated hazard: an ESP
  that computes a hash and pushes it for the Pico to echo back proves
  nothing about the Pico's actual live state). This codebase's honest
  scoping, stated plainly rather than glossed over: a literal on-Pico hash
  computation reported as a NEW wire value would be a SaftyFW protocol
  addition, which this session's brief said to avoid / stop-and-report on.
  Instead: the ESP side of the comparison is computed from the ESP's own
  live, authoritative zone config, from scratch, every check
  (`safety_ceiling_policy_target_c()`); the "Pico side" is computed from
  values FRESHLY FETCHED off the Pico's own `GET_CONFIG_PAGE` report
  (`safety_cfg_store.c`'s live cache, refreshed every `safety_poll_task`
  tick) -- never an echo of a value the ESP itself just wrote moments ago.
  This satisfies the actual safety property (a value that drifted on the
  Pico's own flash cannot silently pass, since it is re-read, not assumed)
  without a wire change. **This scoping choice is called out explicitly for
  owner review** -- a genuine on-Pico hash (e.g. reusing `config_store_
  record_crc32()`, which already exists and already runs entirely on the
  Pico over its own flash bytes, already reported wire-side as `config_crc`)
  is a natural next step if the owner wants the stronger literal guarantee,
  and was deliberately NOT done here to stay inside the "no protocol bump,
  stop and report" constraint.
- `config_divergence_check(esp_fields[], pico_fields[], n, reason_out,
  reason_cap)` is the operator-facing wrapper: computes both identities,
  and iff they do not match, WALKS the per-field arrays (never the gating
  decision, only message-building) to name the FIRST field that actually
  differs and both sides' values (or "unconfirmed"). `CONFIG_DIVERGENCE_
  REASON_MAX` (160 bytes) is sized deliberately larger than the `char[96]
  ki_refusal_reason` mistake flagged this session (a message silently
  truncated at 162 bytes); `test_reason_buffer_is_never_truncated()` proves
  the longest message shape this file produces fits without truncation.

**Reusability, for the planned multi-kiln config-swap feature** (a saved
profile packaging both processors' config, applied as one transaction --
being planned separately, told to build on this mechanism): `config_
divergence.h`'s two primitives (`config_identity_compute()`/`config_
identity_matches()`) take an arbitrary, caller-supplied field array and
format version -- nothing in the file knows what a "ceiling" or a "zone" is.
A swap feature with a larger field set bumps `CONFIG_IDENTITY_FORMAT_VERSION`
and supplies its own field array; it does not need a second comparator.

### Enforcement: alarm AND disable heaters, continuously

`safety_ceiling_sync.c`'s `enforce_ceiling_divergence()` runs on EVERY
`safety_poll_task` tick, UNCONDITIONALLY -- specifically BEFORE the
raise-attempt backoff check, not after: the backoff exists only to bound the
cost of RE-ATTEMPTING an expensive write, and must never also suppress this
cheap (cache-only) divergence check. The single most likely divergent case
(the Pico is ARMED and refuses a raise) is exactly the case the backoff
spends most of its time in.

On divergence: logs `ESP_LOGE` (rate-limited to avoid spamming, but the
enforcement itself is NOT rate-limited -- it runs every tick regardless of
log cadence) and calls two injected function pointers: `kiln_io_owner_
command_all_relays_off()` (the one sanctioned "relays off now" entry point --
never a direct relay write, per CLAUDE.md's "Bypassed owner module" note)
and `profile_executor_halt()` (so a RUNNING firing does not immediately try
to re-energize on its own next tick). Both are wired in from `main_control_
bringup.c` via `safety_ceiling_sync_set_disable_heat_hooks()` -- injected
rather than a direct `#include` of `kiln_io_owner.h`/`profile_executor.h`
inside `safety_ceiling_sync.c`, because that file is compiled into more than
one host test executable (`test_zones_http.c` and others) with DIFFERENT
fake ecosystems; NULL (every existing host test's default) is a correct,
safe no-op there.

A never-configured board (no zone has a positive `max_temp_c` yet) is
explicitly excluded from this check -- there is nothing to compare, and
alarming on every tick of a fresh, unconfigured board would be exactly the
nuisance-check hazard the owner warned against.

**Clearing the alarm**: there is no dismiss action. The check re-runs from
scratch (a live read-back of the Pico's confirmed value) every tick; it
simply stops firing the moment the two sides genuinely agree. Nothing an
operator does short of making the configs actually match can clear it.

### Display: one source of truth

`readiness_http.h`'s `readiness_ceiling_match_status()` and `readiness_gate.h`
(the firing-start interlock) both consume `safety_ceiling_sync_is_diverged()`
-- the EXACT SAME live verdict the enforcement above is already acting on,
never a second, parallel float-epsilon recomputation. This was a deliberate
correction mid-session: an earlier draft of this pass computed the display
predicate from raw target/pico floats independently of the enforcement path,
which is precisely the "reset one side of a pair" class CLAUDE.md warns
about (two implementations of "do they match" that can silently drift). The
divergence item (`READINESS_GATE_KEY_CEILING_MATCH = "safety_ceiling_match"`)
was promoted into `readiness_gate.h`'s blocking set -- the same firing
interlock `estop_verified` was promoted into on 2026-09-09 -- so a new
firing cannot start while diverged, on top of the continuous active
enforcement above.

## Host tests

- `test_config_divergence.c` (new, own host executable, 20/20 checks):
  identical fields match; a real value mismatch is caught and named; a
  field known on only one side diverges; `config_identity_matches()`'s
  format-version gate; an "unknown" identity never matches even against an
  identical unknown identity; the wire round-trip normalisation proof (see
  above); the reason-buffer-never-truncated proof.
- `test_readiness_gate.c`: extended with `test_ceiling_divergence_alone_
  refuses()` and the cross-product test widened to 128 combinations (added
  `ceiling_diverged` as a 7th boolean fact).
- `check_readiness_gate_display_agreement.py`: `PAIRS` extended with
  `"safety_ceiling_match": "readiness_ceiling_match_status"`.

**Negative test, `config_divergence.c`** (2026-09-14, RED confirmed,
restored by hand, build dir deleted and rebuilt): changed `config_identity_
matches()`'s `return a->hash == b->hash;` to `return true;` (i.e. the hash
gate never actually gates anything). Result: 5 FAILURE(S) in
`test_config_divergence.c` (`test_value_mismatch_is_caught` and `test_
reason_buffer_is_never_truncated`'s assertions), `RUN FAILURES (1):
config_divergence` in the full suite. Restored by reversing the edit
textually (confirmed via `git diff` / re-grep of the restored line, since no
commit had landed yet to diff against); `App/test/build` deleted and the
full 42-executable suite rebuilt from scratch, all green.

## Verification performed this session

- `firmware/KilnFW/App/test/build_host_tests.ps1` (PowerShell tool): 42/42
  executables built (up from 41 -- `test_config_divergence.c` added), all
  pass, no RUN FAILURES.
- `firmware/SaftyFW/test/build_host_tests.ps1` (Bash tool, clean detached
  worktree at HEAD `71252b49`, short path `C:\wt\sfw14`): 220/220 checks
  pass across all SaftyFW host-test executables. SaftyFW source is
  UNTOUCHED by this session (every change is KilnFW-side, ESP-only) -- run
  purely to confirm no cross-contamination.
- `firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1` (PowerShell
  tool): clean ESP-IDF target build from a mirrored worktree
  (`C:\wt\checkbuild`), `KilnCtrl.bin`/`.elf` produced and fresh. Caught and
  fixed one real `-Werror=format-truncation` in `readiness_http.c`'s new
  divergence-detail message (the reason string plus a fixed suffix could
  exceed `READINESS_DETAIL_MAX`; fixed with an explicit `%.130s` width
  bound) and one real link failure (`config_divergence.c`/`readiness_gate.c`
  were not yet in `App/drivers/CMakeLists.txt`'s source list; added --
  `readiness_gate.c` turned out to already be present, so only `config_
  divergence.c` was actually added).
- `tools/run_all_checks.ps1`: see the final report for the tally; this
  session's own new checks are the ones listed above.

## Board verification (SaftyFW/KilnFW state)

See the final session report for the live-board readback of
`safety_get_rate_guard()` (expect `33.3 C/min`, 60 s window, ARMED) and
`safety_get_commissioning()` (expect `abs_max_temp_c` matching the ESP's
zone ceiling exactly, ARMED), plus link/trip/heap state after the ESP was
brought current and both processors were confirmed to come back up cleanly.

## No `ZONES_CFG_VERSION` bump

Confirmed still `26` (`firmware/KilnFW/App/drivers/persist/zones_config_json.h`).
Nothing in this pass touches zone persistence schema, `target_c`/
`ramp_c_per_hr`/`dwell_min`/`segment_count`, or the wire protocol
(`KILNLINK_PROTOCOL_VERSION`, still 14). `CONFIG_IDENTITY_FORMAT_VERSION` is
a new, independent, in-memory-only version number belonging solely to
`config_divergence.h`'s own comparator, not persisted anywhere and not on
the wire.
