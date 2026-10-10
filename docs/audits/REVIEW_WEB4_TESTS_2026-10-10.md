# Review: webfx3, c78b, r2dgh, a3seam (2026-10-10)

Opus review of four origin/dev batches. Review only, no code changes. Focus:
vacuous tests, behaviour changes hidden in refactors, `$totalExpected`, and
firmware defects that tests encode as expected.

| Batch | Commits | Scope |
|-------|---------|-------|
| A webfx3 | b908598ca, 4fac4ad91, 478f03d9e | live profile generation guard (M1), unsaved-edit guards L1-L7, backup partial-write header |
| B c78b | bd64bc01e | host tests: dashboard_autotune GET handlers, zone_aux_convert move |
| C r2dgh | f5e7a6002..5d01d185e | host tests: wifi_prov_api, thermo_owner, endian, adaptive_tune commit, ui_lcd_lock, security_backend_web_auth |
| D a3seam | 21b163662, 745afe327 | LCD profile builder segment logic seam |

## Verdict

No MED or HIGH findings. No firmware defect is encoded as expected
behaviour by these tests. `$totalExpected = 92` is correct. Findings are
LOW or informational.

## A. webfx3

### A1 (LOW) M1 generation guard: the race is web-vs-LCD, not two httpd workers

The fix notes say check-then-write is not atomic "across httpd workers".
esp_http_server runs one shared worker task (`http_async_job.h` says so),
so two web requests cannot interleave between `live_gen_stale()` and
`live_profile_save_working()`. The real concurrent writer is the LCD/LVGL
task (`ui_edit_firing_apply.c`), which does its own generation check and
then `live_profile_save_working()`. `live_profile.c` has no mutex; only
`s_live_profile_generation` is `_Atomic`. Window: check to verified save
(one cfg write). Effect if hit: last write wins, both sides report
success, and a concurrent save can make the other side's read-back
verification fail spuriously. Needs a simultaneous web and LCD edit of the
same working copy. Severity LOW. A real fix is a lock around
check+save+bump in `live_profile.c` (or a compare-and-save API taking the
expected generation).

Related, smaller:
- The edit response reads `generation` after the save, not as part of it,
  so a save landing in between hands the client a generation that already
  includes someone else's write (ABA, tiny window).
- `s_live_profile_generation` is RAM-only and restarts at 0 on reboot. A
  client holding gen N from before a reboot matches again after exactly N
  post-reboot saves (reset-one-side class). LOW; the page refetches on
  load, so only a long-open tab or a PcTools caller is exposed.

### A2 (LOW) Page sends no gen while `lastGen` is null

`genQuery()` returns `''` when `lastGen` is null and the board treats an
absent `gen` as accepted. `lastGen` is set to null on Reload, after fork
and after decide, until the status fetch lands. A Save in that window is
ungated. Ordering is status (gen) then content, which fails safe when both
complete. Fix: disable Save until `lastGen` is known, or have the board
require `gen` from the page (keep it optional only for PcTools).

### A3 (doc) Comment claims PcTools always sends gen

`profiles_live_http.c`'s comment says "the PcTools client and the page
always send it". The PcTools `generation` parameter is optional and
defaults to None (`_gen_path` appends `?gen=` only when given), and the page
omits it per A2. The `?content=1` GET also carries no generation.

### A4 Backup partial-write header: sound

`X-Kiln-Partial-Write: 1` is set only on the 500 path where
`partial_write` is true. Every other 500 in `backup_import.c` is a pre-write
OOM or the dry-run path. `backup_import_apply` sets `partial` on each
post-write failure (kiln_configs, aux, candidates OOM, two-pass). The page's
three messages (partway, before anything written, connection lost) match.
Tests cover both 500 variants.

### A5 (LOW) Unsaved-edit guards

- Tests are not vacuous: inflight `release()` throws if no POST happened,
  and fail mode uses the identical setup, so the POST is proven reached.
  `_page_vm.js` now throws on unregistered selectors. The D7GUARD doc
  already notes the profiles inflight mutation is caught by an incidental
  exception, not the assertion; that stands.
- `setup_wizard_page.html` `postStepState` has no edit-seq protection: a
  successful save clears `kcDirty` even for edits typed while it was in
  flight. Same class the other pages were fixed for.
- Profiles `kcSuppressUnload` (1500 ms around a download) also skips the
  prompt for a real leave inside that window. Minor.
- Informational, pre-existing: in-page hash navigation in the setup wizard
  discards step edits without a prompt.

## B. c78b

### B1 (LOW) zone_aux_convert gate action: near-equivalent mutant MISSED

