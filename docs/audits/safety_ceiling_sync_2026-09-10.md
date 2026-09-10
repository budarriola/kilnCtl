# Safety ceiling sync (Pico `abs_max_temp_c` tracks the ESP zone max), 2026-09-10

Owner request, verbatim: "if i change the max temp in the web gui it should
change it in the pico too."

## Problem

The ESP's per-zone `max_temp_c` (`zones_cfg_t`) and the RP2040 safety
processor's own independent absolute ceiling (`abs_max_temp_c`,
`safety_cfg_store.h` param id `0x0104`) were completely unrelated numbers
that happened to start out equal (80 C). Raising the ESP's limit to fire
hotter silently left the Pico's ceiling behind, so the Pico tripped as soon
as the kiln passed the OLD limit -- fail-safe, but a trap: the Pico's
`abs_max_temp_c` had been sitting on the roadmap as a manual "raise
Pico-first-then-ESP" chore precisely because nothing kept them in sync.

## Standing invariant

**The Pico's ceiling must never be TIGHTER than the highest ESP zone
ceiling.** It is the second set of eyes -- a previous proposal to make the
Pico's limit tighter was an explicit owner rejection
(`feedback_abs_max_same_or_looser.md`). That invariant fixes the ORDER of
operations:

- **Raising** (new zone max > current Pico ceiling): the Pico is written
  and CONFIRMED first, before the ESP's zone config is allowed to commit.
- **Lowering** (new zone max < current Pico ceiling): the ESP's own commit
  happens first; the Pico is only tightened afterward, best-effort.

Reversing either direction opens a window where the Pico's ceiling is below
the ESP's live max -- exactly the hazard this feature exists to prevent.

## Design

### Pure decision logic: `safety_ceiling_policy.{h,c}`

`firmware/KilnFW/App/drivers/safety/safety_ceiling_policy.{h,c}` -- no
ESP-IDF, no wire I/O, host-tested directly
(`firmware/KilnFW/App/test/test_safety_ceiling_policy.c`).

- `safety_ceiling_policy_target_c(max_temp_c[], n)`: the Pico target,
  `max(max_temp_c[i] for max_temp_c[i] > 0) + SAFETY_CEILING_HEADROOM_C`
  (5 C, the same headroom convention the profile-target-vs-configured-limit
  dashboard warning already uses -- see that macro's own comment for why
  reusing it, rather than inventing a second number, is deliberate). Returns
  `0.0f` ("no target") if no zone has a positive `max_temp_c`.
- `safety_ceiling_policy_guard_raise(...)`: call BEFORE committing a
  proposed zone config. Writes the Pico (via an injected
  `safety_ceiling_writer_fn` callback) only when a raise is actually needed,
  and returns `false` -- meaning the caller MUST NOT commit -- if that write
  is not confirmed.
- `safety_ceiling_policy_apply_lower(...)`: call AFTER a zone config has
  already committed. Best-effort; a failure here is reported for logging but
  never blocks anything, since the invariant survives it (Pico stays wider
  than strictly necessary, never tighter).

### Zero-ceiling zones

A zone's `max_temp_c == 0` is NOT a "use the firmware default" sentinel for
this field (2026-08-27 audit,
`project_zone_band_zero_is_default_sentinel.md`): there is no
repo-established safe absolute-temperature default, and
`profile_executor_start.c` already refuses to run any zone whose ceiling is
`<= 0`. Such a zone can never actually fire, so it is excluded from the
maximum entirely -- it can never drag the Pico's ceiling up OR down on its
behalf. An all-zero zone config yields no target at all, and the Pico's
ceiling is left completely untouched (never a manufactured default).

### ESP-side glue: `safety_ceiling_sync.{h,c}`

`firmware/KilnFW/App/drivers/safety/safety_ceiling_sync.{h,c}` binds the
pure policy to real hardware:

- Reads the Pico's currently-cached `abs_max_temp_c` from
  `safety_cfg_store.c`'s existing GET_CONFIG_PAGE cache (no new wire
  traffic).
