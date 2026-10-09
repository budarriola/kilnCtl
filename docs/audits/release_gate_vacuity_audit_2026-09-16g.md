# Release gate vacuity audit — 2026-09-16, part 7

Continuation of `docs/audits/release_gate_vacuity_audit_2026-09-16.md` and
`...16b.md`, blocker 3 of `docs/RELEASE_HARDENING.md`. This pass has two
parts: (1) fix a real, previously-identified defect in
`tools/check_duplicate_symbols.ps1` that was silently excluding a genuine
component object from every duplicate-symbol scan, and (2) negative-test the
next slice of eight not-yet-covered gates.

Work was done in a dedicated worktree at `C:\wt\gatesaudit0916g_ooo0nv`
(origin/main, HEAD c7759914 at checkout, provisioned via
`tools\worktree_mint.ps1 -RunSetup`). Baseline `tools/run_all_checks.ps1
-Fast` on that checkout: 95 passed, 1 skipped (expected —
`check_recovery_image_size.ps1`, another session building that image), 0
failed. All negative tests below sabotage PRODUCTION source (never a
test-local mirror/copy), confirm RED with a decisive named line, restore BY
HAND (never `git checkout --`/`git restore`/`git stash`), confirm both an
empty `git diff` and a `git hash-object` match against the committed blob,
and re-confirm PASS.

## Part 1 — real defect fixed: `tools/check_duplicate_symbols.ps1`

**Claim:** the script scans every compiled `.obj` in the ESP build tree,
classifying each by which component's declared source files it should
belong to, so a genuine duplicate symbol between two components is caught
rather than dismissed as a stale/leftover object from a prior build
configuration.

**Defect found:** `$componentSourceRoots` mapped the `hwabstraction_esp`
component's build directory to a single source root,
`firmware\hwAbstraction\esp`. But `firmware/hwAbstraction/idf/hwabstraction_esp/CMakeLists.txt`
genuinely lists `../../common/hal_status.c` in its `SRCS` — that object is
compiled into this component from
`firmware\hwAbstraction\common`, a source root the map never named. Every
object built from that root (`hal_status.c.obj`) was therefore
misclassified as "stale" (no known source with that basename) and silently
dropped from every scan — including scans for a duplicate symbol that
genuinely lives inside it. Confirmed pre-fix: the script reported 264
objects scanned; `hal_status.c.obj` was not among them despite being a real,
freshly-compiled object in the build tree.

**Fix:** changed the hashtable value to an array of both real source roots,
and rewrote the two loops that consumed it (source-basename indexing, and
per-object classification) to iterate over all of a component's source
roots rather than assuming exactly one:
```powershell
"esp-idf\hwabstraction_esp\CMakeFiles\__idf_hwabstraction_esp.dir" = @("firmware\hwAbstraction\esp", "firmware\hwAbstraction\common")
```
Post-fix, the script reports 265 objects scanned (264 to 265),
`hal_status.c.obj` now correctly attributed to `hwabstraction_esp` instead
of silently excluded.

**Negative test:** appended `int zz_audit_dup_symbol(void) { return 1; }` to
`firmware/hwAbstraction/common/hal_status.c` and, separately,
`int zz_audit_dup_symbol(void) { return 2; }` to
`firmware/hwAbstraction/esp/common/hal_esp_common.c` — a genuine duplicate
definition spanning exactly the source root the defect had been dropping.
Ran `firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1`: the ESP-IDF
linker failed with a real "multiple definition of `zz_audit_dup_symbol`"
error (a build failure, not a check verdict — confirmed real, not the gate
firing). Copied the fixed `check_duplicate_symbols.ps1` into the isolated
build directory the failed build's object files actually lived in
(`check_00_kilnfw_target_build.ps1` only publishes to the worktree's own
`build/` on success, so a build-failing negative test must inspect that
isolated directory directly) and ran it there against the real compiled
objects: **FAILED, naming `zz_audit_dup_symbol` as duplicated between
`hal_status.c.obj` (hwabstraction_esp) and `hal_esp_common.c.obj`
(hwabstraction_esp)** — the fixed classification is what makes this
duplicate visible at all; pre-fix, `hal_status.c.obj` would never have
reached the comparison.

