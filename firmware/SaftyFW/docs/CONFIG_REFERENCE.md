# Configuration Reference

> **Status:** planning · **Last reviewed:** 2026-08-16
> **Keep this file current.** This table is the one place the whole tunable
> surface is visible; a field added in code and not added here is a field nobody
> will commission. Update it in the same commit. If it disagrees with the code,
> **the code wins.** Checklist at the bottom.

Every tunable in `SaftyFW`, in one table, with its default and — the column that
matters most — **whether getting it wrong is dangerous, annoying, or cosmetic.**

Stored in the last flash sector, versioned and CRC'd, with compiled-in fallbacks.
See `ARCHITECTURE.md` §7 for the store and §8 for the RP2040 flash-write
constraints.

**How these values actually get set is [`COMMISSIONING.md`](COMMISSIONING.md)** —
the ESP web GUI writes them once, they live on the safety processor outside
both OTA slots, and the ESP refetches only when the reported `config_crc`
changes. This file stays the authority on *what* each field is and what
happens if it is wrong; that one owns the mechanism.

**Legend for *Risk if wrong*:**

- 🔴 **Dangerous** — a wrong value can leave a real fault undetected.
- 🟠 **Nuisance** — a wrong value causes spurious trips, which leads to the
  system being bypassed, which is dangerous by a longer route (`SAFETY_MODEL.md` §2).
- ⚪ **Cosmetic** — affects a displayed number only.

---

## 1. Commissioning — required, no safe default

Most of these have **no defaults**. Until each is set, the guards that depend
on it stay disabled and the diagnostic frame reports `calibration_missing`.
`tc_type` is the one exception in this section: it keeps a real compiled
default (K) rather than shipping disabled, but is *also* required for
`calibration_missing` to clear — see its own row's Notes for why.

| Field | Unit | Affects | Risk | Notes |
|---|---|---|---|---|
| `tc_source` | enum | **everything thermal** | 🔴 | `OWN_J7` \| `BORROWED_ZONE` \| `BOTH`. `BORROWED_ZONE` trades away sensor independence — see `SAFETY_MODEL.md` §3 before choosing it |
| `borrowed_zone_index` | 0–2 | S13, and the reading itself | 🔴 | Only meaningful when `tc_source` ≠ `OWN_J7`. Must name a zone the ESP actually reports |
| `tc_placement_mode` | enum | S1 ceiling mode, **S2**, **S10** | 🔴🟠 | `CHAMBER_AGREED` \| `EXTERNAL_OVERHEAT`. A statement about where the sensor physically is, not a preference. Wrong in one direction silences the only cross-check; wrong in the other makes S10 fire constantly on correct readings. **Forced to `CHAMBER_AGREED` when `tc_source` is `BORROWED_ZONE`** |
| `abs_max_temp_c` | °C | **S1** | 🔴 | In `CHAMBER_AGREED`, what the furniture and elements survive (~1300 for cone 10). In `EXTERNAL_OVERHEAT`, what *that location* must never exceed — unrelated to any firing temperature |
| `tc_type` | enum | the **own** sensor's readings | 🔴 | Must match the thermocouple physically fitted to J7. A mismatch reads **plausible and wrong**, usually low. **Per-thermocouple, not global** — each zone has its own, configured on the ESP and reported in the context frame. See `THERMOCOUPLE.md` §2. **Unlike the other rows in this table, `tc_type` keeps a real compiled default (K)** — an uncommissioned board still runs a real, usable type, it is not left disabled. As of 2026-08-24 it is *also* `fields_set`-gated (`CONFIG_STORE_SET_TC_TYPE`), for a narrower reason than "no safe default": a commissioned K and a never-touched, defaulted-to-K record are the identical byte, and the per-type plausibility band (`THERMOCOUPLE.md` §2/§5, `max31856_tc_range_policy.h`) needs to tell them apart. Uncommissioned ⇒ that band widens to the union of all eight types' ranges instead of applying K's alone, and `calibration_missing` stays true until this field is committed too (folded into §7's existing required-set check) |
| `ct_channel_map[3]` | zone/relay ids | **S3**, **S4** | 🔴🟠 | Which relay each CT actually watches. Confirmed by the one-relay-at-a-time check in `CURRENT_SENSE.md` §5 step 2. S3/S4 stay disabled until it passes |

