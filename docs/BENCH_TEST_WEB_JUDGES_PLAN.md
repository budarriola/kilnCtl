# Bench test WEB judges: spec for the judge-less WEB-* cases

> **Status:** spec, 2026-10-09. Nothing here is built. Parent plan:
> [`BENCH_TEST_SYSTEM_PLAN.md`](BENCH_TEST_SYSTEM_PLAN.md) section 3.7, which gives each WEB id
> a one-line intent. This doc turns those lines into judges that an implementer can write
> without making anything up. Every route, field and line number below was read from source at
> `origin/dev` `676c503f`. Line numbers drift, so re-grep the quoted key before you trust a number.

## 1. Scope

`registry.py` registers 120 WEB ids (`_WEB_IDS`). These 27 already have a judge and are **out of
scope**:

- the 17 `-01` render cases, WEB-X-01, WEB-X-03, WEB-WIFI-06 and WEB-SEC-05 (`cases_web.py`
  `_CASE_FUNCS`);
- WEB-DASH-13, WEB-DIAG-07, WEB-DIAG-08, WEB-ZONE-14, WEB-SEC-03 and WEB-SEC-04
  (`cases_web_rw.py` `_CASE_FUNCS`).

This doc specifies the other **93**. Twenty-three of them are already listed in `SUITES["nightly"]`
(`registry.py` `_NIGHTLY_ORDER`). They have no judge yet, so the runner reports them as
`NOT_RUN: not_implemented` (`runner.py` ~line 540):
WEB-DASH-03/06/07/09, WEB-PROF-02..09, WEB-ZONE-02/03/05/09/12, WEB-BAK-02/03, WEB-KCFG-02/03,
WEB-OTA-02 and WEB-X-02.

## 2. Rules every judge in this doc follows

1. **No JavaScript runs in the harness.** `_web_client(ctx).goto(path)` (`cases_web.py`) returns
   the served HTML and does not execute it. A case whose plan line describes browser behavior,
   such as a popup, a banner shown by JS, or a button enabled by JS, judges two things instead:
   (a) the server-side **input** the page's JS reads, meaning the route and JSON field with the
   page line that consumes it, and (b) the **static presence** of the element id or JS function
   in the served HTML. WEB-ZONE-14 set this precedent: the graphic is drawn in the browser from
   `GET /api/zones`, so its inputs are what is judged. A case that cannot be reduced this way is
   marked **NEEDS OWNER**.
2. **Seams.** Board reads go through `cases_web_rw._get_json(ctx, path)` and writes through
   `_post_json(ctx, path, fields)` (authed, form-encoded). Page HTML goes through
   `_web_client(ctx).goto(path)`. Fake-board tests inject `ctx["http_get_json"]` and
   `ctx["http_post_json"]` (the `FakeHttp` pattern in
   `tools/PcTools/tests/test_bench_test_cases_web_rw.py`) and a fake `web_client` for HTML. Use
   an existing PcTools client or MCP tool where the block names one. Do not add a second HTTP
   path.
3. **Read-only is preferred.** A MUTATING case:
   - writes only after its gate passes;
   - restores in `finally`;
   - confirms the restore by read-back;
   - FAILs unconditionally if the restore does not round-trip, the same contract as
     `judge_web_rw_toggle`.

   **Standard mutating gate:** the case SKIPs, with a reason, unless all of these hold, checked
   immediately before the first write:
   - the running suite is a mutating suite (anything except `smoke`/`static`/`stack`);
   - `GET /api/profile_exec` reports the executor `idle` (`profiles_get_exec_status`);
   - autotune is not active (`autotune_get_status`).

   Note: `smoke`, `static` and `stack` are the read-only suites (plan section 6 rule 13).
4. **Never.** No case in this doc may:
   - write a relay during a firing;
   - call `factory_reset`;
   - call `backup_import` (`POST /api/backup/import`);
   - call `load_config_preset`;
   - call `estop_verify`;
   - call `update_stage_clear`;
   - push an OTA image (either processor);
   - do anything in plan section 6 rule 4: ack or clear a crash report, reset relay-life counts,
     format cfg, clear login credentials, press the bench preset, write CT calibration, write
     `abs_max_temp_c`, or change Wi-Fi provisioning.

   Where the plan line needs one of these, the case is marked **NEEDS OWNER** and a safe
   interpretation is recommended.
5. **Verdicts.**
   - FAIL is for a disagreement.
   - INCONCLUSIVE is for a board state that does not let the case judge, such as a feature that
     is not configured on this bench or a window that never opened.
   - NOT_RUN is for an observer whose host case did not run in this invocation. It is never a
     fabricated PASS.
   - SKIP is for a gate refusal or a missing env var.
6. **Observers.** These cases need a window held open by another case: "while HP-01 runs" or
   "during OT-B01's trip". By the time a later case runs, that window has closed. `_hp_run`
   (`cases_heat.py`) tears down the bench slot in `finally`, and OT-B01 (`cases_ota.py`
   `_case_otb01`) clears the S6a trip itself. Observers therefore need one small harness
   addition, **window probes** (NEW):
   - `CaseSpec` gets an optional `window_probe: Optional[Tuple[str, Callable[[dict], dict]]]`
     (window name, probe function).
   - Before the case loop, `BenchTestRunner` puts the probe of every requested case that has one
     into `ctx["_window_probes"][<window>]`, a list of `(case_id, fn)`.
   - Each `fn(ctx)` returns a JSON-able dict. The host case stores it under
     `ctx["_probe_results"][<window>][<case_id>]`. If `fn` raises, the host stores
     `{"error": "<repr>"}` instead and does not let the exception change its own verdict.
   - Window `hp01_running` fires once, on the first `_hp_run` poll with `state == "running"`
     when `zone_mask == 0b001` (`cases_heat.py` right after `state = st.state_name`, ~line 598).
   - Window `hp04_paused` fires once, on HP-04's confirmed pause (`cases_heat.py`, where
     `ctx["_hp04"]` is stashed, ~line 796).
   - Window `otb01_tripped` fires once, inside `if clear_allowed:` immediately **before**
     `clear_trip_fn()` (`cases_ota.py` ~line 597).

   An observer's `depends_on` is its host case. The runner already turns a non-PASS host into
   NOT_RUN (`runner.py` ~line 516). A host that is absent from the run does not block the
   observer, so its judge reads `ctx["_probe_results"]` and returns:
   - NOT_RUN if the host did not run, meaning the window key is absent;
   - INCONCLUSIVE if the host ran but the window never opened. For example, OT-B01 often sees
     no S6a on `sw_reset` (CLAUDE.md, 2026-10-01), so `otb01_tripped` never fires.

   Probes must be read-only. The only exception is a refusal probe that a block below explicitly
   allows.
7. **Aliases.** Some plan lines say "= <other case>". These are marked ALIAS. The judge copies
   the verdict of the case it aliases from `ctx["_results"]`. This is NEW: `BenchTestRunner` already keeps a
   `results` dict (`runner.py` ~line 559), and the change is to set `self.ctx["_results"] = results`,
   the same dict object, before the loop. The alias must be ordered after its target, either by
   `depends_on` or by `_NIGHTLY_ORDER` position. It is NOT_RUN when that case did not
   run, and it never repeats that case's writes.
8. **Fake-board tests.** Every judge gets at least one positive and one negative unit test in
   `tools/PcTools/tests/test_bench_test_cases_web_*.py`. The negative test feeds the specific
   bad response named in the block and asserts FAIL, not INCONCLUSIVE. Per the repo rule, a check
   that cannot fail is vacuous.

## 3. Harness changes these judges need

These are the only non-judge changes. Each is small. Build them before the judges that use them.

1. **Window probes and `ctx["_results"]`.** These are rules 6 and 7 in section 2.
2. **Host stashes named in the blocks.** Some blocks name a stash key on a host case instead of
   a probe:
   - `ctx["_hp01"]["web_exec_samples"]` (WEB-DASH-03)
   - `ctx["_hp04"]["web_states"]` (WEB-DASH-06)
   - `ctx["_otb01"]["web_status_tripped"]` and `["web_status_cleared"]` (WEB-DASH-08)
   - `ctx["_otb01"]["setup_progress_before"]` (WEB-WIZ-11)
   - `ctx["_otb01_window_observers"]` (WEB-SAF-03, WEB-RDY-04)

   Implement each of these as a rule-6 window probe. Some blocks need a window that rule 6
   does not define. Add these windows:
   - `hp01_tick`: every HP-01 poll, appended.
   - `hp04_states`: four samples: running, paused, after resume, and the last one before
     cleanup.
   - `otb01_before_reset`: just before `ota.sw_reset`.
   - `otb01_cleared`: after the clear is confirmed.

   Keep each block's PASS/FAIL logic exactly as written. Only the plumbing is unified.

   Probes stay read-only, with one exception: WEB-RDY-04's `POST /api/profile_exec/start` with
   body `probe=1`. That body has no `id`, so it can never start a firing:
   `dashboard_exec_http.c` answers 409 from the readiness gate before it reads the body, or 400
   "id missing".
3. **Smoke stashes.**
   - FL-01 (`cases_smoke.py` ~line 141) stores its parsed partition list in
     `ctx["_fl01_partitions"]`, for WEB-DIAG-03.
   - FL-07 (`cases_smoke.py` ~line 201) stores its `/api/cfgfs` dict in `ctx["_fl07_cfgfs"]`,
     for WEB-DIAG-04.
4. **New HTTP seams.** Add these in `cases_web_rw.py` next to `_get_json`/`_post_json`, with
   fakes `ctx["http_get_text"]` and `ctx["http_post_raw"]`:
   - An authed text GET for `/api/history.csv` (WEB-DASH-09). `_http_get_raw_authed` already
     exists.
   - An authed raw-text POST for plain `ok` replies (WEB-PROF-03..05). Use
     `_http_post_raw_authed`.
   - An authed JSON-body POST for `/api/profile/import` (WEB-PROF-07).
5. **Wi-Fi and credential deny-list** (WEB-WIFI-05, WEB-SEC-06).
   - `_post_json` and every raw POST helper raise on `/provision`, `/forget` and `/ip_config`,
     and on a `cmd=clear_credentials` field.
   - The deny-list is one constant, `_WIFI_WRITE_DENYLIST`.
6. **`_NIGHTLY_ORDER` changes** (`registry.py`). Today these observers run before their hosts.
   Move each one after its host and give it `depends_on`:
   - WEB-DASH-03 and WEB-DASH-09 go after HP-01. WEB-DASH-09 goes directly after HP-01, before
     HP-02, because a new run clears the history ring.
   - WEB-OTA-02 goes after HP-01.
   - WEB-DASH-06 goes after HP-04.
   - WEB-X-02 joins WEB-LOG-02 in the auth-on group next to WEB-SEC-03 (plan section 5.3,
     rule 4). Both stay ahead of `_ALWAYS_LAST` WEB-SEC-05.
   - WEB-LOG-03 becomes a post-run alias of WEB-SEC-05, because SEC-05 is pinned last.
7. **Finding (separate fix).** `judge_cfgfs_state` (`judgments.py` ~line 105) reads a
   `format_pending`/`pending` key that `GET /api/cfgfs` never emits.
   - `pending` exists only on `GET /api/cfgfs/format_pending` (`cfg_fs_format_http.c:32`).
   - As a result, FL-07 can never judge "pending". See WEB-DIAG-04 and WEB-SET-02.
8. **Finding.** The CLAUDE.md note that narrow zone writers re-post PID gains at GET's `%.4f`
   is stale.
   - `GET /api/zones` now prints gains at `%.9g` (`zones_http_get.c:455`), which is lossless.
     WEB-ZONE-02/05 and WEB-DASH-05 rely on this.
   - tau and deadtime still print at `%.1f`, so a whole-page re-POST must strip them
     (WEB-ZONE-02).

## 4. Per-case specs

Each block lists:
- Intent.
- Class: READ-ONLY, MUTATING, OBSERVER, ALIAS or NEEDS OWNER.
- Observable and route, with file:line and tier.
- PASS, FAIL and INCONCLUSIVE conditions.
- For a mutating case: the gate and the restore.
- The fake-board test: a positive test, plus a negative test that must FAIL.

### 4.1 Dashboard `/` (WEB-DASH)

#### WEB-DASH-02 — Profile picker lists favorites first
- **Intent:** The `#profileSelect` picker shows favorited profiles first, following `GET /api/profiles/favorites`.
- **Class:** READ-ONLY — the grouping happens in the browser, so the judge checks the two responses that drive it and re-runs the page's grouping in Python.
- **Observable / route:** `GET /api/profiles/favorites` → `{user_mask,builtin_mask,ids[]}` (emitter `profiles_catalog_http.c:594` header, `:614` user ids filtered by `profiles_slot_used` at `:610`, `:623` builtin ids = 128+i; consumer `main_page.html:1350-1354`); `GET /api/profiles` → `[{id,...,last_run_started_unix_s}]` (`profiles_catalog_http.c:335-336`; consumer `main_page.html:1402`); grouping `groupProfilesForPicker` `main_page.html:1327-1343` → `appendGroup('Favorites'…)` first `main_page.html:1445-1447`; both tier USER (`route_tier_table.h:215,220`).
- **PASS:** favorites body is an object with integer `user_mask`, `builtin_mask` and integer list `ids`. Every id below 128 is present in `/api/profiles`. Every id of 128 or more has bit (id-128) set in `builtin_mask`. The Python port of `groupProfilesForPicker(list, ids)` puts every favorite id in group 1 and none in "recent"/"all". The served `/` contains `id="profileSelect"`, `/api/profiles/favorites` and `appendGroup('Favorites'`.
- **FAIL:** non-200 or malformed body; a user id in `ids` that `/api/profiles` does not list (an orphaned favorite, which `:610` should have filtered); a builtin id whose mask bit is clear; any static token missing.
- **INCONCLUSIVE:** `ids` is empty (no favorites on the bench, so the ordering cannot be observed).
- **Fake-board test:** positive: favorites `{"user_mask":2,"builtin_mask":1,"ids":[1,128]}` with profiles `[{id:0},{id:1},{id:128}]` → PASS; negative: favorites `{"ids":[5],...}` with profiles `[{id:0},{id:1}]` → FAIL.

#### WEB-DASH-03 — Start via runBtn starts the hidden bench slot's profile
- **Intent:** Pressing Start on the dashboard starts the hidden bench slot's profile (uses HP-01).
- **Class:** OBSERVER (depends_on HP-01) — reads only during HP-01's run and never starts a firing itself.
- **Observable / route:** `GET /api/profile_exec` → `state`, `profile_id` (emitter `dashboard_exec_http.c:268`, state names `:41-45`; tier OPEN `route_tier_table.h:161`). Start path, checked statically only: `runBtn` handler `main_page.html:3051-3063` → `fetch('/api/profile_exec/start'` with `body: 'id=' + id` (`main_page.html:2998-3000`); route `dashboard_http.c:824`, tier USER (`route_tier_table.h:209`). Gap: HP-01 starts the slot over UART `srv._profiles.start` (`cases_heat.py` `_start_bench_profile`), so the web POST itself is never exercised.
- **Hook needed:** `_zone_diag_snapshot` (`cases_heat.py:432-452`) already GETs `/api/profile_exec` on every HP tick but throws away `state`/`profile_id`; it should keep them, e.g. as `ctx["_hp01"]["web_exec_samples"]`.
- **PASS:** at least one sample has `state=="running"` and `profile_id==7` (`BENCH_PROFILE_SLOT_ID`, `cases_heat.py:42`), and the last sample before cleanup is `done`. The served `/` contains `id="runBtn"`, `id="profileSelect"` and `/api/profile_exec/start`.
- **FAIL:** HP-01's MCP state reads running but no web sample reads `running`/`profile_id 7`; a `state` outside {idle,running,paused,done,faulted}; any static token missing.
- **INCONCLUSIVE:** never. NOT_RUN when HP-01 did not PASS or `web_exec_samples` is absent.
- **Registry:** `_NIGHTLY_ORDER` (`registry.py:488`) currently runs WEB-DASH-03 before HP-01. It must move after HP-01 and get `depends_on="HP-01"`.
- **Fake-board test:** positive: `_hp01={"ok":True,"web_exec_samples":[{"state":"running","profile_id":7},{"state":"done","profile_id":7}]}` plus a fake `/` with the tokens → PASS; negative: samples `[{"state":"idle","profile_id":0}]` with `_hp01.ok` true → FAIL.

#### WEB-DASH-04 — Feasibility popup "Start anyway"
- **Intent:** A profile whose ramp exceeds the zone's `max_ramp_c_per_hr` opens `feasPopupProceedBtn` ("Start anyway"), and proceeding starts the firing.
- **Class:** NEEDS OWNER — the plan's premise does not match the code, and "proceeding starts" would mean an extra firing.
- **Observable / route:** `GET /api/profile?id=N` → `feasibility` plus per-segment `feasibility` (emitter `profiles_catalog_http.c:484-487`, `:504-507`; values `profile_feasibility.c:117-121`: ok/too_fast/unreachable/unknown); tier USER (`route_tier_table.h:214`). Popup gate: `feasSummarize` (`main_page.html:2663-2676`) returns null only for `ok`, and `runBtn` opens `openFeasStartPopup` with `feasPopupProceedBtn` (`main_page.html:2945-2955`, `:3055-3058`). `max_ramp_c_per_hr` (`zones_http_get.c:455`, tier ADMIN `route_tier_table.h:233`) only drives `#profileLimitIcon`. A ramp above that limit is "refused at start" (`main_page.html:2824-2831`, `:2844-2845`) and never reaches "Start anyway".
- **PASS (recommended):** for each id in `GET /api/profiles` (builtins included), `feasibility` and every segment's `feasibility` are in the four-value set. Expected popup = `feasibility != "ok"`, recorded in observed. Static: `/` contains `id="feasPopupOverlay"`, `feasPopupProceedBtn` and `feasSummarize(`.
- **FAIL:** a missing `feasibility` field or a value outside the set; a static token missing.
- **INCONCLUSIVE:** every profile reads `ok` (the popup branch cannot be shown on this bench).
- **Fake-board test:** positive: `/api/profiles`=`[{id:128}]`, `/api/profile?id=128`=`{"feasibility":"too_fast","segments":[{"feasibility":"too_fast"}]}` → PASS; negative: `{"feasibility":"bogus","segments":[]}` → FAIL.
- **NEEDS OWNER:** Should this case judge the model-feasibility popup (what the code does) or the max-ramp limit (what the plan says, which the code refuses at start instead)? — recommended: read-only over the `feasibility` input as above. Drop "proceeding starts", which WEB-DASH-03/HP-01 already cover; never start an infeasible profile.

#### WEB-DASH-05 — Per-zone PID popup shows gains; Apply writes POST /api/zones/pid
- **Intent:** The PID popup shows the zone's current gains, and `pidPopupApplyBtn` writes them with `POST /api/zones/pid`; the test restores them afterwards.
- **Class:** MUTATING — writes one zone's Kp and restores it in a finally block.
- **Observable / route:** `GET /api/zones` → `zones[i].pid_kp/pid_ki/pid_kd` (emitter `zones_http_get.c:455,501`; consumer `main_page.html:983-985`, `:1023-1028`); tier ADMIN (`route_tier_table.h:233`). `POST /api/zones/pid` form `zone,kp,ki,kd` (`main_page.html:1071-1076`). Handler: refuses through `system_mode_gate_check` with 409 while a firing or autotune is active (`zones_http_pid.c:112-114`), parses at `:136-160` (0..1000), succeeds with `{"ok":true}` at `:172`. Route `zones_http.c:846`, tier ADMIN (`route_tier_table.h:234`). No PcTools helper wraps this route; use `_post_json`.
- **PASS:** the first read shows zone 0 with numeric gains. The POST returns 200 `{"ok":true}`. The read-back matches the test value within the page's tolerances (kp/kd 1e-6, ki 1e-9; `main_page.html:1093-1094`). The restore POST returns ok and the read-back equals the original gains. The served `/` contains `id="pidPopupOverlay"`, `pidPopupApplyBtn` and `/api/zones/pid`.
- **FAIL:** a write response that is not ok; a read-back mismatch; a restore read-back mismatch (always FAIL, same as `judge_web_rw_toggle`); a static token missing.
- **INCONCLUSIVE:** the gate does not hold (the case writes nothing); zone 0 is not configured.
- **Mutation / gate / restore:** run only in a mutating suite, only when `GET /api/profile_exec` `state=="idle"` and `GET /api/autotune` `state=="idle"` (checked right before the write), never during or alongside an HP/AT case. Write `zone=0, kp=kp0+0.125` (or `kp0-0.125` if that would exceed 1000), with `ki`/`kd` re-sent as the exact strings read back. In `finally`, POST `zone=0` with the original `kp/ki/kd` strings as received (`%.9g` round-trips a float), then GET to verify.
- **Fake-board test:** positive: a fake stores the POSTed gains and GET reflects them → PASS; negative: the restore POST returns ok but GET still shows `kp0+0.125` → FAIL.

#### WEB-DASH-06 — Sticky bar: Pause/Resume/Stop in RUNNING, Acknowledge in DONE/FAULTED
- **Intent:** The sticky bar shows Pause/Resume and Stop while a firing is running (HP-04), and Acknowledge once it is done or faulted (HP-07).
- **Class:** OBSERVER (depends_on HP-04) — the bar is built in app.js from `/api/profile_exec` `state`, so the judge checks that input across HP-04's window.
- **Observable / route:** `GET /api/profile_exec` → `state` (`dashboard_exec_http.c:268`, `:41-45`; OPEN `route_tier_table.h:161`; app.js tier OPEN `route_tier_table.h:156`). Consumer: heartbeat `app.js:2083`, `2093-2096`. Rules: `STOPPABLE_STATES={running,paused}` / `ACK_STATES={faulted,done}` (`app.js:1836-1837`); running→"Pause", paused→"Resume" (`app.js:1962-1967`); Stop shown only when stoppable (`app.js:1981`); ACKNOWLEDGE for done/faulted (`app.js:1985-1994`).
- **Hook needed:** HP-04 records the web `state` while running before the pause, while paused, after resume, and the final reading before `_cleanup_bench_profile`, as `ctx["_hp04"]["web_states"]` (today `cases_heat.py:796-800` stores only the MCP `paused_state`). The FAULTED branch is judged only if `ctx["_hp07"]["web_states"]` exists; HP-07 is not in nightly.
- **PASS:** `web_states` contains `running`, `paused` and `done` (or `faulted`), in that order. The paused reading agrees with HP-04's MCP `paused_state`. `/app.js` contains `kc-stop-bar`, `kc-pause-btn`, `STOP FIRING`, `ACKNOWLEDGE (firing complete)` and `/api/profile_exec/pause`.
- **FAIL:** the web reading at the pause point is not `paused` while MCP says paused; a state outside the vocabulary; a static token missing.
- **INCONCLUSIVE:** HP-04 passed but no `done`/`faulted` reading was captured, so the ACK branch is unobservable. NOT_RUN when HP-04 did not run or `web_states` is absent.
- **Registry:** `registry.py:488` places this before HP-04. Move it after HP-04 and set `depends_on="HP-04"`.
- **Fake-board test:** positive: `_hp04={"paused_state":"paused","web_states":["running","paused","running","done"]}` plus a fake `/app.js` with the tokens → PASS; negative: `web_states:["running","running","done"]` with `paused_state:"paused"` → FAIL.

