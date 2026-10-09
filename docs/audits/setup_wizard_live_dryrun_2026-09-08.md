# Setup wizard: live dry run against the real board (2026-09-08)

Board: `kilnctl.local` / `192.168.1.156`, `fw_version` `V1.0_Purchased_This_Board-1649-`,
`safety_build_commit` `36394fbd`, `uptime_s` 5928. Read-only pass: no writes, no flashes,
no firing/autotune, no relay actuation. Wizard code reviewed:
`firmware/KilnFW/App/drivers/http/setup_wizard_page.html` (2611 lines),
`setup_wizard_http.c`, `setup_progress_http.c`, `readiness_http.c`, `app.js`.

## Board's real condition (confirmed live)

- `diag_trip_reason: 5` / `diag_trip_reason_words: "S5 TC invalid/missing"`,
  `diag_trip_mask: 16`, `diag_state: 4` -- the safety thermocouple is tripped right now.
- `ct_installed=0` (`/api/safety/commissioning` id 265), `ct_topology=per_zone` (id 799,
  value 0) even though `/api/status` shows `ct_fitted:[true,true,true]` and non-zero
  `ct_counts` -- a summed CT is fitted but never commissioned as such.
- Zones are commissioned with tuned PID gains and measured plant models (`model_k_dc`/
  `model_tau_s`/`model_dead_time_s` all non-zero for zones 0-2).
- `/api/setup/progress` reads all 13 steps `pending, ts:0` -- nobody has ever clicked through
  this wizard on this board.
- `/api/crash_report`: `present:true, acknowledged:true` -- an old panic, already acked, not
  from this boot (`uptime_s` 5928 vs a stale crash record); not a live concern.

## 1. Endpoint data paths -- all resolved, no silent field-name drift

Called every endpoint the wizard's steps consume: `/api/readiness`, `/api/setup/progress`,
`/api/status`, `/api/zones`, `/api/zones/ct_channel_map`, `/api/safety/commissioning`,
`/api/profile_exec`, `/api/autotune`, `/api/autotune/matrix`, `/api/profiles`,
`/api/crash_report`. All returned 200 with the shapes `computeStepState`/each step renderer
expects -- no missing/renamed field found. `readiness.items` carries 12 keys; the wizard's
`WIZARD_STEPS` table references `network, thermo_count, relays_assigned, control_mode,
guard_max_temp, guard_cross_zone, calibration, safety_commissioned, autotune, profile_saved,
storage` -- all present. Steps 8, 9, 11 declare no `readinessKeys` by design (documented in
the code) -- not a drift, a known gap (see §2/§3 below).

## 2. Live step states actually computed

Ran `mergeAllSteps()`/`computeCompleteness()` (the actual functions from
`setup_wizard_page.html`) against this board's live `/api/readiness` + `/api/setup/progress`:

| step | title | state | safety |
|---|---|---|---|
| 0 | Welcome / preflight | pending | no |
| 1 | Network, time & units | **done** | no |
| 2 | Zone count & TC channels | **done** | no |
| 3 | TC types & cal offsets | **done** | no |
| 4 | Zone type | **done** | no |
| 5 | Relays | **done** | no |
| 6 | Zone commissioning limits | **done** | yes |
| 7 | Safety processor commissioning | **done** | yes |
| 8 | Current sensing install/cal | pending | yes |
| 9 | CT mapping verification | pending | yes |
| 10 | PID gains | **done** | no |
| 11 | Coupling matrix (optional) | pending | no |
| 12 | First profile & final gate | **done** | no |

Steps 1-7, 10, 12 read **done** purely because their mapped readiness items are
`ok`/`deliberately_off` -- `computeStepState()`'s "readiness is authoritative in both
directions" rule marks a step done even though the operator never opened this wizard, which
is correct per its own design note. Steps 8/9/11 correctly show pending (no readiness item
backs them, and they were never touched).

**Nothing here reflects the live S5 trip.** `readiness`'s `hardware` item is `ok`
(`"io=up thermo=up safety=up"`) -- confirmed in `readiness_http.c` (~line 480) this only
checks `dashboard_http_get_hw_ready()`'s io/thermo/safety **link-up** flags, not
`diag_trip_mask`/`diag_state`. A live, active S5 trip and an uncommissioned CT topology are
both real "not ready to fire" conditions with no corresponding readiness item at all.

## 3. Completeness gate verdict -- BUG: reports complete when it should not

Ran `computeCompleteness(mergedSteps, readiness)` against this board's live data:

```
GATE complete = true   reasons = []
```