Restored both files by hand. `hal_status.c` restoration initially produced
a byte mismatch (916 vs. 915 bytes — an extra trailing newline from the
hand-edit) caught by the hash check, not the visual diff; fixed by exact
byte reconstruction. Verified both files:
- `firmware/hwAbstraction/common/hal_status.c`: `git diff --quiet` empty;
  `git hash-object` (first 8 chars
  blob:firmware/hwAbstraction/common/hal_status.c`cf1dbafb`) matched
  `git rev-parse HEAD:<path>` exactly.
- `firmware/hwAbstraction/esp/common/hal_esp_common.c`: `git diff --quiet`
  empty; `git hash-object` (first 8 chars
  blob:firmware/hwAbstraction/esp/common/hal_esp_common.c`7f544609`) matched
  HEAD's blob exactly.

Forced a full rebuild (`check_00_kilnfw_target_build.ps1` re-run from clean)
before the confirming PASS — per this repo's own standing rule that a
poisoned object surviving in a build directory has produced a committed
wrong verdict here before. Rebuild succeeded; `check_duplicate_symbols.ps1`
re-run: PASS, 265 objects scanned, no duplicates.

**Verdict: real defect, fixed.** `tools/check_duplicate_symbols.ps1` is the
deliverable change of this pass.

## Part 2 — eight gates negative-tested (all load-bearing)

### 1. `tools/check_c_files_in_cmakelists.ps1`
**Guards:** every `.c` file under `App/`, `drivers/`, `SaftyFW/src/`, and
hwAbstraction basenames is actually referenced by some `CMakeLists.txt` —
catching a new file added but never wired into the build (which silently
compiles nothing and links fine).

**Negative test:** created untracked
`firmware/KilnFW/App/drivers/zz_audit_unwired.c`. Result: FAILED, naming
that exact file as present on disk but absent from every CMakeLists.txt.
Restored via `rm`; `git status --porcelain` confirmed clean (never tracked,
nothing to hash-check). Re-run: PASS — "C files in CMakeLists check passed:
6 App/, 218 drivers/, 52 SaftyFW/src/ refs found ... hwAbstraction/: 38
basenames covered."

**Verdict: load-bearing.**

### 2. `tools/check_no_duplicate_crc.ps1`
**Guards:** no new hand-rolled CRC polynomial/table duplicate is introduced
outside the allowlisted pre-migration set. Scans `git ls-files` (tracked
files only).

**Negative test:** created and `git add`ed
`firmware/KilnFW/App/drivers/zz_audit_crc.c` containing
`static unsigned short poly = 0x1021;`. Result: FAILED, naming the file.
Restored via `git reset -- <path>` then `rm`. Re-run: PASS — "Duplicate CRC
check passed ... (4 known pre-migration duplicate(s) allowlisted)".

**Verdict: load-bearing.**

### 3. `tools/check_relay_authority_paths.py`
**Guards:** relay writes only happen through the authorized owner path, not
from an arbitrary handler.

**Negative test:** inserted `kiln_io_set_relay(0, 1);` as the first
statement inside `json_escape()` in
`firmware/KilnFW/App/drivers/http/dashboard_json.c`. Result: FAILED, naming
the file:line and the exact unauthorized call. Restored via exact
string-replace; `git diff --quiet` empty; `git hash-object` (first 8 chars
blob:firmware/KilnFW/App/drivers/http/dashboard_json.c
cc9dfc29 (not backtick-quoted here, per this repo's superseded-citation
convention), since superseded
by later unrelated edits to the file; the underlying finding -- that
`json_escape()` is where the negative test was inserted and that the
restore was byte-exact -- is unchanged, so the citation is refreshed to
blob:firmware/KilnFW/App/drivers/http/dashboard_json.c`dd0478dc` (citation
refreshed 2026-09-24, again 2026-09-28 after HP-02, again 2026-10-05 after
spare-relay WP-6, again 2026-10-08 after the /api/status ETag helpers)) matched HEAD. Re-run: PASS.

