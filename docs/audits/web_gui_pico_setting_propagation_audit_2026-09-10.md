# Web-GUI -> safety-processor setting propagation audit (2026-09-10)

Owner instruction (verbatim): *"fit it to 120v setting this on the web gui
should also set it on the safty processor. all setting like this that are
configured in the web gui should propigate"*. `abs_max_temp_c` (the specific
case that prompted this) is done and verified end to end, and `c99356f8`
made the Pico's copy equal the ESP's exactly, with no headroom. This audit
generalizes: every web-GUI-configurable setting with a counterpart on the
RP2040 safety processor (SaftyFW), whether it actually propagates, and what
"propagates" has to mean given this repo's own history of vacuous writes.

## What "propagates" means here

Not "the ESP sent it." Per CLAUDE.md's standing findings, a write whose
return code is trusted without a read-back is a known recurring defect
(`boot_guard_mark_healthy()` reported `HAL_OK` while nothing was actually
cleared). The bar used throughout this audit, matching `abs_max_temp_c`'s
already-verified pattern:

1. The web GUI's write reaches an ESP handler that stages the value to the
   Pico (`SET_PARAM` over the safety link).
2. The ESP then sends `COMMIT_CONFIG` and inspects the **numeric** accept/
   reject reply (`KILNLINK_COMMIT_CONFIG_REJECT_*`), not a guessed string.
3. The ESP then does a **live read-back** of the Pico's config
   (`safety_cfg_store_refetch()`) and compares the field's *value* against
   what was submitted -- an ACK within the reply window is explicitly *not*
   treated as proof (`safety_cfg_http.c`'s `confirm_commit_landed()` docblock
   states this outright: commit accepted-in-window is not the same as
   confirmed-by-read-back).
4. Failure is surfaced to the caller/operator, classified by the real reject
   reason, not swallowed.

## The mechanism that already does this generically

`firmware/KilnFW/App/drivers/http/safety_cfg_http.c`'s `POST
/api/safety/commissioning` handler (`commissioning_post_handler` ->
`apply_pairs()` -> `confirm_commit_landed()`) is **not special-cased to
`abs_max_temp_c`**. It is generic over every id in
`firmware/KilnFW/App/drivers/safety/safety_cfg_store.c`'s
`SAFETY_CFG_PARAM_TABLE`:

- `apply_pairs()` looks the id up (`safety_cfg_store_lookup()`), stages it
  via `safety_link_send_set_param()`, sends `COMMIT_CONFIG`, and classifies
  any reject reply through `reject_reason_to_refusal_class()` --
  `safety_ceiling_refusal_class_t` (`safety_ceiling_policy.h`), the enum a
  recent rework (`1e20721b`) introduced specifically so refusal
  classification reads the Pico's actual `KILNLINK_COMMIT_CONFIG_REJECT_*`
  code instead of substring-matching an English sentence (the old
  `strstr(reason, "ARMED")` approach, which `1e20721b`'s header comment
  documents as wrong on multiple counts: not every ARMED refusal's message
  contains the word, no word boundary, and a read-back mismatch used to get
  misclassified the same way). Per-class backoffs now exist for the
  reconcile loop: ARMED 6 s + jitter, STORAGE 60 s, everything else 5 s.
- On `commit=true`, `apply_pairs()` unconditionally calls
  `confirm_commit_landed()`, which forces `safety_cfg_store_refetch()` and
  compares every submitted pair's value against what actually reads back,
  failing closed (`"...does not read back as the submitted value --
  treating the write as FAILED, not successful"`) rather than trusting the
  ACK.
- `safety_cfg_http_set_and_confirm_f32()` (added for `abs_max_temp_c`,
  used by `safety_ceiling_sync.c`) is a *thin single-field wrapper* around
  this exact same `apply_pairs()`/`confirm_commit_landed()` machinery, not a
  second implementation.

**Conclusion: any setting that is (a) present in `SAFETY_CFG_PARAM_TABLE` and
(b) actually exposed as an input on a web-GUI page that POSTs through this
endpoint already gets the full stage + numeric-reject-classification +
read-back-confirm treatment identical to `abs_max_temp_c`.** The real
propagation risk in this codebase is therefore not "the generic mechanism is
missing checks" -- it already has them, generalized, and the ceiling-specific
refusal-class enum is exactly the reusable classification `1e20721b`'s
header comment already documents as reusable. The risk is in the places
listed under "Gaps" below, where a setting bypasses this mechanism entirely,
where the Pico's table and the ESP's mirror of it can drift apart silently,
or where a directional safety invariant is not enforced the way
`abs_max_temp_c`'s is.

