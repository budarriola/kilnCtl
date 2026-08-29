# Commissioning UX — deriving the safety configuration instead of asking for it

> **Status:** implemented · **Written:** 2026-08-27, guided flow shipped
> 2026-08-28 (`64d0a8e`), `ct_channel_map`/thermocouple-max derivation shipped
> 2026-08-28 (`069f05e`), `k_ct_v_per_a` calibration shipped 2026-08-28 · **Owner request:** *"look at all
> of the settings in the safety commissioning page, and think if there is a user
> friendly way to derive these settings. this page is far too complex."*
>
> This started as a specification with no code behind it (2026-08-27). It is
> now the reference this repo keeps in sync with what's actually shipped —
> the guided flow, the derivations, and the field classifications below
> describe the real page, not a proposal for one. Anything still marked
> **NEW** or **CHANGE** below is the remaining gap between this doc and the
> code.
>
> **Authorities this file answers to, and does not override:**
> `SaftyFW/docs/CONFIG_REFERENCE.md` (what each field is, its default, its risk
> badge), `SaftyFW/docs/COMMISSIONING.md` (the mechanism: ids, staging, commit,
> the ESP cache), `SaftyFW/docs/SAFETY_MODEL.md` (what each guard does and the
> anti-nuisance doctrine). Where this file and one of those disagree, they win —
> except where this file explicitly proposes a *change*, which is always marked
> **NEW** or **CHANGE**.
>
> Every claim that is an inference rather than something read out of a file is
> tagged **⟨inferred⟩**.

---

## 0. The problem, stated precisely

`App/drivers/safety_commissioning_page.html` renders **58 parameters** as a flat
list of id/value pairs, grouped only by `CONFIG_REFERENCE.md`'s section numbers.
Every one is an editable box. The operator is asked, in the same visual weight,
for `abs_max_temp_c` (🔴, no default, disables the single guard that justifies
the board) and for `mainfault_debounce_ms` (🟠, 200 ms, contact bounce only).

Three facts make it worse than "long":

1. **Most of those 58 are not the operator's to answer.** 40 of them have a
   documented default in `CONFIG_REFERENCE.md`; 10 more are already known to the
   ESP from the zones configuration or from a measurement the system can take
   itself.
2. **The page is a write surface that usually cannot write.**
   `config_store_write()` refuses whenever `relay_owner_get_state() ==
   RELAY_OWNER_STATE_ARMED` (`SaftyFW/src/config_store_flash.c:279`), which is
   the steady state roughly `startup_grace_s` (60 s) after Pico boot. Nothing on
   the page says so.
3. **The page's values are not the Pico's values.** They come from the ESP-local
   NVS cache (`safety_cfg_store_*` in `App/drivers/safety_cfg_http.c`), and
   `set` has been asserted unconditionally per field, so a never-commissioned
   `abs_max_temp_c` renders as a *commissioned* `0 °C` — which in S1's arithmetic
   is a ceiling no temperature is below, i.e. never trip. Another agent is
   fixing that path; §6 states what the page must show once it is fixed.

**Parameter count discrepancy, worth knowing before you count anything:** the
page carries 58 fields; `SaftyFW/src/config_params.c`'s `CONFIG_PARAM_TABLE`
carries **59** — the page is missing `safety_tc_installed` (`0x0211`), minted in
firmware after the page was written (see that file's comment on why it is
`0x0211` and not `0x0207`). This spec classifies all **59** and notes the
missing one. Also note `max_expected_power_w` is `0x0319` in firmware but is
listed as decimal id 793 on the page, which is `0x0319` — consistent.

**The design goal:** the operator answers **four questions**. Everything else is
derived, measured, or defaulted, and lives behind an "Advanced" disclosure that
a normal commissioning never opens.

---

## 1. Classification of all 59 parameters

Legend for *Source*: **ASK** = the operator answers. **DERIVE** = computed from
something already known (formula given). **DEFAULT** = `CONFIG_REFERENCE.md`'s
documented default, never shown unless Advanced is opened. **MEASURE** = the
firmware measures it (a DERIVE whose input is an instrument, not the operator).

### 1.1 ASKED — the whole list, four questions

| # | Question shown to the operator | Parameter(s) written | Id(s) | Risk | Why it cannot be derived |
|---|---|---|---|---|---|
| Q1 | **"What is the highest temperature this kiln may ever reach?"** °C, required, positive, no unlimited option | `abs_max_temp_c` | `0x0104` | 🔴 | It is a property of the kiln's furniture, elements and brick — physically outside anything the firmware can see. `SAFETY_MODEL.md` §4/S1 and `CONFIG_REFERENCE.md` §1 both say it has no default. Deriving it from the hottest saved profile would set the ceiling from the thing the ceiling exists to catch. **ROADMAP M12 names this the field whose absence leaves S1's ceiling disabled.** M12 also *suggests* the selected `tc_type`'s datasheet maximum in the box (as a `placeholder`, never a `value` — see §1.4) and caps the field there: a starting number the operator lowers to what this kiln survives, still ASKED, and never committed unless they actually enter it. |
| Q2 | **"Where is the safety processor's own thermocouple?"** — three radio options: *Not fitted* / *In the kiln chamber* / *External overheat sensor (shell, exhaust, enclosure)* | `safety_tc_installed`, `tc_source`, `tc_placement_mode` | `0x0211`, `0x0101`, `0x0103` | 🔴 | A statement about physical reality (`SAFETY_MODEL.md` §3: *"not a tuning knob"*). Nothing on either board can see whether J7 has a probe in it or where that probe is mounted, and getting it wrong breaks S2/S10 in opposite directions. One question, three parameters — see §2.4 for the mapping. |
| Q3 | **"Mains supply voltage"** — dropdown, see §2.1 | `mains_voltage_v` | `0x030E` | ⚪ | Installation fact. A dropdown, not a number box, and **"Not set"** stays a first-class option (`CONFIG_REFERENCE.md` §3: unset ⇒ report `—`, never assume). |
| Q4 | **"Roughly how much power does this kiln draw at full output?"** kW, optional | `max_expected_power_w` | `0x0319` | ⚪ | ROADMAP M12 asks for it explicitly. Sanity check only; no guard reads it. Optional because it fails safe when absent. |