**Verdict: load-bearing.**

### 4. `tools/check_heat_enable_wiring.ps1`
**Guards:** every `heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE)`
acquire has a matching release, so an executor path can't leak the claim and
permanently block another claimant.

**Negative test:** commented out (prefix `//ZZ_AUDIT `) the three real
`heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);` calls in
`firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c` (line 505)
and `profile_executor_status.c` (lines 59, 153). Result: FAILED —
"profile_executor ... no heat_enable_release() call...". Restored via sed
removing the prefix; `git diff --quiet` empty on both files;
`git hash-object` (first 8 chars
blob:firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c
489bc22c (not backtick-quoted here, per this repo's superseded-citation
convention), since superseded by a later unrelated edit; line 505 still holds the same
`heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);` call this negative test
targeted, so the citation is refreshed to
blob:firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c`0ca1a2b0`
(citation refreshed again 2026-09-24 after 540b2d72 added the per-zone claim release, 2026-10-05 after spare-relay WP-3 added the aux functions, and 2026-10-07 after the on/off min_off_s seed helper, and 2026-10-09 after later unrelated edits, and again 2026-10-09 after 45c2b4de and 8c287553; the heat_enable_release() call this test targets is unchanged))
and, at the time of this audit, 552f8a05 for `profile_executor_status.c`
-- that file has since changed, so its blob id is no longer cited as
resolvable against current HEAD) matched HEAD on both at audit time. Re-run:
PASS — "1 enable / 2 release wire call(s)...".

**Verdict: load-bearing.**

### 5. `tools/check_uri_handler_cap.ps1`
**Guards:** `wifi_provision_http.c`'s `config.max_uri_handlers` stays ahead
of the real worst-case count of `httpd_uri_t` route registrations across all
of `drivers/*.c` — the fourth attempt at guarding a bug that has reached the
bench three times before as a silent 404.

