# Kiln Profiles Plan — up to 10 complete, swappable kiln identities

2026-09-14. Owner request, verbatim:

> "I also want a kiln selection page in the website or if you see a better
> place next to other config save backup and restore that is fine. that
> allows me save up to 10 separate complete kiln configurations that allow
> me to swap this controller between different kilns. This should allow me
> to name, add remove select from dropdown and download upload them."

Settling the scope question:

> "It must swap out everything for each kiln so that i can quickly go back
> and forth without human error in reentering things every time. Maybe
> package both processors together in each config and that way they are
> easy to swap"

Setting the failure behaviour:

> "If a config doesn't land and match on both sides then alarm and dissable
> heaters."

**Scope is decided: everything travels, in one package, spanning BOTH
processors, and any disagreement between the two processors is a fault state
that alarms and disables heaters.** This document is an implementation plan,
not a proposal. A sonnet agent should be able to implement each numbered item
in section 8 without re-deriving any decision here.

---

## 0. Read this first: most of this already exists

Anyone starting here will be tempted to build a new store. Do not. The board
already has:

| existing thing | file |
|---|---|
| `kiln_cfg_store` — 8 named slots, save / clone / apply / delete / rename, active-id tracking, boot-time restore of the active slot | `firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c` |
| its `cfg` LittleFS dual-write bridge (rev counter, file wins only on a strictly higher rev) | `firmware/KilnFW/App/drivers/persist/kiln_cfg_store_cfg_fs.c` |
| `GET /api/kiln_configs` + `POST .../save\|clone\|apply\|delete\|rename` | `firmware/KilnFW/App/drivers/http/kiln_cfg_http.c` |
| a working dropdown UI with save / clone / rename / delete / apply | `firmware/KilnFW/App/drivers/http/main_page.html`, from ~line 1313 |
| firing interlock already enforced inside `kiln_cfg_store_apply()` | `kiln_cfg_store.c:767` |
| whole-system JSON export/import (profiles + zones), two-pass validate-then-commit, heap-not-stack candidate arrays | `http/backup_export.c`, `http/backup_import.c` |
| Pico param set-and-confirm ("never trust a bare ACK") | `safety_cfg_http_set_and_confirm_f32()`, `http/safety_cfg_http.c:1043` |
| batched multi-param commit + confirm | `apply_pairs()` / `confirm_commit_landed()`, same file |
| Pico ceiling mirroring policy, apply-lower/guard-raise ordering, link-up reconcile | `safety/safety_ceiling_sync.c` |
| ESP-side cache of all 68 Pico params, plus the Pico's live `config_crc` arriving in every telemetry frame | `safety/safety_cfg_store.c`, `SAFETY_CFG_PARAM_COUNT 68` |

**What is actually missing, and is what this plan delivers:**

1. The stored blob is **ESP-only** — `zones_config_export_blob()`, i.e. one
   `zones_cfg_t`. The Pico's 68 commissioning parameters are NOT in it. A
   swap today silently leaves the previous kiln's `abs_max_temp_c`, S8 rate
   guard, thermocouple type and CT calibration on the safety processor.
   **This is the central defect this plan fixes.**
2. There is no standing check that the two processors agree, and no
   alarm-and-disable-heaters response when they do not.
3. No download / upload of a single slot.
4. `KILN_CFG_MAX_COUNT` is 8; the owner asked for 10.
5. Several ESP-side, non-`zones_cfg_t` items that are part of a kiln's
   identity are also absent (section 1.2).

---

## 1. Part A — what "a complete kiln configuration" is

### 1.1 The rule

A **kiln package** holds everything whose correct value depends on *which
physical kiln the controller is bolted to*. A **firing profile** (a recipe:
ramp to 1000 C, dwell 20 min) depends on *what you are firing*, not on which
kiln. Recipes stay where they are.

Everything the owner would otherwise retype is IN. The exception list in
section 1.4 is short and each entry is justified there.

### 1.2 IN — ESP half

All of `zones_cfg_t` (`persist/zones_config_json.h`, `ZONES_CFG_VERSION 26`),
already captured verbatim by the existing blob. Named explicitly because the
owner asked for several of them by name:

| field(s) | why IN |
|---|---|
| `model_k_dc` / `model_tau_s` / `model_dead_time_s` / `model_fit_temp_c` / `model_fit_ambient_c` | the identified plant. Purely a property of this kiln's mass and element coupling. Re-deriving means a multi-hour autotune per zone. |
| `pid_kp` / `pid_ki` / `pid_kd` | derived from that plant. Wrong gains on a different kiln oscillate or under-track. |
| `autotune_baseline_k_dc` | the reference gain adaptive tuning ratchets against; kiln-specific by construction. |
| `error_band_c` / `rate_band_c_per_s` / `fuzzy_strength_pct` | fuzzy membership half-widths, scaled to this kiln's error magnitudes. |
| `adaptive_tune_enabled` | a per-kiln operator policy. |
| `coupling_coeff[]` / `coupling_tau_s[]` / `coupling_dead_time_s[]` / `coupling_diag_k_dc` | the coupling matrix — the single most expensive thing in the box to re-measure (three settled runs). Utterly kiln-specific: it encodes the physical zone stacking. |
| `coil_power_w` | element rating. Different kiln, different elements. |
| zone `name`, `max_temp_c`, `min_temp_c` | identity and the per-zone ceiling. |
| `zone_type`, `hyst_c`, `min_on_s`, `min_off_s`, `failsafe_state`, `relay_type` | on/off vs PID zone types and their switching constraints — wiring-dependent. |
| `relay_mask`, `thermo_mask`, `ct_mask`, `relay_count`, `thermo_count`, `max_simultaneous_relays` | the physical wiring map. Wrong here means firing the wrong element. |
| `tc_type`, `cal_offset_c` | thermocouple hardware and its calibration. |
| every `guard_*` field, `cross_zone_max_delta_c`, `sanity_rate_c_per_min`, `progress_band_c` | ESP-side guard thresholds, tuned to this kiln's thermal rates. |
| `heater_window_ms` / `heater_min_on_ms` / `heater_min_off_ms` | PWM window and relay-life constraints for this kiln's contactors. |
| `max_ramp_c_per_hr`, `approach_rate_cap_c_per_hr`, `ease_off_window_mult` | per-kiln rate policy. |
| `timing_profiles[]` + `timing_profile_count` + each zone's `timing_profile` | named guard-threshold sets, referenced by INDEX from the zone — so they must travel with it, or the index silently means something different after a swap. |
| `settings_source[]` | provenance of each group (autotuned vs hand-entered). Travels so provenance is not silently laundered by a swap. |
| `continue_on_zone_trip`, `pc_link_abort_silence_ms`, `safety_tc_type` | board-wide policy an operator sets per kiln. |

Plus, ESP-side but **outside** `zones_cfg_t`, and therefore new work:

| item | where it lives today |
|---|---|
| ramp-assist configuration | `control/ramp_assist_cfg.c` |
| CT calibration *input* form (`a_fs`, `zero_mv`) and rate-guard metadata (source + value) | `safety/safety_cfg_store.c` — ESP NVS, not a Pico param. Without it the Pico's derived `k_ct_v_per_a` cannot be explained or re-edited after a swap. |
| safety relay type | `safety_cfg_store_get_safety_relay_type()` |

### 1.3 IN — Pico half

**All 68 parameters** in `firmware/SaftyFW/src/config_params.c`'s
`CONFIG_PARAM_TABLE`. Taking "everything" literally and transporting the whole
table is *safer* than curating a subset: a curated list silently leaves stale
values behind, which is exactly the class of defect that cost three days of
plant models (`137dea1a`). The table is machine-enumerable
(`config_params_count()` / `_id_at()`, mirrored on the ESP by
`safety_cfg_store_param_count()` / `_get_by_index()`), so a package built by
walking it can never fall behind a newly minted param id the way a
hand-written field list would.

The groups, for the record:

- `0x0101`–`0x010A` — sensing: `tc_source`, `borrowed_zone_index`,
  `tc_placement_mode`, **`abs_max_temp_c`**, `tc_type`, `ct_channel_map[3]`,
  `ct_installed`, `tc_offset_c`.
- `0x0201`–`0x0212` — guards: firing/overshoot margins, **`max_rate_c_per_min`
  and `rate_window_s` (the S8 rate guard)**, blind/frozen windows, TC
  disagreement, cold-junction limits, borrowed-TC staleness,
  `safety_tc_installed`, `estop_active_level`.
- `0x0301`–`0x031F` — current: `i_present_a`, zero counts,
  correlation/stuck-on/trip-verify windows, **`k_ct_v_per_a[3]`,
  `ct_cal[3].gain/.offset/.calibrated`**, `gain[3]`, `mains_voltage_v`,
  `power_window_s`, `max_expected_power_w`, `i_normal_a[3]`,
  `overcurrent_pct`/`_time_s`, `ct_topology`.
- `0x0401`–`0x0405` — link timing.
- `0x0501`–`0x0504` — startup grace, e-stop debounce, watchdog, config-check
  period.

### 1.4 OUT — the complete, deliberate exception list

Nothing here is "probably fine to omit"; each has a reason an operator would
accept. If something is left out and not listed here, the user discovers it by
a kiln behaving wrongly after a swap — so this list is the contract.

| item | why OUT |
|---|---|
| **Firing profiles / schedules** (`profiles_nvs`) | recipes, not kiln identity. A cone-6 glaze schedule is the same schedule on any kiln. Already independently exportable via `/api/backup` and `profiles_export_http.c`. Including them would mean swapping kilns silently replaced the operator's recipe library — a worse surprise than any omission on this list. |
| **Wi-Fi SSID / password / static IP** (`wifi_nvs`) | a property of the *building*, not the kiln. Also: packages are downloadable files handed between controllers — a credential in one is a credential leaked. The strongest OUT on this list. |
| **OTA state, `otadata`, boot_guard counters** | firmware-lifecycle state of *this controller*. Restoring another controller's boot-guard count could push a healthy board into recovery mode. |
| **Crash reports, logs, firing statistics, relay cycle counters** | history, not configuration. Relay cycle counts belong to the contactors physically installed in this controller and must never be reset or transplanted by a swap — that would falsify the relay life budget. |
| **Display/LCD preferences, brightness, units, timezone** | operator preference on this controller; identical whichever kiln is attached. (If the owner later wants them to travel they are additive keys — the schema in section 5 stays forward-compatible for exactly that.) |
| **`run_state` breadcrumb, live PID integrator state** | runtime, not config. A swap is refused mid-firing anyway (section 4.3). |
| **Board serial / MAC / partition table / hardware-revision strings** | properties of the controller, not the kiln. A package carrying and applying them would be nonsense. |

The user-facing text on the kiln page must state this in one sentence:
*"A kiln package carries everything about the kiln — wiring, sensors, tuning,
guards and the safety processor's limits. It does not carry firing schedules,
Wi-Fi settings, or this controller's own logs and counters."*

---

## 1a. REVISION 2026-09-14: one flash home, both processors run from RAM

Owner, verbatim: *"The configs should exist in one of the esp flash zones and
be run out of ram on both processors"*.

This supersedes the storage model below wherever they conflict. **Section 2's
storage arithmetic is restated in 1a.7; sections 1 (what is IN), 3
(divergence), 5 (upload), 6 (missing-field rule) and 7 (surface) are
unchanged.**

### 1a.1 The good news: the Pico already runs from RAM

This is not a change to the Pico's runtime architecture — it is already the
architecture, and that is what makes this request cheap rather than a rewrite.

- `config_store_flash.c:181` holds `static config_store_record_t s_cached_record`
  — the **live RAM record**.