## Full inventory

`config_params.c`'s `CONFIG_PARAM_TABLE` (Pico, authoritative,
`COMMISSIONING.md` sec 2.1) and `safety_cfg_store.c`'s
`SAFETY_CFG_PARAM_TABLE` (ESP mirror) currently agree exactly: 68 ids, same
type, same name each (confirmed mechanically -- see "New check" below). All
68 are enumerated on the generic commissioning page/endpoint and therefore
propagate per the definition above, *provided the web GUI actually presents
an input for them*. Grouped by section, with what feeds a safety guard
called out:

| id | field | safety-relevant? | verdict |
|---|---|---|---|
| 0x0104 | `abs_max_temp_c` | **yes** -- S1 overtemperature ceiling | Propagates, confirmed. Reference implementation (also has the extra directional-invariant enforcement in `safety_ceiling_policy.*` -- see below). |
| 0x030E | `mains_voltage_v` | **yes** -- feeds `I_expected = P / V` in `zone_sweep_derive_k_ct()` (`zones_current_sweep_engine.c:238`), which rescales `k_ct_v_per_a` (0x0308-0x030A), the CT current-sense calibration S3/S4/S9/S11/S14 threshold decisions ultimately depend on | Propagates through the same generic `apply_pairs()`/`confirm_commit_landed()` path as every other id in the table (it is just another row, 0x030E, `KILNLINK_PARAM_TYPE_F32`). No ESP-local NVS shadow copy exists to drift from the Pico's -- the ESP reads the value back live off the Pico's committed config (`safety_cfg_store` cache, refetched after every commit) rather than keeping its own. **This closes the owner's 120 V/240 V concern as a mechanism question**: a value entered in the GUI is staged, committed, and read back from the Pico before the endpoint reports success; a stale 240 in the field today is a **commissioning fact** (nobody has written 120 through this endpoint yet on this bench), not evidence the pipe is broken. Recommend: confirm the bench's live `mains_voltage_v` via `GET /api/safety/commissioning` (or `kiln_call(name="...")`) and write 120 through the commissioning endpoint if it still reads 240 -- a read-only check, not a fix, and left to the owner/next session since it is a live-value question, not a code defect. |
| 0x0204 | `max_rate_c_per_min` | **yes** -- S8 rate guard | Propagates (same generic path); additionally the manual-write path also tags provenance (`safety_cfg_store_set_rate_guard_meta(..MANUAL..)`, `commissioning_post_handler` lines ~1051-1062) so the page can tell a manual entry from the S8 auto-calc's own write. |
| 0x0101, 0x0105, 0x0106-0x0109, 0x010A | `tc_source`, `tc_type`, `ct_channel_map[0..2]`, `ct_installed`, `tc_offset_c` | yes -- gate which thermocouple/CT the Pico trusts and how it corrects it | Propagates via the generic path; commissioning-gate hazard covered separately below (already fixed, `b5cb83a4`). |
| 0x031F | `ct_topology` | yes -- selects per-zone vs summed CT interpretation, read by `dashboard_status_http.c` and `readiness_http.c` | Propagates via the generic path. This is the field whose *absence from the required-set gate* (not from propagation) once made a summed-CT board permanently uncommissionable -- see "Commissioning gate" below; already fixed. |
| 0x0211, 0x0212 | `safety_tc_installed`, `estop_active_level` | yes -- S5 promotion and E-stop input polarity | Propagates via the generic path. `estop_active_level`'s commit additionally invalidates any standing E-stop bench-verification record (`apply_pairs()`, `SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL` block) -- itself gated on the clear's own read-back-confirmed result, not a bare NVS write, matching this audit's bar. |
| 0x0201-0x0203, 0x0205-0x020F, 0x0210 | firing/overshoot/blind/frozen/CJ/borrowed-TC guard thresholds | yes -- S2/S6/S10/S12/etc thresholds | Propagates via the generic path (all rows in the same table). |
| 0x0301-0x0307, 0x0308-0x030D, 0x0310-0x0319, 0x031A-0x031E | current-sense calibration, gains, CT cal, S14 over-current | yes | Propagates via the generic path; CT-cal (0x0310-0x0318) and the S14 "record normal current" button additionally have their own dedicated POST handlers (`ct_cal_post_handler`, etc.) but those are documented as thin front ends that stage through the same `apply_pairs()`/`confirm_commit_landed()` core -- not a second, unverified mechanism. |
| 0x0401-0x0405, 0x0501-0x0504 | link timing / watchdog / startup-grace / config-check-period | mostly infrastructure, not a guard threshold directly, but `link_dead_hard_s`/`mainfault_debounce_ms`/`watchdog_timeout_ms` gate how fast a real fault is detected | Propagates via the generic path. |
| relay type (`safety relay type`, ESP-local) | not a Pico param at all | n/a | Explicitly documented as ESP-only (`relay_type_post_handler`'s header comment) -- no Pico counterpart exists, so this is out of scope for "does it propagate to the Pico," by design. |

**Net verdict: every setting in the current commissioning surface that has a
Pico counterpart propagates correctly, confirmed by read-back, via one
shared, generic mechanism** -- the same one `abs_max_temp_c` uses. No second,
weaker implementation was found for any of the 68 ids.

## Directional-invariant check (the `abs_max_temp_c` pattern's other half)

`abs_max_temp_c` is not just "propagates" -- `safety_ceiling_policy.h`/
`safety_ceiling_sync.*` additionally enforce that a **failed raise** leaves
the Pico's ceiling no *tighter* than it needs to be for longer than
necessary, and, per the corrected 2026-09-10 statement, a failed raise
leaves the Pico **strictly tighter than the ESP** for as long as the
underlying cause (most often ARMED) persists -- the reconcile loop keeps
retrying with the classified backoff until it lands, rather than accepting
"tighter is always safe, stop caring."

Checked whether any *other* propagated setting has a comparable directional
safety invariant that a failed propagation could violate:

- **`max_rate_c_per_min` (S8)**: no directional invariant exists or is
  claimed -- 0 means "disabled" on both sides (`CHECK_F32_RANGE_OR_ZERO`
  lets 0 through unconditionally), and there is no "tighter is always safe"
  relationship the way a temperature ceiling has one (a *smaller* rate cap
  is not obviously the safe direction the way a *lower* temperature ceiling
  is -- an operator raising it after a legitimate re-tune could equally be
  the safety-relevant direction). No fix proposed; flagging that this field
  does not need the ceiling-style backoff/invariant machinery because it has
  no monotonic safe direction to begin with.
- **`safety_tc_installed` / `estop_active_level`**: both are enum-like
  (0/1) with a documented safe default, not a continuous threshold with a
  "tighter" direction -- the ceiling-style invariant does not apply to
  either by construction (their own code comments already say so: "0 is a
  genuinely safe compiled default").
- **`mains_voltage_v`**: no directional invariant applies -- it is an input
  to a *derivation* (`I_expected = P/V`), not a guard threshold itself, so
  there is no "safe direction" for a failed write to fall back to. The
  actual hazard here is a **stale/wrong value silently accepted as current**
  (the 240-vs-120 case), which the read-back-confirm mechanism already
  defends against for the *write* path; it does not defend against an
  operator never attempting a corrective write in the first place, which is
  a UX/procedure gap, not a code defect. No fix proposed for the same reason
  as `abs_max_temp_c`'s ceiling-refusal machinery would not add anything
  here: there is nothing to reconcile in the background because there is no
  "wrong direction is unsafe" relationship.

No new directional-invariant hazard was found among the 68 ids that isn't
already either (a) covered by `abs_max_temp_c`'s existing machinery or (b)
inapplicable by the field's own nature. If a future field is added with a
"one direction is safe, the other isn't" shape (e.g. any future absolute
ceiling), `safety_ceiling_policy.h`'s enum/backoff pattern is directly
reusable, per the owner's own note -- do not build a second one.

## Commissioning-gate hazard (checked, already fixed)

`config_params_all_required_set()` (`config_params.c:949`) is the function
whose omission of a `ct_topology` check once made a summed-CT board
permanently uncommissionable by requiring `ct_channel_map` regardless of
topology (`b5cb83a4`). Read as it stands today: the required-set gate no
longer has this shape for `ct_topology`/`ct_channel_map`, and
`estop_active_level`/`ct_topology` are both deliberately *excluded* from the
required-set gate with a documented reason (their own code comments,
`config_params.c` lines ~443-448 and ~501-505): both have a safe compiled
default, so making them *required* would make every pre-existing board
uncommissionable until an operator re-answers a question whose safe answer
is already the default. No further gate hazard of this shape was found
across the other 66 ids -- none of the remaining fields are referenced by
`config_params_all_required_set()` in a way that assumes a topology only
some boards have.

## Reset-one-side-of-a-pair check

Checked whether any propagated setting has a paired counterpart that a Pico
reboot could strand, per the four known instances of this class. The
config-store fields audited here are all persisted Pico-side NVS/flash
fields with `fields_set` bits that survive a Pico reboot by construction
(`config_store_record_t` is loaded from flash at boot, not reinitialized) --
none of the 68 ids has an ESP-side "shadow" counter or cursor that a Pico
reboot could desynchronize the way `trip_seq`/`trip_last_seq` did. The one
place a Pico reboot genuinely interacts with this surface is
`safety_cfg_store_refetch()`'s CRC-based cache invalidation (already
designed to refetch on any CRC change, including the one a reboot-triggered
reconfirm produces) -- not a new finding, and outside this audit's scope
(commit config CRC-vs-seq behavior is `docs/audits/safety_config_crc_seq_2026-09-10.md`'s
own subject).

## Gaps found

1. **No mechanical check existed for `CONFIG_PARAM_TABLE` (Pico) vs
   `SAFETY_CFG_PARAM_TABLE` (ESP) staying in sync.** This is the actual
   structural risk in an otherwise-correct generic mechanism: the two are
   maintained by hand in two different files/firmwares with no shared
   header, an id/type/name mismatch here either strands a Pico field with no
   writer (producer, no writer) or lets the ESP stage-and-commit an id the
   Pico's `config_params_set()` silently drops (write, no consumer;
   `confirm_commit_landed()` would look the bogus id up in the SAME
   ESP-side table that manufactured it and could misreport the write as
   confirmed). **Fixed** -- see below.