- The writer callback is `safety_cfg_http_set_and_confirm_f32()`, a new
  public wrapper in `safety_cfg_http.c` around that file's own
  `apply_pairs()`/`confirm_commit_landed()` machinery -- the SAME
  stage+commit+confirm-by-readback discipline every other commissioning
  write in that file already uses. This was a deliberate reuse, not a new
  implementation: `confirm_commit_landed()` is the audited fix for "ok
  cannot fail" (SET_PARAM/COMMIT_CONFIG are fire-and-forget broadcasts; a
  REJECTED reply can miss the ACK window) -- a second, independent write
  path for this one field would have had to re-earn that same audit.

### Wired into `zones_post_handler()` (`zones_http_post.c`)

- **Before** the existing `s_zones.cfg = tmp;` commit line:
  `safety_ceiling_sync_guard_raise()` runs against the PROPOSED config
  (`tmp`). If it returns `false`, the handler answers `409 Conflict` with a
  JSON `{"ok":false,"error":"safety_ceiling_raise_failed","reason":"..."}`
  and returns without touching `s_zones` at all -- same "never partially
  apply" discipline the rest of that handler already follows.
- **After** the commit (and after `nvs_save()`/`relay_names_save()`):
  `safety_ceiling_sync_apply_lower()` runs against the now-live config,
  best-effort, logged either way.

### Does the Pico accept this write at all times? No -- and this is central.

SaftyFW's `config_store_write()` refuses EVERY config write unconditionally
whenever the relay is `ARMED`
(`config_store_decide_write()`: `armed ? REFUSED_ARMED : OK`, no per-field
carve-out -- confirmed by reading `config_store_flash.c`/`link_task.c`
directly, not assumed). Critically, `RELAY_OWNER_STATE_ARMED` is the Pico's
ORDINARY STANDING STATE -- everything past the post-boot or
post-trip-clear grace window (`relay_grace.c`), not "only mid-firing". So a
raise attempt is EXPECTED to be refused whenever the Pico has been up for
more than about a `startup_grace_s` window, which is most of the time on a
bench that isn't freshly reset. This is the SAME restriction this
codebase's own memory already documented for `max_rate_c_per_min`
(`abs_max_temp_c` is just another row in the identical
`SAFETY_CFG_PARAM_TABLE`/`config_store_record_t`, gated by the exact same
`config_store_write()` call) -- not a special case invented for this
feature.

**This is handled, not papered over:** a raise attempt refused this way
surfaces the Pico's own reason text (`"relay is ARMED -- config writes are
refused while ARMED"`, `commit_reject_reason_words()`) verbatim through the
409 response, and the zones page shows it via `msg.textContent` instead of a
generic failure. The UI never silently requires an undocumented
safety-processor reset -- it tells the operator exactly why the raise did
not happen. (The zones POST endpoint already refuses to run at all while a
firing/hot zone is active via `ota_http_check_interlocks()`, which is a
DIFFERENT, narrower gate than ARMED -- so a raise can still be legitimately
refused even when no firing is running, simply because the Pico has been up
a while.)

### Read-back verification

Every write goes through `apply_pairs()`/`confirm_commit_landed()`, which
forces a live re-fetch of the Pico's config page and compares the
JUST-SUBMITTED value bit-for-bit against what the Pico now reports -- never
trusting a bare ACK. This is the exact audited fix for the "logging
unchecked success" class already found and fixed in this file
(`project_safety_calls_logging_unchecked_success.md`); this feature reuses
it rather than re-introducing a narrower version of the same defect.

### UI visibility

`GET /api/zones` now emits a top-level `safety_ceiling` object:
`{"target_c": <what the current zone maxima imply>, "pico_known": <bool>,
"pico_current_c": <what the Pico's cache last confirmed>}`. `zones_page.html`
renders this read-only, distinguishing three cases: matches, wider-than-needed
(safe, will tighten later), and narrower-than-target (a raise likely could
not be confirmed -- check the save error). `tools/PcTools/src/kilnctrl/
zones_http_client.py`'s field-mirror table was updated to recognise this as
read-only telemetry (same treatment as `ct_warn_mask`/`safety_wiring`), which
`tools/run_all_checks.ps1`'s zones-field-drift check enforces.

