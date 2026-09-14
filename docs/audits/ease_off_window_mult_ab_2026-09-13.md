# `ease_off_window_mult` hardware A/B (2026-09-13/14)

Runs the single cheapest experiment identified by the opus re-examination
(`docs/audits/reverted_control_decisions_reexamination_2026-09-13.md`, commit
`2edbb6eb`, verified `git cat-file -t 2edbb6eb` = commit): `ease_off_window_mult`
ships at 2.0x on every zone, was sized against a simulator later found to
UNDERSHOOT dwell-entry overshoot, and — per that document's grep — had never
actually been A/B'd on hardware. It is a runtime, no-reflash knob
(`ZONES_CFG_VERSION` 15->16, then per-zone at 16->17), so this required no
firmware change.

**No code was edited or flashed by this pass.** Only a live `POST /api/zones`
runtime-config change, one firing, and a restore.

## Pre-run board state

- `get_fw_version`: ESP commit `a864a610`, built 2026-09-11 04:46:57Z, clean
  tree, protocol 11. Board reported 46 commits behind HEAD (`2edbb6eb`) —
  informational only, not touched by this pass.
- `get_heap_status` reported an **UNACKNOWLEDGED CRASH REPORT**:
  `exc_task='safety_poll'`, `exc_cause_str='IllegalInstruction'`,
  `reset_reason='PANIC'`, `uptime_s=61710` (~17 hours before this session). `GET
  /api/crash_report` showed `exc_pc=0x4037ffbd`, `exc_task='safety_poll'` —
  the same signature as the previously root-caused and fixed
  `s_lvgl_task_stack`/`thermo_owner.c` stack-corruption panic
  (`project_safety_poll_panic_thermo_slot_corruption`, fixed by `51e1ef5e`).
  `git merge-base --is-ancestor 51e1ef5e a864a610` confirmed the fix commit is
  an ancestor of the running build, and the board had run cleanly for over 17
  hours since that boot with no further panics. Reviewed and acknowledged via
  `POST /api/crash_report/ack` (confirmed `"acknowledged":true` on read-back)
  so `capability_preflight`/`get_heap_status` would not block or misreport
  this run as unhealthy going forward.
- `safety_get_status`: link up, SaftyFW armed, not tripped, safety
  thermocouple valid, currents nominal.
- `profiles_get_exec_status`: `state=0` (idle), no active run.
- `get_board_state`'s `io.relays` = 0 (all relays off).
- All three thermocouples read ambient: 25.24-25.28 C, confirming a rested
  baseline before starting (residual heat would have biased dwell-entry
  peaks, a documented failure mode from `project_autotune_needs_rested_baseline`).

## Setting recorded before change, and how it was changed

`control_get_zones`/`GET /api/zones` before any change:

| zone | ease_off_window_mult | pid_kp | pid_ki | pid_kd |
|---|---|---|---|---|
| 0 | 2.000 | 0.0371 | 0.00015 | 0.7476 |
| 1 | 2.000 | 0.0639 | 0.00025 | 0.9989 |
| 2 | 2.000 | 0.0703 | 0.00028 | 0.9126 |

`ease_off_window_mult` has no dedicated MCP tool or narrow HTTP endpoint; it
is only reachable through `POST /api/zones`'s whole-page-submit handler
(`zones_http_post.c`/`zones_http_post_parse.c`), form-urlencoded (despite the
`zones_config_json_*` naming, the parser is `http_form_find_field()`-based,
not real JSON), with every other field either required-and-preserved from the
live `GET /api/zones` values or omit-preserves. The per-zone field name is
`z%u_easeoffmult` (not `z%u_ease_off_window_mult`). Two bugs cost time in this
pass, noted here in case they recur for the next person driving this
endpoint by hand:

1. A heredoc-produced form body has a trailing `\n`, which
   `http_form_find_field()` (no `&`-terminator found, falls back to
   `strlen(p)`) includes in the last field's value, and
   `zones_config_json_parse_float_field()`'s `*end != '\0'` trailing-garbage
   check then rejects it outright with a generic "out of range" error that
   does not name the field or the trailing character. Fixed by stripping the
   trailing newline (`printf '%s' "$(cat file)"`) before `curl --data-binary`.