`op_mode_blocked()` uses `SYS_ACTION_WRITE_ZONES_CONFIG`. The test asserts
only the shared prefix "firing or autotune run is active". FACTORY_RESET,
CFGFS_FORMAT, STAGE_WRITE and UPDATE_SETTINGS_WRITE block on the same
condition with the same prefix, so swapping the action is MISSED (confirmed
below). Behaviourally near-equivalent today, but the gate's policy could
diverge later. Fix: assert the action-specific tail ("zone config cannot be
changed").

Otherwise the convert test is strong: stateful fakes, refusal leaves state
unchanged, success targets `8+(relay-1)`, rollback paths, journal kept on an
incomplete rollback, NOTHING_CHANGED gives 500, verify mismatch undoes every
step.

### B2 (info) dashboard_autotune GET tests

Cover status (idle, stepping, aborted with escaping, done, OOM), matrix
(cells, RGA, one-zone grid, OOM) and trace CSV (128-row batches, exact
multiple, client drop leaves no terminator). The link stub
`struct { char pad[256]; } s_dash;` is a type-mismatched global; harmless
because the autotune GET path never touches it, but an ODR trap if a later
test links code that does. The "request body ignored" case is weak.

## C. r2dgh

### C1 `$totalExpected = 92` is correct

bd64bc01e added 2 `Invoke-HostTestExe` blocks (86 to 88); r2dgh added
thermo_owner, safety_link_endian, ui_lcd_lock and security_backend_web_auth
(88 to 92). There are 92 call sites, all at the try block's top level, with
no duplicate names. wifi_prov and adaptive_tune additions extend existing
executables. D adds to the main executable's source list, not a new block.

### C2 (info) Test quality

- thermo_owner: dispatch checks exact channel and arguments per command,
  absent channel never calls the driver and never returns a stale READ,
  READ_ALL clamp, slot pool owner/client release, pool exhaustion and full
  queue leak nothing. Solid.
- ui_lcd_lock: tier ordering, auth-off collapse, deferred action context,
  USER PIN against an ADMIN action, countdown rounding, relock edge fires
  once, force-lock one-shot keypad exemption, policy off/on reset of the
  session. Solid.
- security_backend_web_auth: drives the installed vtable and captured
  bootstrap route over the real store and session table, including lying
  erase, half-wipe and truncated body. Solid.
- endian: exact-byte LE plus unaligned round trip. Fine.
- wifi_prov_api and adaptive_tune commit paths: no defects found.
- Cosmetic: the three adaptive_tune calls sit inside the "F3 follow-up"
  section of `test_adaptive_tune.c`.

### C3 (LOW, behaviour the test pins) Clean `clear_all_credentials` keeps live sessions

`test_clear_all_credentials` asserts "clean success does not itself
invalidate". In `security_backend_web_auth.c`, the half-wipe branch
(web cleared, TOTP stuck) destroys both roles' sessions because "they sit on
a credential that no longer exists", but the full-success branch leaves the
same sessions alive. While bootstrap is outstanding `http_auth_check()`
denies ADMIN routes to a stale session, but ROUTE_TIER_USER routes still
allow it until the bootstrap handler's `web_auth_table_destroy_all()`.
Little practical exposure (anyone can bootstrap at that point), but the two
branches are inconsistent. Recommend destroying all sessions on the success
path too and updating the test.

## D. a3seam

### D1 No behaviour change in the refactor

Captions, limit caption and pad unit conversion produce the same strings and
values as before. The `"Maximum 12 segments reached"` literal ignores the
new `max_segments` argument (it was a literal before too); harmless while
`PROFILE_MAX_SEGMENTS` is 12.

### D2 (LOW) R4 test cannot catch the regression it names

The R4 case only proves `ui_pbs_pad_to_celsius()` converts with whatever
unit was captured. The guarantee R4 needs, that the page's done callbacks
use `s_pad` and not `unit_pref_get()`, lives in
`ui_page_profile_builder_segment.c`, which is not host-built. Reverting a
done callback to `ui_unit_entry_to_celsius(value, unit_pref_get(), ...)`
would pass every test. The seam is a pass-through; the test is close to
vacuous for R4. The R5 caption and limit caption checks are meaningful.

## Negative tests

`tools/negtest.ps1 -RequireAssertion`, kilnfw host tests narrowed with
`-Only` to the affected executables, ExpectPattern
`(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`.

Baseline passed.

| Mutation | Verdict | First failing check |
|----------|---------|---------------------|
| `profiles_live_http.c` edit-path `live_gen_stale()` disabled | CAUGHT | test_profiles_live_http.c:651 stale gen edit is 409 |
| `profiles_live_http.c` decide-path `live_gen_stale()` disabled | CAUGHT | test_profiles_live_http.c:659 stale gen decide is 409 |
| `zone_aux_convert_http.c` gate action WRITE_ZONES_CONFIG to FACTORY_RESET | MISSED | (B1) |
| `thermo_owner.c` READ_ALL clamp removed | CAUGHT | test_thermo_owner.c:298 |
| `security_backend_web_auth.c` half-wipe session invalidation removed | CAUGHT | test_security_backend_web_auth.c:366 |
| `ui_profile_builder_segment_logic.c` `<= 1` to `< 1` | CAUGHT | test_ui_profile_builder_segment_logic.c:39 |

The run's overall verdict was ERROR only because this audit file was created
in the worktree while it ran (`real_tree_unchanged: false`; `git status`
afterwards showed only this file). Each mutation verdict above is valid.