## 2. Temperature guards

| Field | Default | Unit | Guard | Risk | Notes |
|---|---|---|---|---|---|
| `firing_margin_c` | 100 | °C | S1 | 🟠 | Added to the profile's peak. **`CHAMBER_AGREED` only** |
| `overshoot_margin_c` | 75 | °C | S2 | 🟠 | Generous on purpose — ramp-end overshoot and TC placement spread both live inside this |
| `overshoot_time_s` | 120 | s | S2 | 🟠 | Two minutes of sustained excess, not a transient |
| `max_rate_c_per_min` | **33.3** (2026-09-05, was 0/off) | °C/min | S8 | 🟠 | Compiled record default is **2× the fastest RISING `ramp_c_per_hr` in any of KilnFW's 28 built-in profiles** (`profiles_builtin_table.inc`). S8 (`safety_guards.c`) only trips on a positive (climbing) delta, so a fast COOLING segment cannot be the basis even though its declared rate is a larger number: the four tied 9999.0 C/hr segments (e.g. `FSCGCL` "Shimbo Crystal Celestite Schedule") are all crash-cool (target below the previous segment's) and are excluded. The fastest actual rising segments are two tied 999.0 C/hr steps in `FSCGB1` "Shimbo Crystal Holding Pattern 2" (both part of its holding-pattern dwell cycling): 1050 C to 1075 C, and 1075 C to 1100 C, so 999.0 / 60 × 2 = 33.3 C/min — permissive by construction, no shipped profile's rising ramp can trip it. **Still fields_set-gated** (`safety_core.c`'s `safety_core_load_guard_cfg()`): on an uncommissioned board the guard stays forced OFF (0.0f) exactly as before — this default only becomes the *armed* value once `0x0204` is explicitly written through commissioning, or is otherwise just the record's at-rest value. A bench rig should still commission its own tighter, measured value rather than rely on this one — see `COMMISSIONING.md` |
| `rate_window_s` | 60 | s | S8 | 🟠 | |
| `blind_grace_s` | 60 | s | S5 | 🔴🟠 | WARN immediately, TRIP after this. Long enough for a connector wiggle, short enough that a whole firing cannot run blind |
| `frozen_window_s` | 600 | s | S11 | 🟠 | Matches `thermal_guard.c`'s `FROZEN_WINDOW_S`. Only armed while heat is happening |
| `tc_disagreement_c` | 200 | °C | S10 | 🟠 | vs the **nearest** valid zone, not the mean. `CHAMBER_AGREED` only |
| `tc_disagreement_time_s` | 300 | s | S10 | 🟠 | |
| `tc_expected_offset_c` | 0 | °C | S10 | 🟠 | Optional captured steady-state offset; compare the *change*, not the raw difference |
| `cj_warn_c` | 60 | °C | S12 | ⚪🟠 | Enclosure warning |
| `cj_max_c` | 85 | °C | S12 | 🟠 | Well inside the MAX31856's ±125 °C limit |
| `cj_time_s` | 60 | s | S12 | 🟠 | |
| `borrowed_stale_s` | 10 | s | **S13** | 🔴 | `sample_counter` not advancing ⇒ reading treated as invalid. `BORROWED_ZONE`/`BOTH` only |
| `borrowed_stale_trip_s` | 60 | s | **S13** | 🔴 | …and then trip |
| `borrowed_type_expected` | — | enum | S13 | 🟠 | The `tc_type` the borrowed channel is expected to report. A change means someone reconfigured the main board's channel underneath us |