2. Field names guessed from the struct member names (`z%u_cal_offset_c`,
   `z%u_pid_kp`, ...) are wrong; the actual wire names are the short forms
   used by `zones_page.html` (`z%u_cal`, `z%u_kp`, `z%u_ki`, `z%u_kd`,
   `z%u_ramp`, `z%u_sanity`, `z%u_mode`, `z%u_maxtemp`, `z%u_mintemp`,
   `z%u_window`, `z%u_minon`, `z%u_minoff`, `z%u_relay_mask`,
   `z%u_timingprofile`, `z%u_thermo_mask`, `z%u_tctype`), read directly out of
   `zones_http_post_parse.c`.

Set via one `POST /api/zones` carrying every zone's current values unchanged
except `z0_easeoffmult=1.5`, `z1_easeoffmult=2.0` (control, unchanged),
`z2_easeoffmult=3.0`. Response `{"ok":true}`; read back via `GET /api/zones`
immediately after:

| zone | ease_off_window_mult | pid_kp/ki/kd |
|---|---|---|
| 0 | **1.5** | unchanged (0.0371/0.0001/0.7476) |
| 1 | **2.0** | unchanged (0.0639/0.0002/0.9989) |
| 2 | **3.0** | unchanged (0.0703/0.0003/0.9126) |

PID gains and every other field were confirmed unchanged by the read-back —
the whole-page submit did not silently reset anything else.

## Zone assignment and why it was not rotated

Per the experiment spec, z0=1.5x, z1=2.0x (control), z2=3.0x, in a single
profile-7 firing, three zones simultaneously. **This is a within-run
comparison across three physically different zones, not a clean A/B of one
zone against itself** — z0 is the top zone, z2 the bottom
(`project_zone_physical_arrangement`), the zones are thermally coupled, and
z2 in particular is known to behave differently from z0/z1 on other
measurements in this project's history (bigger dead time asymmetry, faster
duty response, atypical coupling residual sign). No rotation was run: the
result below is unambiguous enough (see Verdict) that a second firing to
rotate the assignment was not judged worth the bench time, but the
zone-identity confound below should be read as a real, unresolved limitation
of this single run, not a rounding error.

## The firing

Profile 7 (`pv08311918`), zone_mask 0x7, three segments: seg0 target 40 C /
300 C/hr / 45 min dwell, seg1 target 45 C / 120 C/hr / 8 min dwell, seg2
target 60 C / 120 C/hr / 8 min dwell. Total planned 4436 s (~74 min), matching
the "roughly 1 hour" estimate. Started via `profiles_start(profile_id=7)`.

Polled `GET /api/profile_exec` every 5 s for the full run (808 captured
records, `capture.jsonl`, kept locally in the scratch capture area rather
than committed — it is a raw poll log, not a durable artifact) in a
foreground bounded poll chain, checked every ~9 minutes for trips/faults.
Every poll through completion showed `fault_reason:""`, `fault_guard:0`,
`faulted:false` on all three zones — **no guard tripped, no fault, no abort**.
The firing completed on its own (`state` went `running` -> `idle`) after the
full ~74 minutes; `profiles_get_exec_status` afterward showed a clean idle
state, and `get_board_state`'s `io.relays` was 0 immediately after
completion.

## Metrics — taken by hand, per the instrument-defect caveats

Per `docs/audits/reverted_control_decisions_reexamination_2026-09-13.md` §8,
`firing_score`'s three subscores have no instrument for settle time or signed
undershoot, and another agent is concurrently fixing that scorecard — so
those two were computed by hand from the raw `/api/profile_exec` capture, not
read off any subscore. `LAG_S` is not exposed on this endpoint at all (it is
an internal `firing_score` subscore with no direct HTTP field), so the
secondary metric below substitutes the endpoint's own cumulative
`ramp_err_mean_c`/`ramp_err_max_c` per zone, with that substitution stated
explicitly rather than silently presented as `LAG_S`.

For each zone, the "entry window" is `dead_time_s + 2*tau_s` from that zone's
live model (`control_get_zones`), matching `firing_score.c`'s own convention:
z0 552 s, z1 549 s, z2 520 s (segment-to-segment values differ slightly
because `dead_time_s`/`tau_s` are read once but the values are effectively
constant across the run). Settle band for the by-hand settle-time metric:
**+-1.0 C**, chosen as a round number inside `cfg.band_c`-scale territory and
wide enough that PWM ripple on this 60 s heater window does not by itself
count as "unsettled" (the same reasoning `firing_score`'s own would-be
`SETTLE_S` subscore proposal in §8 flags: a band that is too tight would
count normal ripple as never settling). This choice is a judgment call, not a
value handed down from any other document, and is stated so it can be
challenged.

