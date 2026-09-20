# boot_guard: post-flash "unhealthy boot" foot-gun (2026-09-08)

Second finding from the same session as `docs/audits/boot_guard_recovery_loop_2026-09-08.md`
(the stuck-counter/RTC-marker fix, commit `0b5d9dad`). That audit fixed a board that was
already stuck in RECOVERY MODE. This one addresses how it got there: **the boot immediately
after `flash_firmware()` comes up with no partition table visible for at least one NVS
partition (`kiln_nvs not present`), so it never marks itself healthy** -- and, with the
threshold at 3, a handful of ordinary development flashes in a row silently walks a
perfectly good board into recovery mode.

## 1. Mechanism -- observed vs. inferred

**OBSERVED in code** (not hypothesized):

- `boot_guard_init()` (`boot_guard.c`) opens its OWN handle on `kiln_nvs` via
  `hal_kv_init_partition(KILN_NVS_PARTITION)`, independent of anything else in the boot.
  If that fails, `load_count()`/`persist_count()` are both skipped and the loaded count is
  forced to 0 -- i.e. **boot_guard's own counter already refuses to count a boot whose own
  storage it cannot see.** This is item 2(a) below, and it was already correct before this
  pass.
- Separately, `boot_confirm_is_healthy(nvs_ok, web_ok, ota_routes_ok)` gates
  `boot_guard_mark_healthy()` (the ONLY thing that clears the counter automatically). `nvs_ok`
  comes from `nvs_report_capture()` (`nvs_report.c`), which walks THREE partitions
  (`wifi_nvs`, `kiln_nvs`, `profiles_nvs`) via `esp_partition_find_first()` +
  `hal_kv_mount_probe()` and requires ALL THREE present and mounted.
- `nvs_report_capture()` is called **exactly once**, early in
  `main_network_http_bringup()` (`main_network_http.c`), and its result is captured into
  `main_ota_confirm_ctx_t.nvs_ok` **once**, then read every poll for the rest of the boot by
  `main_ota_rollback_confirm_task()`. There is no retry and no re-sampling.
- `hal_kv_mount_probe()` (`hal_kv_esp.c`) deliberately does NOT do the erase-and-retry
  `hal_kv_init_partition()` does (its own comment: "must never trigger an erase of a
  partition it does not own") -- it can report a partition "not mounted" (or, if
  `esp_partition_find_first()` itself misses it, "not present") in a way nothing downstream
  ever re-checks.

Put together, this is a real, code-provable gap independent of any single boot's hardware
behavior: **`boot_confirm_is_healthy()`'s only input is a one-shot snapshot with no retry**,
while `boot_guard_init()`'s own counting is already retry-tolerant (it just doesn't count when
it can't see its storage). A snapshot instant that catches even one of three unrelated
partitions mid-mount poisons the ENTIRE boot's health determination -- forever, for that
boot -- even though `boot_guard`'s own partition access (opened earlier, in
`main_boot_early.c`, before this snapshot runs) may already be working fine, and even if the
snapshotted partition becomes mounted a few hundred milliseconds later.

**INFERRED (plausible, not confirmed against hardware in this pass -- board access was off
limits)**: exactly why `kiln_nvs` specifically reports absent at that sampling instant right
after a `flash_firmware()` reset. `flash_firmware()` writes only the `factory` app partition,
never touches the partition table at `0x8000` (per `CLAUDE.md`), so a *static* partition
table cannot explain an intermittent "not present." The most likely candidate consistent with
the code above is a transient NVS/partition-table settling window in the first tens/hundreds
of milliseconds after a JTAG-flash reset, sampled by the one-shot `nvs_report_capture()` call
before every module that owns one of the three partitions has necessarily finished its own
mount. This was NOT reproduced live in this pass (no board access); the fix below does not
depend on pinning the exact cause, because it treats the entire class of "the one-shot health
snapshot was wrong for reasons that have nothing to do with the firmware" the same way.

## 2. What should count as an unhealthy boot

Three options were on the table:

- **(a) Don't increment when the counter's own storage is unavailable.** Already true --
  see "OBSERVED" above. Kept as-is; this pass does not touch it.