**S5's `bad_read_count_threshold` / `bad_read_time_s` are deliberately NOT in
this table (owner decision, 2026-09-03).** `blind_grace_s` above is S5's one
commissionable knob; the other two bars of S5's "count AND time" gate
(`safety_guards.c`'s `BAD_READ_COUNT_DEFAULT` = 10, `BAD_READ_TIME_S_DEFAULT`
= 5.0 s) stay **compiled-in**, unlike every other guard's limits in this
file. `safety_guards.h`'s `safety_guard_config_t` still carries
`bad_read_count_threshold`/`bad_read_time_s` fields with the usual "0 means
use the compiled default" convention — that plumbing was added for Phase 7
consistency with the struct's other fields — but nothing sets them:
`config_store.c`/`config_params.c` (contrast `frozen_window_s`, S11's
equivalent, which has a real `config_store` record field, a wire param at
0x0207, and NVS persistence) never populate them, so they read 0 forever and
S5's count/time gate always runs on the compiled defaults regardless of what
a commissioning client sends.

This is not an unwired gap waiting for its Phase 7 turn — **it is staying
this way.** S5 works correctly on its compiled defaults, and the guard this
table exists to protect is exactly the one a wrong commissioned value could
weaken: making the count/time bars configurable would require a
persisted-struct migration (a new `config_store` record field, a wire param
id, `config_params.c` get/set cases, NVS layout versioning) to add a field
whose only credible failure mode is "someone sets it wrong and a real bad
read stops tripping S5 in time." There is no established benefit on record
that offsets that risk — no bench finding that the compiled 10-reads/5.0s
bar is wrong for this hardware, no commissioning workflow that needs it
per-installation the way `abs_max_temp_c` or `tc_type` genuinely do. If a
future finding DOES establish a concrete need (a specific installation
where 10/5.0s is measurably wrong), reopen this as a fresh decision with
that evidence — this note is not itself a blocker, it is the record of why
nobody should re-propose the same migration on "more configurability is
better" alone.

## 3. Current channels

Remember the scope limit: **load-active detection and a power estimate,
plus (2026-08-28) a per-channel magnitude WARN against the channel's own
measured normal.** There is still no *tripping* over-current guard, and
breakers remain the electrical protection (`SAFETY_MODEL.md` §3/§4 S14/§7).