- Every guard reads it through `config_store_seqlock_read()`
  (`config_store_flash.c:408`), a seqlock with a writer-owned fallback double
  buffer; `safety_core.c:1010` calls `safety_core_load_guard_cfg(&cfg_rec)` on
  a **seqlock snapshot**, never on flash.
- Flash is purely the *persistence* behind that RAM record: loaded once at
  boot, rewritten on a commissioning commit.
- `link_task.c` already has `s_staged_config`, a RAM staging buffer that
  `SET_PARAM` accumulates into and `COMMIT_CONFIG` installs.

So "run out of RAM on both processors" describes what already happens. **The
owner's change is about persistence, not runtime**: the Pico stops being a
second persistent home for profile config.

### 1a.2 The bounded swap window is NOT needed — drop it

I flagged, and the owner approved, an explicitly-marked bounded window in which
the Pico is disarmed so config writes are accepted. **That approval should not
be spent. The window is unnecessary under this model and must not be built.**

The reason is precise and checkable in the code:
`config_store_decide_write(relay_owner_get_state() == RELAY_OWNER_STATE_ARMED)`
is called from inside **`config_store_write()`**
(`firmware/SaftyFW/src/config_store_flash.c:978-980`) — the function that
performs the **flash** write. It is not a gate on receiving, staging, or
installing config; it is a gate on persisting it.

Therefore: **a push that installs into `s_cached_record` and does not touch
flash never reaches that refusal, and the Pico never has to leave ARMED.** That
is strictly better than the approved window — the window's entire cost was a
period in which the Pico was not armed, which is exactly what the owner's rule
wants never to exist. Building an approved-but-unnecessary disarm mechanism
would be adding the hazard back for no benefit.

**Consequences for section 4:** section 4.1's contradiction dissolves (keep the
section as the record of *why* no disarm is needed); section 4.2's steps 3, 5
and 11 — disable heaters, disarm, re-arm — are **deleted**; item 5 is
**unblocked**. Section 4.2's ordering (Pico first, read back, then ESP), the
pending-swap marker, the rollback, and section 4.4's boot recovery all stand
unchanged: they protect against a dropped push and a crash mid-transaction,
which a RAM push does not make impossible. Refusing a swap during a firing
(4.3) also stands — swapping a live kiln's tuning and guard thresholds out from
under a running firing is wrong whether or not flash is involved.

### 1a.3 The new wire operation

One addition to kilnlink: **install the staged config into RAM without writing
flash**. Either a flag on `COMMIT_CONFIG` or a sibling command
(`APPLY_CONFIG_VOLATILE`); the flag is preferred because it reuses
`COMMIT_CONFIG`'s existing staging, validation, rejection-reply and
`current_task_reload_cal()` plumbing verbatim.

Requirements on it:

- It performs the **same** `config_params_set()` validation as today. A
  volatile install is not a less-checked install.
- It updates `s_cached_record` through `config_store_seqlock_write()` — never
  by plain assignment. The seqlock exists because the trip path reads this
  record concurrently (`config_store_flash.c:183-198`); bypassing it would hand
  the trip path a torn record, which is precisely the defect the seqlock and
  today's atomicity fix (`98d237b0`) exist to prevent. **This is the one place
  a careless implementation would do real damage.**
- It **bumps `config_version`/`config_crc`** exactly as a persisted commit
  does. Section 3.1's identity check depends on the Pico's own CRC reflecting
  its live record; a volatile install that left the CRC stale would make the
  Pico report the *old* config's identity while running the new one — a token
  that lies, which is the one thing section 3.1 exists to prevent.
- It **requires no disarm**, and must be commented at the call site as
  deliberately bypassing `config_store_write()` and therefore
  `config_store_decide_write()`. A reader who does not know why will "fix" it.

**The one genuine conflict with the code, named rather than forced:**
`config_crc` is today computed by `config_store_record_crc()` over the *packed
record* — the thing written to flash. Under a volatile install nothing is
written, so the CRC must be computed over the packed form of the **in-RAM**
record. That is the same function on the same bytes, not a new hash, but the
code currently only calls it on the write path. **Cost: one small refactor**,
and `test/test_config_store.c` already covers `config_store_record_crc()`
directly, so it is testable at the host level. No other conflict was found —
the seqlock, the commissioning flow and the way `config_store` feeds the guards
all accommodate this without change.

### 1a.4 What still persists on the Pico, and why `config_store` stays

`config_store`'s flash machinery **stays**, reduced in role but not vestigial.
It keeps exactly one job:

> **Bring-up fallback**: hold the last known-good record so a Pico that boots
> before the ESP has pushed is not running on nothing.

It is no longer written by a profile apply as the authoritative act. It is
written only by:

1. **A confirmed profile apply**, *after* the volatile install has been read
   back and verified — a deliberate "persist what is now proven live", written
   opportunistically and **allowed to fail**. If the Pico is ARMED at that
   moment the flash write is refused exactly as today; that refusal is logged
   as `fallback_not_persisted` and is **not** a swap failure, because the RAM
   config is already correct and verified. This is the only place the ARMED
   refusal still appears, and it is now harmless.
2. **Commissioning data that is genuinely a property of the safety board rather
   than the kiln** — see the end of this section.

**The fallback must not silently diverge from the active profile**, which is
the obvious trap in giving it a second life. Three rules close it:

- The persisted record carries the **`pkg_hash` of the profile it was persisted
  from**. On boot the Pico reports that alongside `config_version`/`config_crc`.
- At link-up the ESP compares the Pico's reported `pkg_hash` against the active
  profile's. A mismatch is **not** an alarm — it means the Pico is running a
  stale fallback — it triggers an **immediate push** of the active profile,
  after which the normal identity check of section 3 applies.
