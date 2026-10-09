# Investigation: were the three zones' plant models lost from the bench board?

2026-09-14. Triggered by a review reporting `model_k_dc = 0.0` on all three
zones of the live bench board, against an apparent earlier-today reading of
`42.731 / 32.397 / 33.849`. This document establishes ground truth, names the
cause, and records the repair.

**Answer, up front: the models WERE genuinely absent, and have now been
restored from recorded measurements — no re-autotune was needed. But the loss
did NOT happen tonight; it happened on or before 2026-09-11, and the "earlier
today those zones read 42.731/..." premise is itself mistaken.**

## 1. Ground truth: three independent reads

The running `kilnctrl` MCP server was confirmed stale (see section 5) and was
restarted after `profiles_get_exec_status` showed no firing. All three paths
below were then read.

| path | result |
|---|---|
| raw `GET /api/zones` (curl, outside the MCP facade) | `model_k_dc`/`model_tau_s`/`model_dead_time_s` = `0.0/0.0/0.0` on z0, z1, z2 |
| raw `GET /api/zones_diag` | `model_fit_temp_c` = `model_fit_ambient_c` = `-273.15` (the `ZONE_MODEL_FIT_TEMP_UNKNOWN` sentinel) on all three |
| restarted MCP, `control_get_zones` (UART CONTROL wire, not HTTP) | "z0/z1/z2: no model identified (all-zero sentinel)" |

The third path is genuinely independent of the first two: it reaches the same
config over the hardened UART link rather than the HTTP stack.

What was still intact at the same moment, on every path:

| field | z0 | z1 | z2 |
|---|---|---|---|
| `coupling_diag_k_dc` | 42.7310 | 32.3969 | 33.8493 |
| `tuning_valid` | yes | yes | yes |
| `tuning_seq` / `tuning_baseline_c` / `tuning_raw_rise_c` | 1 / 37.17 / 20.93 | 1 | 1 |
| `pid_kp` / `pid_ki` / `pid_kd` | 0.0371 / 0.0001 / 0.7476 | 0.0639 / 0.0002 / 0.9989 | 0.0703 / 0.0003 / 0.9126 |
| `autotune_baseline_k_dc` | 0.0 | 0.0 | 0.0 |
| coupling matrix off-diagonals | populated | populated | populated |

So exactly three fields per zone were zero, and every neighbouring field
survived. That selectivity is the central clue.

## 2. The reported "earlier today" reading was a different field

The figures `42.731 / 32.397 / 33.849` quoted in the alarm are
**`coupling_diag_k_dc`**, not `model_k_dc`. They were still on the board,
unchanged, at the moment the alarm was raised.

This is traceable. `docs/audits/zones_post_model_key_omission_2026-09-13.md`
says so explicitly and honestly (its section 3):