- **(b) Require a successful application-level milestone before counting.** Rejected: this
  inverts the module's whole purpose. `boot_guard.h`'s own doc comment is explicit that the
  increment must be persisted immediately, before anything later in the boot can crash --
  "an increment that only lived in RAM until some later 'healthy' point would never survive
  the very boot it exists to detect." Gating the INCREMENT on a health milestone would mean a
  board that panics between boot and that milestone is never counted at all -- exactly the
  failure mode this module exists to catch.
- **(c) Have `flash_firmware()` explicitly reset the counter as part of a deliberate flash.**
  **Adopted.** See section 4.

The new firmware-side piece is `boot_guard_reset_counter()` (`boot_guard.c`/`.h`): a clear
that does NOT go through `boot_confirm_is_healthy()`'s flaky one-shot snapshot at all. It
shares its actual NVS write-verify-retry logic with `boot_guard_mark_healthy()` (both now
call a common `clear_persisted_counter_verified_locked()`), so the fix already proven in
`0b5d9dad` for "the write claims success but doesn't stick" applies to this path too, for
free -- there is exactly one clear-and-verify implementation in the file, not two that could
drift apart.

Unlike `boot_guard_mark_healthy()`, `boot_guard_reset_counter()` does not require
`s_bg.initialized` and does not short-circuit on `s_bg.healthy_marked` -- it's a distinct
kind of event (an external, deliberate "this was a flash" declaration) from an in-boot
automatic health confirmation, even though both end up writing the same 0.

## 3. Interaction with the RTC marker (0b5d9dad)

`boot_guard_reset_counter()` arms the SAME RTC marker (`s_bg_rtc.marked_healthy = 1`) that
`boot_guard_mark_healthy()` arms, through the SAME shared helper. Walking the two failure
directions explicitly:

- **Could my change cause a case where NEITHER path clears the counter?** No: both paths
  fail identically (return `false`, marker not armed) whenever
  `clear_persisted_counter_verified_locked()` fails to verify -- there is only one
  implementation of "did the clear verify," so there is only one way for both to agree it
  didn't, and the RTC-stuck-escape from `0b5d9dad` still exists for exactly that case.
- **Could my change cause a case where the RTC marker gets armed WITHOUT the clear actually
  landing, permanently masking a genuinely broken board?** This was tested directly (see
  section 5's negative test) via a different, more realistic sabotage: wiring
  `boot_guard_reset_counter()` into `boot_guard_init()` unconditionally (simulating a future
  wiring mistake -- calling it from somewhere other than a deliberate, tool-confirmed flash).
  That DOES let a genuinely-failing board's counter get reset every boot and never trip
  recovery -- which is exactly why `boot_guard_reset_counter()` must only ever be called from
  an explicit, external, "I just flashed this on purpose" action, never from anything that
  runs unconditionally inside a boot. This is documented prominently in the function's own
  header comment as the one way to misuse it.
- **Does a legitimate reset ever combine with the stuck-counter escape to release a board
  that never actually flashed?** No: `boot_guard_counter_is_stuck()` only distrusts a loaded
  nonzero count when the PREVIOUS boot's RTC marker says a clear verified -- a board that was
  never reset and never marked healthy carries `marked_healthy = 0`, so the predicate is
  false regardless of the new function's existence.

## 4. Tool vs. firmware

The reset call belongs in the **tool** (`flash_firmware()`), not in firmware's own automatic
boot path, for one structural reason established while investigating this: **firmware
genuinely cannot distinguish "a developer just deliberately flashed this board" from "this
board is quietly reset-looping on its own."** Both look identical from inside `app_main()` --
a boot that starts, runs, and (for whatever reason) doesn't reach
`boot_confirm_is_healthy()`. Any unconditional in-firmware trigger for
`boot_guard_reset_counter()` collapses back into option (b)'s problem, or worse (the negative
test in section 5 demonstrates exactly this: wiring the reset into every boot unconditionally
defeats the counter entirely, for every board, healthy or not).

