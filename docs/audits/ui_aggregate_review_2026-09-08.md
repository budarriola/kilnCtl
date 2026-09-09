# Aggregate UI / firmware-facing review — 2026-09-08

Review only. **No production code was changed by this pass.** Every finding below is
handed back; nothing was "fixed in passing".

Scope: the day's UI-relevant commits, all of which resolve:
`172e3081`, `0a5b642c` (on/off zone UI), `9d515708` (dashboard current-sense grouping),
`d3756497` (info-icon placement), `40ae1891` (relay-reset button theming),
`c86623c9` (dashboard K4 label), `456b0fa4` (recovery banner in `app.js`),
`d5d23b98` (safety TC faults in diagnostics), `7f7e3d0e` (crash-report fields),
`fd02df05` (safety TC type/offset controls), `a24fa033` (PcTools zones field table),
`dd1d6ada` / `bf1db47f` (on/off rule storage and actuation).

Note for future readers: the shared web files moved to
`firmware/KilnFW/App/drivers/http/` — the paths in this review are the live ones.

---

## 1. Clobbered / partially reverted commits — NONE FOUND

Method (not commit messages): for each of the thirteen commits, every added line of
length >= 25 chars was extracted from `git show -U0` and searched for verbatim in the
file's current HEAD content. Thirteen apparent misses were then run down by hand:

* Ten were an encoding artifact of the comparison itself, not the tree — every one of
  those lines contains a non-ASCII character (degree sign, em dash, curly apostrophe).
  All ten are present in the files (e.g. `safety_commissioning_page.html:506`
  `{ id: 266, name: 'tc_offset_c'`, `CONFIG_REFERENCE.md:47` `tc_offset_c`,
  `app.js`'s `RECOVERY_BANNER_TEXT`).
* Three were `dd1d6ada`'s test lines in `test_profile_executor_prestart.c`, legitimately
  rewritten fifteen minutes later by `bf1db47f` (which explicitly reworks that file,
  24 lines changed) as the decision core was wired to the real relay. The behaviour is
  still asserted — `g_last_relay_write_mask` / `claimed_relay_mask` assertions live at
  `test_profile_executor_prestart.c:1329-1660`.

Secondary check for the other half of the failure mode (a merge that *duplicates* rather
than drops): every line >60 chars in `main_page.html`, `zones_page.html`,
`diagnostics_page.html`, `app.js`, `theme.css` was counted for exact repeats. All repeats
are ordinary idiom (`method: 'POST', headers: {...}`, comment rules, the
`parseInt(... relayCount ...)` preamble), no duplicated blocks, no half-applied hunks.

**Conclusion: despite the reported dirty-tree commits, no hunk was lost or duplicated.**
This was the highest-value question and the answer is clean.

## 2. JS coherence — clean, one cosmetic transient

* No function defined twice in any page or in `app.js`. (`function x` / `yDuty` / `yTemp`
  in `main_page.html` and `function apply` in `safety_commissioning_page.html` are
  distinct local scopes, verified by reading, not by the grep.)
* No handler bound twice; no `getElementById` target that is never rendered. The one
  apparent miss, `gscreen` in `safety_commissioning_page.html:1794`, is
  `'gscreen' + i` against `#gscreen0..4`.
* No render function orphaned: `renderCurrentCard` (`main_page.html:780`) has its single
  call site updated to the new two-argument form at `main_page.html:1220`.
* The two `app.js` banners do not race: `pollRecoveryMode()` runs on its own 20 s timer
  and, on fetch failure, deliberately leaves the banner in its last confirmed state;
  `hideBanner()` only ever touches the connection banner.
* All 14 JS host harnesses pass (`check_js_host_tests.ps1`), including those added today
  (`test_current_sense_grouping.js`, `test_recovery_banner.js`,
  `test_safety_tc_diagnostics.js`, `test_safety_relay_pill.js`).

**Cosmetic, low severity** — `main_page.html:780-800`: `renderCurrentCard(data, zones)`
treats `zones !== undefined` as "apply the grouping filter". The live call site passes
`zonesCache.zones`, which is `[]` until `loadZones()` resolves, so between the first
status poll and the first zones load every channel scores `claimants.length < 2` and the
whole Current-sense card is suppressed (`parts.length === 0` -> `return null`). It
appears on the next poll. Not worth a fix on its own, but worth knowing if someone
reports "the current card flickers in on load".

## 3. Page/handler JSON field agreement — clean

Checked by extracting emitted keys from the C handlers and consumed keys from the pages:

* Zones round-trip is complete for the new on/off fields. `zones_http_get.c:323` emits
  `zone_type`, `failsafe_state`, `hyst_c`, `min_on_s`, `min_off_s`,
  `on_off_hyst_c_default`, `on_off_min_on_off_s_default`; `zones_page.html:1094-1104`
  reads all seven; `zones_page.html:2052-2056` posts `z<N>_zonetype/_failsafe/_hystc/
  _minons/_minoffs`; `zones_http_post_parse.c:204/218/235/255/269` parses exactly those
  keys; `tools/PcTools/src/kilnctrl/zones_http_client.py:322-366` carries the same five
  plus `relay_type` in its POST table. No orphan on either side.
* Diagnostics safety-TC block agrees: `diagnostics_http.c:230` emits exactly
  `no_link` / `faulted` / `not_converting` / `ok`, and `diagnostics_page.html:1231-1265`
  branches on exactly those four. `tc_c`/`cj_c` are rendered through `char tc_buf[16]`
  (`diagnostics_http.c:258`), so a NaN safety reading emits valid JSON rather than a bare
  `nan` token — the obvious way this would have broken is not present.
* The recovery banner spends no new JSON: `app.js` sources `recovery_mode` from the
  pre-existing `GET /api/ota/esp/status`.

## 4. Theme rules — held, with one inaccurate comment

Across all of today's `.html`/`.css` diffs the only colour literals introduced are two
occurrences of `#fff`, both as foreground on an already-tokened accent fill
(`theme.css:349` `.kc-recovery-banner`, `diagnostics_page.html:161`
`.relay-reset-btn:hover`) — the same pairing `.kc-conn-banner` / `.kc-stop-btn` already
use. No new palette values, and every new control uses `var(--...)` tokens.

**Documentation defect, no visual bug** — `diagnostics_page.html:141-155`: the comment
justifying `.relay-reset-btn` describes it as reusing `button.danger-btn`'s
"outlined-red" treatment via `--fault-color`. On *this* page `--fault-color` is mapped to
`var(--warn)` (`diagnostics_page.html:25`, `:32`, `:39`), not to `--bad`/`--ui-accent-5`
as it is on `backup_page.html` / `settings_page.html`. The buttons therefore render in the
page's warn colour, not red. The rule is still token-correct and internally consistent
with the rest of the diagnostics page; only the comment (and `40ae1891`'s message)
overstates it. Left alone deliberately — changing the token would change how a
destructive control reads, which is an owner call, not a review fix.

## 5. Responsive sweep — a flake, not a geometry failure

`tools/run_all_checks.ps1` initially reported `check_ui_responsive_sweep.ps1` FAILED.
Re-run in isolation, with nothing else loading the machine:

```
All 111 (page, width) checks passed.
check_ui_responsive_sweep.ps1: sweep passed.
```

and the subsequent full `run_all_checks.ps1` also passed it. This includes both new
`main_page.html [recovery_hidden]` / `[recovery_shown]` fixtures at all six widths, so
the recovery banner's in-flow placement is genuinely verified rather than merely
untested. **The first failure was Chrome/CDP flakiness under concurrent load, as the
agents reported; there is no real geometry regression today.**

## 6. Contradictory / unreachable UI

### 6a. Relay-timing (ms) fields stay visible on an On/off device zone — REAL, hand back

`zones_page.html:1041-1048`. The `<label>Relay timing (ms, 0 = use firmware default:
60000/10000/2000)</label>` and its `row2` of Window / Min on / Min off inputs are **not**
wrapped in `.heaterOnly`, unlike the blocks immediately above and below them
(`:1021`, `:1027`, `:1049`). `toggleZoneTypeUi()` (`zones_page.html:1545`) therefore
leaves them on screen when the operator selects "On/off device".

What the user sees: an on/off zone shows the `.onoffPanel`'s "Minimum ON time (s)" /
"Minimum OFF time (s)" (`zones_page.html:1129-1133`) and then, a few rows lower, a second
"Min on (>= 10000)" / "Min off" pair in **milliseconds** that does nothing. Two
contradictory minimum-on controls, in two different units, on one form.

That these really are inert for an on/off zone is confirmed on the firmware side:
`profile_executor.c:1288-1290` reads `zones_config_get_min_on_s/min_off_s` for the on/off
path and `on_off_trigger_decide.c:143` enforces them; `heater_window_ms` /
`heater_min_on_ms` are consumed only by the heater PWM path.

The zone-type warning paragraph (`zones_page.html:1112-1116`) enumerates what is hidden —
"PID gains, ramp rate, control mode and those guard thresholds" — and would need its text
updated alongside any fix. Not applied here: `zones_page.html` was under concurrent edit
today and the paragraph is owner-facing copy, so this is not the "trivially safe" class.
(`Minimum rise rate` at `:1031` is arguably in the same category but is a weaker case —
left for the owner to decide.)

### 6b. The recovery banner says firing is unavailable; nothing stops the user starting one

`app.js:435-440` states "Firing is NOT available." That is true — `main_control_bringup.c`
does not start `profile_executor` in recovery mode — but no HTTP route enforces it:
`grep boot_guard_is_recovery_mode` across `App/` hits only `ota_http_esp.c`,
`ota_http_recovery.c`, `cfg_fs_mount.c`, `boot_guard.c`, `screen_idle.h`,
`main_boot_early.c` and `main_control_bringup.c`. Neither `profiles_http.c` nor
`profile_executor_start.c` consults it.

What the user sees: the loud, undismissable banner on every page, and directly below it a
fully enabled Start control on the dashboard and Profiles page. Clicking it produces
whatever the prestart-hardened accessors return, not an explanation. The banner is
honest; the page around it is not. The fix belongs with whoever owns the recovery work —
either disable the start controls while `recovery_mode` is true, or refuse the start
route with a reason naming recovery mode.

### 6c. No banner collision today

The setup-wizard work (`e949dc7e`) is **plan-only** — `ROADMAP.md` and
`docs/SETUP_WIZARD.md`, no page changes — so there is no wizard offer banner
competing for the top of the page yet. The safety TC offset warning
(`safety_commissioning_page.html:1386+`) is in-card on its own page, not a top-of-page
banner, and does not compete with the recovery banner either. The recovery banner and the
connection banner are ordered deliberately (`app.js:454-461`, recovery inserted above
connection) and both are in-flow, verified by the sweep's `recovery_shown` fixture. When
the wizard banner does land, this ordering is the thing to revisit.

## 7. Check/test state at review time

`firmware/KilnFW/App/test/build_host_tests.ps1`: **31/31 executables built and passed**,
`6/6` checks.
`check_js_host_tests.ps1`: **14/14**.
`check_ui_responsive_sweep.ps1` in isolation: **111/111**.

`tools/run_all_checks.ps1` (second, quiescent run): **65 passed, 0 skipped, 2 failed.**
Both failures are in files this review was told not to edit, and both belong to the
filesystem/wizard work in flight:

* `check_flash_worker_lint.ps1` —
  `drivers/persist/setup_wizard_progress.c:223,225` call `hal_kv_set_blob()` /
  `hal_kv_commit()` directly, outside the flash-worker allowlist; and
  `drivers/persist/cfg_fs_status.c:196` calls `cfg_fs_format_is_stalled()` outside
  `CFG_FS_ALLOWLIST`. **Flagged to that owner as more than a lint nit:** a direct NVS
  write from a task that may be PSRAM-stacked is the documented panic-every-time failure
  that routing through the flash worker exists to prevent. Worth confirming which task
  calls the progress store before allowlisting it.
* `check_hal_include_boundary.ps1` — `cfg_fs_mount.c` and `setup_wizard_progress.c` both
  include `esp_timer.h` outside `hwAbstraction/` without an allowlist entry.

`check_c_files_in_cmakelists.ps1` failed on the first run
(`setup_wizard_progress.c` unreferenced) and passed on the second — fixed concurrently by
its owner mid-review.

---

## Verdict

The aggregate is in better shape than the conditions predicted. The specific failure this
review was commissioned to find — one agent's commit silently reverting another's in a
shared file — **did not happen**: every hunk from all thirteen commits is present, and
nothing is duplicated. The JS is coherent, the JSON contracts line up in both directions,
the theme tokens hold, and the responsive sweep failure was a genuine flake.

Two real user-facing defects came out of it, both handed back rather than fixed:
the duplicated minimum-on/off controls on an On/off device zone (6a), and the recovery
banner promising a restriction the HTTP layer does not enforce (6b).
