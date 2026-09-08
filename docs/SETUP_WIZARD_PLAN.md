# Whole-Kiln Setup Wizard — plan

> **Status:** design only, nothing implemented. **Opened:** 2026-09-08.
> Owner request, verbatim: *"like the safety commissioning wizard i want a
> wizard that guides me through the entire setup process of the kiln. ie pid,
> zones, current exc. including the safety processor."*
>
> **One-line goal:** one page, `/setup`, that walks a new owner from a blank
> board to a kiln that `/api/readiness` reports fully ready — reusing every
> existing endpoint, adding no second copy of any validation rule.

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
| 1 | **Progress store.** `setup_wizard_progress.{c,h}` in `persist/`: versioned NVS blob, `NVS_KEY_LEN_CHECK`, get/set/clear per step. No HTTP yet | New host test: round-trip, version migration, unknown-step rejection, key-length static assert. Negative-test it by breaking the production encoder by hand, then restoring by hand | F, low |
| 2 | **`GET/POST /api/setup/progress`** + register the store. Report JSON size against `json_cap` headroom in the PR; **do not enlarge httpd stack buffers** — build the response in the existing pattern | Host test for the serializer; `curl` against a board | F, low |
| 3 | **`/setup` page shell**: overview screen, 13 rows, status merged from `/api/setup/progress` + `/api/readiness`, resume button, deep-link `#step=N`. No step content yet | `check_ui_responsive_sweep.ps1` at every viewport; JS host tests (`check_js_host_tests.ps1`) for the merge function incl. the regressed case | F, R at runtime |
| 4 | **Steps 0–1** (preflight, network/time/units) | Sweep + manual; preflight refusal exercised with a synthetic crash-report response | F |
| 5 | **Steps 2–3** (zones, thermocouples, types, offsets) | Reuse `zones_page.html` parsers; host test the client-side validation mirror if one is added — better, add none and let the POST reject | F |
| 6 | **Steps 4–5** (zone type, relays, names, `max_simultaneous_relays`) | Relay-click step exercised on the bench with heat disabled; ON_OFF consequence text checked against `ON_OFF_ZONE_PLAN.md` §1 | F, ◆ |
| 7 | **Step 6** (zone commissioning limits, 0-as-unset rendering) | Host test: a 0 renders NOT SET and blocks completion | F |
| 8 | **Step 7** (safety processor) — link out to the existing page first, embed only if the link proves awkward | Verify the wizard marks it done only from `/api/readiness`'s `safety_commissioned`, never from a local flag | F, ◆, **irreversible Pico write** |
| 9 | **Steps 8–9** (CT setup, then sweep verification under load) | Bench, owner present, after CT_COMMISSIONING_PLAN step 6 | F, 🔥 |
| 10 | **Steps 10–11** (autotune, coupling) — longest-running, most refusal paths | Every refusal string from `autotune_engine.c` rendered verbatim; abort control exercised | F, 🔥, highest |
| 11 | **Step 12 + completion gate** — the refusal to declare complete | Host test the gate: `not_done` blocks; a skipped safety step blocks; `deliberately_off` does not | F |

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

## 10. Questions for the owner before implementation

1. Web-only confirmed? (§9 — no LCD wizard.)
2. Should step 7 **embed** the safety commissioning flow inside `/setup`, or
   link out to the existing page and return? Linking out is far less code and
   zero duplication; embedding feels more like one wizard.
3. Should completing setup **save a named `kiln_config` preset** automatically,
   so a later re-commission has a known-good baseline to compare against?
4. Is "skip" allowed on step 10 (PID) with hand-entered gains, and should that
   count as complete or as `deliberately_off`?
5. Should the wizard offer a **factory-reset / start-over** entry point, or
   stay separate from `/api/factory_reset`?
6. Does the first-boot experience need to *force* `/setup` (redirect from `/`
   while readiness is incomplete), or only offer it?