- If the fallback could not be persisted (case 1's ARMED refusal), the ESP
  records that and knows a reboot will come up stale. That is fine: it
  re-pushes.

**What remains genuinely Pico-owned and does not travel in a profile:** nothing
in `CONFIG_PARAM_TABLE`, as far as this review can tell — every one of the 68 is
either a kiln property or a property of the CT/TC hardware, and section 5.3
already handles the latter by refusing or invalidating a foreign calibration. If
an implementer finds a param that is genuinely a property of *this safety board*
(a board-specific ADC trim, say), it must be **named explicitly in this section
and excluded from the profile**, not left to be discovered — the same discipline
as section 1.4's exception list.

### 1a.5 The unconfigured state, and boot ordering

**This is now the critical path, and it is where a mistake is dangerous.**

`safety_core_load_guard_cfg()` (`safety_core.c:333-334`) reads:

```c
s_guard_cfg.abs_max_temp_c =
    (rec->fields_set & CONFIG_STORE_SET_ABS_MAX_TEMP_C) ? rec->abs_max_temp_c : 0.0f;
```

So an **unconfigured Pico has `abs_max_temp_c = 0.0f`, meaning S1 never
trips.** Unconfigured is a **missed-trip** state, not a fail-safe one. That is
deliberate and correct under `CONFIG_REFERENCE.md` section 7's "no default may
be a guess dressed as a value" rule — but it means the safety of this whole
model rests on one thing:

> **UNCONFIGURED must never be ARMED.** A Pico with no profile installed — and,
> with no persisted fallback, a fresh one has none — must not enter
> `RELAY_OWNER_STATE_ARMED`, must hold the safety relay de-energized, and must
> report itself unconfigured. This is the direct expression of both owner
> rules: *"there should never be a way that the pico is not armed"* becomes
> "an unarmed Pico is a fault state that alarms and disables heaters", and
> *"if a config doesn't land and match on both sides then alarm and disable
> heaters"* covers the case where the push never lands.

| Pico state | condition | arming | what the ESP sees |
|---|---|---|---|
| `UNCONFIGURED` | no record in RAM: no push received this boot, and no valid flash fallback | **never arms**; relay de-energized | `config_crc == 0` (the existing documented "never commissioned" sentinel, `kilnlink_fw_version.h:77`) plus a new explicit unconfigured flag |
| `FALLBACK` | running a persisted record whose `pkg_hash` differs from the ESP's active profile | arms — it has a real, complete, CRC-valid config | mismatched `pkg_hash` → ESP pushes immediately |
| `CONFIGURED` | running a pushed profile, verified by read-back | arms | identity matches |

**How the ESP notices a restarted Pico: `boot_id`, which already works.**
`safety_link_frames.c` already parses the peer's `boot_id`, and its own comment
(around lines 210–239) states the contract: the announce is *"re-sent whenever
the Pico's boot_id changes"*. The `boot_id_changed` block is already the place
where the 2026-08-27 audit cleared `trip_last_seq` for exactly this class of
reason. **Hook the re-push there** — one existing, already-correct detector,
not a new one (a second detector would be a sixth instance of
`project_reset_one_side_bug_class`).

Re-push is triggered by any of: a `boot_id` change, the unconfigured flag, a
`pkg_hash` mismatch, or a `config_crc` mismatch against the recorded reference.
All four converge on the same action — push the active profile, verify by
read-back, record the new identity.

### 1a.6 Link down at ESP boot, or dropped mid-push

- **Pico configured, link drops**: the Pico holds its RAM config across the
  outage and keeps guarding correctly. Loss of the link itself is already
  S6b/`SAFETY_TRIP_LINK_DEAD`'s job; this feature adds nothing there. On
  link-up the identity check runs and confirms, or re-pushes.
- **Fresh/unconfigured Pico with a dead link**: it can never be configured.
  **This must be a loud, safe state, not a wait.** The Pico stays
  `UNCONFIGURED` and never arms. The ESP, seeing no link, raises
  `CONFIG_DIVERGENCE` (an unarmed Pico is a divergence by section 3.5),
  disables heaters, and reports *"safety processor is unconfigured and
  unreachable — the kiln cannot fire"*. **No bounded wait and no
  retry-forever spinner**: a timeout that silently expires into "proceed"
  would be the worst outcome this feature could produce. It stays latched
  until a push lands and verifies.
- **Push drops mid-transfer**: staging accumulates but the install is one
  seqlock write, so a dropped push leaves the Pico on its previous RAM record,
  never a blend. The read-back at transaction step 7 fails and the transaction
  rolls back or retries — the same failure `confirm_commit_landed()` ("never
  trust a bare ACK") already handles.
- **ESP boots, Pico already running and configured from a previous session**:
  the `pkg_hash`/`config_crc` check confirms or re-pushes. Neither side needs a
  reboot to reconcile.

### 1a.7 Which ESP flash zone, and the revised arithmetic

**`kiln_nvs`, with the `cfg` LittleFS dual-write — unchanged from section
2.2**, with the Pico's half riding in the same `kiln_cfg_entry_t`. The zone
choice is unchanged because the arithmetic still fits comfortably, and changing
it would mean touching the boot path for no gain.

Section 2.3 already sized the entry to include the Pico's half (640 B
`kiln_pkg_safety_t` for the 68 params plus the ESP extras), so the totals barely
move. Restated with this revision's one addition — each entry now also records
the Pico identity it was last verified at (`config_version` 1 B + `config_crc`
2 B + a persisted-fallback flag 1 B, padded to 4 B):

```
entry  = 928 (ESP half) + 640 (Pico half) + 4 (pkg_hash) + 2 (pkg_schema)
       + 4 (recorded Pico identity)                     = 1578 B -> 1580 B
10 slots                                                = 15 800 B
+ header + pending-swap record (one rollback package)   ~  1 700 B
kiln_cfg_store_blob_t                                   ~ 17 500 B
```

`kiln_nvs` = `0x10000` = 65 536 B, ~56 KB usable after NVS overhead →
**~3x headroom**. `cfg` = 512 KiB; the file is ~17.5 KB, ~24 KB across 4096 B
blocks, ~48 KB peak during a copy-on-write commit → **9.4 % of the
partition**. Both pass. **No partition is added, moved or resized.**

Why not elsewhere, briefly: `nvs` (`0x6000`) is far too small and holds
pre-existing live data; `profiles_nvs` belongs to the recipe library and mixing
kiln identity into it would couple two things section 7.1 deliberately keeps
apart; a new partition would mean editing `partitions.csv` above the
append-only line, the one change this repo has consistently refused to make.

### 1a.8 What remains of the divergence check

Narrower, and still worth having. With one persistent store, two flash images
can no longer disagree — that entire class of cause is gone. What remains:

> **Does the Pico's live RAM state match what the ESP believes it pushed?**

Still a real question with real failure modes: a push that was ACKed but
dropped, a Pico that rebooted and came up unconfigured or on a stale fallback, a
corrupted RAM record, a seqlock bug handing the guards a torn snapshot. The
mechanism is unchanged and is section 3's: the Pico's own `config_version` +
`config_crc`, computed over its own live record, compared against the pair the
ESP recorded at the last verified read-back. **Each side still hashes its own
live state; nothing is echoed.** The response is unchanged: alarm, disable
heaters, name the differing fields, clear only on a verified match.

Nothing is removed from section 3's *mechanism*. What is removed is a category
of *causes* — two independently written flash stores drifting apart — which is
a real simplification even though the check that would have caught them stays.

---

## 2. Part B — storage, and the capacity arithmetic

> **Superseded in part by section 1a.** Section 2.2's store and 2.3's
> arithmetic stand (1a.7 restates them with the small identity addition);
> section 2.1's reasoning is strengthened rather than changed — the active
> config still stays where it is, and the Pico is now definitively not a second
> persistent authority.

### 2.1 Decision: the active configuration stays exactly where it is

**The active configuration remains authoritative where it is today**: the live
`zones_cfg_t` in `kiln_nvs` (dual-written to `cfg`), and the live Pico config
in the Pico's own flash config store. Packages are *stored copies*.

This is not merely the cheap option, it is the correct one:

- The boot path does not change. `zones_http_start()` still loads NVS; the
  Pico still loads its own store. A package store that failed to mount could
  not strand the board.
- `kiln_cfg_store_init()`'s existing boot-time restore of the active slot
  already runs *after* `zones_http_start()`, so "apply nothing" means "keep
  what already loaded" — never a half-state. **Preserve that ordering.**
- The Pico half genuinely cannot be centralised: the Pico owns its own flash
  and refuses writes while armed (section 4.1). Any design that made a package
  the authority would have to re-push 68 params on every boot, which would mean
  disarming the Pico on every boot. Unacceptable.

Anything more invasive is recommended against.

### 2.2 Where packages live

Extend the existing store rather than adding a second one. Keep the existing
NVS + `cfg` dual-write shape (`kiln_cfg_store_cfg_fs.c`): NVS authoritative,
the `cfg` file as the rev-counted mirror, file wins only on a strictly higher
rev.

### 2.3 Arithmetic

Today, per slot (`kiln_cfg_store_internal.h`):

```
kiln_cfg_entry_t = in_use(1) + id(4) + name(24) + blob_len(2) + blob(896)
                 = 927 B, padded to 928 B
```

`ZONES_CONFIG_BLOB_MAX_SIZE` is 896 (`zones_config_accessors.h:1338`).

Adding the Pico half. Store it as a fixed array of 68 entries of
`{u16 param_id, u8 type, u8 flags, u32 value_bits}` = 8 B each = **544 B**,
plus the ESP-side extras (CT cal inputs 3 × 2 floats = 24 B, rate-guard meta
8 B, safety relay type 1 B, ramp-assist ~32 B) ≈ **616 B**; round to a fixed
640 B `kiln_pkg_safety_t`.

```
new entry = 928 + 640 + 4 (package CRC32) + 2 (package schema version)
          ≈ 1576 B, padded to 1580 B

10 slots  = 15 800 B
+ header (version 1 + active_id 4 + next_id 4 + pending-swap record) ≈ 1 700 B
kiln_cfg_store_blob_t ≈ 17 500 B
```

(The pending-swap record of section 4.4 holds one full rollback package, hence
its ~1.6 KB.)

**NVS**: `kiln_nvs` is `0x10000` = 65 536 B (`firmware/KilnFW/partitions.csv`).
NVS overhead is roughly 6–8 % plus page-management reserve, so usable is
~56 KB. One 17.5 KB blob fits with ~3x headroom, and NVS's single-blob ceiling
(508 000 B since IDF v4) is nowhere near. **Passes.** Today's 8-slot blob is
already ~7.4 KB, so this is a 2.4x growth of something already proven to fit.
**No new NVS key is added** — the store uses one blob key, well inside the
15-character limit (`project_nvs_key_too_long_zone_normals`).

**`cfg` LittleFS**: 512 KiB at `0xDB0000` (`234ce9f3`). The file is
`4 + sizeof(kiln_cfg_store_blob_t)` ≈ 17.5 KB. LittleFS block size is 4096 B,
so the file occupies 5 data blocks plus metadata ≈ 24 KB; LittleFS is
copy-on-write, so peak transient usage during a commit is ~48 KB — **9.4 % of
the partition**, shared with `zones_config.json`, `profiles/` and
`ramp_assist`. Comfortable.

**Wear and append-only.** `cfg` is append-only *at the partition-table level*
(the row was appended without moving any existing partition). LittleFS itself
is copy-on-write with wear levelling across all 128 blocks, so rewriting a
24 KB file does not repeatedly erase the same block. A kiln swap is an operator
action measured per *month*, not per minute; even at 100 writes/day the
endurance margin remains enormous (`project_flash_endurance_is_not_the_problem`
measured 1720 years of margin on far more frequently written stores). **Wear is
not a design constraint here.**

**When it fills.** Two distinct cases that must behave differently:

- *Store full* (10 slots in use): `find_free_slot()` already returns −1 and
  `kiln_cfg_store_clone()` already answers `"kiln config store is full"`.
  Extend the same refusal to upload. **Never evict, never implicitly overwrite
  an existing slot.** The UI must say which slot to delete.
- *`cfg` filesystem write fails* (partition full, unmounted, or corrupt): the
  existing dual-write already treats this as non-fatal — NVS is authoritative
  and the failure is logged, not raised. Keep that. But **the apply path must
  not treat an NVS write failure the same way**: a failed NVS write means the
  package would not survive a reboot and is a hard failure; a failed `cfg`
  write is a logged warning. That asymmetry is deliberate and must be
  commented at the call site.

### 2.4 Auto-save: user changes flow into the active package

Owner: *"When the user makes changes to the current config it should be
automatically saved to the current configuration and the hash recalculated."*

So the active package is not a stale snapshot taken at the last manual Save —
it tracks the live config. The dropdown's "Save" button becomes a way to name
or re-point, not the only way state is preserved.

**Mechanism.** One function, `kiln_package_autosave_active()`, called from a
single debounced task. Every existing writer of live config already funnels
through `zones_config_set_*()` / the commissioning POST handlers; each of those
paths raises one dirty flag. Nothing else writes the package.

**Rules, each of which exists to stop a specific failure:**

1. **Only when a package is active.** `active_id == KILN_CFG_NO_ACTIVE_ID` →
   no auto-save, and the UI says "unsaved — no kiln package selected" with a
   Save-as prompt. Silently inventing a package would be worse than saying so.
2. **Complete, never partial.** Auto-save serializes the config by the same
   whole-struct builder used for a manual save (section 6's "walks the struct,
   never a field list"). It never persists "the fields this request touched".
3. **Read-back verified, and the hash follows the verification, not the
   write.** Persist, then read the package back and compare field-for-field
   against what was intended. **Only on a verified match is `pkg_hash`
   recalculated and stored.** A write that cannot be verified leaves the
   previous hash in place and raises a visible `autosave_failed` warning — it
   does **not** stamp a fresh hash over unverified bytes.
4. **Debounce 5 s of quiet, hard-flush at 60 s.** A slider drag or a typed
   field produces one write 5 seconds after the last change; a continuous
   stream of changes still commits at least once a minute so a power cut loses
   at most 60 s of edits.
5. **Never during the apply transaction** (pending-swap marker set) and never
   during a firing tick that would contend the config lock — the dirty flag
   simply persists until the next opportunity.
6. **Suppressed while `CONFIG_DIVERGENCE` is latched.** Writing a package hash
   while the two processors are known to disagree would launder the divergence
   into a "consistent" saved state. This is the most important of these rules.

**Write rate, stated rather than hand-waved.** Worst realistic case: an
operator actively editing the commissioning pages for an hour produces at most
60 writes (the 60 s hard flush) and realistically far fewer (5 s debounce, and
human edit bursts are seconds apart but separated by minutes of thought). A
normal day is single-digit writes. Each write is one ~24 KB LittleFS
copy-on-write commit plus one NVS blob write. Against the measured margin in
`project_flash_endurance_is_not_the_problem` (100x–1000x headroom, ~1720 years
on stores written far more often), this is **sanity engineering, not a wear
risk** — the debounce exists so that dragging a slider does not produce a write
per pixel, not because the flash is fragile.

**The blast radius this rule closes.** The plant-model loss (`137dea1a`,
`docs/audits/plant_model_loss_investigation_2026-09-14.md`) was a whole-object
`POST /api/zones` that omitted the model keys and **deleted** the plant models,
unnoticed for three days. With auto-save present, that same POST would
immediately write the loss into the saved package **and recalculate the hash so
it looked perfectly consistent** — destroying the last copy and erasing the
evidence in one step. Rules 2 and 3 are what stop it: auto-save persists a
complete config or nothing, and an unverified write never updates the hash.
Item 12's negative test encodes exactly this scenario.

---

## 3. Part C.0 — the standing invariant: the two processors must agree

This is the owner's failure rule, and it is **broader than a swap-rollback
rule**. It is a continuously enforced invariant, not a step at the end of an
apply.

> **INVARIANT.** At any moment the ESP's committed configuration and the Pico's
> live configuration must correspond. If they do not, the system **raises a
> latched CONFIG DIVERGENCE alarm and disables heaters**, names the specific
> fields that differ and their value on each side, and refuses to start or
> continue a firing until the configurations are made to match and that match
> is confirmed by read-back.

It covers every route into disagreement, not just a failed swap:

- a swap that failed part-way;
- a swap interrupted by power loss and resumed at boot;
- a manual edit that reached only one side (a `POST /api/zones` that raised a
  ceiling while the Pico push failed);
- drift discovered during ordinary running (a Pico that rebooted and came up
  on an older config record);
- **an unarmed Pico**, which is a divergence by definition (section 3.5).

### 3.1 Identity = format version + hash, and the machinery already exists

Owner: *"Matching config meens format version number and a hash that identifys
the set."* Two configs match when **both** their format version and their hash
match. There is no standing field-by-field comparator.

**The good news, found in the code rather than designed: the Pico already does
exactly this.** `kilnlink_fw_version.h:77` — the FW_VERSION frame carries
`config_version` (u8) **and** `config_crc` (u16), described as *"CRC of the
Pico's active threshold/calibration set; 0 = never commissioned"*. It is
computed by `config_store_record_crc()`
(`firmware/SaftyFW/src/config_store.c:1126`) as a `bootloader_crc32()` over the
Pico's own packed 512-byte record, i.e. **over the Pico's real live bytes**.
`safety_cfg_store.c` already tracks it (`cached_config_crc` vs
`peer_config_crc`) and `safety_cfg_store_maybe_refetch()` already acts on a
change.

#### 3.1.1 Two distinct hashes, and why

| hash | computed by | over what | used for |
|---|---|---|---|
| **package hash** `pkg_hash` (CRC-32) with `pkg_schema` | the ESP | the canonical serialization of the whole package, both halves | package identity: which kiln this is, whether an uploaded file is intact, whether the active config still equals the saved profile |
| **Pico config identity** `(config_version, config_crc)` | **the Pico, over its own record** | the Pico's live config bytes | the cross-processor match check |

**The Pico's hash is never pushed to it.** The ESP does not compute what the
Pico's CRC "should be" by reproducing `config_store_pack()` — that would be a
mirror of the Pico's packing layout, in the ESP, and mirrors drift
(`project_negative_test_on_a_mirror_is_vacuous`); worse, a mirror bug would
produce a *spurious* mismatch that disables heaters, which is precisely the
failure that gets a safety check switched off. Instead:

> At transaction step 7 the ESP has just written all 68 params and **verified
> them by paging the Pico's config back and comparing field by field**. At that
> exact moment — and only then — it records the `(config_version, config_crc)`
> the Pico reports, into the active package. That recorded pair is a
> *observation of verified state*, not a token the ESP invented.
>
> Thereafter the standing check is: **does the Pico's live
> `(config_version, config_crc)` still equal the pair recorded for the active
> package?** Both numbers come from the Pico, computed over the Pico's own
> bytes, every FW_VERSION/telemetry frame. A Pico whose values drifted, or that
> rebooted into an older flash record, reports a different CRC and is caught.
> A token that cannot lie is the whole point, and this one cannot.

The one field-by-field comparison in the whole design is the **apply-time
read-back** that establishes the reference (step 7), plus the **post-trip
report** (3.1.4). Neither is a standing comparator, and neither can go stale,
because both **walk `CONFIG_PARAM_TABLE`** rather than a hand-written field
list.

#### 3.1.2 The format version gates the hash comparison

Comparing hashes across format versions is meaningless — the same values packed
under a different record layout produce a different CRC.

- **`config_version` differs** (Pico live vs recorded): this is **not** treated
  as a hash mismatch. The hash comparison is skipped entirely and the ESP
  **re-establishes the reference**: page the Pico's full config, compare field
  by field against the active package's Pico half, and if every field matches,
  record the new `(config_version, config_crc)` and carry on with no alarm.
  This is the legitimate case where the Pico's firmware was updated and its
  record layout changed while the *values* are unchanged.
  If any field differs, **that** is the divergence, reported by field.
- **`config_version` is one this ESP firmware does not know how to page**
  (the `GET_CONFIG_PAGE` contract itself changed): the ESP cannot establish a
  reference, so it **cannot prove the configs match** — latch
  `CONFIG_DIVERGENCE` and disable heaters, with the reason "safety processor
  config format version N is newer than this firmware understands; update the
  main firmware". Fail safe, and the message names the actual fix.
- **`config_crc == 0`** means never commissioned (`kilnlink_fw_version.h:77`'s
  own documented sentinel). A never-commissioned Pico with an active package is
  a divergence — not a special case to wave through.

#### 3.1.3 The package hash input, defined exactly

Floats are the trap: a hash that spuriously mismatches will disable heaters and
get switched off. Rules, all mandatory:

1. **The hash is computed over the validated BINARY package, never over its
   JSON text.** JSON is a transport. On upload, the file is parsed into the
   binary package first, validated, and the hash is then recomputed *from the
   binary* and compared against the file's stated `pkg_hash`. Whitespace, key
   order and formatting therefore cannot affect it.
2. **Field order is the declaration order** of `zones_cfg_t` (then the ESP
   extras in the fixed order of section 1.2), then the Pico half **in ascending
   `param_id` order**, never in table order (the table's order is editorial and
   has already been appended to out of numeric sequence — see `0x0211`'s
   comment in `config_params.c`).
3. **Padding is never hashed.** The canonical buffer is built field by field
   into a packed byte stream, not `memcpy`'d from a struct. (A struct hash
   would make the CRC depend on compiler padding — a silent divergence across a
   toolchain change.)
4. **Float normalisation.** Each `f32` is hashed as its IEEE-754 little-endian
   bit pattern, after canonicalisation: **`-0.0` is flushed to `+0.0`**, and
   **NaN/Inf are rejected at validation** (never hashed — a NaN has many bit
   patterns and no two compare equal).
5. **JSON round-trip exactness.** Every float is serialized with `%.9g`, which
   is round-trip exact for IEEE-754 binary32. Parse then re-serialize is
   therefore bit-identical, and a package downloaded and re-uploaded hashes the
   same. **An implementation that uses `%.4f` or `%.1f` for a hashed field is a
   defect** — note `backup_export.c` currently uses `%.4f`/`%.1f`, which is
   fine for a human-readable backup but must **not** be reused for the package
   format. This is the single most likely way to ship a spuriously mismatching
   hash.
6. **Migration.** If a `pkg_schema` migration changes any field's stored value,
   the hash is recomputed after the migration and the package rewritten — a
   migrated package's hash is expected to change, and the migration path
   records that it did. A hash carried across a migration unchanged would be
   the lie.

#### 3.1.4 What the report says after a trip

A hash says *that* something differs, never *what*, and the owner also requires
the operator be told what. So on trip — and only on trip — the ESP pages the
Pico's full config (`safety_cfg_store_refetch()`, existing `GET_CONFIG_PAGE`
machinery) and walks `CONFIG_PARAM_TABLE`, naming every differing param with
both values. A param id present on the Pico but absent from the package is
itself reported ("this firmware does not know param 0x031F"), never skipped.

