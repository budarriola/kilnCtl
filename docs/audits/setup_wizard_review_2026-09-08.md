# Setup wizard review — 2026-09-08

Scope: the 13-screen `/setup` guided flow as one artifact, as an operator meets
it. Commits `e949dc7e` (plan), `90bd8b2c` (progress store), `24ba3568` (shell),
`dd9deb74` (0-3), `b46c120c` (4-6), `da6d8acf` (omit-delete fix), `b284bd11`
(step 7 + 8-12). Files: `firmware/KilnFW/App/drivers/http/setup_wizard_page.html`
(2307 lines), `.../http/commissioning_shared.js`,
`firmware/KilnFW/App/test/test_setup_wizard.js`,
`firmware/KilnFW/App/test/check_setup_wizard_zones_post_helper.ps1`.

Review only. Two fixes applied (§9); everything else is handed back.

Verdict: **the flow is sound in its bones** — dependency order is right, the
anti-drift merge is real and honoured, and the single zones-POST helper holds.
Four defects are real and one of them is safety-bearing.

---

## 1. Walking screens 0 → 12

Dependency order is correct and each screen that depends on an earlier one
*says so and refuses to render* rather than guessing:

| Step | Depends on | Enforcement |
|---|---|---|
| 3 (TC types) | 2 (thermo_count) | `setup_wizard_page.html:1634` — "finish step 2 first", no editor rendered |
| 4 (zone type) | 2 | `:1756` same |
| 5 (relays) | 2, 4 | `:1873` same; on/off badge from `zone_type` at `:1823` |
| 6 (limits) | 4 | `:2000`; ramp field hidden for `zone_type===1` (`:1976`) |
| 8 (CT review) | 7 (`ct_installed`) | `:848` — "unknown until step 7 is completed" |
| 9 (CT sweep) | 7, 8 | `:922` reads `ct_installed`, offers the no-CT exit |
| 10 (PID) | 2, 4, 5 | `autotunePrecheck()` `:1012` refuses no-thermo / on/off / no-relay |

A user who follows 0→12 on a blank board ends with a correctly configured kiln.
No step assumes state an earlier step fails to establish.

Two ordering observations, neither a defect:

- **6 before 7 is the right call even though 6 wants 7's number.** On a virgin
  board `getAbsMaxTempC()` (`:712`) returns `null`, step 6's ceiling half is
  skipped (never treated as 0), and step 7 then carries the invariant in the
  other direction. Both halves are re-checked at commit against a fresh fetch
  (`:2047` and `:2181`). This is correct, and deliberately so.
- Step 1 configures the network over a connection the operator already has
  (AP fallback). Not circular.

**Things a first-time user cannot answer from what is on screen:**

1. **Step 8 sends the operator out of the wizard for work step 7 already did.**
   `:842` says CT installed/topology "is written through the safety
   processor's guided commissioning flow — *open it →*". But step 7
   (`:2138`, `:2145`) writes `ct_installed` (id 265) and `ct_topology`
   (id 799) *inline*. The operator is told to go do a thing they just did.
   Worse, the thing step 8 genuinely cannot do — per-channel CT **gain/offset
   calibration** — is bundled into that same sentence and never separated out,
   so the one item that really does require leaving is invisible.
   *Fix:* split the sentence — say installed/topology were set in step 7, and
   link out only for per-channel gain/offset.
2. **Step 6's `min_temp_c` has no default and no guidance.** `:1985` renders
   `value="' + z.min_temp_c + '"` with the label "below this reads as a
   broken/disconnected sensor" and no suggested value; `zoneToPostParams()`
   (`:573`) defaults it to `-20` only when absent. A first-timer has nothing to
   answer from. (Note `:1985` will also print `undefined` into the input if the
   board ever omits the field, unlike every neighbouring field which is
   `|| 0`-guarded.)
3. **Step 7's `abs_max_temp_c` hint** (`:2126`) is genuinely good — "the highest
   temperature this kiln's furniture/elements/brick can survive, not the
   hottest firing you plan". Called out as the model the other fields should
   follow.

---

## 2. Anti-drift: is `/api/readiness` authoritative end to end?