**Negative test:** changed `config.max_uri_handlers = 140;` to `= 1;` (line
1040). Result: FAILED — "max_uri_handlers (1) is below the real worst-case
route count (137)...". Restored to `140`; `git diff --quiet` empty;
`git hash-object` matched HEAD at the time (this doc's own blob citation for
this file has since been superseded three times by later legitimate edits to
it unrelated to this check's subject -- first WEB_AUTH_PLAN.md section 8's cap
bump to 151 (superseded citation 8b14215d, prefix not backtick-quoted here
so this check does not try to grade a hash that is expected not to resolve),
then commit 6de75575's disclosure gating for the saved SSID and static-IP
topology fields (superseded citation 0821916d, prefix not backtick-quoted
here for the same reason), then commit ca202dbf's revert of an unnecessary
`+ sizeof(sta_ip_field)` term in the `json[]` buffer sizing (superseded
citation 8fc84030, prefix not backtick-quoted here for the same reason),
then commit b7bc31d2's move of `networks_get_handler`'s JSON buffer off the
stack to heap scratch, unrelated to this check's subject (the auth/gate
logic the negative test exercises is unchanged), then a further unrelated
edit that superseded citation 06e191a4 too, then the 2026-09-28 AP-fallback
change's `/status` JSON `ap_pending_teardown` field, unrelated to this
check's subject, which superseded citation f22fcfff (not backtick-quoted,
superseded) was itself superseded by `93a8716f`'s shared AP-subnet helper/
HTTP-layer message/confirm-side guard change, unrelated to this check's
subject (the auth/gate logic the negative test exercises is unchanged);
the underlying claim still holds, refreshed to
blob:firmware/KilnFW/App/drivers/http/wifi_provision_http.c`cebd2f59` (citation refreshed 2026-10-05)
is the current one (citation refreshed 2026-09-30); see
check_doc_hash_citations.ps1). Re-run at that time:
PASS, with an informational note (not a defect) that headroom was thin — 3
spare slots for 137 routes against a cap of 140. That cap/count pair is
itself now stale (superseded, like the blob above, by ordinary route/cap
changes unrelated to this check's subject): re-running today
(2026-09-18) reports 145 routes against a cap of 151 — 6 spare slots.

**Verdict: load-bearing.** (Informational: cap headroom is thin enough that
the next added route may need another bump — not a fix, since the check is
correctly reporting current reality, not failing. The specific spare-slot
count drifts with ordinary route additions; re-run the check rather than
trusting a number quoted here.)

### 6. `tools/check_test_c_files_wired.ps1`
**Guards:** every test `.c` file under the two `test/` trees is reachable
from some build target, catching an orphaned test file that silently stops
running.

**Negative test:** created untracked
`firmware/KilnFW/App/test/zz_audit_orphan.c`. Result: FAILED, naming the
file. Restored via `rm`; `git status --porcelain` confirmed clean. Re-run:
PASS — "198 test .c files across 2 test/ trees all reachable...".

**Verdict: load-bearing.**

### 7. `tools/check_uart_version_independence.ps1`
**Guards:** `UART_PROTOCOL_VERSION` (the PC-to-ESP link) is never re-derived
from `KILNLINK_PROTOCOL_VERSION` (the ESP-to-Pico link) — these are two
deliberately independent protocol versions.

**Negative test:** changed
`#define UART_PROTOCOL_VERSION ((uint16_t)12)` to
`((uint16_t)KILNLINK_PROTOCOL_VERSION)` in
`firmware/KilnFW/App/drivers/common/uart_task_ids.h` line 191. Result:
FAILED — "UART_PROTOCOL_VERSION must never be re-derived from
KILNLINK_PROTOCOL_VERSION (or any other KILNLINK_* symbol)...". Restored to
`((uint16_t)12)`; `git diff --quiet` empty; `git hash-object` (first 8
chars b2f1e683, at the time of this audit -- that file has since changed
(UART_PROTOCOL_VERSION 12 -> 13, 2026-09-20), so its blob id is no longer
cited as resolvable against current HEAD) matched HEAD at audit time.
Re-run: PASS.

**Verdict: load-bearing.**

### 8. `tools/check_heartbeat_contract.ps1`
**Guards:** the cross-language producer/consumer pair between
`link_hub.py`'s `_heartbeat_loop` (PC side) and
`UART_BRIDGE_LINK_TIMEOUT_MS` (firmware side) — no compiler/linker
relationship connects them, so this check regex-scans both files for three
specific historical regressions: (1) the producer thread never actually
starting, (2) the interval/ack-timeout margin thinning past half the
firmware timeout, (3) `_HEARTBEAT_TASK_ID` colliding with a real
`UART_TASK_ID_*`.

**Negative test:** commented out the producer's start call in
`tools/PcTools/src/kilnctrl/link_hub.py` (`LinkHub.start()`, line 291:
`#threading.Thread(target=self._heartbeat_loop, daemon=True,
name="link-hub-heartbeat").start()`). Result: FAILED — "The heartbeat thread
start() call is commented out in link_hub.py -- producer exists in source
but never runs." Restored by removing the `#`; `git diff --quiet` empty;
`git hash-object` of the file matched
the pre-sabotage blob at the time (its HEAD blob has since moved on).
Re-run: PASS — "producer runs, margin holds, task id is isolated."

**Verdict: load-bearing.**

## Summary

Part 1: one real, previously-identified defect fixed
(`tools/check_duplicate_symbols.ps1`), with a negative test that required
reaching into the isolated build directory directly (a build-failing
negative test does not get published back to the worktree's own `build/`).

Part 2: eight additional gates negative-tested against sabotaged PRODUCTION
code, all eight found load-bearing (FAIL with a decisive named violation on
sabotage, clean hand-restore verified by empty diff + hash match, PASS on
restore). No vacuous gate found in this slice; no additional fixes were
needed beyond Part 1.

Combined with `...16` and `...16b`, this closes out roughly 16 gates of the
~85+ originally uncovered, with zero vacuous checks found among the 15
negative-tested so far (`check_duplicate_symbols.ps1` was a real
misclassification bug, not a vacuous check — the check itself does fire
correctly once objects are classified right).