**That is the entire ASKED list: four questions, four screens' worth of one
control each.** Q4 is skippable. Everything below this line is invisible in the
default flow.

Two fields sit one step outside this list and are deliberately *not* asked:

- `max_rate_c_per_min` (`0x0204`, S8) — ships **0 = disabled** and stays there.
  `SAFETY_MODEL.md` §4/S8 forbids guessing it; it is enabled later from a logged
  full-power ramp, from the Advanced screen, with the measured maximum shown
  next to the box. Asking a first-time commissioner for it produces a guess,
  which is worse than off.
- `tc_expected_offset_c` (`0x020A`) — captured by a one-shot button at a soak
  (`SAFETY_MODEL.md` §4/S10's own suggestion), never typed.

### 1.2 DERIVED — computed from something already known

Sources referenced below:
**Z** = the zones configuration (`App/drivers/zones_http.h`: `zones_config_get_safety_tc_type()`,
`zones_config_get_tc_type()`, `zones_config_get_ct_mask()`, `zones_config_get_relay_mask()`,
`zones_config_get_thermo_mask()`).
**M** = the per-zone normal-current measurement of ROADMAP M12 (assumed to
exist, see §3).
**Q1–Q4** = the answers above. **HW** = a board constant.

| Parameter | Id | Risk | Formula / source |
|---|---|---|---|
| `tc_type` | `0x0105` | 🔴 | **Stored as Z `zones_config_get_safety_tc_type()`** — a dedicated ESP global (zones blob v5, *"the RP2040 safety processor's OWN"* — see `zones_http.c`'s version comment), resynced to the Pico by `safety_link.c`. **Not derived: it is ASKED, on the commissioning page** (field `tc_type`), because it states which physical probe is fitted to J7. The **zones page displays it read-only** and links here. See §2.3. |
| `borrowed_zone_index` | `0x0102` | 🔴 | **= Z**, the index of the zone the operator picked as the borrowed source *on the zones page*. Only reachable when Q2 = *Not fitted* (see §2.4). Constrained by the picker to zones the ESP actually reports, satisfying `CONFIG_REFERENCE.md` §1's "must name a zone the ESP actually reports" without a validation message. |
| `borrowed_type_expected` | `0x0210` | 🟠 | **= Z `zones_config_get_tc_type(borrowed_zone_index)`.** It is by definition the type of the zone channel being borrowed; asking for it separately only creates a way for the two to disagree. |
| `ct_channel_map[0..2]` | `0x0106`–`0x0108` | 🔴🟠 | **= M, from the zone current-sweep.** **SHIPPED** (`zones_http.c`, M12). The sweep on the zones page energizes one zone's relay(s) at a time with every other relay forced off, and records all three CT channels separately for that window; the channel that both carries real load (≥ 2 A, the same order as `i_present_a`) and dominates the other two by 4× is that zone's channel. Note this is a *measurement*, not the `ct_mask` inversion this row originally specified — `ct_mask` is an operator-entered field, so inverting it would only restate what was already typed, and the doc's own condition already required M's one-zone-at-a-time energize to confirm it. **Nothing is derived unless the derivation is unambiguous:** two channels within the dominance factor (a shared CT, which `ct_mask` explicitly permits), two zones resolving to the same channel, a zone whose `relay_mask` is not exactly its own relay bit (the zone-id-equals-relay-id identity `safety_core.c` indexes `relay_now_mask` with), or a sweep that aborted — each leaves the channel unset and says so on both pages, and the three ASK-in-Advanced fields stay available. Written over the same `SET_PARAM`/`COMMIT_CONFIG` path a typed value uses, so the Pico's own `config_params_finalize_ct_channel_map()` marks the group commissioned once all three land. |
| `i_normal_a[0..2]` **NEW** | `0x031A`–`0x031C` | 🟠 | **= M**, directly: the current recorded while that channel's zone was the only one energized. §3. |
| `zero_counts[0..2]` | `0x0302`–`0x0304` | 🟠 | **MEASURE.** Already re-measured at runtime after ≥5 min idle (`CONFIG_REFERENCE.md` §3). The page shows the live value and a *"re-zero now"* button; it is never typed. |
| `ct_cal[0..2].gain` / `.offset` / `.calibrated` | `0x0310`–`0x0318` | ⚪ | **= M.** `gain` = `i_normal_a[ch] / measured_counts_span`, `offset` = the zero from `zero_counts[ch]`, `calibrated` = true once M has completed for that channel. Before M runs they stay unset and `calibrated` = false. |
| `k_ct_v_per_a[0..2]` | `0x0308`–`0x030A` | ⚪ | **= K, from the zone current-sweep and the operator's own Q3/Q4 answers.** **SHIPPED** (`zones_http.c`, M12b) — this row previously said ⟨inferred⟩ from the CT's nameplate (`R_burden / N_turns`), which **OQ4** recorded as not written down anywhere, so in practice the field stayed at `config_store.c`'s `memset(0)`. The same sweep that derives `ct_channel_map` already measures each zone's own CT current with all other relays forced off; summing the resolved channels gives the whole-kiln current at full output, and `max_expected_power_w / mains_voltage_v` (Q4 / Q3) gives what that current should be. Since the Pico computes amps as `I = V_adc / (gain · √2 · k_ct)`, the whole calibration is one scale factor: **`k_new[c] = k_old[c] · (I_measured / I_expected)`**. **Nothing is calibrated unless the whole run is honest about itself:** any zone that did not resolve to a CT (the total would be short by that zone's share, and the scale dragged down with it), a shared-CT conflict, an unset Q3 or Q4, a `k_old` still at its uncommissioned `0.0` (the link carries amps, not counts — an uncalibrated `k_old` makes every reading `0.0 A`, so there is no measurement to scale), a measured total below the 2 A load threshold, a correction outside 0.2×–5×, or a result outside 0.0005–0.5 V/A each refuse the whole calibration and say why on both pages. Written over the same `SET_PARAM`/`COMMIT_CONFIG` path a typed value uses, then **confirmed by a live read-back** and backed out of the Pico's staged buffer on every failure arm, exactly as `ct_channel_map` is. The commissioning row keeps a clamp-meter override (`CURRENT_SENSE.md` §5 step 3) behind a checkbox. Not guard-irrelevant in the way this row used to claim: `current_presence_policy.c` uses `k_ct_v_per_a` to put `i_present_a` into the counts domain **when it is commissioned**, so calibrating it moves presence detection off the fixed fallback margin and onto the configured threshold. |
| `firing_margin_c` | `0x0201` | 🟠 | Stays at its default 100 °C, but shown **as an outcome of Q1**, not as an input: the review screen prints the actual ceiling S1 will enforce — `min(Q1, firing_max_c + 100)` in `CHAMBER_AGREED`, `Q1` flat in `EXTERNAL_OVERHEAT` — so the operator sees the number the guard uses rather than a margin they must reason about. |
| `correlation_window_s` | `0x0305` | 🟠 | **= 2 × `HEATER_WINDOW_MS`/1000 + decay ⇒ 150 s.** `CONFIG_REFERENCE.md` §6 makes this a hard cross-repo dependency on `profile_executor.c:27`. Because the ESP *knows* `HEATER_WINDOW_MS` at compile time, the page must compute and display it rather than let an operator shorten it — §6 calls shortening it "the fastest way to make S4 fire on every healthy low-duty firing". **CHANGE: make this read-only outside Advanced, with the deriving constant named on screen.** |
| `telemetry_period_ms` | `0x0405` | 🔴 | **= HW/ESP constant.** `CONFIG_REFERENCE.md` §4: the ESP's 1.5 s liveness window (`SAFETY_LINK_UP_PERIODS` × this) keys on it. It is half of a two-sided constant and must never be an operator field. **CHANGE: read-only, Advanced only, with the coupling stated.** |
| `context_max_age_s` | `0x0401` | 🟠 | **= 10 × `CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS`/1000 ⇒ 5 s** (`CONFIG_REFERENCE.md` §6 states the 10× relation explicitly). Same treatment as above. |

### 1.3 DEFAULTED — never shown outside Advanced

All values are `CONFIG_REFERENCE.md`'s, not invented.

| Parameter | Id | Default | Risk |
|---|---|---|---|
| `firing_margin_c` | `0x0201` | 100 °C | 🟠 |
| `overshoot_margin_c` | `0x0202` | 75 °C | 🟠 |
| `overshoot_time_s` | `0x0203` | 120 s | 🟠 |
| `max_rate_c_per_min` | `0x0204` | **0 = off** | 🟠 |
| `rate_window_s` | `0x0205` | 60 s | 🟠 |
| `blind_grace_s` | `0x0206` | 60 s | 🔴🟠 |
| `frozen_window_s` | `0x0207` | 600 s | 🟠 |
| `tc_disagreement_c` | `0x0208` | 200 °C | 🟠 |
| `tc_disagreement_time_s` | `0x0209` | 300 s | 🟠 |
| `tc_expected_offset_c` | `0x020A` | 0 °C (capture button) | 🟠 |
| `cj_warn_c` | `0x020B` | 60 °C | ⚪🟠 |
| `cj_max_c` | `0x020C` | 85 °C | 🟠 |
| `cj_time_s` | `0x020D` | 60 s | 🟠 |
| `borrowed_stale_s` | `0x020E` | 10 s | 🔴 |
| `borrowed_stale_trip_s` | `0x020F` | 60 s | 🔴 |
| `i_present_a` | `0x0301` | 2.0 A | 🟠 |
| `stuck_on_time_s` | `0x0306` | 20 s | 🟠 |
| `trip_verify_s` | `0x0307` | 10 s | 🔴 |
| `gain[0..2]` | `0x030B`–`0x030D` | 0.715 (R46/R43) | ⚪ |
| `power_window_s` | `0x030F` | 120 s | ⚪ |
| `link_timeout_s` | `0x0402` | 10 s | 🟠 |
| `link_dead_hard_s` | `0x0403` | 120 s | 🔴🟠 |
| `mainfault_debounce_ms` | `0x0404` | 200 ms | 🟠 |
| `startup_grace_s` | `0x0501` | 60 s | 🟠 |
| `estop_debounce_ms` | `0x0502` | 50 ms | 🟠 |
| `watchdog_timeout_ms` | `0x0503` | 1000 ms | 🔴 |
| `config_check_period_s` | `0x0504` | 10 s | 🔴 |
| `overcurrent_pct` **NEW** | `0x031D` | 150 % | 🟠 |
| `overcurrent_time_s` **NEW** | `0x031E` | 30 s | 🟠 |

**Tally:** 4 ASKED questions writing 6 parameters · 20 DERIVED/MEASURED ·
29 DEFAULTED · 3 NEW derived (`i_normal_a[0..2]`) · 2 NEW defaulted
(`overcurrent_pct`, `overcurrent_time_s`) = 59 existing + 5 new = 64 parameters,
of which the operator sees **four**.

### 1.4 The four 🔴 fields nobody may derive from a guess

`abs_max_temp_c`, `tc_source`, `tc_placement_mode`, `tc_type`. All four are
ASKED, on the commissioning page — `tc_type` is not derived from anything.
2026-08-28: it moved from an editable field on the zones page to read-only
there (`zones_page.html`'s `renderSafetyTcType()`), pointing at the safety
commissioning page as the one place it can actually be set — see §1.3/§2.3.
`ct_channel_map` is the fifth 🔴 and is
derived only under the unambiguity + measurement condition in §1.2 — shipped as
described there.

`abs_max_temp_c` stays ASKED, and the M12 suggestion does not change that:
selecting a `tc_type` shows that type's own datasheet maximum in the field and caps
the field there (`max` attribute plus `checkTcMaxContradiction()`'s existing hard
block on commit). The number almost always wants lowering — a kiln's furniture and
brick usually give out well below what the sensor can report.

**The suggestion is a `placeholder`, never a `value`** (opus review, 2026-08-28),
in both the Advanced form (`fieldInputHtml()`) and the guided flow (`gPrefill()`).
Rendered as a `value` it was indistinguishable from a typed answer: the Advanced
form's save pass posts every non-empty input, so saving any *other* field on the
page committed the suggestion, and `gBuildAnswers()` put it in the guided commit
body for the same reason. As a placeholder the input stays genuinely empty, is
skipped by both, and is visible to the operator all the same — so nothing commits
it on the operator's behalf, and a value the operator did type, or one already
committed on the Pico, still renders as a real `value` and saves normally. In the
guided flow Q1 remains *required*: `gValidateScreen1()` refuses to advance past an
empty field, so the effect is a prompt, never a silent default.

---

## 2. The owner's four specific instructions

### 2.1 Mains voltage becomes a dropdown

`mains_voltage_v` (`0x030E`, F32, ⚪, power estimate only).

| Option | Value written | Why offered |
|---|---|---|
| **Not set** *(default selection)* | *unset* — no `SET_PARAM` emitted at all | `CONFIG_REFERENCE.md` §3: *"Unset ⇒ report `—`, never assume."* This must remain reachable and must be the initial selection on a never-commissioned board. Selecting it on an already-set board is a **clear**, which needs the unset-write path in OQ3. |
| 120 V | 120 | North American single-phase line-to-neutral. Owner-named. |
| 208 V | 208 | North American **three-phase** line-to-line (120 V × √3). This is the single most common voltage for a commercial kiln in a US building with three-phase service, and it is 13 % below 240 — which, since power goes as V², is a **25 % power error** if 240 is assumed. Worth its own entry precisely because it is the one most likely to be mistaken for 240. |
| 240 V | 240 | North American split-phase; UK/AU single-phase. Owner-named. |
| 277 V | 277 | North American 480/277 wye line-to-neutral. Offered because single-phase kiln elements on a 480 V three-phase service are commonly fed line-to-neutral at 277. Lower priority than the others — ⟨inferred⟩ as a common installation, not read from any project document. |
| 380 V | 380 | Owner-named. Three-phase line-to-line on a 220 V system. |
| 400 V / 415 V | 400 / 415 | European (400) and UK/AU legacy (415) three-phase line-to-line. Offered as **two entries, not one**, because a 4 % difference is an 8 % power error and both are in current use. |
| 460 V | 460 | Owner-named. North American industrial three-phase. |
| **Other…** | free number | Escape hatch, so a dropdown never becomes a wall. Reveals a number box only when chosen. |

The stored parameter type and id do not change. The dropdown is purely a page
concern; the Pico still receives an F32 on `0x030E`.

### 2.2 Current-monitor calibration comes from the zones config

**Removed from the commissioning page entirely** (moved to Advanced, read-only):
`zero_counts[0..2]`, `k_ct_v_per_a[0..2]`, `gain[0..2]`,
`ct_cal[0..2].gain/.offset/.calibrated`, and the new `i_normal_a[0..2]`.
That is **16 of the 58 fields gone from the operator's view in one move.**

They are produced by the ROADMAP M12 zones-page button that *"energizes each
zone one at a time and records its normal current"*. This spec **assumes that
measurement exists** and consumes it. What the commissioning page shows instead
is one line per CT channel:

```
CT0 → Zone 1 "Bottom"   normal 11.4 A   measured 2026-08-27 14:02   ✓
CT1 → Zone 2 "Middle"   normal 11.1 A   measured 2026-08-27 14:05   ✓
CT2 → (not measured)                                                 ⚠ S3/S4 disabled, over-current guard inactive
```

with a link to the zones page and **no editable control**. A channel with no
measurement is the honest, expected state on a fresh board and must read as
"not measured", never as `0.0 A`.

**`k_ct_v_per_a[0..2]` is the one row in this block that is now more than a
mirror (M12b, shipped 2026-08-28).** It is no longer "read-only because the
zones config owns it" — it is *calibrated* by the same sweep, from the operator's
Q3/Q4 answers, by the scale-factor derivation in §1.2. Its row renders exactly
like a derived `ct_channel_map` row: the calibrated value as an `<output>`, a
sentence naming where it came from, a warning if what the Pico currently holds
disagrees with what the sweep calibrated, and a **"Override with a clamp-meter
measurement"** checkbox that reveals a plain number input. Unchecked, that input
stays empty and the save pass skips the id entirely, so a calibrated value can
never be restaged as though it had been typed. A channel the sweep did not
calibrate falls straight back to the read-only presentation above — the change
only ever adds provenance and an escape hatch, never removes a way to set the
field.

### 2.3 Safety thermocouple and safety relay configuration — read-only on the zones page

**Exactly which parameters move**, and where each surfaces:

| Parameter | Id | Owned and edited on | Displayed read-only on |
|---|---|---|---|
| `tc_type` (the safety processor's own probe type) | `0x0105` | **Commissioning page** — it names the probe physically fitted to J7, not a zone-layout choice. Stored in the zones blob v5 global (`zones_config_set_safety_tc_type()` / `zones_config_get_safety_tc_type()`) | **Zones page**, in the safety row, with a link to the commissioning page |
| `borrowed_zone_index` | `0x0102` | **Zones page** — the zone picker, only enabled when Q2 = *Not fitted* | Commissioning review screen |
| `borrowed_type_expected` | `0x0210` | **Nowhere** — always mirrors the picked zone's `tc_type` | Zones page (next to the borrowed zone), commissioning review screen |
| `ct_channel_map[0..2]` (the safety relay/CT mapping) | `0x0106`–`0x0108` | **Zones page**, measured by the current sweep (§1.2) and written to the Pico when it completes | Zones page, per zone, as a derived "safety CT" line; commissioning review screen as the three-row table in §2.2 |
| `i_normal_a[0..2]` **NEW** | `0x031A`–`0x031C` | **Zones page**, written only by the M12 measurement button | Both pages |
| `k_ct_v_per_a[0..2]` (the CT volts-per-amp scale) | `0x0308`–`0x030A` | **Zones page**, calibrated by the current sweep against Q3/Q4 (§1.2) and written to the Pico when it completes, with a clamp-meter override on the commissioning page | Zones page, in the sweep result line; commissioning page, as a derived row |

"Displayed but not reassignable" means, concretely: the commissioning page
renders each row it does not own as an `<output>`-style row with the value, its
source, and a link — no `<input>`, no `<select>`, no participation in the page's
POST body. The zones page gives `tc_type` exactly that treatment (as of M12 it
is text plus a link, and is no longer submitted in the zones POST body); the
zone-owned rows above are still edited there, and a change on either page
triggers the same stage-and-commit path described in §5.

### 2.4 Q2's three answers → three parameters

| Q2 answer | `safety_tc_installed` `0x0211` | `tc_source` `0x0101` | `tc_placement_mode` `0x0103` | Consequence stated on screen |
|---|---|---|---|---|
| **In the kiln chamber** *(recommended when a J7 probe is fitted)* | 1 | `OWN_J7`, or `BOTH` if a borrowed zone is also picked | `CHAMBER_AGREED` | "S2 and S10 active. S1's ceiling may be tightened by the running profile." |
| **External overheat sensor** (shell / exhaust / enclosure) | 1 | `OWN_J7` | `EXTERNAL_OVERHEAT` | "S2 and S10 are **off** — an external sensor has no obligation to agree with the zone readings. S1 uses your kiln maximum as a flat ceiling." |
| **Not fitted** | 0 | `BORROWED_ZONE` + a zone picker | forced `CHAMBER_AGREED` (Pico rejects otherwise) | The `SAFETY_MODEL.md` §3 warning, verbatim and unmissable: this trades away the sensor independence that justifies the board; S6 becomes the primary temperature protection. **Plus the current hard blocker: S13 is dormant — `sample_counter_advancing` is hardcoded false in `safety_core.c` — so `BORROWED_ZONE`/`BOTH` trips unconditionally today. Until that is fixed the page must refuse this option with that reason, not merely warn.** |

⟨inferred⟩ The numeric enum codes (`OWN_J7`=0 etc.) are still the page's
assumption — the existing page flags this too. Confirm against `config_store.c`
before implementing; a wrong code here is a silent 🔴.

---

## 3. NEW: the over-current guard, S14

Paired with the existing under-current family so the two are symmetric and read
from the same measured normal.

### 3.1 What exists today (the thing being mirrored)

`safety_guards.c`, inside the `context_valid` block:

- **S3** (`SAFETY_TRIP_LOAD_STUCK_ON`, **TRIP**): `in->any_current_present &&
  !in->relay_commanded_recently`, accumulated in `state->s3_stuck_elapsed_s`,
  fires at `stuck_on_time_s` (20 s).
- **S4** (**WARN only**): one line — `state->s4_warn =
  in->relay_commanded_continuously && !in->any_current_present;`. No timer of
  its own; the window lives in the caller's `relay_commanded_continuously`,
  which is computed over `correlation_window_s`.
- **S11** is the frozen-*reading* guard, not a current guard; it takes
  "current flowing or heat commanded" only as its qualifier.

Both S3 and S4 are **presence/absence against `i_present_a`**, never magnitude —
`SAFETY_MODEL.md` §3 is explicit that the current channels are *"not an
over-current or under-current protection device"*.

### 3.2 S14 — Zone current above its measured normal · **WARN** · *needs context*

```
for each channel ch in 0..2:
  i_normal_a[ch] is SET                                      (else: guard inactive for ch)
  AND that channel's mapped relay is commanded on now        (ct_channel_map[ch])
  AND amps[ch] > i_normal_a[ch] * overcurrent_pct / 100
    continuously for overcurrent_time_s                      ->  WARN
```

**What it compares.** The channel's measured current against *its own* recorded
normal, as a percentage — not against an absolute amp figure, and not against
any other channel. The owner's instruction, and it is also the only formulation
that works: three zones on one kiln can legitimately differ by 2× in element
draw, so a shared absolute threshold is either useless or a nuisance generator.

**Debounce / window.** `overcurrent_time_s`, default **30 s**. Reasoning against
`SAFETY_MODEL.md` §2's two-bar rule: the magnitude bar is `overcurrent_pct` =
150 % (a healthy element does not draw half again its measured normal; element
aging moves current *down*, not up), and the duration bar must exceed the
current front end's 1 s peak-hold decay and survive the switching edges inside
`HEATER_WINDOW_MS`. 30 s is ⟨inferred⟩ as the smallest value comfortably clear
of both, chosen to be shorter than S3's correlation window because this guard
does not need to reason about duty history — it only looks at the instant the
relay is commanded on. It is a DEFAULT and therefore revisable from Advanced
after the first firings.

**WARN, not TRIP.** Three reasons, in descending order of force:

1. `SAFETY_MODEL.md` §2: *"The default for a new guard is WARN. Promoting one to
   TRIP requires an argument in this document about what physical harm it
   prevents."* No such argument is offered here, so it does not get to trip.
2. §3 and §7 both place over-current squarely with the fuses and breakers —
   *"Element short / over-current: **No, by design.**"* Adding a tripping
   over-current guard would contradict the safety model's own honest-gaps
   register, which is a document change requiring the owner's sign-off, not a
   side effect of a UX spec. **This guard's purpose is the M12 goal — catching a
   CT on the wrong jack and an element/wiring change — not element protection.**
3. It shares an input path with the diagnostics that were guard-irrelevant until
   2026-08-24. A magnitude comparison on a 12-bit ADC behind a peak-hold, fed by
   a calibration produced by a self-service button, is not yet evidence anyone
   should open a contactor on.

If it is later promoted, the promotion is a `SAFETY_MODEL.md` §4 edit plus a new
`SAFETY_TRIP_*` reason, not a config change. **Open question OQ1.**

**When no normal has been measured — the property that matters.** The guard is
evaluated **per channel** and a channel whose `i_normal_a[ch]` has no
`fields_set` bit is **skipped entirely**: no accumulation, no warn, and the
channel reports **inactive** (not passing) in the diagnostic frame, following
§6a's rule that *"a guard that cannot evaluate must not look like a guard that
evaluated and found nothing wrong."* An unmeasured zone can never produce a
warning from S14. This must be **negative-tested** — a test that sets a huge
current on an unmeasured channel and asserts no warn — per this repo's
"negative-test every check" rule; three checks have shipped vacuous without it.

State to add to `safety_guard_state_t`: `float s14_over_elapsed_s[3];` and
`bool s14_warn[3];`, both reset in the same `!in->context_valid` block that
already resets `s3_stuck_elapsed_s`/`s4_warn` — S14 needs `ct_channel_map` and
the commanded-relay fact, both of which are context.

Input to add to `safety_guard_input_t`: `float amps[3];`,
`bool amps_valid[3];`, `bool relay_commanded_now_for_ct[3];` — flattened
scalars with their own validity flags, matching the module's existing
convention and keeping it link-header-free.

### 3.3 New parameter ids

Minted by the rule `config_params.c` already follows — *append after the last
used id in the field's own doc section*. Section 3 (current channels) currently
ends at `0x0319` (`max_expected_power_w`), so:

| Id | Name | Type | Default | `fields_set` | Section |
|---|---|---|---|---|---|
| `0x031A` | `i_normal_a[0]` | F32 | *unset* | per-channel bit, **NEW** `CONFIG_STORE_SET_I_NORMAL_0` | §3 |
| `0x031B` | `i_normal_a[1]` | F32 | *unset* | `..._1` | §3 |
| `0x031C` | `i_normal_a[2]` | F32 | *unset* | `..._2` | §3 |
| `0x031D` | `overcurrent_pct` | U16 | 150 | — | §3 |
| `0x031E` | `overcurrent_time_s` | U16 | 30 | — | §3 |

Three separate `fields_set` bits, **not** one group bit — deliberately unlike
`ct_channel_map`'s single group bit. The reasoning is opposite in the two cases:
a half-populated CT *map* means a guard is watching a channel nobody confirmed
(unsafe subset), whereas a half-populated set of *normals* means S14 is active
on two zones and inactive on the third, which is a strictly correct partial
result and is exactly what a shop that has measured two zones so far should get.

Where they slot into `config_params.c`: three places, all of which
`test_config_store.c`'s table-walk test already checks for drift —
`CONFIG_PARAM_TABLE[]` (append after the `0x0319` entry, before `0x0401`),
`config_params_get()`'s switch, and `config_params_set()`'s switch. Plus the
record fields in `config_store.h` (a v2 → v3 record bump, or the reserved tail
if it has room — **open question OQ2**) and `safety_core.c`'s
`safety_core_load_guard_cfg()`, which is the step the 2026-08-27 audit found
missing for S1: **a host test cannot see a value that never arrives.**

`SAFETY_MODEL.md` §3's sentence *"There is no over-current guard"* and
`CONFIG_REFERENCE.md` §3's identical claim both become false the day S14 lands
and must be edited in the same commit.

---

## 4. Screen-by-screen flow

Five screens, of which a returning operator sees only the last.

### Screen 0 — Preflight (always first, never skippable)

Answers "can I even do this right now?" before showing a single field.

```
Safety processor:   LINK UP · firmware 1.4.2 · config_crc 0xCAFE
Write window:       ARMED — configuration cannot be written right now      [Open write window]
Commissioned:       NO — abs_max_temp_c, tc_source, tc_placement_mode, tc_type not set
                    Guards disabled because of it: S1, S2, S8, S10, S11, S12
```

The write-window line is the new thing and §5 is its whole design. The
"guards disabled" line is generated from the same `fields_set` data the current
page already renders per field — hoisted to the top, where it reads as a
statement about the machine rather than as decoration on a form row.

### Screen 1 — This kiln (Q1, Q3, Q4)

Three controls. Kiln maximum temperature (°C, required, `> 0`, with the
thermocouple's own inferred maximum shown beneath it as a hard cap — see
Screen 3). Mains voltage dropdown (§2.1). Maximum expected power (kW, optional,
"used only to sanity-check wiring; no guard reads it").

**No unlimited option, and no 0.** The input rejects empty, 0 and negative
client-side, and the Pico rejects them at commit exactly as it rejects NaN. The
helper text says *what* the number is for in the operator's language: "the
highest temperature the kiln's furniture, elements and brick can survive — not
the hottest firing you plan to do."

### Screen 2 — The safety thermocouple (Q2)

The three radio options of §2.4, each with its consequence sentence rendered
*with* the option rather than in a footnote, so the trade is visible at the
moment of choosing. Choosing *Not fitted* expands the zone picker and shows the
`BORROWED_ZONE` warning as a block, not an aside — and, today, is disabled with
the S13-dormant reason.

### Screen 3 — Review what we worked out (read-only)

The screen that makes derivation trustworthy: everything derived, with its
source named.

```
Asked, on this page (Advanced)
  Safety thermocouple type      Type K              → Advanced
  Thermocouple maximum          1372 °C             inferred from Type K (MAX31856 datasheet)

From the zones configuration
  CT0 → Zone 1 "Bottom"         normal 11.4 A       measured 2026-08-27 14:02
  CT1 → Zone 2 "Middle"         normal 11.1 A       measured 2026-08-27 14:05
  CT2 → not mapped              ⚠ S3/S4 and the over-current guard inactive on CT2

From your answers
  S1 will trip above            1250 °C             = your kiln maximum
                                                    (in CHAMBER_AGREED it tightens to the
                                                     running profile's peak + 100 °C)
  Over-current warning at       17.1 A on CT0       = 150 % of 11.4 A, sustained 30 s
  Estimated full power          10.9 kW at 240 V    vs your stated 11 kW ✓
```

**Thermocouple maximum is inferred, never asked** (ROADMAP M12), from the
`TC_MAX_C_BY_TYPE` table the existing page already carries — Type K 1372 °C etc.,
the MAX31856's own per-type range. If Q1 exceeds it, the page refuses with
*"a Type K thermocouple cannot measure 1500 °C — S1 would go blind before it
tripped"*, which is a genuinely more useful message than a range error.

### Screen 4 — Commit

One button. It stages every parameter — the four answered, every derived value,
and the defaults for anything still unset — and issues a single
`COMMIT_CONFIG`. On rejection it renders the Pico's own field name and reason
verbatim (the current page already does this correctly; keep it) and returns to
the screen that owns that field. On success it re-reads from the Pico and shows
Screen 3 with the live `config_crc`.

### Advanced (a disclosure at the bottom of Screen 4, closed by default)

The existing 59-field page, unchanged in structure, with its risk badges and
guard-disabled banners intact — plus the read-only rows for everything §2.3
moved to zones. **This is the compatibility escape hatch and it must not be
deleted:** the flow above cannot express every configuration, and a field that
exists in firmware but nowhere in the UI is a field nobody can fix. Its heading
says so: *"Every parameter the safety processor holds. Defaults are documented
and safe — change one only with a measurement in hand."*

---

## 5. The write window — the workflow constraint, not a detail

**The fact:** `config_store_write()` refuses whenever the relay owner is ARMED
(`config_store_flash.c:279`, via `config_store_decide_write()`). ARMED is the
steady state ~60 s after Pico boot (`startup_grace_s`). So today the only write
window is the boot GRACE period, and the page says nothing about it — an
operator fills in a form, presses save, and gets a refusal whose cause is a
sixty-second window that closed before they opened the page.

Three ways to handle it. This spec picks the third and specifies the first two
as fallbacks.

**(a) Tell the truth and let them race it.** Screen 0's write-window line shows
`GRACE — 41 s remaining` with a countdown, or `ARMED — closed`. The commit
button is disabled while closed and says why. Cheap, honest, and *unusable* —
a real commissioning pass takes longer than 60 s to fill in.

**(b) Stage first, commit inside a window the operator opens deliberately.**
The page never needs the window until Screen 4. Everything before that is
browser-local. So Screen 4's button becomes: *"Restart the safety processor and
apply"* — the ESP asks the Pico to reset, waits for it to come back in GRACE,
and fires the whole `SET_PARAM`×N + `COMMIT_CONFIG` burst inside that window,
with the countdown visible. Requires only an existing reset path and costs the
operator one confirmation. **Preconditions the page must enforce before
offering it: no profile firing, relay open, and the operator confirming it.**
This works today with no new wire command and is the **minimum viable
behaviour** — implement it first.

**(c) A deliberate configuration window — the recommended end state.** A new
command, **`REQUEST_CONFIG_WINDOW` (`0x25`, next unallocated after `0x24`)**,
ESP → Pico: *"disarm the relay owner and hold it disarmed for N seconds so
configuration can be written."* The Pico enforces its own preconditions exactly
as §6b's update mode does — relay open, no trip pending, no current flowing —
and refuses otherwise, with the reason on the wire. It is strictly safer than
(b): (b) reboots a safety processor to make it writable, which means a window
where nothing is watching; (c) never stops the guards, it only declines to
energize. It also makes the window explicit and bounded rather than a side
effect of a boot. **Open question OQ5** — this is a two-sided change and needs
the owner's and SaftyFW's agreement, so it is proposed here, not assumed.

Whichever lands, the invariant the page must never break: **it must never
present a form it cannot save.** The commit button's enabled/disabled state, and
its label, are driven by the live write-window state, not by whether the form
validates.

---

## 6. What the page must show once the read-back path is trustworthy

Another agent is fixing the ESP-cache/`set`-assertion path. Once it is fixed:

**Three display states per parameter, visually distinct, never collapsible into
two:**

| State | How it renders | Rule |
|---|---|---|
| **UNSET** | The word `Not set` in the value position — *no number at all*, no `0`, no greyed placeholder that looks like a value — plus the card's `NOT SET` chip and the "disabled while unset" guard list the current page already draws | `COMMISSIONING.md` §3.1: an unset parameter carries `"set": false` and **omits** `value` entirely. The page must render from the *absence of the key*, and treat a `value` present alongside `set: false` as a bug worth logging, not as data. |
| **SET to a real value** | The number, with its unit | — |
| **SET to a value that disables its guard** (e.g. `max_rate_c_per_min` = 0) | The number **plus** the `GUARD DISABLED` banner already implemented | `CONFIG_REFERENCE.md` §7: a disabled guard that looks enabled is worse than no guard. |

**`0` is never a display of UNSET, and the specific case that motivated this:**
`abs_max_temp_c` = 0 rendered as a commissioned value means "S1 never trips",
displayed as though the kiln were protected at 0 °C. After the fix the page must
show `Not set — S1 absolute over-temperature is DISABLED` and Screen 0 must
carry it as a top-level blocker.

**Stale and link-down stay as they are** — the current page's whole-page banner
plus per-field tint is correct and matches `COMMISSIONING.md` §3's "the
telemetry wins and the GUI must say so". Keep it, and extend it to the read-only
derived rows: a derived value shown during a stale period is no more trustworthy
than a typed one.

**One addition:** Screen 3's derived rows must show whether the derivation's
*input* is fresh, not just the output. "CT1 → Zone 2, normal 11.1 A" is
meaningless if the zones config changed after the last commit; the row carries
the measurement timestamp and goes amber when the zones config's own revision is
newer than `cached_config_crc`'s fetch.

---

## 7. Migration and rollout

**What a board already carrying a committed config meets.**

| Situation | What happens |
|---|---|
| Board committed under the current 58-field page, all fields set | Screens 1–3 open **pre-filled from the committed values**, not from defaults. Q1 shows the existing `abs_max_temp_c`, Q2's radio is selected from the existing `tc_source`/`tc_placement_mode` pair, Q3's dropdown pre-selects the existing `mains_voltage_v` — or **Other…** with the number, if it is not one of §2.1's entries, which must not silently snap to the nearest option. Committing again with no edits must produce a byte-identical record and therefore an unchanged `config_crc`; if it does not, the derivation disagrees with what is stored and that is a bug worth surfacing, not smoothing over. |
| A committed value **disagrees with what the new flow would derive** — e.g. stored `tc_type` = J, zones says K; or stored `ct_channel_map` does not match `ct_mask` | **Do not overwrite silently.** Screen 3 shows the row in conflict with both values and asks which is right, with the consequence spelled out ("the type must match the probe physically fitted to J7 — a mismatch reads plausible and wrong, usually low"). Choosing zones' value writes it; choosing the stored value writes the zones config instead, so the two converge either way. **This is the single most important migration behaviour in this section** — a derivation that quietly overwrites a hand-commissioned 🔴 field is exactly the failure this whole spec is trying to avoid. |
| Board committed but `i_normal_a[*]` unset (every existing board) | S14 inactive on all three channels, reported as inactive. Screen 3's CT table shows "not measured" with a link to the zones page. **No behaviour change of any kind on an existing board until the M12 measurement is run** — this is the property that makes S14 safe to ship. |
| Record version bump (if `i_normal_a`/`overcurrent_*` do not fit the reserved tail) | `COMMISSIONING.md` §1.1's three-outcome load applies unchanged: a v2 record migrates forward, the new fields default (unset for the normals, 150/30 for the thresholds), and **`calibration_missing` stays true** because the record was never commissioned against the new fields. That last part is a real operational cost — every already-commissioned board comes back reading NOT COMMISSIONED after the update — so **prefer the reserved tail if it has room** (OQ2). If it does not, the release note must say plainly that a re-commit is required, and the M12 "uncommissioned refuses heating enable" item must not land in the same release. |
| Bench boards carrying the bench preset | Unchanged — the preset leaves `calibration_missing` set, and Screen 0 says NOT COMMISSIONED for that reason. The new flow must not make the bench preset look like a commissioning; §4.1's rule is untouched. |

**Rollout order**, which is really ROADMAP M12's ordering note applied to this
page. Each step is shippable and reversible on its own:

1. The read-back/`set` fix (in flight elsewhere) — **nothing here is safe to
   build on a cache that lies.**
2. Write-window handling, mode (b). The current page becomes honest about when
   it can save. No field changes.
3. Screens 0–4 over the existing parameter set, with Advanced holding the full
   59. Mains dropdown lands here. No firmware change required.
4. Zones-side moves (§2.3): read-only rows on both pages, derivation of
   `ct_channel_map`, `borrowed_*` and `tc_type`.
5. M12's per-zone measurement button ⇒ `i_normal_a[0..2]`, `ct_cal[*]`.
6. S14, once (5) has produced real normals on a real kiln.
7. `REQUEST_CONFIG_WINDOW` (mode c), if the owner accepts OQ5.
8. **Last, unchanged from M12's own note:** an uncommissioned safety processor
   refuses heating enable. Landing it before step 3 locks the bench out of
   heating.

---

## 8. Open questions for the owner

| # | Question | Why it blocks |
|---|---|---|
| **OQ1** | **S14 as WARN, or TRIP?** This spec says WARN, because `SAFETY_MODEL.md` §2 makes WARN the default for a new guard and §3/§7 currently state, twice, that over-current is the breakers' job by design. Promoting it to TRIP is a safety-model change with a written argument, not a threshold edit. | Determines whether §3's *"There is no over-current guard"* becomes "there is a warning" or "there is a trip", and whether a new `SAFETY_TRIP_*` reason is minted. |
| **OQ2** | Does `config_store_record_t`'s reserved tail have room for 5 more parameters (3×F32 + 2×U16 = 16 B) without a format-version bump? | A bump forces `calibration_missing` back on for every already-commissioned board (§7). |
| **OQ3** | Is there a wire representation for **clearing** a parameter back to unset? `SET_PARAM` sets; nothing observed clears. Selecting "Not set" on the mains dropdown of an already-set board needs one. | Without it, "Not set" is a one-way door — reachable only before the first commit — and `CONFIG_REFERENCE.md` §3's unset semantics become unreachable in practice. |
| **OQ4** | ~~What is `k_ct_v_per_a` for the CTs actually fitted?~~ **CLOSED 2026-08-28 (M12b).** Nobody has to know: the zone current-sweep calibrates it against the Q3/Q4 answers rather than against a part number (§1.2). The nameplate figure is still what a clamp-meter override would be compared to, but it is no longer required to get a real kW figure. | — |
| **OQ5** | Accept `REQUEST_CONFIG_WINDOW` (§5 mode c) as the end state, or live with the reboot-into-GRACE approach (mode b)? | Mode (c) is a two-sided wire change; mode (b) reboots a safety processor to make it writable. |
| **OQ6** | Confirm the enum codes for `tc_source` and `tc_placement_mode` against `config_store.c`. Both this spec and the existing page are guessing (`OWN_J7`=0 / `BORROWED_ZONE`=1 / `BOTH`=2; `CHAMBER_AGREED`=0 / `EXTERNAL_OVERHEAT`=1). | A wrong code writes a 🔴 field to a plausible wrong value with no error anywhere. |
| **OQ7** | S13 is dormant (`sample_counter_advancing` hardcoded false in `safety_core.c`), so `BORROWED_ZONE`/`BOTH` trips unconditionally. Should Q2's *Not fitted* option be **hidden**, **shown-disabled with the reason**, or **allowed with a warning**? This spec assumes shown-disabled. | It is the only Q2 answer available to a board built without the J7 daughterboard. |
| **OQ8** | The page carries 58 parameters; firmware carries 59 (`safety_tc_installed`, `0x0211`). Confirm that field's semantics — this spec treats it as the boolean form of Q2's *Not fitted*. | If it means something else, Q2's mapping in §2.4 is wrong. |

---

## 9. Success measure

Before: **58 editable fields**, no grouping by who should answer them, 5 of them
🔴 with no default, and a save button that usually cannot save.

After: **4 questions** (one of them a three-way radio, one optional), a read-only
review of 20 derived values with their sources named, 29 documented defaults the
operator never sees, and a commit button that is enabled exactly when a write
will succeed.
