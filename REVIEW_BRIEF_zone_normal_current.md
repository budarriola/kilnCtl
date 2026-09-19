# Review brief: "Measure Zone Normal Current" in individual and shared CT setups

Prepared for Opus review, 2026-09-18. This session made **no functional code
changes** — it is a verification pass against an owner task that turned out to
already be fully implemented by the immediately preceding session (commit
`40e3ae14`, landed the same day, minutes before this task was assigned). This
document records what was checked, exactly, against each of the four items the
task asked for, and what was run to confirm it.

## Task as given

"Make sure the Measure Zone Normal Current works in both individual and
shared CT setups," with four specific checks: (1) correct per-zone
attribution under a shared/summed topology, (2) individual topology reads the
*mapped* channel, not index==zone, (3) refuses cleanly on unset/partial
config rather than recording a wrong baseline, (4) the web UI presents the
right thing for whichever topology is configured.

## What I found already in place

The relevant code lives mainly in two files:
- `firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c` — pure,
  host-testable decision functions (`zone_sweep_summed_normal_a()`,
  `zone_sweep_verify_ct_attribution()`, `zone_ct_verify_threshold_a()`,
  `zone_sweep_check_refusal()`, etc.)
- `firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c` — the
  stateful sweep driver that calls those pure functions
  (`zone_sweep_shared_ch_for_zone()`, `zone_sweep_task_record_normal()`,
  `zone_sweep_task_record_ct_channels()`).

Plus a persisted verdict store, `firmware/KilnFW/App/drivers/persist/
ct_verify_store.c`, and UI in `firmware/KilnFW/App/drivers/http/
zones_page.html` / `setup_wizard_page.html`.

### Item 1 — shared/summed topology attribution