**Yes, on the merge path — and it is the best-built part of the wizard.**

`computeStepState()` (`:313`) is the single place step state is decided.
Every screen renders from `latestMerged` via `gRenderStepDetail()` (`:2219`);
no screen computes its own done/pending. Grep confirms no second reader of
`latestProgress` outside `mergeAllSteps()` (`:2287`). Both directions hold:
a stored `done` contradicted by `not_done` becomes `regressed` (`:322`), and a
step never visited whose readiness reads `ok` is allowed to show `done`
(`:334`, `allOkOrOff`). No screen trusts stored state.

Three holes, in descending severity:

**2a. A failed `/api/readiness` fetch renders a green "Setup complete" banner.**
`loadAll()` (`:2282`) falls back to `{ items: [] }`. `computeCompleteness()`
(`:376`) iterates `readiness.items` — an empty list produces **zero reasons**,
so `complete === true`, and `gRenderOverview()` (`:1293`) paints
`gate-complete`: *"Setup complete — every readiness item is ok or deliberately
off."* The offline warning at `:195` is shown directly above it, but the two
contradict each other and the green banner is the louder one. The same false
verdict is repeated inline by `renderStep12()` (`:1166`).
*What the operator experiences:* board loses Wi-Fi mid-setup, they hit Refresh,
and the wizard tells a half-configured kiln it is ready to fire.
*Fix:* `computeCompleteness()` should push a reason when
`readiness.items.length === 0` — "the readiness checklist could not be read;
completeness is unknown, not confirmed". Two lines, and it makes the fallback
fail closed the way `readiness_http.c:630`'s own `checklist_truncated` item does.

**2b. `cannot_yet` does not regress a stored `done`.** `:321` tests only
`anyNotDone`. A step stored `done` whose readiness item has since become
`cannot_yet` (prerequisite lost) falls through to `:334` and renders **DONE**.
Completion is still blocked globally by `:379`, so this is cosmetic-but-
misleading rather than dangerous — the stepper shows green on a step whose own
checklist entry says "cannot determine".

**2c. A truncated checklist silently returns steps to stored-state authority.**
If `readiness_http.c`'s `append_item()` drops an item (`:90`/`:99`), that key
vanishes from `byKey`, `items` is empty for the mapped step, and `:334` renders
whatever the progress blob claims. `checklist_truncated` blocks *completion*,
correctly — but the per-step pills lie. Worth one line in `computeStepState()`:
if a step declares `readinessKeys` and resolved none of them, it cannot be
`done`.

Also noted: readiness key `hardware` (`readiness_http.c:497`) is mapped to no
wizard step. Harmless — `computeCompleteness()` scans all items, not just
mapped ones — and correct, since no single screen owns it.

---

## 3. Safety confirmations: four screens, four different idioms

The consequence-bearing screens have **drifted apart**, and the drift is
monotone: the later the pass, the stronger the confirmation.

| Step | Consequence | Idiom | Strength |
|---|---|---|---|
| 4 | Disables guards 1, 2, 3, 4, 9 per zone; zeroes its coupling row/column | plain `Save & mark done` (`:1780`) | **none** — no confirm, no read-back |
| 6 | Sets `max_temp_c` — hard trip, no debounce | plain `Save & mark done` (`:2038`) | **none** — no confirm, no read-back |
| 7 | Writes RP2040 flash | named `confirm()` + busy check + independent second read-back (`:2196`) | **full** |
| 9 | Applies heat, energizes relays | checkbox ack + busy check (`:1969`) | partial — no `confirm()` |

**Step 4 is the weakest and carries the largest consequence.** Its own
`ZONE_TYPE_CONSEQUENCE_TEXT` (`:658`) states that five guards are DISABLED —
and then the click handler at `:1780` POSTs with no confirmation at all. An
operator can disable five guards on a zone with one mis-click of a `<select>`
followed by a button labelled the same as step 1's timezone save.
*Fix:* route steps 4 and 6 through a named confirm listing old→new per zone —
`kcCommissioningFindCriticalChanges()`'s display shape is already the right one,
and `commissioning_shared.js` is already loaded on this page.