### 3.2 What does NOT count as a divergence

**A divergence check that is too eager becomes a nuisance that gets disabled.**
Enumerate the legitimate differences up front rather than letting the
comparison discover them in the field:

| difference | why it is not a divergence |
|---|---|
| ESP-side-only fields that have no Pico counterpart at all (PID gains, plant models, coupling matrix, timing profiles, zone names) | the Pico has no opinion on them. They are compared against the package on the ESP side only (section 4.2 step 8), never against the Pico. |
| Pico-side runtime/telemetry state (trip counters, `trip_seq`, `boot_id`, uptime, live temperatures, CT readings) | not configuration. The `config_crc` covers the config record only; do not widen it. |
| A Pico param whose value the ESP deliberately derives rather than stores, if any (`k_ct_v_per_a[]` derived from `a_fs`/`zero_mv`) | compared as the derived value, computed by the single existing converter `safety_ct_cal_convert()` — never by a second formula. If the derivation and the stored value disagree that IS a divergence, and a real one. |
| A window where the ESP has *deliberately* opened a swap (pending-swap marker set, heaters already disabled, operator watching) | suppressed for the duration of the transaction by the marker itself, and only by it. The marker is persistent, so a crash inside the window does not suppress the check at the next boot — it *triggers* the boot check of section 3.4. |
| Floating-point comparison | compare `f32` params **bit-for-bit on the stored representation**, not with an epsilon. Both sides hold the same IEEE-754 bits that travelled over the link; an epsilon would be a tolerance nobody can justify and would hide a real mis-write of a nearby value. |
| The Pico reporting `set=false` for a param the package also leaves unset | both agree it is unset. Only a set/unset *mismatch* counts. |

No other exception exists. Anything not in this table that differs is a
divergence.

### 3.3 The response: fail safe, not fail quiet

On divergence:

1. **Disable heaters immediately**, through the existing owner module
   (`kiln_io_owner`) — never by writing relays directly
   (`project_bypassed_owner_module_bug_class`: six features have made that
   mistake and produced one-directional interlocks).
2. **Latch** a `CONFIG_DIVERGENCE` fault. Latched, not momentary.
3. **Refuse to start a firing** while latched — add it to
   `capability_preflight`'s refusal set, alongside the existing unacknowledged-
   crash refusal.
4. **Stop an in-progress firing** via `profiles_stop`, and verify relays are
   off afterwards (`project_stopping_host_does_not_stop_firing`: killing the
   host does not stop a firing; the stop must be commanded and confirmed).
5. **Report what diverged**: field name, ESP value, Pico value — for up to the
   first N differing fields, with a count of any remainder.

**Message-buffer hazard, stated because this repo has already been bitten.**
`adaptive_tune.h`'s `ki_refusal_reason` is `char[96]` and a recent message
silently truncated at 162 bytes. Every operator-facing string this feature
adds must be sized against its buffer, and the host tests must **assert the
rendered message is not truncated** for the worst realistic case (the longest
param name plus two `%.4f` values). Do not reuse a 96-byte buffer for a
multi-field divergence report: the report goes in a heap-allocated JSON
response with a per-field array, and the short summary string that must fit a
fixed buffer says only `"config divergence: N field(s) differ, see /api/config_divergence"`
— sized and asserted.

### 3.4 When the check runs, and why heating can never precede it

The check must pass **before heating is ever possible**, on every boot:

- The `CONFIG_DIVERGENCE` latch is **persisted**. If it was set when the board
  went down, it is set again the moment the store loads, before any control
  task can command heat.
- At boot the latch is **set pending by default** when a pending-swap marker
  is found (section 4.4) — i.e. the fail-safe default is "diverged until
  proven otherwise", not "fine until proven diverged".
- The first affirmative clear can only come from a completed comparison, which
  requires the safety link to be up. Until then the latch stays set and
  heaters stay disabled. **A board that never brings its safety link up never
  heats** — which is already the intent of the S6a/S6b link guards, so this
  adds no new failure mode.
- Practically: hook the comparison into
  `safety_ceiling_sync_reconcile_on_link_up()`'s existing link-up path (one
  callback, not a parallel mechanism) plus the per-telemetry-frame CRC compare.

### 3.5 Composition with the ceiling rule and the unarmed-Pico case

The owner also requires that the Pico's `abs_max_temp_c` always equal the
ESP's, and that *"there should never be a way that the pico is not armed"*.

- A ceiling mismatch is, by construction, one of the 68 params and therefore
  trips the same CRC comparison. No separate ceiling watchdog is added.
- **An unarmed Pico outside a deliberate, marked swap window is a divergence**
  and trips the same alarm-and-disable path. (Inside a marked swap window it is
  expected and suppressed by the marker — and heaters are already disabled
  there.)

**Dependency, stated explicitly, so it is not duplicated.** Another agent is
implementing the always-equal ceiling mirroring and its own divergence
detection on top of `safety/safety_ceiling_sync.c` right now. This plan
**consumes** that work:

- `safety_ceiling_sync_guard_raise()`, `safety_ceiling_sync_apply_lower()`,
  `safety_ceiling_sync_get_current_pico_ceiling()` and
  `safety_ceiling_sync_reconcile_on_link_up()` are **called, never
  reimplemented**.
- If that agent's work lands a divergence-detection primitive, this feature's
  layer-1 check **must be that primitive**, generalised from the one ceiling
  param to the whole config CRC — not a second, parallel detector.
  Two detectors with two latches would be a sixth reset-one-side instance.
- **Items 5 and 7 of section 8 must not land before that work does.**

### 3.6 How the alarm clears — and how it cannot

**The latch clears only when the configurations actually match, confirmed by
read-back.** Specifically: the ESP re-reads the Pico's full config page, walks
`CONFIG_PARAM_TABLE`, finds zero differences against the active package,
records the Pico's current `config_crc` as the new reference, and only then
clears the latch and persists the clear.

**There is no operator "dismiss" and no HTTP route that clears the latch
directly.** A banner the operator can wave away is precisely how this class of
protection dies. The only operator actions available are:

- **Re-apply** the active package (runs the full transaction of section 4.2,
  which ends in the read-back that clears the latch); or
