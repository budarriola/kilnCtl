# Whole-Kiln Setup Wizard

> **Status: COMPLETE.** All 13 wizard steps and all 11 implementation steps
> below have landed and are exercised by `test_setup_wizard.js` /
> `test_setup_wizard_progress.c` (`check_js_host_tests.ps1`). This file is
> kept as the as-built reference (design rationale, the reuse map, the
> safety rules) now that nothing in it is open -- the `_PLAN` suffix was
> dropped per this repo's standing convention (`feedback_completed_plans_lose_plan_suffix`)
> and every citing file was repointed to `docs/SETUP_WIZARD.md`.
>
> **2026-09-09 pass:** closed the one real gap found against the newly-landed
> firing interlock, `readiness_gate.c`/`.h` (owner decision 2026-09-09:
> `/api/readiness` now actually blocks a firing on `safety_trip`,
> `recovery_mode`, `crash_report`, `estop_verified`, no override). The
> wizard's `computeCompleteness()` was already correct by construction --it
> iterates every item in the raw `/api/readiness` response, not just the keys
> any of the 13 `WIZARD_STEPS` rows declares, so a whole-board item no step
> tracks (`crash_report`, `recovery_mode`, `safety_trip`, `estop_verified`,
> `safety_context`, `cfg_fs`) already blocked "Setup complete" the instant
> `readiness_http.c` reported it `not_done`, since both sides read the SAME
> `readiness_*_status()` predicates readiness_gate.h now also gates on -- no
> second, wizard-owned copy exists to drift from it. That absence of a bug
> was not previously proven by a test, though: no assertion exercised an
> unmapped whole-board item, and the bench board's live `/api/readiness` at
> the time (`crash_report`/`estop_verified` both `not_done`, from an
> unacknowledged panic during a heating run that same day) was exactly the
> shape a wizard-owned copy would have gotten wrong. Added
> `testGateBlocksOnUnmappedFiringGateItem()` to `test_setup_wizard.js`,
> reproducing that exact live shape (all 13 steps done, all step-tracked
> readiness keys `ok`, but `crash_report`/`estop_verified` `not_done`) and
> asserting the gate still refuses and names both. Negative-tested by hand:
> changed `computeCompleteness()`'s `items.forEach` condition to
> `if (false && (it.status === 'not_done' || ...))`, reran
> `test_setup_wizard.js`, got 6 RED (including the new test), reverted the
> line by hand, confirmed `git diff` on `setup_wizard_page.html` empty before
> committing. No production logic changed -- the gap was in test coverage,
> not behavior.
>
> Prior history: implementation steps 1-7 landed 2026-09-08 (progress store,
> `/api/setup/progress`, the `/setup` page shell, and steps 0-6's real
> content: preflight, network/time/units, zone count + thermocouple channel
> assignment, thermocouple types + calibration offsets, zone type (heater vs
> on/off), relay assignment + names + `max_simultaneous_relays`, and zone
> commissioning limits incl. the abs-max-vs-Pico-ceiling refusal); steps
> 8-11 (safety processor, CT setup, autotune) not started as of that pass;
> **this pass** landed steps 8-9 (CT install/topology/mapping review, and CT
> mapping verification under load), 10-11 (PID gains -- autotune-or-hand-
> entered, explicitly either counts as complete -- and the optional coupling
> matrix review), and step 12 (final gate screen, consuming the already-
> landed `computeCompleteness()` rather than reimplementing it). Step 7
> (safety processor commissioning) landed in a separate, concurrent pass
> (commit noted at that step's row below) -- **embedded** inline per owner
> decision, reusing the existing `/api/safety/commissioning` write path and
> its `confirm_commit_landed()` read-back via a newly-extracted shared file,
> `commissioning_shared.js`, rather than a second confirm-and-verify
> implementation. All 13 steps now have real content. **Opened:**
> 2026-09-08.
> Owner request, verbatim: *"like the safety commissioning wizard i want a
> wizard that guides me through the entire setup process of the kiln. ie pid,
> zones, current exc. including the safety processor."*
>
> **One-line goal:** one page, `/setup`, that walks a new owner from a blank
> board to a kiln that `/api/readiness` reports fully ready — reusing every
> existing endpoint, adding no second copy of any validation rule.
>
> **Owner decisions (2026-09-08), settled, build to these:**
> 1. **Embed** the safety-processor commissioning inline in the wizard
>    (embed over link-out, section 10 Q2) — but reuse the existing write path
>    and its `confirm_commit_landed()` read-back
>    (`safety_set_commissioning_fields` / the commissioning commit endpoint)
>    rather than a second implementation of confirm-and-verify. If a future
>    pass finds itself copying that logic, extract and share it instead.
> 2. **Offer, do not force** (section 10 Q6): an unconfigured board gets a
>    prompt/banner pointing at `/setup`; no forced redirect.
> 3. **Hand-entered PID gains complete step 10** — autotune is not mandatory
>    (section 10 Q4). The step's own UI must say so explicitly, so the choice
>    reads as deliberate rather than as a shortcut nobody noticed.

---

## 1. The model: how the safety commissioning wizard works

`firmware/KilnFW/App/drivers/http/safety_commissioning_page.html` (served by
`safety_cfg_http.c`, route `/safety/commissioning`). Extract this pattern; do
not invent a second idiom.

| Aspect | What it does | Where |
|---|---|---|
| Structure | `#guided` div holds `.gscreen0..4`, one visible at a time via `gGoto(n)`; a `.gstepper` renders `GSTEP_NAMES` with `.current`/`.done` classes | page JS `gGoto`, `gRenderStepper` |
| Screen 0 | "Before you start": preflight status line (`link_up`, `commissioned`), write-window banner, `gStart()` which **refuses to advance** if the initial load failed (`gLoadFailed`) | `gRenderPreflight`, `gRenderLoadFailure` |
| Answers | Choices are `.radio-card`s, each with a `.consequence` span spelling out *which guards this answer turns off*. Unavailable choices get `.radio-card.disabled` with the reason in `--bad` | screens 1–2 |
| Validation | Per-screen `gValidateScreenN()` gates `Next`; cross-field contradictions (`checkTcMaxContradiction()`) are re-checked **again at commit**, because the review screen's advisory text was once the only check and could be walked past (fix "F3") | `gCommit()` |
| Review | Screen 3 renders every answer as `.review-row` name/value before anything is written | `gRenderReview` |
| Write | Screen 4 stages all params and issues **one** `POST /api/safety/commissioning` with `commit=1` | `gCommit()` |
| Success is never assumed | A rejected commit is **HTTP 200** with `{"ok":false,"reason":...}`. `res.ok` alone is never trusted; the parsed body's own `ok` is. Server-side `confirm_commit_landed()` re-reads from the Pico; the page then does its **own** second read-back of the fields it changed and fails loudly, naming the field, on mismatch | `gCommit`, `findCriticalChanges` |
| Cannot-complete | ARMED refusal is detected by regex on the reason, then `gStartWriteWindowRetry()` retries every 3 s × 15 with an honest banner — it never renders a countdown of a window it cannot observe (fix "F2") | `gStartWriteWindowRetry` |
| Destructive confirm | `tc_type`/`tc_offset_c` changes require an explicit **named** confirmation before reaching the wire, and refuse mid-run | `findCriticalChanges`, `doSubmit` |
| Busy refusal | `checkFiringOrAutotuneRunning()` polls `/api/profile_exec` + `/api/autotune` and refuses; a *failed* fetch never blocks (Pico's ARMED refusal is the backstop) | same |
| Reload / interruption | **Progress is NOT persisted.** All state is browser-local (`gQ2Choice`, `gPrefilled`); a reload restarts at screen 0 and re-prefills from the board. Acceptable there (4 questions), **not acceptable for a multi-day setup** — see §5 |
| Degrade gracefully | Every auxiliary fetch (`/api/zones`, `/api/zones/ct_channel_map`) `.catch()`es into a placeholder rather than blocking |
| Styling | No new colours: `--ui-*` tokens, `.field`, `.gscreen`, `.radio-card`, `.writewindow ww-{unknown,armed,waiting,ok}`, `.unset`, `.guard-off-banner`. 32 px touch floor on every `<summary>`/checkbox label |

**Rules inherited verbatim by the new wizard:** unset renders as *unset*, never
as a plausible zero; every option states its safety consequence; a write is
only "saved" after an independent read-back; rejection text is rendered
verbatim from the firmware, never paraphrased.

---

## 2. What a full setup actually requires

`/api/readiness` (`readiness_http.c`) **already** enumerates most of this, with
a four-way status (`ok`/`not_done`/`cannot_yet`/`deliberately_off`), a detail
string and a `fix_url` per item: `network`, `thermo_count`, `relays_assigned`,
`control_mode`, `guard_max_temp`, `guard_cross_zone`, `calibration`,
`profile_saved`, `autotune`, `hardware`, `safety_commissioned`, `storage`.
**The wizard is a guided front end onto that checklist, not a new model.**

Fields, by owner:

**ESP `zones_cfg_t` (`persist/zones_config_json.h`, `ZONES_CFG_VERSION 23`), via `GET/POST /api/zones`:**
- global: `thermo_count`, `relay_count`, `max_simultaneous_relays` (0 = unlimited),
  `continue_on_zone_trip`, `safety_tc_type`, `timing_profile_count` + `timing_profiles[]`
- per zone: `name`, `thermo_mask`, `tc_type`, `cal_offset_c`, `relay_mask`,
  `relay_type` (ssr/contactor/mercury), `ct_mask`, `control_mode`,
  `zone_type` (HEATER vs ON_OFF_DEVICE) + `failsafe_state`/`hyst_c`/`min_on_s`/`min_off_s`,
  `max_temp_c` (**0 = not set**), `min_temp_c`, `max_ramp_c_per_hr` (**0 = never
  configured → refuse to start**), `sanity_rate_c_per_min`, `cross_zone_max_delta_c`,
  the eight `guard_*` thresholds, `pid_kp/ki/kd` + `model_k_dc/tau_s/dead_time_s`,
  `coupling_coeff[]/coupling_tau_s[]/coupling_dead_time_s[]`, `fuzzy_strength_pct`,
  `approach_rate_cap_c_per_hr`, `ease_off_window_mult`, `error_band_c`,
  `progress_band_c`, `adaptive_tune_enabled`
- relay names: `zones_config_get/set_relay_name()` (separate `relay_names_cfg_t` blob)

**Pico `config_store`, via `GET/POST /api/safety/commissioning` (+ `/ct_cal`,
`/ct_auto_zero`, `/relay_type`, and PcTools `safety_set_commissioning_fields`):**
- `tc_source`, `borrowed_zone_index`, `tc_placement_mode`, `tc_type`, `tc_offset_c`
- `abs_max_temp_c` (**must never be tighter than the ESP's** — see §6)
- `max_rate_c_per_min` (S8; 0 = guard off), `mains_voltage_v`, `max_expected_power_w`
- `ct_installed` (param `0x0109`; **no** ⇒ S3/S4/S9/S14 report *off*, `5cd56b6`),
  `ct_topology` (per_zone | summed), `ct_channel_map[0..2]`, `ct_cal[]` (gain + idle offset)
  — bench today: one summed CT on RP2040 GPIO28, 1 A : 1 V, ~+59 mV idle offset (`b8f0f47`, `c49bb0e9`)
- safety relay (K4) type — ESP-local, own endpoint

**Board-level prefs:** Wi-Fi (`/provision`, `/networks`, `/scan`, `/ip_config`),
time zone (`/api/settings/tz`), units (`/api/unit_pref`), display power
(`/api/settings/display_power`), named config presets (`/api/kiln_configs`).

---

## 3. Ordered steps and their dependencies

`◆ = needs owner physically present · ▲ = kiln must be idle · 🔥 = applies heat`

| # | Step | Needs from earlier | Writes via | Notes |
|---|---|---|---|---|
| 0 | Welcome / preflight | — | read-only | `/api/readiness`, `/api/status`, `/api/crash_report`. Refuse to start on an unacknowledged crash |
| 1 | Network + time + units | — | `/provision`, `/api/settings/tz`, `/api/unit_pref` | Skippable if already on the LAN. Time matters: firing logs |
| 2 | Zone count + thermocouple channels | 0 | `POST /api/zones` (`thermo_count`, per-zone `thermo_mask`) | Live per-channel readings shown so mis-wiring is visible immediately (`/api/status`, `/api/thermo/faults`) |
| 3 | Thermocouple types + calibration offsets | 2 | `POST /api/zones` (`tc_type`, `cal_offset_c`) | `tc_type` sets the sensor ceiling that bounds step 7's `abs_max_temp_c`. Offsets may be deferred (`deliberately_off`) |
| 4 | Zone type: HEATER vs ON_OFF_DEVICE ◆ | 2 | `POST /api/zones` (`zone_type`, `failsafe_state`, `hyst_c`, `min_on_s`, `min_off_s`) | ON_OFF disables guards 1/2/3/4/9 and zeroes its coupling row/column (`d58492c9`, `172e3081`). Radio-card must say exactly that. Fail-safe default OFF, confirm-gated opt-in to ON |
| 5 | Relays: count, per-zone mask, names, type, `max_simultaneous_relays` ◆ | 2, 4 | `POST /api/zones`, `zones_config_set_relay_name()` | Uses `/api/diagnostics/danger/relay` to click one relay at a time so the owner can *hear* which is which — dry contacts, heat disabled |
| 6 | Zone commissioning limits ▲ | 2, 4 | `POST /api/zones` (`max_temp_c`, `min_temp_c`, `max_ramp_c_per_hr`, `sanity_rate_c_per_min`, `cross_zone_max_delta_c`) | **0 = uncommissioned, refuse to start.** Must render as NOT SET, never 0 |
| 7 | Safety processor commissioning ▲◆ | 3, 6 | existing `/api/safety/commissioning` guided flow — **embed/link, do not reimplement** | Pico `abs_max_temp_c` ≥ ESP `max(max_temp_c)`; requires the ~60 s post-boot GRACE window and a power-cycle |
| 8 | Current sensing: `ct_installed`, `ct_topology`, `ct_mask`, calibration ▲ | 5, 7 | `/api/safety/commissioning`, `/ct_cal`, `/ct_auto_zero` | Idle offset auto-zero requires **all relays off**. Answering "not installed" is a legitimate finish (S3/S4/S9/S14 report off) |
| 9 | CT mapping verification 🔥◆ | 8 | `/api/zones/current_sweep/start` + `/status` | Energises one relay group at a time and watches the CT. Needs load. Explicit heat warning + abort button |
| 10 | PID gains — autotune or by hand 🔥◆ | 2,3,5,6,7 | `/api/autotune/start`, `/matrix`, `/accept`; `POST /api/zones/pid` | **Multi-hour, one zone at a time.** Needs a *rested* kiln (all zones at ambient, not just the one under test). Refusals to surface verbatim from `autotune_engine.c`: no relay mask, no thermo mask, on/off zone, zone active in a firing, sweep active, OTA in progress, relay authority blocked, invalid zones cfg |
| 11 | Coupling matrix | 10 | `/api/autotune/matrix` | Optional; ON_OFF zones excluded. Can be deferred |
| 12 | First profile saved + final readiness gate | all | read-only + `/api/readiness` | Refuses "complete" while any item is `not_done` (§5) |

Dependency rules the implementer must enforce (each is already enforced
somewhere in firmware — the wizard only *explains* it earlier):
- No PID before zones + thermocouples exist (autotune refuses `thermo_mask == 0`).
- No CT verification before relays are assigned (a sweep with no relay mask is meaningless).
- No Pico `abs_max_temp_c` above the sensor ceiling implied by `tc_type`.
- No heat at all while `safety_commissioned` is false.

---

## 4. Steps that need heat, or the owner

- **Heat:** 9 (CT verification under load), 10 (autotune). Both must show a
  full-width warning screen naming the expected duration and the temperature
  the kiln will reach, with an explicit "I am present and the kiln is safe to
  heat" confirmation, before anything energises.
- **Owner present:** 4 (what is this zone actually driving?), 5 (which relay
  clicks?), 7 (power-cycle the Pico for the GRACE window), 8/9 (CT probe
  fitted), 10 (heat).
- **Idle required:** 6, 7, 8. Steps 1–5 are safe with the kiln cold and
  de-energised; steps 0–3 are read-mostly.

---

## 5. Interruption, and where progress lives

Setup spans days. Requirements:

1. **Progress persists in NVS on the ESP**, not in `localStorage` and not in
   the `cfg` LittleFS partition. Reason: user config is mid-migration to `cfg`
   with NVS dual-write (`docs/CONFIG_FILESYSTEM.md`); a filesystem problem
   during setup must not lose the record of what has already been done, and
   the wizard's record is exactly what you want intact while diagnosing that
   problem. One small blob, namespace/key ≤ 15 chars — `NVS_KEY_LEN_CHECK()`
   is mandatory (see `project_nvs_key_too_long_zone_normals`).
2. **Blob content is deliberately thin:** `{version, per-step {state:
   pending|done|skipped, ts, note}}`. It stores *no configuration values* —
   every value's source of truth stays its existing store. So the wizard can
   never disagree with the board about a value; at worst it disagrees about
   whether a step was *visited*, which is why step status is always displayed
   **beside** the live `/api/readiness` verdict for the same item.
3. **`/api/readiness` is authoritative over the blob.** If the blob says done
   and readiness says `not_done`, the step renders as **regressed** with the
   readiness detail — never as done.
4. **Every step is independently re-runnable and directly linkable**
   (`/setup#step=6`). No step may depend on in-memory state from a previous
   screen; each re-derives from the board on entry.
5. **Overview screen** listing all 13 steps with done/pending/skipped/regressed
   and a "resume where you left off" button. Skipping is allowed and recorded
   with a reason; skipping a safety-relevant step blocks completion (§6).
6. Writes stay **per-step**, committed at the end of that step. There is no
   global "commit everything at the end" — a wizard abandoned at step 6 must
   leave steps 1–5 genuinely applied, not staged.

---

## 6. Safety rules

- **Every Pico flash write** goes through the existing
  `POST /api/safety/commissioning` commit path: named confirmation, verbatim
  rejection text, server-side `confirm_commit_landed()` read-back **and** the
  page's own second read-back. Never add a second write path.
- **The Pico ceiling is a second set of eyes and must never be tighter than
  the ESP's.** Validate `abs_max_temp_c ≥ max(zone max_temp_c)` on entry to
  step 7 *and* re-check at commit; refuse, do not clamp. Do not propose 70 °C.
- **Never half-commissioned-but-complete.** "Setup complete" is granted only
  when `/api/readiness` reports no item in `not_done` **and** no safety-relevant
  step is `skipped`. Safety-relevant = 6, 7, 8 (if `ct_installed`), 9 (if
  `ct_installed`). `deliberately_off` (e.g. `ct_installed=no`) is a legitimate
  complete state and must be shown as such, with which guards are consequently
  off named on the completion screen.
- **`capability_preflight`** (`tools/PcTools/src/kilnctrl/capability_preflight.py`)
  already refuses to start a run on a board with an unacknowledged crash. Step 0
  mirrors that refusal in the browser (`/api/crash_report`), and the wizard adds
  **no** new bypass. If PcTools would refuse, the wizard refuses.
- Heat-applying steps re-check `/api/profile_exec` + `/api/autotune` the way
  `checkFiringOrAutotuneRunning()` does, and never block on a *failed* fetch.
- Stopping the browser does not stop a firing: the autotune step must show a
  live abort control and say plainly that closing the tab does not abort
  (`project_stopping_host_does_not_stop_firing`).

---

## 7. Reuse map — wire, do not rewrite

| Step | Already exists |
|---|---|
| all | `GET /api/readiness` (items, status, detail, `fix_url`), `readiness_page.html` rendering |
| 0 | `/api/status`, `/api/crash_report`, `/api/partitions`, `capability_preflight.py` |
| 1 | `wifi_provision_http.c` (`/provision`, `/networks`, `/scan`, `/ip_config`, `/forget`), `/api/settings/tz`, `/api/unit_pref` |
| 2,3,4,5,6 | `zones_http.c` `GET/POST /api/zones`, `zones_config_json.c` validation, `zones_page.html` field parsers, `zones_config_get/set_relay_name()`, `/api/diagnostics/danger/relay` |
| 7 | the whole guided flow in `safety_commissioning_page.html`; `safety_cfg_http.c` commit + `confirm_commit_landed()`; PcTools `safety_set_commissioning_fields()` |
| 8 | `/api/safety/commissioning/ct_cal`, `/ct_auto_zero`, `/api/zones/ct_channel_map` |
| 9 | `/api/zones/current_sweep/{start,status,abort}` (`zones_current_sweep_task.c`) |
| 10,11 | `/api/autotune{,/start,/abort,/accept,/matrix}`, `/api/zones/pid`, `/api/tuning_recommendations`, `autotune_engine.c`'s refusal strings |
| 12 | `/api/profiles`, `/api/kiln_configs` (save the finished setup as a named preset) |

**Do not duplicate:** any bound, any 0-means-unset rule, any rejection text.
The wizard renders what the firmware says.

---

## 8. Implementation steps (ordered, individually shippable, riskiest last)

Each is independently mergeable. R = reversible without a flash. F = needs a flash.

| # | Step | Test strategy | Risk |
|---|---|---|---|
| 1 | **DONE (`90bd8b2c`).** **Progress store.** `setup_wizard_progress.{c,h}` in `persist/`: versioned NVS blob (namespace `setup_wiz`, key `progress_v1`, both `NVS_KEY_LEN_CHECK`'d), `{version, per-step {state, ts, note}}` only — no config value. v1 `{state, ts}` migrates to v2's tail-appended `note`, `_Static_assert`'d layout. `setup_wizard_progress_effective_state()` is the readiness-authoritative REGRESSED-vs-DONE precedence rule, pure and host-tested standalone. `flash_worker_lint.py`'s ALLOWLIST covers the one `hal_kv_set_blob()`/`hal_kv_commit()` call site (Pattern 3, internal-SRAM-stack httpd task — same shape as `display_power_cfg.c`/`unit_pref.c`) | `test_setup_wizard_progress.c`: round-trip, v1 migration, unknown-step rejection, state-value rejection, NVS-unavailable degrade, wire-name round trip. Negative-tested: disabled the REGRESSED precedence check, watched `test_setup_wizard_progress.c:232` fail, restored by hand, confirmed `git diff` clean | F, low |
| 2 | **DONE (`90bd8b2c`).** **`GET/POST /api/setup/progress`** (`setup_progress_http.{c,h}`), registered in `main_network_http.c` right after `setup_wizard_http_start()` (step 3). Body: `{"version":1,"steps":{"<index>":{"state","ts","note"}}}` — object keyed by step index, matching the already-landed page shell's `defaultProgress()`/`mergeAllSteps()` contract. `SETUP_PROGRESS_JSON_CAP=2048` against a ~1300-byte worst case (13 steps). POST is one step per call (plan section 5 point 6 — no global commit-everything). Does NOT itself fetch/merge a live readiness snapshot — that merge is step 3's page-side JS (`computeStepState`), so this endpoint never duplicates any readiness check | Host tests cover the shared wire-format helpers (`setup_wizard_step_state_name/_from_name`) in `test_setup_wizard_progress.c`; `curl` against a board still to do | F, low |
| 3 | **DONE (`dd9deb74`).** **`/setup` page shell**: overview screen, 13 rows, status merged from `/api/setup/progress` + `/api/readiness`, resume button, deep-link `#step=N`. No step content yet. `firmware/KilnFW/App/drivers/http/setup_wizard_page.html` + `setup_wizard_http.{c,h}`, reusing the safety-commissioning page's `.gstepper`/`.gscreen`/`.review-row` CSS verbatim; added regressed-state stepper/pill classes and the completeness gate. Offer banner in `app.js` (`pollSetupOffer`/`buildSetupBanner`), theme.css's `.kc-setup-banner` (accent-1, no new colour). JS host test `test_setup_wizard.js` (20 assertions incl. readiness-overrides-stored, resume-after-reload, negative-tested completeness gate). No new endpoint added at this step -- zero json_cap cost; the page consumes the already-landed `GET/POST /api/setup/progress` (step 2) and `GET /api/readiness` verbatim. | `check_ui_responsive_sweep.ps1` at every viewport; JS host tests (`check_js_host_tests.ps1`) for the merge function incl. the regressed case | F, R at runtime |
| 4 | **DONE (`dd9deb74`).** **Steps 0-1** (preflight, network/time/units). Step 0 reuses `GET /api/status` + `GET /api/crash_report` read-only, mirrors `capability_preflight`'s unacknowledged-crash refusal in the browser, re-checks crash_report again at commit (not just on load) before marking done. Step 1 reuses `POST /api/settings/tz` and `POST /api/unit_pref` verbatim, links out to the existing `/wifi` page for network changes rather than re-embedding `wifi_provision_http.c`'s scan/connect flow (same "link, don't duplicate" choice the plan makes for step 7); a client-side `validateStep1()` mirrors `time_sync_tz_is_valid()`'s POSIX-vs-IANA distinction only to grey out the button early -- the server's own rejection text is still rendered verbatim on a 400, never paraphrased | `test_setup_wizard.js`: `validateStep1()` (real TZ, empty, IANA name, missing unit); `check_ui_responsive_sweep.ps1` at every viewport | F |
| 5 | **DONE (`dd9deb74`).** **Steps 2-3** (zone count + thermocouple channel assignment; thermocouple types + calibration offsets). Reuses `GET/POST /api/zones` verbatim (same `thermo_count`/`z<i>_thermo_mask`/`z<i>_tctype`/`z<i>_cal` fields `zones_page.html` already posts) and `GET /api/status`'s per-zone `actual_c`/`actual_valid` for live readings, same pattern as `zones_page.html`'s `pollCtCurrents()`. `TC_TYPES` is the same 8-entry array (codes 0-7, `uart_task_ids.h`'s `THERMO_TC_*`) copied verbatim from `zones_page.html` rather than invented. Step 3's write is followed by a poll of `/api/status` before the step is marked done -- a write returning 200 is reported separately from "the channel is now producing a valid (non-NaN) reading", naming the CR1 verify-on-chip failure mode explicitly rather than collapsing both into one "success" (main-board MAX31856 path has no transmitted `tc_type_verified` bit the way the Pico's safety TC does -- see `diagnostics_http.c`'s own documented gap -- so this is the best available proxy, and says so). `isZoneCommissioned()`/`formatZoneLimitC()` express the `max_temp_c`/`max_ramp_c_per_hr` 0-means-unset rule (`1fc9b1dd`) as a read-only badge on each zone card now, ready for step 6's own editor to reuse unchanged | `test_setup_wizard.js`: `validateStep2()` (unassigned channel, out-of-range count), `validateStep3()` (bad type code, out-of-bound/NaN offset), and `formatZoneLimitC`/`isZoneCommissioned` (0 renders NOT SET, never a valid limit) -- negative-tested by flipping `isZoneCommissioned` to `!!zone`, watching two assertions fail, restoring by hand, `git diff` confirmed clean; `check_ui_responsive_sweep.ps1` at every viewport | F |
| 6 | **DONE (this pass).** **Steps 4-5** (zone type, relays, names, `max_simultaneous_relays`). Step 4's consequence text is copied byte-for-byte from `zones_page.html`'s `.onoffWarn` paragraph (`ZONE_TYPE_CONSEQUENCE_TEXT`, `172e3081`) rather than reworded. Step 5 surfaces the on/off-vs-cap interaction from `bf1db47f` (on/off zones count toward `max_simultaneous_relays` and are suppressed LAST) as a live count against the configured cap, refused at commit if exceeded. Both steps (and step 7 below) post through a new shared `zoneToPostParams()`/`submitZonesConfig()` that echoes every GET `/api/zones` field `zones_http_post_parse.c` requires or would otherwise silently reset/delete (relay_mask, cal/kp/ki/kd, ramp/sanity/mode/maxtemp/mintemp/window/minon/minoff, the measured plant model k/tau/deadtime, guard thresholds, timing profiles) rather than posting only the fields each step edits -- steps 2/3's own hand-rolled save bodies above post a strict subset of the server's required fields and would very likely 400 against a real board outside a test harness; flagged here rather than fixed, out of this pass's scope | Host tests (`test_setup_wizard.js`): `validateStep4` (on/off hyst/min-on/min-off bounds, consequence text content), `validateStep5` (relay count bound, the on/off-count-vs-cap refusal citing `bf1db47f`, cap 0 = unlimited never refuses); `check_ui_responsive_sweep.ps1` at every viewport. Not yet exercised on the bench (relay-click audio confirmation, `ON_OFF_ZONE_PLAN.md` §1 cross-check) | F, ◆ |
| 7 | **DONE (this pass).** **Step 6** (zone commissioning limits, 0-as-unset rendering, abs-max-vs-Pico-ceiling refusal). Reuses `isZoneCommissioned()`/`formatZoneLimitC()` (step 5's own functions) unchanged rather than reimplementing the 0-means-unset rule; `getAbsMaxTempC()` reads the Pico's `abs_max_temp_c` straight off `GET /api/safety/commissioning`'s `params[]` (id 260) -- never a second, wizard-owned copy -- and returns `null` (never 0) when unset/unknown, so the ceiling half of the check is skipped rather than misread as "no ceiling". Validated on load AND re-validated at commit against a freshly-refetched ceiling (step 7's own commissioning could change it in another tab) | Host tests: `validateStep6` (0-as-unset for both `max_temp_c` and heater-only `max_ramp_c_per_hr`, on/off zones exempt from the ramp requirement, the abs-max refusal at/above/below the ceiling), `getAbsMaxTempC` (set/unset/absent/no-commissioning-yet all handled). Negative-tested by hand: disabled the abs-max comparison (`if (false && ...)`), reran `test_setup_wizard.js`, got 2 RED (`step6: a zone max_temp_c above the Pico abs_max_temp_c ceiling is refused`), restored by hand, `git diff` confirmed clean before committing | F |
| 8 | **DONE (this pass).** **Step 7** (safety processor) — **embedded** inline per owner decision (not linked out), covering tc_type, tc_offset_c (fd02df05), abs_max_temp_c, ct_installed, ct_topology. Reuses `POST /api/safety/commissioning` verbatim; every write is treated as critical (named confirm + independent read-back). The confirm-and-read-back logic (busy check, critical-change diff, POST + confirm_commit_landed() read-back + this page's own second read-back) was extracted into `firmware/KilnFW/App/drivers/http/commissioning_shared.js` (new static asset, wired into CMakeLists.txt's gzip-embed list and wifi_provision_http.c's route table) so this step does not duplicate safety_commissioning_page.html's existing implementation of the same contract; that existing page was left as-is (not retrofitted to consume the shared file, to avoid regression risk on a previously-shipped/tested page -- flagged as a follow-up, not done here). Abs-max relationship enforced both ways: step 6 already refused a zone max_temp_c above the Pico ceiling; step 7 now refuses (not clamps) an abs_max_temp_c below any zone's max_temp_c, re-checked at commit against a fresh zones fetch. CR1-verify gap worded explicitly (fd02df05's wording): a successful write confirms the config record, not that the MAX31856 chip's CR1 register accepted the type. The persistent non-zero tc_offset_c banner already lives on `/safety/commissioning` (fd02df05) and is echoed read-only on this step too. | Verify the wizard marks it done only from `/api/readiness`'s `safety_commissioned`, never from a local flag -- confirmed: `postStepState(7,'done')` is a progress-blob note only, `computeStepState()` still checks the real `safety_commissioned` readiness item and renders REGRESSED if it ever disagrees | F, ◆, **irreversible Pico write** |
| 9 | **DONE (this pass).** **Steps 8–9** (CT setup, then sweep verification under load). Step 8 does NOT reimplement the Pico write path -- it reads `ct_installed`/`ct_topology` off `GET /api/safety/commissioning`'s `params[]` (same `getCommissioningParam()` pattern step 6's `getAbsMaxTempC()` already used) and `GET /api/zones/ct_channel_map` for the per-channel assignment, links out to `/safety/commissioning` for any edit, and spells out the dashboard-grouping consequence (`9d515708`) per channel: single-zone assignment renders in that zone's card, a summed/shared CT renders in the shared section, and an unmapped channel is simply hidden -- so a forgotten mapping never even shows up as an error. `validateStep8()` is the one new client-side rule: under per-zone topology, two CT channels mapped to the same zone is flagged (the dashboard can only show one CT per single-zone card). Step 9 is explicitly heat-marked: a full-width warning banner states heat + owner presence + "closing this tab does not stop it" before anything is enabled, an "I am present" checkbox gates the Start button, `step9CheckBusy()` mirrors `checkFiringOrAutotuneRunning()` (polls `/api/profile_exec` + `/api/autotune`, never blocks on a failed fetch), and the existing `/api/zones/current_sweep/{start,status,abort}` triad is reused verbatim with the sweep's own refusal string (`zone_sweep_refusal_str()`) rendered on failure. `ct_installed=0` is treated as a legitimate skip, not a dead end | Host tests (`test_setup_wizard.js`): `validateStep8` (not-installed short-circuits, missing topology refused, per-zone clash refused and named, summed topology tolerant), page-source assertions that the heat warning/ack checkbox/skip control/"does not stop it" text are all actually present, `step9CheckBusy()` (a running firing refuses; a failed fetch never blocks). `check_ui_responsive_sweep.ps1` at every viewport (117/117 passed) | F, ◆, 🔥 |
| 10 | **DONE (this pass).** **Steps 10–11** (PID gains, coupling). Step 10 states explicitly, in the UI text itself, that hand-entered gains complete the step exactly as fully as autotune (`/api/readiness`'s own `autotune` item already accepts `kp > 0` typed by hand OR an identified model per zone -- this screen checks the identical thing, not a second rule) -- both paths are offered side by side, never framed as a fallback. The screens this pass could check client-side without duplicating the engine (`zone.thermo_mask == 0`, `zone_type == 1` on/off) surface `autotune_engine.c`'s own refusal strings verbatim (`autotunePrecheck()`) before the "Run autotune" link (out to the existing `/settings/zones` flow -- its own multi-hour warning and every OTHER real-time refusal, e.g. busy/OTA/sweep-active, are not re-implemented here) is even offered; hand-entered gains post through the existing narrow `POST /api/zones/pid` and re-read `/api/zones` to confirm the write landed, same "confirm, don't assume" pattern `main_page.html`'s PID popup already uses. Step 11 (coupling matrix) reuses `GET /api/autotune/matrix` verbatim, excludes ON_OFF zones from the review, and is explicitly optional/skippable | Host tests: `autotunePrecheck()` (no-thermocouple / on-off-zone / no-relay-mask refusal text matches `autotune_engine.c` verbatim; a normal zone has no refusal), page-source assertion that the hand-entered-counts-as-complete framing is actually in the UI text. `check_ui_responsive_sweep.ps1` at every viewport | F, ◆, 🔥, highest |
| 11 | **DONE (this pass).** **Step 12 + completion gate** — the refusal to declare complete. NOT reimplemented: this screen calls the already-landed, already-tested `computeCompleteness()` (step 3's own shell) and renders its verdict plus a link to `/profiles`; there is still no separate "mark setup complete" write anywhere -- readiness, read fresh, stays the only authority (plan section 5). Negative-tested THIS pass (not just re-verified): disabled the `not_done`/`cannot_yet` branch in `computeCompleteness()` (`if (false && (it.status === 'not_done' ...`), reran `test_setup_wizard.js`, got 4 RED including `FAIL: gate refuses complete with one outstanding not_done item`, restored the line by hand, confirmed `git diff` on the page clean before committing | Host test: a `not_done` readiness item blocks `complete` and is named in `reasons` (negative-tested as above) | F |

**UI rules for every step above:** no new colours (reuse `--ui-*`, `--ok`,
`--warn`, `--bad`, `--fault-color`); `check_ui_responsive_sweep.ps1` must pass
at every viewport with `<details>` sprung open; 32 px minimum touch targets;
LCD pages must not scroll (this wizard is **web-only** — see §9); do not
enlarge httpd stack buffers; report every new endpoint's JSON cost against its
`json_cap` headroom.

---

## 9. Risks I would not take

- **Do not build an LCD version of this wizard.** 320×480 with no scrolling
  (owner standing rule) cannot carry 13 steps of consequence text. The LCD
  keeps its existing per-page config screens; `/setup` is web-only, and the
  LCD may at most show "setup incomplete".
- **Do not add a wizard-owned copy of any config value.** The moment the blob
  stores a value, it can disagree with the board's own store — this repo's
  "reset one side of a pair" class, four confirmed instances.
- **Do not add a second Pico write path** or a "write everything at the end"
  batch. One commit path, already read-back-verified.
- **Do not auto-run autotune** as part of a "finish setup" button. Multi-hour,
  heat-applying, needs a rested kiln.
- **Do not gate heating on wizard completion** in this plan's scope. The
  existing `commissioned` gate is the safety interlock; adding a second,
  UI-derived one risks bricking the bench out of heating (M12's ordering
  lesson, `ddbd024`/`3149393`).
- **Do not enlarge httpd buffers** to fit a bigger readiness/progress payload
  (`project_httpd_stack_near_overflow`: 64 B free under real load).

## 10. Questions for the owner — resolved

1. Web-only, confirmed (§9 — no LCD wizard; shipped that way).
2. **Embed** (owner decision, §0 above) — shipped as step 7, reusing
   `POST /api/safety/commissioning` verbatim via `commissioning_shared.js`.
3. **Not implemented.** No named `kiln_config` preset is auto-saved on
   completion; step 12 links to `/profiles` and `/settings` instead. Left
   out of scope rather than decided "no" — a reasonable follow-up if a real
   re-commission workflow needs a known-good baseline, but nothing in this
   pass's requirements called for it.
4. **Hand-entered gains count as complete** (owner decision, §0 above) —
   shipped in step 10, stated explicitly in the step's own UI text.
5. **Not implemented; stays separate.** The wizard does not surface a
   factory-reset entry point; `/api/factory_reset` remains its own,
   unrelated flow.
6. **Offer, do not force** (owner decision, §0 above) — the unconfigured-board
   banner (`app.js`'s `pollSetupOffer`/`buildSetupBanner`) points at `/setup`
   with no forced redirect.
