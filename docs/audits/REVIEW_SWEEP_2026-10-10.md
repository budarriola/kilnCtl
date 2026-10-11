# Review coverage sweep, 2026-10-10

Scope: every commit in `git log origin/main..origin/dev` at `9c9600384` that touches at
least one non-doc file. That is 534 of 899 commits. For each one, the sweep checked whether
a `docs/audits/` review doc dated 2026-10-09 or 2026-10-1x names it as reviewed. A doc
counts when it gives the SHA or a pre-rebase SHA with the same subject, or when it states a
diff range or snapshot the commit falls inside. A doc that mentions the SHA only as "fixed
in" does not count as a review.

Result:

| Bucket | Commits |
|---|---|
| Reviewed by an existing doc | 444 |
| Excluded because a review is already in flight (dk7fix, toolfx7, saftyfx7, lcdfx2, devbreak, webfx6, execfx2) | 16 |
| Unreviewed until now, reviewed in this doc | 74 |

The 74 commits reviewed here break down as:

- 8 production firmware commits
- 33 KilnFW and SaftyFW host-test or check commits
- 33 tools commits

Review order was firmware first, safety-relevant first. Tools commits got a lighter pass,
weighted toward the confirm and safety gates (`7d27efd10`), the dev-flow git scripts
(`0b6c0c052`, `b8b8c695e`), and bench-judge verdict logic.

## Findings

### LOW-1: `727aa12d9` deletes the REVIEW_WEBFX4 MED-1 regression test its subject claims to add

`firmware/KilnFW/App/test/test_ui_edit_firing_apply.c`, around line 561 at the parent commit.

The subject reads "web edit inside the check-to-save window is refused (REVIEW_WEBFX4
MED-1)". The diff does something else: it removes `test_apply_refused_after_web_edit()`
(31 lines, including its `main()` call) and drops a stray 10th `NULL` argument from one
`live_profile_fork()` call. The excluded commit `e36279885` had added both.

The commit was needed because the `NULL` argument does not compile against the 9-argument
prototype (`live_profile.h:225`). But it threw the test away along with the bad argument.
`test_apply_stale_generation_refused()` (lines 384-416) still covers a web save before the
first Apply and a web save inside the validate-to-save window. The removed case is no longer
covered: a successful Apply, then a web edit, then a second Apply.

Verified at `9c9600384`: re-adding the removed function unchanged builds, and
`build_host_tests.ps1 -Only ui_edit_firing_apply` reports 129/129 checks passed, MED-1
section included. The test was sound; only the stray argument in the other hunk was broken.

Suggested fix: restore `test_apply_refused_after_web_edit()` and its `main()` call
verbatim. Keep the 9-argument `live_profile_fork()` call.

### LOW-2: `check_link_impl_isolation.ps1` fails on origin/dev

`firmware/KilnFW/App/test/test_cfg_fs_mount_state.c:48`: `static uint32_t ref_crc(...)`

Running the check directly at `9c9600384` exits 1:
"1 CRC/byte-stuffing implementation(s) found outside firmware/CommonFW". The function came in
with `0131bb43a` (cfg_fs_mount format state machine host test, round 2 R2-F). It is a
nibble-table CRC-32 that builds a valid LittleFS superblock image, so it is not link code.
The check still flags it, and there is no allowlist entry. Every full suite run on dev will
show this check red until it is resolved.

Suggested fix: add a by-path allowlist entry with a reason in
`firmware/SaftyFW/tools/check_link_impl_isolation.ps1`, next to the `test_link_task_fuzz.c`
entry (line ~351). Reason: "LittleFS metadata CRC used to forge a superblock in a host
test; not a kilnlink CRC". The alternative is to call littlefs's own `lfs_crc()`, if the
host test links littlefs.

### LOW-3: LCD-19 stays INCONCLUSIVE on the bench after the first run seeds the PIN

**FIXED (sweepfx):** `seed_lcd_pin` no longer raises on a pre-set PIN; it returns the env PIN flagged `unverified`. WEB-SEC-04 hands it on flagged, and LCD-19 proves it with a real `enter_pin_verified` unlock, reporting INCONCLUSIVE only when the env var is missing or the unlock fails.

`tools/PcTools/src/kilnctrl/bench_test/cases_web_rw.py:1004` (from `047844c5d`)

`seed_lcd_pin()` now raises `unverified` (INCONCLUSIVE) whenever `admin_pin_set` is
already true. `set_lcd_pin` is one-way, so the first run that seeds the PIN leaves it set
for good. After that, every run takes the `unverified` branch:

- standalone LCD-suite runs report LCD-19 INCONCLUSIVE
- in a combined run, WEB-SEC-04 no longer populates `ctx["_lcd_pin"]`, because of its
  `admin_pin_set_before` branch (lines 1084-1093)

The L6 residual fix is right that a PIN of unknown origin must not be trusted. The trouble
is that nothing ever lets a later run trust the PIN this harness set itself. The PIN-lock
case on the bench therefore loses its coverage silently, as an INCONCLUSIVE rather than a
FAIL.

Suggested fix: let LCD-19 prove the PIN itself. Attempt `enter_pin(right_pin)` on the
locked LCD. If the unlock succeeds, the PIN is verified. If it fails, report INCONCLUSIVE
("pre-existing PIN differs from KILNCTL_LCD_PIN"), not FAIL. This uses an LCD-side attempt
the case already makes, so it needs no new web route.

### INFO-1: `af93e8232`: the "ONE shared gate" comment overstates its coverage

**FIXED (sweepfx):** comment narrowed to zones_config blob writers and names the relay_names/zone_normals savers as not covered.

`firmware/KilnFW/App/drivers/persist/zones_config_store.c:902`

The comment says `nvs_save()`'s rollback-journal gate covers every zones writer.
`relay_names_save_locked()`/`relay_names_save()` (lines 1435/1453) and
`zone_normals_save_locked()` (line 1625) write without going through `nvs_save()`, so the
gate does not apply to them. This is harmless as long as the kiln-config rollback journal
never restores relay names or zone normals.

Suggested fix: confirm that, and narrow the comment to "every zones_config blob writer", or
add the same `kiln_cfg_swap_zone_edits_at_risk()` check to those savers.

### INFO-2: `708baa986`: an oversized pref file is treated as "newer" for good

**NOT CHANGED (sweepfx):** deliberate earlier "cannot decide, so keep" design (same trade-off as K10-11); left alone.

`firmware/KilnFW/App/drivers/persist/pref_cfg_fs.c:265-273`

`cfg_fs_read()` returning `ESP_ERR_INVALID_SIZE` now always means "newer, version 0xFF",
so `resolve()` never rewrites the file. A file that is current-version but corrupt and
oversized (for example, a torn append) cannot be told apart from a newer one. That
preference stays on its NVS value with no path to repair. This is the same deliberate
"cannot decide, so keep" trade-off as K10-11 and is acceptable.

Suggested fix: log the condition once at WARN with the file size, so the bench log shows it.

### INFO-3: `6515efe70`: generic key names exempted globally in the cfg_convert drift check

**FIXED (sweepfx):** `c`/`unit`/`type` are exempt only inside `backup_export_relay_cycles`/`backup_export_prefs`; elsewhere they are compared against cfg_convert.py.

`firmware/KilnFW/App/test/cfg_convert_field_mirror_drift_check.py:77-89`

`NON_ZONE_STRUCTURAL_KEYS` now includes `"c"`, `"type"` and `"unit"`. No zone key uses
these names today. If a future zone field takes one of them, the drift check will skip it
silently.

Suggested fix: subtract the additive top-level keys only at the top level, not from the
zone key set.

### INFO-4: `f3a331550`: `test_link_task_fuzz.c` is allowlisted as a whole file

`firmware/SaftyFW/tools/check_link_impl_isolation.ps1:351-354`

The entry exempts the whole test file, so a real CRC or stuffing copy added to it later
would pass the check. This matches how the other by-path entries work, so it is noted only.

### INFO-5: `06eb0b62c`: the autotune test stub mirrors `relay_authority_on_blocked()` by hand

**FIXED (sweepfx) as a drift note:** linking the real function would pull in relay_authority.c dependencies; the stub comment now names the real function and the host test that pins its semantics.

`firmware/KilnFW/App/test/test_autotune_engine_prestart.c:245-250`

The comment acknowledges the risk ("kept in step by hand"). If the real function changes,
the LD-01 test keeps passing against the old semantics.

Suggested fix: link the real `relay_authority_on_blocked()` into this test, or add a
mirror-drift line check.

### INFO-6: `b8b8c695e`: the `worktree_mint.ps1 -Remove` unlanded-commit guard fails open

**FIXED (sweepfx):** `-Remove` refuses on a non-zero rev-list exit (even with -Force); `check_worktree_mint.ps1` has a broken-ref case.

`tools/worktree_mint.ps1:170`

The `git rev-list` call ignores its exit code. If it errors, the unlanded list is empty and
the removal goes ahead without `-Force`.

Suggested fix: check `$LASTEXITCODE` and refuse when it is non-zero.

### No issue found

All other commits reviewed here showed no issue. Points checked specifically:

- **`af93e8232` (firmware):** `rollback_ex` keeps `s_rollback_id_kept` when `clear_pending()` fails. `/pid` returns 409 when zone edits are at risk. The legacy-blob retire clears the used bit before erasing the key, so a power cut leaves the slot marked unused, which is the safe side.
- **`708baa986` (firmware):** backup import reports `relay_cycles_kept` (96 B buffer, fits). `ota_page.html` handles 401.
- **`806f9c7b5` (firmware):** the thermo bridge passes a NULL reason, which `system_mode_gate_check` accepts.
- **`4bd1de732`, `11a86b33d`:** comment and doc-path changes only.
- **`062456379` (firmware):** `live_profile_save_working_if_gen()` does the check, the save and the generation bump in one locked section. Successful `clear_all_credentials` now invalidates both roles' sessions. When this commit landed, `live_profile_clear()` locked only the bump, but tip already takes the lock around the whole clear.
- **`a23a67daa`, `f3a331550` (SaftyFW):** the SET_PARAM range, type and direction refusals and the update routing assertions are real, not vacuous: they count staged entries and handler calls.
- **`f36fed0eb`:** drives the aux fault-drop through the real task tick. The stale and never-received cases deliberately leave aux ON; link loss is handled through `fault_sources`, not this derivation.
- **`60d372811`:** a test executable that hangs past the timeout returns -1 and counts as a failure, with tree-kill.
- **`7d27efd10`:**
  - zone PID and model writers get strict confirm, a mid-run refusal and read-back
  - the safety writers' mid-run probe now fails closed
  - the facade treats `confirm*`/`force*`/`allow_*`/`ack_*` as strict booleans
- **`f2e9f35a1`:** `adaptive_tune_revert` verifies the revert by reading back `revert_available`.
- **`b8b8c695e`:** release branch and tag go up in one atomic push. `wt_status` protects mid-rebase worktrees and ones holding gitignored data.
- **`86fc229a9`:** `BodyDecodeError` subclasses `OSError`, so a bad gzip maps to INCONCLUSIVE.

## Tests run (worktree at `9c9600384`)

- `build_host_tests.ps1 -Only ui_edit_firing_apply`, run twice:
  - as-is: passed
  - with the removed MED-1 test restored, as a temporary local edit that was reverted afterwards: 129/129 passed
- `firmware/SaftyFW/test/build_host_tests.ps1`: SAFTYFW HOST TESTS all passed, including `test_link_task_fuzz` with the a23a67daa scenarios.
- `firmware/SaftyFW/tools/check_link_impl_isolation.ps1`: FAILED, see LOW-2.

No negative test was run. None of the reviewed fixes looked suspicious enough to need one;
LOW-1 was confirmed by running the restored test instead.

## Coverage table

"this doc" means the commit was reviewed above. "excluded" means another review of it is
in flight. Any other value names the review doc that covers the commit.

The diff-range rules applied:

- SaftyFW/CommonFW-only commits that are ancestors of `942cefb97` are covered by REVIEW_SAFTYFW_SINCE_PRE2.
- tools-only commits that are ancestors of `1e6d375f` are covered by DEV_FLOW_SCRIPTS_REVIEW.
- tools-only commits that are ancestors of `d3e2a6ba` are covered by DEV_TOOLS_REVIEW.
- Hand mappings by subject: `163980001`, `4271767d6`, `48e1ba8ac`, `9cc5ed06b`, `65276a7b5`, `725559930`, `0744e4bf5`, `ffcea4314`.

| SHA | Subject | Reviewed by |
|---|---|---|
| `7c5cfaa18` | Exec review: restore tests for late-only restore, cap and no-rule refusal sites | excluded (in flight: execfx2) |
| `ee2b0b7ad` | Exec review: LOW-E caller epoch test, history kept test | excluded (in flight: execfx2) |
| `93336ac79` | Exec review fixes: warm-start replay after last refusal, restore keeps history, snapsho... | excluded (in flight: execfx2) |
| `696529d9a` | ui_page_network_manage.c: restore '\0' literal (raw NUL byte committed in 5c89d3a3b bro... | excluded (in flight: devbreak) |
| `dea054328` | Fix dev break: stub cfg_fs_degraded in dashboard_settings_http/setup_progress_http host... | excluded (in flight: devbreak) |
| `8cda68715` | live_profile: fix merge of clear wrapper with upstream fork_gen | excluded (in flight: webfx6) |
| `727aa12d9` | ui_edit_firing_apply test: web edit inside the check-to-save window is refused (REVIEW_... | this doc |
| `e36279885` | Web UI JS audit LOWs, persfx2 LOW-3, webfx4 MED-1/LOW-1..4,6: page failure reporting, r... | excluded (in flight: webfx6) |
| `9d80a45ee` | heat_enable test: F8 release keeps snapshot | excluded (in flight: saftyfx7) |
| `8dd0e984f` | SaftyFW: F1 test non-vacuous | excluded (in flight: saftyfx7) |
| `a2ba68fa4` | SaftyFW: F5 test | excluded (in flight: saftyfx7) |
| `c9ffc21e7` | SaftyFW/KilnFW: review saftyfx6 fixes F1-F8 (S12/S8/S1 clear semantics, trip snapshot a... | excluded (in flight: saftyfx7) |
| `16108208f` | ui_page_network_manage.c: restore LF line endings (previous commit converted the whole ... | excluded (in flight: lcdfx2) |
| `465379da4` | Tools review 3 fixes: ota params need confirm+preflight, drop leave_halted, HP-05 card ... | excluded (in flight: toolfx7) |
| `235e4e3c4` | K7 review fixes F1/F3/F4/F5: unfailable danger-mode rollback, unserialised all-off neve... | excluded (in flight: dk7fix) |
| `1557df89a` | LCD stale-gen test: assert the early check refuses before the validator | excluded (in flight: lcdfx2) |
| `5c89d3a3b` | LCD review LOW1-4: close network modals on relock, hub USER gate rules, fork reports ow... | excluded (in flight: lcdfx2) |
| `8fc0549db` | aux rule profile: call-site test for derived-field tolerance (negtest L5) | REVIEW_MCPFX4_2026-10-10 |
| `bc09d681c` | MCP review fixes (mcpfx2 H1, M1-M3, L1-L5; mcpfx1 LOW-5/7): AP password presence-only r... | REVIEW_MCPFX4_2026-10-10 |
| `01fbfc045` | test: LOW-5 covers coil_power, cal_offset, adaptive_tune and _no_save variants | REVIEW_WEB7_2026-10-10 |
| `703173bb2` | persfx2 review: MED-2 per-store 409, MED-3 never re-adopt frozen NVS, LOW-1/2/4/5 | REVIEW_WEB7_2026-10-10 |
| `0c33a9ef3` | profile save 409: send via httpd_resp_send; actually call the expected_rev test (WEB_UI... | REVIEW_WEB7_2026-10-10 |
| `e6144a13e` | web: stale-write guards for zones/profile saves, keep dirty commissioning form (WEB_UI_... | REVIEW_WEB7_2026-10-10 |
| `c9777156f` | wifi_prov test: assert the saved-nets retry waits (INFO-3 pin) | REVIEW_FIRE2_2026-10-10 |
| `2590ea462` | LD-01 follow-up: tests that pin the commit-time link recheck and the saved-nets retry d... | REVIEW_FIRE2_2026-10-10 |
| `d48f77266` | wifi_provision_http: fix literal NUL char in ap_password_set (target -Werror) | REVIEW_FIRE2_2026-10-10 |
| `01c74a3cc` | fake_kv typed fix follow-ups: favorites legacy u32 mask read, used-bitmap wrong-size bl... | REVIEW_FIRE2_2026-10-10 |
| `cb0874b68` | LD01 WWFIX review fixes: gate profile resume and recheck at commit, garbage-on-error li... | REVIEW_FIRE2_2026-10-10 |
| `32b33598c` | profile executor firing-path audit MED-1/MED-3/LOW-2/LOW-4: failed IO-segment and super... | REVIEW_FIRE2_2026-10-10 |
| `020c23526` | profile executor: relay-type IO-segment relays are run-owned for the running pending-OF... | REVIEW_FIRE2_2026-10-10 |
| `e6201482f` | fwlow16 review fixes: save_ex rolls back fresh slot in-lock (drop helper removed), try_... | REVIEW_MISC8_2026-10-10 |
| `ac2b1b217` | Tooling review toolfx5 L-1..L-7: seam flags gated and recorded in the verdict, retry-sk... | REVIEW_TOOLS4_2026-10-10 |
| `c09136468` | Surface pause_reason on web, LCD home and profiles_get_exec_status; keep full run-claim... | REVIEW_WEB7_2026-10-10 |
| `f006d22d9` | lint_pages: mojibake regex covers U+00C3/U+00C2 + U+20AC and cp1252 continuations; fixt... | REVIEW_MISC8_2026-10-10 |
| `0598cd290` | Review R2ACE/SL3 test fixes: cfg_fs format reason buffer 160 B (full gate text), strict... | REVIEW_MISC8_2026-10-10 |
| `a3c6475c1` | Review SL3-R2 A1-A4: commit DIAG baseline after send, bounded watchdog autotune abort, ... | REVIEW_FIRE2_2026-10-10, REVIEW_SAFTYFX6_2026-10-10 |
| `1fcf3148e` | Firing audit MED-4/MED-5: link blip with same boot id is not a reboot; reboot verdict s... | REVIEW_FIRE2_2026-10-10, REVIEW_SAFTYFX6_2026-10-10 |
| `537fd6402` | KilnFW host tests round 3: cfg_fs_format_http, setup_progress_http, dashboard_settings_... | REVIEW_MISC8_2026-10-10 |
| `0cffc8086` | aux bench: control_set_zone_relay_mask narrow writer; AX-C03 runs via it and restores t... | REVIEW_TOOLS4_2026-10-10 |
| `3eb2f983a` | LCD review L1-L3: close number pad on relock, compare-and-save for live edit, per-cell ... | REVIEW_LCDFX_2026-10-10 |
| `6d5599f1e` | MCP gates review fixes: write_memory markers authoritative + catch-wrapped halt + leave... | REVIEW_TOOLS3_2026-10-10 |
| `382a66875` | bench_test: HP-05 accepts 'no previous-run record' ack (boot-record only); LCD-05 dims ... | REVIEW_TOOLS3_2026-10-10 |
| `ec5581cee` | Firing path: refused start from DONE restores the finished run (MED-2); stale queued AU... | REVIEW_EXECFX_2026-10-10 |
| `8099b1829` | negtest/push_verify: job-member kill instead of taskkill /T, final-scan and normal-exit... | REVIEW_TOOLS3_2026-10-10 |
| `9410690ca` | MCP tool gates (review 2026-10-10 MED M1-M11): fail-closed preflight reads, read-back r... | REVIEW_MCPFX2_2026-10-10 |
| `43cf6c767` | tests: mock pico_armed_state in two pico_gpio_write tests; isolate ESP write_memory tes... | REVIEW_TOOLFX5_2026-10-10, REVIEW_TOOLS3_2026-10-10 |
| `1fdb15bdd` | Toolfx3 review T-1..T-6: keep user ssh config in pin check, classify origin probe, fetc... | this doc |
| `57addc36a` | SaftyFW trip path review fixes: refuse S1/S8 clear while condition holds (T1), keep las... | REVIEW_SAFTYFX6_2026-10-10 |
| `2695eac4a` | fwlow16: review LOW fixes (live save_as drop, reset fence, rev_unknown notice, empty id... | REVIEW_FWLOW16_2026-10-10 |
| `ad3640f9f` | cfg_convert: stale_or_unknown_stores is a known additive top-level key (MED-1) | REVIEW_WEB7_2026-10-10 |
| `062456379` | Web4 review fixes A1 A2 A3 A5 B1 C3 D2: live profile compare-and-save, Save needs known... | this doc |
| `ad517c152` | check_land: pin case message does not embed child FAIL text | REVIEW_TOOLFX3_2026-10-10 |
| `80381eb3c` | check_land: pin case creates empty submodule dir so tree is clean | REVIEW_TOOLFX3_2026-10-10 |
| `fa2c7d9a7` | Submodule pin check: origin probe, gitlink naming, tests; land/dev_promote refuse all b... | REVIEW_TOOLFX3_2026-10-10 |
| `44ecaf482` | negtest: resolve bare check names via git ls-files (tracked only); document real-tree g... | REVIEW_NEGFX_2026-10-10 |
| `93df083f2` | negtest: adopt only children created after their parent, no taskkill /T, spare names on... | REVIEW_NEGFX_2026-10-10 |
| `99b0f5f9e` | MCP tool gates H1-H3: aux manual validates unknown/i2c flags and board identity; write_... | REVIEW_MCPFX1_R3SFW_2026-10-10 |
| `f394e7036` | SaftyFW host test: R3 fuzz harness failure lines carry file:line; matrix rows for R3 li... | REVIEW_MCPFX1_R3SFW_2026-10-10 |
| `9ae6f3586` | SaftyFW host test: SET_CT_CAL, SET_FIRING_CEILING/SET_CLOCK and REBOOT/ROLLBACK_RESULT ... | REVIEW_MCPFX1_R3SFW_2026-10-10 |
| `f28189d99` | host tests: link degraded-store stub into kiln_io_owner_sx_dispatch | REVIEW_PERSFX2_2026-10-10 |
| `26da1f472` | zones_http test: failed save keeps the load fault | REVIEW_PERSFX2_2026-10-10 |
| `8a8b35e7a` | persist: refuse zones POST while undecided, visible degraded stores with 409, safe defa... | REVIEW_PERSFX2_2026-10-10 |
| `4bd1de732` | cfg_fs_mount: document why deferred format returns the original error; close tooling re... | this doc |
| `d93cf774a` | Stack budget: bx_flash_worker ceiling 6240 -> 6320 (measured 6272 B, 10240 B stack) | LCD_UI_REVIEW_2026-10-10, REVIEW_R2ACE_TOOLFX4_2026-10-10 |
| `24a5f6116` | estop heat-only gate: tolerate fakes without board/heat_blocked; fix sweep test mock | REVIEW_MCP_TOOL_GATES_2026-10-10 |
| `378d76eab` | bench_test: estop_verified not_done blocks heat cases only (SKIP estop_unverified); oth... | REVIEW_MCP_TOOL_GATES_2026-10-10 |
| `35d6fda6a` | kiln_cfg_swap: shorten rollback-refused reason arg so snprintf fits 200 bytes | REVIEW_SL3_R2ACE_2026-10-10 |
| `51df2ccc1` | Safety link fix batch sl3: slow bounded re-announce, undecided reboot status, estop cle... | REVIEW_FIRING_PATH_FIX_2026-10-10, REVIEW_SAFETY_LINK_FIX2_2026-10-10 +1 |
| `ae7124b01` | lint_pages: wire mojibake fixture test into check_lint_pages, add C2/C3 cases; negtest ... | REVIEW_R2ACE_TOOLFX4_2026-10-10, REVIEW_SL3_R2ACE_2026-10-10 |
| `0131bb43a` | Host test: cfg_fs_mount format state machine (round 2, R2-F) | REVIEW_R2ACE_TOOLFX4_2026-10-10, REVIEW_SL3_R2ACE_2026-10-10 |
| `2e515d27e` | Host test: dashboard_set_relay result mapping and dashboard_http_get_safety_trip (R2-E) | REVIEW_R2ACE_TOOLFX4_2026-10-10, REVIEW_SL3_R2ACE_2026-10-10 |
| `a3950b140` | SaftyFW host test: COMMIT_CONFIG and APPLY_CONFIG_VOLATILE handlers (R2-B) | REVIEW_R2ACE_TOOLFX4_2026-10-10, REVIEW_SL3_R2ACE_2026-10-10 |
| `68548bef8` | SaftyFW host test: real saftyfw_image_identity_record round trip through the scanner (R... | REVIEW_R2ACE_TOOLFX4_2026-10-10, REVIEW_SL3_R2ACE_2026-10-10 |
| `51ab1ea36` | Host test: owner_task relay and SX_RESET dispatch over the fake SX1509 (R2-A) | REVIEW_R2ACE_TOOLFX4_2026-10-10, REVIEW_SL3_R2ACE_2026-10-10 |
| `8eba89bd2` | lint_pages: extend mojibake pattern to U+00C2/U+00C3 lead pairs (no hits in tree) | REVIEW_MCP_TOOL_GATES_2026-10-10, REVIEW_R2ACE_TOOLFX4_2026-10-10 +1 |
| `3d9486bbf` | check_negtest: not-ok fixture prints via Write-Host so the reqassert_notok case is effe... | REVIEW_FIREFX3_TOOLFX2_2026-10-10, REVIEW_TOOLING_BATCH_2026-10-10 |
| `c43e0a0f2` | check_push_verify: P1 test narrows the fetch refspec to a branch that exists so a plain... | REVIEW_FIREFX3_TOOLFX2_2026-10-10, REVIEW_TOOLING_BATCH_2026-10-10 |
| `e5936650a` | check_main_baseline: far-future fork-point commit date so B2b rank ordering is load-bea... | REVIEW_FIREFX3_TOOLFX2_2026-10-10, REVIEW_TOOLING_BATCH_2026-10-10 |
| `1d54d0730` | Fix tooling review batch 2026-10-10: main_baseline B1/B2, push_verify P1-P3, negtest N1... | REVIEW_FIREFX3_TOOLFX2_2026-10-10 |
| `3c57e1d54` | Firing fix batch 3: keep pending zone OFF across run start and retry unowned bits while... | REVIEW_FIREFX3_TOOLFX2_2026-10-10, REVIEW_SL3_R2ACE_2026-10-10 |
| `804f67e85` | LD-01/LD-02: autotune and profile start gates require the safety link positively up (re... | REVIEW_LD01_WWFIX_2026-10-10, REVIEW_SL3_R2ACE_2026-10-10 |
| `5d01d185e` | host tests: fix totalExpected after rebase merge (92) | REVIEW_WEB4_TESTS_2026-10-10 |
| `31779fc9f` | Host test: security_backend_web_auth vtable and bootstrap route over the real credentia... | REVIEW_WEB4_TESTS_2026-10-10 |
| `508b30f44` | host tests: ui_lcd_lock state machine with LVGL shim (round 2, R2-10) | REVIEW_WEB4_TESTS_2026-10-10 |
| `2a5f1d60c` | host tests: adaptive_tune commit_zone failure/persist-failed/writer-race paths (R2-9) | REVIEW_WEB4_TESTS_2026-10-10 |
| `b9a659737` | host tests: thermo_owner dispatch/slot/producer coverage; register endian test (R2-9, R... | REVIEW_WEB4_TESTS_2026-10-10 |
| `f5e7a6002` | host tests R2-D: wifi_prov_api bodies (add/forget/getters/cache/scan); endian test draft | REVIEW_WEB4_TESTS_2026-10-10 |
| `bd64bc01e` | Host tests: dashboard_autotune GET handlers and zone_aux_convert move_handler adapter | REVIEW_WEB4_TESTS_2026-10-10 |
| `4fac4ad91` | Web fix batch: editSeq guards on safety_config/zones/profiles/settings_display, setup_w... | REVIEW_WEB4_TESTS_2026-10-10 |
| `b908598ca` | live_profile: working-copy generation on GET/edit/decide (409 on stale), page adopts it... | REVIEW_WEB4_TESTS_2026-10-10 |
| `8ce01a41b` | web_review test: F5 test fails by assertion, not crash | REVIEW_LD01_WWFIX_2026-10-10 |
| `843f4e7d0` | wifi_prov test: gateway-equals-ip and broadcast-gateway assertions | REVIEW_LD01_WWFIX_2026-10-10 |
| `2cd02753c` | Wi-Fi/web review fixes F1-F5, F6, F8: tri-state saved_nets probe, refused-record hint, ... | LCD_UI_REVIEW_2026-10-10, REVIEW_LD01_WWFIX_2026-10-10 |
| `21b163662` | LCD: extract pure profile-builder segment seams (pad unit capture, captions) with host ... | LCD_UI_REVIEW_2026-10-10, REVIEW_LCD_PCTOOLS_CAMPAIGNS_2026-10-10 +1 |
| `dfd3a346f` | Review TGFIX: host-test list fails closed on lost registration; nits, S5 try_clear test... | REVIEW_SMALL_BATCH_2026-10-10 |
| `dad356035` | SaftyFW host test: pin heat-possible probe counting present current (LOW-1) | REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10, REVIEW_SMALL_BATCH_2026-10-10 |
| `fd1905824` | Submodule pin check: FAIL on bad URL/auth, bounded git, read .gitmodules from the commi... | REVIEW_SUBFX_2026-10-10 |
| `392da5b7f` | web: unsaved-changes beforeunload guard on profiles, setup_wizard, safety_config, kiln_... | REVIEW_D7GUARD_2026-10-10 |
| `1cd26d6d3` | Pooled firmware LOW batch: doc status marks, factory_reset fence scope test | REVIEW_FWB15_2026-10-10 |
| `ea86f4d43` | Pooled firmware LOW batch: strict kiln_cfg id, readiness cap buffers, kiln_nvs fence on... | LCD_UI_REVIEW_2026-10-10, REVIEW_FWB15_2026-10-10 |
| `7c4624cc3` | Stack ceiling profile_exec_wdt 2752; review doc: MED-A/MED-B fixed | REVIEW_FIRING_FIX2_2026-10-10 |
| `603f062c9` | Review fixes: fatal-reboot hold owes release frame, bounded watchdog pause, failed stat... | REVIEW_FIRING_FIX2_2026-10-10, REVIEW_FWB15_2026-10-10 |
| `1fe83614a` | Firing-path review fixes: guard 9 pending verdict carried to FAULTED, APP source before... | REVIEW_FIRING_FIX2_2026-10-10 |
| `06eb0b62c` | autotune host test: down link with SAFETY_LINK source latched refuses, names source, re... | this doc |
| `a433b9637` | persist review fixes: unreadable cfg file keeps NVS copy and refuses saves (M1), zones ... | REVIEW_PERSFX_2026-10-10 |
| `8711ebf7d` | Web pages: fix settings/backup/live-profile defects D1-D7 (sign-in cancel, reboot wordi... | REVIEW_WEBFIX_2026-10-10 |
| `3b2e8b732` | Test/tooling LOWs: S5 fault-bit guard tests (C3), host-test exit chain derived from Add... | REVIEW_TGFIX_2026-10-10 |
| `ccb7b5d26` | tools: check_submodule_pins_pushed (gitlink pin must exist on submodule remote); run fr... | REVIEW_SUBCHK_A1B1_2026-10-10, REVIEW_SUBFX_2026-10-10 |
| `979d05a63` | fix warn_prefix literal | REVIEW_SUBCHK_A1B1_2026-10-10 |
| `7cc343260` | LCD touch_test role exception keyed on calibration-saved flow (A1/A2); debug_program pi... | LCD_UI_REVIEW_2026-10-10, REVIEW_SUBCHK_A1B1_2026-10-10 +1 |
| `df2c8fe53` | autotune handler test: real 193-byte body for the content cap; docs for campaigns 7/8 | this doc |
| `251f9b2e3` | Host tests: autotune start/abort/accept HTTP handler matrix (campaign 8) | REVIEW_FIRING_FIX2_2026-10-10 |
| `fbb9ed468` | Fix check blind spot: ++ counts as safety_link_status producer; dns_hijack TASKS row + ... | REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10 |
| `f2e9f35a1` | adaptive_tune_revert: verify by re-reading status (FAILED/UNVERIFIED) | this doc |
| `f3a331550` | check_link_impl_isolation: allowlist test_link_task_fuzz.c by path (recording fake, kil... | this doc |
| `ba267a692` | Host tests round 2: ct_leak_alarm_service glue, discrete_task E-stop/mainFault loop, ki... | REVIEW_FIRING_FIX2_2026-10-10, REVIEW_TGFIX_2026-10-10 |
| `2ac609343` | Tooling batch: negtest default verdict patterns and -RequireAssertion, push_verify defa... | REVIEW_TOOLING_BATCH_2026-10-10 |
| `a9c3d5b08` | PcTools MCP coverage sweep: generic confirm-gate sweep, kiln_configs tool tests, audit doc | REVIEW_TOOLING_BATCH_2026-10-10 |
| `d26d2e4b1` | Web page tests: executing vm tests for backup restore, settings resets, kcUnit conversi... | this doc |
| `b3e65ca23` | uart_bridge test: watchdog re-arms the drop when the unowned mask changes | this doc |
| `a23a67daa` | SaftyFW host test: SET_PARAM range refusal and update command routing (campaign 2 exten... | this doc |
| `bbb25f5de` | Host test: uart_bridge guards, codecs, reply_reject and link watchdog (campaign 10) | REVIEW_FIRING_FIX2_2026-10-10 |
| `942cefb97` | Salvage: build-gate re-entrancy check (negtested), two SaftyFW gate-evidence rows upgra... | REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10 |
| `a62493c24` | Comments: omitted guard keeps stored value, claim-refusal scope, danger_mode indent (re... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `3408a12ad` | autotune accept: a ceiling write refused without writing is REFUSED_NOT_WRITTEN, not 'l... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `f7d4b08fb` | kiln_cfg_store: boot restore refused only on the run claim keeps the active id (review ... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `138975bee` | zones cfg_fs: unreadable (over-size/IO error) zones.json is cannot-decide, never absent... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `e5bdb8a3c` | backup_import fuzz: hostile shapes carry all three gains and have positive controls (re... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `c83c63584` | profiles: refuse save/delete/retarget writers for the whole pre-boot-load window (revie... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `f5c91ecde` | backup/zones/safety_link: scope topology override to owning task, resolve topology once... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `f81607868` | fix: register v3 migration tests | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `ea14c8fbd` | crash_report: migrate unacked v3 record on upgrade, retry late coredump capture; factor... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `1453133ce` | Allowlist relay_unknown_prelock_check fail-safe cut; capture danger_mode re-entry enabl... | REVIEW_DANGER_K7_FIX_2026-10-10, REVIEW_PERSIST_BATCHES_2026-10-10 |
| `2f8192dd2` | SaftyFW: test_link_task_fuzz uses trip_seq_next after helper move | REVIEW_PERSIST_BATCHES_2026-10-10, REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10 |
| `6e5e30b52` | Stack budgets: re-pin http_async_job 7712, bx_flash_worker 6240, system_uart_bridge 315... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `d70ee7816` | Bump mykicadMcp pointer: re-vendored mcpkit_registry.py | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `efab45a18` | run_all_checks: clarify 'not recorded' message under dev flow (stored main baseline sti... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `4ec7e3bf1` | test_danger_mode: use CHECK so the vacuity check sees its assertions | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `0a03e8b01` | backup_import: zone/timing candidate scratch via persist_scratch_alloc; drop allowlist ... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `cf3359523` | SaftyFW: trip_seq_next() in link-free trip_seq.h so safety_core.c no longer includes li... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `0248c33fe` | profile_executor: capture relay OFF write results (fail-safe pending + ESP_LOGE), test ... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `c41cffab6` | ramp_lock_onesided: mirror exec_elapsed_accumulate in step_schedule, bind it in the dri... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `8ee4a1723` | Review fixes: no classify timeout (late fatal DIAG still holds); trip seq bumped inside... | REVIEW_SAFETY_LINK_FIX2_2026-10-10 |
| `5fa28ed3b` | heat_enable/profile_executor: fatal Pico reboot holds the firing paused with K4 open (M... | LCD_UI_REVIEW_2026-10-10, REVIEW_SAFETY_LINK_FIX2_2026-10-10 +1 |
| `3ff645555` | safety_cfg_write: unconfirmed polarity still clears E-stop verification (MED-3); 5 s DI... | REVIEW_SAFETY_LINK_FIX2_2026-10-10, REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `ccbcb992e` | SaftyFW: trip snapshot is a real seqlock (review LOW-6); test pins link_task HEAT_OWNER... | REVIEW_SAFETY_LINK_FIX2_2026-10-10, REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `af1a9e26a` | SaftyFW host tests: thermo_task fault injection and watchdog_task deadline/feed/reset c... | REVIEW_LCD_PCTOOLS_CAMPAIGNS_2026-10-10 |
| `4af0b912f` | SaftyFW host tests: safety_core guard campaign and link_task fuzz campaign | REVIEW_LCD_PCTOOLS_CAMPAIGNS_2026-10-10 |
| `fbf6e1070` | Stack budget: re-pin kiln_io_owner (2160) and lvgl (7520) ceilings after K7 relay IO lo... | REVIEW_DANGER_K7_FIX_2026-10-10 |
| `bcf448e71` | KilnFW relay IO: fail OFF on init/reinit failure, relay_state_unknown fault, bounded lo... | REVIEW_DANGER_K7_FIX_2026-10-10 |
| `fd18342d6` | persist: fix campaign 10 findings K10-01..K10-14 (cfg read error channel, ct_verify sav... | REVIEW_PERSIST_BATCHES_2026-10-10 |
| `0e76d38b6` | PcTools batch D: batch C review LOW/NIT fixes, write-tool read-backs, negtest baseline-... | PCTOOLS_WRITE_TOOLS_REVIEW_2026-10-09, REVIEW_LCD_PCTOOLS_CAMPAIGNS_2026-10-10 +1 |
| `6d4cc7ab6` | danger mode: exclude autotune and profile start both ways, claim-first recheck, fail-cl... | REVIEW_DANGER_K7_FIX_2026-10-10 |
| `96b4e2ae4` | Host test: update_*_http handler gate refusals (campaign 9c) | REVIEW_LCD_PCTOOLS_CAMPAIGNS_2026-10-10 |
| `82bea6511` | wifi_prov: tag guard lines with finding ids for negative tests | REVIEW_WIFI_WEB_BATCHES_2026-10-10 |
| `52c19a8d0` | wifi: review 2026-10-09 fixes (DNS admission, saved-nets hardening, strict static IP, W... | REVIEW_WIFI_WEB_BATCHES_2026-10-10 |
| `110650ae0` | Recovery fix3 review N1-N4: type-agnostic wifi_nvs empty-namespace probe (hal_kv_key_ex... | REVIEW_WIFI_WEB_BATCHES_2026-10-10 |
| `9538a47b7` | web batch 2: forgot modal test uses real strength rule, add weak-password retry group; ... | REVIEW_WIFI_WEB_BATCHES_2026-10-10 |
| `af305abbf` | web batch 2: reset password precheck keeps token usable (W1), backoff test (W2), behavi... | REVIEW_WIFI_WEB_BATCHES_2026-10-10 |
| `3de432469` | LCD review R4-R7: builder pad converts back with the captured unit, card titles follow ... | LCD_UI_REVIEW_2026-10-10, REVIEW_LCD_PCTOOLS_CAMPAIGNS_2026-10-10 |
| `479f3a473` | Stack-fix review follow-ups: boot profile scratch OOM host tests, double-OOM log, analy... | REVIEW_STACK_FOLLOWUP_2026-10-10 |
| `8fac7a103` | Host test: OTA HTTP handler refusal and failure paths (campaign 9b) | this doc |
| `2d469f412` | SaftyFW config_store: note v1-branch clamp is an unobservable backstop | REVIEW_SAFTYFW_GUARD_FIX3_2026-10-10 |
| `e6877a521` | SaftyFW volatile gate: judge abs_max/max_rate by guard-runs direction, compare k_ct, re... | REVIEW_SAFTYFW_GUARD_FIX3_2026-10-10 |
| `ecfd6bbb4` | build_host_tests: resolve committed conflict markers; expect 81 executables (campaign 10) | REVIEW_FWBATCH13_2026-10-10, REVIEW_WEB_BATCH_2026-10-10 |
| `6a5a06c01` | stack budget: re-pin profile_exec_wdt 2704 -> 2720 after guard 9 pre-lock path (firing ... | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `19fa986a8` | tests: pin zone OFF retry wiring and NaN-safe duty clamp in source; strengthen negtest ... | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `4106c8eab` | profile_executor: zone relay pending-OFF mask retried in every non-RUNNING tick; unread... | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `74b7bd9dd` | profile_executor: log failed heat_enable_acquire_since at start and resume | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `9929fbc0a` | thermal_guard: guard 1 climbing window floor 120 s with no model (only lengthens window... | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `3f39e0dac` | profile_executor: drop second heat_enable_reconcile in lock-timeout branch (must stay l... | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `fa1ebe421` | heater_output: NaN duty renders OFF (firing review item 5) | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `a56e51e0e` | profile_executor: accumulate total/segment elapsed in ms with a sub-second carry (firin... | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `b5502c191` | profile_executor: run thermal_guard_tick before apply_relay so a trip never writes ON t... | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `6c7b9eddb` | guard 9: test control-tick staleness and cut relays before taking s_exec.lock; bounded ... | REVIEW_FIRING_PATH_FIX_2026-10-10 |
| `6c341bb33` | Auth reset: check password strength before consuming token; recv-spin cap in TOTP fuzz ... | REVIEW_WEB_BATCH_2026-10-10 |
| `56979d766` | zones config: resolve (the .bad writer) runs under the zones save lock (review 11 LOW-1... | REVIEW_FWBATCH13_2026-10-10 |
| `8fdfee2bd` | kiln_nvs writers fenced after factory-reset: hal_kv mutation-fence hook installed by ma... | REVIEW_FWBATCH13_2026-10-10 |
| `2f8625f12` | autotune start refusals: size buffers to READINESS_GATE_MSG_CAP so the full readiness t... | REVIEW_FWBATCH13_2026-10-10 |
| `0daf13619` | HTTP parsers: refuse invalid percent escapes, plus-signed/whitespace ids, ERANGE ids (F... | REVIEW_FWBATCH13_2026-10-10 |
| `0cb07df6e` | backup import: refuse truncated/duplicate-key/wrong-typed documents; whole pass-1 parse... | REVIEW_FWBATCH13_2026-10-10 |
| `5abfe307d` | stack budget ceilings: re-pin profile_exec_wdt and safety_core after safety link review... | REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `2fef31929` | safety link review F3 follow-up: narrow safety_link_get_diag_flags() keeps the status s... | REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `bc52e1517` | SaftyFW: release/acquire barriers around trip-event publication (safety link review F5) | REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `4ab08d964` | SaftyFW: trip-event seq wraps 255 -> 1, never 0 (safety link review F4) | REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `f58560ed7` | safety cfg write: persistent commit read-back also requires DIAG volatile-dirty clear (... | REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `e327e0e20` | SaftyFW: drop inherited heat grant when a new ESP session claims no heat ownership (saf... | REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `5c7c6a451` | heat_enable: reconcile the held grant against the Pico's reported K4 state (safety link... | REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `fb931e6f8` | docs: campaign 10 findings (persist parsers and stores) | REVIEW_SAFETY_LINK_FIX_2026-10-10 |
| `4a8c7dc35` | kiln_io_sx_fake: assert reinit read-back catches a stuck-high relay latch | REVIEW_K7_RELAY_IO_FIX_2026-10-10 |
| `501bb0d35` | kiln_io: fix relay IO defects K7-01..K7-04 (chip read-back resync, verified re-init aft... | REVIEW_K7_RELAY_IO_FIX_2026-10-10 |
| `7fba785cc` | Host tests: campaign 8 handler matrix for aux outputs and profile start/stop/pause | this doc |
| `2549d8bf0` | tests: safety_clear_trip tests mock a received DIAG frame (precheck refuses when none) | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `beac10dec` | tests: tighten fixture/debug-write/wifi gate tests so equivalent-looking mutants are ca... | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `e685436df` | bench_test FL-10/FL-11 opt-in test (review LOW-1) | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `c769c8a6a` | read-backs fail loud: fixture relay missing, profile_live_decide working_id, pico_gpio_... | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `3f4e12260` | wifi writes: fail closed on unreadable executor; wifi_forget unreadable read-back is FA... | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `3c3e12d9f` | debug tools: fail closed on unreadable executor, cover autotune, guard step and leave_h... | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `6279e8a61` | bench_test FL-10/FL-11: allow_flash must be exactly True (review LOW-1) | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `19c22806d` | flash_firmware: latched safety trip in link-down mode is a note, not a hazard (review M... | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `6bf6d34b9` | flash_firmware: latched trip in link-down mode is a note, not a hazard (review MED-1) | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `1c1bacee2` | PcTools deferred: safety commissioning read-back failure reports state UNKNOWN | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `f3aa6e50c` | PcTools deferred: expander_* writers need confirm and idle run state; kiln_config_apply... | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `a55afd94c` | PcTools deferred: backup_import transport wording, crash_report_clear timeout wording, ... | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `6f90cd82c` | PcTools batch A review fixes MED-3, LOW-4..11, INFO-12: per-zone preset progress, clear... | REVIEW_TOOLING_BATCH_C_2026-10-10 |
| `cc62edf2f` | LCD review fixes: N1 touch_cal exits and profile Start gated, N2 danger-mode start refu... | LCD_UI_REVIEW_2026-10-10, REVIEW_LCD_FIX_2026-10-10 |
| `ddf820fdb` | build_host_tests: expect 75 executables (diagnostics_http added) | this doc |
| `793e72065` | Host test: compile diagnostics_http.c against stubs (crash report, estop, coredump, rel... | this doc |
| `163980001` | SaftyFW guard fixes review: fail-closed volatile install gate while heat possible, tc_o... | REVIEW_SAFTYFW_GUARD_FIX2_2026-10-10.md |
| `546cc2878` | SaftyFW test: assert the volatile install backfills ct_topology | REVIEW_SAFTYFW_GUARD_FIX2_2026-10-10 |
| `9c9b2245b` | SaftyFW tests: fields_set-only change and F6 S2/S10 hold coverage (review LOW-3) | REVIEW_SAFTYFW_GUARD_FIX2_2026-10-10 |
| `c815092ff` | SaftyFW: clamp an out-of-bound stored tc_offset_c at load instead of rejecting the slot... | REVIEW_SAFTYFW_GUARD_FIX2_2026-10-10 |
| `20da358a0` | SaftyFW: fail-closed volatile config gate keyed on heat-possible (review HIGH-1, MED-1,... | REVIEW_SAFTYFW_GUARD_FIX2_2026-10-10 |
| `c1f97a3fe` | build_host_tests: expected executable count 77 (danger_mode added; origin/dev already b... | this doc |
| `e9c574639` | Host tests: safety_link payload golden layouts (campaign 5), danger_mode lifecycle with... | this doc |
| `e788b153e` | Recovery fix batch 2 review findings F1-F7: not-applicable boot_guard reply, empty lega... | REVIEW_RECOVERY_FIX3_2026-10-10 |
| `806f9c7b5` | thermo bridge gate: no reason buffer (stack budget) | this doc |
| `19217cb0c` | host tests: expect 74 executables (thermo gate test) | this doc |
| `0e929d123` | Fuzz part 3: over-cap delete body test, findings doc coverage | this doc |
| `2e9f6288f` | Host tests: HTTP body fuzz part 3 (login, live decide/accept, profile delete/favorite/h... | this doc |
| `532b0c27d` | Review 12 Part B: gate THERMO UART writers (config_channel, thresholds, cj offset, clea... | DEV_FIRMWARE_REVIEW_12_2026-10-09 |
| `4b731a6a7` | Review 12 LOW-5: tests for persisted used bitmap and equal-rev identical-bytes retire | this doc |
| `af93e8232` | Review 12 LOW-1/2/4, INFO-4/5: shared nvs_save rollback-journal gate, uncleared-journal... | this doc |
| `6a6e0aeca` | build_host_tests: expected executable count 74 | this doc |
| `8419de36e` | tests: kiln_io + SX1509 against a fake I2C expander (campaign 7); findings K7-01..04 | this doc |
| `854bc247c` | Fix two standing check failures on origin/dev | this doc |
| `91eb1f0ce` | Fix 3 dev pytest failures: zone writer happy-path mocks use a real form body (and prove... | REVIEW_TOOLING_BATCH_C_2026-10-10, STACK_FIX_BATCH_REVIEW_2026-10-09 |
| `6ecf90f6b` | stack budgets: re-baseline executor 3920->3408 and httpd 4496->4448 after the worker-on... | STACK_FIX_BATCH_REVIEW_2026-10-09 |
| `794fce57b` | profiles_http: boot-path profile_t scratch on the heap (check_main_task_stack_budget 63... | STACK_FIX_BATCH_REVIEW_2026-10-09 |
| `650f6159e` | stack analyser review fixes: drop worker-only autosave edge, long-call edges only to fu... | STACK_FIX_BATCH_REVIEW_2026-10-09 |
| `3b2905d84` | recovery check: mutant needles independent of line endings | REVIEW_RECOVERY_FIX2_2026-10-09 |
| `959aee923` | recovery: exit/boot_guard_reset use not-applicable-aware clear (R2-L1); Wi-Fi reset era... | REVIEW_RECOVERY_FIX2_2026-10-09 |
| `a2d6d6c32` | SaftyFW guard review: target-build fix (forward decl), docs, mark F1-F7 fixed | REVIEW_SAFTYFW_GUARD_FIXES_2026-10-09 |
| `d53125fec` | SaftyFW guard review fixes F1-F7 (WIP, docs follow) | REVIEW_SAFTYFW_GUARD_FIX2_2026-10-10, REVIEW_SAFTYFW_GUARD_FIXES_2026-10-09 |
| `f57d29f74` | Port abandoned work: bootloader build on light gate lane, refused-accept autotune asser... | this doc |
| `382c3936d` | web: zones Save timer pauses for login modal and re-arms for retry, maybe-saved text on... | REVIEW_TOOLINGB_WEBAUTH_2026-10-09 |
| `f333e9e7b` | login page: inline host-refusal mapper (app.js never loaded there), behavioral test; li... | REVIEW_TOOLINGB_WEBAUTH_2026-10-09 |
| `20e1263f9` | http_form: cap-0 decode writes nothing, parse_float refuses hex; zones numeric fields r... | REVIEW_TOOLINGB_WEBAUTH_2026-10-09 |
| `0c2846fb3` | PcTools board-writing tools part B: gate flags strict bools, flash_firmware confirm and... | REVIEW_MCPFX1_R3SFW_2026-10-10, REVIEW_TOOLINGB_WEBAUTH_2026-10-09 |
| `bb92ec5d4` | PcTools write tools: ui_run_script gate, preset partial reporting, zone omit-preserved ... | PCTOOLS_BATCH_A_REVIEW_2026-10-09 |
| `2a1d69a84` | Fix KilnFW target build: shorten zone-topology refusal to fit err_msg (format-truncatio... | DEV_FIRMWARE_REVIEW_15_2026-10-09, STACK_FIX_BATCH_REVIEW_2026-10-09 |
| `5dcd6dbd2` | tests: HTTP fuzz part 2 (backup import, aux POST, profile POST, kiln_cfg apply) | DEV_FIRMWARE_REVIEW_15_2026-10-09 |
| `473f597eb` | danger_mode: revert stack to 3072 B (measured 800 B static lower bound, 780 B live); re... | DEV_FIRMWARE_REVIEW_15_2026-10-09 |
| `fa4a62ef3` | zones: never disable guard 8 by blank/missing xzone; do not overwrite newer zones.json;... | DEV_FIRMWARE_REVIEW_15_2026-10-09 |
| `ffd506904` | Review 9 fix review F1-F6: atomic model+gains write, revert snapshot validity, claim-ga... | DEV_FIRMWARE_REVIEW_15_2026-10-09 |
| `013ff420d` | Suite fixes: HAL allowlist for wifi_provision_http esp_netif.h; refresh wifi_provision_... | DEV_FIRMWARE_REVIEW_15_2026-10-09 |
| `65851a470` | esp_netif stub: esp_netif_set_hostname | DEV_FIRMWARE_REVIEW_14_2026-10-09 |
| `4805274f1` | Review LOW-1..9 (host rule/linkcov): DHCP hostname, trailing-dot and IPv6 literal check... | DEV_FIRMWARE_REVIEW_14_2026-10-09 |
| `2cb20eef2` | PcTools: stale-image crash banner, preflight note, find_crash_elf by dump elf sha | DEV_FIRMWARE_REVIEW_14_2026-10-09 |
| `d06bf7841` | crash_report: stamp coredump's own image identity; foreign dump is not attributed to ru... | DEV_FIRMWARE_REVIEW_14_2026-10-09, REVIEW_PERSIST_BATCHES_2026-10-10 +1 |
| `3cb3fb94a` | backup: format v7 carries thermo_count/relay_count; import onto an empty board sets top... | DEV_FIRMWARE_REVIEW_14_2026-10-09, DEV_STACK_ANALYSER_REVIEW_2026-10-09 +1 |
| `04993bcdb` | tests: HTTP parser fuzz (http_form helpers, zones POST fields); findings doc | this doc |
| `994c14d11` | factory reset: kiln/all scopes clear the crash report and coredump via crash_report_cle... | DEV_FIRMWARE_REVIEW_14_2026-10-09 |
| `6f4fa7d0c` | Pico announce review fixes: zero stale ESP version on first context under another boot_... | DEV_FIRMWARE_REVIEW_14_2026-10-09 |
| `fdf8153ae` | totp reset/forgot: zero secrets on every early return; scan results to PSRAM | DEV_FIRMWARE_REVIEW_14_2026-10-09, DEV_PSRAM_TOTP_REVIEW_2026-10-09 |
| `04848cd95` | Factory-reset fence: cfg_fs_write_atomic refuses under the reset mark (kiln_cfg_store, ... | DEV_RSTFENCE_OTAFIX_REVIEW_2026-10-09 |
| `bc334d45c` | PcTools: backup-first on every factory reset path (action, preset tool, GUI), WIPED mes... | DEV_REVIEW_13_2026-10-09 |
| `462eb8385` | Review 10 LOW-1/2/3: refuse zones save while a kept rollback journal would revert it; f... | DEV_FIRMWARE_REVIEW_12_2026-10-09 |
| `65ac06506` | profiles: retire superseded legacy NVS blob when the higher-rev cfg file is adopted (be... | DEV_FIRMWARE_REVIEW_12_2026-10-09 |
| `3b3b9dcec` | httpd stack: shrink profile_exec/autotune start path 5248 -> 4496 B (JSON refusal buffe... | DEV_REVIEW_13_2026-10-09 |
| `f1cb36042` | backup import: refuse zone entries for zones the board lacks in pass 1 (400), not a 500... | DEV_REVIEW_13_2026-10-09 |
| `2b9b8e73a` | safety link: log Pico boot_id change (old and new) once per change | DEV_REVIEW_13_2026-10-09 |
| `753f40f68` | Fix dev web-fix review: blank guard/xzone keeps stored value, run-queue in-place rewrit... | DEV_FIRMWARE_REVIEW_12_2026-10-09 |
| `ab210508d` | zones config: keep a rejected zones.json as zones.json.bad, latch a cfg-file load fault... | DEV_FIRMWARE_REVIEW_11_2026-10-09 |
| `f8860c92e` | totp http: fix forgot handler stack-buffer zeroing, reset handler read-failure zero+fre... | DEV_PSRAM_TOTP_REVIEW_2026-10-09 |
| `e76e582b1` | Move web-auth tables, touch groups, wifi scan buffers to PSRAM (EXT_RAM_BSS_ATTR); audi... | DEV_PSRAM_TOTP_REVIEW_2026-10-09 |
| `0aec8e7c4` | Web: host-refusal guidance on login/forgot/reset/stage; zones blank guard refusal + sav... | DEV_FIRMWARE_REVIEW_11_2026-10-09, DEV_WEB_REVIEW_2026-10-09 |
| `ea9aea5a7` | profiles_http test: gen_begin hook proves the bracket opens before the RAM assign (host... | DEV_FIRMWARE_REVIEW_11_2026-10-09, DEV_HOSTFIX_REVIEW_2026-10-09 |
| `5e03dbc09` | Captive 302: absolute AP URL only for requests that arrived on the AP; log netif fallba... | DEV_FIRMWARE_REVIEW_11_2026-10-09 |
| `e0a038d6a` | KilnFW safety link: bounded DIAG-driven re-announce (max 3, 2 s apart) when a v17+ Pico... | DEV_PICO_ANNOUNCE_REVIEW_2026-10-09 |
| `937af9cc7` | MED-2/LOW-4/LOW-5 (rstfence review): swap fault kind buffer 24->32 with static assert a... | DEV_FIRMWARE_REVIEW_10_2026-10-09, STACK_FIX_BATCH_REVIEW_2026-10-09 |
| `f354981b7` | update fetch: heap precheck 29556 B (scratch 1024, margin 3840) so an idle board is adm... | DEV_FIRMWARE_REVIEW_10_2026-10-09 |
| `e6a0ff346` | SaftyFW link: group ANNOUNCE state in link_peer_announce_t with pure record/clear helpe... | DEV_PICO_ANNOUNCE_REVIEW_2026-10-09, DEV_WEBFIX_REVIEW_2026-10-09 |
| `2ca124f58` | test_bench_test_admin_auth: scripts the idle gate reads and the INCONCLUSIVE first-read... | this doc |
| `f8dffd8db` | http: Host allow-list accepts only bare name, .local and fixed LAN suffixes (reopened F... | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_HOSTRULE_LINKCOV_REVIEW_2026-10-09 |
| `7452e63b7` | profiles: refuse starts until boot load done, bracket boot load writes; drop dead s_slo... | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_HOSTRULE_LINKCOV_REVIEW_2026-10-09 |
| `b813b027c` | Dev review 9 L1: host test for Accept landing between adaptive plan and apply | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `4e3282fb6` | Dev review 9 L1/L2: Accept vs adaptive run-end apply; heat-claim re-check inside zones ... | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `762a1f2a3` | test_setup_wizard: follow shortened no-CT step note | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_WEBFIX_REVIEW_2026-10-09 |
| `aba69118e` | zones POST: blank guard/xzone fields mean 0, blank pc_link keeps stored value; pages om... | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_WEBFIX_REVIEW_2026-10-09 |
| `b744de755` | setup progress: long note truncates (UTF-8 safe) instead of 400; reset body cap 512 on ... | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_WEBFIX_REVIEW_2026-10-09 |
| `c094c089a` | stack analyser: LongCallTracker keeps union of l32r literals and taints on non-l32r wri... | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_STACK_ANALYSER_REVIEW_2026-10-09 +1 |
| `696e981ff` | check_aux_relay_conflict_sites: follow zones_post_apply_body (review MED-1) | SAFTYFW_8838DEA7_REVIEW_2026-10-09 |
| `fe5ac57f2` | SaftyFW: keep peer version when context boot_id matches the announced one (8838dea7 rev... | SAFTYFW_8838DEA7_REVIEW_2026-10-09 |
| `3ade435b6` | POST /api/profile: accept built-in id explicitly as copy-to-first-free; tests | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_WEBFIX_REVIEW_2026-10-09 |
| `cdc44d2e3` | profiles test: slot generation bracket coverage for edit, retarget commit/revert, delete | DEV_HOSTFIX_REVIEW_2026-10-09 |
| `2d1f637d2` | Host allow-list: accept board hostname first label, absolute captive redirect, refusal ... | DEV_FIRMWARE_REVIEW_15_2026-10-09, DEV_HOSTFIX_REVIEW_2026-10-09 |
| `c23ebb76d` | PcTools review fixes: per-field zone model tolerance, apply_staged polls to deadline, a... | DEV_RSTFENCE_OTAFIX_REVIEW_2026-10-09 |
| `f293f196d` | tests: refused time_sync/favorites sets leave live state untouched | DEV_RSTFENCE_OTAFIX_REVIEW_2026-10-09, SAFTYFW_8838DEA7_REVIEW_2026-10-09 |
| `be451c323` | test: clear stale boot fault before ESP_DONE fallback section | DEV_RSTFENCE_OTAFIX_REVIEW_2026-10-09 |
| `65276a7b5` | kiln_cfg_swap: distinct rollback_active_id_unsaved fault (ROLLED BACK banner/LCD), latc... | DEV_RSTFENCE_OTAFIX_REVIEW |
| `8838dea73` | SaftyFW: boot_id change forgets peer protocol version; tested push_context session trig... | SAFTYFW_8838DEA7_REVIEW_2026-10-09 |
| `dd46bc6da` | profiles test: seqlock generation tests (in-flight odd, failed save) | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09 |
| `6f1545601` | profiles: per-slot seqlock generation around every RAM assign so a start cannot pass th... | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09 |
| `a69ff8fb0` | http: status_get json and POST /api/zones zones_cfg_t scratch on the heap (HTTP input a... | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09 |
| `cd90a1255` | test: isolate reannounce request from boot-id change | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09 |
| `cb4ffda77` | safety link: clear DIAG uptime baseline with pico_boot_id_known on the same link-down c... | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09 |
| `63b808e83` | tests: per-writer reset-mark refusal for zones, profiles, favorites, time_sync | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09, DEV_RSTFENCE_OTAFIX_REVIEW_2026-10-09 |
| `c2aa85e05` | cfg_fs_mount: own write-refuse hook so executables without pref_cfg_fs.c link | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09, DEV_RSTFENCE_OTAFIX_REVIEW_2026-10-09 |
| `b1020e56a` | factory reset: cfg save lock barrier plus refusals under each save lock, backup restore... | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09, DEV_RSTFENCE_OTAFIX_REVIEW_2026-10-09 |
| `c6e704596` | WIP reset writer fence | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09, DEV_RSTFENCE_OTAFIX_REVIEW_2026-10-09 |
| `2c0ca0437` | ota_page: merge split comment that killed the main inline script; page lint also parses... | DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09, DEV_RSTFENCE_OTAFIX_REVIEW_2026-10-09 |
| `5c046ea11` | profiles: start refuses a slot deleted and re-saved after the copy (L23 residual): publ... | DEV_FIRMWARE_REVIEW_9_2026-10-09, DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09 |
| `47fc83f1f` | Review LOWs: kept swap journal latches visible fault; unit_pref adopts file value after... | DEV_FIRMWARE_REVIEW_9_2026-10-09, DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09 |
| `b475e7d7d` | safety link v17 review LOW-1/2/4: reboot forgets cached DIAG trip_seq; boot clear waits... | DEV_FIRMWARE_REVIEW_9_2026-10-09, DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09 |
| `cececc45b` | Restore work dropped by 0a1cdc7d; keep only its stack-budget changes | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `28adc35bb` | check_config_migration_steps: find AUX_OUTPUTS_CFG_VERSION in the header (moved from .c... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `0a1cdc7dc` | stack budget: follow l32r+callx long calls, declared flash-worker autosave edge, re-bas... | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_FIRMWARE_REVIEW_9_2026-10-09 +3 |
| `3bc27318a` | zones probe-OOM test: drop ok-called check | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `c8214cae9` | zones: test probe-OOM 503 | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `b99529961` | HTTP LOWs from 0744e4bf review: custom_err 503, stage-specific too-slow text, recovery ... | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `fffa15041` | profiles: delete-in-flight mark closes delete vs start race (HTTP audit L23) | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `6fbbb28b7` | kiln_cfg_swap: rollback keeps the journal when the active_id restore failed; boot calle... | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `d7d11578b` | factory_reset: refuse sweep/restore, guard writers, reboot-failure path (ffcea431 review) | DEV_FIRMWARE_REVIEW_10_2026-10-09, DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `3ff453867` | unit_pref_set: persist first, publish to RAM only on success (HTTP input audit L12) | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `8b65ea9a1` | LCD audit L29/L30: chart ticks/legend content-local, rail write-on-change, zone name un... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `68d349420` | Suite fixes: path-drift check ignores tmp-rooted Join-Path; refresh stale blob citations | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `0744e4bf5` | Upload-too-slow 503: set status line by hand (no 503 httpd_err_code_t in IDF) | HTTP_INPUT_PARSING_AUDIT_2026-10-09 (follow-up: Opus review of 0744e4bf) |
| `f9995a76c` | persist scratch check: cover zones_http_post_parse, wifi_provision_http, kiln_cfg_http | this doc |
| `473c2ac77` | HTTP low fixes: upload deadline 503 not 408, upload_too_slow UI text and PC client mess... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `5ade853b3` | SaftyFW test: pin the M4 clear-trip occurrence gate in safety_core.c and link_task.c (s... | REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10 (diff range) |
| `c2b151b51` | kilnlink 17: bind CLEAR_TRIP to the latched trip occurrence (audit M4) | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `bcc75d61b` | kilnlink M1: Pico boot_id from pico_rand; ESP also detects a Pico reboot by DIAG uptime... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `59b0d6afe` | SaftyFW: S6b liveness refresh only for ESP->Pico frames (kilnlink audit L1) | REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10 (diff range) |
| `50d2826ba` | SaftyFW: refuse non-positive/absurd ct_cal gain and absurd offset (kilnlink audit L2) | REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10 (diff range) |
| `106dcc3dd` | kiln_cfg_swap: fix Opus review LOWs on 7f10efe3 (LOW-1..7) | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `95d857930` | cfgfs test: aux stub accepts newer version so only the POST wrapper refuses it | this doc |
| `db395fa79` | cfgfs POST rules: distinct per-row test stubs, refuse newer aux_out.dat, drop unreachab... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `2014cde8c` | Flash worker review follow-ups: document retarget_commit worker hold, fix stale comment... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `2fcd20c11` | executor stack budget: hide on-worker autosave from the static path; heap the coupled-s... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `9cc5ed06b` | flash worker: reserve save sections before the worker starts; worker-side host test | FLASH_WORKER_LOCK_INVERSION_AUDIT (follow-ups) |
| `2086e1dec` | Strict form follow-ups: wizard step 11 sends full set_policy, blank autotune duty omits... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `953903c91` | http_form: leave out empty on every decode refusal; zones POST globals refuse -2 | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `d85e9e985` | cfgfs file POST: validate every flat pref file with a load-path validator (raw=1 no lon... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `4115bc192` | update_fetch: writer WR_ABORT owner-scoped; missing completion give after timeout insta... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `e51f9402b` | HTTP audit LOWs: login body cap/heap/fail-closed lock, upload deadlines, stage gate cle... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `ffcea4314` | factory reset: refuse when a firing/autotune starts during dispatch (reset-in-flight mark) | HTTP_INPUT_PARSING_AUDIT_2026-10-09 (L37 follow-up: review of ffcea431) |
| `6ce51540c` | tests: strict form parsing coverage -- zones POST empty/over-long optional keys, iter_t... | this doc |
| `b51dfc593` | update_fetch: free a post-wedge job's buffers; only the wedging job's are abandoned | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `f6f3608bc` | Dev-flow tests: concurrent-promote case, wt_status mid-rebase and cwd-only cases; mark ... | this doc |
| `7f10efe35` | kiln_cfg_swap: check marker and active-id saves, verify swap record by read-back (audit... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `60d372811` | Host-test runners: per-executable timeout (default 300 s, -TestTimeoutSec), tree-kill o... | this doc |
| `d971fb4c4` | run_all_checks: launch children -NonInteractive with stdin at EOF, per-phase timeouts; ... | this doc |
| `e7c982092` | http: strict input parsing -- refuse NUL in form values, end-pointer/range checks on id... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `fdec416ae` | KilnFW host test for update_fetch.c and update_http.c; fix two bugs it exposed | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `0e04c0a9e` | SaftyFW link: stage SET_PARAM as edits; drop them on a new ESP session (kilnlink audit ... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `0795908c7` | SaftyFW config_store: skip byte-identical writes (kilnlink audit L3) | REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10 (diff range) |
| `a517d2923` | zones/pid: refuse %00 in body, read thermo_count under the zones lock (audit L3/A5) | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `ada683808` | Fix four suite failures: re-vendor mcpkit registry, web-commission check follows sweep ... | this doc |
| `edd9a73ef` | checks: run child scripts -NonInteractive so a stray prompt fails instead of hanging | this doc |
| `4d79ff1af` | Current sweep: surface failed ESP-side CT provenance saves as esp_persist_failed (audit... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `aa16ae248` | Persist-result audit L1/L3/L5/L7/L8/L9: retry after failed save writes, firing history ... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `647e7d9d0` | cfgfs file POST: validate body with the loader (zones.json), refuse unknown names witho... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `5c6384238` | zones POST: refuse a commit when a firing/autotune started during the ceiling raise (HT... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `cdbc6fe5c` | backup import tests: cover kp ceiling, clear stale header-refusal message | this doc |
| `02ddc9443` | backup import: pass-1 range checks for zone gains/tuning/model_fit, fail-closed Dry-Run... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `595bd701c` | HTTP input L4/L7/L15/L26/L35/L43/L44: strict sim swap and diag numerics, wifi NUL refus... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `74bf36ec6` | LCD: crash-ack and profile Delete PIN success callbacks run the arm step (no second tap... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `31bba2aac` | commit_guard: F17 comma arrays, binary numstat, deleted paths; add check_push_verify an... | this doc |
| `6856e4dc6` | bootstrap_password: loop httpd_req_recv to content_len, wipe body/password on every exi... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `b6dbcf410` | SaftyFW update metadata write: erased-slot check and byte-for-byte read-back (audit L4) | REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10 (diff range) |
| `047844c5d` | bench_test: LCD-19 never trusts an unverified pre-existing admin PIN (audit 2 L6 residual) | this doc |
| `0b6c0c052` | Dev-flow scripts review fixes F1-F3, F6-F12: land main gate, tree-bound check logs, dev... | this doc |
| `f9b100c39` | relay_cycles_init: load into locals, hold s_rc.lock only to publish (flash-worker lock-... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `0968a6991` | LCD chart: index history by elapsed_s, one locked pass, dashed plan flag, axis follows ... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `207acff7d` | tests: strengthen L4/L8 audit tests | this doc |
| `55cfee20e` | bench_test: web judge contract audit 2 fixes (M1, M2, L1-L10) with tests | this doc |
| `b0892ef42` | test_backup_import: gap 7 remainder (candidate OOM, err prefix truncation, live thermo_... | this doc |
| `a81ebe7fb` | test_web_xss_fixes: isolate loginReturnPath URL origin re-check (negtest CAUGHT) | this doc |
| `3e4b5ef6e` | CSRF origin wiring check: checkcache marker, gate evidence row (negtest: 2 mutations CA... | this doc |
| `8ddb7a88c` | LCD network manage: forget confirm body 96->128 B (fixes -Werror=format-truncation from... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `8f81eb27f` | Restore zones 409 Pico-ceiling restore (gap 3) reverted by ef99c327 | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `697d10274` | http: Host-header allowlist (DNS rebinding), shared control-char JSON escaper, name wri... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `15f4edd3d` | adaptive_tune: interlock revert with run_end's unlocked apply; per-zone ki_clear_gen | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `48e1ba8ac` | executor/autotune: dispatch flash-worker persists after the lock is given (lock-inversi... | FLASH_WORKER_LOCK_INVERSION_AUDIT (follow-ups) |
| `d0c7cdefe` | ui_content_smoke: 60s CDP timeout, tab retry, Chrome relaunch, harness errors FAIL not ... | this doc |
| `60c02217e` | LCD audit L12/L13/L15/L18/L19/L20/L21: touch-group cap 32 with loud overflow, paged bui... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `7d27efd10` | MCP confirm gates: strict confirm on factory_default/load_config_preset/safety flash wr... | this doc |
| `762afa350` | negtest: -FindBase64/-ReplaceBase64 carry quotes past PS 5.1 arg stripping; document qu... | this doc |
| `c15e20f16` | Host-test gaps 4-7: assert legacy erase commit is not swallowed; mark gaps addressed in... | this doc |
| `f2854d5c9` | Host-test gaps 4-7: wifi legacy migration failures, CSRF pre-handler wiring, backup pre... | this doc |
| `b6f3cc458` | Negative-test web judge audit fixes and XSS regression test; add AT-01 active-state tes... | this doc |
| `9651bfeb8` | adaptive_tune: run_end applies zones setters outside adaptive_tune_lock (flash-worker a... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `a98b1dce8` | test_buildgate: max-hold sibling test waits for the kill instead of a fixed sleep; soft... | this doc |
| `4271767d6` | Save mutex vs flash worker: reserve the worker before every save lock | FLASH_WORKER_LOCK_INVERSION_AUDIT (follow-ups) |
| `ef99c3275` | LCD UI audit L1-L6, L9-L11, L14, L16, L17, L22: admin gates, Network Manage paging, mas... | DEV_FIRMWARE_REVIEW_9_2026-10-09, LCD_UI_REVIEW_2026-10-09 |
| `3047beb65` | zones POST: lost-update 409 lowers the Pico ceiling back to the live zone max (host-tes... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `82f1dafcb` | Web pages: escape names/state strings at audited innerHTML sinks (XSS audit F1,F2,F3,F8... | DEV_FIRMWARE_REVIEW_10_2026-10-09 |
| `b01e14a45` | bench_test web judges: fix contract audit H1, M1-M4, L1-L12 with fake-board tests | this doc |
| `b8b8c695e` | Dev-flow scripts review F4/F5/F13-F16/F18: safe worktree remove/prune, atomic release push | this doc |
| `86fc229a9` | Review 10 LOW-1..6: restore mangled evidence rows and make the table check reject split... | this doc |
| `2cb5499b3` | recovery image: clear boot_guard before erasing/selecting app (M1/M2), READONLY-first e... | DEV_FIRMWARE_REVIEW_10_2026-10-09 |
| `11a86b33d` | Repoint OTA_SINGLE_SLOT, ON_OFF_ZONE, CT_ATTRIBUTION_VERIFICATION references to the his... | this doc |
| `f36fed0eb` | KilnFW host test: drive aux fault-drop through executor task tick (gap 2); mark audit g... | this doc |
| `725559930` | OTA single slot: drop USB-serial last-resort path (owner 2026-10-09); keep in-app RECOV... | DEV_FIRMWARE_REVIEW_10_2026-10-09.md |
| `7059c2586` | bench_test: fix writing-judge review findings (LCD-10/11/12, OT-E11) and dev review 9 i... | this doc |
| `9d7431a2a` | docs: lean RELEASE_HARDENING, PICO_AUTO_UPDATE, LIVE_PROFILE_EDIT plans to pending-only... | DEV_REVIEW_10_2026-10-09 |
| `69afffd73` | SaftyFW review LOW fixes: v1 provenance comment, torn-data-byte CRC test, fuzz seed hin... | DEV_REVIEW_10_2026-10-09 |
| `39f4d8ce0` | Plan docs lean: split history of FILESYSTEM, CONFIG_MIGRATION_CHAIN, ITER_TUNE_REDESIGN... | DEV_REVIEW_10_2026-10-09 |
| `e265386a7` | docs: SYSTEM_MODE_GATE_PLAN.md -> SYSTEM_MODE_GATE.md (fully implemented); fix all refe... | DEV_REVIEW_10_2026-10-09 |
| `dbd1dfa93` | web-commission: profile sweep honors deadline before each icacls/rm, ages by newest inn... | DEV_REVIEW_10_2026-10-09 |
| `94f6de3b2` | bench_test: guard that every srv.<tool> a case calls exists on mcp_server; plan guards ... | DEV_REVIEW_10_2026-10-09 |
| `121d96046` | WEB judges: gzip decode failure is a transport error (INCONCLUSIVE), not a FAIL; plan d... | DEV_REVIEW_10_2026-10-09 |
| `a7be7ba65` | bench_test: LCD-10 start/stop confirm, LCD-11 two-tap delete, LCD-12 builder save judges | BENCH_WRITING_JUDGES_REVIEW_2026-10-09, DEV_REVIEW_9_2026-10-09 |
| `6076535a3` | bench web judges: fix ZONE-03 bool failsafe_state, DIAG-02 honors ack, OTA-03/04 target... | DEV_REVIEW_9_2026-10-09 |
| `3164aab2c` | bench_test: authed GET sends Accept-Encoding gzip and decodes body (fixes 17 WEB judge ... | DEV_REVIEW_9_2026-10-09 |
| `91191a595` | bench_test: implement LCD-07 rail pills, LCD-15 network page, LCD-17 touch-cal back-out... | DEV_REVIEW_9_2026-10-09 |
| `b952c3028` | cfg stores: save lock for unit_pref and profiles_favorites (multi-task savers); audit o... | DEV_FIRMWARE_REVIEW_9_2026-10-09, DEV_REVIEW_9_2026-10-09 |
| `3a593a7c0` | bench_test: OT-E11 recovery image receives an app image, LCD-20 recovery idle observati... | BENCH_WRITING_JUDGES_REVIEW_2026-10-09, DEV_REVIEW_9_2026-10-09 |
| `ea3453489` | zones_config_store: save mutex makes rev read, cfg write and rev bump one critical sect... | DEV_REVIEW_9_2026-10-09 |
| `7e08005d2` | backup import L6: live thermo_count is correct (import cannot change it); rename apply_... | DEV_REVIEW_9_2026-10-09 |
| `8073e66f8` | Aux review F5-F7: accurate handoff log text, rollback hazard note, guard-8 coverage los... | DEV_REVIEW_9_2026-10-09 |
| `a03093fb3` | web commission driver: icacls-grant current user before profile dir rm (sweep and detac... | DEV_REVIEW_9_2026-10-09 |
| `fb236ec4d` | Profile save: atomic convert-busy flag; busy refusal answers 409 | DEV_FIRMWARE_REVIEW_9_2026-10-09, DEV_REVIEW_9_2026-10-09 |
| `d55ab48be` | profiles: re-validate under the save lock, refuse saves while a zone conversion runs, e... | DEV_FIRMWARE_REVIEW_9_2026-10-09, DEV_REVIEW_9_2026-10-09 |
| `252e37801` | profiles: whole retarget under the save lock, re-check plan at commit, single stats pru... | DEV_FIRMWARE_REVIEW_9_2026-10-09, DEV_REVIEW_9_2026-10-09 |
| `9df033475` | profiles: close save-mutex review gaps (create outside critical section, delete/retarge... | DEV_FIRMWARE_REVIEW_9_2026-10-09, DEV_REVIEW_9_2026-10-09 |
| `0880162bf` | profiles: save mutex makes rev read, RAM assign, file write and rev bump one critical s... | DEV_FIRMWARE_REVIEW_9_2026-10-09, DEV_REVIEW_9_2026-10-09 |
| `e31e6a061` | Fix leaked rm node processes in web-commission driver; add build_host_tests -Only | DEV_REVIEW_9_2026-10-09 |
| `523ba6dc2` | Review 8 L1-L4: v1 image refused when manifest has a commit, boot fallback keeps rev-un... | DEV_FIRMWARE_REVIEW_9_2026-10-09 |
| `2841144cd` | Review 7 L1/L2/L4: wedge shown while busy, atomic owned abort, v1+v2 identity records; ... | DEV_FIRMWARE_REVIEW_8_2026-10-09 |
| `9e2ac9ef6` | bench_test: gate WEB-ZONE-10 abort POST behind write_refusal; move to WRITING_IDS | DEV_FIRMWARE_REVIEW_8_2026-10-09 |
| `6afbcb6f6` | Review 7 L3, L5: load_raw alloc failure is an error not 'absent'; gate zones unlock tes... | DEV_FIRMWARE_REVIEW_8_2026-10-09 |
| `ba714c93a` | wifi_prov: adopt legacy creds until a verified saved_nets record exists (review-6 M1, L1) | DEV_FIRMWARE_REVIEW_8_2026-10-09 |
| `df75de64f` | bench-test gate test: make every writing judge reach its gate (review 6 M2) | this doc |
| `f19b7c62b` | Fix review-5 L4/L5: heap scratch for rev_repair_junk and load_raw; guard zones CRC writ... | DEV_FIRMWARE_REVIEW_6_2026-10-09, DEV_FIRMWARE_REVIEW_7_2026-10-09 |
| `dcd67f549` | update chain: review 5 M1 image-embedded commit gate, L1 writer/timeout arbiter, L2 wed... | DEV_FIRMWARE_REVIEW_6_2026-10-09, DEV_FIRMWARE_REVIEW_7_2026-10-09 |
| `52502d56b` | bench_test tests: WEB-LOG-03 deliberately trails WEB-SEC-05 | DEV_FIRMWARE_REVIEW_6_2026-10-09 |
| `7551d44ad` | wifi_prov: make legacy default-partition wifi_cfg migration one-shot (review 5 M2) | DEV_FIRMWARE_REVIEW_6_2026-10-09 |
| `3a240a3a4` | suite gate test: WEB-ZONE-02 is read-only now | DEV_FIRMWARE_REVIEW_6_2026-10-09 |
| `8b193746f` | bench_test: fail-closed write gate (absent ctx suite refuses) for all WEB writing judge... | DEV_FIRMWARE_REVIEW_6_2026-10-09 |
| `f9549ae4e` | profiles test: junk prof_rev repair raises file-less slots to max observed rev | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `8c287553f` | Aux fault-drop covers every non-zone relay; disabling an aux drives OFF; failed aux wri... | DEV_FIRMWARE_REVIEW_5_2026-10-09, DEV_FIRMWARE_REVIEW_6_2026-10-09 |
| `61e579c9a` | profiles test: unmount cfg for deferred-repair test | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `512cb4e38` | profiles: preserve newer-firmware prof_rev tail on rewrite; junk repair requires cfg mo... | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `c2c0267a5` | ota_page: error text for writer_wedged_reboot_required (suite fix for d19d3df4) | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `ccabb020a` | test: LOW-4 snapshot test requires both lock acquisitions (snapshot + rev publish) | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `8a4739e6a` | CSRF guard: shared header-extraction glue, long Referer keeps scheme://host[:port], key... | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `cb7af5147` | profiles: erase NVS before dropping RAM slot on delete so failure is retryable (LOW-7) | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `667578a9b` | zones store: persist a snapshot taken under zones_cfg_lock, not the live struct (LOW-4) | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `80b957da4` | aux_outputs_cfg: load NVS/cfg-fs into locals, hold s_lock only to publish (LOW-3) | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `9155d1949` | backup import: refuse oversized builtin catalogue in pass 1, before any pref write (rev... | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `adbbec150` | Bench web judges: KCFG-02 no longer saves (activates), ZONE-02 read-only, LOG-02/X-02 v... | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `7d155f5a9` | Fix 3 suite failures: rev_repair_junk PSRAM guard, gate table counts (uri cap row NOT A... | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `95070c9ad` | Factory reset erases legacy Wi-Fi keys; run_state migration erases source; READ_ONLY probe | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `513c07e6e` | test_ota_page_poll_auth: error mocks carry a renderable body so removing the !r.ok guar... | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `d19d3df43` | update: surface wedged fetch writer, 8 KB-floor heap preflight with KDF margin, manifes... | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `11a85801e` | MED-1: re-check update claim after heat claim publish in profile and autotune starters | DEV_FIRMWARE_REVIEW_5_2026-10-09 |
| `708baa986` | Review 2 lows 5,6,8,9: backup relay_cycles message/kept report, over-size newer cfg fil... | this doc |
| `8340e540a` | Bench web judges: run-start probe hook (WIFI-05, SEC-06), LOG-02 HttpOnly via real clie... | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `45c2b4de2` | Fix suite failures: recovery health needle, aux fault-drop result handling (LOW-2) | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `d56fa8775` | backup import: size hidden-builtin parse to the 32-bit mask, not 8 (MED-2) | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `70ec462c3` | profiles: unknown rev floor no longer a permanent outage (review 2 findings 2, 4) | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `6d6d04e50` | Bench web judges: WEB-SAF/COMM/RDY/WIZ/SET/DISP (4.4, 4.5) | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `7beee17c5` | Wire uri_handler_cap max-routes negative test into run_all_checks; evidence row | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `15748a196` | web judges: ZONE-11 exact firing_stats key paths; cite PROF wire contract | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `139471ffd` | Bench test: WEB-DIAG-02..11, WEB-OTA-02..08, WEB-WIFI/SEC/BAK/KCFG/LOG/X-02 judges | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `c231c97c9` | Dev tools review LOW-1/3/4: zone_sweep array + eol rule, DRAM gate test, route-tier sta... | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `9e664dfba` | make_release: wire release_gates bench-evidence (--app-bin derives fw_build; whitespace... | DEV_FIRMWARE_REVIEW_4_2026-10-09 |
| `1c9d6bb75` | bench_test: WEB-PROF-02..11, WEB-STIM-02, WEB-ZONE-02..13 judges | this doc |
| `17dec75a7` | bench_test: autotune gate treats idle/done/aborted as not running | this doc |
| `424f4129b` | bench_test: WEB-DASH-02..12 judges (cases_web_dash.py), multi-window probes, ctx suite | this doc |
| `6515efe70` | Fix dev checks: cfg_convert additive keys, flash worker allowlist, doc blob citations, ... | this doc |
| `6ea993ce3` | recovery checkbuild: mirror http_origin_check.h | DEV_FIRMWARE_REVIEW_3_2026-10-09 |
| `fd1966271` | http: refuse cross-origin state-changing requests (ROUTE_TIER_REVIEW MED-1) | DEV_FIRMWARE_REVIEW_3_2026-10-09 |
| `6e5fd4fcf` | bench_test: fix FL-07 stash test after rebase | this doc |
| `d44a3b4c3` | bench_test: WEB judge harness prerequisites (window probes, _results, HTTP seams, Wi-Fi... | this doc |
| `4c8ecc4f8` | Fix dev review L1/L2/L3/L5: delete retry, zones commit lock, aux switch_count, stale sizes | DEV_FIRMWARE_REVIEW_3_2026-10-09 |
| `ae4fde7fe` | backup: export/import unit, ramp_assist, display_power, hidden profiles, tz, relay_name... | DEV_FIRMWARE_REVIEW_3_2026-10-09 |
| `b5bcdb731` | update chain review fixes: internal 2 KiB fetch scratch, manifest cross-check gate, cla... | DEV_FIRMWARE_REVIEW_3_2026-10-09 |
| `1a5167065` | aux outputs: drop aux on any safety fault while paused or idle (audit F1, F2) | DEV_FIRMWARE_REVIEW_3_2026-10-09 |
| `2ae8d1e61` | aux_outputs_cfg: commit RAM only after a successful save (F3); lock the store (F4) | DEV_FIRMWARE_REVIEW_3_2026-10-09 |
| `55932dbe6` | FL-07: judge format pending from GET /api/cfgfs/format_pending (the only route emitting... | this doc |
| `7d8722f8e` | release_gates bench-evidence: require full suite, per-case verdicts, designed-INCONCLUS... | this doc |
| `2796392b7` | bench_test: ST status parses all lines and FAILED always fails; OT-B02 not PASS with IN... | this doc |
| `d327138a7` | iter_tune_store: newer-blob probe uses persist_scratch_alloc, not plain malloc | DEV_FIRMWARE_REVIEW_2_2026-10-09 |
| `74c7265a1` | profiles: legacy short rev array is known; corrupt NVS blob never deletes live file; un... | DEV_FIRMWARE_REVIEW_2026-10-09, DEV_FIRMWARE_REVIEW_2_2026-10-09 |
| `3d9af5363` | factory_reset: erase legacy default-partition kiln_cfg keys; relay_cycles migration era... | DEV_FIRMWARE_REVIEW_2_2026-10-09 |
| `748e1becc` | pref_cfg_fs: report size-changing newer-version cfg file as newer, never rewrite it | DEV_FIRMWARE_REVIEW_2_2026-10-09 |
| `290db19e6` | Backup: export/import relay_cycles wear counters (raise-only); document iter_tune/ki_ba... | DEV_FIRMWARE_REVIEW_2026-10-09, DEV_FIRMWARE_REVIEW_2_2026-10-09 |
| `80da71d6c` | bench_test: fix LCD click-target allowlist test syntax | this doc |
| `ba9bbcc36` | bench_test: implement LCD-05 brightness, LCD-06 blank timeout, LCD-13 segments paging | this doc |
| `46616a4e8` | Route tier review LOW-3: recovery_mode to /api/status, /api/ota/esp/status now ADMIN; L... | DEV_FIRMWARE_REVIEW_2026-10-09, DEV_FIRMWARE_REVIEW_2_2026-10-09 |
| `d3e2a6baf` | bench_test: add OT-B02 OTA suite summary case | DEV_FIRMWARE_REVIEW_2026-10-09, DEV_TOOLS_REVIEW_2026-10-09 |
| `375476ed8` | iter_tune_ripple: no gain estimate; NEEDS_GAIN without --gain-c | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `8b69f0b9d` | Pico reboot script: judge E2 on the link-fault line, drop relay-write probe | DEV_TOOLS_REVIEW_2026-10-09 |
| `104a0055f` | bench_test: drop ST-01..04 from judge-less allowlist | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `cacd63ce2` | Add offline PWM-ripple sensor-tau estimator (diagnostic only, refuses gate-set captures) | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `fe39e090e` | bench_test: ST-01..04 judges (cases_static) with fake-workbench tests | DEV_TOOLS_REVIEW_2026-10-09 |
| `5e26edeaa` | Add Pico-reboot-mid-firing injection script and fake-board tests | DEV_TOOLS_REVIEW_2026-10-09 |
| `4be2ab671` | release_gates: wire bench-evidence dispatch in main(); add end-to-end CLI test | DEV_TOOLS_REVIEW_2026-10-09 |
| `b7a0648d5` | bench_test registry ratchet: drop SP-10/SP-11 from allowlist | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `acc9944a7` | bench_test: add SP-10 (CT fields, INCONCLUSIVE by design) and SP-11 (SK-03 relabel) judges | DEV_TOOLS_REVIEW_2026-10-09 |
| `4d9ccd04c` | bench_test registry: ratchet test against new judge-less case ids | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `215ef8e23` | release_gates: bench-evidence subcommand for the bench-pass-7d gate | DEV_TOOLS_REVIEW_2026-10-09 |
| `ec2cd8e74` | worktree_mint: verify -Base resolves, fix stale header text | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `55155b73a` | UI strings: cross-zone is guard 8 | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `1a2d2e33e` | Comments: cross-zone is guard 8, not guard 9 (guard 9 is control-tick liveness) | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `7ff9bf719` | route tier: fail on stale table rows; cap check requires KILN_HTTP_MAX_ROUTES >= max_ur... | DEV_TOOLS_REVIEW_2026-10-09 |
| `6f7de17d8` | Gate evidence table: rows for typed_field_producers and duplicate_symbols test; wire to... | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `70659b5d6` | Aux outputs: per-run on_time_s and switch_count in /api/profile_exec | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `e9d7ea846` | iter_tune_store: report size-changing newer-version NVS blob as newer, not corruption | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `631920d3b` | kiln_cfg_store: frozen byte-exact v1/v2 blob migration asserts plus corrupted-byte nega... | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `9fb7bc1bf` | SaftyFW config_store: committed frozen v1/v2 blobs with migration and truncation asserts | DEV_FIRMWARE_REVIEW_2026-10-09, SAFTYFW_REVIEW_2026-10-09 |
| `c712ca2c3` | make_release: enforce .dram0.bss budget via the standing checker; refresh stale gate ev... | DEV_TOOLS_REVIEW_2026-10-09 |
| `36d9f6a95` | release notes: real schema-version diff vs previous tag (hazard flags) | DEV_TOOLS_REVIEW_2026-10-09 |
| `a62d5c42e` | Stack-margin registration check: cover recovery image tasks | DEV_TOOLS_REVIEW_2026-10-09 |
| `a12ff94c7` | Migration check: govern ITER_TUNE_STORE_VERSION; document discard-on-mismatch exclusions | this doc |
| `02fd73ab7` | OTA page: remove orphaned ESP picker and update-order hint | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `7716998ef` | Fix dev check failures: backup_import truncation/malloc, profiles_validate allowlist, s... | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `ba8b60c45` | Crash ELF lookup: search recovery ELF archive; drop stale dual-slot advice | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `90b75e27e` | OTA page: retire dead single-slot Update ESP / Roll back buttons; point at Stage/Instal... | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `9584737f4` | profiles: degraded boot path never deletes files; unknown rev blob -> rev-unknown; eras... | DEV_FIRMWARE_REVIEW_2026-10-09, ROUTE_TIER_REVIEW_2026-10-09 |
| `8982a37a7` | dup-symbol check: pick manifest by coverage, fail loud if none matches; POST /api/zones... | DEV_FIRMWARE_REVIEW_2026-10-09, DEV_TOOLS_REVIEW_2026-10-09 |
| `99d9e0948` | build: kill whole process tree on stall/timeout; clamp ceiling under gate hard max | DEV_TOOLS_REVIEW_2026-10-09 |
| `806f183c5` | Fix mint fetch of dev, typed-field {0} constant capture, ON_OFF plan PAUSE text | this doc |
| `5207d0c37` | bench_test: taint on non-idle teardown, safe aux slot cleanup, fix C02 text | DEV_TOOLS_REVIEW_2026-10-09 |
| `bea8238ff` | Fix dev-flow check failures: negtest table rows, main_baseline land -Target main, drift... | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `c07b31325` | Worktree tools default to origin/dev; harden dev_promote promote detection | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `f8c641bd0` | Profile rev floor: seed from persisted revs on files-only path, refuse saves when floor... | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `9a3e7c842` | dev/main flow: land.ps1 -Target (default dev), dev_promote.ps1 + test, docs | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `e24d32553` | Tooling: backlight_pwm stack ceiling 112->192 (measured), check_duplicate_symbols ignor... | DEV_TOOLS_REVIEW_2026-10-09 |
| `be457072e` | land.ps1: read check log with FileShare.ReadWrite/Delete, fail fast on persistent read ... | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `4c1aebf1f` | Stage upload: erase the header sector only after identity/version/policy checks pass | DEV_FIRMWARE_REVIEW_2026-10-09, REVIEW_FWBATCH13_2026-10-10 |
| `007e2ad63` | build_kilnfw: progress-based stall timeout with 3 h ceiling instead of fixed 1800 s | DEV_TOOLS_REVIEW_2026-10-09 |
| `d338466f7` | Defer parse-time kc* helper calls on ota/setup_wizard/main/readiness pages; smoke loads... | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `56d1d9c00` | producers check: constant values no longer count as producers; remove dead failsafe_on_... | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `ef6ccdd0e` | Backup import: run real profile validators in pass 1, commit zones/aux before profiles | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `887c5a96f` | Profile rev floor follow-ups: seed floors on NVS load failure (F8), adopt rev-0 file (F... | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `415bd9420` | zones config: OOM paths cannot-decide, keep rev floor, no overwrite of newer file; dist... | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `31bec63b3` | WIP zones OOM | DEV_FIRMWARE_REVIEW_2026-10-09 |
| `8b8617f24` | bench aux: teardown deletes BENCH_AUX_RULE slots, stops paused/polls idle, reports hook... | DEV_TOOLS_REVIEW_2026-10-09 |
| `0b25b1f88` | aux bench: C02 expects 409 zone conflict; parse numeric exec state | DEV_FLOW_SCRIPTS_REVIEW_2026-10-09 (range) |
| `ee2d07240` | persist: live_profile/pref_cfg_fs/etc scratch via persist_scratch_alloc; drop grandfath... | DEV_FIRMWARE_REVIEW_2026-10-09 |