No other gap with a safety consequence was found: every currently-exposed
setting with a Pico counterpart already propagates through the verified,
generic, read-back-confirmed mechanism.

## Fix: mirror-drift check between the two param tables

Added, following this repo's established mirror-drift pattern
(`approach_rate_cap_mirror_drift_check.py`,
`readiness_ct_channel_map_mirror_drift_check.py`):

- `firmware/KilnFW/App/test/safety_cfg_param_table_mirror_drift_check.py` --
  parses both `CONFIG_PARAM_TABLE` (Pico, `firmware/SaftyFW/src/config_params.c`)
  and `SAFETY_CFG_PARAM_TABLE` (ESP, `firmware/KilnFW/App/drivers/safety/safety_cfg_store.c`)
  and fails, naming the offending id, on any id present on only one side or
  disagreeing in type or name.
- `firmware/KilnFW/App/test/check_safety_cfg_param_table_mirror_drift.ps1`
  -- the standard `run_all_checks.ps1`-discoverable wrapper.

**Negative-tested against the production file**, per this audit's own
standing rule (three prior checks shipped vacuous without this): temporarily
changed `firmware/SaftyFW/src/config_params.c`'s real `0x0212`
(`estop_active_level`) row from `KILNLINK_PARAM_TYPE_U8` to
`KILNLINK_PARAM_TYPE_U16`, confirmed the check failed and named exactly that
id/mismatch, then restored the line by hand (not `git checkout --`/`git
restore`/`git stash`) and confirmed `git diff` on that file is empty and the
check passes again clean:

```
$ python firmware/KilnFW/App/test/safety_cfg_param_table_mirror_drift_check.py .
SAFETY-CFG PARAM TABLE MIRROR DRIFT DETECTED
  0x0212: type mismatch -- Pico says KILNLINK_PARAM_TYPE_U16 (estop_active_level), ESP says KILNLINK_PARAM_TYPE_U8 (estop_active_level). ...
$ # restored by hand
$ git diff firmware/SaftyFW/src/config_params.c   # empty
$ python firmware/KilnFW/App/test/safety_cfg_param_table_mirror_drift_check.py .
OK: 68 param ids agree (id, type, name) between firmware/SaftyFW/src/config_params.c and firmware/KilnFW/App/drivers/safety/safety_cfg_store.c.
```

No production behavior changed by this pass -- `config_params.c` and
`safety_cfg_store.c` are unmodified from `HEAD`; only the two new test files
were added. `KILNLINK_PROTOCOL_VERSION` and `config_store_record_t`'s layout
were not touched, and did not need to be.

## What was not done

- Did not write 120 through the commissioning endpoint on this bench --
  boards/firing are off-limits during the live coupling capture, and doing
  so is a live-value commissioning action, not a code fix. Whoever next has
  board access should read `mains_voltage_v` back
  (`GET /api/safety/commissioning`) and, if it still reads a bench-inconsistent
  value (e.g. 240 on this 120 V bench), write 120 through the same endpoint
  and confirm the reply's read-back succeeded.
- Did not touch `safety_cfg_http.c` or `safety_ceiling_policy.*` beyond
  reading them, per instruction (freshly reworked by another pass,
  `1e20721b`).
