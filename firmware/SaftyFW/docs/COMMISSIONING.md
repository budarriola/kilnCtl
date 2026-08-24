# Commissioning — how safety parameters get in, and stay in

> **Status:** design · **Last reviewed:** 2026-08-22
> Owner decision, 2026-08-22: *"it should be set from the ESP settings GUI
> during initial setup along with other safety parameters. The safety
> parameters should be saved on the safety processor in a place that is not
> overwritten during flash or OTA but should have full version checking and
> CRC. These should not be sent from the ESP every boot, but they must be set
> at least once, and the ESP should receive these sometime during boot up or
> on request."*

**What** gets commissioned is not decided here — [`CONFIG_REFERENCE.md`](CONFIG_REFERENCE.md)
§§1–5 is the authoritative table of every tunable, its default, and whether
getting it wrong is dangerous, a nuisance, or cosmetic. This document is only
about the mechanism: where the values live, how they travel, and who may
change them.

---

## 1. Where the values live

**On the safety processor, not the ESP.** The processor that enforces a limit
is the one that must hold it. An ESP that has to hand the Pico its thresholds
at every boot is an ESP that can withhold them, mangle them, or fail to send
them at all — and the failure looks like a safety processor running on
defaults while the GUI cheerfully displays the values it *thinks* are in
force.

The storage already exists and already satisfies the requirement:

| Property | Where it comes from |
|---|---|
| Outside both application slots, so an OTA never touches it | `bootloader/flash_layout.h`, `BOOTLOADER_CONFIG_FLASH_OFFSET` = `0x1B1000`, well past slot B's end |
| The bootloader never writes there | `docs/BOOTLOADER.md` §3, "what it must never do" |
| Versioned | `config_store_record_t.format_version` |
| CRC'd | trailing CRC32 over the record; `config_store_unpack()` rejects any mismatch |
| Survives a fresh SWD flash of either slot | the region is not part of either slot image |
| Wear-levelled | a 16-slot log in one 4K sector, highest `seq` wins |

A full-chip erase over SWD *does* destroy it. That is correct and deliberate:
a chip erase is "this board is now a different board", and silently carrying
one kiln's commissioning across to another is exactly the failure that ends
with a guard trusting a threshold nobody set for it.

### 1.1 Record format version 2

Version 1 holds `tc_type`, `calibration_missing` and `ct_cal[3]`, with 37
reserved bytes — not enough for `CONFIG_REFERENCE.md`'s full surface. Version 2:

- `CONFIG_STORE_RECORD_LEN` 256 → 512 B (8 slots per sector instead of 16;
  still ample, since a record is written only at commissioning, not at runtime).
- Every field in `CONFIG_REFERENCE.md` §§1–5, explicitly including the four §1
  fields that **have no compiled-in default** and whose guards stay disabled
  until set.
- Reserved tail packed `0xFF`, matching the erased-flash background, so a
  later field costs a pack/unpack change and not a layout move.

**Three-outcome load**, the same discipline `KilnFW`'s NVS loaders use:

| Stored version | Action |
|---|---|
| == current | load |
| < current | migrate forward, preserving what v1 held, defaulting the rest — and **`calibration_missing` stays true**, because a migrated record was never commissioned against the new fields |
| > current | **refuse**, fall back to compiled-in defaults with `calibration_missing` set |

Refusing a newer record matters more here than the usual "don't crash on
future data" argument: a v3 record read as v2 would silently reinterpret
whatever v3 put at those byte offsets as thresholds. Misreading a byte offset
produces a confident, plausible, wrong limit.

## 2. How values get there

Field-addressed staging, not whole-record streaming. A v2 record is 512 B and
the wire's payload cap is 253 B, so a whole-record write would need chunking
anyway; field addressing gets the same result with better properties:

- **Version-tolerant.** An ESP that knows fewer parameters than the Pico (or
  more) still works — unknown ids are refused individually and named in the
  reply, rather than the whole transfer failing on a length mismatch.
- **Retry-safe.** Each `SET_PARAM` is idempotent; a lost frame costs one retry,
  not the whole commissioning pass.
- **Maps 1:1 to the GUI**, so a form field and a wire parameter are the same
  thing and there is no translation layer to get out of step.

Staged in RAM, then committed as one record:

| Id | Name | Direction | Meaning |
|---|---|---|---|
| `0x1B` | `SET_LOG_LEVEL` | ESP → Pico | Runtime log verbosity. Unrelated to commissioning; minted in the same pass because it was the last unallocated id blocking `log_task_set_level()` from being reachable over the wire |
| `0x1C` | `SET_PARAM` | ESP → Pico | `param_id` u16 + type tag + value. **Stages only** — nothing reaches flash |
| `0x1D` | `COMMIT_CONFIG` | ESP → Pico | Validate the staged set as a whole, write one record, bump `config_crc` |
| `0x1E` | `GET_PARAM` / `PARAM` | both | Request one parameter / the reply, sharing an id per the `GET_CT_CAL`/`CT_CAL` convention |
| `0x1F` | `GET_CONFIG_PAGE` / `CONFIG_PAGE` | both | Bulk read: packed `(id, value)` pairs, one page per frame, so the ESP can fetch the whole set in a few frames |
| `0x20` | `COMMIT_CONFIG_REJECTED` | Pico → ESP | Sent only when a `COMMIT_CONFIG` is refused: names the offending `param_id` (or a "not field-specific" sentinel) and a coarse reason code (range / contradiction / ARMED / storage). Closes the gap section 3.1 used to describe as a known limitation |

### 2.0.1 `SET_CONFIG` (`0x16`) and `SET_CT_CAL` (`0x19`) — pre-v2 leftovers, not this model

Two more ESP → Pico commands write to the store outside the `SET_PARAM`/
`COMMIT_CONFIG` flow above: `SET_CONFIG` (`0x16`, sets `tc_type`) and
`SET_CT_CAL` (`0x19`, sets one `ct_cal` channel). Both predate the field-
addressed staging model this section documents, and the ESP still calls
`SET_CONFIG` **automatically** on every link reconnect
(`safety_link.c`'s `tc_type_last_sent` resets on link down→up, forcing a
resync on the next poll) — routine on this project, not a rare edge case.

**2026-08-24 fix:** both handlers used to build the record they wrote by
starting from `config_store_default()` and setting only the one or two
fields the command actually names, then handing the result to
`config_store_write()` — which replaces the **entire** stored record with no
merge. A one-field wire command was therefore silently factory-resetting
every other commissioned field, every `SET_PARAM`-staged threshold, and (for
`SET_CONFIG`) all three `ct_cal` channels, on every call — including the
automatic reconnect resend above, with no operator action and no wire-visible
warning beyond `calibration_missing` flipping true and `config_crc` changing.
Fixed by reading the currently-committed record first
(`config_store_get_full_record()`) and mutating only the field(s) the command
names — see `link_frame_apply_set_config()` / `link_frame_apply_set_ct_cal()`
(`src/tasks/link_frame.h/.c`) for the extracted, host-tested mutation logic
and `test/test_link_frame.c` for the regression coverage.

**The general rule this bug is an instance of: any wire command that updates
part of a record must read-modify-write the committed record, never rebuild
it from compiled defaults.** `SET_PARAM`/`COMMIT_CONFIG` above get this for
free (staged in RAM against the live cache, committed as one validated
whole); a bespoke single-field command has to earn it by hand, and `SET_
CONFIG` didn't.

**Recommendation:** `SET_CONFIG` and `SET_CT_CAL` should eventually be
retired in favor of routing `tc_type` and `ct_cal` through `SET_PARAM`/
`COMMIT_CONFIG` (`0x0105` and `0x0310`-`0x0318` already exist in the table
below) so there is exactly one write path and one place this class of bug can
occur. That is a two-sided change (KilnFW currently calls `0x16` directly,
including the automatic reconnect resend) and is out of scope here — noted so
the next person doesn't have to rediscover why this section exists.

### 2.1 Parameter ids

Ids are grouped by `CONFIG_REFERENCE.md` section, one hex hundred per section,
so an id read out of a log says which part of the surface it belongs to
without a lookup. **Ids are permanent**: a field that is removed leaves its id
burned, never reused, because a stale ESP writing a recycled id would land a
value in the wrong field with the right type tag and no error anywhere.

