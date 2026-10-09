# WEB bench judge contract audit (2026-10-09)

Scope: the read-only WEB bench judges in
`tools/PcTools/src/kilnctrl/bench_test/cases_web_*.py` on origin/dev
(`a7433500`). Each field a judge reads was checked against the firmware
route that emits it: the field name, its type, its units, and the
conditions under which it is emitted. Three questions were asked of
every judge:

- Would it PASS vacuously?
- Would it FAIL on a healthy board?
- Does it check something the route never emits?

Two bug classes that have already been fixed were checked for remaining
instances:

- gzip/406: fixed for authenticated GETs in `3164aab2`.
- The age-based thermo `stale` threshold: fixed in `6076535a`.

This is a read-only audit. No judge was changed. Paths below are
relative to `tools/PcTools/src/kilnctrl/bench_test/` (judge side) and
`firmware/KilnFW/App/drivers/http/` (firmware side) unless a path is
given in full.

## HIGH

### H1. WEB-ZONE-11 FAILs a healthy board after HP-01 (wrong field names)

- **Judge:** `cases_web_prof.py:999` requires
  `isinstance(zz.get("method"), (str, int))` for every zone with
  `tuning_valid` true. It also checks `zz.get("settled", False)`.
- **Firmware:** `zones_http_get.c:613-618` emits `tuning_method` and
  `tuning_settled`. No per-zone `method` or `settled` key exists in
  `/api/zones`.
- **Effect:**
  - Once any zone carries an accepted tuning (`tuning_valid` true), the
    judge's `method` check always sees None and FAILs. ZONE-11 runs
    straight after HP-01 in nightly (`registry.py`, `_NIGHTLY_ORDER`).
  - The `settled` half is vacuous, because it defaults to False and
    therefore always passes.
- **Fix:** read `tuning_method` and `tuning_settled`, and require both.

## MEDIUM

### M1. WEB-ZONE-06 misses the relay autotune states (vacuous PASS)

- **Judge:** `cases_web_prof.py:796` sets `_ACTIVE_AT` to
  `("stepping","running","settling","analyzing","waiting","preparing","tuning","starting")`.
  Lines 817-823 FAIL only when the state is in that tuple.
- **Firmware:** `/api/autotune` `state` comes from
  `autotune_state_name()`, `dashboard_json.c:282-294`. It emits `idle`,
  `settling`, `stepping`, `relay_approach`, `relay_cycling`, `done`,
  `aborted` and `unknown`.
- **Effect:**
  - A relay-method autotune that is still running (`relay_approach` or
    `relay_cycling`) reads as finished, so the judge PASSes.
  - Five of the eight names in the judge's tuple are never emitted.
- **Fix:** treat every state outside `{idle, done, aborted}` as active.
  That is the convention `cases_web_dash.py` already uses.

### M2. WEB-DASH-10 checks the wrong route and a comment

- **Judge:** `cases_web_dash.py:485-499`.
  - It reads `recovery_mode` from `GET /api/ota/esp/status`.
  - It requires the static tokens `kc-recovery-banner`,
    `/api/ota/esp/status` and `st.recovery_mode` in `/app.js`.
- **Firmware:**
  - Route tier review LOW-3 moved `recovery_mode` onto `/api/status`
    (`dashboard_status_http.c:872-874`).
  - `/api/ota/esp/status` is now ADMIN (`ota_http_esp.c:578`, `:657`,
    `:676`).
  - The page banner polls `/api/status` (`app.js:1663-1669`).
  - The string `/api/ota/esp/status` survives in `app.js` only in a
    comment (`app.js:1574`).
- **Effect:**
  - The judge validates a field the banner no longer reads, so it is
    vacuous for the banner's real input.
  - Deleting that comment would FAIL a healthy board.
  - With web auth on, the admin GET also depends on a session.
- **Fix:**
  - Read `recovery_mode` from `/api/status`.
  - Use `/api/status` (or the actual fetch call) as the static token.

### M3. WEB-ZONE-03 FAILs when fewer than four relays are configured

- **Judge:** `cases_web_prof.py:708-710` requires
  `len(relay_types) == relay_count`.
- **Firmware:**
  - `relay_types` is always `KILN_IO_RELAY_COUNT` (4) entries
    (`zones_http_get.c:353-358`; `kiln_io.h:65`).
  - `relay_count` is the configured count. Validation rejects only
    values above the maximum (`firmware/KilnFW/App/drivers/persist/zones_config_json.c:248`).