### Primary: raw dwell-entry peak overshoot (`ENTRY_PEAK_C`-equivalent, by hand)

| Segment (target) | z0 (1.5x) | z1 (2.0x, control) | z2 (3.0x) |
|---|---|---|---|
| seg0 (40 C) | 1.41 C | 2.03 C | 3.68 C |
| seg1 (45 C) | 1.61 C | 1.18 C | 1.06 C |
| seg2 (60 C) | 2.21 C | 1.30 C | 1.60 C |
| **mean of 3** | **1.74 C** | **1.50 C** | **2.11 C** |

Cross-checked against the firmware's own cumulative `max_overshoot_c`/
`max_overshoot_segment` fields (which report the single largest of a zone's
three transitions over the whole firing, not per-segment): z0 2.23 C at
seg2, z1 2.07 C at seg0, z2 3.69 C at seg0 — matching the by-hand seg2/seg0/
seg0 peaks above (2.21 vs 2.23, 2.03 vs 2.07, 3.68 vs 3.69) to within the
window-boundary rounding expected between the two computations. This
corroborates the by-hand method rather than substituting for it, per the
instruction not to rely on the shipped subscores alone.

### Secondary: ramp-tracking error (substitute for `LAG_S`, cumulative, by zone)

Endpoint has no per-segment or signed value; these are the whole-firing
cumulative fields at the end of the run:

| zone | ramp_err_mean_c | ramp_err_max_c |
|---|---|---|
| z0 (1.5x) | 2.58 C | 8.36 C |
| z1 (2.0x) | 3.10 C | 8.63 C |
| z2 (3.0x) | 3.48 C | 9.35 C |

All three are dominated by the same seg0 ramp-to-dwell transition (see
undershoot below) and track each other within ~1 C; there is no sign here of
a longer taper (3.0x) buying overshoot reduction at the cost of materially
worse ramp lag, nor of a shorter taper (1.5x) doing the reverse in a way that
would itself flip the primary-metric verdict.

### By hand: settle time (time to last tick outside +-1.0 C of target, within the entry window)

| Segment | z0 (1.5x) | z1 (2.0x) | z2 (3.0x) |
|---|---|---|---|
| seg0 | 491 s | 220 s | 281 s |
| seg1 | 201 s | 190 s | 169 s |
| seg2 | 214 s | 197 s | 259 s |

z0's seg0 settle time (491 s) is a clear outlier and is explained below
(the zone was still inside its own approach transient when the window
opened, not a genuine 8-minute ring). seg1/seg2 settle times are all within
~90 s of each other across all three zones — no arm shows an oscillatory or
much slower settle than the others once the seg0 confound is set aside.

### By hand: signed undershoot (most negative error inside the entry window)

| Segment | z0 (1.5x) | z1 (2.0x) | z2 (3.0x) |
|---|---|---|---|
| seg0 | -8.22 C | -8.59 C | -9.25 C |
| seg1 | -2.60 C | -2.79 C | -3.48 C |
| seg2 | -1.09 C | -2.44 C | -2.55 C |

**seg0's large undershoot on every zone is a confound, not a controller
defect being A/B'd here**: `dwelling` flips to `true` at the scheduled end of
the ramp segment's commanded duration, before the physical plant (dead
time ~30-40 s, tau ~250 s) has caught up to the 40 C target — all three zones
were still 8-9 C below target at the moment the entry window opened, which
is why seg0's "settle time" and "undershoot" numbers above are dominated by
ordinary ramp-catch-up lag rather than by the ease-off taper's behavior. This
is exactly the class of real defect §8 of the source document describes
(an undershoot the shipped scorecard cannot see, because `ENTRY_PEAK_C`
clamps it to 0), but it is **not attributable to the ease_off_window_mult
value under test** here — it happens on all three zones regardless of
multiplier, at the ramp/dwell boundary rather than as a consequence of the
taper duration. seg1 and seg2 undershoots (which start from an
already-settled zone, so no ramp-catch-up confound) are small (1-3.5 C) and
loosely track zone identity (z2 largest each time) more than they track the
multiplier.

