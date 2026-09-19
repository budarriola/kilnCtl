# CT commissioning plan — real amps from any probe

Status: opened 2026-09-06. Owner request: user-entered probe rating and
zero offset, an automatic idle-offset measurement, and readings in real
amps regardless of which probe is fitted. Facts below come from
`CURRENT_SENSE.md`, `HARDWARE.md` §6/§9, `current_sense.c`,
`safety_guards.c` and `zones_current_sweep_*.c` as of `05087f0`.

## Does the request make sense?

Mostly yes; three things need adjusting.

1. **Probe rating is already the model.** `current_sense.c:170-196` computes
   `I = (counts - zero_counts) * 3.3/4096 / (gain * sqrt2 * k_ct_v_per_a)`.
   Every probe in this family outputs 1 V at full scale, so
   `k_ct_v_per_a = 1 / A_fs`. The user should type `A_fs` (the number on
   the probe: 1, 20, 50, 100 A); the firmware derives `k_ct`. No new physics,
   only a different unit at the input, and the field must become **editable**
   — today `k_ct_v_per_a[0..2]` and `zero_counts[0..2]` are readonly on
   `safety_commissioning_page.html` (ids 770-778) because the zone sweep
   writes them.
2. **Zero offset in the user's units.** The +59 mV is at the CT output. Through
   the 0.715 front-end gain that is ~42 mV at the ADC, ~52 counts. Let the
   user enter mV-at-probe; store `zero_counts` as now (the Pico's only
   representation). Auto-measurement writes the same field.
3. **The bench probe sits at 7 % of scale; real kilns will not.** At 70 mA
   total the whole kiln is ~50 mV at the ADC ≈ 62 counts above offset, one
   heater ≈ 21 counts. The reference is the Pico's own 3.3 V rail
   (`HARDWARE.md` §6) and no noise floor has ever been measured — the
   16x oversample buys "about 2 bits". So: step 0 is a measured noise floor,
   and everything at this scale is judged against it. On a real kiln (a
   20-50 A probe at 30-60 % of scale) the same firmware has 20-50x the
   signal. Design for the real kiln; verify what the bench can.

Two things the request did not cover but that decide whether the readings
mean anything:

- **One summed CT vs the per-zone design.** S3/S4/S9 read "any current
  present" and work unchanged on a summed CT. S14 (overcurrent) compares
  channel `ch` against `i_normal_a[ch]` — per zone. With a summed probe the
  expected value is the *sum of the normals of the zones currently
  commanded on*, which the Pico knows from relay feedback. The sweep's
  `ct_channel_map` derivation (`GUARD_TEST_MATRIX.md` §3.3: one relay must
  map to exactly one channel) will see all three zones on channel 3 —
  today that is either rejected or produces a nonsense map. A
  `ct_topology` setting (per_zone | summed) is needed, and CURRENT_SENSE.md
  (last reviewed 2026-08-16) still describes three per-zone probes.
- **The owner's 70 mA cannot be checked to better than about ±10 mA with a
  1 A probe** (1 % of full scale is 10 mA). The firmware will report what
  it sees; whether the meter or the probe is right is a bench question.
  Do not chase sub-10 mA disagreements.

## Model

```
A_fs            probe rating, amps at 1 V output          user field, per channel
zero_mv         probe output at zero current (mV)         user field OR auto-measured
gain            front-end divider, 0.715 default          existing, readonly
counts          16x-oversampled ADC mean                  existing
amps_rms = max(0, counts - zero_counts) * (3.3/4096) / (gain * sqrt2) * A_fs
zero_counts = zero_mv/1000 * gain * 4096/3.3
```

`sqrt2` assumes a sinusoidal load (rectified peak envelope, τ = 1 s); true
for resistive heaters on mechanical relays, wrong for phase-chopped SSRs —
document, do not solve.

## Steps

0. **Noise floor — DONE, 2026-09-18.** Measured with Wi-Fi associated and
   the front panel flushing (a realistic in-service condition, not a quiet
   bench), all four relays off, nothing heating. Mean/std/3σ figures are
   recorded in `firmware/SaftyFW/docs/CURRENT_SENSE.md` §4, "Measured noise
   floor — RUN 2026-09-18" — see that section rather than repeating the
   numbers here.