**This is the exact bug the task predicted.** `computeCompleteness()` only adds a reason
when (a) a readiness item is `not_done`/`cannot_yet`, (b) a **safety-relevant step is
skipped**, or (c) a step's readiness is `unknown` (unresolved). It does **not** add a reason
for a safety-relevant step that is merely **pending** (never done, never skipped) when that
step has no `readinessKeys` -- which is exactly steps 8 and 9 (CT install/mapping) on this
board today. Because `ct_installed=0` maps to no readiness item at all, and steps 8/9 sit at
`pending` rather than `skipped`, the gate sees zero reasons and reports "Setup complete"
even though:
- CT commissioning was never done (steps 8/9 pending, safety-relevant), and
- the safety processor has an active, untripped-clear S5 fault right now (no readiness item
  even looks at this).

An owner opening step 12 or the overview right now would see the green
`gate-complete` banner ("Setup complete -- every readiness item is ok or deliberately off,
and no safety-relevant step was skipped") on a kiln that cannot currently fire (S5 tripped)
and whose CT setup was never reviewed.

## 4. Offer banner -- same blind spot, banner stays hidden

`app.js`'s `pollSetupOffer()` (~line 591) shows the banner only when some `/api/readiness`
item is `status === 'not_done'`. On this board's live readiness response, zero items read
`not_done` (one is `deliberately_off`, the rest `ok`). **The banner evaluates false and would
not appear anywhere on the site right now**, consistent with the same gap: nothing in
`/api/readiness` encodes the S5 trip or the un-reviewed CT topology, so nothing trips either
the gate or the banner.

## 5. POST-body construction vs. handler requirements

- **Zones (steps 4/5/6)**: `submitZonesConfig()`/`zoneToPostParams()` (lines ~601-688)
  already echo **every** field `zones_http_post_parse.c` requires or would otherwise
  reset/delete (`k`/`tau`/`deadtime` included) -- this is the `da6d8acf` fix, already
  landed in the wizard's own builder, not a second, diverging implementation. Steps 2/3's
  narrower per-field posts are explicitly justified in the surrounding comment against the
  parser's own documented omit-preserves. No partial-POST risk found here.
- **Safety commissioning (step 7)**: reuses `/api/safety/commissioning`'s existing write
  path and `commissioning_shared.js`'s confirm-and-read-back logic verbatim (same code the
  standalone `safety_commissioning_page.html` already uses and has its own tests for) --
  not a reimplementation, so it inherits that page's already-reviewed behavior rather than
  being a new risk surface.
- **PID (step 10)**: posts to the narrow `/api/zones/pid` endpoint (`zone`, `kp`, `ki`, `kd`
  only) and re-reads `/api/zones` to confirm -- no whole-object POST, no partial-delete
  shape possible.
- **CT mapping (step 9)** and **profile save (step 12)**: step 9 was not read in full detail
  this pass (time-boxed); step 12 only reads `/api/profiles` and links out to `/profiles`
  for the actual save (no POST body built in the wizard itself). Neither showed anything
  resembling the zones bug's shape in the code read so far, but step 9's write path was not
  exhaustively verified against its handler and should get a follow-up look before it's
  trusted at the same level as steps 6/7/10.

## 6. Honest first-run experience

An owner opening `/setup` on this exact board right now would see:
- An overview with 9 of 13 steps already marked **DONE** (1,2,3,4,5,6,7,10,12) despite never
  having opened this page before -- correct by the tool's own "readiness is authoritative"
  design, but likely to read as confusing/suspicious to a first-time user ("I haven't done
  any of this").
- Steps 8, 9, 11 pending, correctly reflecting that CT and coupling-matrix work is unstarted.
- **A green "Setup complete" gate on step 12 and the overview banner**, which is wrong: the
  safety processor has an active S5 trip and CT topology was never reviewed. This is the
  most consequential finding of the pass -- an owner would be told the kiln is ready when it
  is not, with no visual cue pointing at the safety trip anywhere in this wizard flow (the
  trip is visible on `/diagnostics` and `/status`, just not here).
- No setup-offer banner would appear on any other page of the site for the same reason.
- Nothing observed would 400 on a real save, based on the code paths read.

## What was fixed this pass

Nothing. Per instructions, no code changes were made -- the completeness-gate/banner gap
(§3/§4) and the CT/S5 blind spot in `/api/readiness`'s `hardware` item are structural, not a
one-line fix, and are reported here for routing rather than patched.

## Checks

`tools/run_all_checks.ps1` (bypass, foreground): **74 passed, 0 skipped, 0 failed** --
matches the expected count, no regressions from this read-only pass.