- **Effect:** a board configured with fewer than four relays FAILs while
  healthy. This is latent on the 4-relay bench.
- **Fix:** require `len(relay_types) >= relay_count`, the way PROF-10
  already does for `relay_names`.

### M4. WEB-WIFI-02 FAILs on a transient scan miss

- **Judge:** `cases_web_misc.py:58` requires `saved`, `in_range` and
  `connected` all true for the bench SSID.
- **Firmware:** `wifi_provision_http.c`:
  - Lines 455-462: on a transient scan failure, `/networks` degrades to
    saved-only rows with `in_range:false`.
  - Lines 490-525: the scan list is capped at 20 entries. A saved SSID
    outside that list also reads `in_range:false`.
- **Effect:** a connected, healthy board can FAIL.
- **Fix:** return INCONCLUSIVE when `connected` is true and `in_range`
  is false.

## LOW

- **L1. WEB-OTA-07 checks a field the route never emits.**
  - `cases_web_diag.py:522` accepts `boot_button_bypass_active` when it
    is None or False.
  - That field was retired on 2026-09-29 and `/api/status` no longer
    emits it (`dashboard_status_http.c:1016-1022`).
  - That half of the judge always passes. Drop it, or replace it with
    whatever now carries the same meaning.
- **L2. DIAG `mutating_gate` state list is inconsistent.**
  - `cases_web_diag.py:62` lists `complete`, `failed` and `accepted`,
    none of which `autotune_state_name()` emits.
  - This is harmless, because the gate fails closed.
  - It differs from the dash gate (`cases_web_dash.py:27`) and from the
    safety `_mutating_gate`. Share one helper.
- **L3. WEB-DIAG-04's comparison with the FL-07 snapshot is brittle.**
  - `cases_web_diag.py:179-215` compares `file_count`, `status` and the
    `items` names with the snapshot FL-07 took.
  - DIAG-04 is not in nightly. In `full` and `web` it runs after cases
    that can legitimately create cfg files (firing stats after heat, for
    example), so a healthy board could FAIL on `file_count`.
  - **Fix:** drop `file_count` from the stable set, or compare only when
    no writing case ran in between.
- **L4. WEB-DIAG-05 ignores how the safety thermocouple is installed.**
  - `cases_web_diag.py:243` FAILs on `not_converting`.
  - It does not consult the safety TC `not_installed` or
    `tc_is_separate_sensor` flags. A board whose safety TC is absent or
    borrowed could FAIL while healthy.
  - Gate on those flags first.
- **L5. WEB-DIAG-02 records a misleading value.** The observed dict
  stores `acknowledged: bool(present)`. This is cosmetic.
- **L6. WEB-DASH-09 and inactive zones.**
  - The firmware prints `nan` for inactive zones. Python's
    `float("nan")` accepts it, so the judge is fine.
  - The firmware comment claims the field is empty, which is wrong.
- **L7. WEB-DASH-11 never checks `banner_expected`.** It is computed at
  `cases_web_dash.py:512` and only recorded.
- **L8. WEB-DASH-03 and DASH-06 do not handle `unknown`.**
  `exec_state_name()` can emit `unknown` (`dashboard_exec_http.c:38-48`).
  The judges should name it explicitly rather than fall through.
- **L9. Silent truncation in two routes.**
  - **DASH-07:** if the `last_run` snprintf overflows, the firmware
    omits the field (`dashboard_exec_http.c:85`). The judge then reports
    the field missing, not truncated.
  - **KCFG-05 (`cases_web_misc.py:498`):** `/api/readiness` can drop
    items at its size cap and sets `dropped`. The judge reports "no
    item" and FAILs. It should be INCONCLUSIVE when `dropped` is set.
- **L10. WEB-BAK-03 export determinism.**
  - The export includes the `relay_cycles` wear counters.
  - The gate does not check manual aux outputs, so a relay toggle in the
    5 s gap would make the two exports differ.
- **L11. Zone loops pass vacuously when `zones` is missing.**
  - Affected: `cases_web_prof.py:525-527`, `:629`, `:694`, `:734`,
    `:991-993`. They iterate `(z.get("zones") or [])[:tc]`.
  - A response with no `zones` array therefore passes.
  - The firmware always emits `MAX31856_CHANNEL_COUNT` entries
    (`zones_http_get.c:386`), so the risk is low.
  - Assert `len(zones) >= tc` anyway.