**Step 9's checkbox is weaker than step 7's `confirm()`** for a step that
actually applies heat. The banner text (`:934`) is excellent — it names the
tab-closing hazard explicitly, per `project_stopping_host_does_not_stop_firing`
— but a checkbox is a lower bar than a modal for the more physical action.

### abs-max invariant — both directions present, neither clamps ✅

- **6 refuses a zone limit above the ceiling:** `validateStep6()` `:743-749`.
  Message ends "Refused, not clamped."
- **7 refuses a ceiling below a zone limit:** `validateStep7()` `:1228-1233`.
  Message ends "refused, not clamped."
- Both re-fetch fresh at commit (`:2047`, `:2181`), so a change made in another
  tab cannot slip through.
- No `Math.min`/`Math.max` clamp of either value anywhere; both `reduce(Math.max)`
  uses compute *the highest zone ceiling to compare against*, not a clamped input.
- `absMaxTempC === null` skips the ceiling half rather than treating it as 0
  (`:721`) — correct, and tested.

This is the requirement the flow got most right. One defect inside it:

**3a. Step 7 compares against zone slots the wizard does not manage.**
`:2086` and `:2183` both do
`((zonesData && zonesData.zones) || []).map(z => z.max_temp_c || 0)` over the
**whole** array. `zones_http_get.c:290` emits all `MAX31856_CHANNEL_COUNT` (5)
slots, and `zones_http_post_parse.c:113` deliberately *preserves* slots at
`i >= thermo_count`. Step 6 correctly slices (`liveZones.slice(0, thermoCount)`,
`:2043`); step 7 does not.
*What the operator experiences:* a board once configured for 5 zones at 1300 °C,
now running 3 zones at 900 °C, refuses `abs_max_temp_c = 1000` with
*"BELOW the highest configured zone max_temp_c (1300 °C)"* — naming a
temperature that appears on no screen in the wizard, with no way to clear it.
Hard stop, no workaround inside the flow.
*Fix:* slice both to `zonesData.thermo_count`, matching step 6.

---

## 4. The single zones-POST helper — holds ✅

- `grep "fetch('/api/zones'"` over the file: **one** hit, line 641, inside
  `submitZonesConfig()`.
- `check_setup_wizard_zones_post_helper.ps1` passes and is picked up by
  `tools/run_all_checks.ps1`'s recursive `check_*.ps1` glob (confirmed by name
  in the run output — it is not an orphan).
- Steps 2, 3, 4, 5, 6 all merge onto the live GET and call the helper.
  `zoneToPostParams()` (`:555`) echoes `k`/`tau`/`deadtime` on every submission.
- Step 10 uses `POST /api/zones/pid` (`:1087`) — a genuinely narrow handler
  (`zones_http_pid.c`) with no omit-delete semantics. Correct, not a second path.
- Step 5's relay names: only *unowned* relays get a `relayN_name` param, but
  `zones_http_post.c:330` explicitly preserves omitted names. Correct.
- Steps 7/8 write the Pico via `/api/safety/commissioning` only. No second zones
  path was added by the later screens.

---

## 5. `commissioning_shared.js` vs `safety_commissioning_page.html`

**Real drift risk, not acceptable debt — and the drift already happened.**

`safety_commissioning_page.html` still owns `findCriticalChanges()` (:1417),
`checkFiringOrAutotuneRunning()` (:1453) and its own commit/read-back block
(:1615-1720). Two implementations of one safety-critical contract.

That the risk is real is not speculative: **the third, ad-hoc copy of the busy
check in step 9 shipped broken** (§7a below). The extraction was done and then
not used, which is the worst of both worlds — a file whose header claimed
(incorrectly, until this review) that "neither page owns a second copy any
more", giving a future reader false confidence that one edit is enough.

*What I would do:* retrofit `safety_commissioning_page.html` onto
`commissioning_shared.js` in a dedicated pass — the three functions are
behaviourally identical, and `test_guided_flow.js` already exists to hold the
line. Until then, treat any edit to the shared file as requiring a hand-mirror.
I corrected the header comment to say exactly this (§9).

---

## 6. Completion-gate honesty — wrong in *both* directions

