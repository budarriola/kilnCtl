# WEB bench judge contract audit, part 2 (2026-10-09)

Scope: the WEB bench judges that `WEB_JUDGE_CONTRACT_AUDIT_2026-10-09.md`
did not trace. That means WEB-WIZ-03..11, WEB-SET-02..04 and
WEB-DISP-02..04, plus every writing round-trip judge listed in
`WRITING_IDS`:

- COMM-07, WIZ-03, WIZ-10 and DISP-02;
- DASH-05, DASH-07 and DASH-13;
- DIAG-07, DIAG-08 and DIAG-09;
- SEC-03 and SEC-04;
- KCFG-02 and LOG-02;
- PROF-02..08;
- ZONE-05, ZONE-09 and ZONE-10.

The audit ran on origin/dev `c70b84e8`. For each judge it checked five
things:

- Every key and route the judge uses was traced to the firmware handler.
- Enums and types were checked, and the route's tier was checked in
  `route_tier_table.h`.
- Every write must be restored on every exit path.
- The judge must not PASS vacuously.
- INCONCLUSIVE must be used correctly.

This is a read-only audit. No judge was changed. Paths are relative to
`tools/PcTools/src/kilnctrl/bench_test/` on the judge side and to
`firmware/KilnFW/App/drivers/` on the firmware side.

## Status

All findings fixed in 55cfee20 (strengthened tests in 207acff7); L6 has one residual noted below. Tests: tests/test_bench_test_cases_web_{rw,prof,safety}.py (AuditTwo* classes).

## HIGH

None.

## MEDIUM

### M1. An unmounted cfg partition turns several writing judges into FAIL instead of INCONCLUSIVE

**Fixed (55cfee20).** cfg_guarded (pre-gate on GET /api/cfgfs mounted:false, POST 503 relabels FAIL as INCONCLUSIVE) on DASH-13, DIAG-08, KCFG-02, PROF-02..08.

Many write routes refuse with 503 while the `cfg` LittleFS partition is not
mounted. The refusal comes from `cfg_fs_http_refuse_if_unmounted()` at
`common/cfg_fs_refusal_http.h:26` (it sends 503, lines 12-14). Persist
failures go through `cfg_fs_http_persist_failed()` at `:38`, which answers
503 when the partition is unmounted and 500 otherwise.

An unmounted partition is a legitimate board state. For example, a pending
format waits for the operator to confirm it through
`POST /api/cfgfs/format_confirm`. SET-02 already reports this state as
INCONCLUSIVE, and WIZ-10 maps a 503 to INCONCLUSIVE. The judges below do
not, so they report FAIL, or a misleading "restore did not round-trip"
error, on a board whose only issue is the cfg state:

| Judge | Judge location | Route it writes | Firmware refusal |
|---|---|---|---|
| WIZ-03 | `cases_web_safety.py:606-661` | `/api/settings/tz`, `/api/unit_pref` | `http/settings_http.c:78`, `http/dashboard_settings_http.c:38` |
| DISP-02 | `cases_web_safety.py:965-1011` | display_power | `http/settings_http.c:181` |
| DASH-13 | `cases_web_rw.py:334-365` | `/api/unit_pref` | `http/dashboard_settings_http.c:38` |
| DIAG-08 | `cases_web_rw.py:374` via `_bool_toggle_case` `:300-331`; verdict in `judgments.py:1897` `judge_web_rw_toggle` (has no INCONCLUSIVE path) | `/api/ramp_assist` | `http/diagnostics_http.c:1044` |
| KCFG-02 | `cases_web_misc.py:393-473` | `/api/kiln_configs/clone` | `http/kiln_cfg_http.c:315` (persist_failed, 503 when unmounted) |
| PROF-02 | `cases_web_prof.py:113-155` | `/api/profile/favorite` | `http/profiles_edit_http.c:902` |
| PROF-03..07 | `cases_web_prof.py:160-450` | `POST /api/profile`, delete | `http/profiles_edit_http.c:478`, `:721` |
| PROF-08 | `cases_web_prof.py:463-496` | builtin hide/restore | `http/profiles_edit_http.c:832`, `:870` |

The same is true of ZONE-05 in one respect. It writes
`/api/zones/pid`, which has no cfg refusal, but the persist failure path
there answers 500 `{"ok":false}` (`http/zones_http_pid.c:162-166`) and the
judge FAILs on it. That is correct, because zones still persist to NVS.

**Failure scenario.** A bench board has a cfg format pending, or its cfg
mount failed at boot. A nightly run reports about ten WEB-* FAILs that look
like firmware regressions: "restore did not round-trip", "clone failed" and
so on. The single underlying cause is reported once as INCONCLUSIVE by
SET-02, and nothing in the FAIL lines points to it.