**Rule implemented:** the sweep energizes exactly one zone's relay(s) at a
time with every other relay forced off (`zone_sweep_hw_energize()`'s 0xFF
mask, unchanged from the per-zone path). So even when N zones share one
physical CT channel, the delta observed on that channel during zone Z's own
energize window (`with_on - idle_a[channel]`) is genuinely Z's own
contribution, not a copy of the channel's total. `zone_sweep_shared_ch_for_
zone()` (task.c line ~178) determines, from the committed `zone_ct_channel`
map (`member(ch) > 1` for the zone's assigned channel — generalizing the
legacy binary `ct_topology` flag), whether a zone's normal must come from
this shared-channel path rather than the `sample_current()` per-zone sum.
`zone_sweep_task_record_normal()` explicitly *skips* writing `zone_normals_
set()` for a zone resolved as shared, so the two producers (shared-channel
path vs. ordinary per-zone path) can never both write a value for the same
zone — there is exactly one writer per zone, chosen structurally, not by
convention. Each shared zone therefore gets its **own, independently
measured** normal current, not a duplicate of another zone's or of the
summed total. This is explicit in code (not implicit): `zone_sweep_shared_
ch_for_zone()`'s doc comment states the equivalence, and `zone_sweep_verify_
ct_attribution()` (engine.c) separately reports `ZONE_CT_VERIFY_SHARED_
CHANNEL` (verdict INCONCLUSIVE, never PASS/FAIL) whenever a *later*
verification pass is asked to attribute a reading on a channel two zones
share — per-zone attribution is genuinely undecidable from a bare reading in
that case, and the code says so rather than guessing.

A below-idle or NaN reading on the shared channel is treated as *unmeasured*
for that zone (`summed_unmeasured_mask` bit set), never persisted as a zero —
persisting a zero would silently make the S14/S15 guard permanently inert for
that zone.

### Item 2 — individual topology reads the mapped channel

**Rule implemented:** `zone_cfg_committed_zone_ct_channel()` (task.c) reads
the per-zone `zone_ct_channel[0..2]` map (wire ids 0x0320-0x0322) from the
safety processor's committed config cache — it does not assume channel index
== zone index. It is deliberately all-or-nothing: unless every zone's entry
is `set` and in range, the whole map is rejected (returns false) and the
sweep falls back to the pre-existing behaviour rather than inventing missing
entries. Legacy boards with no map committed at all fall back to `s_ct_
topology_summed` (the older binary flag), which is itself read fresh at the
start of every run, never cached stale.

For the ordinary (non-shared) per-zone derivation path,
`zone_sweep_task_record_ct_channels()` additionally refuses to derive a
channel for any zone whose `relay_mask` is not exactly `1u << zone_index`
(covered by `test_zone_sweep_record_ct_refuses_a_zone_whose_relay_is_not_
its_own_bit`), since the map's own wire semantics only hold under that
identity.

### Item 3 — unset/partial configuration refuses rather than guessing

Multiple independent refusal points, each host-tested:
- `zone_sweep_check_refusal()` returns `ZONE_SWEEP_REFUSE_CT_TOPOLOGY_UNKNOWN`
  if the topology has never been fetched from the safety processor
  (`safety_cfg_store_fetched_ms_ago() == UINT32_MAX`).
- `zone_cfg_committed_zone_ct_channel()` refuses (returns false) on any
  unanswered or out-of-range zone entry — a partial map is treated as no map
  at all, never partially trusted.
- `zone_sweep_verify_ct_attribution()` returns INCONCLUSIVE with a named
  reason for: not fitted (`ZONE_CT_VERIFY_NOT_FITTED`), no clamp ratio
  entered (`ZONE_CT_VERIFY_RATIO_NOT_ENTERED` — explicitly does NOT
  substitute a default ratio), no recorded normal current
  (`ZONE_CT_VERIFY_NO_NORMAL_CURRENT`), and shared channel (above). FAIL is
  reserved for genuine miswiring evidence (`ZONE_CT_VERIFY_WRONG_CHANNEL`,
  `ZONE_CT_VERIFY_CONFLICT`) so a merely-unconfigured state can never read as
  a false pass *or* a false fail.
- The verdict is structurally guarded: `ZONE_CT_VERDICT_PASS` is written in
  exactly one place in `zone_sweep_verify_ct_attribution()`, after every
  other branch has already returned; INCONCLUSIVE is the function's initial
  value, so an input this function does not explicitly recognize can never
  fall through to PASS.
- A stored PASS also can't outlive the configuration it was measured against:
  `ct_verify_fingerprint()` hashes every input that changes what a reading
  means (channel map, fitted bits, clamp cal, k_ct, i_normal_a); a config
  change makes `ct_verify_current_fact()` report STALE, not the old verdict
  (`test_ct_stale_verdict_is_never_reported_as_the_verdict_it_was`).

### Item 4 — web UI presents the operation correctly

- `zones_page.html`'s "Measure Zone Normal Current" panel already renders,
  after a sweep completes, which zones could not be resolved on a shared
  channel (`summed_unmeasured_mask`, rendered as "zone N not measured —
  re-sweep") and which channels got a derived CT scale
  (`k_ct_derived_mask`/`k_ct_reason`).
- The commissioning page's shared-CT section (`safety_commissioning_page.
  html`, referenced from zones_page.html's own comments) already explains
  "one CT clamp shared across zones on the same supply line" and lists, per
  channel, which zones share it.
- `setup_wizard_page.html` step 9 renders the persisted CT-attribution verdict
  (PASS/FAIL/INCONCLUSIVE/STALE/NOT_INSTALLED/NEVER_RUN) via `readiness_http.
  c`'s `readiness_ct_attribution_fact_t`, and (per commit `7590a1b7`,
  "Stop setup wizard step 9 printing a green verdict it did not earn") does
  not show a green verdict without an actual PASS.

I did not find a case where the UI would show a wrong or misleading
attribution for either topology.

## What was tested (pre-existing, confirmed still passing)

`firmware/KilnFW/App/test/test_zones_http.c` already carries, among many
other things:
- All 11 attribution-verdict scenarios from `docs/CT_ATTRIBUTION_
  VERIFICATION_PLAN.md` (correct attribution, swapped clamp, below-floor,
  no-dominant-channel, NaN, shared channel, conflict, ratio-not-entered,
  not-fitted, threshold-scales-with-ratio, no-recorded-normal, bad input).
- The mandated negative test (`test_ct_verify_below_floor_is_inconclusive_
  never_pass`): explicitly documents that deleting/inverting the single
  floor-comparison line must turn this into a false PASS, and separately
  asserts a variant that would be vacuous (all-equal channels) does NOT
  exercise that same line, so the negative test isn't accidentally hollow.
- The relay-identity and cross-zone-conflict refusals in
  `zone_sweep_task_record_ct_channels()`.
- The configuration fingerprint (stability, sensitivity to every field,
  NaN/±0 canonicalization) and the stale-verdict downgrade (case 7).
- The blob validation and NVS round trip for the persisted verdict.

## What I ran this session

- Minted worktree `C:\wt\ctzonenormal_2kfcmy` from `origin/main` via
  `tools/worktree_mint.ps1 -Label ctzonenormal -RunSetup`.
- `firmware/KilnFW/App/test/build_host_tests.ps1`: **54/54 host test
  executables built and passed** (2 expected SKIPs — `firing_score_from_
  capture` and `sim_credibility_gate` — both refuse without gitignored
  `logs/coupling/*.jsonl` captures not present on a fresh clone, which is
  documented, correct behaviour, not a failure). The zones test executable
  itself reported **4980/4980 checks passed**.
- `tools/run_all_checks.ps1 -ExecutionPolicy Bypass` (full run, all three
  phases): **113 passed, 0 skipped, 0 failed.**
- Read through `zones_current_sweep_engine.c`, `zones_current_sweep_task.c`,
  `ct_verify_store.c`, `readiness_http.c`/`.h`, `zones_page.html`,
  `config_params.c` (SaftyFW), and the two planning docs
  (`firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`,
  `docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`) end to end, cross-checking each
  claim above against the actual code rather than the comments alone.

## What I deliberately did not change

- No functional code in `zones_current_sweep_engine.c`,
  `zones_current_sweep_task.c`, `ct_verify_store.c`, `readiness_http.c`, or
  any `.html` page. I found no defect in the four checklist items against the
  current `origin/main` HEAD (`40e3ae14`).
- No new host tests were added: the existing suite already covers every
  scenario the task's four checklist items describe, including the mandated
  negative test for the floor comparison, and I did not want to add
  duplicate/overlapping tests just to have a diff.
- I did not touch `firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c`,
  which showed as modified in the *main tree's* `git status` at the start of
  this session — that is unrelated, uncommitted work from another session
  living in the shared main tree, not something this worktree carries (this
  worktree was minted fresh from `origin/main`), and per repo practice it is
  not mine to touch or judge.
- I did not flash or exercise any real hardware, per the task's explicit
  instruction.

## Suggested focus for Opus review

Given no code changed, the useful review here is an adversarial second look
at the same four claims above, specifically:
1. Is there a real scenario where `zone_sweep_shared_ch_for_zone()`'s
   `member(ch) < 2u` per-zone-CT-alone collapse could misclassify a
   genuinely shared channel as individual (or vice versa) given a
   partially-migrated `zone_ct_channel` map?
2. Whether `zone_sweep_verify_ct_attribution()`'s INCONCLUSIVE-vs-FAIL split
   for "not fitted" (never FAIL) is the correct safety posture, or whether an
   operator could exploit "leave a channel unfitted" to avoid ever failing
   commissioning.
3. Whether the "no functional change" conclusion itself is correct, or
   whether this review turns up the actual gap the owner had in mind that
   this session's code-reading missed.