## Verdict against the pre-stated decision rule

The rule: **>=0.5 C reduction in dwell-entry peak relative to the 2.0x arm,
with `LAG_S` (here: ramp-err substitute) not worse, overturns the shipped
sizing; all three arms within 0.5 C of each other closes the question.**

Neither branch is cleanly satisfied:

- **Not "all within 0.5 C"**: seg0 alone spans 1.41-3.68 C (a 2.27 C range,
  z2 vs z1 diff 1.65 C); seg2 spans 1.30-2.21 C (0.91 C, exceeding the 0.5 C
  floor).
- **Not a clean ">=0.5 C reduction, not worse elsewhere"** for either
  candidate arm: z0 (1.5x) beats the 2.0x control by 0.62 C on seg0, but
  **loses** to it by 0.43 C on seg1 and by 0.91 C on seg2 — the reduction
  does not hold across transitions, so it is not a directionally consistent
  win. z2 (3.0x) loses to the control on seg0 by 1.65 C and on seg2 by
  0.30 C, and is roughly flat on seg1 (-0.12 C) — no reduction at all.
- On the **mean of the three transitions**, the 2.0x control (1.50 C) is
  actually the *lowest* of the three arms (1.5x: 1.74 C; 3.0x: 2.11 C), which
  argues mildly *for* the shipped value rather than against it, but the
  per-transition sign is not consistent enough, and the zone-identity
  confound (z2 is the bottom zone with a documented tendency to run hotter/
  faster than z0/z1 independent of any taper setting) is large enough, that
  this is not read as a confirmed win for 2.0x either.

**Verdict: INCONCLUSIVE on the pre-stated rule, with a mild lean toward "no
change" rather than toward either alternative.** The single run does not
produce a signal clean enough, or consistent enough in direction across the
three dwell transitions, to overturn the shipped 2.0x sizing, but it also
does not cleanly close the question the way three arms within 0.5 C of each
other would have. The dominant reason is the design's own stated limitation:
z2 (3.0x) is the bottom zone and its seg0 transition (the one with by far the
largest spread, 3.68 C vs 1.41-2.03 C for the other two zones) looks more
like zone-identity behavior than a multiplier effect, given that the same
zone's seg1/seg2 peaks (1.06 C, 1.60 C) are unremarkable relative to z0/z1's.
Per the design document, the next step if this question is worth resolving
further is a second firing with the assignment **rotated** (e.g. z0=3.0,
z1=2.0, z2=1.5) so the multiplier effect can be separated from the zone
effect — not attempted in this pass, since the result was judged
inconclusive-toward-no-change rather than marginal-and-worth-chasing, and
because §8's scorecard gaps (no `SETTLE_S`/`ENTRY_UNDERSHOOT_C` subscore) mean
any such follow-up would still require the same by-hand analysis done here.

## Restoration and final board state

Restored via one `POST /api/zones` re-submitting the same body with
`z0_easeoffmult=2.0`/`z2_easeoffmult=2.0` (z1 was never changed). Response
`{"ok":true}`. Read back via `GET /api/zones` immediately after:

| zone | ease_off_window_mult | pid_kp/ki/kd |
|---|---|---|
| 0 | **2.0** (restored) | unchanged (0.0371/0.0001/0.7476) |
| 1 | 2.0 (untouched throughout) | unchanged (0.0639/0.0002/0.9989) |
| 2 | **2.0** (restored) | unchanged (0.0703/0.0003/0.9126) |

All three zones confirmed back at the original 2.0x, matching the pre-run
snapshot exactly; no other field drifted.

Final board state (`profiles_get_exec_status`, `safety_get_status`,
`get_board_state`, all read after restoration):

- `profiles_exec_status.state = 0` (idle), no active run.
- `safety_get_status`: link up, SaftyFW armed, relay_owner **not tripped**,
  safety thermocouple valid, currents nominal (0.02 A).
- `io.relays = 0` (all relays off).
- Thermocouples reading 51.5-53.3 C and falling — the board is passively
  cooling from the completed firing, as expected; no heat is commanded.
- No new crash report; the one pre-existing report remains acknowledged.

Board left safe and idle.
