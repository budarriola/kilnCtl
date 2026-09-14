# Check-suite cross-agent failures, 2026-09-14

Four `run_all_checks.ps1` failures at the start of this pass, caused by three
other agents' concurrent, uncommitted work plus one pre-existing known-failure
pin. This note records root cause, disposition, and ownership for each.

## 1. `tools\check_coil_power_w_sentinel_guard.ps1`

**Root cause:** `firmware/KilnFW/App/test/test_zones_http.c` (added by the
agent wiring `autotune_baseline_k_dc` into the GET response) mentions
`coil_power_w` once, at line ~4203, purely in a comment enumerating other
0-sentinel fields on the same endpoint ("same convention as every other
0-sentinel field on this endpoint (hyst_c, coil_power_w,
ease_off_window_mult, ...)"). `coil_power_w_sentinel_guard_check.py` does a
plain substring search over whole file text with no comment/code
distinction, so any mention at all trips it.

**Judgment:** the guard's design is sound, not brittle. Its job is to force
review of every new file that *references* the sentinel-carrying field,
precisely because a plain grep is the only mechanical way to catch a new
producer/consumer before it's reviewed for the 0.0f hazard — and the guard's
own docstring already prescribes the remedy ("Review it, then add it to
ALLOWED_FILES"). Teaching it to parse comments vs. code would add real
complexity (a C comment/string-literal scanner) to save exactly one manual
review step, and would create a blind spot: a hazardous reference hidden
inside a comment-stripped macro or string constant would then pass
silently. Distinguishing "reads the field" from "mentions the field" is
what a human reviewer is for.

**Fix:** reviewed the reference (comment-only, no code touches the
sentinel) and added `test_zones_http.c` to `ALLOWED_FILES` in
`tools/PcTools/scripts/coil_power_w_sentinel_guard_check.py`, with a comment
recording the review.

**Negative test:** temporarily changed the required guard string in
`zones_current_sweep_engine.c` (`coil_power_w_override > 0.0f` →
`coil_power_w_override != 12345.0f`) — check failed loud, naming the file
and the missing guard. Restored the line by hand; `git diff` on that file
is empty; check passes again (exit 0).

## 2. `tools\PcTools\check_zones_per_zone_field_drift.ps1`

**Root cause:** `autotune_baseline_k_dc` was added to firmware at
`ZONES_CFG_VERSION` 25→26 (`zones_http_get.c`, docs/audits/
zones_get_autotune_baseline_exposure_2026-09-13.md) with no matching entry
in the PC client's field tables. Per that firmware comment,
`zones_http_post_parse.c` has **no** `z%u_` POST key for this field —
`adaptive_tune.c` is its only writer, and a whole-page POST must preserve
whatever is stored (same class as `tuning_*`/`model_fit_*`).

**Fix:** added `autotune_baseline_k_dc` to a new
`_ZONE_AUTOTUNE_BASELINE_READONLY_KEYS` set, folded into `_ZONE_READONLY_KEYS`,
in `tools/PcTools/src/kilnctrl/zones_http_client.py` — not
`_ZONE_FIELD_FORM_KEY`, since it has no POST suffix to round-trip.

**Negative test:** temporarily set the new readonly set to `set()` — check
failed, correctly naming `autotune_baseline_k_dc` as missing from the
client. Restored by hand; check passes.

**Status at the end of this pass:** the firmware side landed (verified in
`zones_http_get.c`) and my client-side fix is correct and self-consistent —
confirmed by running the check script directly against the committed
firmware. However, **the live tree currently fails this check again**, for
an unrelated reason: a different concurrent agent (owning `firing_score.c`
et al. per the task's exclusion list — actually traced to the agent working
on `test_zones_http.c`/`zones_http_get.c`) has, as of this writing, a
**live, uncommitted negative test** injecting a field named
`NEGATIVE_TEST_OVERFLOW_FIELD_DELETE_ME` into `zones_http_get.c`'s GET
response (confirmed via `git status --porcelain` showing both
`zones_http_get.c` and `test_zones_http.c` modified, not yet committed).
That is their in-progress negative-test scaffolding, not a defect in my
fix or theirs — it will disappear when they restore by hand. I did not
touch that file. Re-run `check_zones_per_zone_field_drift.ps1` /
`tools/PcTools/selfcheck.py` once that agent's work lands to confirm both
are green; as of the last check in this session they were still mid-test.

## 3. `tools\PcTools\selfcheck.py`

**Diagnosis:** same root cause as (2), not independent. `selfcheck.py`
runs `zones_per_zone_field_table_checks()` (from `selfcheck_zones_fields.py`)
among its other sections; with my client fix applied and no other agent's
WIP present, a direct run showed `all checks passed`. The only reason it
now shows red in a full-suite run is the same transient
`NEGATIVE_TEST_OVERFLOW_FIELD_DELETE_ME` collision described in (2).

## 4. `firmware\KilnFW\App\test\check_sim_iter_tune_bars.ps1`

**Not touched**, per instructions — owned by the agent unwiring the new
subscores from `firing_compare.c`'s verdict. Ran it read-only: as of this
session's run it is **currently PASS** (A1 24/660 = 3.6364%, exactly at
the pinned known-failure ceiling, not over it; A2/A5/A6 all clear). This
looks like that agent's in-progress fix already landed in the working tree
(uncommitted). Not verified further and not claimed as fixed by this pass —
attribute any remaining flakiness on this check to that agent's own
commit when it lands.

## Guard-suite design note

No check was weakened. (1) and (2) were both genuine drift — a reviewed
new reference and a genuinely un-mirrored field — not false positives from
an over-broad rule. The full-suite red seen at the end of this pass is
cross-agent timing noise (an in-flight negative test on a shared, unpushed
tree), consistent with this repo's known concurrent-session hazards
(`git commit -o`, never `git checkout --`/`stash`).

## Final tally (this session's own checks, run in isolation)

- `tools\check_coil_power_w_sentinel_guard.ps1` — **PASS** (fixed, negative-tested).
- `tools\PcTools\check_zones_per_zone_field_drift.ps1` — **PASS** in isolation
  against committed/reviewed firmware; **red in the shared tree** only while
  another agent's uncommitted negative test is live — not this fix's fault.
- `tools\PcTools\selfcheck.py` — same as above, same root cause.
- `firmware\KilnFW\App\test\check_sim_iter_tune_bars.ps1` — **PASS**, not
  touched, attributed to the firing-score agent's in-progress work.

Files changed by this pass:
- `tools/PcTools/scripts/coil_power_w_sentinel_guard_check.py`
- `tools/PcTools/src/kilnctrl/zones_http_client.py`