| Id | Field | Type |
|---|---|---|
| `0x0101` | `tc_source` | U8 |
| `0x0102` | `borrowed_zone_index` | U8 |
| `0x0103` | `tc_placement_mode` | U8 |
| `0x0104` | `abs_max_temp_c` | F32 |
| `0x0105` | `tc_type` | U8 |
| `0x0106`–`0x0108` | `ct_channel_map[0..2]` | U8 |
| `0x0201` | `firing_margin_c` | F32 |
| `0x0202` | `overshoot_margin_c` | F32 |
| `0x0203` | `overshoot_time_s` | U16 |
| `0x0204` | `max_rate_c_per_min` | F32 |
| `0x0205` | `rate_window_s` | U16 |
| `0x0206` | `blind_grace_s` | U16 |
| `0x0207` | `frozen_window_s` | U16 |
| `0x0208` | `tc_disagreement_c` | F32 |
| `0x0209` | `tc_disagreement_time_s` | U16 |
| `0x020A` | `tc_expected_offset_c` | F32 |
| `0x020B` | `cj_warn_c` | F32 |
| `0x020C` | `cj_max_c` | F32 |
| `0x020D` | `cj_time_s` | U16 |
| `0x020E` | `borrowed_stale_s` | U16 |
| `0x020F` | `borrowed_stale_trip_s` | U16 |
| `0x0210` | `borrowed_type_expected` | U8 |
| `0x0301` | `i_present_a` | F32 |
| `0x0302`–`0x0304` | `zero_counts[0..2]` | U16 |
| `0x0305` | `correlation_window_s` | U16 |
| `0x0306` | `stuck_on_time_s` | U16 |
| `0x0307` | `trip_verify_s` | U16 |
| `0x0308`–`0x030A` | `k_ct_v_per_a[0..2]` | F32 |
| `0x030B`–`0x030D` | `gain[0..2]` | F32 |
| `0x030E` | `mains_voltage_v` | F32 |
| `0x030F` | `power_window_s` | U16 |
| `0x0310`–`0x0312` | `ct_cal[0..2].gain` | F32 |
| `0x0313`–`0x0315` | `ct_cal[0..2].offset` | F32 |
| `0x0316`–`0x0318` | `ct_cal[0..2].calibrated` | BOOL |
| `0x0401` | `context_max_age_s` | U16 |
| `0x0402` | `link_timeout_s` | U16 |
| `0x0403` | `link_dead_hard_s` | U16 |
| `0x0404` | `mainfault_debounce_ms` | U16 |
| `0x0405` | `telemetry_period_ms` | U16 |
| `0x0501` | `startup_grace_s` | U16 |
| `0x0502` | `estop_debounce_ms` | U16 |
| `0x0503` | `watchdog_timeout_ms` | U16 |
| `0x0504` | `config_check_period_s` | U16 |