**Suggested shape.** Before writing, read `GET /api/cfgfs` (or reuse
SET-02's probe). If `mounted` is false, return INCONCLUSIVE "cfg unmounted"
without writing anything. Separately, treat a 503 reply from any of these
routes as INCONCLUSIVE.

### M2. WEB-SEC-03 overwrites the admin password without first proving the credential is live

**Fixed (55cfee20).** SEC-03 logs in with the harness credential first; INCONCLUSIVE and no set_web_password on failure.

`cases_web_rw.py:640` calls
`client.set_web_password(username, password)` using
`KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD`. It does not first log in
with that same credential. The firmware applies the write through
`http/security_http.c:435`, which reaches
`http/security_http_core.c:132` (`vt->set_web_password(SECURITY_ROLE_ADMIN, ...)`),
and replaces the stored admin credential outright. The restore in the
judge's `finally` puts back the policy only (web_enabled, lcd_enabled and
the timeouts). It never restores the previous password.

`DEV_FIRMWARE_REVIEW_4_2026-10-09.md` L3 found exactly this hazard and
fixed it for LOG-02 and X-02 only: `_open_window` in
`cases_web_misc.py:545` now logs in before calling `set_web_password`.
SEC-03 was not changed.

**Failure scenario.** Web auth is off, and the owner has since changed the
admin password on the settings page, so the environment variable is stale.
SEC-03 runs, silently sets the password back to the stale value, PASSes
and restores auth to off. The next time the owner turns auth on, their own
password is rejected, and nothing in the bench log says why.

**Suggested shape.** Apply the review 4 L3 pattern. Before the write, log
in with the harness credential and require a 200. If the login fails,
return INCONCLUSIVE ("harness credential is not the live one") and write
nothing.

## LOW

### L1. The rw-module writers have no executor or autotune idle gate

**Fixed (55cfee20).** idle_gate_reason (executor idle, autotune idle/done/aborted) in the toggle cases, DASH-13 and ZONE-10.

DASH-13 (`cases_web_rw.py:338`), DIAG-07 and DIAG-08 (via
`_bool_toggle_case`, `:302`) check only `board_lock.write_refusal(ctx)`.
The dash, diag and misc modules use `mutating_gate` instead
(`cases_web_dash.py:66`, `cases_web_diag.py:48`), which also requires the
executor to read `idle` and autotune to be idle, done or aborted. Neither
firmware route refuses mid-run either: `POST /api/ramp_assist`
(`http/diagnostics_http.c:1042`) and `POST /api/watchdog_cfg` (`:749`)
have no system mode gate.

**Failure scenario.** Another tool starts a firing on a board where the
web suite holds the lock (the lock is a PC-side convention, not a firmware
gate). DIAG-08 then turns ramp assist on and off in the middle of the
ramp, which changes the control behaviour of the live firing for one tick
window.

### L2. The prof module's gate accepts only autotune `idle`

**Fixed (55cfee20).** prof gate accepts idle/done/aborted.