LCD display was deliberately NOT touched in this pass: no existing LCD
screen has room for this without a real layout change, and this session was
expressly forbidden from flashing (so any LVGL layout edit could not be
visually verified against the real 480x320 panel -- the established
practice here is numeric pixel sampling via `capture_lcd.ps1`, not a blind
edit). Left as a follow-up.

## Host tests

`firmware/KilnFW/App/test/test_safety_ceiling_policy.c`, pure-logic, no
hardware, four scenarios exactly as requested:

1. **Raising**: Pico written first via the injected writer, confirmed, then
   (implicitly) the caller's commit may proceed. Asserts the writer is
   called exactly once with the correct target (`120 + 5 = 125`).
2. **Lowering**: proves the pre-commit guard does NOT touch the Pico for a
   lowering change (writer never called), then that the separate
   post-commit `apply_lower()` call writes it afterward.
3. **Failed/unverified Pico write**: the injected writer returns `false`
   (with a reason text modelling the real ARMED refusal) -- proves
   `guard_raise()` returns `false` (the caller must not commit) and that the
   ARMED reason text survives to the caller.
4. **Zero-ceiling zone**: an all-zero zone config yields no target and never
   touches the Pico; a mixed config's zero zones are excluded from the
   maximum (proven against a real `144.0f`/`150.0f`-shaped case, not just the
   trivial all-zero one).

Negative-tested against PRODUCTION `safety_ceiling_policy.c` (not a test-local
copy): the "already satisfied" comparison was broken (an unconditionally-false
short-circuit), confirmed RED (2 failures, main host-test executable), then
restored by hand (the exact line reverted textually, confirmed by
re-grepping the restored line).

## Verification performed this session

- `firmware/KilnFW/App/test/build_host_tests.ps1`: 34/34 executables built,
  all pass (bash tool, per this repo's own documented flakiness of that
  script under the PowerShell tool).
- `idf.py -C firmware/KilnFW build` (PowerShell tool): clean KilnFW target
  build, no warnings from the new code. NOT flashed (out of scope for this
  session).
- SaftyFW host tests (`build_host_tests.ps1`, run from a short-path git
  worktree at HEAD, `C:\wt\sfw3`): 196+ checks pass -- SaftyFW itself is
  untouched by this change; run only to confirm no cross-contamination and
  to satisfy this session's own verification requirement.
- `tools/run_all_checks.ps1`: fixed one real regression this work caused
  (`tools/PcTools/src/kilnctrl/zones_http_client.py`'s top-level field
  mirror missing the new `safety_ceiling` key) and confirmed all remaining
  failures (`check_00_kilnfw_target_build.ps1`'s worktree build,
  `check_uri_handler_cap.ps1`, `tools/PcTools/selfcheck.py`,
  `check_saftyfw_task_count.ps1`, `check_all_task_stack_budgets.ps1`'s lvgl
  overage, `check_saftyfw_task_stack_budgets.ps1`'s stale-ELF refusal) are
  other sessions' unrelated in-progress work (a rate-guard-meta feature
  mid-edit in `safety_cfg_http.c`/`safety_cfg_store.c`, a `watchdog_task`
  missing from the SaftyFW stack-budget table, new HTTP routes pushing past
  the URI handler cap, an lvgl task stack overage, and a PC-side comms
  selfcheck flake) -- none touch any file this session edited.

## No protocol or schema change

`abs_max_temp_c` (wire param `0x0104`) already existed on both ends. No
`KILNLINK_PROTOCOL_VERSION` bump, no `ZONES_CFG_VERSION` bump -- this
feature is pure orchestration logic on top of two already-existing knobs.