> `coupling_diag_k_dc` — a related but DISTINCT field from `model_k_dc` (the
> coupling matrix's own measured diagonal, not the feedforward model) — reads
> `z0=42.7310 z1=32.3969 z2=33.8493` [...] This is consistent with (but not
> proof of) these zones also still holding real `model_k_dc`/`tau`/`dead_time`
> values [...] It is not a substitute for reading the actual fields

It further records *why* it could not read the real fields: no published MCP
tool rendered them at the time.

One day later, `docs/audits/fuzzy_no_model_no_fuzzy_2026-09-14.md` restates
that explicitly-hedged inference as measured fact:

> The live bench board has all three zones autotuned (non-zero `model_k_dc`:
> z0=42.731, z1=32.397, z2=33.849)

**No reading, tonight or on 2026-09-13, ever actually observed a non-zero
`model_k_dc` on this board.** The "loss" was discovered tonight, not caused
tonight. This is the project's own "a retraction hid the stale claim" class:
a hedged inference laundered into an assertion by the next document to cite it.

The `model_tau_s` 255.6/258.9/247.1 and `model_dead_time_s` 40.3/31.3/26.0
figures in the alarm are real, but they come from
`docs/audits/coupling_joint_identification_capture_2026-09-10.md` (its
per-zone accepted-fit table) and `logs/coupling/CHECKPOINT.md`, not from a
board read.

## 3. The models were nevertheless real, and were genuinely lost

They are not a fiction. On 2026-09-10 a step autotune ran and was **accepted**
on each zone, with these fits recorded at the time:

| zone | k_dc (C/duty) | tau (s) | dead time (s) |
|---|---|---|---|
| z0 | 42.731 | 255.6 | 40.3 |
| z1 | 32.397 | 258.9 | 31.3 |
| z2 | 33.849 | 247.1 | 26.0 |

Three independent corroborations that these landed on the board:

1. `coupling_diag_k_dc` is written **only inside `if (model_persisted)`**
   (`firmware/KilnFW/App/drivers/control/autotune_engine_guard.c`), and it
   holds exactly these k values to four decimal places. Its presence proves
   `zones_config_set_model()` returned true at accept time.
2. The `tuning_*` quality record is written only after gains *and* model both
   persisted. `tuning_valid=yes`, `tuning_seq=1`.
3. `tuning_baseline_c=37.17` and `tuning_raw_rise_c=20.93` match the
   2026-09-10 capture's own `baseline_c=37.17`, `final_c=58.1`
   (58.1 − 37.17 = 20.93) exactly.

So the models were persisted on 2026-09-10 and were absent by tonight.

## 4. When and how

### When: on or before 2026-09-11 — not tonight

The board's boot log, across all three logged boots, reads:

```
I (6781) zones_http: zones_cfg from 'kiln_nvs' loaded (on-disk version 25) as v26
```

The on-disk blob is **still version 25**. `nvs_save()` stamps the *current*
struct version on every successful save, and `ZONES_CFG_VERSION` was bumped to
26 by `97288659` on 2026-09-11. Therefore **no zones_cfg save had succeeded
since v26 firmware first booted** — the last write to this blob happened while
v25 firmware was running, on or before 2026-09-11.

This exonerates tonight's activity entirely:

- **Tonight's flashes are not the cause.** `flash_firmware()` writes the
  factory partition only. Independently confirmed here: the on-disk cfg
  version is unchanged and every non-model field survived, which a flash that
  touched NVS could not produce.
- **Tonight's agents are not the cause.** Any zones POST they issued would
  have stamped the blob v26.

### How: the documented POST-omission sharp edge

`6093b8b6` / `docs/audits/zones_post_model_key_omission_2026-09-13.md`
document the behaviour: in `zones_http_post_parse.c`, `z%u_k` / `z%u_tau` /
`z%u_deadtime` have **no `else` branch carrying `current_z` through**, so a
whole-object `POST /api/zones` that omits them leaves the caller-zeroed scratch
struct at 0.0 — the "no model" sentinel — and the stored model is deleted.

Every other field in the same struct either has no POST key and an explicit
carry-through line (`tuning_*`, `model_fit_*`, `adaptive_tune_enabled`) or
takes an omit-preserves branch (`coupling_diag_k_dc`, `fuzzy_strength_pct`,
coupling coefficients). **The observed damage signature — exactly the three
model fields zeroed, everything around them intact — is this path's fingerprint
and no other candidate's.**

Corroborating evidence that such a save actually occurred on this board in the
window: `36f88d62` (2026-09-11 09:29) is titled "Adversarial review of
`97288659`: a whole-page zones save silently zeroed the new adaptive_tune
ratchet anchor". A whole-page zones save against the live board is therefore a
documented event in exactly the right window. `autotune_baseline_k_dc` reading
0.0 today is consistent either way: its carry-through line postdates the save,
and 0 is in any case its "no baseline recorded yet" sentinel for an autotune
(2026-09-10) that predates the field's existence (2026-09-11).

**Honest limit:** no HTTP request log survives, so the specific request cannot
be exhibited. The claim is that this is the only mechanism consistent with all
the evidence, not that the request was captured.

### Candidates tested and eliminated

- **The v25→v26 migration.** *Exonerated.* `zones_config_migrate.c` `case 25`
  is a verbatim `memcpy` of each historical `zone_cfg_v25_t` over the current
  `zone_cfg_t` prefix. `model_k_dc`/`tau`/`dead_time` sit inside that prefix
  and are copied unchanged; only the newly appended `autotune_baseline_k_dc`
  is left at the entry memset's 0. It cannot zero a model. It also never
  actually ran destructively here — the on-disk blob is still v25, migrated in
  RAM at each boot and never written back.
- **Tonight's `flash_firmware(allow_sensitive_dirty=True)`.** *Exonerated*, by
  the unchanged on-disk cfg version (above) rather than by assumption.
- **`cfg` LittleFS / NVS divergence.** *Not implicated.* NVS is authoritative
  and unconditional; the loaded values came from `kiln_nvs`, as the boot log
  states, and the damage signature is field-selective rather than
  store-selective.

## 5. The stale MCP server: what else it may have affected

The server running until tonight's restart was commit `fd8d7b93` (2026-09-11
07:44), started 2026-09-11. Two consequences:

1. **It rendered no plant-model section at all.** This is why the absence went
   unnoticed for three days, and why the 2026-09-13 audit had to fall back on
   inference. It is the direct cause of the misdiagnosis in section 2.
2. **It carried a known-wrong hardcoded string.** `a605df46` (2026-09-11
   08:10 — 26 minutes *after* the running server's commit) fixed
   `control_get_zones()` hardcoding the claim that
   `s_coupling_use_measured_diag_k_dc` "is compiled false". The flag moved to
   `zone_coupling_solve.c:zone_coupling_use_measured_diag_k_dc()` on
   2026-09-10 and is compiled **true**. **Every `control_get_zones` reading
   between 2026-09-11 07:44 and tonight's restart printed the inverted claim.**
   Any conclusion drawn in that window about whether the measured diagonal is
   in use should be re-checked against the restarted server, which now reads
   the flag live from the source tree.

A third, newly-stale item found during this pass: the restarted server's
`control_get_zones` still prints "`autotune_baseline_k_dc`: NOT exposed by GET
/api/zones as of 2026-09-13 ... a real firmware gap". That is now false —
`zones_http_get.c` does emit `autotune_baseline_k_dc`, and it read 0.0 over
raw HTTP during this investigation. The note should be retired.

## 6. Recovery — done, without re-autotuning

Recovery source: the 2026-09-10 accepted fits recorded in
`docs/audits/coupling_joint_identification_capture_2026-09-10.md` and
`logs/coupling/CHECKPOINT.md`, cross-validated against the board's own
surviving `coupling_diag_k_dc` (which matches the recorded k to four decimal
places on all three zones, per section 3).

The `tools/PcTools/board-backups/` snapshots were checked and are **not** the
right source: the newest is `20260909T022340Z`, predating the 2026-09-10
autotune, and holds the older preset models (z0 39.2459/263.8/52.8).

Repair path chosen deliberately to avoid the very mechanism that caused the
loss: **`control_set_zone_model` over the UART CONTROL wire**, which calls
`zones_config_set_model()` on the three fields alone. No whole-object
`POST /api/zones` was issued at any point. `abs_max_temp_c`, `target_c`,
`ramp_c_per_hr`, `dwell_min` and `segment_count` were not touched.

Verified by read-back on both paths (UART `control_get_zones` and raw
`GET /api/zones`):

| zone | model_k_dc | model_tau_s | model_dead_time_s | fuzzy_model_valid |
|---|---|---|---|---|
| z0 | 42.7310 | 255.6 | 40.3 | false -> **true** |
| z1 | 32.3969 | 258.9 | 31.3 | false -> **true** |
| z2 | 33.8493 | 247.1 | 26.0 | false -> **true** |

PID gains, `max_temp_c` (80.0), the coupling matrix and `coupling_diag_k_dc`
all read unchanged after the write. `fuzzy_model_valid` flipping to true is the
observable behavioural recovery: per `233ded79`, a zone with no identified
model runs plain PID with fuzzy inactive.

`zones_config_set_model()` ends `return nvs_save() == ESP_OK;` and all three
calls returned success, so this is persisted, not RAM-only. The final
confirmation — the boot log reporting `on-disk version 26` instead of 25 —
will appear at the board's next boot; no reboot was performed here, to avoid
the SX1509 post-reset trip and S6a dual-reset hazards for no diagnostic gain.

**No autotune was run.** Had re-identification been necessary, the cost from
the 2026-09-10 capture's own record would be roughly 14.5 min per accepted
zone run at the firmware's settling gate (37 min/zone against that document's
conservative 8-tau floor), plus a cooldown to a genuinely rested ambient
baseline between zones — a multi-hour heating session, and an owner decision.

## 7. Residual risk

The POST-omission edge is unchanged and still live. Nothing in this pass
altered it; `6093b8b6`'s audit deliberately left it as documented behaviour,
and its host test `test_post_omitting_model_fields_deletes_them()` pins it.
This incident is the first known case of it actually destroying a real
measurement on hardware, which is new information that audit did not have.
Whether to convert the model block to omit-preserves is an owner call, not
this pass's to make — but it should now be made with a confirmed casualty on
the record rather than as a hypothetical.

## 8. Board final state

Relays off (`R1=0 R2=0 R3=0 R4=0`), no firing (`state=0`), safety link up,
SaftyFW armed and not tripped, safety thermocouple valid at 27.60 C, no
unacknowledged crash report, `tx_dropped 0`. Firmware `2a75266f` (dirty),
uptime 6445 s.