**`tc_type` (`0x0105`) gained a `fields_set` bit (`CONFIG_STORE_SET_TC_TYPE`)
on 2026-08-24.** Unlike the other bits in this table, it is not a "no safe
default" field — it keeps its compiled default (K) — but a committed K and a
never-touched, defaulted-to-K record are the same byte in the store, and
`max31856_tc_range_policy.h`'s per-type plausibility band needs to tell them
apart (a genuinely commissioned type gets its own exact band; an
uncommissioned one gets a wider, type-agnostic floor instead —
`THERMOCOUPLE.md` §2). This bit is now folded into the same required-set
check that gates `calibration_missing` (§2.1's own `config_params_all_
required_set()`), so a board that has committed every other field but never
touched `0x0105` still reads back as not fully commissioned.

**`ct_channel_map` is three ids but ONE `fields_set` bit.** The store tracks
it as a single group bit because `CONFIG_REFERENCE.md` §1 describes it as
confirmed by one indivisible commissioning pass (the one-relay-at-a-time check
in `CURRENT_SENSE.md` §5 step 2). So the group bit is set only when **all
three** channels are present in the same commit; two out of three leaves it
unset. A partially-mapped CT set is not a safe subset of a mapped one — S3 and
S4 correlate against the channel they were told to watch, and a half-populated
map means one of them is watching a channel nobody confirmed.

`calibration_missing` has **no id**. It is not settable — it is cleared by a
successful commissioning commit and set by anything that invalidates one.
Letting the ESP write it directly would make "this board is commissioned" a
claim the ESP can assert rather than a fact the store derives.

**Validation happens at `COMMIT_CONFIG`, not at `SET_PARAM`**, because the
rules that matter are cross-field. `CONFIG_REFERENCE.md` §1 requires
`tc_placement_mode` to be **rejected, not silently reconciled**, when it
contradicts `tc_source` — a check that cannot be made one field at a time.
A commit that fails validation writes nothing and names the offending field.

**Writes are refused while ARMED**, both as a safety rule and an RP2040
flash-write constraint (`ARCHITECTURE.md` §8). This is enforced inside the
store's own write path, not at the call site, so a future second caller cannot
forget it.

## 3. How the ESP learns what is in force

The requirement is "not sent every boot, but the ESP must receive them
sometime during boot or on request." `FW_VERSION` already carries
`config_version` and `config_crc`, which makes this nearly free:

1. The ESP caches the last-fetched parameter set in its own NVS, alongside the
   `config_crc` it was fetched at.
2. Every `FW_VERSION` frame — pushed at Pico boot, and requested by
   `safety_poll_task()` until answered — carries the Pico's current
   `config_crc`.
3. **The ESP refetches only when that CRC differs from its cache.** A
   steady-state reboot of either processor transfers nothing.

The CRC is the authority, never the ESP's copy. The cache exists so the GUI
can render instantly and so a dead link still shows the last known values —
clearly labelled as last-known, with the crucial distinction preserved:

> **"What the GUI shows" and "what the safety processor is enforcing" are two
> different claims.** They agree only when the displayed `config_crc` matches
> the one in the live telemetry. When they disagree, the telemetry wins and
> the GUI must say so rather than quietly showing its cache.

### 3.1 The ESP's HTTP surface

Fixed here so the firmware and the page can be built against the same
contract rather than one chasing the other.

`GET /api/safety/commissioning` →

```json
{
  "link_up": true,
  "live_config_crc": 51966,      // from the newest FW_VERSION frame
  "cached_config_crc": 51966,    // what the values below were fetched at
  "stale": false,                // cached_config_crc != live_config_crc
  "commissioned": true,          // !calibration_missing
  "fetched_ms_ago": 4210,
  "params": [ { "id": 257, "name": "tc_source", "type": "u8",
                "value": 1, "set": true } ]
}
```

`"stale": true` is the case that must never be rendered as ordinary data: it
means the safety processor is enforcing something other than what is shown.
An unset parameter carries `"set": false` and **omits** `value` entirely
rather than sending a zero the page might print.

`POST /api/safety/commissioning` — form-urlencoded, `id=<n>&value=<v>` pairs
plus `commit=1`, mapping onto `SET_PARAM`×N then `COMMIT_CONFIG`. Replies with
the commit verdict, and on rejection **names the offending field and the rule
it broke** — "rejected, not silently reconciled" is only useful if the person
is told what to fix.

`POST /api/safety/commissioning/bench_preset` — applies the §4.1 bench values.
Leaves `calibration_missing` set.

## 4. The GUI

A commissioning page under settings, grouped as `CONFIG_REFERENCE.md` §§1–5
are grouped, carrying that table's risk badges (🔴/🟠/⚪) rather than
presenting forty-five equal-looking numbers.

Non-negotiable behaviours, each of which exists because of a rule already
written in `CONFIG_REFERENCE.md` §7:

- An **unset** §1 field renders as unset, never as a plausible zero, and says
  which guards are disabled because of it.
- A **disabled guard is shown as disabled.** "A disabled guard that looks
  enabled is worse than no guard, because someone is relying on it."
- The live `config_crc` is displayed next to the values, so "which thresholds
  is the safety processor actually enforcing?" is answerable without trusting
  a separate record of what was uploaded.
- Contradictory combinations are refused with the reason, on both sides —
  the browser for immediacy, the Pico because the browser is not a safety
  boundary.

### 4.1 Bench values

A dev-only preset may be applied for bench testing so the link and the flow
can be exercised before the kiln exists. It must be **visibly** a bench
preset: applying it leaves `calibration_missing` set, and the GUI says the
board is running test values. A bench preset that looks like a commissioned
board is the same failure as a guessed default, arrived at more slowly.

---

## Completion checklist

- [ ] Record v2: all `CONFIG_REFERENCE.md` §§1–5 fields, 512 B, three-outcome load
- [ ] v1 → v2 migration preserving `tc_type`/`ct_cal`, `calibration_missing` still set
- [ ] Newer-than-known record refused, not reinterpreted
- [ ] `0x1B` `SET_LOG_LEVEL` codec + consumer
- [ ] `0x1C`/`0x1D` stage-and-commit, validation at commit, ARMED refusal in the store
- [ ] `0x1E`/`0x1F` read-back, single and paged
- [x] `0x20` `COMMIT_CONFIG_REJECTED` -- per-field rejection reason back to the ESP, surfaced in the commissioning page's error text
- [ ] ESP cache keyed on `config_crc`, refetch only on change
- [ ] GUI: risk badges, unset states, disabled-guard display, live CRC, contradiction refusal
- [ ] Bench preset leaves `calibration_missing` set and is labelled as such
- [ ] Every new check proven able to fail before it is trusted