The TOOL, by contrast, has real, external knowledge the firmware cannot have: it just
initiated the flash, and (per `CLAUDE.md`) already does provenance tracking and post-flash
verification (`flash_firmware()`'s `verify` parameter polls the board's own HTTP API for the
running partition and build timestamp). The natural extension, **not yet implemented in this
pass** (it lives outside `firmware/`, and this session's host-test mandate was the C layer):
once `flash_firmware()`'s own verification confirms the NEW build is actually running, call a
new authenticated HTTP route (same shape as `POST /api/ota/esp/recovery_exit`, which already
exists in `ota_http_recovery.c` for a related purpose) that invokes
`boot_guard_reset_counter()` on the board. This is flagged as a follow-up rather than
implemented here to keep the C-side fix (which is what the host tests can actually prove)
separate from the Python/MCP-side wiring.

## 5. Host tests, protection intact, negative test

All in `firmware/KilnFW/App/test/test_boot_guard.c`, run via
`firmware/KilnFW/App/test/build_host_tests.ps1` (bash invocation -- PowerShell-tool
invocation of this script is unreliable per prior session notes). Full suite: **`main`
6966/6966 checks passed**, including every existing `boot_guard`/RTC-marker/legacy-key test
unchanged.

New tests:

- `test_reset_counter_keeps_normal_flashing_under_threshold` -- five consecutive simulated
  "flashes" (`boot_guard_init()` then `boot_guard_reset_counter()`, standing in for the tool
  calling the not-yet-built HTTP route after each verified flash) never reach
  `RECOVERY_MODE_BOOT_THRESHOLD`; each boot's count is 1, never accumulating.
- `test_a_genuinely_failing_boot_still_trips_recovery` -- the same
  `RECOVERY_MODE_BOOT_THRESHOLD`-boot sequence with NOTHING calling either
  `boot_guard_reset_counter()` or `boot_guard_mark_healthy()` still enters recovery mode
  exactly as before this change -- the core protection is unweakened.
- Every pre-existing test (`test_counter_increments_and_enters_recovery`,
  `test_mark_healthy_clears_counter`, `test_stuck_counter_escape`,
  `test_legacy_record_is_read_once_then_retired`, etc.) still passes unmodified.

**Negative test** (proving the protection test above is not vacuous): temporarily wired
`boot_guard_reset_counter()` into `boot_guard_init()` unconditionally --

```c
xSemaphoreGive(s_bg.lock);
boot_guard_reset_counter(); /* NEGATIVE-TEST SABOTAGE */
```

Re-ran the suite. Shortest failing line:

```
FAIL C:\...\test_boot_guard.c:585: NEGATIVE-TEST TARGET: a genuinely failing boot (nothing ever resets or confirms it) still trips recovery mode after RECOVERY_MODE_BOOT_THRESHOLD boots -- proves boot_guard_reset_counter() is an additive escape hatch for a deliberate flash, not a general weakening of the counter
```

(11 other pre-existing `boot_guard` checks also failed under this sabotage, all downstream of
the same broken invariant.) Reverted the sabotage line by hand (deleted the one inserted
call + comment, no other edits); confirmed with `git diff` that
`firmware/KilnFW/App/drivers/persist/boot_guard.c` matches the intended fix with zero trace of
the sabotage. Re-ran the full suite: `main` 6966/6966 passed again.

An earlier, weaker attempt at this negative test (hardcoding
`boot_guard_reset_counter()`'s local `verified` to `true` without doing the real write)
**passed all tests despite being broken** -- worth recording as its own small lesson: that
sabotage accidentally armed the RTC "verified clear" marker exactly the way a real successful
reset would, which then triggered the (correct, `0b5d9dad`-era) stuck-counter escape and made
`boot_guard_init()` treat the un-cleared persisted count as "stuck" and drop it to 0 anyway --
rescuing the sabotage by accident, through a mechanism built for a different failure. The
sabotage that actually stresses the right invariant is one that defeats the WIRING (call the
reset from the wrong place), not one that lies about a single function's own return value.

## Files touched

- `firmware/KilnFW/App/drivers/persist/boot_guard.c` -- `clear_persisted_counter_verified_locked()`
  extracted from `boot_guard_mark_healthy()`; new `boot_guard_reset_counter()`.
- `firmware/KilnFW/App/drivers/persist/boot_guard.h` -- `boot_guard_reset_counter()` declared
  and documented.
- `firmware/KilnFW/App/test/test_boot_guard.c` -- two new tests (above).
- `CLAUDE.md` -- boot_guard paragraph extended with this finding.

## 2026-09-09 follow-up: the tool-side wiring is now done

Section 4's "natural extension, not yet implemented in this pass" is implemented:
`POST /api/ota/esp/boot_guard_reset` (`ota_http_recovery.c`'s
`ota_boot_guard_reset_post_handler()`, its own auth context
`OTA_HTTP_CONTEXT_BOOT_GUARD_RESET`) calls `boot_guard_reset_counter()` directly, and
`flash_firmware()` (`tools/PcTools/src/kilnctrl/mcp_server_flash.py`) calls it -- via the new
`ota_http_client.boot_guard_reset_esp()` -- ONLY once its own post-flash verification
(`_verify_flash_landed()`) returns `""` (full, unambiguous success: running partition is
`factory` AND its build timestamp matches the `.bin` just flashed), gated by a new optional
`ap_password` parameter (omitted by default -- an opt-in, not a behavior change for existing
callers). Never called on a raise, a WARNING, or `verify=False`, proven by host tests on both
sides (`test_ota_http.c`'s handler tests with a controllable `boot_guard_reset_counter()` stub;
`test_flash_firmware_verify.py`'s `BootGuardResetWiringTest`, which patches
`ota_http.boot_guard_reset_esp` and asserts it is NOT called on every non-full-success path).
Also added, per this doc's own suggested follow-up: `GET /api/boot_guard`
(`ota_boot_guard_status_get_handler()`, unauthenticated, same exposure level as
`GET /api/status`) reporting `{"boot_count", "recovery_mode"}` -- so this class of fix no longer
needs a JTAG memory read of `s_bg` to verify (`ota_http_client.get_boot_guard_status()`).

`boot_guard_reset_counter()`'s own lying-write path (both the first write and the bounded
erase-then-retry lying) is now exercised directly against real `boot_guard.c`, not just
`boot_guard_mark_healthy()`'s: `fake_kv_script_silent_set_noops()` was added to
`firmware/hwAbstraction/host/fake_kv.c`/`.h` alongside the existing erase-only noop, since
`persist_count()` writes via `hal_kv_set_blob()`, which the erase-only fake could not lie about.
One incidental finding surfaced while writing that test: when BOTH the first write and the
retry's write lie, the retry's own real `hal_kv_erase_key()` (not noop'd) still removes the
record, so the NEXT boot's `load_count()` reads a missing record as 0 rather than the stale
pre-clear value -- `boot_guard_reset_counter()` still correctly reports `false` for the boot
that made the call (the read-back could not confirm it that boot), but the persisted state
does not stay "stuck," it becomes "accidentally reads as 0 later." Not a defect worth chasing
further: it is strictly no worse than the stuck-at-stale-value case this whole audit is about.

## Owner decision, 2026-09-19

The `ap_password` gate above is no longer opt-in: the post-flash `boot_guard_reset` call is now
DEFAULT ON whenever a credential is available at all. `flash_firmware()` resolves the password
via (in order) an explicit `ap_password` argument, then the `KILNCTL_WEB_USERNAME`/
`KILNCTL_WEB_PASSWORD` environment variables (the same pair `http_auth.py` reads for the admin
session). All of section 4's gating is otherwise unchanged -- still only called once
`_verify_flash_landed()` returns `""` (full, unambiguous success), never on a raise, a WARNING,
or `verify=False`. A new `reset_boot_guard=False` parameter opts out unconditionally regardless
of credential availability. A caller with no credential from either source gets the
pre-`b09294fb` behavior, but the result now says explicitly that the reset was skipped for lack
of credentials rather than staying silent about it. See `tools/PcTools/src/kilnctrl/
mcp_server_flash.py`'s `_resolve_boot_guard_password()`/`_maybe_reset_boot_guard()` and
`tests/test_flash_firmware_verify.py`'s `BootGuardResetWiringTest` for the env-fallback,
opt-out, and no-credential-skip tests.

## Not done in this pass (original, 2026-09-08)

- ~~No HTTP route wired up, and `flash_firmware()` itself was not changed -- see section 4.~~
  Done 2026-09-09, see above.
- The exact hardware cause of the transient `kiln_nvs not present` sample was not reproduced
  live (board access was off limits for this task). The fix does not depend on that
  diagnosis; it treats the whole class of "the one-shot post-flash health snapshot can be
  wrong for reasons unrelated to firmware correctness" the same way, via an explicit
  tool-driven escape rather than trying to make `nvs_report_capture()` itself retry-safe.
