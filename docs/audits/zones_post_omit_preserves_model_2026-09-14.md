# Zones POST: omitting the plant model triple now PRESERVES it (owner directive)

2026-09-14. Owner directive following a real casualty
(`docs/audits/plant_model_loss_investigation_2026-09-14.md`, `137dea1a`): all
three bench zones' identified plant models (`model_k_dc`/`model_tau_s`/
`model_dead_time_s`) were found genuinely zeroed by a whole-object
`POST /api/zones` that omitted `z%u_k`/`z%u_tau`/`z%u_deadtime`, undetected for
three days. `docs/audits/zones_post_model_key_omission_2026-09-13.md`
(`6093b8b6`) had confirmed, the day before, that this omit-deletes behaviour
was documented and intentional. This pass changes it.

## 1. What the old comment said it was protecting, and whether that survives

`zones_http_post_parse.c`'s comment above the `z%u_k` block (pre-change) gave
one reason for delete-on-omit: **uniformity** -- "this one field group
[should not] behave unlike every other one on the page" (contrasted
explicitly, in the very next comment block, with `z%u_xzone`, which is the
one other field group that also deletes-on-omit rather than preserving). It
named exactly one condition that made this safe in practice: **both shipped
clients (`zones_page.html`'s save button, `tools/PcTools/.../
zones_http_client.py`'s whole-zone encoder) always re-serialize all three
keys from the most recently read state, so neither can produce an omitting
POST.**

The comment did **not** name a real use case that an omit-preserves change
would break -- it named a design symmetry (every optional field on this page
behaves the same way) and a safety condition (both real clients always echo
the keys back). `plant_model_loss_investigation_2026-09-14.md` proved the
safety condition false: something reached the endpoint with a whole-page-shaped
body that did not carry these three keys, and a real, measured, multi-hour
autotune result was silently erased on all three zones. Per that document's
section 7: "Whether to convert the model block to omit-preserves is an owner
call... it should now be made with a confirmed casualty on the record." The
owner has now made that call.

**Nothing the old comment protected is lost.** The uniformity argument, if
anything, now points the other way: `z%u_k`/`z%u_tau`/`z%u_deadtime` join
`z%u_fuzzy_strength`/`z%u_coupling_c%u`/`coupling_diag_k_dc`/etc as
omit-preserves, leaving `z%u_xzone` (guard 5's cross-zone threshold, an
operator-typed sanity bound rather than a measured quantity, with its own
separate reasoning) as the only remaining delete-on-omit field. The two
shipped clients that always echoed all three keys back are entirely
unaffected by this change: sending real values via those three keys still
sets the model exactly as before, whether the previous value was the same,
different, or absent.

## 2. Schema: no `ZONES_CFG_VERSION` bump

This is a POST-parser behaviour change only. `zone_cfg_t`'s on-disk shape is
unchanged, `model_k_dc`/`model_tau_s`/`model_dead_time_s` keep their existing
storage and their existing "0.0 = no model" sentinel. `ZONES_CFG_VERSION`
stays at 26.

## 3. How a model is now deliberately cleared

There is still exactly one way to erase a zone's model through this endpoint:
**post all three keys explicitly, with value 0** (`z%u_k=0&z%u_tau=0&
z%u_deadtime=0`). 0.0 remains `model_k_dc`'s own "no model" sentinel
(`zones_config_set_model()`'s convention, unchanged), so an explicit zero is
still a legal value within each field's own `[0, MAX]` range -- nothing new
had to be added to accept it.

**Why this is the right choice, and why it is hard to trigger by accident:**

- No new endpoint or sentinel value was introduced. This follows the exact
  pattern already used elsewhere in the same file for "0 is a legal,
  intentional value, distinct from omission" (`z%u_easeoffmult`,
  `z%u_approachratecap`, `z%u_errorband`, `z%u_rateband`,
  `z%u_progressband` all parse `[0, MAX]` where 0 means "reset to firmware
  default" and rely on presence, not value, to decide whether to touch the
  field at all).
- **Omission is now the safe default; an explicit send is the only clearing
  path.** A client that has never heard of `z%u_k`/`z%u_tau`/`z%u_deadtime`
  (an old test harness, a hand-typed partial POST, anything that predates or
  ignores this field group) cannot erase the model any more, because it
  never sends these keys at all -- the exact shape of the POST that caused
  the real incident.
- Triggering the clear requires a client to *know about* these three specific
  keys and *choose* to send `0` for all three. That is qualitatively
  different from the old behaviour, which erased the model on the mere
  *absence* of three keys among dozens on a whole-page form -- the failure
  mode that actually happened. It is not literally impossible to trigger by
  accident (a client could always send stale zeros), but that requires
  active, specific action rather than an omission.
- A dedicated `POST /api/zones/model/clear`-style endpoint was considered and
  rejected: it would be a second way to reach the same three fields (this
  endpoint already has exactly one writer path from HTTP), adding surface
  area and a second place to keep in sync with `zones_config_set_model()`'s
  own validation, for a benefit (marginally harder to trigger) that the
  explicit-three-keys-of-zero rule already delivers by requiring intent on
  all three fields simultaneously.

## 4. The comment update

`zones_http_post_parse.c`'s comment above the `z%u_k`/`z%u_tau`/`z%u_deadtime`
block was rewritten in place, dated 2026-09-14, to:
- state the new contract (omission preserves) up front,
- **name the old contract explicitly** ("Until this date the comment here...
  documented the OPPOSITE, deliberate 'omit deletes it' behaviour") rather
  than silently erasing the historical reasoning -- this repo has a named
  bug class for a retraction hiding a stale claim
  (`project_retraction_hid_the_stale_claim` memory / this same investigation's
  own section 2, where a hedged inference was laundered into an assertion by
  a second document), and a rewritten comment with no trace of the prior
  behaviour would repeat that shape from the other direction,
- cite both audits (`6093b8b6`, the 2026-09-13 audit that had just confirmed
  the old behaviour as intentional; `137dea1a`, the 2026-09-14 investigation
  that found the real casualty) so a future reader can reconstruct exactly
  why the decision flipped in 24 hours,
- document the new deliberate-clear mechanism and why it is safer than the
  old default.

The very next comment block (`z%u_fuzzy_strength` etc.), which used to single
out the model triple by name as "this file's OWN documented sharp edge, not a
precedent to repeat," was also updated -- it now states that the model triple
uses the same omit-preserves convention as of 2026-09-14, with a pointer to
the incident that changed it, rather than leaving a comment that pointed at a
sharp edge that no longer exists.

## 5. Tests

All new/changed tests are in `firmware/KilnFW/App/test/test_zones_http.c`,
run against the real `zones_http_parse_zone_fields()` / `zones_post_handler()`
/ `zones_get_handler()` -- no reimplementation.

- **`test_post_omitting_model_fields_preserves_them()`** (replaces the old
  `test_post_omitting_model_fields_deletes_them()`, which asserted the
  now-reversed behaviour and would fail against the new code unmodified):
  - a whole-page body omitting all three keys preserves the stored triple
    exactly (12.0/300.0/30.0 in, 12.0/300.0/30.0 out) -- **this is the
    assertion that would have caught the original defect**: had it existed
    before 2026-09-11, a save shaped like the one that actually hit the
    board would have failed this check immediately instead of going
    unnoticed for three days;
  - per-field behaviour: an explicit `z0_k` alongside omitted
    `z0_tau`/`z0_deadtime` sets the one sent and preserves the two omitted,
    proving this is a true per-field rule, not an all-or-nothing group;
  - the deliberate-clear path (`z0_k=0&z0_tau=0&z0_deadtime=0`, all three
    explicit) still zeroes the model;
  - re-posting all three keys with their prior values (the shipped page's
    actual behaviour) still round-trips unchanged.
- **`test_post_then_get_round_trips_model_across_an_omitting_save()`**
  (new, end-to-end through the real HTTP handlers, not just the per-field
  parser) -- **this is the test named as "the one that matters most" in the
  task brief, and is a second, handler-level instance of the regression
  test**: POST 1 establishes a real measured model
  (42.731/255.6/40.3, the actual 2026-09-10 bench values from the incident
  report); POST 2 is an ordinary whole-page save that only changes `pid_kp`
  and omits the three model keys entirely -- exactly the shape of POST that
  caused the live incident; a GET afterward must still report the original
  model (and the field POST 2 *did* change, `pid_kp`, must show the new
  value, proving this is a real per-field save and not a no-op); POST 3
  proves the explicit-zero clear still works end-to-end.
- Both tests are registered in `main()`.

Ran via `firmware/KilnFW/App/test/build_host_tests.ps1`:
**`kilnctl_host_tests_zones.exe`: 1894/1894 checks passed** (up from 1832 in
the 2026-09-13 audit's own run, reflecting the added/expanded checks above),
and the full host-test suite: **40/40 executables built and passed** after
the change, and again after the negative-test restore (section 6).

## 6. Negative test (break production code, confirm, restore by hand, rebuild)

Removed the `else { z->model_k_dc = current_z->model_k_dc; }` carry-through
line for `z%u_k` only (leaving the `if` branch and the other two fields'
carry-through intact), rebuilt from a deleted `build/` directory (full
rebuild, not incremental), and reran the zones host-test executable:

```
-- parse_zone_fields -- omitting z0_k/z0_tau/z0_deadtime PRESERVES the stored plant model
   (2026-09-14 contract change -- this is the regression test for the real plant-model-loss
   incident) --
  FAIL C:\...\test_zones
RUN FAILURES (1)
```

The new test failed exactly as expected, confirming it actually exercises the
carry-through and is not vacuous. Restored the removed line **by hand**
(re-typed, not via `git checkout`/`restore`), deleted
`firmware/KilnFW/App/test/build` again, ran a full rebuild, and reran the
entire host-test suite: **40/40 executables built and passed**, matching the
pre-negative-test baseline.

## 7. Sibling fields re-checked

Re-walked every field in `zone_cfg_t` touched by `zones_http_post_parse.c`
(the same table `zones_post_model_key_omission_2026-09-13.md` built),
including the three additions that document mentioned as "added later, after
this comment was written": `autotune_baseline_k_dc` (v26), `fuzzy_model_valid`,
`generation`.

| Field | POST key | Omit behaviour | Correct? |
|---|---|---|---|
| `model_k_dc` / `model_tau_s` / `model_dead_time_s` | `z%u_k`/`z%u_tau`/`z%u_deadtime` | **preserves (`current_z`), as of this pass** | Yes -- the fix |
| `coupling_diag_k_dc`, `ease_off_window_mult`, `approach_rate_cap_c_per_hr`, `error_band_c`, `rate_band_c_per_s`, `progress_band_c`, `fuzzy_strength_pct`, `coupling_coeff[]` | own keys | preserves | Yes (unchanged from 2026-09-13 audit) |
| `tuning_*` (11 fields), `model_fit_temp_c`/`model_fit_ambient_c`, `autotune_baseline_k_dc`, `adaptive_tune_enabled` | none (read-only from this endpoint) | preserves via explicit `current_z->x` lines | Yes (unchanged) |
| `coupling_tau_s[]` / `coupling_dead_time_s[]` | none | preserves via `memcpy` | Yes (unchanged) |
| `fuzzy_model_valid` | none -- this is a **derived** field, computed at GET/control-tick time from `model_k_dc`/`model_tau_s` via `pid_fuzzy_derive_bands()` (see `zones_http_get.c`'s own comment, `:370-396`), never stored as an independent value written by this parser | N/A -- not a persisted input to this parser at all | N/A, not applicable to this table |
| `generation` | none -- bumped by `zones_post_handler()` itself on every successful whole-page commit, deliberately, not carried through from `current_z` | intentionally NOT preserved (it is a change counter, not a measurement) | Yes, by design |
| `z%u_xzone` -> `cross_zone_max_delta_c` | `z%u_xzone` | **still deletes on omission** | Deliberately unchanged -- see below |

`cross_zone_max_delta_c` (guard 5's cross-zone threshold) remains
delete-on-omit and was deliberately **not** changed in this pass: it is an
operator-typed sanity threshold, not a measured quantity produced by a
multi-hour step test, and the task's directive was specifically about the
plant-model triple. Converting it too would be a second, separate design
change outside this task's scope and is left as a candidate for a future,
separately-justified pass if a similar incident is ever found against it.

**No sibling field lacks a carry-through it should have.** The model triple
was, and after this pass no longer is, the field group needing one; every
other model/tuning-adjacent field already had the correct treatment, and the
three newer additions (`autotune_baseline_k_dc`, `fuzzy_model_valid`,
`generation`) each fall cleanly into "correctly preserved", "not a stored
input to this parser", or "intentionally not preserved" respectively.

## 8. Mechanical guard: feasibility assessment (recommend against)

Considered: a check that every persisted `zone_cfg_t` field either has a
`current_z` carry-through line in `zones_http_post_parse.c` or is explicitly
listed as intentionally-cleared-on-omit.

**Recommendation: do not build this as an automated check.** Reasons:

1. **No mechanical signal distinguishes the three legitimate shapes.** A
   field can correctly be: (a) parsed with a `current_z` else-branch
   (preserve), (b) parsed with no else-branch because omission legitimately
   means a safe zero/disabled default (`relay_mask`'s siblings like
   `guard_wrong_dir_window_s`, which the file's own comments say should
   default to disabled, not preserved, when omitted), or (c) not parsed at
   all because it is derived, a change counter, or written only by a
   different subsystem entirely (`generation`, `fuzzy_model_valid`,
   `coupling_tau_s[]` via `memcpy` rather than a per-field branch). A static
   check pinned to "does an `else` exist" cannot tell (b) apart from a real
   gap without a per-field annotation that itself would need to be kept
   correct by hand -- which is the same maintenance burden the check is
   meant to remove.
2. **This exact tradeoff was already evaluated and rejected in this codebase**
   for the closely related "reset one side of a pair" bug class (see
   `CLAUDE.md`'s own writeup): four confirmed instances shared no syntactic
   shape a regex/AST rule could target without also flagging the large
   majority of ordinary, correct one-sided resets. The same argument applies
   here at the level of individual struct fields: `zone_cfg_t` mixes
   measured quantities (preserve), operator sanity bounds with a "0 = default"
   convention (preserve), guard thresholds where omission legitimately means
   "leave disabled" (do NOT preserve), and derived/administrative fields (not
   applicable) in the same struct, with no field-name or type convention
   separating them.
3. **A narrower, pinned check remains honest and IS worth doing separately**:
   `tools/PcTools/check_zones_per_zone_field_drift.ps1` already exists in
   this repo and (per this pass's `run_all_checks.ps1` run) currently passes;
   extending that check's own table -- which already enumerates per-zone
   field key <-> struct field mappings for a *different* purpose (drift
   detection) -- with an explicit preserve/clear/omit-default column per
   field would give exactly the "pin a concrete, stable pair" shape this
   repo's standing practice calls for, without inventing a second, less
   precise mechanism. That is a reasonable follow-up but is out of this
   task's declared scope (`zones_http_post_parse.c`/`test_zones_http.c` and
   this doc only) and was not built here to avoid shipping an under-designed
   check under time pressure -- consistent with this repo's own
   "negative-test every check" and "don't ship a check that cries wolf" rules.

No guard was added. This assessment itself was not negative-tested, since no
check was shipped.

## 9. Checks run

- `firmware/KilnFW/App/test/build_host_tests.ps1` (KilnFW host tests):
  **40/40 executables built and passed**, both before and after the
  negative-test cycle. Zones executable: **1894/1894 checks passed**.
- `tools/run_all_checks.ps1` (PC-side / repo-wide checks): **94 passed, 0
  skipped, 0 failed** -- matches the stated 94/94 baseline.
- KilnFW target build (`idf.py -C firmware/KilnFW build`, then a direct
  `ninja -j 24` retry in the existing `build/` dir after an environment
  mismatch on the `idf.py` route): **failed**, but the failure is in
  `firmware/KilnFW/App/drivers/safety/safety_ceiling_sync.c`
  (`kiln_io_owner_command_all_relays_off`/`profile_executor_halt` implicit
  declarations), a file this task does not own and which `git status` shows
  modified by a concurrent session, not by this pass. Both files this task
  actually touched --
  `firmware/KilnFW/App/drivers/http/zones_http_post_parse.c` and
  `firmware/KilnFW/App/drivers/http/zones_http_get.c` -- compiled cleanly
  before the unrelated failure was hit (ninja steps 45-47 in the build log).
  This failure is attributed to the other session's in-progress edit, not to
  this change; a clean target build of this change alone was not obtained
  because of that concurrent, unrelated breakage.

## Verified hashes

- `6093b8b6` -- `git cat-file -t` -> `commit` (2026-09-13 audit that had just
  confirmed the old delete-on-omit behaviour as intentional).
- `137dea1a` -- `git cat-file -t` -> `commit` (2026-09-14 investigation that
  found the real casualty and triggered this owner directive).
- `36f88d62bf1495ddaa43334898f7d944463155a6` -- `git cat-file -t` -> `commit`
  (last touch of `zones_http_post_parse.c` before this pass).

## Bottom line

- Omitting `z%u_k`/`z%u_tau`/`z%u_deadtime` from a whole-page zones POST now
  **preserves** the stored plant model instead of deleting it.
- A deliberate clear is still possible: explicit `z%u_k=0&z%u_tau=0&
  z%u_deadtime=0` (all three, present).
- No `ZONES_CFG_VERSION` bump.
- The file's comment names both the old and new contract, with dates and
  citations, rather than silently rewriting history.
- `test_post_omitting_model_fields_preserves_them()` and
  `test_post_then_get_round_trips_model_across_an_omitting_save()` are the
  regression tests for the real incident; the latter runs the actual HTTP
  handlers end-to-end with the incident's real recorded values.
- Negative-tested by removing one carry-through line: the new test failed,
  was restored by hand, and a full clean rebuild reconfirmed 40/40.
- `cross_zone_max_delta_c` (`z%u_xzone`) is the one remaining delete-on-omit
  field in this file, deliberately left unchanged (out of this task's scope).
- A mechanical "every field has a carry-through or is listed as
  intentional" guard was assessed and is **not recommended** -- no syntactic
  signal separates the three legitimate shapes a `zone_cfg_t` field can take,
  the same reasoning this repo already applied to the "reset one side of a
  pair" bug class. Extending the existing, narrower
  `check_zones_per_zone_field_drift.ps1` is a better-scoped follow-up, not
  built in this pass.
- KilnFW host tests (40/40) and `run_all_checks.ps1` (94/94) both pass. The
  ESP-IDF target build hit an unrelated, pre-existing failure in a
  concurrently-modified file outside this task's ownership
  (`safety_ceiling_sync.c`); the two files this task changed compiled
  cleanly before that unrelated failure was reached.