1. **Editable calibration fields** (KilnFW page + SaftyFW params). **ESP
   side: done (2026-09-06).** `A_fs[3]`/`zero_mv[3]` are ASKED fields on
   `safety_commissioning_page.html` (section 3), each with its own "Apply"
   button posting to the new `POST /api/safety/commissioning/ct_cal`
   (`ch=<0-2>&a_fs=<v>&zero_mv=<v>&commit=1`); that handler converts
   (`safety_ct_cal_convert()`: `k_ct_v_per_a = 1/A_fs`,
   `zero_counts = zero_mv/1000 * gain * 4096/3.3`, `gain` read from this
   same cache's committed `gain[ch]`, falling back to the 0.715 physical
   default) and stages the two derived fields through the existing generic
   `SET_PARAM`/`COMMIT_CONFIG` path (`safety_cfg_http.c`'s `apply_pairs()`)
   -- Pico storage/`config_store` unchanged, no migration. A `source` marker
   per channel (manual | sweep | auto-zero) lives in a new ESP-local NVS
   record (`safetyctcal`, `safety_cfg_store.c`); manual always wins over the
   sweep (`zone_sweep_plan_k_ct()` skips a manually-calibrated channel
   entirely, so the sweep's own k_ct derivation is never even computed for
   it). `A_fs` accepts any probe rating; range checks are sanity only:
   `A_fs` in [0.1, 2000], `zero_mv` in [-200, 200]. The derived
   `k_ct_v_per_a`/`zero_counts` stay visible, readonly, next to the inputs.
   The step-2 auto-zero action itself (writing `source = auto-zero`) is not
   part of this pass -- `safety_cfg_store_set_ct_cal_input()`'s AUTO_ZERO
   path exists and is host-tested, but nothing calls it yet.
2. **Auto idle offset: done (2026-09-06).** A commissioning action, not a
   background task. `current_sense_recalibrate_zero()` itself is still
   never called directly from the wire path -- it blocks its caller for the
   whole measurement, which link_task's 30 ms watchdog check-in deadline
   cannot survive (see `LINK_PROTOCOL.md`'s `SAFETY_CMD_CT_AUTO_ZERO_BEGIN`
   comment). Instead: three new wire commands, additive, no
   `KILNLINK_PROTOCOL_VERSION` bump needed (brand-new ids, same "old peer
   simply never sends/sees it" shape as `SAFETY_CMD_ROLLBACK_RESULT`) --
   `SAFETY_CMD_CT_AUTO_ZERO_BEGIN` (0x26, ESP→Pico, arms one channel),
   `SAFETY_CMD_GET_CT_AUTO_ZERO` (0x27, ESP→Pico, poll), `SAFETY_CMD_CT_
   AUTO_ZERO_STATUS` (0x28, Pico→ESP reply). Pico side:
   `current_task.c` accumulates one raw ADC sample per its own normal
   period (`current_task_ct_auto_zero_begin()`/`_poll()`, 200 samples ≈10 s
   at `SAFTYFW_PERIOD_CURRENT_TASK_MS`) rather than blocking link_task or
   itself for the whole measurement; `link_task.c` only dispatches the
   three frames. ESP side: `POST /api/safety/commissioning/ct_auto_zero`
   (`ct_auto_zero_post_handler()`, `safety_cfg_http.c`) checks every
   precondition on a fresh read each request (no server-side session
   between preview and confirm) -- every relay reported off for ≥ 5 s
   (`kiln_io_relays_off_ms()`, new tracker in `kiln_io.c`/`.h`), K4 closed,
   no trip latched, no profile/autotune running, and manual-wins-unless-
   override_manual=1 -- then sends `CT_AUTO_ZERO_BEGIN`, polls `GET_CT_
   AUTO_ZERO` (blocking the httpd worker thread for the ~10-12 s
   measurement -- acceptable since httpd runs multiple worker threads,
   unlike the Pico-side link_task/current_task blocking hazard this whole
   async design exists to avoid), and refuses with a reason if the measured
   zero differs from the stored value by more than 100 mV-at-probe. `confirm=1`
   re-runs the SAME checks and measurement before committing through the
   exact `ct_cal_post_handler()` path (Pico first, ESP-local record only on
   success) with `source=auto-zero`. `safety_commissioning_page.html` gained
   an "Auto-zero" button per channel (measure → `kcConfirm()` preview →
   confirm → commit). Host tests: `test_ct_auto_zero.c` (CommonFW, the three
   codecs), `test_safety_cfg_http.c` (`ct_auto_zero_check_preconditions()`
   -- each refusal, the 5 s boundary, manual-wins, a negative test proving
   the K4 check is not vacuous -- and `ct_auto_zero_counts_to_mv()` at two
   front-end gains), SaftyFW's existing suite (2328/2328, unaffected).
3. **`ct_topology`**: new commissioning question (per_zone | summed). In
   summed mode the sweep records per-zone normals from channel 3 alone
   (`normal_a[zone] = sum(with zone on) - sum(idle)`), skips the
   channel-map check, and S14 compares channel 3 against the sum of normals
   of commanded-on zones. `i_present_a` auto-derives as half the smallest
   NONZERO zone normal unless set by hand (a zone commissioned at 0 A is
   skipped, never allowed to drive the shared threshold to 0). Add an
   **under-current** warn (open heater: commanded sum minus measured > 0.7x
   that zone's normal for 30 s) — the deficit is one shared-CT scalar
   checked against each commanded zone's own threshold, so it identifies
   "one of the commanded zones," not necessarily every zone whose bit sets.
   WARN-only like S4/S14 until it has been seen on hardware.

   **Pico side: done** (`safety_guards.c`/`.h`, `safety_core.c`,
   `config_store.c`/`.h`, `config_params.c`/`.h` — CT_COMMISSIONING_PLAN.md
   commit). `ct_topology` is param `0x031F` (U8, 0=per_zone/1=summed,
   `CHECK_U8_MAX(1u)`, no `fields_set` gate — per_zone is already the safe
   silent default). New guard **S15** (WARN-only, per zone, `0.7×` that
   zone's `i_normal_a` sustained 30 s, hardcoded not config-exposed) covers
   the under-current/open-heater case; S14 gains a summed-topology branch
   (channel 2 vs. the sum of `i_normal_a[]` for zones commanded on right
   now; channels 0/1 report not-fitted via `amps_valid`). `i_present_a`
   auto-derivation lives in `config_params_finalize_i_present_a()`, called
   at `COMMIT_CONFIG` next to the existing `ct_channel_map` finalizer, gated
   by a new non-`fields_set` marker byte `i_present_a_manual` (fields_set is
   full — see config_store.h). No `KILNLINK_PROTOCOL_VERSION` bump: SET_
   PARAM/GET_PARAM/GET_CONFIG_PAGE are already generic by param id, so a new
   id needs no frame/version change. Host tests: `test/test_safety_guards.c`
   (summed-topology S14 + new S15 cases, quantized-counts negative test) and
   `test/test_config_store.c`/`test/test_config_params.c` (topology pack/
   unpack round-trip, legacy-record decode, `i_present_a` auto-derive).

   **ESP side: done (2026-09-06)**, except the dashboard/LCD real-amps
   display, which is step 4. `ct_topology` is a new commissioning question
   on `safety_commissioning_page.html` (enum, per_zone/summed), forwarding
   `SET_PARAM` 0x031F through the same generic apply_pairs()/COMMIT_CONFIG
   path every other field on that page uses. `zones_current_sweep_task.c`'s
   `zone_sweep_task_record_ct_channels()`/`zone_cfg_committed_ct_topology()`
   read the committed topology fresh at the start of every sweep; in summed
   mode the one-relay-one-channel derivation (`GUARD_TEST_MATRIX.md` §3.3)
   is skipped entirely (`s_ct_derive` is left untouched, so the map/k_ct
   pushes that follow naturally no-op) and `normal_a[zone] = sum(with zone
   on) - sum(idle)` is derived from channel 3 (index 2) alone, using an idle
   baseline sampled once per sweep with every relay off
   (`zone_sweep_summed_normal_a()`, pure and host-tested). `safety_trip_
   words.h` gained `safety_warn_words_short()`, decoding the Frame B
   `warn_mask` bits (S4/S5/S9/S10/S12/S13/S14/S15) that had no name anywhere
   on this side before -- wired into `safety_page.html`'s "Warn mask" row.
   Dashboard/LCD real-amps display and the "channels 0/1 read not fitted"
   presentation remain step 4, not touched by this pass.

   **Superseded by the per-zone selector surface (2026-09-18,
   `docs/CT_CHANNEL_MASK_PLAN.md`).** The commissioning question an operator
   answers is no longer one whole-kiln `ct_topology` enum but three per-zone
   ones: `zone_ct_channel[0..2]` (params `0x0320-0x0322`, U8, 0-2), asked on
   `safety_commissioning_page.html` immediately below the `ct_topology` row
   as "zone N CT channel". The two topologies this step shipped are just two
   answers to those three questions — per-zone is `{0, 1, 2}`, summed is
   `{2, 2, 2}` — and a genuine split (zones 0 and 1 sharing one clamp, zone
   2 on its own) is now expressible, which `ct_topology` could not say at
   all. `member(ch) = { z : zone_ct_channel[z] == ch }`; a channel with two
   or more members is shared, and every zone on a shared channel gets the
   idle-subtracted derivation this step gave the summed case, while a zone
   alone on its channel keeps the per-zone path.

   `ct_topology` is NOT removed and is still written. The map is honoured
   only when it is committed all-or-nothing — every zone answered, `set`,
   and in range — and while it is not, both the Pico's S14/S15 and the ESP's
   sweep run the `ct_topology` branches exactly as this step built them. That
   is what leaves every already-commissioned board's behaviour unchanged and
   what keeps a board mid-commissioning, with a half-entered map, from being
   guarded against a mapping nobody finished answering. The three rows are
   also disabled and cleared alongside `ct_channel_map` when `ct_installed`
   is answered "No".

   Bench-gated, not software-gated: which channel each zone is actually read
   on is a fact about the wiring loom, so nothing in software can check the
   answer. A wrong answer points a guard at another zone's conductor —
   `docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md` is the (still PLANNED)
   energize-one-zone-at-a-time check that would catch it.
4. **Real-amps display: done (2026-09-06)**, ESP + web dashboard + LCD +
   PcTools (`safety_get_status`, `devices_safety.py`/`mcp_server_safety.py`,
   done 2026-09-06 -- see below).
   `dashboard_status_http.c`'s `GET /api/status` gained three fields after
   `ct_counts`: `ct_topology` ("per_zone"/"summed", read straight off the
   ESP's own committed safety-cfg cache, param 0x031F -- same "unset reads
   as per_zone" convention `zones_current_sweep_task.c` already uses),
   `ct_fitted` (per-channel bool; false for channels 0/1 in summed mode,
   true otherwise), and `ct_summed_attrib_zone` (0-based zone index or null
   -- null covers both "not summed" and "not exactly one zone commanded on
   right now", read from `ds->relay_on[]` against each zone's committed
   `relay_mask`, so it also works with relays flipped by hand outside a
   profile run). `DASHBOARD_JSON_STATUS_BUF_SIZE` raised 5120 -> 5248 (+128)
   for the ~87B these three fields cost worst-case; `test_dashboard_json.c`'s
   `render_worst_case_status_json()` mirror updated in the same commit --
   measured worst case 5040B against 5248B, 208B real headroom.
   `main_page.html` shows a new "Current sense" card (one badge per CT
   channel: a real `X.XX A` figure, "not fitted" for a `ct_fitted[i]==false`
   channel, never a fabricated `0.00 A`) and, in summed mode, an amps tag
   next to whichever single zone `ct_summed_attrib_zone` names (every other
   zone, and every zone when zero or 2+ are on, shows "-" instead of a
   guess). `ui_page_diagnostics.c`'s Safety & Board Health page appends the
   summed channel's amps to the existing Power row (no new row -- that
   page's own budget comments document it already runs 237-257px of its
   ~267px no-scroll budget; per_zone topology's three independent channels
   have no room for a per-channel breakdown on this screen and keep the old
   behavior unchanged) -- no new theme colors, existing accent token only.
   **PcTools side: done (2026-09-06)**. `SafetyStatus.describe()`
   (`devices_safety.py`) now takes an optional `ct_fitted`/
   `ct_summed_attrib_zone` pair -- `None` (default) keeps the old
   unconditional-raw-floats behavior since `amps_valid` still never crosses
   the wire (`GET_STATUS`'s V1/V2 payload only ever carried the three raw
   current floats); when supplied, a `False` entry renders "not fitted"
   instead of a fabricated amps figure, and the attributed zone renders as
   "zone N" or "-". `mcp_server_safety.safety_get_status()` supplies these
   from `GET /api/status`'s `ct_fitted`/`ct_summed_attrib_zone` (preferred,
   matches the ESP dashboard/LCD exactly) and falls back to `GET /api/
   safety/commissioning`'s own `ct_topology` param when `/api/status` is
   unreachable or omits the field (no per-zone attribution available from
   that source, so it always reads "-" in the fallback path); with neither
   source reachable it keeps printing raw amps, topology unknown. Tests:
   `tools/PcTools/tests/test_ct_fitted_display.py`.
5. **Docs: done (2026-09-06).** `CURRENT_SENSE.md` §0.1/§0.2/§5 now describe
   both topologies (model, calibration sources, auto-zero preconditions/
   refusal, S14/S15, the sweep's cache-unfetched refusal and
   `summed_unmeasured_mask`); `GUARD_TEST_MATRIX.md` §3.3 covers the same
   sweep behaviour; `ROADMAP.md` row M and `CONFIG_REFERENCE.md` (`ct_topology`,
   `0x031F`) updated. Finished detail lives in those files, not here.
6. **Bench** (owner present): steps 0 and 2 on the test kiln, then one
   heating run to record the three zone normals and check the 70 mA figure.

   **6a. Set commissioning to match the fitted hardware (do this before the
   heating run above).** The commissioning config as of 2026-09-06 still
   reads `ct_installed=0`/`ct_topology=per_zone`, but a summed-heater
   split-core CT has been physically fitted since 2026-09-05 (RP2040
   GPIO28, 1A:1V, ~+59mV idle offset -- `CURRENT_SENSE.md` §4,
   `project_ct_sensor_on_gpio28`). Left at the stale values, the sweep runs
   the per-zone `ct_channel_map` derivation against a topology that does
   not exist, and S14/S15 judge the board against per-channel CTs that are
   not there. Correct values for this bench: `ct_installed=1`,
   `ct_topology=1` (`summed` -- `CONFIG_STORE_CT_TOPOLOGY_SUMMED`, the only
   other enum value is `0`/`per_zone`; there is no third topology).

   Both params (`ct_installed` 0x0109, `ct_topology` 0x031F) go through the
   existing generic `SET_PARAM`/`COMMIT_CONFIG` path documented in step 1
   above (`safety_cfg_http_client.apply_safety_fields()` --
   GET-then-POST-then-read-back-verify, never trusts the POST's own
   `{"ok":true}` alone). `4f1b9a4f` (now flashed, running at `fc6d30f8`)
   fixed a masked `hal_flash_program()` failure in exactly this write path
   (`config_store_write()`), so a write attempted before that fix could
   have reported success while nothing actually persisted -- this bench is
   past that point. The write is also refused outright while the Pico is
   ARMED (relay_owner: config writes rejected while ARMED); it only lands
   during the 60s post-reset GRACE window, same rule `safety_set_rate_guard`
   documents.

   Exact command: `kiln_call(name="safety_set_commissioning_fields", args={"fields": {"ct_installed": 1, "ct_topology": 1}})`
   -- the MCP-facade tool (`tools/PcTools/src/kilnctrl/mcp_server_safety.py`)
   that wraps this exact GET/POST/read-back-verify cycle, refuses outright
   during a live firing or autotune run, and fails loudly naming the field
   on a read-back mismatch. (Fallback if the tool is unavailable: the raw
   `safety_cfg_http_client.apply_safety_fields(host, fields, verify=True)`
   snippet this step used to document.)

   If it refuses with "ARMED" in the reply: `debug_reset(peer="pico")`,
   then re-run the same call within 60 seconds.

   Readback verification (independent of the above -- confirm the board's
   own view, not just the client's): `GET /api/safety/commissioning` (or
   `kiln_call(name="safety_get_commissioning")`) and check `ct_installed`
   reads `set=true, value=1` and `ct_topology` reads `set=true, value=1`.

   After this lands, before any current flows: `S14` (over-current, WARN)
   reports "inactive" on channel 2 until a zone is commanded on AND that
   zone's `i_normal_a` has been measured (it has not yet -- that is this
   step's own heating run); channels 0/1 report not-fitted/inert
   permanently in summed mode. `S15` (new, under-current/open-heater WARN,
   per zone) stays fully inert for the same reason -- no zone normal
   measured yet. Both guards report DORMANT-not-armed via
   `safety_get_commissioning`'s description helper
   (`mcp_server_safety._describe_commissioning()`) until the heating run
   populates `i_normal_a[]`; that is expected, not a fault. Code review of
   `safety_guards.c`'s summed branch (as of this pass) found no defect --
   S14/S15 sum `i_normal_a[]` only over zones currently commanded on and
   skip entirely (never a false pass) when any commanded zone's normal is
   unmeasured; nothing divides by a fixed channel count that would be wrong
   for one shared CT.

   **6a done (2026-09-08).** `safety_set_commissioning_fields({"ct_installed":
   1, "ct_topology": 1})` written and read-back verified; no firing/autotune
   in progress at the time (`profiles_get_exec_status` state=0), so the write
   was not blocked by ARMED. Full before/after param dump compared field by
   field: only `ct_installed` (0->1) and `ct_topology` (0->1) changed;
   `abs_max_temp_c` (80), `max_rate_c_per_min` (33.3), `tc_type` (3),
   `tc_offset_c` (0) and every other one of the ~65 stored params were
   unchanged. `commissioned` flipped `true`->`false` as a result -- expected,
   not a regression: `ct_channel_map[0..2]` are still unset, which
   `safety_get_commissioning` already lists as still required. S5 was
   latched-tripped (stuck-DRDY thermocouple, trip_reason 5 per
   `safety_get_diag`) throughout and was left alone -- the commissioning
   write is independent of that trip and was not blocked by it. `control_get_zones`
   shows all three zones' `ct_mask=1`, consistent with one shared summed
   channel. S14/S15 remain DORMANT (no `i_normal_a` measured yet); this
   step does not include the heating run above or any `ct_cal`/auto-zero
   calibration, which stay outstanding bench steps.

   **CT zero calibration done (2026-09-08), separately from `6a`.** Root
   cause of the latched trip found by
   `docs/audits/s6a_unclearable_trip_2026-09-08.md`: the board's latched
   trip was S3 (`LOAD_STUCK_ON`), not S6a as first reported -- channel 2
   (GPIO28/ADC2, the summed CT) still read 59-66 raw counts against the
   25-count safety margin because it had never been zero-calibrated, so S3
   correctly saw "current present with no relay commanded" and latched.
   Preconditions confirmed before touching anything: `profiles_get_exec_status`
   state=0 (idle, no run in progress), `safety_get_status`/`io_read` showed
   all four relays off (`relays: 0`), link up and stable. Raw counts before:
   a 15s capture (all relays off) read ch0 mean=16.63 std=0.481, ch1
   mean=17.00 std=0.000, ch2 mean=63.19 std=1.320 (min 62 / max 66) --
   consistent with the 2026-09-06 60s baseline in `CURRENT_SENSE.md` §4
   (ch2 mean 66.89 std 4.678). No `ct_auto_zero` MCP tool exists yet, and
   the ESP's `POST /api/safety/commissioning/ct_auto_zero` handler itself
   refuses without an already-committed `a_fs` to convert against, which
   channel 2 has never had -- so the zero was written the other documented
   way: `safety_set_commissioning_fields({"zero_counts[2]": 63})`, the
   same `zero_counts[2]` field (`config_params.c` id `0x0304`) that
   `current_sense.c`'s conversion actually reads (`CURRENT_SENSE.md` §5.2),
   not the separate legacy `ct_cal[3]`/`safety_set_ct_cal` gain-offset
   record, which stayed at "uncalibrated" throughout and was left alone.
   Read-back confirmed `zero_counts[2]=63`. Only that one field changed --
   `abs_max_temp_c` (80), `max_rate_c_per_min` (33.3) and `tc_source` all
   read unchanged in `safety_get_commissioning` immediately after. Gain
   (`k_ct_v_per_a[2]`/`gain[2]`) was deliberately left untouched -- that is
   a bench step needing a known load with the owner present, and stays
   outstanding; `current_a[2]` still reads `0.00 A` for that reason,
   expected. Raw counts after settled at 58-70 (mean ~61-65 across three
   post-write captures), consistent with a genuine near-zero reading once
   the offset is subtracted. `safety_clear_trip()` cleared S3 immediately
   and it stayed clear (`safety_get_status`: "not tripped") through a 65s
   poll window with no relays touched or heating attempted. Remaining
   blockers to firing: CT gain calibration (bench step, owner present,
   known load), and `ct_channel_map[0..2]` still unset -- the reason
   `safety_get_commissioning` still reports `commissioned=False`.

## Legacy `ct_cal[3]` write surface removed (2026-09-08)

The incident above ("not the separate legacy `ct_cal[3]`/`safety_set_ct_cal`
gain-offset record") was not a one-off near-miss -- the tool existed
specifically to invite it, since its name and docstring both said
"commission the CT" while it only ever wrote a display-only correction
(§ CURRENT_SENSE.md 5.3 has the full map of readers/writers). Owner decision:
remove the write surface entirely rather than leave it as a second attempt
waiting to happen.

- **Removed:** `devices.safety_set_ct_cal()`, `SafetyClient.set_ct_cal()`,
  the `safety_set_ct_cal` MCP tool. `safety_get_ct_cal` stays, read-only.
- **Record layout unchanged.** `config_store.h`'s `ct_cal[3]` bytes stay
  exactly where they are in the persisted record (both A/B sectors, both
  processors already flashed) -- removing them would be a migration, and
  the abs-max ceilings and `tc_type` sharing this record are not worth that
  risk for a field that can simply go unwritten from now on. Firmware's
  `SAFETY_CMD_SET_CT_CAL` handler and `ct_amps_cal_apply()` are untouched
  and still compiled in; with no PC-side caller left, the wire command is
  unreachable, which is the point.
- **Guard:** `tools/check_ct_cal_write_surface.ps1` (part of the standing
  `tools/run_all_checks.ps1` suite) fails if `SAFETY_CMD_SET_CT_CAL` or a
  `set_ct_cal(...)` call reappears in `kilnctrl/safety.py` or
  `kilnctrl/mcp_server_safety.py`. Negative-tested by hand (a stub
  `set_ct_cal` method was inserted, the check was confirmed to fail
  naming that exact line, then the stub was removed again).
- **`ct_auto_zero` was already correct, not a second trap.** Its refusal
  without a committed `A_fs` (`ct_auto_zero_check_preconditions()`,
  `safety_cfg_http.c`) is deliberate, not a bug: an earlier version *did*
  fabricate `a_fs=1.0` for a zero-only measurement and silently committed a
  wrong gain, which was worse than refusing. The correct, and only,
  procedure for a channel with no committed `A_fs` (channel 2, today) is:
  enter a real `A_fs` manually first (commissioning page, or `POST
  /api/safety/commissioning/ct_cal`), *then* run auto-zero to refine the
  offset. No code change made here; this is a documentation gap, now
  closed.
- **Surviving calibration path, end to end:** operator enters `A_fs`
  (probe rating) and `zero_mv` (or runs auto-zero after `A_fs` exists) on
  the commissioning page -> ESP's `ct_cal_post_handler()`
  (`safety_cfg_http.c`) converts to `k_ct_v_per_a`/`zero_counts`
  (`safety_ct_cal_convert()`) -> staged via the generic `SET_PARAM`/
  `COMMIT_CONFIG` path -> Pico's `config_store.c` persists them ->
  `current_sense.c` reads them directly for both the §5 amps conversion
  and `current_presence_is_flowing()`. This is a single, live path with no
  second calibration stage in front of the guards. **Works today for
  channel 2 (the summed topology channel)** for both halves: the zero half
  (`zero_counts[2]=63`, confirmed holding) and, as of 2026-09-18, the gain
  half too — the live board's `GET /api/safety/commissioning` reads
  `k_ct_v_per_a[2] = 1`, `gain[2] = 0.715` (`CURRENT_SENSE.md` §5.2's
  2026-09-18 bench-state note), so `amps[2]` no longer reads a hard
  `0.00 A`. S14/S15 remain DORMANT, but for the separate, still-true reason
  that `i_normal_a` (the per-zone expected-current baseline) is unmeasured —
  not because the gain was never entered.

## Step 6 CLOSED by owner decision, 2026-09-19 — software walkthrough only

The owner decided this bench's ~4 W fixture cannot record `i_normal_a` and
that no real load will be attached now, so step 6's remaining bench run is
closed as a software walkthrough rather than left open pending hardware that
is not coming. The walkthrough ran end to end against the live board
(COM14, link protocol v12, FW `1baa828c` built 2026-09-18 22:20:20Z):

- **Commissioning state read back:** `ct_installed=1`, `ct_topology=1`
  (summed); `ct_channel_map` is not applicable in summed mode. Channel 2's
  calibration: `k_ct_v_per_a[2]=1`, `zero_counts[2]=63`, `gain[2]=0.715`;
  channels 0/1 hold their unset `k_ct_v_per_a=0`/`zero_counts=0` defaults
  and are not-fitted in this topology. `i_present_a=2` (auto-derived).
  `i_normal_a` was not measured on any channel (`safety_get_commissioning`:
  S14 ch0/ch1/ch2 all DORMANT; S15 z0/z1/z2 all DORMANT).
- **Live status:** `safety_get_status` reported the link up, SaftyFW armed
  and not tripped, the safety thermocouple valid at 25.23 C, currents
  "not fitted, not fitted, 0.02 A" (raw ADC counts 16, 17, 85 -- channel 2
  near its zero offset of 63, consistent with no load), and "ct zone: -"
  (no single zone attributable, as expected with no zone commanded on).
- **Current sweep run** (`zone_current_sweep_start(confirm=true)`, all
  three zones, ~5 s/zone): finished `state=done`, `zones_done=3/3`, but
  `summed_unmeasured_mask=7` (all three zones) and
  `k_ct_reason='an incomplete pass -- some zone never cleared the noise
  floor, so k_ct was not set'`, `nameplate_reason='zone 0: no measured
  normal current for this zone yet'`. The sweep tool's own status message
  states this outcome is expected on this project's ~4 W bench fixture,
  whose per-zone current (~23 mA alone, ~70 mA all three) sits below the
  firmware's 0.045 A noise floor. **Verdict: INCONCLUSIVE**, exactly as
  predicted -- no `i_normal_a` was pushed for any zone
  (`i_normal_pushed_mask=0`).
- **Post-sweep confirmation nothing changed:** `safety_get_ct_cal_raw` and
  `safety_get_commissioning` read back byte-identical to their pre-sweep
  values (config CRC 21975 unchanged both before and after), no trip
  latched, and `safety_get_status` still reported "armed (relay_owner not
  tripped)" with all relays off.

**A real load is required before S14/S15 can ever arm on this fixture.**
No amount of further software work on this bench changes that; the sweep
already runs correctly and reports INCONCLUSIVE honestly rather than
fabricating a normal-current baseline from noise. This closes step 6 and,
with it, the whole `CT_COMMISSIONING_PLAN.md` step list -- steps 0-5 were
already done, and step 6 is now closed by owner decision rather than
technically completed.

## Order and ownership

Step 0 first (it may make step 3's under-current warn moot on this bench).
Steps 1-2 are independent of 3. All SaftyFW edits wait until the S8 change
(`safety_guards.c/.h`, `test_safety_guards.c`) is committed; `current_sense.c`
and `current_task.c` are free. Host tests must feed quantized counts, not
ideal amps (idealized-input class).