| Field | Default | Unit | Guard | Risk | Notes |
|---|---|---|---|---|---|
| `i_present_a` | 2.0, or auto | A | S3, S4, S9 | 🟠 | A load-active threshold. Only has to separate noise from a conducting element — one to two orders of magnitude of slack. **2026-09-06:** auto-derives to half the smallest committed `i_normal_a[]` at `COMMIT_CONFIG` unless set directly by hand (a direct `SET_PARAM` on this field always wins, permanently) |
| `zero_counts[3]` | measured | ADC counts | S3, S4, S9 | 🟠 | Re-measured at runtime after ≥5 min idle. **Not zero** — single-supply offset |
| `correlation_window_s` | 150 | s | S3, S4 | 🟠 | **≥ 2 × the ESP's 60 s heater window + decay.** Shortening this is the fastest way to make S4 fire on every healthy low-duty firing |
| `stuck_on_time_s` | 20 | s | S3 | 🟠 | |
| `trip_verify_s` | 10 | s | **S9** | 🔴 | Past the 1 s peak-hold decay and the contactor's drop-out. Too short → false "trip ineffective"; too long → delayed alarm on a genuinely live kiln |
| `k_ct_v_per_a[3]` | — | V/A | *none* | ⚪ | Power estimate only |
| `gain[3]` | 0.715 | — | *none* | ⚪ | R46/R43; refine only if the resistors are not 1 % |
| `mains_voltage_v` | unset | V | *none* | ⚪ | Nominal. Power goes as V², so a 5 % sag is a 10 % error. Unset ⇒ report `—`, never assume |
| `power_window_s` | 120 | s | *none* | ⚪ | Conduction-fraction averaging window |
| `i_normal_a[3]` **NEW** | unset | A | **S14** | 🟠 | Per-zone measured normal (energize-one-zone-at-a-time, ROADMAP M12/zones page). Individually gated — a channel with no measurement is skipped by S14 entirely, never treated as 0 A |
| `overcurrent_pct` **NEW** | 150 | % | **S14** | 🟠 | WARN threshold, as a percentage of that channel's own `i_normal_a` |
| `overcurrent_time_s` **NEW** | 30 | s | **S14** | 🟠 | Sustained-above-threshold window before WARN |
| `ct_topology` **NEW** (0x031F) | 0 (per_zone) | enum | **S14, S15** | 🟠 | `per_zone` (0, default) — one CT per zone, unchanged behaviour above. `summed` (1) — one shared CT (channel 3/GPIO28) reads every zone; S14 compares it against the sum of `i_normal_a[]` for zones commanded on, channels 1-2 report not-fitted, and S15 (new, WARN-only, `0.7×` a zone's normal for 30 s) flags a likely open heater. See `CT_COMMISSIONING_PLAN.md` step 3 |

**2026-08-24 note, made true by this date's commit, not before it.** The
`*none*`/⚪ badges on `k_ct_v_per_a`/`gain`/`mains_voltage_v` were *aspirational*
until this date: `current_sense_set_cal()` (the function that loads these
values into `current_sense.c`) was never called anywhere in `src/`, which
left `k_ct_v_per_a` permanently `0.0f` and, as an unintended side effect,
silently disabled S3/S9/S11's presence detection too (`current_snapshot_t
.amps[]`, the only input `current_any_present()` read, is hard-zeroed by
`cs_counts_to_amps()` whenever `k_ct_v_per_a <= 0.0f`) — i.e. `k_ct_v_per_a`
WAS affecting guards, just never in the direction anyone intended. The fix
wires `current_sense_set_cal()` from `config_store` and, separately,
decouples presence detection from `k_ct_v_per_a` entirely
(`current_presence_policy.h`) so these three fields are now honestly
guard-irrelevant, matching the table above. `i_present_a`/`zero_counts`
above were never affected by this bug — they always had real defaults and
`current_task_reload_cal()` (`src/tasks/current_task.c`) is the code that
now actually delivers them to `current_sense.c`.

## 4. Link and liveness

| Field | Default | Unit | Guard | Risk | Notes |
|---|---|---|---|---|---|
| `context_max_age_s` | 5 | s | S2, S3, S4 | 🟠 | Stale context ⇒ those guards go **inactive**, never pessimistic |
| `link_timeout_s` | 10 | s | S6b | 🟠 | Trips only **if current is also flowing** |
| `link_dead_hard_s` | 120 | s | S6b | 🔴🟠 | Unconditional backstop |
| `mainfault_debounce_ms` | 200 | ms | S6a | 🟠 | |
| `telemetry_period_ms` | 500 | ms | — | 🔴 | The ESP's liveness detector keys on this. Changing it means changing the ESP's 1.5 s window too (`LINK_PROTOCOL.md` §8) |

## 5. Timing and system

| Field | Default | Unit | Risk | Notes |
|---|---|---|---|---|
| `startup_grace_s` | 60 | s | 🟠 | No thermal or correlation guard arms before this. A rolling window with three samples in it has no opinion worth acting on |
| `estop_debounce_ms` | 50 | ms | 🟠 | Contact bounce only |
| `watchdog_timeout_ms` | 1000 | ms | 🔴 | Hardware watchdog |
| `config_check_period_s` | 10 | s | 🔴 | Re-CRC of the in-RAM config |

## 6. ESP-side settings this depends on

Not `SaftyFW` config, but changing any of them invalidates a default above.
**Cross-repository coupling, so it is written down rather than assumed.**

| `KilnFW` constant | Value | Source | What breaks if it changes |
|---|---|---|---|
| `HEATER_WINDOW_MS` | 60000 | `profile_executor.c:27` | **`correlation_window_s` must stay ≥ 2× this.** The single most important cross-dependency in the system |
| `HEATER_MIN_ON_MS` / `MIN_OFF_MS` | 2000 | `profile_executor.c:28-29` | Minimum conduction burst the current channels must resolve |
| `PROFILE_EXECUTOR_TICK_MS` | 1000 | `profile_executor.h:174` | Context freshness granularity |
| `CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS` | 500 | `Kconfig:222` | Context arrival rate; `context_max_age_s` is 10× it |
| `CONFIG_KILNCTL_SAFETY_BAUD_RATE` | see `Kconfig:349` for the committed value | `Kconfig:349` | Must match `SaftyFW`'s `UART_OWNER_BAUD_RATE` exactly — no negotiation. Historically capped at 9600 by the TCMT1109 optocoupler pair (115200 and 57600 delivered zero frames, ever; measured 2026-08-23, `docs/HARDWARE.md` §1); that pair was replaced by a non-inverting ADuM1201 digital isolator on 2026-08-25 and the baud sweep against it is now complete — see `Kconfig:349` for the result |
| `SAFETY_LINK_UP_PERIODS` | 3 | `safety_link.h:119` | The ESP's 1.5 s liveness window |

`relay_recent_mask`'s window is **transmitted in the context frame**
(`recent_window_s`) precisely so this table does not need a seventh row that
someone forgets to update.

---

## 7. Rules for the config system

**No default may be a guess dressed as a value.** Where there is no defensible
default the field ships unset and its guard ships disabled — `mains_voltage_v`
is the example, and `thermal_guard.h:67-75` is the precedent in this project.
`max_rate_c_per_min` (section 2) is a partial exception since 2026-09-05: its
*compiled record value* is a defensible, derived default (2× the fastest
RISING shipped built-in profile ramp — S8 only trips on a climbing delta, so
a fast cooling segment does not count) rather than a guess, but the guard it
feeds still ships fields_set-gated OFF like the rest of this section — see
section 2's row for the exact mechanism.

**A guard that is off must say so.** The diagnostic frame carries which guards
are active; the GUI shows it. A disabled guard that looks enabled is worse than
no guard, because someone is relying on it.

**Writes are refused while ARMED.** Both a safety rule and an RP2040 flash
constraint (`ARCHITECTURE.md` §8).

**Every write bumps `config_crc`, and the CRC is reported in telemetry.** "Which
thresholds is the safety processor actually enforcing?" must be answerable from
the GUI without trusting a separate record of what was uploaded.

**The safe fallback is not the permissive one.** On CRC failure, load the
compiled-in defaults *and* set `calibration_missing` *and* keep every
no-safe-default guard disabled — do not invent values to fill the gaps.


---

## Completion checklist

- [ ] `config_store.c` implemented: versioned, CRC'd, last flash sector
- [ ] Written via `flash_safe_execute()` with multicore lockout (`ARCHITECTURE.md` §8)
- [ ] Writes **refused while ARMED**
- [ ] CRC failure ⇒ compiled-in defaults **and** `calibration_missing` set **and** no-safe-default guards stay disabled
- [ ] `config_crc` reported in the version frame and shown in the GUI
- [ ] Periodic in-RAM re-CRC against flash (`config_check_period_s`)
- [ ] Every field in §§1–5 present, with these defaults
- [ ] **The §1 commissioning fields have no compiled-in default** (except `tc_type`, which keeps K but is still required for `calibration_missing` to clear) and their guards refuse to arm until set
- [ ] `tc_placement_mode` rejected (not silently reconciled) if it contradicts `tc_source`
- [ ] Config read-back over the link, so the GUI can display what is actually enforced
- [ ] §6's ESP-side coupling re-checked whenever `KilnFW` changes a heater or poll constant