`_mutating_gate()` (`cases_web_prof.py:62-78`) passes autotune only when
the state reads exactly `idle` (`_is_idle_state`, `:58`). Autotune reports
`done` or `aborted` after a tune ends (the module's own `_AT_NOT_ACTIVE`,
`:810`) and keeps reporting it until reboot. The dash and diag modules
were already fixed to accept all three states ("autotune gate treats
idle/done/aborted as not running").

**Failure scenario.** The nightly order runs AT-01 before the WEB suite.
After that, ZONE-05 (`:760`) and ZONE-09 (`:908`) SKIP with "autotune not
confirmed idle" every night, so the PID identity-write and the adaptive
tune toggle never actually run.

### L3. WIZ-06 PASSes vacuously on an empty zones list

**Fixed (55cfee20).** empty zones[] is FAIL (INCONCLUSIVE only when thermo_count is 0).

`cases_web_safety.py:726-748` checks its page markers, then loops over
the zones array and PASSes when no zone lacks an integer `zone_type`. An
empty array yields PASS with the `zone_type` half of the check never
exercised. The first audit added `_short_zones()` (`cases_web_prof.py`) for
exactly this class, but WIZ-06 does not use it.

**Failure scenario.** A firmware regression emits `"zones":[]` (for example
`thermo_count` reads 0 after a bad config load). WIZ-06 PASSes.

### L4. DISP-02's brightness write proves nothing when the board is already at 50

**Fixed (55cfee20).** DISP-02 writes 60 when the snapshot is 50.

`cases_web_safety.py:978` always writes `brightness_percent=50` and checks
that 50 reads back (`:982`). When the snapshot is already 50, the write is
an identity write. A firmware that ignored the field would still PASS.

**Failure scenario.** The display_power POST parser stops applying
`brightness`, on a board whose stored brightness happens to be 50. DISP-02
still PASSes.

**Suggested shape.** Write 50 unless the snapshot is 50, and write 60 in
that case.

### L5. Some first-read failures FAIL instead of INCONCLUSIVE

**Fixed (55cfee20).** first read with status None or 401 is INCONCLUSIVE, nothing written.

DASH-13 (`cases_web_rw.py:341-347`), `_bool_toggle_case` (`:305-311`) and
SEC-03 (`:615-621`) return FAIL when the very first GET fails, before
anything was written. A transient HTTP failure (a 401 after session
expiry, a timeout) then reads as a contract failure of the route under
test. The prof and dash modules return FAIL here too, but they read routes
that other judges already cover. These three are the only judges of their
routes.

**Failure scenario.** The admin session expires between suites. DIAG-07
reports "GET /api/watchdog_cfg failed or missing 'panic_disabled'", which
points at the watchdog route instead of the session.

### L6. WEB-SEC-04 can PASS with an unverified pre-existing LCD PIN and pass it to LCD-19

**Fixed (55cfee20).** SEC-04 no longer hands a pre-existing, unverified PIN to LCD-19 (observed.pin_unverified). Residual fixed: `cases_web_rw.seed_lcd_pin` now raises `LcdPinSeedError("unverified")` when the board already had an admin PIN (no side-effect-free verify route exists), and LCD-19 returns INCONCLUSIVE with `observed.pin_unverified`, never driving the keypad.

`_write_lcd_pin_if_needed()` (`cases_web_rw.py:836-851`) skips the write
when `admin_pin_set` is already true. The firmware config GET
(`http/security_http.c:79-116`) reports only that a PIN is set, never which
one. SEC-04 then PASSes and stores the environment PIN in `ctx["_lcd_pin"]`
(`:938-942`), even though nothing has proved that this PIN is the one on
the board. The module's own comment says it "never acts on a PIN that
might not actually be live".

**Failure scenario.** The owner changed the LCD admin PIN on the panel.
SEC-04 PASSes, and LCD-19 then enters the stale PIN, gets locked out, and
reports a lockout or UI FAIL that has nothing to do with LCD-19's own
contract.

### L7. ZONE-05's whole-page compare includes live sensor fields

**Fixed (55cfee20).** ZONE-05 compare drops generation, safety_wiring, safety_ceiling, ct_warn_mask.

ZONE-05 (`cases_web_prof.py:759-805`) compares every field of two
`GET /api/zones` bodies, minus `generation`
(`_strip(after) != _strip(snap)`, `:791`). That body carries live
readings, not just config:

- `safety_wiring.tc_temp_c` (`%.1f`), `link_up` and `relay_energized`;
- `ct_warn_mask`;
- `safety_ceiling.pico_current_c`.

All of these are emitted at `http/zones_http_get.c:316-319`.

**Failure scenario.** The Pico thermocouple reading moves by 0.1 C between
the two GETs, which happens routinely on a warm bench. ZONE-05 FAILs with
"another config field changed across the identity PID write" even though
the write was a perfect identity.

**Suggested shape.** Drop `safety_wiring`, `safety_ceiling` and
`ct_warn_mask` before the compare.

### L8. ZONE-09 accepts a "save failed" enable as success

**Fixed (55cfee20).** ZONE-09 FAILs on an ok:true response carrying a warning, enable and restore.

`POST /api/adaptive_tune/enable` answers
`{"ok":true,"warning":"applied live, save failed"}` when the NVS save
fails (`http/adaptive_tune_http.c:197`). ZONE-09
(`cases_web_prof.py:907-953`) checks only `ok is True` and the live
read-back from `GET /api/adaptive_tune`, which reflects RAM. A persist
failure on the toggle and on its restore both go unnoticed. The restore
then leaves RAM correct and NVS holding whatever the failed write left.

**Failure scenario.** NVS writes for the zones namespace start failing.
ZONE-09 PASSes. After the next reboot, zone 0's adaptive tune enable bit
comes back from the stale NVS state.

### L9. ZONE-10's abort can kill a sweep that starts between the status read and the abort

**Fixed (55cfee20).** ZONE-10 re-reads sweep status immediately before the abort; running gives INCONCLUSIVE.

ZONE-10 (`cases_web_prof.py:956-976`) reads
`/api/zones/current_sweep/status`. It does the right thing when the state
is `running` (INCONCLUSIVE, no abort). Otherwise it posts the abort, which
is unconditional in firmware (`http/zones_http.c:470-475`,
`zones_current_sweep_abort()`, always `{"ok":true}`). Like the rw module,
it has no executor gate (`:957`).

**Failure scenario.** An operator starts a CT sweep from the settings page
in the window between the two requests. The bench run aborts it, and the
sweep's CT calibration is never derived. The window is small, so this is
LOW.

### L10. With no stored policy, the LOG-02 and SEC-03 restores create one

**Fixed (55cfee20).** policy_unstored: SEC-03, SEC-04 and the LOG-02/X-02 prelude refuse to start from the false/false/-1/-1 signature (no clear-policy route exists to restore none).

The security config GET (`http/security_http.c:79-116`) prints
`false`/`false`/`-1`/`-1` when no policy has ever been stored. The restore
POSTs `set_policy` with those values (`http/security_http.c:472-493`), so
the read-back matches. The board, however, now holds an explicit stored
policy where it had none. This is harmless today, because the defaults
are the same. It would silently pin the old defaults if a later firmware
changed them.

**Failure scenario.** A firmware update changes the default
`web_timeout_min`. The bench board keeps the old value, because a test
restore stored it explicitly.

## Notes (no finding)

- ZONE-09's `en()` selects the zone with `x.get("index", 0) == 0`. The
  route emits `"zone"`, not `"index"` (`http/adaptive_tune_http.c:46`),
  so every entry matches, and the first one, which is zone 0, is chosen.
  The result is correct, but only by accident of ordering.
- ZONE-05's gate and restore are otherwise sound. A 409 clears `posted`,
  so nothing is re-posted. The restore re-posts the same `%.9g` strings and
  compares the read-back exactly against the snapshot. A same-value write
  leaves `tuning_valid` alone (`zones_config_set_pid_no_save()`,
  `persist/zones_config_accessors.c:560-582`).

## Judges found clean

| Judge | Firmware traced |
|---|---|
| WEB-COMM-07 | `http/safety_cfg_http.c:1003-1050` (POST `type`), `:360` (GET `relay_type`); ADMIN `route_tier_table.h:301`; same-value write, restored in `finally` |
| WEB-DASH-05 | `http/zones_http_pid.c:105-170`; `ZONE_PID_GAIN_MAX` `persist/zones_config_accessors.h:199`; `%.9g` GET `http/zones_http_get.c:455`; ADMIN `route_tier_table.h:234` |
| WEB-DASH-07 | `http/dashboard_exec_http.c:61-81`, `:849` (ack); phase names `control/run_state.c:213-224` |
| WEB-DIAG-07 | `/api/watchdog_cfg` contract (`http/diagnostics_http.c:749`); see L1 and L5 for the gate and the first read |
| WEB-DIAG-09 | `http/diagnostics_http.c:1156` (GET active), `:1249-1253` (409 when inactive, before the body is parsed) |
| WEB-LOG-02 | `http/web_auth_login_http.c:16-24` backoff ladder, `:479` success reset; the 6 s wait is longer than the first 5 s step; logs in before writing (review 4 L3) |
| WEB-PROF-03..07 | `http/profiles_edit_http.c:616-632` (lowest free slot), `:711` (`{"ok":true,"id":N}`), `:721-800` (delete codes); `persist/profiles_builtin.h:38` (`PROFILE_BUILTIN_ID_BASE` 128); cleanup deletes only slots verified empty beforehand (see M1 for the cfg case) |
| WEB-PROF-08 | `http/profiles_edit_http.c:830-886`; `hidden` defaults to 1; `?all=1` lists hidden builtins (`http/profiles_catalog_http.c:247`); restore via `finally` plus a snapshot compare of the hidden map (see M1 for the cfg case) |
| WEB-WIZ-04, -05, -07, -08, -09, -11 | read-only setup progress/readiness contracts |
| WEB-WIZ-10 | `http/setup_progress_http.c:150`; 503 is already INCONCLUSIVE |
| WEB-SET-02, -03, -04 | settings routes; SET-02 already treats cfg unmounted as INCONCLUSIVE |
| WEB-DISP-03, -04 | display_power read contracts (`http/settings_http.c:181` handler family) |
| WEB-ZONE-05 | apart from L2 and L7 |
| WEB-ZONE-09 | `http/adaptive_tune_http.c:135-197`; ADMIN `route_tier_table.h:329`, `:334`; apart from L2 and L8 |
| WEB-ZONE-10 | `http/zones_http.c:470-487` (state enum idle/running/done/aborted/failed); ADMIN `route_tier_table.h:242-243`; never posts start; apart from L9 |