#### WEB-DASH-07 — Last-run card + ackLastRunBtn
- **Intent:** The last-run card appears and `ackLastRunBtn` dismisses it with `POST /api/profile_exec/ack_last_run`.
- **Class:** NEEDS OWNER — `last_run` is the previous boot's record, not HP-01's, and acking it cannot be undone.
- **Observable / route:** `GET /api/profile_exec` → `last_run{present,interrupted,phase,profile_id,…}` (emitter `dashboard_exec_http.c:66` and `:76-83`; the source is the boot record, `run_state.c:445-460`; phases none/running/paused/done/halted/faulted at `run_state.c:180-185`). Card visible only when `present && (interrupted || phase=="faulted")` (`main_page.html:2274`); button `main_page.html:2301`, POST `main_page.html:2305`. Handler returns plain `"ok"`, or 400 when there is nothing to ack (`dashboard_exec_http.c:849-856`). Tier USER (`route_tier_table.h:213`).
- **PASS (recommended read-only):** `last_run` is either `{"present":false}` or a full record with boolean `interrupted` and `phase` in the set above. The expected card state follows the `:2274` rule and is recorded in observed. `/` contains `id="lastRunBanner"`, `ackLastRunBtn` and `/api/profile_exec/ack_last_run`.
- **FAIL:** `last_run` missing; `present:true` with missing or invalid fields; `interrupted:true` while `phase` is not running/paused (contradicts `run_state.c:195-198`); a static token missing.
- **INCONCLUSIVE:** `present:false` (no record on this boot, so the card cannot be observed).
- **Fake-board test:** positive: `{"state":"idle","last_run":{"present":true,"interrupted":false,"phase":"halted","profile_id":7,...}}` → PASS (card expected hidden); negative: `{"state":"idle"}` with no `last_run` key → FAIL.
- **NEEDS OWNER:** May the harness POST `ack_last_run`? It permanently dismisses whatever record the previous boot left, which may be an owner's interrupted or faulted firing. HP-05 already acks over MCP (`cases_heat.py:836`). Also, HP-01 cannot supply this record because it is boot-crossing. — recommended: read-only as above; press the ack only when `last_run.profile_id == 7` (the harness's own bench slot, e.g. after an OT-E reboot), then expect 200 and `present:false`; otherwise never press.