- **Apply a different package**; or
- correct the mismatched field by hand through the existing commissioning UI,
  after which the next link-up comparison clears the latch on its own.

The clear must itself be verified, not assumed —
`project_safety_calls_logging_unchecked_success` and `0b5d9dad`'s
boot_guard "write reported OK but never landed" precedent both apply: **do not
trust a write's return code here; read it back.**

---

## 4. Part C — making a swap atomic across two processors

### 4.1 The hard constraint — RESOLVED by section 1a.2, kept as the record of why

> **Status: no longer a blocker.** This section is retained because the
> constraint is real and an implementer who does not know about it will
> rediscover it the hard way. The resolution is section 1a.2: a volatile RAM
> push never reaches `config_store_write()`, so the Pico never leaves ARMED and
> no disarm window is built. The owner-approved bounded window is deliberately
> **not** spent. Everything below is the original analysis.

**The Pico refuses every config write while it is ARMED.**
`config_store_decide_write()` (`firmware/SaftyFW/src/config_store.c:1121`)
returns `CONFIG_STORE_WRITE_REFUSED_ARMED` unconditionally — no per-field
carve-out. `RELAY_OWNER_STATE_ARMED` (`src/tasks/relay_owner.h:37`) is the
*normal healthy state*: the safety relay is permitting heating.

The owner's rule is that the Pico must always be armed.

**These two statements cannot both hold while 68 parameters are being
written.** This must be resolved explicitly, not discovered during
implementation. The resolution this plan adopts:

> The rule "the Pico is always armed" means *the Pico must never be unarmed
> while heating is possible, and must never be silently unarmed*. A swap is
> refused during a firing and with the heaters commanded (section 4.3), and the
> divergence path disables heaters anyway, so during a swap heating is already
> impossible. The Pico's non-armed window during a swap is therefore in the
> **safe** direction: not-armed means the safety relay is de-energized and the
> kiln *cannot* heat. What survives of the rule is: the window must be
> **deliberate, marked, bounded, operator-visible, and must always terminate in
> a re-armed Pico whose config matches the ESP's** — including after a crash or
> power loss, where section 3.4's fail-safe default takes over.

**This interpretation must be confirmed by the owner before item 5 is
implemented.** It is the one place where a literal reading of a safety
requirement and the feature are incompatible, and guessing is not acceptable.
Items 1–4, 7 and 9–11 do not depend on the answer and can land first.

### 4.2 The apply transaction

Ordering is chosen so that every possible interruption leaves the system either
fully on the old package, fully on the new one, or **alarmed with heaters
disabled** — never quietly blended, and never with a Pico ceiling looser than
the ESP's.

```
APPLY(package P):
 0. Interlocks (section 4.3). Refuse outright on failure. Nothing written.
    Also refuse if CONFIG_DIVERGENCE is latched for a reason this apply
    would not resolve -- an apply IS the sanctioned way out of the latch,
    so this refusal is narrow: refuse only if the safety link is down.
 1. Validate P in full, in RAM: schema version, package CRC32, every ESP
    field via zones_config_json_validate(), every Pico param via the
    section 5.2 rules, plus the this-hardware checks of section 5.3.
    Refuse the WHOLE apply on the first problem. Nothing written.
 2. Snapshot the CURRENT state into a rollback package R, using the exact
    same builder that produces a saved package. Persist R into the reserved
    pending-swap record (not a user slot), with P's id and marker = STAGED.
    This record is the crash-recovery anchor of section 4.4 and the ONLY
    thing that suppresses the section 3 divergence check during the swap.
 3. [DELETED by section 1a.2 -- no heater disable / disarm is needed, because
    a volatile RAM push never reaches config_store_decide_write().]
 4. CEILING ORDER, monotonically safe:
      if P.abs_max_temp_c >= current Pico ceiling:
          raise the Pico's ceiling FIRST
      else:
          lower the Pico's ceiling LAST
    This is exactly safety_ceiling_sync_guard_raise() /
    safety_ceiling_sync_apply_lower()'s existing contract. Call them; do
    not re-derive the ordering.
 5. [DELETED by section 1a.2 -- the Pico stays ARMED throughout.]
    Marker -> PICO_OPEN (kept as the marker name: it now means "a push is in
    flight", not "the Pico is disarmed").
 6. PUSH the Pico half VOLATILE: all 68 params staged via the existing
    batched apply_pairs()/confirm_commit_landed() machinery, installed with
    section 1a.3's volatile-install flag -- RAM only, no flash write, no
    disarm. Then safety_cfg_http_set_and_confirm_f32() for abs_max_temp_c
    specifically. Do NOT invent a second write path.
 7. READ BACK the Pico's whole config page; compare all 68 values against P
    field-by-field (section 3.1 layer 2). Any mismatch -> ROLLBACK.
    Record the Pico's reported (config_version, config_crc) pair into
    the package as the new reference -- section 3.1.1. This is the ONE
    field-by-field comparison in the design; everything after is hash-only.
    Marker -> PICO_DONE.
 8. Commit the ESP half: zones_config_import_blob() (already all-or-nothing)
    plus the non-zones_cfg_t extras of section 1.2. Marker -> ESP_DONE.
 9. READ BACK the ESP half and compare field-for-field against P (section 6).
    Any mismatch -> ROLLBACK.
10. Assert the ceiling identity: Pico abs_max_temp_c == the ESP's derived
    ceiling, read from BOTH sides live, not from either side's cache.
    Mismatch -> ROLLBACK.
11. [DELETED by section 1a.2 -- the Pico never left ARMED.] Instead: confirm
    the Pico is ARMED and NOT reporting UNCONFIGURED (section 1a.5). Either
    -> alarm and disable heaters (section 3), latched.
12. Only now: active_id = P.id, clear the pending-swap record, persist,
    clear CONFIG_DIVERGENCE if it was latched. Marker -> NONE.
13. Opportunistically persist the Pico's bring-up fallback (section 1a.4
    case 1): write the now-verified record to the Pico's flash. ALLOWED TO
    FAIL -- an ARMED refusal here is logged as fallback_not_persisted and is
    NOT a swap failure. The swap is already complete at step 12.
```

**"Pico confirms and the ESP fails"** (failure at step 8 or 9): ROLLBACK
re-runs steps 5–7 with R instead of P, reads back, re-arms. The ESP was never
committed, so it is already on R. Result: fully on R, heaters re-enabled, no
alarm.

**"ESP confirms and the Pico fails"** cannot occur, because the Pico is written
and read back (steps 6–7) before the ESP is touched at all. **This ordering is
the whole reason the Pico goes first and must be commented as such at the call
site**, or a later refactor will helpfully "optimise" it.

**Rollback itself fails** (the Pico will not accept R either — link down, or
flash failure): **this is exactly the owner's rule.** Do not re-arm silently
and do not report success. Latch `CONFIG_DIVERGENCE`, disable heaters, report
the differing fields, refuse firing. A stuck-mid-swap controller that refuses
to fire and says why is acceptable; one that fires on a blended config is not.

### 4.3 Refusing during a firing

`kiln_cfg_store_apply()` already calls
`ota_http_check_interlocks(ack_no_safety_processor, ...)` **first, before
anything reads the store** (`kiln_cfg_store.c:767`) — the same predicate
`POST /api/ota/esp` gates on: is a profile running, are the heaters commanded,
is the kiln hot. Reuse verbatim; do not write a second predicate, and do not
use `heat_interlock.c` (it answers the opposite question — may heat run during
an update).

Extend the same call to the new upload-apply path. **A swap is refused, never
queued.** Queuing would mean a config change landing at an unattended moment —
precisely the surprise this feature exists to remove.

Confirm in item 5's acceptance that `ota_http_check_interlocks()` genuinely
covers the "hot but not firing" case rather than assuming it; if it does not,
add that condition there rather than duplicating the predicate here.

### 4.4 Interrupted swap: what the next boot looks like

The persistent pending-swap record from step 2 is the entire mechanism. At
`kiln_cfg_store_init()`, **after** the existing active-slot restore (preserve
that ordering), and **before** any control task can command heat:

| marker found | meaning | boot action |
|---|---|---|
| `NONE` / absent | no swap was in flight | nothing (today's behaviour). Normal section 3 checking applies once the link is up. |
| `STAGED` | crashed before the Pico was touched | discard the record; both sides are still on the old package. Log it. Section 3's check then runs normally and confirms. |
| `PICO_OPEN` | crashed while the Pico was disarmed, mid-write | the Pico's own config store is atomic per record, so it holds either old or new values, not a torn one — **but we do not know which**. Latch `CONFIG_DIVERGENCE`, heaters disabled, then attempt to re-apply R in full, read back, re-arm; on success clear the latch, on failure leave it latched. |
| `PICO_DONE` | Pico is on P, ESP is still on R | **the dangerous blend.** Latch `CONFIG_DIVERGENCE`, heaters disabled, attempt to re-apply R to the Pico, read back, re-arm, discard the record on success. **Do not "finish the swap"** — the operator's intent is not knowable after a crash, and reverting to the last fully-consistent state is the only defensible choice. |
| `ESP_DONE` | both sides are on P; only `active_id` was not yet written | complete: verify the ceiling identity and the full field comparison, and only if both pass, set `active_id` and discard. If either fails, latch. This is the one case where finishing is correct, because both halves already agree. |

In every case the board comes up **with heaters disabled and the alarm raised
until the comparison passes** — it never guesses which side is right and never
silently prefers one. That is section 3.4's fail-safe default doing its job.

**Power loss with the Pico disarmed is the worst case, and it self-heals in the
correct direction:** an unarmed Pico means the relay is de-energized and the
kiln cannot heat. On the next boot the Pico enters its own startup grace
window, the recovery above re-pushes a known-consistent config and re-arms, and
the alarm clears only once the comparison confirms. The board is inert, not
dangerous, in the interval. **This must be stated in the LCD/web banner** so an
operator who finds a dark, non-firing kiln after a power cut knows why.

---

## 5. Part D — upload is untrusted input

A package file may come from a different controller, a different firmware
version, or a text editor. Treat it exactly as `backup_import.c` treats an
imported backup: **two passes, validate everything, then commit; refuse the
whole thing on the first problem; write nothing on refusal.**

### 5.1 Envelope

```json
{ "kind": "kilnctl_kiln_package",
  "pkg_schema": 1,
  "name": "Skutt KM-1027",
  "created_utc": "2026-09-14T00:00:00Z",
  "source_board": "<opaque id, informational only>",
  "esp":  { "...every zones_cfg_t field, plus ramp_assist / ct_cal_input / rate_guard_meta..." },
  "pico": { "params": [ {"id": 260, "type": 2, "value": 1285.0} ] },
  "pkg_hash": "0x00000000" }
```

- `kind` must match exactly; anything else is rejected with "not a kiln package
  file".
- `pkg_schema` is the **package** schema, versioned independently of
  `ZONES_CFG_VERSION`. A package with a *higher* `pkg_schema` than this
  firmware knows is **rejected**, never best-effort parsed. A *lower* one is
  accepted under the additive-key rule of section 6.
- `pkg_hash` is the CRC-32 of section 3.1.3's canonical BINARY serialization
  of everything except `pkg_hash` itself — not a hash of the JSON text. **One CRC over the whole package, not one per half** — a
  half-valid package is rejected outright, per the owner's instruction.
- A CRC, not a signature: this is integrity against truncation and editing
  mistakes, not authentication. Say so in the docs; do not imply a signature's
  guarantees. An authenticating signature would require key management the
  board has no story for, and is **recommended against** for now.

### 5.1a Two distinct checks: validity, then compatibility

Owner: *"Upload should varafy config validity and compatibility."* These are
different questions and are reported differently, so they are separated
explicitly:

| check | question | failure means |
|---|---|---|
| **Validity** (5.2) | is this a well-formed, intact, in-range kiln package at all? | the file is broken or was not produced by this system. "This file is not a valid kiln package: …" |
| **Compatibility** (5.2a) | can *this controller* run this kiln package? | the file is fine but does not fit this hardware. "This package needs 3 thermocouple channels; this controller has 2." |

Both refuse the upload. Neither ever coerces. A rejected upload leaves the
active configuration **completely untouched and its `pkg_hash` unchanged** —
upload writes into a new slot only, and a refusal allocates no slot at all.

### 5.2a Compatibility with THIS controller

Compatibility is about properties of the **controller**, not the kiln. A
package from a 3-zone controller loaded onto a differently-configured one is
**refused with a reason naming what does not fit** — never silently coerced,
never truncated to the channels that happen to exist.

| property | source of truth on this controller | refusal when |
|---|---|---|
| thermocouple channel count | `MAX31856_CHANNEL_COUNT` and the live `thermo_count` | the package configures a zone on a channel index this build does not have |
| relay count / `relay_mask` bits | live `relay_count` | any `relay_mask` bit is set above `relay_count` — that bit would command a relay that does not exist |
| CT fitted, and on which channel | `ct_installed`, `ct_channel_map[]`, `ct_topology` from the Pico | the package expects a CT on a channel this controller reports as absent, **and** any guard in the package depends on it (S9/S11/S14). A CT-less controller loading a CT-configured package is refused rather than loading guards that can never fire (`project_no_cts_fitted_guard_coverage`). |
| safety thermocouple fitted | `safety_tc_installed` | the package's safety config depends on a safety TC this controller does not have |
| **kilnlink protocol version** | `KILNLINK_PROTOCOL_VERSION`, **currently 14** (`CommonFW/include/kilnlink/kilnlink_version.h:307`) | the package records a protocol version this build cannot speak to the Pico with. **Note:** the package records the protocol version as *provenance*, and the live check is against the Pico actually attached — two boards can disagree. See the caution below. |
| `pkg_schema` | this firmware's known package schema | higher than known → refused (section 5.1) |
| Pico `config_version` the package's half was captured under | the attached Pico's live `config_version` | a difference is **not** a refusal — it is handled by section 3.1.2's re-establish path, because the values may be identical under a new record layout. Refuse only if this firmware cannot page that version at all. |

**Caution on protocol version, since this is a real inconsistency in the tree
rather than a design choice:** `project_two_protocol_versions` records that
`get_fw_version` reports the UART link's own version (11) while `/api/status`
reports kilnlink's (12 at the time; the constant is **14** today) — *both
correct, different things*. An implementer who compares the wrong one will
reject every package. **Compare `KILNLINK_PROTOCOL_VERSION` only**, and name
the constant in the code comment so the next reader does not "fix" it to the
UART one.

Every compatibility refusal names the property, the package's value, and this
controller's value. A generic "incompatible" is not acceptable — see the
truncation rule in section 3.3 for how the message is sized.

### 5.2 Validity, in order

1. Envelope: `kind`, `pkg_schema`. Cheapest checks first. The `pkg_hash`
   comparison happens AFTER parsing into the binary package (section 3.1.3
   rule 1) — a hash over the text could not be checked here and must not be
   attempted.
2. Structural: both `esp` and `pico` present and non-empty. A package missing
   either half is rejected — "package both processors together" means a
   half-package is not a kiln package.
3. ESP half: every field through `zones_config_json_validate()` — the single
   source of truth for "what is a valid `zones_cfg_t`", per that module's own
   header comment. **Do not duplicate any bound.**
4. Pico half: every `{id, type, value}` through the same range rules
   `config_params_set()` enforces. That function lives on the Pico, so the
   ESP-side validator is a **mirror** — and a mirror is a liability
   (`project_negative_test_on_a_mirror_is_vacuous`). **Mitigation: the mirror
   is not the only gate.** The mirror rejects the obvious; the Pico's own
   `config_params_set()` plus the `commit_config` rejection reply is the
   authoritative gate, and a Pico rejection at step 6 of the transaction
   triggers ROLLBACK. Item 6's negative test must prove that a value the ESP
   mirror wrongly accepts is still refused by the Pico and rolls back cleanly.
5. Any **unknown param id** in the package is **rejected**, not skipped — an
   unknown id means the file was written by a firmware that knows something
   this one does not, which is the same silently-dropped-field failure this
   plan exists to prevent.

### 5.3 This-hardware checks — values valid *there* and unsafe *here*

These make a foreign package safe, and they are why a plain CRC is
insufficient:

| field | check | reason |
|---|---|---|
| `abs_max_temp_c` (Pico) | must be `>=` the maximum `max_temp_c` across all configured zones in the *same* package, and `<=` this firmware's absolute ceiling constant | the Pico ceiling must never be tighter than the ESP's — `zones_http_post.c:337`'s existing rule. Also catches a high-fire package landing on a low-fire kiln. |
| `ct_cal[].gain` / `.offset` / `k_ct_v_per_a[]` / `a_fs` / `zero_mv` | must sit inside `SAFETY_CT_CAL_A_FS_MIN/MAX` and `SAFETY_CT_CAL_ZERO_MV_MIN/MAX`, and **`calibrated` is forced to false on upload unless `source_board` matches this board** | a CT calibration is a property of the *sensor physically fitted to this controller*. Another controller's calibration is not merely wrong; it makes overcurrent guards read a fabricated current. Forcing `calibrated=false` degrades to "uncalibrated" — guards needing calibration stay dormant — rather than to "confidently wrong". **The single most important check in this table.** |
| `i_normal_a[]` (S14 normals) | forced unset whenever `calibrated` was forced false | derived from that calibration; keeping them would arm S14 against a fabricated baseline (`project_invalidating_one_side_inverted_arming` is the cautionary precedent — invalidate BOTH sides together). |
| `ct_installed`, `ct_topology`, `safety_tc_installed`, `relay_count`, `thermo_count` | if they differ from what this controller currently reports: warn loudly and require an explicit `ack_hardware_differs=1` on the apply | these describe what is physically wired to *this* box. Differing is legitimate — that is the point of swapping kilns — but must be a conscious act. |
| `estop_active_level` | accepted, but surfaced in the confirm dialog | inverting it silently would disable the e-stop. |
| every `max_temp_c` | `<=` firmware's zone ceiling bound | ordinary range check, stated because it is what stops a 1300 C package on a 1100 C kiln. |

**A rejected upload leaves the active configuration bit-for-bit untouched.**
Upload writes into a **new slot only, and never applies**. Applying is a
separate, explicit second action. That separation is deliberate: a malformed or
hostile file has no path to the live config without an operator pressing Apply
on a package the UI has already shown them, with warnings.

Partial or truncated upload: the handler accumulates the whole body before
parsing anything. A truncated body fails JSON parsing, or parses short and then
fails the `pkg_hash` comparison; either way it is discarded and **no slot is
allocated**. Same treatment as any other refusal, and the active package's own
hash is untouched.

---

## 6. Part D — the missing-field rule (the three-day defect, generalised)

`docs/audits/plant_model_loss_investigation_2026-09-14.md` / `137dea1a`: a
whole-object `POST /api/zones` that omitted the model keys **deleted** the
plant models, undetected for three days. The same shape here would wipe a
kiln's identity — with the safety processor attached.

**Rule, non-negotiable:**

- **Apply is all-or-nothing and verified by read-back.** Steps 7 and 9 of the
  transaction re-read the committed configuration on each side and compare it
  field-for-field against the package. A mismatch is a failure and a rollback,
  not a log line.
- Compare **per field, explicitly** — never `memcmp` on the struct. `memcmp`
  passes on padding differences and, worse, can pass while a field the
  serializer silently dropped happens to match the default that was already
  there.
- **A missing field in an uploaded package is a REJECTION, not a default.** Not
  "defaulted with a warning". The owner's entire purpose is that a package is
  complete; a package that cannot state a value for a field this firmware knows
  about is not a complete kiln identity, and accepting it would recreate the
  above defect with a bigger blast radius.
- **The one carve-out**: fields introduced *after* the package's declared
  `pkg_schema` are defaulted, because an older package genuinely could not have
  known about them. Each such default is emitted as a **named** warning in the
  upload response and shown in the UI before Apply is offered. This is exactly
  the additive-key mechanism `backup_import.c` already uses for its version-2
  and version-4 fields; follow that precedent.
- **No path in this feature may clear a field it was not asked to change.** The
  builder walks the struct and the param table; it never writes a
  field-by-name list that can fall behind.

---

## 7. Part E — surface

### 7.1 Decision: extend the existing kiln-config machinery; share the backup format's conventions; do NOT merge them

- **Extend** `kiln_cfg_store` / `kiln_cfg_http` / the existing
  `#kilnConfigPicker` UI. Save/clone/rename/delete/apply/active-id already
  work; this adds the Pico half, the divergence alarm, download, upload, and
  8 → 10.
- **Share** `backup_json.c`'s reader/writer helpers and `backup_export.c`'s
  chunked streaming, and reuse the same key names for every zone field so a
  human can read both files. Reuse `backup_import.c`'s two-pass
  validate-then-commit *structure*.
- **Do not merge** the two formats. A backup is "everything on this controller
  including recipes"; a kiln package is "one kiln's identity, no recipes".
  Merging would force every backup to carry a Pico half and every kiln package
  to carry the recipe library — and would make `/api/backup/import` a path into
  the safety processor, widening a surface that is currently ESP-only. Kept
  separate, with shared helpers.

### 7.2 HTTP API

Additions to the existing `/api/kiln_configs` family (existing routes
otherwise unchanged):

| route | notes |
|---|---|
| `GET /api/kiln_configs` | add `pkg_schema`, per-config `has_pico_half` and `hardware_matches`, plus top-level `divergence` (bool) and `active_name`. Summary only; never inline package contents. |
| `GET /api/kiln_configs/export?id=N` | streams one package as JSON, `Content-Disposition: attachment; filename="<name>.kilnpkg.json"`. **Streamed in chunks via `backup_stream_printf()`'s existing mechanism — never assembled in one buffer.** |
| `POST /api/kiln_configs/import` | body = package JSON. Body buffer and both candidate structures on the **heap** (`MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT`), exactly as `backup_import_post_handler()` already does. Creates a new slot; **never applies**. Returns the slot id plus the warning list. |
| `POST /api/kiln_configs/apply` | existing route, now starts the two-processor transaction on a worker task and returns immediately. Gains `ack_hardware_differs=1`. |
| `GET /api/kiln_configs/swap_status` | pending-swap marker, progress, and the outcome, for the UI to poll. |
| `GET /api/config_divergence` | the field-by-field report: `[{param, esp_value, pico_value}]`, plus `latched`, `since_ms`, and the truncation-safe summary string. Heap-built. |

**httpd stack: absolutely no stack buffer may grow.** Two panics came from
exactly that (`project_httpd_stack_blob_class`,
`project_httpd_stack_near_overflow`), and `backup_import_post_handler` already
sits at 7952 B of an 8192 B stack with 240 B free. Every buffer this feature
adds is heap or streamed. `check_httpd_task_stack_budget.py` must be run
against the resulting ELF as part of item 10's acceptance, and its measured
margin recorded in the commit message. `kiln_cfg_store_blob_t` is already
malloc'd rather than stacked (`kiln_cfg_store_cfg_fs.c:161`) — keep it so at
10 slots and the larger entry.

`/api/zones`'s ~763 B and `/api/zones_diag`'s ~268 B of headroom are **not to
be consumed by this feature at all**; no kiln-package field is added to either.
The active kiln's name goes in `GET /api/kiln_configs`, which the page already
fetches.

### 7.3 Web UI

Extend the existing kiln-config section on the main page, and add a pointer
link from the backup/restore page ("kiln packages are managed on the main
page") so the owner finds it wherever he looks.

Controls: the existing dropdown plus Save / Save as / Rename / Delete / Apply,
plus **Download** (per slot) and **Upload** (file input). Apply opens a confirm
dialog that must show, before the operator commits:

- the package's `abs_max_temp_c` and each zone's `max_temp_c`;
- every hardware-differs warning and every defaulted-field warning;
- "the safety processor will be briefly disarmed and reconfigured; the kiln
  cannot heat during the swap".

A **divergence banner**, when latched, sits above everything on the page: the
differing fields with both values, and the three resolution routes of section
3.6. **No dismiss control.**

Degrade exactly as the existing code does: a 404 or a malformed response hides
the new controls rather than rendering broken ones.

### 7.4 LCD

**Yes, the LCD must show the active kiln name.** An operator standing at a kiln
with no browser needs to know the controller thinks it is attached to *this*
kiln — that is the whole failure mode the feature prevents. Minimum viable, and
all that is proposed:

- One line on the existing status/home page: `Kiln: <name>` (or
  `Kiln: (none)`), truncated with an ellipsis to the available width.
- `CONFIG DIVERGENCE — heaters disabled` on the existing alert/banner
  mechanism, plus the mid-swap state.

Constraints honoured: 480x320 landscape, **no scrolling** — one truncated line,
not a list; **no new colours** — reuse the existing status/alert text styles
(`project_status_color_contrast_impossible`: do not attempt a repaint).

A full LCD picker (select a kiln from the panel) is **recommended against for
now**: a swap is a two-processor safety transaction with warnings the operator
must read, and a 480x320 no-scroll page cannot present section 7.3's confirm
dialog honestly. Selecting is a browser action; the LCD reports.

### 7.5 `ZONES_CFG_VERSION`

**No bump.** `zones_cfg_t` is untouched; the Pico half is added *alongside* it
inside `kiln_cfg_entry_t`, whose own `KILN_CFG_STORE_VERSION` (currently 2)
goes to 3 instead. That is a store-local migration with an established handler
(`kiln_cfg_store_cfg_fs.h`: a file at any other version is treated as absent,
not migrated in place) and a far smaller blast radius than a
`ZONES_CFG_VERSION` bump, which carries the OTA-rollback hazard documented in
`CLAUDE.md`. **No work item in section 8 may bump `ZONES_CFG_VERSION`; if one
appears to need it, stop and escalate.**

---

## 8. Part F — work items

Ordered so useful, low-risk work lands first. Items 1–4, 7 and 9–11 are
independent of the unresolved section 4.1 question. Every item: **no
`ZONES_CFG_VERSION` bump**, and every negative test that breaks a production
function must be **restored by hand**, never with `git checkout --` (this tree
is shared across sessions).

---

**Item 1 — Raise the slot count 8 → 10.**
`KILN_CFG_MAX_COUNT` 8 → 10 in `persist/kiln_cfg_store.h`; bump
`KILN_CFG_STORE_VERSION` 2 → 3. Confirm `kiln_cfg_store_blob_t` stays malloc'd,
never stacked. The UI reads `max_count` from the response already, so no UI
change.
*Acceptance:* 10 slots can be saved; the 11th is refused with "kiln config
store is full"; a v2 blob on an upgrading board is treated as absent and the
store comes up empty rather than corrupt, with a log line naming the version.
*Negative test:* stage a v2 blob in a host test and assert the store reports
not-valid and does NOT `memcpy` it into a v3 struct — break the version check
in the production function by hand, watch the test fail, restore by hand.

**Item 2 — Package builder/parser module, ESP half only.**
New `persist/kiln_package.c/.h`: serialize the live ESP-side identity
(`zones_cfg_t` + ramp-assist + CT-cal inputs + rate-guard meta + safety relay
type) to the section 5.1 envelope, and parse it back with full validation.
Uses `backup_json.c`'s helpers. Host-testable, no HTTP.
*Acceptance:* a round trip of a fully-populated config is field-for-field
identical, asserted by an explicit per-field comparison (**not `memcmp`** —
section 6). `pkg_schema` higher → reject; lower → accept with named defaults.
*Negative test:* delete `model_k_dc` from a serialized package; the parser must
REJECT it, and the test must fail if the parser instead defaults it to 0. This
is `137dea1a`'s defect, encoded.

**Item 3 — Download.** `GET /api/kiln_configs/export?id=N`, streamed.
*Acceptance:* the downloaded file parses back through item 2's parser with a
valid CRC; the response is produced with no stack buffer over 256 B.
*Negative test:* truncate the downloaded file by one byte; the parser rejects
it on CRC **before parsing any field**.

**Item 4 — Upload into a new slot (never applies).**
`POST /api/kiln_configs/import`. Heap body buffer, two-pass
validate-then-commit, the section 5.3 this-hardware checks, warning list in the
response.
*Acceptance:* a valid foreign package lands in a new slot and the live config
is bit-for-bit unchanged (assert by reading the live blob before and after);
`calibrated` is forced false and `i_normal_a[]` unset when `source_board`
differs; an upload with all 10 slots full is refused and allocates nothing.
*Negative test:* a package with an out-of-range `abs_max_temp_c`, and one with
an unknown Pico param id, are both rejected, nothing is written, and the live
config is verified unchanged afterward.

**Item 15 — Volatile (RAM-only) config install on the Pico.**
Section 1a.3: a flag on `COMMIT_CONFIG` that installs `s_staged_config` into
`s_cached_record` through `config_store_seqlock_write()` **without** touching
flash, running the same `config_params_set()` validation and bumping
`config_version`/`config_crc` over the packed in-RAM record. Includes the small
refactor that lets `config_store_record_crc()` be called off the write path.
**Item 5 depends on this.** SaftyFW host tests (`test_config_store.c`,
`test_config_page.c`) plus a `build_saftyfw_host_tests` run; remember the short
worktree path (`C:\wt\...`) — the default overflows the MSVC command line.
*Acceptance:* a volatile install changes the guards' live config (observable
via `safety_core_load_guard_cfg()`'s output) and bumps `config_crc`, while the
flash sector's byte content is unchanged; it succeeds **while ARMED**; a
subsequent reboot comes back on the older persisted fallback, proving nothing
was written.
*Negative test, two parts:* (a) make the volatile install write
`s_cached_record` by plain assignment instead of through the seqlock and assert
a concurrent reader test observes a torn record — this proves the seqlock
requirement is load-bearing, not decoration; restore by hand. (b) make the
volatile install skip the `config_crc` bump and assert the divergence check
(item 7) then reports a false match — the exact "token that lies" failure
section 3.1 exists to prevent.

**Item 16 — The UNCONFIGURED state and boot ordering.**
Section 1a.5: the Pico never arms while unconfigured; the explicit unconfigured
flag on the wire; `pkg_hash` carried in the persisted fallback and reported at
boot; re-push hooked into the **existing** `boot_id_changed` block in
`safety_link_frames.c` (not a new detector); section 1a.6's link-down
behaviour.
*Acceptance:* a Pico booted with an erased config sector never enters
`RELAY_OWNER_STATE_ARMED` and reports `config_crc == 0` plus the unconfigured
flag; the ESP pushes within one link-up cycle of a `boot_id` change; a Pico on a
stale fallback (`pkg_hash` mismatch) is re-pushed **without** raising an alarm;
an unconfigured Pico with no link leaves `CONFIG_DIVERGENCE` latched and heaters
disabled indefinitely, with the message naming "unconfigured and unreachable".
*Negative test:* force the unconfigured path to arm anyway and assert a test
catches that S1's ceiling is then `0.0f` — i.e. that an armed unconfigured Pico
is a missed-trip state, which is the whole reason for this item. Restore by
hand. Also assert the no-link case never times out into "proceed" after any
duration.

**Item 5 — The two-processor apply transaction.**
**Unblocked** (section 1a.2 dissolved the 4.1 constraint). **Depends on item 15
and on the ceiling-mirroring agent's work landing.** Implements section 4.2
**as revised by section 1a** — no heater disable, no disarm, no re-arm; steps
3, 5 and 11 deleted, step 13 is the opportunistic fallback persist that is
allowed to fail. Implements section 4.2 **on a worker task, not the
httpd worker** (a 68-param round trip inside an HTTP handler risks the
watchdog), with the persistent pending-swap record, using
`safety_cfg_http_set_and_confirm_f32()` / `apply_pairs()` /
`safety_ceiling_sync_*()` — no new Pico write path. Check flash-worker
ownership before choosing the task (`project_flash_worker_reentrancy`:
dispatching to the flash worker from the flash worker deadlocks the board, and
the host stub models no lock so tests cannot see it). Adds the Pico half to the
package format (`pkg_schema` 1 → 2 within this item).
*Acceptance:* a swap between two saved packages leaves both processors on the
new package, verified by **independent** read-back on both (ESP via
`GET /api/zones`, Pico via a fresh `GET_CONFIG_PAGE` — never either side's
cache); the ceiling identity holds at every observable moment; `active_id` is
set only after both read-backs pass; heaters are demonstrably disabled for the
whole window and re-enabled only at step 13.
*Negative test:* inject a Pico read-back mismatch at step 7 and assert full
rollback on BOTH sides with `active_id` unchanged and no latch. Then inject an
unrecoverable rollback failure and assert `CONFIG_DIVERGENCE` IS latched,
heaters ARE disabled, and a firing start is refused.

**Item 6 — Pico-half validation mirror and its honesty test.**
The ESP-side range validator for all 68 params.
*Acceptance:* every param id in `CONFIG_PARAM_TABLE` has a validator entry; a
test walks the table and fails if any id is unhandled — that walk is the drift
guard, and it is why a table walk was chosen over a field list.
*Negative test:* deliberately widen one bound in the ESP mirror so it accepts a
value the Pico refuses; assert the transaction still fails at step 6 and rolls
back cleanly. Restore the bound by hand. (The mirror must never be the only
gate — `project_negative_test_on_a_mirror_is_vacuous`.)

**Item 7 — The standing divergence invariant (section 3).**
Layer-1 continuous `config_crc` comparison hooked into the existing telemetry
path and `safety_ceiling_sync_reconcile_on_link_up()`; layer-2 field report on
trip; the persisted latch; heaters disabled via `kiln_io_owner`; firing refused
via `capability_preflight`; an in-progress firing stopped via `profiles_stop`
with relays verified off; `GET /api/config_divergence`; the clear-only-by-
read-back rule of section 3.6 with **no dismiss route**. Reuses the ceiling
agent's divergence primitive if one exists rather than adding a second
detector.
*Acceptance:* a deliberately mismatched Pico param trips the alarm within one
link-up cycle **and** within one telemetry frame of a live change; heaters go
off; the report names the exact field and both values; the latch survives a
reboot; it clears only after a verified read-back match; there is no HTTP route
or UI control that clears it directly.
*Negative test, two parts, both required:*
(a) with the configs matching, assert the alarm does NOT latch across 200
telemetry frames and 10 link-up cycles, including across every entry in section
3.2's legitimate-difference table — an alarm that always fires is worse than
none and will be disabled;
(b) assert that attempting to clear the latch without fixing the mismatch
leaves it set.
Also assert the summary string is **not truncated** against its fixed buffer
for the longest realistic field name plus two `%.4f` values (the
`ki_refusal_reason` `char[96]` truncation precedent).

**Item 8 — Interrupted-swap boot recovery.**
Section 4.4's five-case table in `kiln_cfg_store_init()`, after the existing
active-slot restore (preserve that ordering), with the fail-safe
diverged-until-proven default of section 3.4.
*Acceptance:* each of the five markers produces exactly the stated action, in
host tests that stage the marker directly; in every case heating is impossible
until the comparison passes.
*Negative test:* stage `PICO_DONE` and assert the code re-applies R rather than
finishing with P — the test must fail if "finish the swap" is implemented.

**Item 9 — UI: download / upload / warnings / confirm dialog / divergence banner.**
Extends the existing `#kilnConfigPicker` section. Confirm dialog per section
7.3. Backup page gains a pointer link. LCD line and banner per section 7.4.
*Acceptance:* Apply cannot be pressed without the dialog; hardware-differs
requires the explicit acknowledgement checkbox; a 404 on the new routes hides
the new controls without breaking the existing ones; the divergence banner has
no dismiss control; the LCD line fits without scrolling and adds no new colour.
*Negative test:* stub the import route to 500 and assert the page shows an
error and does not clear or reorder the existing slot list.

**Item 10 — Stack and size verification.**
Run `check_httpd_task_stack_budget.py` against the built ELF; record the
measured margin.
*Acceptance:* the margin on the deepest httpd path is no worse than before this
feature, with the number in the commit message.
*Negative test:* add a 2 KB stack local to the import handler and assert the
check FAILS — this is the check's own negative test, and three checks have
shipped vacuous in this repo without one. Remove the local by hand.

**Item 12 — The package hash (`pkg_schema` + `pkg_hash`).**
Section 3.1.3's canonical binary serializer and CRC-32, with `%.9g` float
emission in the JSON layer. Lands with item 2 or immediately after; items 3, 4
and 13 depend on it.
*Acceptance:* download → upload of the same package produces a bit-identical
`pkg_hash`; reordering JSON keys or reformatting whitespace does not change it;
`-0.0` and `+0.0` hash identically; a package containing NaN or Inf is rejected
at validation and never reaches the hash.
*Negative test:* change one float in a package by one ULP and assert the hash
changes. **Then** swap the float emitter to `%.4f` and assert the download →
upload round trip now FAILS its hash comparison — this proves the precision
requirement is load-bearing and not decorative. Restore `%.9g` by hand.

**Item 13 — Auto-save into the active package (section 2.4).**
The dirty flag on every live-config writer, the single debounced task, the
complete-serialize / persist / read-back-verify / then-recalculate-hash order,
and all six suppression rules.
*Acceptance:* a config change lands in the active package within 5 s of quiet;
a continuous stream of changes commits at least once per 60 s; no auto-save
occurs with no active package, during the apply transaction, or while
`CONFIG_DIVERGENCE` is latched; `pkg_hash` changes only after a verified
read-back.
*Negative test, the important one:* simulate the `137dea1a` defect — a
whole-object write that omits the model keys — and assert that auto-save either
(a) refuses because the serialized config failed read-back verification, or
(b) persists the complete config including the models, and that in **no** case
does `pkg_hash` get recalculated over a config with the models missing. The
test must fail if a partial config is persisted with a fresh hash. Then make
the read-back verification always report success and assert the test catches it
— restore by hand.

**Item 14 — Upload compatibility checks (section 5.2a).**
Distinct from item 4's validity checks, and reported distinctly.
*Acceptance:* a package configuring a zone on a nonexistent thermocouple
channel, one with a `relay_mask` bit above `relay_count`, one expecting a CT
this controller does not have with a CT-dependent guard enabled, and one
recording an unsupported `KILNLINK_PROTOCOL_VERSION`, are each refused with a
message naming the property, the package's value and this controller's value.
*Negative test:* assert a package that is merely *different* but compatible
(different zone names, different gains, a CT-less package on a CT-equipped
controller) is **accepted** — an over-strict compatibility check that refuses
legitimate packages makes the feature useless and will be worked around.

**Item 11 — Docs.**
Update `docs/CONFIG_FILESYSTEM.md` (capacity), `docs/SYSTEM_ARCHITECTURE.md`
(the package concept and the divergence invariant), `docs/SAFETY_CASE.md` (the
divergence fault as a named safe state), and add section 1's IN/OUT list to the
operator-facing help text. Rename this file without the `_PLAN` suffix once
nothing in it is open.
*Acceptance:* `tools/check_doc_hash_citations.ps1` passes; the IN/OUT sentence
shipped in the UI matches section 1.4 exactly.
*Negative test:* n/a (documentation) — instead a reviewer must confirm the
shipped UI sentence and section 1.4 agree, and the item is not closed
otherwise.

---

**Ordering note (revised by section 1a).** Item 12 (the package hash) must land
with or immediately after item 2 — items 3, 4, 13 and 14 all depend on it.
Item 15 (volatile install) is the new prerequisite for item 5. Nothing is
blocked on an owner decision any more; items 5 and 7 wait only on the
ceiling-mirroring agent. A sensible landing order:

> 1, 2, 12, 3, 4, 14, 13, 6, 9, 10 → then **15, 16** (SaftyFW side, independent
> of the ESP work above and safely parallelisable) → then 7 and 8 once the
> ceiling-mirroring work is in → then 5 → then 11.

**Rough size.** Items 1, 3, 10, 11, 12 are small (under ~200 lines each). Items
2, 4, 6, 8, 9, 13, 14, 15, 16 are moderate (~300–600 lines each including
tests). Items 5 and 7 are the large ones (~700–900 and ~500–700 lines
respectively, plus the worker task) and carry most of the risk. Total order of
magnitude: **~6000 lines including tests, across 16 items.**

---

## 9. What could go wrong, and what I would not do

**Would not do:**

1. **Would not make a package the boot-time authority.** It would mean
   re-pushing 68 Pico params on every boot, which means disarming the Pico on
   every boot. Rejected.
2. **Would not put firing profiles in the package.** A swap that replaced the
   recipe library is a worse surprise than any omission in section 1.4.
3. **Would not put Wi-Fi credentials in the package.** Packages are files
   handed between controllers.
4. **Would not trust an uploaded CT calibration.** Forced to
   `calibrated=false` unless the source board matches. Degrading to
   "uncalibrated" is safe; degrading to "confidently wrong current" is not.
5. **Would not push a hash to the Pico for it to echo back** (section 3.1.1).
   A hash the ESP computed and delivered proves only that a number arrived. The
   Pico's own `config_crc`, computed over its own bytes, is the one that cannot
   lie.
5a. **Would not reproduce `config_store_pack()` on the ESP** to predict what the
   Pico's CRC ought to be. That is a mirror of a binary layout, and a mirror bug
   would produce a *spurious* mismatch that disables heaters — the fastest route
   to this check being switched off.
5b. **Would not hash the JSON text**, and would not reuse `backup_export.c`'s
   `%.4f`/`%.1f` float emission for a hashed field (section 3.1.3 rule 5).
5c. **Would not auto-save a partial config, or stamp a hash over an unverified
   write** (section 2.4 rules 2 and 3) — that is `137dea1a` with the last good
   copy destroyed and the evidence erased.
5d. **Would not auto-save while `CONFIG_DIVERGENCE` is latched.** It would
   launder a known disagreement into a "consistent" saved state.
6. **Would not compare floats with an epsilon** (section 3.2). Bit-for-bit or
   nothing.
7. **Would not give the divergence alarm a dismiss control** (section 3.6).
8. **Would not build an LCD kiln picker** in this pass (section 7.4).
9. **Would not add a signature** to the package format. No key-management story
   exists; a CRC with honest documentation beats a signature implying a
   guarantee the board cannot make.
10. **Would not default a missing field** (section 6). Reject.
11. **Would not "finish the swap" after a `PICO_DONE` crash** (section 4.4).
    Revert to the last consistent state.
12. **Would not build the owner-approved bounded disarm window** (section
    1a.2). It is approved but unnecessary, and building it would reintroduce
    the exact hazard — a period with the Pico unarmed — that the owner's rule
    wants never to exist. An approval is not an obligation.
13. **Would not delete the Pico's `config_store` flash machinery** (section
    1a.4). It keeps a real job as the bring-up fallback, and a Pico with no
    fallback is one dropped push away from being unconfigured and unable to
    fire.
14. **Would not let an unconfigured Pico arm** under any circumstance, and
    would not add a timeout that expires into "proceed" when a fresh Pico
    cannot be reached (section 1a.6).

**Risks, named:**

- **~~The ARMED / always-armed contradiction~~ — RESOLVED** by section 1a.2. A
  volatile RAM push never reaches `config_store_decide_write()`. The
  owner-approved disarm window is deliberately not built.
- **An armed, unconfigured Pico is now the biggest safety risk in this plan**
  (section 1a.5). `abs_max_temp_c` defaults to `0.0f` when its `fields_set` bit
  is clear, so S1 never trips — unconfigured is a *missed-trip* state, not a
  fail-safe one. The whole model rests on UNCONFIGURED never arming. Item 16's
  negative test exists solely for this, and it is the one test in this plan I
  would not let a reviewer wave through.
- **Bypassing the seqlock on the volatile install** would hand the trip path a
  torn record — the exact defect the seqlock and `98d237b0` exist to prevent,
  and it would be intermittent and nearly unreproducible. Item 15's negative
  test (a) is the guard.
- **A volatile install that forgets to bump `config_crc`** makes the Pico
  report the old config's identity while running the new one. Every check in
  section 3 would then pass while the two sides disagreed. Item 15's negative
  test (b) is the guard.
- **The bring-up fallback silently diverging** from the active profile
  (section 1a.4). Closed by carrying `pkg_hash` in the persisted record and
  treating a mismatch as "push now", not as an alarm — but an implementer who
  skips the `pkg_hash` and compares only `config_crc` will produce a fallback
  that looks valid and is wrong.
- **An over-eager divergence check gets disabled.** Section 3.2 enumerates the
  legitimate differences and item 7's negative test (a) exists specifically to
  prove the check is quiet when it should be. If that test is hard to make
  pass, the check is wrong — do not weaken the test.
- **The ESP-side Pico validator is a mirror** of `config_params_set()`. Mirrors
  drift, and negative tests on mirrors prove nothing. Mitigation in section
  5.2; item 6's negative test must break the *Pico's* acceptance, not a
  test-local copy.
- **The reset-one-side class.** This feature introduces a new pair: the ESP's
  active package and the Pico's config contents, joined by a semantic contract
  expressed nowhere as a shared type. The section 3 invariant *is* the
  enforcement of that contract, and is the one narrow mirror-drift check in
  this repo worth pinning. Any future code that resets one side must revisit
  the other — CLAUDE.md's standing practice: *"who else holds a copy or a
  derived expectation of this?"*
- **A spuriously mismatching hash is the way this feature kills itself.** It
  disables heaters on a healthy kiln; the second time it happens the owner
  turns the check off. Section 3.1.3's float rules, section 3.1.2's
  format-version gate, and item 12's negative test all exist for this single
  risk. Treat any spurious trip during bring-up as a stop-ship defect, not a
  tuning problem.
- **Over-strict compatibility checking** has the same shape: refuse a package
  the controller could actually run and the feature stops being used. Item 14's
  negative test is the guard.
- **Read-back cost.** Step 7 pages the Pico's whole config over the UART link
  inside the transaction. The mechanism exists
  (`safety_cfg_store_refetch()`), but it must not run on the httpd worker —
  hence the worker task in item 5.
- **Message truncation.** `ki_refusal_reason` is `char[96]` and a recent
  message truncated at 162 bytes. Every operator-facing string here is sized
  against its buffer and asserted untruncated (item 7).