- **L12. Remaining gzip-class gap.**
  - Authenticated POST replies and the unauthenticated raw POST bodies
    (`_post_json` / `_post_raw` in `cases_web_rw.py`) are still not
    gzip-decoded.
  - This is the same class as `3164aab2`. DEV_REVIEW_9 LOW-2 already
    records it.
  - No current judge reads a gzip-encoded POST reply.

## Contracts verified OK

| Judge | Firmware evidence |
|---|---|
| DASH-02 | `profiles_catalog_http.c:577-633` |
| DASH-04 | `profile_feasibility.c:114-123` |
| DASH-07 | `run_state.c:213-224`, `:481-508` |
| DASH-08 | `dashboard_status_http.c:737-759`, `main_page.html:1966-1979` |
| DIAG-03 | `partition_info_http.c:55-98` |
| DIAG-04 keys | `cfg_fs_status.c:173-245` |
| DIAG-06 | `dashboard_status_http.c:404-430`; `RELAY_CYCLES_COUNT`=5 (`relay_cycles.h:51`); `rated` and `percent` are null if and only if the relay is an SSR |
| DIAG-09 | `danger/relay` answers 409 when inactive (`diagnostics_http.c:1236-1255`) |
| DIAG-10 | timing at `diagnostics_http.c:1128-1141`; board temps at `board_temps_http.c:40-54` (null-aware); lwip at `diagnostics_http.c:698-722` |
| OTA-08 | `ota_http_pico.c:704-716` |
| WIFI | `/status` at `wifi_provision_http.c:372-380` |
| SEC-02, SEC-06 | `security_http.c:111-115` |
| BAK-02 | `backup_export.c:343`, `:359`, `:388-403` (zones limited by `thermo_count`, as `/api/zones` is, `zones_config_accessors.c:525`) |
| KCFG-03 and the KCFG snapshot | `kiln_cfg_http.c:172-182`, `:495-518` |
| Readiness item shape and statuses | `readiness_http.c:54-61`, `:186`; `safety_ceiling_match` at `:711` |
| RDY-02 hard keys | `safety_trip` at `readiness_http.c:849`, `crash_report` at `:913`, `recovery_mode` at `:934`, `estop_verified` at `:1109` |
| RDY-04 | `readiness_item` in the 409 body, `dashboard_autotune_http.c:221`, `:268` |
| PROF-09 | builtin bool at `profiles_catalog_http.c:167`, `:199`, `:335` |
| PROF-10 | `relay_names` and `relay_zone_owned_mask` at `zones_http_get.c:315-338`; aux `enabled_mask` at `aux_outputs_http_core.c:222` |
| STIM-02 | `timing_profiles` is dense, `zones_http_get.c:359-375` |
| ZONE-04 | group names `limits`, `relaytiming`, `control`, `guards` and `tc` (`zones_http_post_parse.c:25-27`; `zones_http_get.c:669-673`) |
| ZONE-07 | artifact rows carry `method` (`/api/tuning_recommendations`) |
| ZONE-08 | `dashboard_autotune_http.c:128-189` |
| ZONE-12 | `zones_diag` has three entries, `zones_http_get.c:776-791` |
| ZONE-13 | `zones_http.c:579-636` |
| `zone_type` | enum 0 or 1, `zones_config_accessors.h:1243-1244` |
| SAF-02, SAF-04 | `/api/status` diag keys at `dashboard_status_http.c:340-349` (diag=1 block) and `:540-547`, `:739-766` |
| SAF-03 | the S6a mask `0x20` matches `link_frame_trip_mask_for_reason()` |
| COMM-02, COMM-03, COMM-06 | `safety_cfg_http.c:318-321`, `:339`, `:363` (`live_config_crc`, `cached_config_crc`, `commissioned`, `unset_reporting_reliable`, `ct_cal[].has_value`) |
| WIZ-02 | `setup_progress_http.c:24`, `:92-106` (API version 1, steps `"0"`..`"11"` with `state`/`ts`/`note`); `SETUP_WIZARD_STEP_COUNT` is 12 (`persist/setup_wizard_progress.h:76`); the judge's `_WIZ_STEP_COUNT` is 12 (`cases_web_safety.py:536`) |

**Not covered in depth:**

- WIZ-03..11, SET-02..04 and DISP-02..04 were read but not traced field
  by field.
- The writing round-trip judges in `cases_web_rw.py` were outside this
  read-only scope.