#### WEB-DASH-08 — Trip banner + clearTripBtn only while a trip is latched
- **Intent:** The trip banner and `clearTripBtn` are visible only while `safety_get_status` shows a trip (during OT-B01's S6a window).
- **Class:** OBSERVER (depends_on OT-B01) — reads `/api/status` during OT-B01's window and never POSTs `clear_trip` itself (plan §6 rule 5; OT-B01 owns the only clear).
- **Observable / route:** `GET /api/status` → `diag_ever_received`, `diag_trip_reason`, `diag_state`, `diag_age_ms` (emitter `dashboard_status_http.c:736,738,744,758`); tier OPEN (`route_tier_table.h:160`). The banner shows when `diag_ever_received && diag_state==4 && diag_age_ms<1500` (`main_page.html:1966-1967,1979-1981`). `clearTripBtn` is rendered when `reason != 10` (`main_page.html:2033-2034`) and POSTs `/api/safety/clear_trip` (`main_page.html:2041`, tier ADMIN `route_tier_table.h:294`).
- **Hook needed:** OT-B01 stashes up to 3 `GET /api/status` bodies, about 1 s apart, after the trip is confirmed and before it clears it (`ctx["_otb01"]["web_status_tripped"]`), plus one after the clear is confirmed (`["web_status_cleared"]`). The `_stash` at `cases_ota.py:418-428` holds only MCP data today.
- **PASS:** `outcome=="s6a_latched"`; at least one tripped body meets the banner predicate with `diag_trip_reason==6`; the cleared body fails the predicate; `/` contains `id="safetyTripBanner"`, `clearTripBtn` and `/api/safety/clear_trip`.
- **FAIL:** MCP saw reason 6 latched but no tripped web body meets the predicate; the cleared body still meets it; a static token missing.
- **INCONCLUSIVE:** `outcome` is `no_trip` or any `inconclusive_*` (no trip window this run). NOT_RUN when OT-B01 did not run or the hook keys are absent.
- **Fake-board test:** positive: `_otb01={"outcome":"s6a_latched","web_status_tripped":[{"diag_ever_received":True,"diag_state":4,"diag_age_ms":200,"diag_trip_reason":6}],"web_status_cleared":{"diag_ever_received":True,"diag_state":1,"diag_age_ms":200,"diag_trip_reason":0}}` → PASS; negative: same but `web_status_tripped[0].diag_state=1` → FAIL.

#### WEB-DASH-09 — History chart source non-empty after HP-01
- **Intent:** `GET /api/history.csv`, the history chart's data source, is non-empty after HP-01.
- **Class:** OBSERVER (depends_on HP-01) — one read-only GET, run immediately after HP-01 and before HP-02, because the ring is cleared only when a new run starts (`profile_executor_run.c:1266-1267`).
- **Observable / route:** `GET /api/history.csv` (text/csv). Emitter: header `elapsed_s,desired_c` + `,z{i}_actual_c,z{i}_duty,z{i}_guard` × channels + `,zone_mask` (`dashboard_exec_http.c:638-648`); rows `:656-663`; 30 s sampling while running (`profile_executor.h:406`). Tier OPEN (`route_tier_table.h:163`). Consumer: `pollHistory` `main_page.html:3597`; `parseHistoryCsv` needs at least 2+3·3 fields (`main_page.html:3089-3097`); canvas `main_page.html:414`. `_get_json` cannot carry CSV, so use `_http_get_raw_authed` (`cases_web_rw.py:112`) or a new `ctx["http_get_text"]` seam.
- **PASS:** status 200; the header matches the emitter for 3 zones; at least 2 data rows, each with ≥11 fields; `elapsed_s` is non-decreasing; at least one row's `zone_mask` has bit 0 set (HP-01 uses `zone_mask=0b001`).
- **FAIL:** header only or no rows; a wrong header; a short or non-numeric row; no row with zone 0 in its mask.
- **INCONCLUSIVE:** never. NOT_RUN when HP-01 did not PASS, or when another heat case started between HP-01 and this read.
- **Registry:** `registry.py:488` runs this before HP-01. Move it directly after HP-01 and set `depends_on="HP-01"`.
- **Fake-board test:** positive: the 3-zone header plus rows `0,30.00,25.00,0.500,0,…,1` and `30,35.00,…,1` → PASS; negative: the header line only → FAIL.

#### WEB-DASH-10 — Recovery banner absent when recovery_mode false
- **Intent:** When `GET /api/ota/esp/status` reports `recovery_mode` false, the recovery banner is absent (the true branch belongs to OT-E11).
- **Class:** READ-ONLY.
- **Observable / route:** `GET /api/ota/esp/status` → `recovery_mode`, which is never redacted (emitter `ota_http_esp.c:657,665` and `:676,681`; route `ota_http.c:759`); tier OPEN (`route_tier_table.h:421`); helper `ota_http_client.get_esp_status` (`ota_http_client.py:423`). Consumer: `pollRecoveryMode` `app.js:1664-1670` → `setRecoveryBanner`, which also disables `#runBtn` (`app.js:1655-1658`); the banner is built hidden (`app.js:1605-1606`).
- **PASS:** 200; `recovery_mode` is JSON `false` (a real boolean); `/app.js` contains `kc-recovery-banner`, `/api/ota/esp/status` and `st.recovery_mode`.
- **FAIL:** the field is missing or not a boolean; non-200; a static token missing.
- **INCONCLUSIVE:** `recovery_mode` is `true` (the board is in recovery; that branch belongs to OT-E11).
- **Fake-board test:** positive: `{"phase":"idle","recovery_mode":false,"last_update":null}` → PASS; negative: `{"phase":"idle","last_update":null}` (no field) → FAIL.

#### WEB-DASH-11 — Setup-offer banner iff readiness has not_done items
- **Intent:** The setup-offer banner appears exactly when `GET /api/readiness` has `not_done` items.
- **Class:** READ-ONLY — the banner is drawn in app.js, so the judge checks its input and the expected visibility.
- **Observable / route:** `GET /api/readiness` → `items[].status` (emitter `readiness_http.c:137`, `:186`; values ok/not_done/cannot_yet/deliberately_off `readiness_http.c:57-60`; route `:1204`); tier OPEN (`route_tier_table.h:162`); helper `readiness_http_client.get_readiness`. Consumer: `pollSetupOffer` `app.js:1761-1769` (banner iff any `status==='not_done'`; never on `/setup`, `app.js:1760`); banner `kc-setup-banner` `app.js:1710`.
- **PASS:** 200; `items` is a non-empty list; every item has `key` and a `status` in the set; expected banner = any `not_done`, recorded in observed; `/app.js` contains `kc-setup-banner`, `/api/readiness` and `'not_done'`.
- **FAIL:** a missing `items` list or a `status` outside the set (including `unknown`); a static token missing.
- **INCONCLUSIVE:** never; both banner states are valid outcomes.
- **Fake-board test:** positive: `{"items":[{"key":"a","status":"ok"},{"key":"b","status":"not_done"}]}` → PASS (banner expected shown); negative: `{"items":[{"key":"a","status":"pending"}]}` → FAIL.

#### WEB-DASH-12 — Disconnected banner after 2 failed polls
- **Intent:** The disconnected banner appears after 2 failed `/api/profile_exec` polls (plan: simulate by pointing the client at a closed port; client-side only).
- **Class:** NEEDS OWNER — the trigger is a failed `fetch()` in the browser, not a server field, and the harness cannot run JavaScript.
- **Observable / route:** heartbeat `fetch('/api/profile_exec')` (`app.js:2083`); `FAILURES_BEFORE_BANNER = 2` (`app.js:1497`); the catch counts failures and calls `showBanner` once the count reaches 2 (`app.js:2099-2103`); the success path hides it (`app.js:2088-2091`); banner `kc-conn-banner` (`app.js:1511`). Server side: `/api/profile_exec` tier OPEN (`route_tier_table.h:161`).
- **PASS (recommended):** `GET /api/profile_exec` returns 200 JSON with `state` (the success path), and `/app.js` contains `FAILURES_BEFORE_BANNER = 2`, `kc-conn-banner`, `fetch('/api/profile_exec')` and `consecutiveFailures >= FAILURES_BEFORE_BANNER`.
- **FAIL:** a constant other than 2; a static token missing; a non-200 heartbeat route.
- **INCONCLUSIVE:** never.
- **Fake-board test:** positive: a fake `/app.js` with the tokens plus `/api/profile_exec`=`{"state":"idle"}` → PASS; negative: a fake `/app.js` with `FAILURES_BEFORE_BANNER = 3` → FAIL.
- **NEEDS OWNER:** The closed-port simulation proves nothing without a browser. — recommended: the static-constant check above, with the behavioural coverage living in a node unit test next to `firmware/KilnFW/App/test/test_recovery_banner.js` (none exists today for the connection banner).

NEEDS OWNER ids: WEB-DASH-04, WEB-DASH-07, WEB-DASH-12 (resolved 2026-10-09: accepted recommendation)

### 4.2 Profiles `/profiles` (WEB-PROF, WEB-STIM)

#### WEB-PROF-02 — Favorite star toggles and reorders the dashboard picker
- **Intent:** The favorite star calls `POST /api/profile/favorite`, and the dashboard picker then lists that profile in its Favorites group.
- **Class:** MUTATING. It flips one favorite bit and restores it in `finally`.
- **Observable / route:**
  - Write: `POST /api/profile/favorite` with `id=N&favorite=0|1`. Response `{"ok","id","favorite","persisted"}` (emitter `profiles_edit_http.c:899`; handler `:860`; consumer `profiles_page.html:1720-1722`). Tier ADMIN (`route_tier_table.h:273`).
  - Read-back: `GET /api/profiles/favorites` → `ids` (emitter `profiles_catalog_http.c:594`; user ids are filtered by the used bitmap at `:610`, builtin ids are 128+i at `:618-626`). Consumers are `main_page.html:1349-1356` (`loadFavoriteIds`), `groupProfilesForPicker` at `:1327` (Favorites group appended at `:1445`) and `profiles_page.html:1413-1422`. Tier USER (`route_tier_table.h:220`).
- **Target:** The first non-hidden builtin from `GET /api/profiles` with `builtin:true` (`profiles_catalog_http.c:349-356`). No user slot is touched.
- **PASS:**
  - The POST returns 200 with `ok:true`, `id` equal to the target, and `favorite` equal to the requested value.
  - Favorites `ids` contains the target exactly when `favorite=1`.
  - The target is still listed in `/api/profiles`, which is the picker's input (`main_page.html:1400-1406`).
  - Static check: the served `/profiles` HTML contains `favToggleBtn` and `/api/profile/favorite`.
- **FAIL:**
  - The POST is not 200 or `ok` is not true.
  - The `ids` membership does not follow the write.
  - The restore read-back does not match the original membership. This is an unconditional FAIL.
- **INCONCLUSIVE:** `/api/profiles` lists no visible builtin, so there is no safe target.
- **Mutation / gate / restore:**
  - Gate: the suite is mutating (web/nightly/full) and holds the board lock, and `GET /api/profile_exec` `state` is idle (emitter `dashboard_json.c:374`; reuse `_is_idle` from `cases_ota.py:106`).
  - Write: snapshot the original membership `orig`, then POST `favorite=(not orig)`.
  - Restore in `finally`: POST `favorite=orig`, re-read favorites `ids`, and require membership to equal `orig`.
  - Model this on `_bool_toggle_case` (`cases_web_rw.py:192`) and `judge_web_rw_toggle`.
- **Fake-board test:**
  - Positive: favorites `ids` is `[]`, the POST echoes `favorite:true`, the next GET returns `ids:[128]`, and the restore GET returns `[]`.
  - Negative: the restore GET still returns `ids:[128]`. This must be a FAIL (restore mismatch).

#### WEB-PROF-03 — New profile lands in a free slot
- **Intent:** `addSegBtn`, then fill the form, then `saveBtn` sends `POST /api/profile`, which stores the profile in a free slot.
- **Class:** MUTATING. It creates one scratch profile and deletes it in `finally`.
- **Observable / route:**
  - Save: `POST /api/profile` with `id=-1`, `name`, `zone_mask`, `seg_count`, `seg0_kind`, `seg0_target`, `seg0_ramp`, `seg0_dwell` (parser `profiles_edit_http.c:43/56/73/105/165/177/149`). An `id` of -1 means first free slot (`:520-545`). Response `{"ok":true,"id":N,"warnings":...}` (`:681`). Page consumer `profiles_page.html:2210-2258`, which reads `result.data.id` at `:2270`. Tier ADMIN (`route_tier_table.h:265`).
  - Read-back: `GET /api/profile?id=N` (`profiles_catalog_http.c:483-530`; USER tier, `route_tier_table.h:214`).
- **PASS:**
  - The POST returns 200 with `ok:true`.
  - The returned `id` was absent from the pre-snapshot of `/api/profiles` user ids and equals the lowest free id below 100.
  - GET detail has `name`, `zone_mask`, `segment_count`=1 and `segments[0]` with `seg_kind`/`target_c`/`ramp_c_per_hr`/`dwell_min` equal to what was sent.
  - Static check: the served `/profiles` HTML contains `addSegBtn` (`:385`) and `saveBtn` (`:395`).
- **FAIL:**
  - The POST is rejected.
  - The returned id was already in use (an overwrite).
  - The detail does not match what was sent.
  - The post-cleanup `/api/profiles` user list differs from the pre-snapshot. This is an unconditional FAIL.
- **INCONCLUSIVE:**
  - All 100 user slots are in use.
  - The scratch name `BT_PROF03` already exists. The name check refuses duplicates (`profiles_edit_http.c:621-654`).
- **Mutation / gate / restore:**
  - Gate: mutating suite with the board lock, executor idle (`dashboard_json.c:374`), and the scratch name absent.
  - Write: one profile `BT_PROF03` with `zone_mask=1`, `seg_count=1`, `seg0_kind=0`, `seg0_target=30`, `seg0_ramp=0`, `seg0_dwell=1`.
  - Restore in `finally`: `POST /api/profile/delete id=N` must return plain `ok` (`profiles_edit_http.c:761`). Then `GET /api/profile?id=N` must return 404 (`profiles_catalog_http.c:397`), and the user list must equal the snapshot.
  - Needs a raw-text authed POST seam such as `_http_post_raw_authed` (`cases_web_rw.py:136`).
  - Plan §6 rule 12 conflict: rule 12 limits writes to the hidden bench slot 101, which HTTP cannot reach (POST only accepts ids below 100). This case therefore uses a transient free user slot (owner ruling 2026-10-09, now recorded as the rule 12 exception in BENCH_TEST_SYSTEM_PLAN.md): the slot is read from `/api/profiles` and verified empty (`GET /api/profile?id=N` 404) before any write, the profile is named `BT_PROF*`, INCONCLUSIVE if no slot is free, and `finally` deletes only what the case created and reads back that the slot is empty (a leftover is FAIL with an "ERROR:" reason). Implemented in `cases_web_prof.py`.
- **Fake-board test:**
  - Positive: the snapshot has ids `[0,1]`, the POST returns `id:2`, the detail matches, the delete returns `ok`, and the GET returns 404.
  - Negative: the POST returns `id:0` (an occupied slot). This must be a FAIL.

#### WEB-PROF-04 — Edit and save an existing profile
- **Intent:** Edit an existing profile and save it with `POST /api/profile` using the same id.
- **Class:** MUTATING. It only edits a scratch profile that this case creates. It never edits a pre-existing user profile.
- **Observable / route:**
  - Write: `POST /api/profile` with `id=N`, where N is in 0..99 (overwrite path, `profiles_edit_http.c:476`, fields as in PROF-03). Page consumer `editProfile` `profiles_page.html:2038`, then save `:2202-2276`. Tier ADMIN (`route_tier_table.h:265`).
  - Read-back: `GET /api/profile?id=N` (`profiles_catalog_http.c:483-530`).
- **PASS:**
  - The create returns id N.
  - The edit POST with the same `id=N` (`seg_count=2`, `seg0_target=40`, `seg1_kind=0`, `seg1_target=35`, ramp and dwell set) returns `ok:true` and `id` equal to N.
  - The detail shows `segment_count`=2 and the new values.
  - The user-id set is unchanged apart from N.
- **FAIL:**
  - The edit returns a new id or is rejected.
  - The detail still holds the old segments.
  - Cleanup fails or the post-cleanup list differs from the snapshot. This is an unconditional FAIL.
- **INCONCLUSIVE:** No free slot is available, or the scratch name `BT_PROF04` exists.
- **Mutation / gate / restore:**
  - Gate: same as PROF-03.
  - Write: create `BT_PROF04`, then overwrite it once.
  - Restore in `finally`: delete N, confirm the GET returns 404, and confirm the list equals the snapshot.
  - Plan §6 rule 12 conflict: same note as PROF-03.
- **Fake-board test:**
  - Positive: the create returns id 3, the edit returns id 3, and the detail shows `segment_count:2`.
  - Negative: the edit response returns `id:4`. This must be a FAIL.

#### WEB-PROF-05 — Delete with confirm
- **Intent:** Delete a profile after the confirm dialog, using `POST /api/profile/delete`.
- **Class:** MUTATING. It creates a scratch profile and deletes it. That delete is the case under test.
- **Observable / route:**
  - `POST /api/profile/delete` with `id=N` returns 200 with the plain-text body `ok` (`profiles_edit_http.c:761`; handler `:689`). Page consumer `deleteProfile` `profiles_page.html:2065-2080`, confirmed by `kcConfirm` at `:1775`. Tier ADMIN (`route_tier_table.h:266`).
  - Builtin refusal returns 400 (`profiles_edit_http.c:715-719`).
- **PASS:**
  - The scratch create succeeds.
  - The delete returns 200 with body `ok`.
  - `GET /api/profile?id=N` returns 404 (`profiles_catalog_http.c:397`), and N is absent from `/api/profiles`.
  - Negative probe: `POST /api/profile/delete id=128` returns 400, and builtin 128 is still in `/api/profiles/builtin?all=1`.
  - Static check: the served HTML contains `kcConfirm` and `/api/profile/delete`.
- **FAIL:**
  - The delete returns non-200 or a body other than `ok`.
  - The profile is still readable after the delete.
  - The builtin delete returns 200.
  - The list after the case differs from the snapshot.
- **INCONCLUSIVE:** No free slot is available, or the scratch name `BT_PROF05` exists.
- **Mutation / gate / restore:**
  - Gate: same as PROF-03. The executor must be idle because deleting the running id returns 409 (`:735-741`).
  - Write: create `BT_PROF05`, then delete it.
  - Restore in `finally`: if the delete under test failed, retry the delete and verify a 404. A remaining profile is an unconditional FAIL.
  - Plan §6 rule 12 conflict: same note as PROF-03.
- **Fake-board test:**
  - Positive: the delete returns `ok`, the GET returns 404, and the builtin delete returns 400.
  - Negative: after the delete returns `ok`, the GET still returns 200 with the profile. This must be a FAIL.

#### WEB-PROF-06 — Export a profile
- **Intent:** Export the selected profile with `GET /api/profile/export`. The output must round-trip through PROF-07.
- **Class:** MUTATING. It needs a scratch profile to export. The export itself is read-only.
- **Observable / route:**
  - `GET /api/profile/export?id=N` returns `{"kind":"kilnctl_profile","version":2,"name","zone_mask","segments":[{seg_kind,target_c,ramp_c_per_hr,dwell_min,io_target,io_state,io_blocking,io_leave_on_at_end}],"on_off_rules"}` (emitter `profiles_export_http.c:104-134`). It sets a Content-Disposition attachment header (`:153-156`). An id of 100 or more returns 404 (`:46`).
  - Page consumer `profileExportUrl` `profiles_page.html:2086`; the bulk-export path is at `:1628-1642`. Tier USER (`route_tier_table.h:221`).
- **PASS:**
  - The response is 200 with `kind=="kilnctl_profile"`, `version==2`, and the `name` and `zone_mask` that were created.
  - The `segments` list equals the GET-detail segments field by field.
  - `GET /api/profile/export?id=128` returns 404.
  - Static check: `modeExportBtn` (`:358`) is present.
- **FAIL:**
  - Wrong `kind` or `version`.
  - A segment does not match the detail.
  - A builtin export returns 200.
  - Cleanup does not match. This is an unconditional FAIL.
- **INCONCLUSIVE:** No free slot is available, or the scratch name exists.
- **Mutation / gate / restore:**
  - Gate: same as PROF-03.
  - Write: create `BT_PROF06` with 2 segments.
  - Restore in `finally`: delete it, verify a 404, and verify the list equals the snapshot.
  - Plan §6 rule 12 conflict: same note as PROF-03.
- **Fake-board test:**
  - Positive: the export JSON segments equal the detail segments.
  - Negative: the export returns `"version":1`, or a segment has a different `target_c`. Either must be a FAIL.

#### WEB-PROF-07 — Import round-trips with identical segments
- **Intent:** `POST /api/profile/import` (used by both page call sites) recreates the exported profile with identical segments.
- **Class:** MUTATING. It imports into a scratch slot and deletes both the source and the import in `finally`.
- **Observable / route:**
  - `POST /api/profile/import`, optionally with `?id=N`. The body is raw JSON, not form-encoded. With no id it uses the first free slot (`profiles_export_http.c:423-436`). It saves through `profiles_http_save` (`:441`).
  - Success is `{"ok":true,"id":N,"warning_count":N}` (`:458`). Errors are 400 `{"ok":false,"error"}`.
  - Page call sites: bulk at `profiles_page.html:2147-2148` and single at `:2179-2180`. Tier ADMIN (`route_tier_table.h:267`).
- **PASS:**
  - Take the PROF-06-style export of scratch S and change `name` to `BT_PROF07I`, because duplicate names are refused (`profiles_http.c:1665-1685`).
  - The POST returns `ok:true` with a new id M not equal to S, and M was previously free.
  - `GET /api/profile?id=M` has `segments` equal to the source detail for every field except `feasibility`.
  - Static check: the served HTML contains `/api/profile/import` at least twice, plus `validateImportJson` (`:2103`).
- **FAIL:**
  - The import is rejected.
  - Any segment field differs.
  - M overwrote an occupied slot.
  - Cleanup does not match. This is an unconditional FAIL.
- **INCONCLUSIVE:** Fewer than 2 free slots are available, or either scratch name exists.
- **Mutation / gate / restore:**
  - Gate: same as PROF-03.
  - Writes: create the source `BT_PROF07`, then import a copy as `BT_PROF07I`.
  - Restore in `finally`: delete M and S, require 404 on both, and require the list to equal the snapshot.
  - Needs a new authed JSON-body POST seam. `_post_json` is form-encoded only.
  - Plan §6 rule 12 conflict: same note as PROF-03.
- **Fake-board test:**
  - Positive: the import returns `id:5`, and the detail for 5 equals the source segments.
  - Negative: the detail for the imported profile has `dwell_min` differing by 1. This must be a FAIL.

#### WEB-PROF-08 — Hide builtin and restore
- **Intent:** Hide a builtin with `POST /api/profile/builtin/hide` and bring hidden builtins back with `restoreBtn` (`/restore`).
- **Class:** MUTATING. It hides one builtin and restores that id in `finally`.
- **Observable / route:**
  - Hide: `POST /api/profile/builtin/hide` with `id=128..&hidden=0|1` returns `{"ok","id","hidden","persisted"}` (`profiles_edit_http.c:811-823`). Page consumer `hideBuiltin` `profiles_page.html:2032`. Tier ADMIN (`route_tier_table.h:268`).
  - Restore all: `POST /api/profile/builtin/restore` (`profiles_edit_http.c:829-844`; `restoreBtn` `profiles_page.html:346/2034-2036`). Tier ADMIN (`route_tier_table.h:269`).
  - Read-back: `GET /api/profiles/builtin?all=1` → `hidden` (`profiles_catalog_http.c:199-206/247`). `/api/profiles` skips hidden builtins (`:351`). The page shows `restoreBtn` only when the hidden count is above 0 (`profiles_page.html:1442-1444`).
- **PASS:**
  - Target: the first builtin with `hidden:false`.
  - The hide returns `ok` and `hidden:true`.
  - `?all=1` shows the target as `hidden:true`, and the target is absent from `/api/profiles`.
  - When the pre-snapshot had zero hidden builtins, also exercise `/restore`: it returns `ok`, and every builtin is then `hidden:false`.
- **FAIL:**
  - The hide is ignored.
  - The target still appears in `/api/profiles`.
  - After restore, the per-id `hidden` map differs from the pre-snapshot. This is an unconditional FAIL.
- **INCONCLUSIVE:** Every builtin is already hidden.
- **Mutation / gate / restore:**
  - Gate: mutating suite with the board lock and executor idle.
  - Restore in `finally`: POST hide with the target and `hidden=0`. Use the per-id restore, not the global `/restore`, because the global one would also unhide builtins the user had hidden. Then re-read `?all=1` and compare the full `hidden` map with the snapshot.
- **Fake-board test:**
  - Positive: snapshot all `false`, after the hide the target is `true` and absent from the list, after the restore all are `false`.
  - Negative: after the restore the target is still `hidden:true`. This must be a FAIL.

#### WEB-PROF-09 — Bulk select/delete mode
- **Intent:** Bulk select/delete mode using `modeDeleteBtn`, `bulkActionBtn` and `bulkCancelBtn`.
- **Class:** READ-ONLY. Selection mode is client-side JS. The server effect is sequential `POST /api/profile/delete` calls, which PROF-05 already covers.
- **Observable / route:**
  - Static content of `GET /profiles`: element ids `modeDeleteBtn` (`profiles_page.html:359`), `bulkActionBtn` (`:363`) and `bulkCancelBtn` (`:364`).
  - Functions: `toggleMode`/`exitSelectMode` (`:1525-1535`), `selectionCheckbox` (`:1570-1586`; builtins get no delete checkbox, `:1572`) and `bulkDelete` (`:1644-1680`, using `kcConfirm` and then `/api/profile/delete` per id).
  - Server input: `GET /api/profiles` → `builtin` flag (`profiles_catalog_http.c:335-338`).
  - Tier ADMIN for the page (`route_tier_table.h:462`).
- **PASS:**
  - The served HTML is 200 and contains all three ids plus `bulkDelete` and `selectionCheckbox`.
  - Every `/api/profiles` entry has a boolean `builtin` field.
- **FAIL:** Any id or function is missing, or an entry lacks `builtin`.
- **INCONCLUSIVE:** none.
- **Fake-board test:**
  - Positive: HTML containing all tokens plus a valid list.
  - Negative: HTML without `bulkCancelBtn`. This must be a FAIL.

#### WEB-PROF-10 — Relay segment target list comes from /api/zones
- **Intent:** I/O (relay) segment rows build their target list from `GET /api/zones`.
- **Class:** READ-ONLY.
- **Observable / route:**
  - `GET /api/zones` returns `relay_count` (`zones_http_get.c:285`), `relay_zone_owned_mask` (`:315`, which is `zone_owned_relay_mask()`, `zones_config_store.c:1592`), `relay_names` (`zones_http_get.c:320/334-338`) and `zones[i].relay_mask`.
  - Page consumers `loadZones` (`profiles_page.html:503-505`) and `ioTargetOptionsHtml` (`:653-671`).
  - The aux exclusion comes from `GET /api/aux_outputs` → `enabled_mask` (`profiles_page.html:480`; tier `route_tier_table.h:247`).
  - `/api/zones` is ADMIN tier (`route_tier_table.h:233`).
- **PASS:**
  - `relay_names` is a list with length at least `relay_count`.
  - `relay_zone_owned_mask` equals the OR of `zones[i].relay_mask` for i below `thermo_count`.
  - The expected offered set is relays 1..`relay_count` minus (owned OR aux `enabled_mask`). Record it as evidence.
  - Static check: the served `/profiles` HTML contains `ioTargetOptionsHtml` and `relay_zone_owned_mask`.
- **FAIL:** A field is missing or has the wrong type, the mask does not equal the OR of the zone masks, or a static token is absent.
- **INCONCLUSIVE:** `/api/aux_outputs` is unavailable. In that case judge the owned mask only and note it.
- **Fake-board test:**
  - Positive: `relay_count:4`, zones masks 1 and 2, owned 3.
  - Negative: `relay_zone_owned_mask:1` while the zone masks are 1 and 2. This must be a FAIL.

#### WEB-PROF-11 — User tier can list but cannot save
- **Intent:** A `user`-tier session can list profiles, but `POST /api/profile` is refused (ADMIN tier).
- **Class:** READ-ONLY. The only POST it sends is designed to be refused at the gate, and its body is invalid in any case.
- **Observable / route:**
  - `GET /api/profiles` is USER tier (`route_tier_table.h:215`). `POST /api/profile` is ADMIN tier (`:265`).
  - A wrong role gets 403 with `X-Kiln-Auth-Reason: insufficient_role` (`http_auth_http.c:413-415`).
  - `GET /api/auth/session` returns `role` and `auth_enabled` (`web_auth_session_status_http.c:55-56/124-126`; OPEN, `route_tier_table.h:140`).
- **PASS:**
  - Precondition: `auth_enabled` is true.
  - Log in through `_SecHttpClient.login` (`cases_web_rw.py:373`) with the user credentials, then check that `/api/auth/session` reports `role=="user"`.
  - With that cookie, `GET /api/profiles` returns 200 with a JSON array.
  - With the same cookie, `POST /api/profile` with the body `name=BT_PROF11` (no `seg_count`) returns 403 with `insufficient_role`. A 401 is accepted as a refusal, following the OT-E10 precedent (`cases_ota.py:1341-1400`).
  - `/api/profiles` is unchanged before and after.
- **FAIL:**
  - The list is refused.
  - The POST returns 200, or returns 400, which means the request got past the gate to the handler.
  - The list changed.
- **INCONCLUSIVE:** Auth is disabled on the bench. Do not turn it on.
- **SKIP:** User credentials are missing from `KILNCTL_WEB_USER_USERNAME`/`_PASSWORD` or from the ctx keys `web_user_username`/`web_user_password`.
- **Credentials:** Never log or persist them (plan §6 rule 9).
- **Fake-board test:**
  - Positive: session `role:user`, list 200, POST 403 with the reason header.
  - Negative: the POST returns 400 `seg_count` missing. This must be a FAIL.

#### WEB-STIM-02 — Zone timing-profile assignment
- **Intent:** Round-trip a zone's timing-profile assignment through `POST /api/zones` and restore it.
- **Class:** NEEDS OWNER. There is no narrow writer, so the only write path is the whole-config POST, which also re-syncs the Pico `abs_max_temp_c`.
- **Observable / route:**
  - `GET /api/zones` returns the `timing_profiles` list with `index` and `name` (`zones_http_get.c:359-374`), and `zones[i].timing_profile` (`:492/:511`).
  - Page consumers: `profileOptionsHtml` (`safety_config_page.html:245-253`), the `.tprofile` select (`:271`), and the save that sends `z<i>_timingprofile` (`:354`) to `/api/zones` (`:360`).
  - The POST requires that field and bounds-checks it (`zones_http_post_parse.c:305-318`).
  - Tier ADMIN for GET and POST (`route_tier_table.h:232-233`).
- **PASS (read-only):**
  - `timing_profiles` is not empty, and `timing_profiles[k].index` equals k.
  - Every zone i below `thermo_count` has `timing_profile` in 0..len-1.
  - Static check: the served `/settings/safety` HTML contains `profileOptionsHtml`, `tprofile` and `_timingprofile`.
- **FAIL:** An assignment is out of range, the indexes are inconsistent, or a static token is missing.
- **INCONCLUSIVE:** `thermo_count` is 0.
- **Fake-board test:**
  - Positive: 2 profiles, zones with `timing_profile` 0 and 1.
  - Negative: a zone has `timing_profile:2` while only 2 profiles exist. This must be a FAIL.
- **NEEDS OWNER:**
  - Why the write is a problem: tools/PcTools has no `control_set_zone_timing_profile`. The writers in `mcp_server_control.py` (`:445/:458/:621/:844/:1090`) all go through the whole-page POST. That POST rewrites the full config (`zones_http_post.c:654/673`) and also pushes the Pico `abs_max_temp_c` ceiling up and down (`:559-646`), which comes close to the §6 rule 4 ban.
  - Question: is a write round trip acceptable at all?
  - Recommended: read-only, following WEB-ZONE-14. If the owner wants a write round trip, first add a narrow `zone_timing_profile` POST.

NEEDS OWNER ids: WEB-STIM-02. Owner should also sign off on the plan §6 rule 12 conflict for the transient scratch user slots used by WEB-PROF-03/04/05/06/07.

### 4.3 Zones `/zones` (WEB-ZONE)

#### WEB-ZONE-02 — POST /api/zones round trip of the unchanged config is byte-identical
- **Intent:** Re-POST the config exactly as GET returned it; GET afterwards must be byte-identical.
- **Class:** NEEDS OWNER — MUTATING whole-page write. A no-change POST can still write abs_max_temp_c to the Pico (plan §6 rule 4) and can silently round the stored model.
- **Observable / route:** `GET /api/zones` (emitter `zones_http_get.c:284-332` top level, `:454-512` per zone, `:523-526`, `:613-621`, `:669-673`) then `POST /api/zones`, which returns plain text `ok` (`zones_http_post.c:722`). Page save POST is at `zones_page.html:2897`; page load GET at `:2610`. Tier ADMIN (`route_tier_table.h:232-233`).
- **CLAUDE.md %.4f note is stale:** PID gains now print at `%.9g` (`zones_http_get.c:455`), which is lossless. Byte-identity of the config text is reachable, with two exceptions:
  - `generation` always increments on success (`zones_http_post.c:654-663`). It is excluded from the text compare and must equal pre+1, or FAIL.
  - Live telemetry is excluded: `safety_wiring`, `ct_warn_mask`, `safety_ceiling`, `safety_tc_type*`, `normal_current_*`, `fuzzy_model_valid`.
- **Body:** `zones_http_client.build_post_body(snapshot, {})` (`zones_http_client.py:1070`), with the omit-preserved keys (`z%u_k`, `z%u_tau`, `z%u_deadtime`, coupling/coupling_diag; `zones_http_post_parse.c:718-760`) stripped.
  - Reason: GET prints tau and deadtime at %.1f (`zones_http_get.c:476`). Re-posting them rounds the measured model without changing the GET text.
- **PASS:**
  - POST answers `ok`.
  - The second GET, with exclusions applied, equals the first GET byte for byte.
  - generation is exactly first+1.
- **FAIL:**
  - Any non-excluded byte differs.
  - generation did not advance.
  - A non-409 error is returned.
  - The restore read-back mismatches.
- **INCONCLUSIVE:**
  - 409 from system_mode_gate (`zones_http_post.c:171-173`, `:209`).
  - The gate preconditions below are not met.
  - Generation moved between snapshot and POST (another writer).
- **Mutation / gate / restore:**
  - Writes the whole zones config with identical values.
  - Run only when all of these hold:
    - the suite is mutating and the board lock is held
    - `GET /api/profile_exec` state is idle
    - `GET /api/autotune` state is idle
    - `safety_ceiling.pico_known` is true and `target_c == pico_current_c`
  - The ceiling condition exists because `safety_ceiling_sync_guard_raise` and `apply_lower` write the Pico unless the two agree within 0.01 °C (`zones_http_post.c:578-646`, `:699-716`; `safety_ceiling_policy.c:65-80`, `:121-147`; EPS at `safety_ceiling_policy.h:128`).
  - In finally, re-POST the original snapshot body and read it back. A restore mismatch is an unconditional FAIL and taints the run (plan §5.3 rule 2).
- **Fake-board test:** positive: GET1, then POST `ok`, then GET2 identical except generation+1. Negative: GET2 has `pid_ki` `0.000123` where GET1 had `0.0001234` (must FAIL).
- **NEEDS OWNER:** GET prints the ceiling at %.1f, so the gate cannot prove the two values agree within 0.01 °C. A residual Pico abs_max resync is possible, which rule 4 forbids. Also, should "byte-identical" exclude the omit-preserved model keys? — recommended: until the firmware exposes the exact ceiling or a skip-ceiling-sync flag, implement as above with the strict ceiling gate and stripped model keys. Otherwise reduce to read-only: GET twice, and check that `build_post_body(snapshot, {})` builds without `ZonesHttpUnknownFieldError` and round-trips its own parse.

#### WEB-ZONE-03 — zone type / failsafe / relay type / TC type selects write and read back
- **Intent:** The page's zone_type, failsafe, relay type and TC type selects write and read back.
- **Class:** NEEDS OWNER — these selects have no narrow writer, so writing them is a whole-page POST.
  - Changing `tc_type` reconfigures MAX31856 channel hardware.
  - Changing `zone_type` switches PID/on-off control.
  - Changing relay type pushes to the Pico (`zones_http_post.c:672`).
  - Each write carries the WEB-ZONE-02 ceiling hazard.
- **Observable / route:** `GET /api/zones` fields:
  - `zones[].zone_type`, `failsafe_state` (`zones_http_get.c:523-526`)
  - `zones[].tc_type`, `relay_type` (`:454-512`)
  - `relay_types[]` (`:353-358`)
  - Consumers: `zones_page.html:1424` (tcTypeSelectHtml), `:1559-1560` (zoneType/failsafeOn), `:2727` (`z%u_failsafe` push), `:2855` (stored tc_type).
  - Writer: POST `/api/zones`, relay`<N>`_type parse at `zones_http_post.c:512`. Tier ADMIN (`route_tier_table.h:232-233`).
- **PASS (recommended read-only):**
  - For every configured zone (i < thermo_count), zone_type, failsafe_state, tc_type and relay_type are present integers.
  - zone_type is in {0,1}; failsafe_state is in {0,1}.
  - `relay_types` length equals relay_count.
  - The served HTML contains `tcTypeSelectHtml(` and the `.failsafestate` select markup.
- **FAIL:** Any field is missing or non-integer, or a value is outside its range for a configured zone, or the static markers are absent.
- **INCONCLUSIVE:** thermo_count == 0.
- **Fake-board test:** positive: 3 zones, each with zone_type 0, failsafe_state 0, tc_type 3, relay_type 0. Negative: zone 1 has `"zone_type":7` (must FAIL).
- **NEEDS OWNER:** Is a live write of these selects wanted? — recommended: keep this read-only plus static check. If a write is required, the only safe target is toggling `failsafe_state` on one zone via the WEB-ZONE-02 gated GET-merge-POST, with restore in finally and read-back. Never write tc_type.

#### WEB-ZONE-04 — "Same as zone N" groupsrc selectors, zone 0 included
- **Intent:** Every zone, zone 0 included, renders the "Same as zone N" group-source selectors.
- **Class:** READ-ONLY
- **Observable / route:** Static markup `class="groupsrc"` with `data-group` limits/relaytiming/control/guards/tc (`zones_page.html:1661-1666`), plus `GET /api/zones` → `zones[].settings_source_groups{limits,relaytiming,control,guards,tc}` and `settings_source` (emitter `zones_http_get.c:669-673`). Tier ADMIN (`route_tier_table.h:233`).
- **PASS:**
  - The served HTML contains all 5 data-group values on groupsrc selects. The rendering code is not gated to i>0, so zone 0 also gets them.
  - Every configured zone has all 5 keys in settings_source_groups.
  - Each value is either 255 (own) or an integer zone index < thermo_count that is not the zone's own index.
- **FAIL:** A data-group marker is missing, a key is missing, a value is out of range, or a zone points at itself.
- **INCONCLUSIVE:** thermo_count == 0.
- **Fake-board test:** positive: zone 0 groups all 255; zone 1 `"limits":0`. Negative: zone 2 `"tc":5` with thermo_count 3 (must FAIL).

#### WEB-ZONE-05 — PID fields → POST /api/zones/pid
- **Intent:** The page's PID fields write through POST /api/zones/pid.
- **Class:** NEEDS OWNER — zones_page.html never calls `/api/zones/pid`; its PID inputs go through the whole-page save at `zones_page.html:2897`. The route's consumers are `main_page.html:1075` and `setup_wizard_page.html:1341`.
- **Observable / route:** `POST /api/zones/pid` with form `zone`, `kp`, `ki`, `kd`.
  - Handler `zones_http_pid.c:105`: system_mode_gate refusal at `:112-114`; zone parse at `:136-143`; kp/ki/kd at `:151-160`.
  - Responses: `{"ok":true}` at `:172`; `{"ok":false,...}` at `:167`.
  - Read-back: `GET /api/zones` → `zones[z].pid_kp/pid_ki/pid_kd` at `%.9g` (`zones_http_get.c:455`). Tier ADMIN (`route_tier_table.h:234`).
- **PASS (recommended identity write):**
  - Zone 0 gains are read from GET and POSTed back as the exact `%.9g` strings.
  - The response is `{"ok":true}`.
  - A GET read-back shows identical pid_kp/ki/kd strings.
  - All other zone-0 fields are unchanged.
- **FAIL:** The response is `ok:false`, any gain string differs, any other config field changed, or the restore read-back mismatches.
- **INCONCLUSIVE:** 409 from system_mode_gate, or thermo_count == 0.
- **Mutation / gate / restore:**
  - Writes zone 0's own current gains (net no-op).
  - Gate: mutating suite, board lock held, profile_exec idle, autotune idle.
  - In finally, re-POST the snapshot gains and verify by read-back. A mismatch is an unconditional FAIL.
- **Fake-board test:** positive: GET kp `2.5` ki `0.000123` kd `0`, then POST ok, then GET same. Negative: read-back `"pid_ki":0.00012` (must FAIL).
- **NEEDS OWNER:** The plan maps a page control to a route the page does not use. — recommended: test the route itself with the identity write above (the page's own PID save is already covered by WEB-ZONE-02), or retarget this id to main_page/setup_wizard.

#### WEB-ZONE-06 — autotune start / abort / accept drive /api/autotune/*
- **Intent:** The autotune Start step, Start relay, Abort and Accept buttons drive `/api/autotune/*`, with AT-01/02/03 observed through the page.
- **Class:** OBSERVER (depends_on AT-01 for start, AT-02 for abort, AT-03 for accept). This case never presses anything.
- **Observable / route:**
  - Static markup: ids `atStartBtn` (`zones_page.html:476`), `atAbortBtn` (`:479`), `atAcceptBtn` (`:507`), `atAckUnsettled` (`:487`).
  - Page posts: start `zone=&step_duty=&rule=` (`:4161-4163`); abort (`:4176`); accept `ack_unsettled=&adopt_ceiling=` (`:4194-4197`).
  - Status: `GET /api/autotune` → `state`, `sub_phase`, `method` (emitter `dashboard_json.c:374`; consumer pollAutotune `zones_page.html:3199`).
  - Tiers ADMIN: start/accept/abort/status at `route_tier_table.h:320`, `:321`, `:328`, `:331`.
- **PASS:**
  - Static ids and fetch paths are present.
  - While AT-01 runs, GET state is non-idle and in AT_STATE_NAMES.
  - After AT-02, state is aborted/idle.
  - After AT-03's refused accept, state and gains are unchanged.
  - Each of these must agree with the verdict of the AT case it observes.
- **FAIL:** A static id or path is missing, or GET state contradicts a PASSing AT case (e.g. idle while AT-01 reports running).
- **INCONCLUSIVE / NOT_RUN:** NOT_RUN for each part whose AT case did not run. The "Start relay" part is N/A because the relay-test UI was removed (`zones_page.html:4170-4174`).
- **Fake-board test:** positive: HTML with the 3 ids, plus `{"state":"stepping",...}` with AT-01 running. Negative: `{"state":"idle"}` while AT-01 reports running (must FAIL).

#### WEB-ZONE-07 — GET /api/tuning_recommendations renders a method
- **Intent:** GET /api/tuning_recommendations yields a method for the page to render.
- **Class:** READ-ONLY
- **Observable / route:** `GET /api/tuning_recommendations` serves the embedded artifact, either real or the fallback with `schema_version` 1 and empty `recommendations` (handler `zones_http.c:685-691`, artifact note `:334-346`). Consumers: `zones_page.html:3404` (tuningRecArtifactUsable), `:3417` (pickTuningRecommendation), `:3508` (fetch), `:3517` (render). Tier ADMIN (`route_tier_table.h:335`).
- **PASS:**
  - Response is 200 JSON with `schema_version == 1` and `recommendations` a non-empty list.
  - A Python mirror of pickTuningRecommendation picks a row with a non-empty `method`.
  - Static ids `tuningRecPanel` (`:452`) and `tuningRecResult` (`:463`) are present.
- **FAIL:** Non-JSON response, schema_version != 1, `recommendations` missing or not a list, a non-empty list where no row has a method, or static ids missing.
- **INCONCLUSIVE:** `recommendations == []` (fallback artifact baked in).
- **Fake-board test:** positive: `{"schema_version":1,"recommendations":[{"method":"simc",...}]}`. Negative: `{"schema_version":2,"recommendations":[]}` (must FAIL).

#### WEB-ZONE-08 — coupling matrix + RGA render after AT-05
- **Intent:** The coupling matrix and RGA render after AT-05.
- **Class:** OBSERVER (depends_on AT-05)
- **Observable / route:** `GET /api/autotune/matrix` (emitter `dashboard_autotune_http.c`):
  - `zone_count`, `cells` at `:128`
  - cells `{i,j,valid,k,tau_s,dead_time_s}` at `:136-139`
  - `rga{available:true,n,det,zones,lambda}` at `:161-174`
  - `rga{available:false,code,reason}` at `:188`
  - Consumers: `zones_page.html:3933` (pollCouplingMatrix) and `:3651` (renderRga).
  - Static ids: `couplingMatrix` (`:564`), `rgaMatrix` (`:577`). Tier ADMIN (`route_tier_table.h:332`).
- **PASS:**
  - Cells has zone_count² entries covering each (i,j) once; every valid cell has numeric k/tau_s/dead_time_s.
  - `rga` is present. When available: n ≥ 2, zones has length n, lambda is n×n numeric, det is numeric.
  - When not available, reason is a non-empty string.
  - Static ids are present.
- **FAIL:** Shape violation, missing rga, or AT-05 PASSed (cells valid) while every cell here is `valid:false`.
- **INCONCLUSIVE / NOT_RUN:** NOT_RUN if AT-05 did not run.
- **Fake-board test:** positive: zone_count 3, 9 cells, with `rga{available:true,n:2,zones:[0,1],lambda:[[1,0],[0,1]],det:0.5}`. Negative: zone_count 3 with 8 cells (must FAIL).

#### WEB-ZONE-09 — Continuous Tuning enable/revert toggles and reverts; gains unchanged
- **Intent:** Continuous Tuning enable/revert (`/api/adaptive_tune/*`) toggles and reverts, and gains are unchanged afterwards.
- **Class:** MUTATING
- **Observable / route:**
  - `GET /api/adaptive_tune` → `zones[].enabled`, `revert_available`, `has_applied` (emitter `adaptive_tune_http.c:85-95`).
  - `POST /api/adaptive_tune/enable` with `zone=&enabled=`: needs both fields (`:154-158`); gate on enable=true only (`:183-193`); responds `{"ok":true}` (`:197`).
  - `POST /api/adaptive_tune/revert`: gated (`:242-252`).
  - Consumers: `zones_page.html:4044`, `:4091`, `:4117`.
  - Tiers ADMIN: `route_tier_table.h:329-330`, `:334`.
  - Gains from `GET /api/zones` pid_kp/ki/kd and model_k_dc (`zones_http_get.c:455`, `:476`).
- **PASS:**
  - Flipping zone 0's `enabled` reads back as flipped.
  - Restoring reads back as the original value.
  - zone 0 pid_kp/ki/kd/model_k_dc strings are identical before and after.
- **FAIL:**
  - Enable returns non-ok, or the read-back does not show the flipped value.
  - Gains changed.
  - The restore read-back mismatches (unconditional FAIL).
- **INCONCLUSIVE:**
  - 409 from system_mode_gate.
  - Revert is never pressed, because it rewrites gains (`adaptive_tune_http.c:237-241`). The revert half is reported INCONCLUSIVE ("revert not exercised"), not PASS.
- **Mutation / gate / restore:**
  - Writes the adaptive_tune enabled flag for zone 0.
  - Gate: mutating suite, board lock held, profile_exec idle, autotune idle.
  - In finally, POST `enabled=<original>` and verify the GET read-back.
- **Fake-board test:** positive: enabled false, then POST ok, then GET true, then POST ok, then GET false, with gains equal. Negative: the restore read-back still shows `"enabled":true` (must FAIL).

#### WEB-ZONE-10 — Measure Zone Normal Current: start refused or INCONCLUSIVE, abort answers 200
- **Intent:** Measure Zone Normal Current. Start is refused or INCONCLUSIVE on this fixture (sweep floor 0.045 A); the ADMIN abort answers 200.
- **Class:** READ-ONLY. The abort is a no-op while idle (`zones_current_sweep_task.c:2492-2497`). **Never POST start:** the sweep derives and pushes k_ct (CT calibration, plan §6 rule 4).
- **Observable / route:**
  - `GET /api/zones/current_sweep/status` → `state`, `ct_installed` and related fields (emitter `zones_http.c:516-529`; consumer `zones_page.html:2487-2573`).
  - `POST /api/zones/current_sweep/abort` → `{"ok":true}` (`zones_http.c:449-454`; consumer `zones_page.html:2590`).
  - Static `sweepStartBtn` / `sweepAbortBtn` (`zones_page.html:439-440`).
  - Tiers ADMIN: `route_tier_table.h:242-243`.
- **PASS:**
  - Status is well-formed JSON with state in {idle,done,aborted,failed}.
  - Abort with the admin session returns 200 `{"ok":true}`.
  - A second status read still shows a non-running state.
  - Static ids are present.
- **FAIL:** Abort returns non-200 or is not ok:true with the admin session, status is malformed, or state becomes `running` after the abort.
- **INCONCLUSIVE:**
  - state == `running` before the case: do not abort another owner's sweep.
  - The start-refused half is always INCONCLUSIVE ("start not exercised: writes CT calibration").
- **Fake-board test:** positive: status `{"state":"idle",...}`, then abort `{"ok":true}`. Negative: abort answers 401/403 with the admin session (must FAIL).

#### WEB-ZONE-11 — Tuning quality / Firing quality sections populate after HP-01
- **Intent:** The Tuning quality and Firing quality sections populate after HP-01.
- **Class:** OBSERVER (depends_on HP-01)
- **Observable / route:**
  - `GET /api/zones` → `zones[].tuning_valid`, `method`, `settled` and related fields (`zones_http_get.c:613-621`; consumer renderTuningQuality `zones_page.html:3774`, gate `:2469`).
  - `GET /api/profile_exec` → `zones[].firing_stats{...,sample_count}` (emitter `dashboard_json.c:196-201`; consumer `zones_page.html:3879`).
  - `GET /api/firing_history?profile_id=` → firing_stats (`dashboard_json.c:483-488`; consumer `zones_page.html:3911`).
  - Static `tuningQuality` (`:539`), `firingStatsCurrent` (`:554`), `firingStatsHistory` (`:557`).
  - The tiers of /api/profile_exec and /api/firing_history were not opened.
- **PASS:**
  - After HP-01, at least one firing_stats has sample_count > 0, either in profile_exec or in the newest history row for HP-01's profile_id.
  - tuning_* fields are well-typed for every configured zone.
  - Static ids are present.
- **FAIL:** HP-01 PASSed but every firing_stats has sample_count 0 or is absent, tuning_* fields are malformed, or static ids are missing.
- **INCONCLUSIVE / NOT_RUN:**
  - NOT_RUN if HP-01 did not run.
  - Tuning-quality half INCONCLUSIVE when no zone has `tuning_valid` (section gated off).
- **Fake-board test:** positive: profile_exec zones[0].firing_stats.sample_count 120. Negative: HP-01 PASS while all zones show `"sample_count":0` (must FAIL).

#### WEB-ZONE-12 — GET /api/zones_diag is well-formed
- **Intent:** GET /api/zones_diag returns well-formed JSON.
- **Class:** READ-ONLY
- **Observable / route:** `GET /api/zones_diag` → `{"generation","zones":[{index,coupling_tau_c0..2,coupling_dead_time_c0..2,model_fit_temp_c,model_fit_ambient_c}]}`. Emitter `zones_http_get.c:775`, `:778`, `:784`, `:790`; 500 `{"ok":false}` on truncation or OOM. No page consumer; client `zones_http_client.get_zones_diag` / `merge_zones_diag` (`zones_http_client.py:200`, `:240`). Tier ADMIN (`route_tier_table.h:260`).
- **PASS:**
  - Response is 200 JSON with integer `generation`.
  - `zones` has length 3, indices 0..2 in order.
  - All 6 coupling keys and both model_fit keys are numeric (−273.15 means unknown and is allowed).
  - `merge_zones_diag(get_zones(), diag)` succeeds.
- **FAIL:** Non-200 or `ok:false`, a missing key, a non-numeric value, a wrong length or index, or a merge exception.
- **INCONCLUSIVE:** generation differs from an adjacent GET /api/zones (config changed mid-read).
- **Fake-board test:** positive: 3 zones with all keys, generation 7 matching zones. Negative: zone 1 missing `coupling_dead_time_c2` (must FAIL).

#### WEB-ZONE-13 — GET /api/zones/ct_channel_map matches the Pico's commissioning
- **Intent:** The CT channel map matches the Pico's commissioning.
- **Class:** READ-ONLY
- **Observable / route:** `GET /api/zones/ct_channel_map` (emitter `zones_http.c:542-645`):
  - `mask` and `zone[3]`, sweep-derived
  - `committed_mask` and `committed_zone[3]`, from Pico params 0x0106-0x0108 via the safety_cfg_store mirror (`:561-603`)
  - `k_mask` and `k[3]` (`:614-641`)
  - Consumers: `safety_commissioning_page.html:1681`, `setup_wizard_page.html:2631`. Tier ADMIN (`route_tier_table.h:259`).
- **PASS:**
  - All 6 keys are present and well-typed, with arrays of length 3.
  - For every channel c with its bit set in both `mask` and `committed_mask`, `zone[c] == committed_zone[c]`.
  - At least one channel is compared.
- **FAIL:** A missing or ill-typed key, an array length other than 3, or any compared channel disagreeing.
- **INCONCLUSIVE:** `committed_mask == 0` (Pico not commissioned), or `mask & committed_mask == 0` (no sweep ever run, nothing to compare).
- **Fake-board test:** positive: mask 7, zone [0,1,2], committed_mask 7, committed_zone [0,1,2]. Negative: committed_zone [0,2,1] with both masks 7 (must FAIL).

Resolved: WEB-ZONE-02, WEB-ZONE-03, WEB-ZONE-05 - Owner 2026-10-09: accepted recommendation.

### 4.4 Safety, commissioning, readiness (WEB-SAF, WEB-COMM, WEB-RDY)

#### WEB-SAF-02 — Frame B diag card agrees with safety_get_diag()
- **Intent:** The /safety diagnostics card (Frame B) shows the same guard state and trip/warn masks as the PcTools `safety_get_diag()` cache read.
- **Class:** READ-ONLY
- **Observable / route:** `GET /api/status` → `diag_ever_received` (`dashboard_status_http.c:736`), `diag_trip_reason` :738, `diag_warn_mask` :742, `diag_trip_mask` :743, `diag_state` :744, `diag_context_age_100ms` :759; consumer renderDiagCard `safety_page.html:380-393`, element `id="diagCard"` `safety_page.html:161`; tier OPEN (`route_tier_table.h:160`). Page `/safety` is ADMIN (`route_tier_table.h:460`). Reference: `safety_get_diag` (`mcp_server_safety.py:195`, `devices_safety.py:84`), injectable as `ctx["safety_get_diag"]`.
- **PASS:** Served /safety HTML has `id="diagCard"`. Both sources report ever_received true. trip_mask, warn_mask, state and trip_reason are equal, either on one sample or on a single immediate retry of both reads (allows for a push landing between the reads).
- **FAIL:** diagCard is missing, or any of the four fields still differs after the retry, or HTTP reports `diag_ever_received` true while the UART cache reports never-received (or the reverse) on both samples.
- **INCONCLUSIVE:** Both sources report never-received (no Pico or link down), or `safety_get_diag` errors with SafetyQueryError (serial not attached).
- **Fake-board test:** positive: status `{diag_ever_received:true, diag_trip_mask:0, diag_warn_mask:0, diag_state:2, diag_trip_reason:0}` with a matching fake diag passes. Negative: fake diag trip_mask 0x20 against HTTP `diag_trip_mask:0` on both samples must FAIL.

#### WEB-SAF-03 — Clear-latched-trip button: disabled with no trip, enabled and effective in OT-B01's window
- **Intent:** The "Clear latched trip" button is disabled when there is no trip, and enabled and effective while OT-B01 holds its S6a trip.
- **Class:** OBSERVER (depends_on OT-B01) / NEEDS OWNER — OT-B01 has no pre-clear observer hook, and the enable predicate is "trip ever received this Pico boot", not "trip latched now".
- **Observable / route:**
  - Static: `<button id="clearTripBtn" class="clear-trip" disabled>` (`safety_page.html:172`).
  - Enable input: `GET /api/status` → `trip_event_ever_received` (`dashboard_status_http.c:765`). Consumer: `safety_page.html:303-324` (enabled :316, disabled :322).
  - The flag resets only on Pico reboot (`safety_link_frames.c:300`) and is set at :1175. A clear does not reset it.
  - Action: `POST /api/safety/clear_trip` (`safety_page.html:486`). Emitter `dashboard_exec_http.c:883`: refusal `{"ok":false,"error":"no trip currently latched, or Pico diagnostics are stale"}` :894, `{"ok":true}` :901. Tier ADMIN (`route_tier_table.h:294`).
- **PASS:**
  - (a) Pre-state, read-only: static `disabled` attribute present, and `trip_event_ever_received==false` with `diag_trip_mask==0`.
  - (b) In the window, read before OT-B01's clear: `ctx["_otb01"].outcome=="s6a_latched"`, `trip_event_ever_received==true` and `diag_trip_mask&0x20`.
  - (c) "Effective" is taken from OT-B01's own result (its clear_trip_fn returned `{"ok":true}` and the readback mask cleared, `ctx["_otb01"].clear_ok`). The observer never POSTs a clear itself.
- **FAIL:**
  - The static `disabled` attribute or the button id is missing.
  - In the window, `trip_event_ever_received` is false.
  - OT-B01 reports clear_ok false after an s6a_latched outcome.
- **INCONCLUSIVE:** Half (a) only: `trip_event_ever_received==true` with mask 0, meaning an earlier trip this boot leaves the button enabled by design.
- **NOT_RUN:** OT-B01 did not run, or its outcome is not `s6a_latched`. A dual reset has been observed to give `no_trip`.
- **Fake-board test:** positive: pre `{trip_event_ever_received:false, diag_trip_mask:0}`, with `_otb01={outcome:"s6a_latched", clear_ok:true}` and a window status `{trip_event_ever_received:true, diag_trip_mask:32}`, passes. Negative: window status `trip_event_ever_received:false` with outcome s6a_latched must FAIL. `_otb01` absent must give NOT_RUN.
- **NEEDS OWNER:** Add a pre-clear hook (e.g. `ctx["_otb01_window_observers"]`, run in `cases_ota.py` `_case_otb01` just before clear_trip_fn)? Also accept that "disabled when no trip" can only be checked when no trip has been seen since the Pico booted? — recommended: yes to both. Without the hook, judge only half (a) plus OT-B01's clear_ok, and report the window half NOT_RUN.

#### WEB-SAF-04 — GET /api/status?diag=1 detail fields present
- **Intent:** `/api/status?diag=1` returns the Frame B detail fields the /safety page needs.
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/status?diag=1`; query parsed at `dashboard_status_http.c:279-286`.
  - Fields: `ok`, `diag_ever_received` :336. When ever_received: `diag_boot_reason` :339, `diag_context_frames_ok` :340, `diag_context_frames_bad` :341, `diag_tx_frames_dropped` :342, `diag_log_frames_dropped` :343, `safety_tc_reconfig_gave_up` :344, `safety_s1_abs_max_disabled` :346, `safety_s8_rate_guard_disabled` :348.
  - Consumer: pollDiagDetail `safety_page.html:545`; numeric checks :399-401; boot reason :403-407.
  - Tier OPEN (`route_tier_table.h:160`).
- **PASS:** `ok==true` and `diag_ever_received` is a bool. When true, the four page-required keys (`diag_boot_reason`, `diag_context_frames_ok`, `diag_context_frames_bad`, `diag_tx_frames_dropped`) are all numbers, and the other four keys are present.
- **FAIL:** Response is not JSON, `ok!=true`, or `diag_ever_received==true` with any required key missing or non-numeric.
- **INCONCLUSIVE:** `diag_ever_received==false` (only the presence of the flag is judged; no Pico data yet).
- **Fake-board test:** positive: `{ok:true, diag_ever_received:true, diag_boot_reason:1, diag_context_frames_ok:10, diag_context_frames_bad:0, diag_tx_frames_dropped:0, diag_log_frames_dropped:0, safety_tc_reconfig_gave_up:false, safety_s1_abs_max_disabled:false, safety_s8_rate_guard_disabled:false}`. Negative: the same payload with `diag_context_frames_bad` removed must FAIL.

#### WEB-COMM-02 — Guided commissioning screens 1-4 prefill from the board's current answers
- **Intent:** The guided flow walks screens 1-4 to "Stage & commit" using the board's current answers, without committing.
- **Class:** READ-ONLY — a reduction. No JS execution, so the prefill inputs and the static flow are judged, and nothing is POSTed.
- **Observable / route:**
  - `GET /api/safety/commissioning` → `link_up` (`safety_cfg_http.c:317`), `stale` :320, `unset_reporting_reliable` :339, `params[]` {id, name, type, set, value only if set} :386-433.
  - Consumers: gPrefill `safety_commissioning_page.html:2100`, gBuildAnswers :2025, review gRenderReview :2075.
  - Static: `id="gStartBtn"` :335 and `id="gCommitBtn"` with `onclick="gCommit()"` :399.
  - Tier ADMIN (`route_tier_table.h:296`), page ADMIN :461.
  - Wrapper: `safety_get_commissioning` (`mcp_server_safety.py:929`) / `safety_cfg_http_client.get_commissioning` :110.
- **PASS:** HTML contains gStartBtn, gCommitBtn, `function gPrefill` and `function gBuildAnswers`. The GET has `link_up:true`, `stale:false`, `unset_reporting_reliable:true`, and every guided param gPrefill reads (abs_max_temp_c 260, tc_type 261, ct_installed 265, and the others the guided screens use) is present with `set:true` and a typed `value`.
- **FAIL:** A static element or function is missing, or a guided param is missing from `params[]`, or has `set:true` with no `value`.
- **INCONCLUSIVE:** `link_up:false`, `stale:true` or `unset_reporting_reliable:false`, or the board is uncommissioned (guided params `set:false`).
- **Fake-board test:** positive: a GET with all guided params set, plus HTML containing the four markers. Negative: param 260 with `set:true` and no `value` key must FAIL.

#### WEB-COMM-03 — Advanced "Stage & commit" of the unchanged set, then verify
- **Intent:** The advanced "Stage & commit" of the unchanged field set POSTs commit=1, and kcCommissioningCommitAndVerify's re-GET matches.
- **Class:** NEEDS OWNER — the "unchanged" re-commit is a real write and includes abs_max_temp_c and the CT gains.
- **Observable / route:**
  - The saveBtn handler (`safety_commissioning_page.html:1792-1867`) posts every `input[data-id]` and `select[data-id]` with a value (:1812-1818). That includes abs_max_temp_c id 260 (:493, editable f32) and CT gains 779-781. Unset bools post 0 (:1106/:1814).
  - `POST /api/safety/commissioning` (`safety_cfg_http.c:773`) calls `safety_cfg_write_apply_pairs(..., commit)` :818, with no exclusion of 260.
  - On the Pico (`config_store_flash.c:1372`): while ARMED, an unchanged record is not a "narrow" change (`config_store.c:1291-1313`, :1318-1319), so it is refused as REFUSED_ARMED. A healthy idle Pico is ARMED (`relay_owner.c:83`). When not ARMED it is a real flash write: seq+1 (:1436), config CRC changes, derived fields are re-finalized and CT cal is reloaded (`link_task.c:2585-2604`).
  - If 0x0212 is in the pairs, the commit clears E-stop verification (`safety_cfg_write.c:517-547`).
  - With no critical changes, commitAndVerify skips both the busy check and the re-GET (`commissioning_shared.js:166-175`, :214-216).
  - Tier ADMIN (`route_tier_table.h:295`).
- **PASS / FAIL / INCONCLUSIVE (recommended read-only form):**
  - PASS: two GETs of commissioning a few seconds apart give an identical `live_config_crc` (:318), `cached_config_crc` :319 equals `live_config_crc`, and `commissioned:true` :321. HTML contains `id="saveBtn"` (:413) and `kcCommissioningCommitAndVerify`.
  - FAIL: CRC mismatch between the two GETs, or the marker is missing.
  - INCONCLUSIVE: `link_up:false` or `stale:true`.
- **Fake-board test:** positive: two GETs with equal CRCs plus a static marker. Negative: the second GET has a different `live_config_crc` and must FAIL.
- **NEEDS OWNER:** Any commit=1 sends abs_max_temp_c and CT gains (forbidden by section 6 rule 4). It is refused while ARMED and otherwise rewrites flash and may clear estop_verified. — recommended: the read-only form above. If the owner insists on a write, POST only `commit=1` with one non-CT, non-abs_max, non-0x0212 param at its current value, gated on Pico not ARMED and profile_exec idle, with CRC drift accepted.

#### WEB-COMM-04 — Commit refused client-side while HP-01 runs
- **Intent:** With HP-01 firing, a commissioning commit is refused client-side by kcCommissioningCheckBusy.
- **Class:** OBSERVER (depends_on HP-01) — read-only reduction; never POSTs.
- **Observable / route:**
  - `GET /api/profile_exec` → `state` ("running"/"paused"). Consumer checkBusy `commissioning_shared.js:83-100` (state test :89). Tier OPEN (`route_tier_table.h:161`).
  - Static: `kcCommissioningCheckBusy` export (`commissioning_shared.js:273-276`), served from `/commissioning_shared.js` (OPEN, `route_tier_table.h:159`).
  - Note: checkBusy runs only for critical changes (:214-216). The server-side guard `cfg_writer_guarded` (`safety_cfg_http.c:71-81`, 409) covers concurrent commissioning ops, not firings.
- **PASS:** During HP-01's run window, `GET /api/profile_exec` reports `state` "running" or "paused", and the served JS contains `checkBusy` and the `kcCommissioningCheckBusy` export.
- **FAIL:** During HP-01's confirmed run window `state` is anything else, or the JS markers are missing.
- **INCONCLUSIVE:** HP-01 ran but finished or aborted before the observer sampled.
- **NOT_RUN:** HP-01 did not run.
- **Fake-board test:** positive: `/api/profile_exec` `{state:"running"}` with a JS body containing the markers, and HP-01 marked running. Negative: `{state:"idle"}` while HP-01 is marked running must FAIL. HP-01 absent must give NOT_RUN.

#### WEB-COMM-05 — Bench preset button present, never pressed
- **Intent:** The bench-preset commissioning button exists (dev only) and is NOT pressed.
- **Class:** READ-ONLY
- **Observable / route:**
  - Static: `id="benchBtn"` with `style="display:none"` (`safety_commissioning_page.html:414`). Its handler is behind kcConfirm :1874 and fetches `/api/safety/commissioning/bench_preset` :1884.
  - `dev_tools_enabled` comes from GET commissioning and decides visibility (`safety_commissioning_page.html:323-325`).
  - Route `POST .../bench_preset` is ADMIN (`route_tier_table.h:297`; registered `safety_cfg_http.c:2425`) and is never called.
- **PASS:** Served HTML contains `id="benchBtn"` with an initial `display:none`, plus the `bench_preset` fetch behind a confirm. The case makes zero POSTs.
- **FAIL:** benchBtn is missing, or is visible in the static HTML (no display:none).
- **INCONCLUSIVE:** none.
- **Fake-board test:** positive: HTML with `<button id="benchBtn" style="display:none"`. Negative: HTML with benchBtn and no display:none must FAIL. Assert `fake.post` was never called.

#### WEB-COMM-06 — CT cal/trim/auto-zero controls (channel 2 only in summed topology); POSTs not exercised
- **Intent:** The CT calibration, trim and auto-zero controls render only for channel 2 in summed topology; their POSTs are not exercised.
- **Class:** NEEDS OWNER — "renders only for ch2" is client-side DOM built from `ct_cal[]`; only its inputs can be judged.
- **Observable / route:**
  - `GET /api/safety/commissioning` → `ct_cal[]` {has_value, a_fs, zero_mv, source, trim_offset_a, trim_gain} (`safety_cfg_http.c:361-377`), plus params ct_installed 265 and ct_topology (799) in `params[]` :386-433.
  - Consumer: row builder `safety_commissioning_page.html:1070-1089` (no data-id, per-channel Apply/Auto-zero).
  - Static: `ct-cal-apply` / auto-zero class strings and the wire functions. Fetch sites are auto-zero :1269, trim :1398, ct_cal :1447-1448.
  - Routes ADMIN (`route_tier_table.h:298-300`) are never called.
- **PASS:** HTML has the ct-cal button classes and the three fetch URLs. `ct_cal` is a 3-element array. With ct_topology set and summed, `ct_cal[2].has_value==true`. Zero POSTs.
- **FAIL:** Markers missing, `ct_cal` is not length 3, or summed topology with `ct_cal[2].has_value==false` on a commissioned board.
- **INCONCLUSIVE:** ct_topology unset or not summed, or `ct_installed` false.
- **Fake-board test:** positive: summed topology with `ct_cal[2].has_value:true`. Negative: `ct_cal` of length 2 must FAIL. Assert no POST.
- **NEEDS OWNER:** Is "ch2 only" a DOM claim the harness must prove? — recommended: judge the inputs (topology plus ct_cal[2]) and the static markers as above. Defer the per-channel DOM check to a browser-based suite.

#### WEB-COMM-07 — relay_type round trip of the current value
- **Intent:** Committing relay_type at its current value round-trips (POST, then the read-back matches).
- **Class:** MUTATING (benign same-value write)
- **Observable / route:**
  - `GET /api/safety/commissioning` → `relay_type` ("contactor"|"mercury"; `safety_cfg_http.c:360`, names :250-253).
  - `POST /api/safety/commissioning/relay_type` with `type=<value>` (`relay_type_post_locked` `safety_cfg_http.c:1003-1051`; 400 on other values; `{"ok":true,"persisted":true}` :1050, `persisted:false` :1046). Registered :2430.
  - Consumer: relayTypeSelect change handler `safety_commissioning_page.html:1780-1790`.
  - Tier ADMIN (`route_tier_table.h:301`).
  - ESP-local NVS only, no safety link. Same value does not reset relay-life counts (`relay_cycles.c:563`).
- **PASS:** The POST returns `ok:true, persisted:true`, and the read-back `relay_type` equals the original.
- **FAIL:** `ok!=true`, `persisted:false`, read-back differs, or the restore read-back mismatches (unconditional).
- **INCONCLUSIVE:** The original `relay_type` is missing or not one of the two names (the case skips without writing), or the gate fails.
- **Mutation / gate / restore:**
  - Writes only `type=<original>`.
  - Gate: mutating suite AND `GET /api/profile_exec` state not running/paused.
  - Restore in finally: if the read-back differs from the original, POST the original and re-GET. A mismatch is an unconditional FAIL.
- **Fake-board test:** positive: GET `relay_type:"contactor"`, POST `{ok:true,persisted:true}`, read-back "contactor". Negative: read-back "mercury" (with the restore also reading "mercury") must FAIL.

#### WEB-RDY-02 — Four hard interlocks ok on a healthy board
- **Intent:** On a healthy idle board, the readiness items trip, recovery, crash and E-stop are all ok.
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/readiness` → `items[]` {key, label, status, detail, fix_url} (`readiness_http.c:126-139`; status names :54).
  - Keys: `safety_trip` :833-850, `crash_report` :913, `recovery_mode` :934, `estop_verified` :1109 (gate keys `readiness_gate.h:125-133`).
  - Status logic: `readiness_http.h:311-317`, :336-342, :400-403, :639-642.
  - Consumer: `readiness_page.html:173` (rows) and the MARK map :149.
  - Tier OPEN (`route_tier_table.h:162`). Wrappers: `get_readiness` (`mcp_server_info.py:905`), `capability_preflight.py:484`.
- **PASS:** All four keys are present, each `status=="ok"`, and every item status is in the four-state set.
- **FAIL:** Any of the four keys is missing, or any is `not_done`, or any status falls outside the set.
- **INCONCLUSIVE:** `safety_trip` (or estop) is `cannot_yet` because the link is down.
- **Fake-board test:** positive: four items with status "ok". Negative: `crash_report` with `status:"not_done"` must FAIL.

#### WEB-RDY-03 — Deep link #<key> highlights the row
- **Intent:** Opening `/readiness#<key>` scrolls to and outlines that item's row.
- **Class:** READ-ONLY — static reduction (the URL fragment never reaches the server).
- **Observable / route:**
  - `GET /api/readiness` → `items[].key` (`readiness_http.c:126-139`); consumer row id = key (`readiness_page.html:173`).
  - Hash handler on first render: getElementById(hash), scrollIntoView, outline `2px solid var(--bad)` (`readiness_page.html:187-194`).
  - Producer of deep links: `main_page.html:3025` (`'/readiness#' + encodeURIComponent(result.readiness_item)`).
  - Page tier ADMIN (`route_tier_table.h:472`); API OPEN :162.
- **PASS:** Every item key is non-empty, unique and an id-safe string. The served `/readiness` HTML contains the `location.hash` lookup, `scrollIntoView` and the outline style. Includes the `safety_trip` key.
- **FAIL:** Duplicate or empty key, or the hash-highlight code is missing from the HTML.
- **INCONCLUSIVE:** none.
- **Fake-board test:** positive: unique keys, with HTML containing the markers. Negative: two items with `key:"safety_trip"` must FAIL.

#### WEB-RDY-04 — In OT-B01's trip window: trip item not_done and Start refused
- **Intent:** During OT-B01's latched trip, readiness `safety_trip` is not_done and the dashboard Start is refused.
- **Class:** OBSERVER (depends_on OT-B01) — needs the pre-clear hook (see WEB-SAF-03).
- **Observable / route:**
  - `GET /api/readiness` item `safety_trip`. Status `not_done` when the diag trip mask is not 0 (`readiness_http.h:311-317`). Emitter `readiness_http.c:833-850`.
  - Start refusal: `POST /api/profile_exec/start` evaluates `readiness_gate_evaluate` BEFORE reading the body (`dashboard_exec_http.c:722-749`) and answers `409 {"ok":false,"readiness_item":"<key>","error":...}` :744-746. A body without `id` otherwise gets `400 "id missing or invalid"` (:771-772), before any `profile_executor_run`.
  - Consumer: `main_page.html:3025`.
  - Tier USER (`route_tier_table.h:209`).
- **PASS:**
  - In the window (`_otb01.outcome=="s6a_latched"`, sampled before the clear), `safety_trip.status=="not_done"`.
  - A probe POST of `/api/profile_exec/start` with body `probe=1` (no `id`, so it can never start a firing) returns 409 with `readiness_item` equal to `safety_trip`.
- **FAIL:**
  - The status is not `not_done` in the window.
  - The probe gets 400 "id missing" (the gate did not block), or 409 with a different item while only S6a is latched.
- **INCONCLUSIVE:** The link went down in the window (`cannot_yet`).
- **NOT_RUN:** OT-B01 did not run, the outcome is not `s6a_latched`, or there is no pre-clear hook.
- **Fake-board test:** positive: readiness `safety_trip` not_done, start returns 409 `{readiness_item:"safety_trip"}`. Negative: start returns 400 "id missing or invalid" in the window and must FAIL. `_otb01.outcome:"no_trip"` must give NOT_RUN.

NEEDS OWNER ids: WEB-SAF-03, WEB-COMM-03, WEB-COMM-06

### 4.5 Setup wizard, settings, display (WEB-WIZ, WEB-SET, WEB-DISP)

#### WEB-WIZ-02 — Resume opens first unfinished step
- **Intent:** Pressing Resume on the wizard overview opens the first step that is not finished.
- **Class:** READ-ONLY — the click is JS-only (`gResume` `setup_wizard_page.html:2990-2997`), so the case judges the inputs `pickResumeStep` uses (`:536-544`).
- **Observable / route:** `GET /api/setup/progress` → `version`, `steps."0".."11".{state,ts,note}` (emitter `setup_progress_http.c:92,104-106`; consumer `setup_wizard_page.html:3002`); `GET /api/readiness` (consumer `:3001`). Tiers: progress ADMIN (`route_tier_table.h:258`), readiness OPEN (`:162`). Static: `id="resumeBtn"` and `onclick="gResume()"` (`:255`), and `function pickResumeStep` present.
- **PASS:** `version==1`; keys 0..11 all present (`SETUP_WIZARD_STEP_COUNT 12`, `setup_wizard_progress.h:76`); every state is in {pending, done, skipped} (`setup_wizard_progress.h:95-110`). The case recomputes `computeStepState` (`:390-453`) and `pickResumeStep` in Python, records the resume id (or null when complete), and the static markers are present.
- **FAIL:** A key is missing, a state is unknown, `version!=1`, or a static marker is absent.
- **INCONCLUSIVE:** None. A null resume id (wizard complete) is still PASS, with the reason recorded.
- **Fake-board test:** positive: steps 0-1 done, 2 pending, so the resume id is 2. Negative: `steps` with only keys 0..10 must give FAIL.

#### WEB-WIZ-03 — Step 1 save (tz, unit_pref) round trip
- **Intent:** Step 1 Save writes the time zone and the temperature unit; the values read back and are then restored.
- **Class:** MUTATING — it writes the same two routes the page's Save posts (`setup_wizard_page.html:1812-1814` tz, `:1817-1819` unit). It never calls `postStepState(1,…)` (`:1823`).
- **Observable / route:** `POST /api/settings/tz` `tz=<POSIX rule>` → `{"ok":true}` (`settings_http.c:76-131`; IANA names get 400 at `:108-115`), tier ADMIN (`route_tier_table.h:244`). `POST /api/unit_pref` `unit=` (`dashboard_http.c:873`), tier ADMIN (`:250`). Read-back: `GET /api/status` `time_tz` (`dashboard_status_http.c:1003`) and `temp_unit` (`:983`); consumer `setup_wizard_page.html:1792,1796`.
- **PASS:** After tz=`UTC0`, `time_tz=="UTC0"`. After the unit is flipped, `temp_unit` shows the flipped value. Both restores read back equal to the originals.
- **FAIL:** A write is not 200 `ok`, or a read-back disagrees. A restore mismatch is an unconditional FAIL (like `judge_web_rw_toggle`).
- **INCONCLUSIVE:** The original `time_tz` is empty or unreadable, because it cannot be restored (the POST would 400). The tz half is then not run.
- **Mutation / gate / restore:** Run only in a mutating suite, and only when `GET /api/profile_exec` `state=="idle"` (OPEN, `route_tier_table.h:161`). Write tz=`UTC0` and unit=the other unit. In `finally`, re-post the original tz and unit and verify through `/api/status`. The unit half reuses the WEB-DASH-13 pattern (`cases_web_rw.py:224-246`).
- **Fake-board test:** positive: the FakeHttp echoes the posted tz and unit into `/api/status`. Negative: `/api/status` still returns the old `time_tz` after the restore post differs from the original, which must give FAIL.

#### WEB-WIZ-04 — Step 2 live poll shows three channels
- **Intent:** Step 2 polls live readings and shows three thermocouple channels.
- **Class:** READ-ONLY — `step2PollLive` (`setup_wizard_page.html:1889-1899`, every 3000 ms at `:1928`) reads `/api/status` `zones[i].actual_valid/actual_c` (`:1896`). The channel count comes from `/api/zones` `thermo_count` (`renderStep2` `:1901-1981`, `THERMO_COUNT_MAX=3` at `:575`).
- **Observable / route:** `GET /api/zones` → `thermo_count` (`zones_http_get.c:285`), tier ADMIN (`route_tier_table.h:233`). `GET /api/status` → `zones[].actual_c, actual_valid` (`dashboard_json.c:146,187`), tier OPEN (`:160`). Static: `function step2PollLive` and the `livereading` class (`:1863`).
- **PASS:** `thermo_count==3`, and zones 0..2 in `/api/status` each have `actual_valid:true` and a finite `actual_c`. Static markers are present.
- **FAIL:** `thermo_count>3`, a configured zone has `actual_valid:false` or a non-numeric `actual_c`, or a static marker is missing.
- **INCONCLUSIVE:** `thermo_count<3` (the bench is configured with fewer channels), or `/api/status` lists fewer zones because `dashboard_json.c` skips inactive zones (`!z->active`, `:126-128`).
- **Fake-board test:** positive: thermo_count 3 with three valid zones. Negative: zone 1 has `actual_valid:false` while thermo_count is 3, which must give FAIL.

#### WEB-WIZ-05 — Step 3 TC types read back
- **Intent:** Step 3 shows each channel's configured thermocouple type, read back from the board.
- **Class:** READ-ONLY — `renderStep3` (`setup_wizard_page.html:2019-2095`) reads `/api/zones` `zones[i].tc_type`/`cal_offset_c` (`:2001,2037`) and maps the codes through `TC_TYPES` 0-7 (`:565-574`).
- **Observable / route:** `GET /api/zones` → `zones[].tc_type` (`zones_http_get.c:480`), `thermo_count` (`:285`). Tier ADMIN (`route_tier_table.h:233`).
- **PASS:** For every zone below `thermo_count`, `tc_type` is an integer in 0..7 and `cal_offset_c` is finite. The case records the types it saw. The served HTML contains `TC_TYPES`.
- **FAIL:** `tc_type` is missing or outside 0..7 for a configured zone, or `TC_TYPES` is absent from the page.
- **INCONCLUSIVE:** `thermo_count==0`.
- **Fake-board test:** positive: tc_type [1,1,1]. Negative: `tc_type:9` on zone 0 must give FAIL.

#### WEB-WIZ-06 — Steps 4/6 consequence-confirm dialogs
- **Intent:** On steps 4 and 6, changing a value opens a consequence-confirm dialog. Cancel writes nothing.
- **Class:** READ-ONLY (static) — the dialog only exists after JS runs, so this reduces under the WEB-ZONE-14 precedent. Its inputs are `/api/zones` `zone_type`, `max_temp_c`, `max_ramp_c_per_hr` (`setup_wizard_page.html:2117`) and `abs_max_temp_c` read via GET commissioning params (`getAbsMaxTempC` `:873-880`). Nothing is posted.
- **Observable / route:** `GET /api/zones` → `zones[].zone_type` (`zones_http_get.c:523`), tier ADMIN (`route_tier_table.h:233`). Static in the served HTML:
  - `step4ZoneTypeConfirmLines` (`:827-838`) and `step6LimitConfirmLines` (`:913-927`).
  - The `kcConfirmConsequentialChange` call sites (`:2215-2264`, `:2523-2563`).
  - The cancel text `Cancelled -- nothing was written` (`:2257-2261`).
  - The `commissioning_shared.js` include (`:44`). That file's `confirmConsequentialChange` is at `commissioning_shared.js:247-271`, exported at `:276`.
- **PASS:** Every static marker is present, and `zone_type` is an integer for each zone.
- **FAIL:** A marker is missing, for example the cancel path or a confirm function.
- **INCONCLUSIVE:** None.
- **Fake-board test:** positive: served HTML containing all markers. Negative: HTML without `step6LimitConfirmLines` must give FAIL.

#### WEB-WIZ-07 — Step 7 embedded commissioning (alias)
- **Intent:** Step 7 embeds the commissioning commit. The plan defines this as `= WEB-COMM-03`.
- **Class:** OBSERVER (depends_on WEB-COMM-03) — alias with no second write. Driving step 7 on its own is forbidden: its body always includes id 260 `abs_max_temp_c` (`setup_wizard_page.html:2868-2873`) and commits through `kcCommissioningCommitAndVerify` (`:2887`; `commissioning_shared.js:145-155` POST, read-back `:182-206`). Plan §6 rule 4 bans writing abs_max_temp_c.
- **Observable / route:** WEB-COMM-03's stashed verdict in ctx. Static: `function renderStep7` (`:2625`) and the `kcCommissioningCommitAndVerify` call are present in `/setup`. Tier for `/api/safety/commissioning`: ADMIN (`route_tier_table.h:295-296`).
- **PASS:** WEB-COMM-03 passed and the static markers are present.
- **FAIL:** WEB-COMM-03 failed, or a static marker is missing.
- **INCONCLUSIVE:** WEB-COMM-03 was INCONCLUSIVE. The verdict and reason are copied.
- **Fake-board test:** positive: a ctx stash with WEB-COMM-03 PASS. Negative: a stash with FAIL must give FAIL. If there is no stash, the result is NOT_RUN. WEB-COMM-03 currently has no case function, so the runner records `not_implemented` (`runner.py:541`).

#### WEB-WIZ-08 — Step 9 sweep: Start gated by presence checkbox
- **Intent:** The step 9 CT sweep's Start button stays disabled until the presence checkbox is ticked. The sweep is never started (same as WEB-ZONE-10).
- **Class:** READ-ONLY (static plus a fixed INCONCLUSIVE) — this is current id 8, displayed as "9". The plan uses the old 13-step numbering, and the page now has 12 steps (`setup_wizard_progress.h:76`).
- **Observable / route:** Static, in the served script text of `/setup`:
  - `id="step8Ack"` (`setup_wizard_page.html:1137-1138`).
  - `id="step8Start"` with `disabled` (`:1144`), enabled by the ack change listener (`:1151`).
  - The Start handler re-checks ack (`:1191`) before POSTing `/api/zones/current_sweep/start` (`:1194`). The harness never POSTs it.
  - Input: `GET /api/safety/commissioning` `ct_installed` (`:1112`), tier ADMIN (`route_tier_table.h:296`).
- **PASS:** Never on this fixture. Starting the sweep needs heat with the owner present.
- **FAIL:** A marker is missing, the Start markup lacks `disabled`, or the handler has no ack re-check.
- **INCONCLUSIVE:** Always, when the static markers are present: "sweep not started by policy". The same applies when `ct_installed==0`, where the page renders only the skip button (`:1112-1131`).
- **Fake-board test:** positive: HTML containing all markers gives INCONCLUSIVE. Negative: HTML where `step8Start` has no `disabled` must give FAIL.

#### WEB-WIZ-09 — Step 10 save gains (alias)
- **Intent:** Step 10 saves PID gains. The plan defines this as `= WEB-ZONE-05`.
- **Class:** OBSERVER (depends_on WEB-ZONE-05) — alias with no duplicate write. This is current id 9. `renderStep9` (`setup_wizard_page.html:1274-1369`) POSTs `/api/zones/pid` `zone=&kp=&ki=&kd=` (`:1341-1343`) and reads it back (`:1351-1356`). That is WEB-ZONE-05's write.
- **Observable / route:** WEB-ZONE-05's stashed verdict. Static: `function renderStep9` and the `/api/zones/pid` string are present. Tier ADMIN (`route_tier_table.h:234`).
- **PASS:** WEB-ZONE-05 passed and the static markers are present.
- **FAIL:** WEB-ZONE-05 failed, or a marker is missing.
- **INCONCLUSIVE:** WEB-ZONE-05 was INCONCLUSIVE.
- **Note:** The page's save also calls `postStepState(9,'done')` when the values match and kp>0 (`:1357-1359`). The alias must not reproduce that progress write. WIZ-10 covers progress writes.
- **Fake-board test:** positive: a stash with PASS. Negative: a stash with FAIL must give FAIL. With no stash the result is NOT_RUN (WEB-ZONE-05 has no case function yet).

#### WEB-WIZ-10 — Mark-done / skip write progress, then revert
- **Intent:** The wizard's mark-done and skip actions write setup progress. The case reverts by re-posting the saved progress.
- **Class:** MUTATING — writes only `POST /api/setup/progress`. It never touches step 11's Save, which writes `/api/auth/security` credentials (forbidden). It uses the page's own skip payload (`step11Skip` `setup_wizard_page.html:1512-1514`; `postStepState` form `:1704-1712`).
- **Observable / route:** `POST /api/setup/progress` `step=&state=&note=` → `{"ok":true}` (`setup_progress_http.c:137-208`, reply `:207`). It returns 400 for a bad step or state (`:182-193`) and 503 when cfg is unmounted (`:150`). Tier ADMIN (`route_tier_table.h:257`). Read-back is `GET` (`:258`, emitter `:92,104-106`).
- **PASS:** Step 10 state=done reads back done. Step 11 state=skipped, note=`optional, deferred` reads back skipped with the same note. After restore, each step's state and note equal the snapshot.
- **FAIL:** A write is not ok, a read-back disagrees, or a restore mismatch in state or note (unconditional). `ts` cannot be restored: `set_step` stamps seconds since boot (`setup_wizard_progress.c:583-607`, `.h:124`), so `ts` is excluded from the comparison.
- **INCONCLUSIVE:** The POST returns 503 (cfg not mounted).
- **Mutation / gate / restore:** Run only in a mutating suite with `/api/profile_exec` `state=="idle"`. Snapshot the GET first. In `finally`, re-post each touched step's snapshot state and note. An empty note clears, which matches the snapshot when the snapshot note was empty (`setup_wizard_progress.c:583-607`). Verify by GET.
- **Fake-board test:** positive: a FakeHttp that stores the posts. Negative: after the restore post, GET still shows step 11 `skipped` when the snapshot was `pending`, which must give FAIL.

#### WEB-WIZ-11 — Progress persists across sw_reset_esp
- **Intent:** Setup progress survives the ESP software reset (OT-B01).
- **Class:** OBSERVER (depends_on OT-B01) — no second reset. Progress is persisted to a cfg file (`setup_wizard_progress.h:16-26`).
- **Observable / route:** `GET /api/setup/progress` (`setup_progress_http.c:80-135`, tier ADMIN `route_tier_table.h:258`), compared before and after the reset. The reset facts come from `ctx["_otb01"]` (`cases_ota.py:418-428`): `esp_restart_confirmed`, `outcome`.
- **PASS:** The reset is confirmed and, for every step 0..11, the post-reset state and note equal the pre-reset snapshot.
- **FAIL:** The reset is confirmed and any state or note differs. `ts` is ignored, because it is seconds since boot and is expected to change.
- **INCONCLUSIVE:** `esp_restart_confirmed` is false (same rule as SP-04, `cases_safety.py:117-133`), or no pre-reset snapshot exists.
- **NOT_RUN:** `ctx["_otb01"]` is absent.
- **Implementation note:** The pre-reset snapshot needs a hook. OT-B01 should stash `ctx["_otb01"]["setup_progress_before"]` (a GET just before `ota.sw_reset`, `cases_ota.py:388`). Without that hook the result is INCONCLUSIVE, never PASS.
- **Fake-board test:** positive: before and after match apart from `ts`. Negative: step 3 is `done` before and `pending` after, with the reset confirmed, which must give FAIL.

#### WEB-SET-02 — cfgfs format banner hidden, never pressed
- **Intent:** The cfg filesystem format banner is hidden (no format pending). Format is never pressed.
- **Class:** READ-ONLY — the banner is revealed only by JS when `data.pending` is true (`settings_page.html:281-286`). Plan wording `format_pending:false` maps to the route's key `pending` (`cfg_fs_format_http.c:32`). `/api/status` emits `cfg_fs_format_pending:true` only when pending (`dashboard_status_http.c:1057-1058`).
- **Observable / route:** `GET /api/cfgfs/format_pending` → `pending`, `reason` (`cfg_fs_format_http.c:22-40`, registered at `:124`). Tier ADMIN (`route_tier_table.h:388`). Static: `id="cfgFsFormatBanner"` carries `hidden` (`settings_page.html:233`), and `cfgFsFormatConfirmBtn` exists (`:239`).
- **PASS:** `pending:false`, and the static banner is `hidden`.
- **FAIL:** The `pending` key is missing or not a boolean, the static banner lacks `hidden`, or `/api/status` has `cfg_fs_format_pending:true` while the route says false.
- **INCONCLUSIVE:** `pending:true` is a real board state. Record `reason`, as FL-07 does with `judge_cfgfs_state` (`judgments.py:105-118`). The case never POSTs `/api/cfgfs/format_confirm` (`route_tier_table.h:366`).
- **Fake-board test:** positive: `{"pending":false,"reason":""}`. Negative: HTML where the banner has no `hidden` attribute, with pending false, must give FAIL.

#### WEB-SET-03 — Danger-zone reset buttons render, none pressed
- **Intent:** The factory-reset buttons in the danger zone render. None are pressed.
- **Class:** READ-ONLY (static) — `factory_reset` must never be called (brief hard rule). The handler wiring is checked as text only.
- **Observable / route:** Served `/settings` (tier ADMIN, `route_tier_table.h:443`) contains:
  - `id="danger"` (`settings_page.html:202`).
  - Four `danger-btn` buttons with `data-scope` wifi, kiln, profiles, and all (`:220-223`), plus `id="resetStatus"` (`:225`).
  - Handler text: `kcConfirm` before the POST to `/api/factory_reset` (`:335-358`). That route is ADMIN (`route_tier_table.h:365`) and registered at `factory_reset.c:586`. The harness never POSTs it.
- **PASS:** All four scopes, `#resetStatus`, and the confirm-guarded handler are present.
- **FAIL:** A scope button is missing, or the handler POSTs without a preceding `kcConfirm`.
- **INCONCLUSIVE:** None.
- **Fake-board test:** positive: real page HTML. Negative: HTML with `data-scope="profiles"` removed must give FAIL.

#### WEB-SET-04 — Reboot both processors (sw_reset) = OT-B01 (alias)
- **Intent:** The Reboot-both-processors button (`/api/sw_reset`) is OT-B01, exercised through the page in one run of the two.
- **Class:** OBSERVER (depends_on OT-B01) — alias, not NEEDS OWNER. The harness cannot click (no JS). The page's handler sends the same request as OT-B01's helper:
  - Page: `kcConfirm`, then `kcOtaAuthedFetch('/api/sw_reset',{method:'POST'})` (`settings_page.html:313-333`).
  - `kcOtaAuthedFetch` is a plain fetch that retries only on 428 (`app.js:1271-1324`).
  - OT-B01: `ota_http_client.sw_reset` POSTs an empty body to the same route (`ota_http_client.py:722-800`), called by `_case_otb01` (`cases_ota.py:342,388`).
- **Observable / route:** `ctx["_otb01"]` (`cases_ota.py:418-428`). Static: `id="swResetBtn"` (`settings_page.html:198`), `#swResetStatus` (`:200`), and the handler string. Tier ADMIN (`route_tier_table.h:367`), registered at `sw_reset_http.c:581`.
- **PASS:** OT-B01's outcome is a PASS outcome (no trip, or an S6a trip cleared) and the static markers are present.
- **FAIL:** OT-B01's outcome is FAIL (another trip), or a static marker or the handler is missing.
- **INCONCLUSIVE:** `esp_restart_confirmed` is false (the SP-04 template, `cases_safety.py:117-133`).
- **NOT_RUN:** `ctx["_otb01"]` is absent. There is never a second reset.
- **Fake-board test:** positive: a stash with a no-trip outcome, restart confirmed, and real HTML. Negative: a stash with outcome (c), another trip, must give FAIL.

#### WEB-DISP-02 — display_power round trip; window for LCD-05/LCD-06
- **Intent:** GET/POST the display power settings. At brightness 50, LCD-05 samples a darker panel. At timeout "1 min", LCD-06 confirms blanking. Then restore.
- **Class:** MUTATING — this case owns the window. LCD-05 and LCD-06 have no case functions today (map at `cases_lcd.py:5771-5785`; the runner records `not_implemented`, `runner.py:541`), so nothing currently sets their brightness or timeout. Because the plan requires restore in the same case's finally (§5.3), WEB-DISP-02 should run the LCD sampling hooks inside its own window. Mechanism:
  - It calls `ctx["_lcd05_sample"]` and `ctx["_lcd06_sample"]` if they are present.
  - It stashes `ctx["_disp02"] = {brightness_window_ok, timeout_window_ok, samples…}`.
  - LCD-05 and LCD-06 then become observers (depends_on WEB-DISP-02) that return NOT_RUN without that stash, following the `_case_lcd02` / `ctx["_hp01"]` precedent (`cases_lcd.py:1440`).
- **Observable / route:**
  - `GET /api/settings/display_power` → `brightness_percent, timeout_setting, keep_on_while_firing, display_on_error, brightness_inert` (`settings_http.c:151-157`). Consumer `settings_display_page.html:154-170`. Tier ADMIN (`route_tier_table.h:246`).
  - `POST` takes all four fields: brightness 0-100 (`:230`), timeout 0..5 where 0 = 1 min and 5 = Never (`:234`), applied all-or-none (`:239`). Tier ADMIN (`:245`). Page payload: `:184-191`.
  - No PcTools helper exists, so the case uses `_get_json` / `_post_json`.
- **PASS:** The brightness=50 POST is ok and reads back 50 with the other three fields unchanged. The timeout=0 POST reads back 0. The restore of all four fields reads back equal to the snapshot.
- **FAIL:** A POST is not ok, a read-back mismatches, or a restore mismatch (unconditional).
- **INCONCLUSIVE:** None for the API half. When `brightness_inert:true`, the LCD-05 half is recorded as INCONCLUSIVE in the stash.
- **Mutation / gate / restore:**
  - Gate: mutating suite and `/api/profile_exec` `state=="idle"`.
  - Phase A: post `brightness=50` with the snapshot's other fields, then run the LCD-05 hook.
  - Phase B: post `timeout=0`, wait 70 s, run the LCD-06 hook (blank, then wake with touch_inject; `_wake_and_home` at `cases_lcd.py:115-140`).
  - `finally`: post the snapshot's four values and verify by GET. Restore the original value, not the plan's literal "Never".
  - The page's slider minimum is 10 (`settings_display_page.html:108`), so 50 is reachable from the page.
- **Fake-board test:** positive: a FakeHttp that stores the posts. Negative: after the restore, GET returns `timeout_setting:0` when the snapshot was 5, which must give FAIL.

#### WEB-DISP-03 — brightness_inert hint matches build flag
- **Intent:** The `brightness_inert` hint on the display page matches the build flag.
- **Class:** READ-ONLY — `brightness_inert` is `false` when `CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE` is set, otherwise `true` (`settings_http.c:143-147`). The Kconfig default is `y` (`Kconfig:438-439`), and neither sdkconfig.defaults nor sdkconfig.paneldetect.defaults overrides it, so the expected value is `false`. The JS picks the hint text from this field (`settings_display_page.html:166-170`). The static markup default is the "Inert…" text (`:113-115`), which JS overwrites.
- **Observable / route:** `GET /api/settings/display_power` → `brightness_inert` (`settings_http.c:151-157`). Tier ADMIN (`route_tier_table.h:246`). Static: `id="kcDpBrightnessNote"` and the `d.brightness_inert ?` branch containing `Applies live to the panel backlight.`
- **PASS:** `brightness_inert` is boolean and equals `not PWM_ENABLE` for the board's build (expected `false`), and the static markers are present.
- **FAIL:** The value is non-boolean or disagrees with the expected flag on a board whose firmware matches the worktree, or a marker or the branch is missing.
- **INCONCLUSIVE:** The board's firmware commit differs from the worktree HEAD, so the build flag cannot be inferred. No route exposes the flag.
- **Fake-board test:** positive: `brightness_inert:false` with a matching commit. Negative: `brightness_inert:true` with a matching commit must give FAIL.

#### WEB-DISP-04 — Theme toggle is browser-local
- **Intent:** The dark/light theme toggle is stored in the browser only (DOM only, no board write).
- **Class:** READ-ONLY (static) — there is no route. The toggle writes only `localStorage 'kilnctl-theme'` (`settings_display_page.html:210-230`), and the head script applies it (`:8`).
- **Observable / route:** Served `/settings/display` (tier ADMIN, `route_tier_table.h:445`) contains `id="themeBtn"` (`:89`) and `id="kcDisplayPrefs"` (`:92`). The theme script contains `localStorage.setItem('kilnctl-theme'` and contains no `fetch(`.
- **PASS:** All markers are present, and the theme script block has no `fetch(` or `/api/`.
- **FAIL:** `#themeBtn` or the localStorage write is missing, or the theme script contains a `fetch('/api/…')`.
- **INCONCLUSIVE:** None.
- **Fake-board test:** positive: real page HTML. Negative: a theme script with `fetch('/api/theme',{method:'POST'})` added must give FAIL.

NEEDS OWNER ids: none

### 4.6 Diagnostics and firmware update (WEB-DIAG, WEB-OTA)

#### WEB-DIAG-02 — Crash card reads "No crash recorded"; Ack/Clear never pressed
- **Intent:** On a healthy board the crash card reads "No crash recorded". The Acknowledge and Clear controls exist but are never pressed.
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/crash_report` → `present` (emitter `diagnostics_http.c:403` for `{"present":false,...}`, `:428` for the present branch).
  - Consumer: `diagnostics_page.html:943-944` (`!cr || !cr.present` gives 'No crash recorded.').
  - Tier ADMIN (`route_tier_table.h:397`).
  - Static check on `/diagnostics`: `id="crashCard"` (`diagnostics_page.html:289`), plus the `crashAckBtn`/`crashClearBtn` strings in the page JS (`:992-994`).
- **PASS:** All of these hold:
  - HTTP 200;
  - `present` is the bool `false`;
  - all three strings are in the served HTML.
- **FAIL:** Any of these:
  - not 200;
  - `present` missing or not a bool;
  - any of the three strings missing.
- **INCONCLUSIVE:** `present:true`. A real crash on record is board state, and the case may not clear it (rule 4). Preflight already refuses on an unacknowledged crash.
- **Fake-board test:**
  - Positive: `{"present":false,"clear_in_progress":false}` plus HTML with the three strings → PASS. Also assert that no POST went to `/api/crash_report/ack` or `/clear`.
  - Negative: `{"clear_in_progress":false}` with no `present` key → FAIL.

#### WEB-DIAG-03 — Partitions table equals FL-01
- **Intent:** The page's partitions table matches what FL-01 verified.
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/partitions` → `running`, `partitions[].{label,type,subtype,offset,size,encrypted}` (emitter `partition_info_http.c:98` for the envelope, `:56` for each entry).
  - Consumer: `diagnostics_page.html:1045-1085` (`renderPartitions`, `resp.running` at `:1066`).
  - Tier ADMIN (`route_tier_table.h:395`).
  - PcTools helper: `partition_http_client.get_partitions` (`partition_http_client.py:140`).
- **Reuse:** FL-01 is `cases_smoke.py:141`, not `cases_fl.py`. It stashes nothing today. NEW: FL-01 stores its parsed list in `ctx["_fl01_partitions"]`. DIAG-03 compares `(label, type, subtype, offset, size)` tuples against it. With no stash, DIAG-03 compares against `partitions.csv` itself, through the same parser FL-01 uses (`judge_partition_table_match`, `judgments.py:33`).
- **PASS:** All of these hold:
  - HTTP 200;
  - `partitions` is a non-empty list;
  - every entry has all six keys;
  - the tuple set equals the reference;
  - `running` names a listed app label.
- **FAIL:** Any of these:
  - an offset, size, type or subtype differs;
  - a label is extra or missing;
  - `running` is not listed;
  - the response is not 200 or not a list.
- **INCONCLUSIVE:** No stash, and `partitions.csv` cannot be located. That is an environment problem, not the board.
- **Fake-board test:**
  - Positive: a route response equal to the stash → PASS.
  - Negative: the stash has `app` at `0x220000`, the route reports `0x210000` → FAIL.

#### WEB-DIAG-04 — cfgfs card equals FL-07
- **Intent:** The config-filesystem card shows the same state FL-07 recorded.
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/cfgfs` → `mounted`, `status`, `capacity.{known,used_bytes,total_bytes,free_bytes}`, `file_count`, `dual_write.items[]` (emitter `cfg_fs_status.c:173-245`; handler `diagnostics_http.c:1508`).
  - Consumer: `diagnostics_page.html:1126-1220`, guarded by `typeof j.mounted === 'boolean'` at `:1220`.
  - Tier ADMIN (`route_tier_table.h:387`).
  - PcTools helper: `dashboard_http_client.get_cfgfs_status` (`:160`).
- **Reuse:** FL-07 is `cases_smoke.py:201`. NEW: it stores its dict in `ctx["_fl07_cfgfs"]`. DIAG-04 compares only the stable keys: `mounted`, `status`, `capacity.total_bytes`, `file_count`, and the `dual_write.items` names. It never compares `used_bytes`, `free_bytes` or `tmp_entries_now`. With no stash, the route is judged alone.
- **PASS:** All of these hold:
  - HTTP 200;
  - `mounted` is the bool `true`;
  - `capacity.known` is true, with used + free ≤ total;
  - `dual_write.items` is a list;
  - the stable keys equal the stash.
- **FAIL:** Any of these:
  - `mounted` missing or not a bool;
  - a stable key differs from the stash;
  - inconsistent capacity;
  - not 200.
- **INCONCLUSIVE:** `mounted:false`. This is board state, and formatting is forbidden.
- **Finding (separate fix):** `judge_cfgfs_state` (`judgments.py:105`) reads `format_pending`/`pending`, and `/api/cfgfs` never emits either key. `pending` exists only on `GET /api/cfgfs/format_pending` (`cfg_fs_format_http.c:32`). As written, FL-07 can never judge "pending".
- **Fake-board test:**
  - Positive: a stash equal to the route response → PASS.
  - Negative: route `file_count:7`, stash `file_count:9` → FAIL.

#### WEB-DIAG-05 — Thermocouple faults: three channels ok, safety TC ok
- **Intent:** All three channels read `ok`, and the safety-processor TC card reads `ok`.
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/thermo/faults` → `channel_count`, `channels[].{channel,state,fault_status,stale}`, `safety.{state,link_up,tc_is_separate_sensor}`. Emitter `diagnostics_http.c`: `:187` envelope, `:201-209` states, `:226` each channel, `:298` the no_link branch, `:326-334` the safety object.
  - Consumer: `diagnostics_page.html:1352-1418`, where `state==='ok' && fault_status!==0` means faulted (`:1374`).
  - Safety card: `:1440-1493`.
  - Tier ADMIN (`route_tier_table.h:308`).
- **PASS:** All of these hold:
  - HTTP 200;
  - `channel_count == 3`;
  - each channel has `state=="ok"`, `fault_status==0` and `stale==false`;
  - `safety.state=="ok"`.
- **FAIL:** Any of these:
  - a channel reads `timeout`, `absent` or `spi_failed`;
  - a channel reads `ok` with a non-zero `fault_status`;
  - `channel_count` is not 3;
  - `safety.state` is `faulted`, `probe_fault` or `not_converting`;
  - a 500 or a malformed body.

  The page has no `probe_fault` branch and renders it as ok. Record that as a page finding in `observed`.
- **INCONCLUSIVE:** `safety.state=="no_link"`. The channels are still judged; if one of them fails, the verdict is FAIL.
- **Fake-board test:**
  - Positive: three channels `{"state":"ok","fault_status":0,"stale":false}` plus `"safety":{"state":"ok","link_up":true}` → PASS.
  - Negative: channel 2 `"state":"spi_failed"` → FAIL.

#### WEB-DIAG-06 — Relay wear rows include safety K4; Reset never pressed
- **Intent:** Every relay row, including "Safety (K4)", is listed. Reset count is never pressed.
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/status` → `relay_life[].{relay,type,cycles,rated,percent,tier}` (emitter `dashboard_status_http.c:403-438`).
  - Count is `RELAY_CYCLES_COUNT` = `KILN_IO_RELAY_COUNT+1` (`relay_cycles.h:51`).
  - Consumer: `diagnostics_page.html:733-775`. `r.relay===4` gives 'Relay 5 (safety, K4)' (`:739`), and `relay-reset-btn` is built at `:749`.
  - Tier OPEN (`route_tier_table.h:160`).
  - Static: `relayWearCard` (`:348`).
- **PASS:** All of these hold:
  - `relay_life` has 5 entries, indices 0..4;
  - `cycles` is an int ≥ 0;
  - `tier` is one of `none`, `warn`, `error`;
  - `rated`/`percent` are null exactly when `type=="ssr"`;
  - the served HTML has `relayWearCard` and `relay-reset-btn`.

  The judge keys on `relay==4`, not on the label text.
- **FAIL:** Any of these:
  - index 4 missing, or the wrong entry count;
  - a bad `tier` or `type`;
  - a non-null `rated` on an SSR;
  - a static string missing.
- **INCONCLUSIVE:** `io_ready` false (`dashboard_status_http.c:361`).
- **Fake-board test:**
  - Positive: five entries with K4 `{"relay":4,"type":"contactor","cycles":12,"rated":100000,"percent":0,"tier":"none"}` → PASS. Also assert that no POST went to `/api/relay_cycles/reset`.
  - Negative: entries 0..3 only → FAIL.

#### WEB-DIAG-09 — Danger Mode
- **Intent:** Ticking accept enables Enter. `/danger/start` opens the live view, R0 is toggled on then off through `/danger/relay` (confirmed by `io_read`), the Firing mode tile is untouched, `/danger/stop` exits, and the 5-minute auto-exit is checked.
- **Class:** NEEDS OWNER, because the full intent energizes a relay. The default is the read-only reduction below.
- **Observable / route:**
  - `GET /api/diagnostics/danger` → `active`, `remaining_ms` (emitter `diagnostics_http.c:1166`; tier ADMIN `route_tier_table.h:351`).
  - Start: `:1184`. It answers 400 without `accept=1` (`:1203`) and 409 during a firing or pause (`:1208-1214`).
  - Relay: `:1246`. It answers 409 when Danger Mode is not active (`:1248-1251`).
  - Stop: `:1227`.
  - Page: `dangerAccept` `diagnostics_page.html:532`, `dangerEnterBtn` with `disabled` `:533`, `dangerLive` `:535`, `dangerExitBtn` `:543`. The accept checkbox enables Enter at `:1784-1786`.
- **PASS (read-only reduction):** All of these hold:
  - the four ids are present, and `dangerEnterBtn` carries `disabled` in the served HTML;
  - `GET /api/diagnostics/danger` gives `active:false`;
  - the refusal probe `POST /api/diagnostics/danger/relay` `relay=1&on=0` answers 409.

  The probe is a non-energizing write that the firmware rejects while inactive. It is allowed under the standard mutating gate.
- **FAIL:** Any of these:
  - an id is missing;
  - Enter is not disabled;
  - `active` is not a bool;
  - the refusal probe answers 200.
- **INCONCLUSIVE:** `active:true` at case start, meaning someone else's session. Do not touch it.
- **Mutation / gate / restore:** This applies only if the owner opts in to the full toggle.
  - Gate: the standard gate, plus `GET /api/ota/interlock` `ok`, plus a new explicit opt-in such as `ctx["allow_danger_relay"]`.
  - Writes, in order:
    1. `start accept=1`.
    2. `relay=1&on=1`, then confirm through `/api/status` `relays[0].on` and `io_read`.
    3. `relay=1&on=0`.
  - Never POST `/danger/enable` (`diagnostics_http.c:1338`). That is the Firing mode tile.
  - `finally`: `relay=1&on=0`, then `/danger/stop`, then read back `active:false` and `relays[0].on==false`. A mismatch is an unconditional FAIL.
  - Auto-exit: INCONCLUSIVE unless the time budget allows a second idle entry plus 5 minutes.
- **Fake-board test:**
  - Positive: GET `active:false`, relay POST answers 409, and the ids are present → PASS.
  - Negative: the relay POST answers `200 {"ok":true}` while inactive → FAIL.
- **NEEDS OWNER:** May the bench energize R0 through Danger Mode at all? Recommended: run the read-only reduction plus the 409 probe by default. Allow the full toggle only behind an explicit opt-in flag, and never during a firing.

#### WEB-DIAG-10 — lwip_stats, timing and board_temps are well-formed
- **Intent:** All three debug routes return well-formed JSON.
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/debug/lwip_stats` → `ok`, `tcp.{xmit,recv,drop,chkerr,lenerr,memerr,rterr,proterr,opterr,err}` (emitter `diagnostics_http.c:703`). It answers 501 `{"ok":false,"error":"CONFIG_LWIP_STATS not built"}` at `:714-719`. Tier ADMIN (`:403`).
  - `GET /api/diagnostics/timing` → `display_flush_us`, `thermo_read_us`, `link_reply_us`, each `{count,last,min,max,mean}`, plus `link_reply_us.timeouts` (emitter `:1130-1136`). Tier ADMIN (`:404`).
  - `GET /api/board_temps` → `esp32_c`, `thermo_cj_c[]` (emitter `board_temps_http.c:40,46`). Tier ADMIN (`:313`).
- **PASS:** All of these hold:
  - timing has all three objects with int fields, `min ≤ mean ≤ max` whenever `count>0`, and an int `timeouts`;
  - in board_temps, `esp32_c` is a number or null, and `thermo_cj_c` is a list of numbers or nulls;
  - lwip_stats is either a 200 with `ok:true` and all ten tcp ints, or the documented 501 not-built body. Record which one in `observed`.
- **FAIL:** Any of these:
  - a missing key or a non-numeric field;
  - `min>max`;
  - a 500 from any of the three;
  - a lwip 200 without `tcp`.
- **INCONCLUSIVE:** none.
- **Fake-board test:**
  - Positive: full timing JSON, board_temps `{"esp32_c":41.5,"thermo_cj_c":[24.1,24.3,24.0]}`, and lwip 501 not-built → PASS.
  - Negative: timing without `link_reply_us` → FAIL.

#### WEB-DIAG-11 — Stale banner when polling is blocked
- **Intent:** The stale banner appears when the page's polls fail.
- **Class:** NEEDS OWNER. Proving this needs JS execution and a blocked network, and the harness has neither.
- **Observable / route:**
  - Static on `/diagnostics`: `id="staleBanner"` and `staleAge` (`diagnostics_page.html:278`).
  - JS: `function setStale` (`:920-928`), and `failCount` (`:1552-1557`, `:1570`).
- **PASS (recommended):** All four strings are present.
- **FAIL:** Any of the four is missing.
- **INCONCLUSIVE:** none.
- **Fake-board test:**
  - Positive: HTML with all four → PASS.
  - Negative: HTML without `staleBanner` → FAIL.
- **NEEDS OWNER:** Is a static-presence check acceptable, or should the harness gain a headless browser? Recommended: the static check now, revisited if a browser backend is added.

#### WEB-OTA-02 — Interlock reads "Blocked" while HP-01 runs
- **Intent:** While HP-01 fires, the interlock box reads "Blocked: <reason>" and the pickers are hidden.
- **Class:** OBSERVER (depends_on HP-01, window `hp01_running`)
- **Observable / route:**
  - `GET /api/ota/interlock` → `ok`, `reason`, `needs_ack` (emitter `ota_http_recovery.c:497` for the ok branch, `:499` for `{"ok":false,"reason":..,"needs_ack":..}`).
  - Consumer: `ota_page.html:306-335`. `allowed = s.ok || s.needs_ack===true` (`:316`), `setPickersVisible(allowed)` (`:327`).
  - Tier ADMIN (`route_tier_table.h:384`).
- **Probe:** a single `GET /api/ota/interlock`.
- **PASS:** The probe result has `ok:false`, `needs_ack` not true, and a non-empty `reason`.
- **FAIL:** Any of these:
  - `ok:true` or `needs_ack:true`, which would leave the pickers visible during a firing;
  - an empty reason;
  - a malformed body.
- **INCONCLUSIVE:** HP-01 ran, but the `hp01_running` window never fired.
- **Registry change:** set `depends_on="HP-01"`. Move WEB-OTA-02 after HP-01 in `_NIGHTLY_ORDER`; today it sits before HP-01.
- **Fake-board test:**
  - Positive: probe result `{"ok":false,"reason":"profile running","needs_ack":false}` → PASS.
  - Negative: probe result `{"ok":true}` → FAIL.

#### WEB-OTA-03 — ESP update through the page file input (OT-E01 via the DOM)
- **Intent:** Push an ESP image through `espFile`/`espUpdateBtn` (`pushImage`). This is OT-E01 driven through the page.
- **Class:** NEEDS OWNER. It is an OTA push, which this spec excludes, and it needs JS execution.
- **Observable / route:**
  - Static: `espPicker` `ota_page.html:140`, `espFile` `:142`, `espUpdateBtn` `:144`, `function pushImage` `:261`, `pushImage('/api/ota/esp')` `:485-490`.
  - Route `POST /api/ota/esp` (tier ADMIN `route_tier_table.h:358`). This case never calls it.
- **PASS (recommended):** The static strings are present, and OT-E01 PASSed in this run (ALIAS per rule 7).
- **FAIL:** A static string is missing, or OT-E01 FAILed.
- **INCONCLUSIVE / NOT_RUN:** NOT_RUN when OT-E01 did not run.
- **Fake-board test:**
  - Positive: the strings are present and OT-E01 PASSed → PASS.
  - Negative: HTML without `espUpdateBtn` → FAIL.
- **NEEDS OWNER:** Should the DOM path be tested separately from OT-E01? Recommended: alias plus static check, and never push from this case.

#### WEB-OTA-04 — Rollback button (OT-E02 via the DOM)
- **Intent:** The page's rollback button performs OT-E02.
- **Class:** NEEDS OWNER, for the same reasons as WEB-OTA-03.
- **Observable / route:**
  - Static: `espRollbackBtn` `ota_page.html:145`, plus the handler string `/api/ota/esp/rollback` (`:501-506`).
  - Tier ADMIN (`route_tier_table.h:359`). This case never POSTs it.
- **PASS (recommended):** The static strings are present, and OT-E02 PASSed (ALIAS).
- **FAIL:** A string is missing, or OT-E02 FAILed.
- **INCONCLUSIVE / NOT_RUN:** NOT_RUN when OT-E02 did not run.
- **Fake-board test:**
  - Positive: the strings are present and OT-E02 PASSed → PASS.
  - Negative: `espRollbackBtn` missing → FAIL.
- **NEEDS OWNER:** Same question as WEB-OTA-03. Recommended: alias plus static check.

#### WEB-OTA-05 — Recovery-exit box hidden when not in recovery
- **Intent:** The recovery-exit box is hidden unless the ESP is in recovery mode.
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/ota/esp/status` → `recovery_mode` (emitter `ota_http_esp.c:657,676`).
  - Consumer: `ota_page.html:395`.
  - Tier OPEN (`route_tier_table.h:421`).
  - Static: `recoveryExitBox` with `display:none` (`ota_page.html:153`), and `recoveryExitBtn` (`:157`).
- **PASS:** `recovery_mode` is the bool `false`, and the box is in the served HTML with `display:none`.
- **FAIL:** `recovery_mode` is missing or not a bool, or the box is missing or not hidden by default.
- **INCONCLUSIVE:** `recovery_mode:true`. That is the OT-E11 branch. Never press `recoveryExitBtn`.
- **Fake-board test:**
  - Positive: `{"recovery_mode":false}` plus the hidden box → PASS.
  - Negative: `{"recovery_mode":"no"}` → FAIL.

#### WEB-OTA-06 — Pico picker present; update/rollback never pressed
- **Intent:** The Pico picker exists. Update and rollback are not pressed while OT-P is blocked.
- **Class:** READ-ONLY
- **Observable / route:**
  - Static on `/ota`: `picoInfo` `ota_page.html:165`, `picoPicker` `:167`, `picoFile` `:169`, `picoUpdateBtn` `:171`, `picoRollbackBtn` `:172`.
  - Never POST `/api/ota/pico` or `/api/ota/pico/rollback` (`route_tier_table.h:363-364`).
- **PASS:** All five ids are present, and no POST is made.
- **FAIL:** Any id is missing.
- **INCONCLUSIVE:** none.
- **Fake-board test:**
  - Positive: all five ids are present, and FakeHttp.post was never called → PASS.
  - Negative: `picoPicker` missing → FAIL.

#### WEB-OTA-07 — Boot-button bypass banner absent
- **Intent:** The boot-button bypass banner is not shown.
- **Class:** READ-ONLY. This is a regression guard: the banner and its field were retired 2026-09-29 (`ota_page.html:685-687`; `dashboard_status_http.c:1012-1018` no longer emits `boot_button_bypass_active`).
- **Observable / route:** Served `/ota` HTML, plus `GET /api/status` (tier OPEN).
- **PASS:** No bypass banner element in the HTML, and `boot_button_bypass_active` is absent from `/api/status` or false.
- **FAIL:** A banner element is present, or `boot_button_bypass_active:true`.
- **INCONCLUSIVE:** none.
- **Fake-board test:**
  - Positive: no element and no key → PASS.
  - Negative: `{"boot_button_bypass_active":true}` → FAIL.

#### WEB-OTA-08 — Pico protocol compatibility reads "yes"
- **Intent:** The Pico block reports "Compatible with this ESP: yes".
- **Class:** READ-ONLY
- **Observable / route:**
  - `GET /api/ota/pico/status` → `protocol_version_known`, `protocol_version`, `protocol_min_compatible`, `protocol_compatible` (emitter `ota_http_pico.c:707-708`; unknown branch `:715`).
  - Consumer: `ota_page.html:441-443`.
  - Tier ADMIN (`route_tier_table.h:385`).
- **PASS:** `protocol_version_known:true` and `protocol_compatible:true`.
- **FAIL:** Any of these:
  - `protocol_version_known:true` with `protocol_compatible` false or not a bool;
  - a malformed body;
  - not 200.
- **INCONCLUSIVE:** `protocol_version_known:false`.
- **Fake-board test:**
  - Positive: `{"protocol_version_known":true,"protocol_version":3,"protocol_min_compatible":3,"protocol_compatible":true}` → PASS.
  - Negative: the same with `"protocol_compatible":false` → FAIL.

### 4.7 Wi-Fi, security, backup, kiln configs, login, cross-page (WEB-WIFI, WEB-SEC, WEB-BAK, WEB-KCFG, WEB-LOG, WEB-X)

#### WEB-WIFI-02 — Saved bench SSID appears in the scan list
- **Intent:** The scan (GET /networks) lists the bench SSID with the Saved badge.
- **Class:** READ-ONLY — one GET. There is no POST, but the GET triggers a radio scan (see INCONCLUSIVE).
- **Observable / route:** `GET /networks` → JSON list of `{ssid,saved,in_range,rssi,secure,connected}`. Saved and in range is emitted at `wifi_provision_http.c:515-518`, saved and out of range at `:521`, unsaved at `:548`; the handler is at `:445` and calls `wifi_prov_scan` at `:453`. Consumer: `wifi_provision_page.html:1031-1034` draws the 'Saved' badge from `net.saved`; the fetch is at `:1071-1072`. Tier WIFI_SETUP: ADMIN once provisioned (`route_tier_table.h:171`, `:88-101`), so use the authed `_get_json`. Bench SSID comes from `GET /status` → `ssid` (`wifi_provision_http.c:269-271`, `:371-390`; OPEN at `route_tier_table.h:167`).
- **PASS:** /status `mode=="home"` and `sta_connected` true. /networks contains exactly one entry whose ssid equals /status `ssid`, with `saved:true`, `in_range:true` and `connected:true`. A /status read after the scan still shows `sta_connected` true and the same ssid.
- **FAIL:** The bench SSID is missing, or `saved` is not true, or `connected` is not true while /status says connected. Also FAIL on a non-200 from /networks, or a body that is not a list.
- **INCONCLUSIVE:** Before the call, /status `mode!="home"`, `sta_connected` is false, or `ssid` is redacted/empty. After the call, the station dropped (`sta_connected` false) — the scan disturbs the link (same reason the WEB-X-03 sweep excludes /networks, cases_web.py `_SIDE_EFFECT_EXCLUDE`). Record that as an observation, not a FAIL.
- **Fake-board test:** positive: /status `{"mode":"home","sta_connected":true,"ssid":"Bench"}` and /networks `[{"ssid":"Bench","saved":true,"in_range":true,"connected":true}]` give PASS. negative: /networks `[{"ssid":"Bench","saved":false,"in_range":true,"connected":true}]` must give FAIL.

#### WEB-WIFI-03 — AP QR canvas present, AP section hidden in home mode
- **Intent:** The QR canvas renders; the AP section stays hidden in home mode. Only DOM presence is checked.
- **Class:** READ-ONLY — static HTML plus one /status read.
- **Observable / route:** `GET /wifi` raw HTML (OPEN shell, `route_tier_table.h:520`) must contain:
  - `id="apSection"` (`wifi_provision_page.html:197`)
  - the CSS rule `#apSection { display: none; }` (`:121`)
  - `id="apQrCanvas"` (`:199-200`)
  - `var KilnQr` (`:237`)
  - `function renderApQr(` (`:662`)
  - the guard `if (s.mode === 'ap') renderApQr` (`:922-923`)

  Input: `GET /status` → `mode` (emitter `wifi_provision_http.c:191-212`, `:371-390`; consumer `wifi_provision_page.html:894`, `:922`).
- **PASS:** All six markers are present and /status `mode=="home"`, so the JS never draws the QR and the section stays hidden.
- **FAIL:** Any marker is missing, or the `#apSection` display:none rule is gone.
- **INCONCLUSIVE:** /status `mode=="ap"`. That is operator territory (WEB-WIFI-06); report it and do not judge visibility.
- **Fake-board test:** positive: served HTML with all markers and /status `{"mode":"home"}` gives PASS. negative: HTML missing `id="apQrCanvas"` must give FAIL.

#### WEB-WIFI-04 — Static-IP toggle reveals fields (never submit)
- **Intent:** Clicking the IP-mode toggle to Static shows the static fields. Nothing is submitted.
- **Class:** READ-ONLY — static HTML plus /status. **Hazard:** the DHCP button POSTs `/ip_config` on click (`submitIpConfig('dhcp')`, `wifi_provision_page.html:845`, which reaches `fetch('/ip_config')` at `:796`). A future browser-driven harness must never click it.
- **Observable / route:** `GET /wifi` HTML must contain:
  - `ipModeDhcpBtn` and `ipModeStaticBtn` (`:175-176`)
  - `staticIpFields` with `style="display:none"` (`:181`)
  - the `applyIpModeUi` function (`:767-773`)
  - the Static click handler (`:852-860`, "does not itself submit"). Its body must contain no `fetch(`.

  Input: `GET /status` → `ip_mode` (emitter `wifi_provision_http.c:215-218`, `:371-390`; consumer `wifi_provision_page.html:925`).
- **PASS:** All markers are present, the Static-handler span (`:852-860`) contains no `fetch(`, and /status `ip_mode` is in {"dhcp","static"}.
- **FAIL:** A marker is missing, the Static handler contains `fetch(`/`submitIpConfig(`, or `ip_mode` is missing or outside the enum.
- **INCONCLUSIVE:** /status is unreachable or not JSON.
- **Fake-board test:** positive: HTML with the markers and `{"ip_mode":"dhcp"}` gives PASS. negative: `{"ip_mode":"weird"}` must give FAIL.

#### WEB-WIFI-05 — /provision, /forget, /ip_config are never called
- **Intent:** The harness never calls the Wi-Fi provisioning writers.
- **Class:** READ-ONLY — a static check of the harness source plus a runtime deny-list guard. No board writes.
- **Observable / route:** These are the routes that must never be called: POST `/provision`, `/forget`, `/ip_config` (ADMIN, `route_tier_table.h:424-426`; registered at `wifi_provision_http.c:1275`, `:1281`, `:1286`). The check has three parts:
  - (a) Source scan of `tools/PcTools/src/kilnctrl/bench_test/*.py`, the same way WEB-X-03 reads route_tier_table.h. No string literal `/provision`, `/forget` or `/ip_config` may appear outside one `_WIFI_WRITE_DENYLIST` constant. No import of `wifi_uart`, `wifi_prov_http_client`, `gui_wifi_firing`, `mcp_server_wifi` or `mcp_server_network` (their writers: `wifi_prov_http_client.py:221`, `wifi_uart.py:85/94/98/113`, `mcp_server_network.py:117`, `gui_wifi_firing.py:479`).
  - (b) `_post_json` and every raw POST helper raise on any denied path.
  - (c) `GET /status` (`mode`, `state`, `ssid`, `ip_mode`) is read at run start and again at run end.
- **PASS:** (a) finds 0 hits, (b) is enforced, and the (c) start and end tuples are equal.
- **FAIL:** A source hit or forbidden import; the guard missing or not raising; or the (c) tuple changed while no operator-only case (WEB-WIFI-06) ran in the run.
- **INCONCLUSIVE:** WEB-WIFI-06 ran in the same run (it legitimately changes mode). Then judge only (a) and (b).
- **Fake-board test:** positive: `_post_json(ctx,"/api/zones",…)` passes through. negative: `_post_json(ctx,"/forget",{})` must raise and the case must FAIL if it does not. Also a fake source file containing `"/provision"` must give FAIL.

#### WEB-SEC-02 — Client refuses enabling web sign-in without passwords
- **Intent:** Client-side rule: enabling web sign-in is refused while a password is unset. Checked as DOM only.
- **Class:** READ-ONLY — the refusal branch is never exercised and nothing is POSTed.
- **Observable / route:** `GET /settings/security` raw HTML (shell, `route_tier_table.h:506`) must contain:
  - `id="kcSecWebEnabled"` (`security_page.html:171`)
  - `kcSecPolicySave` (`:203`)
  - the guard text `Set both passwords before enabling web sign-in.` inside `if (webEnabled.checked && !(state.admin_password_set && state.user_password_set))` (`:509-513`)

  Input: `GET /api/auth/config` → `admin_password_set` / `user_password_set` (emitter `security_http.c:106-116`; consumer `security_page.html:414-415`). ADMIN tier (`route_tier_table.h:458`).
- **PASS:** The guard text and both flag reads are present. Report both flags as observed; they do not change the verdict.
- **FAIL:** The guard is missing, or `/api/auth/config` lacks either key. Note: the client requires BOTH passwords while the server requires only admin (`security_backend_web_auth.c:210-230`). Record this as an observation, not a FAIL.
- **INCONCLUSIVE:** `/api/auth/config` is unreachable (auth on without a session).
- **Fake-board test:** positive: HTML with the guard and `{"admin_password_set":true,"user_password_set":false}` gives PASS. negative: HTML without `Set both passwords before enabling web sign-in.` must give FAIL.

#### WEB-SEC-06 — "Clear login credentials" is never pressed
- **Intent:** The Clear login credentials button is never pressed.
- **Class:** READ-ONLY — static plus a source scan. The clear itself is forbidden (plan §6 rule 4).
- **Observable / route:**
  - `GET /settings/security` HTML must contain `id="kcSecClearCreds"` (`security_page.html:267`), the heading "Clear login credentials" (`:252`), and the handler that is wrapped in `window.kcConfirm` and posts `cmd=clear_credentials` (`:536-553`).
  - Server branch: `security_http.c:494` (POST `/api/auth/security`, ADMIN, `route_tier_table.h:459`).
  - Source scan: `clear_credentials` must not appear in bench_test/*.py outside a deny-list constant, and `_post_json` refuses `cmd=clear_credentials`.
  - `GET /api/auth/config` → `admin_password_set` (`security_http.c:106-116`) is read at run start and at run end.
- **PASS:** The markers are present, including the kcConfirm guard. The source scan is clean. `admin_password_set` did not go true→false during the run.
- **FAIL:** A source hit; or the kcConfirm guard is missing from the handler; or `admin_password_set` went true→false.
- **INCONCLUSIVE:** `/api/auth/config` is unreadable at start or end.
- **Fake-board test:** positive: start and end `{"admin_password_set":true}` with the button and kcConfirm present gives PASS. negative: start `true`, end `{"admin_password_set":false}` must give FAIL.

#### WEB-BAK-02 — Export is valid JSON with every zone and profile
- **Intent:** The backup export is valid JSON containing every zone and every user profile.
- **Class:** READ-ONLY — GET only.
- **Observable / route:**
  - `GET /api/backup/export` (ADMIN, `route_tier_table.h:396`), using the existing helper `backup_export_http_client.get_export(host)` → `(raw, parsed)` (`backup_export_http_client.py:95`). Emitter `backup_export.c:233`; the envelope `{"kind":"kilnctl_backup","version":N,"profiles":[` is at `:268`, the profiles loop at `:271-285`, `"zones":[` at `:313`, and the zones loop with `"index"` at `:316-330`. Consumer: `backup_page.html:113`.
  - Cross-checks: `GET /api/profiles` lists `builtin:false` ids (`profiles_catalog_http.c:308-335`, USER, `route_tier_table.h:215`). `GET /api/zones` gives `thermo_count` (`zones_http_get.c:285/322`, ADMIN `route_tier_table.h:233`). Export emits one zone per index below thermo_count (`zones_config_accessors.c:503-506`).
- **PASS:**
  - It parses as JSON with `kind=="kilnctl_backup"`, an int `version`, and list `profiles`/`zones`.
  - The set of profile ids equals the set of `/api/profiles` builtin:false ids.
  - The set of zone `index` values equals {0..thermo_count-1}.
  - `scan_for_sensitive_fields(raw)` is false (`:84`).
- **FAIL:** Bad JSON or wrong kind; a missing or extra profile or zone; sensitive fields found.
- **INCONCLUSIVE:** `/api/zones` or `/api/profiles` is unreadable, so there is nothing to compare against.
- **Fake-board test:** positive: export `{"kind":"kilnctl_backup","version":1,"profiles":[{"id":0}],"zones":[{"index":0},{"index":1}]}`, profiles `[{"id":0,"builtin":false}]`, `thermo_count:2` gives PASS. negative: the same but zones `[{"index":0}]` must give FAIL.

#### WEB-BAK-03 — Export→import round-trip gives an identical fingerprint
- **Intent:** Importing the just-exported file gives an identical fingerprint.
- **Class:** NEEDS OWNER — requires POST `/api/backup/import` (`backup_import.c:3657`; ADMIN `route_tier_table.h:381`), which is forbidden.
- **Observable / route:** n/a as written. The safe reduction uses only `GET /api/backup/export` (`backup_export.c:233`).
- **PASS:** (reduced) Two exports taken about 5 s apart, with no writes between them, are byte-identical after parsing (same canonical-JSON hash). The import side is covered by host test `test_backup_import.c`.
- **FAIL:** (reduced) The canonical hashes differ while nothing was written.
- **INCONCLUSIVE:** A firing or another mutating case ran between the two exports.
- **Fake-board test:** positive: two equal export bodies give PASS. negative: a second export with one profile name changed must give FAIL.
- **NEEDS OWNER:** May the bench ever POST backup_import, even a self-import? — recommended: no. Reduce to export-determinism plus host-test coverage, or drop.

#### WEB-BAK-04 — Import refused while HP-01 runs
- **Intent:** A backup import is refused while HP-01 (a firing) is running.
- **Class:** NEEDS OWNER — exercising it means POSTing `/api/backup/import` during a firing. That is forbidden, and if the gate failed the import would land mid-firing.
- **Observable / route:**
  - Gate code: the `system_mode_gate` refusal at `backup_import.c:3680-3682`, the OTA interlock at `:3713`, and the async-busy check at `:3731`.
  - Page text: "refused while a profile is running / heaters on / OTA in progress" (`backup_page.html:139-142`); import calls at `:202` and `:236`.
- **PASS:** (reduced, static) The served `/settings/backup` HTML contains the refusal text (`:139-142`).
- **FAIL:** (reduced) The text is missing.
- **INCONCLUSIVE:** n/a.
- **Fake-board test:** positive: HTML with the text gives PASS. negative: HTML without it must give FAIL.
- **NEEDS OWNER:** Is an import POST during HP-01 ever acceptable as an OBSERVER (depends_on HP-01)? — recommended: no. Use a static page-text check plus host-test coverage of `system_mode_gate` in backup_import, or drop.

#### WEB-KCFG-02 — Kiln-config slot lifecycle (save/clone/rename/export/import/delete)
- **Intent:** Save as new "BENCH_tmp", clone, rename, download (/export), import as a new slot, then delete both.
- **Class:** MUTATING — writes kiln-config store slots only and never applies.
- **Observable / route** (all ADMIN):
  - List: `GET /api/kiln_configs` → `active_id`, `configs[{id,name,is_active}]`, `max_count` (`kiln_cfg_http.c:143-188`; USER `route_tier_table.h:222`; consumer `kiln_configs_page.html:208`).
  - Save `{"id"}` at `kiln_cfg_http.c:205-288`; clone at `:291-328`; rename at `:566-608` (400 on a duplicate name, `:590-592`).
  - Export `?id=` at `:616-666` (`Content-Disposition kiln_<id>.kilnpkg.json`, `:659`).
  - Import `{"id":N}` at `:674-718`, which creates a new slot and never applies (`:668-671`).
  - Delete at `:537-563` (refuses the active config, backstop).
  - Tiers: `route_tier_table.h:262`, `:282-286`.
- **PASS:** Each step answers 200/`{"id"}`. The export parses as JSON. The import returns a new id distinct from all others. The restore read-back equals the snapshot.
- **FAIL:** Any step is non-200, or the export is not JSON. **A restore mismatch is an unconditional FAIL.**
- **INCONCLUSIVE:** The gate fails: fewer than 3 free slots, or the executor is not idle.
- **Mutation / gate / restore:**
  - Gate: mutating suite only; `GET /api/profile_exec` state is idle; `max_count - len(configs) >= 3`.
  - Writes:
    1. `save name=BENCH_tmp` gives A.
    2. `clone id=A name=BENCH_tmp_c` gives B.
    3. `rename id=B name=BENCH_tmp_r`.
    4. `export id=B`.
    5. Rewrite the package's name field to `BENCH_tmp_i` (to avoid a name collision), then `import`, which gives C.
  - Restore in finally: delete every created id (C, B, A; only ids this case created; never the active one). Then `GET /api/kiln_configs` must equal the pre-snapshot: the same `(id,name)` set and the same `active_id`.
- **Fake-board test:** positive: a fake store that adds and removes slots gives PASS. negative: the fake delete returns 200 but the slot is still listed at read-back; that must give FAIL.

#### WEB-KCFG-03 — Self-apply returns 202 then done_ok
- **Intent:** Applying the active config to itself returns 202, then apply_status reports done_ok.
- **Class:** NEEDS OWNER — apply is not slot-only. In `kiln_cfg_swap.c`:
  - it writes `SAFETY_PARAM_ID_ABS_MAX_TEMP_C` to the Pico when target ≥ current, which includes the equal self-apply case (`:587-609`);
  - it pushes the whole Pico package, CT keys 0x0308-0x030A included (`:619`; `kiln_cfg_http.c:405-409`);
  - it commits the zones config (`:666`) and writes Pico flash fallback (`:764`).

  Writing abs_max and CT calibration is forbidden by §6 rule 4. Also, `docs/audits/kiln_config_self_apply_diverged_2026-09-22.md` records a self-apply false `done_failed`/`diverged` on the bench. I could not confirm whether that race is fixed: my grep for the old cached_crc==0 check found nothing.
- **Observable / route:** (reduced) `GET /api/kiln_configs/apply_status` → `state`, `id`, `diverged`, `reason` (`kiln_cfg_http.c:491-522`, emit `:518`; USER `route_tier_table.h:281`; consumer `kiln_configs_page.html:338`). Static: `kcApplyBtn` (`:122-123`).
- **PASS:** (reduced) `state` is in {idle,running,done_ok,done_failed}, `diverged` is a bool, and kcApplyBtn and the apply_status poll are present.
- **FAIL:** (reduced) The state is outside the enum or a key is missing.
- **INCONCLUSIVE:** `state=="running"` for the whole read (someone else is applying).
- **Fake-board test:** positive: `{"state":"idle","id":null,"diverged":false,"reason":""}` gives PASS. negative: `{"state":"bogus"}` must give FAIL.
- **NEEDS OWNER:** May the bench POST `/api/kiln_configs/apply` (ADMIN `route_tier_table.h:274`) given that it writes abs_max_temp_c, CT keys, zones and Pico flash? — recommended: no. Use the read-only reduction above, and keep the real self-apply as a separate, operator-confirmed case once the diverged false-alarm audit is closed.

#### WEB-KCFG-04 — Hardware-differs apply needs the ack header (428)
- **Intent:** A config with a different relay count is refused with 428 when the X-Kiln-Ack-Hardware-Differs header is absent.
- **Class:** NEEDS OWNER — needs an edited package import plus an apply POST. If the `kiln_cfg_store_slot_hardware_differs` check (`kiln_cfg_http.c:432-438`) failed to fire, a real swap to a different relay count would run (`kiln_cfg_swap.c`; abs_max/CT/zones writes). The 428 also collides with the OTA interlock's 428 (`:396-403`) and can only be told apart by the body (`kiln_configs_apply_http_client.is_hardware_differs_body`).
- **Observable / route:** (reduced, static) `/settings/kiln_configs` HTML (`route_tier_table.h:504`) must contain the `kcAckHwDiffers` checkbox (`kiln_configs_page.html:131-132`) and the `X-Kiln-Ack-Hardware-Differs` header send (`:291-296`).
- **PASS:** (reduced) Both markers are present.
- **FAIL:** (reduced) Either marker is missing.
- **INCONCLUSIVE:** n/a.
- **Fake-board test:** positive: HTML with both gives PASS. negative: HTML without `X-Kiln-Ack-Hardware-Differs` must give FAIL.
- **NEEDS OWNER:** Is a live 428 probe acceptable? — recommended: no. Use the static reduction plus host-test coverage of the 428 branch. If a live probe is ever wanted, it must only target a slot whose `hardware_differs` is proven by a read-only route first, and that route does not exist today (§7.9: no new routes).

#### WEB-KCFG-05 — Divergence banner absent when the safety ceiling matches
- **Intent:** The divergence banner is absent when `safety_ceiling_match` is ok.
- **Class:** READ-ONLY.
- **Observable / route:** `GET /api/readiness` → the item with key `safety_ceiling_match` and its `status` (emitter `readiness_http.c:690-711`; OPEN `route_tier_table.h:162`). Consumer: `refreshDivergence` (`kiln_configs_page.html:401-421`) shows the banner when status is not in {ok,cannot_yet,deliberately_off}. Static: `id="kcDivergeBanner"` carries `hidden` (`:111-113`).
- **PASS:** Item status is `ok`, the banner markup is present with `hidden`, and refreshDivergence references `safety_ceiling_match`.
- **FAIL:** Item status is `not_done` (or any bad status, i.e. a real divergence); or the item is missing; or the banner or its logic is missing from the HTML.
- **INCONCLUSIVE:** Item status is `cannot_yet` or `deliberately_off`.
- **Fake-board test:** positive: readiness `[{"key":"safety_ceiling_match","status":"ok"}]` plus HTML with the hidden banner gives PASS. negative: `[{"key":"safety_ceiling_match","status":"not_done"}]` must give FAIL.

#### WEB-LOG-02 — Login good and bad credential branches (auth on)
- **Intent:** With web auth on, the good-credential and bad-credential login branches both behave.
- **Class:** MUTATING — opens its own auth-on window. WEB-SEC-03's window closes in its own finally (`cases_web_rw.py:559-565`), so this case cannot observe it.
- **Observable / route:**
  - `POST /api/auth/login` (OPEN `route_tier_table.h:131`; registered `web_auth_login_http.c:637`).
  - Bad credentials give 401 "invalid username or password" (`:487-490`).
  - Good credentials give 200 `{"ok":true}` plus `Set-Cookie: kiln_sid=…; HttpOnly; SameSite=Strict; Path=/` (`:555-567`).
  - A lock gives 429 plus `Retry-After` (`:403-407`).
  - Consumer: `login_page.html:79-100` (429 branch at `:88`).
  - One failure locks the caller's IP for 5 s (`login_backoff.h:39-41`); a success clears the ladder (`web_auth_login_http.c:478-479`).
- **PASS:**
  - The bad attempt returns 401 and sets no cookie.
  - After waiting Retry-After + 1 s (at least 6 s), the good attempt returns 200 with a `kiln_sid` cookie that has HttpOnly.
  - `GET /api/auth/session` with that cookie reports `role` != "none" (`web_auth_session_status_http.c:124-128`).
- **FAIL:** The bad attempt gets 200 or a cookie; the good attempt is not 200 or has no cookie; HttpOnly is missing. **A restore mismatch is an unconditional FAIL.**
- **INCONCLUSIVE:** The first attempt returns 429 (an existing lockout on this IP).
- **Mutation / gate / restore:**
  - Gate: mutating suite only, and KILNCTL_WEB_USERNAME/PASSWORD are set (otherwise SKIP, like SEC-03 at `cases_web_rw.py:482-487`).
  - Writes: SEC-03's exact pattern — `set_web_password` (env creds, §7.7), then `set_policy(web_enabled=1, other fields unchanged)`.
  - Sequence: one wrong login (fixed wrong string), wait, then one good login. The good login must come last so the backoff ladder is left cleared.
  - Restore in finally: `set_policy` with all 4 original fields, then read back `/api/auth/config`; all 4 must equal the original.
  - Credentials and cookie values are never logged.
  - Place this case in the auth-on group next to SEC-03 (§5.3 rule 4).
- **Fake-board test:** positive: FakeSecClient bad login gives 401, good login gives (200, "kiln_sid=x"), restore read-back equal: PASS. negative: bad login returns (200, "kiln_sid=y"), which must give FAIL.

#### WEB-LOG-03 — Login 429 branch (alias of WEB-SEC-05)
- **Intent:** The 429 lockout branch. This is the same test as WEB-SEC-05.
- **Class:** OBSERVER (depends_on WEB-SEC-05) — the alias reuses SEC-05's result. It must never send its own bad logins, because the lockout can only run dead last (`registry.py:334-340`, `_ALWAYS_LAST`).
- **Observable / route:**
  - WEB-SEC-05's status codes come from `_case_web_sec05` (`cases_web.py:399-426`): 6 wrong logins, judged by `judge_login_lockout` (`judgments.py:3207`).
  - Server: 429 plus `Retry-After` (`web_auth_login_http.c:403-407`). Client: the 429 message (`login_page.html:88-96`).
  - Registry: `registry.py:221` declares LOG-03 with no judge today.
- **PASS:** WEB-SEC-05 ran in this run and was PASS. LOG-03 copies that verdict and its observed codes.
- **FAIL:** WEB-SEC-05 ran and was FAIL. Copy the reason.
- **INCONCLUSIVE:** WEB-SEC-05 was INCONCLUSIVE.
- **Fake-board test:** positive: run-results with SEC-05=PASS give LOG-03 PASS. negative: SEC-05=FAIL must give LOG-03 FAIL. If SEC-05 is absent, the result is NOT_RUN, never PASS.
- **Wiring:** Because SEC-05 is pinned last, LOG-03 must resolve after it as a post-run alias. If the run report cannot resolve it there, report NOT_RUN ("SEC-05 did not run").

#### WEB-X-02 — Session-lock prompt and "Stay unlocked" extend (timeout 1 min)
- **Intent:** With web auth on and web_timeout_min=1, the session-lock prompt appears in the last window, and "Stay unlocked" extends the session.
- **Class:** MUTATING — opens its own auth-on window, like LOG-02. SEC-03's window is not observable. The nightly list places X-02 before heat (`registry.py:497-499`); move it into the auth-on group next to SEC-03 (§5.3 rule 4).
- **Observable / route:**
  - `GET /api/auth/session` (OPEN `route_tier_table.h:140`) → `{"role","prompt","seconds_left","bootstrap_needed","auth_enabled"}` (`web_auth_session_status_http.c:124-128`; seconds_left at `:78-84`). The prompt window is 10 s (`web_auth_session.h:259`; `web_auth_session.c:182-199`).
  - Polling this route is not activity (`app.js:2141-2143`).
  - `POST /api/auth/session/extend` (USER `route_tier_table.h:208`) → `{"ok":true}` (`web_auth_session_status_http.c:142-146`).
  - Static `/app.js` (OPEN `route_tier_table.h:156`) must contain `kc-lock-prompt`, `Stay unlocked`, and `fetch('/api/auth/session/extend'` (`app.js:2154-2175`), plus `st.prompt` gating the prompt (`:2312-2318`).
- **PASS:**
  - The static markers are present.
  - After login, an early poll shows `prompt:false` with `seconds_left>10`.
  - Polling every 2 s (no other cookie traffic) reaches `prompt:true` with `seconds_left<=10` within 60 s.
  - The extend call returns 200 `{"ok":true}`.
  - The next poll shows `prompt:false`, `seconds_left>=45`, and `role` != "none".
- **FAIL:**
  - `set_policy(web_timeout_min=1)` is refused.
  - The prompt never turns true before `role` becomes "none".
  - The extend is not 200.
  - `seconds_left` does not reset after the extend.
  - A static marker is missing.
  - **A restore mismatch is an unconditional FAIL.**
- **INCONCLUSIVE:** The login itself returns 429 (an existing lockout).
- **Mutation / gate / restore:**
  - Gate: mutating suite only, and env creds are set (otherwise SKIP).
  - Writes: `set_web_password` (env creds), then `set_policy(web_enabled=1, web_timeout_min=1, lcd fields unchanged)`, then one login.
  - Restore in finally: `set_policy` with all 4 original fields (`_policy_from_config` keeps -1), then read back `/api/auth/config`; all 4 must match. Credentials and cookies are never logged.
- **Fake-board test:** positive: FakeSecClient session sequence `{prompt:false,seconds_left:40}`, `{prompt:true,seconds_left:8}`, extend 200, `{prompt:false,seconds_left:59,role:"admin"}` gives PASS. negative: the post-extend poll `{prompt:false,seconds_left:0,role:"none"}` must give FAIL.

NEEDS OWNER ids: WEB-BAK-03, WEB-BAK-04, WEB-KCFG-03, WEB-KCFG-04

## 5. NEEDS OWNER summary

These 18 ids each carry a recommended interpretation in their block. An implementer builds the
recommended form unless the owner rules otherwise.

| Id | Question | Recommended |
|---|---|---|
| WEB-DASH-04 | The plan says max-ramp popup. The code shows a model-feasibility popup, and refuses an over-max ramp at start. | Read-only over `feasibility`. Never start an infeasible profile. **Owner 2026-10-09: accepted recommendation (resolved).** |
| WEB-DASH-07 | May the harness ack `last_run`? The ack is permanent. | Read-only. Ack only when `last_run.profile_id == 7` (the bench slot). **Owner 2026-10-09: accepted recommendation (resolved).** |
| WEB-DASH-12 | The disconnected banner needs a browser. | A static constant check now. A node unit test later. **Owner 2026-10-09: accepted recommendation (resolved).** |
| WEB-STIM-02 | The timing-profile write is a whole-page POST, which resyncs the Pico abs_max. | Read-only. Add a narrow writer first if a write is wanted. |
| WEB-ZONE-02 | The whole-page identity POST may resync the Pico abs_max, because GET prints the ceiling at `%.1f`. | Strict ceiling gate plus stripped model keys. Otherwise reduce to read-only. |
| WEB-ZONE-03 | The selects have no narrow writer, and writing tc_type reconfigures hardware. | Read-only plus a static check. Never write tc_type. |
| WEB-ZONE-05 | `zones_page.html` never calls `/api/zones/pid`. | An identity write on the route itself, or retarget the case. |
| WEB-SAF-03 | It needs a pre-clear OT-B01 hook. The button's enable predicate is "trip seen this Pico boot". | Yes to the hook (the `otb01_tripped` probe). Otherwise judge only the pre-state half. |
| WEB-COMM-03 | Any `commit=1` sends abs_max and the CT gains. | The read-only CRC-stability form. |
| WEB-COMM-06 | "Channel 2 only" is a DOM claim. | Judge the inputs plus the static markers. |
| WEB-DIAG-09 | May Danger Mode energize R0? | Read-only plus a 409 refusal probe. The full toggle only behind an explicit opt-in. Resolved. Owner 2026-10-09: accepted recommendation. |
| WEB-DIAG-11 | The stale banner needs a browser. | A static presence check. Resolved. Owner 2026-10-09: accepted recommendation. |
| WEB-OTA-03 | This is an OTA push through the DOM. | An alias of OT-E01 plus a static check. Never push. Resolved. Owner 2026-10-09: accepted recommendation. |
| WEB-OTA-04 | This is a rollback through the DOM. | An alias of OT-E02 plus a static check. Resolved. Owner 2026-10-09: accepted recommendation. |
| WEB-BAK-03 | It needs backup_import, which is forbidden. | Export determinism plus host-test coverage, or drop the case. Resolved. Owner 2026-10-09: accepted recommendation. |
| WEB-BAK-04 | It needs an import POST during a firing. | Static page text plus host-test coverage, or drop the case. Resolved. Owner 2026-10-09: accepted recommendation. |
| WEB-KCFG-03 | Self-apply writes abs_max, the CT keys, zones and Pico flash. | The read-only apply_status reduction. Resolved. Owner 2026-10-09: accepted recommendation. |
| WEB-KCFG-04 | A live 428 probe risks a real swap. | The static reduction plus host-test coverage. Resolved. Owner 2026-10-09: accepted recommendation. |

Owner 2026-10-09: every NEEDS OWNER row above takes its Recommended entry (WEB-STIM-02, WEB-ZONE-02, WEB-ZONE-03, WEB-ZONE-05: accepted recommendation). Still open for the owner:

- **WEB-PROF-03..07 (resolved, owner 2026-10-09).** These cases create, edit and delete transient
  `BT_PROF*` profiles in a verified-empty free user slot below 100, then delete and read back in
  `finally`; BENCH_TEST_SYSTEM_PLAN.md rule 12 carries the exception.
- **Aliases of NEEDS OWNER cases.** These inherit the ruling on their target:
  - WEB-WIZ-07 aliases WEB-COMM-03.
  - WEB-WIZ-09 aliases WEB-ZONE-05.

  WEB-SET-04 aliases OT-B01, and WEB-OTA-03/04 alias OT-E01/OT-E02.