**Can report complete on a kiln that would refuse to fire:** yes — §2a. Lose
`/api/readiness` and the banner goes green unconditionally.

**Can refuse to complete a kiln that is genuinely ready:** yes, and this one is
a permanent trap.

`WIZARD_STEPS` marks step 9 `safety: true` **unconditionally** (`:287`), with a
comment at `:274` reasoning that over-marking is "ratchet-safe... blocks
completion a little too eagerly, never too little". But `renderStep9()` (`:923`)
handles `ct_installed === 0` by rendering *"Skipping this step is a legitimate
finish"* and a button that calls
`postStepState(9, 'skipped', 'ct_installed=0, nothing to verify')` (`:930`).
`computeCompleteness()` (`:386`) then pushes
*"Step 9 (CT mapping verification) is safety-relevant and was skipped"* —
**forever**.

*What the operator experiences:* on a kiln with no CTs — which per
`project_no_cts_fitted_guard_coverage` is the normal state of this bench — they
press the button the wizard told them was the legitimate finish, and the wizard
permanently refuses to say complete, with no reachable path to clearing it. The
two screens contradict each other in the same session.

*Fix:* the no-CT exit should `postStepState(9, 'done', 'ct_installed=0')` —
`ct_installed=0` is a *complete* answer, exactly as step 8 already treats it
(`:869`, its Done button) and exactly as `deliberately_off` is treated at
`:380`. Alternatively make `safety` a predicate over live `ct_installed`, but
the one-word change is the honest one and matches step 8's existing precedent.

Step 11's skip is correctly `safety: false` (`:289`) and does not block.

---

## 7. The JS as a whole

**7a. `step9CheckBusy()` compared against state strings the firmware never
sends — FIXED, see §9.** `:909` tested `exec.state === 'RUNNING' || 'PAUSED'`.
`exec_state_name()` (`dashboard_exec_http.c:37-46`) emits lowercase
`idle/running/paused/done/faulted`. **The busy check could not fire.** Step 9
starts a relay-energizing heat sweep; its only other gate is a checkbox and the
firmware's own `zone_sweep_refusal_str()`. Symmetrically,
`at.state !== 'IDLE' && at.state !== 'idle'` treated a *finished* autotune
(`'done'`, `dashboard_json.c:214`) as busy, so after any completed autotune
step 9 would refuse forever with "autotune is currently running".
`commissioning_shared.js:52-60` had it right all along — this is the cost of
§5's duplication, landing within days of the extraction.
**The test was complicit:** `test_setup_wizard.js:492` fed
`{ state: 'RUNNING' }`, a value no firmware path produces —
`project_idealized_test_input_bug_class`, verbatim. Green suite, zero coverage.

**7b. Every navigation renders twice and double-fetches.** `gGoto()` (`:1302`)
sets `window.location.hash`, then renders synchronously; the assignment fires
`hashchange` (`:2302`), whose handler calls `gGoto()` again. Two full renders,
two `/api/zones` fetches per step open (four for steps 6 and 7, which fetch
zones *and* commissioning). Not a correctness bug — every render is idempotent
and rebinds onto fresh `innerHTML` — but it doubles HTTP load on a board with
`project_httpd_wedge_is_dram` / `project_http_resets_acceptmbox` history.
*Fix:* early-return in `gGoto()` when the hash already matches the target.

**7c. Every "Saved." confirmation is erased milliseconds after it appears.**
Steps 1/3/4/5/6/7 all set `okEl.innerHTML = 'Saved.'` and then call `loadAll()`
(e.g. `:1810-1812`), which re-enters `gGoto()` → `renderStepN()` → fresh
`innerHTML`. The operator sees a flash, then a screen that looks like nothing
happened. Set the message *after* `loadAll()` resolves, or hoist it out of the
step body.

**7d. Unscoped selector in `step2ReadCurrentZones()`.** `:1489` uses
`document.querySelector('.zonecard[data-zone="' + i + '"]')` while step 3
(`:1613`) and steps 4/5/6 correctly scope to their own body (`#stepBody3`,
`el.querySelector`, `#s5zones`). Steps 4, 5 and 6 render `.zonecard[data-zone]`
into bodies that stay in the DOM (only `hidden`) after being visited. It works
today **only** because `stepBody2` precedes `stepBody4/5/6` in source order
(`:217` vs `:221-223`) and `querySelector` returns first-in-document-order.
Reorder the divs and step 2 silently saves step 6's values. Latent, one word to
fix (`'#stepBody2 .zonecard...'`), and worth fixing before it becomes real.

**7e. Dead code:** `window.kcStep8Ct` (`:863`) is written and never read
anywhere in the file.

**7f. Duplicated constants (declared, not accidental):** `TC_TYPES` (`:437`,
8 entries with ranges) and `STEP7_TC_TYPES` (`:1186`, 8 bare labels) are two
lists of the same `uart_task_ids.h` enum in one file, for two different
purposes. `ZONE_CT_CHANNEL_COUNT_JS = 3` (`:826`) correctly mirrors
`zones_config_accessors.h:126`; `THERMO_COUNT_MAX = 3` / `RELAY_COUNT_MAX = 4`
mirror `zones_page.html`. All are commented as mirrors. Acceptable, but they are
`project_reset_one_side_bug_class`-shaped: two copies, one contract, no
mechanical link.

**7g. No duplicated function or top-level `var` names** across the four authors'
work (checked by name extraction — zero collisions). Notably clean for 2307
lines from four passes. No competing event bindings: every handler is attached
to freshly-written `innerHTML`, and both interval timers (`step2Poll`,
`step9Poll`) plus `step3PollTimer` are cleared in `gRenderStepDetail()`
(`:2253-2255`) before any re-render.

---

## 8. Defect summary

| # | Location | Severity | Status |
|---|---|---|---|
| 7a | `setup_wizard_page.html:909` + `test_setup_wizard.js:492` | **safety** — heat sweep startable during a live firing | **fixed** |
| 6 | `:287` / `:930` | high — CT-less kiln can never complete | handed back |
| 2a | `:376` / `:2282` | high — false "complete" when readiness is unreachable | handed back |
| 3a | `:2086`, `:2183` | high — step 7 unfixably refused by unmanaged zone slots | handed back |
| 3 | `:1780`, `:2038` | medium — steps 4/6 have no confirmation at all | handed back |
| 5 | `commissioning_shared.js` | medium — two copies of one contract | comment corrected |
| 1.1 | `:842` | medium — step 8 misdirects, hides the real gap | handed back |
| 7b | `:1302` | low — double render/fetch per navigation | handed back |
| 7c | `:1810` etc. | low — "Saved." erased instantly | handed back |
| 7d | `:1489` | low/latent — unscoped selector | handed back |
| 2b/2c | `:321`, `:334` | low — misleading pills | handed back |
| 1.2 | `:1985` | low — `min_temp_c` unguided, may print `undefined` | handed back |
| 7e | `:863` | trivial — dead global | handed back |

## 9. Changed by this review

1. `setup_wizard_page.html:909-912` — `step9CheckBusy()` now compares the
   lowercase state strings the firmware actually emits, and uses the same
   `idle/done/aborted` allowlist as `commissioning_shared.js:59`. A comment
   records why. **Negative-tested:** reintroducing `'RUNNING'`/`'PAUSED'` makes
   the two new assertions fail; restored by hand (no `git checkout --`), suite
   back to green.
2. `test_setup_wizard.js` — the `'RUNNING'`/`'IDLE'` fixture corrected to
   `'running'`/`'idle'`, plus three new cases (paused firing, running autotune,
   and a *finished* run not blocking forever). 106 → 112 assertions, all pass.
   A comment warns against "fixing" a future failure by re-uppercasing.
3. `commissioning_shared.js` header — replaced the false claim that neither page
   owns a second copy with an explicit KNOWN DEBT block naming
   `safety_commissioning_page.html`'s line numbers and the mirror-by-hand rule.

Nothing else was touched. No firmware C, no board writes, no flash, no reset.
`tools/run_all_checks.ps1`: 68 passed, 1 failed — `check_hal_include_boundary.ps1`
on `persist/cfg_fs_mount.c`, the known RED on off-limits `cfg_fs*`, unrelated.
