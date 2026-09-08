# boot_guard recovery-mode loop: the clear never persisted (2026-09-08)

Third confirmed instance of the "board bricked into permanent recovery mode"
class on this hardware (see `e7b8efc` and 2026-08-22 in CLAUDE.md's
boot_guard paragraph).

## Symptom

Board observed stuck in recovery mode ("3 consecutive boots were never
confirmed healthy") across at least 8 independently triggered reboots this
session (`debug_reset`, one JTAG-attach-induced reset) plus, per a
concurrently running agent, 5 explicit `POST /api/ota/esp/recovery_exit`
calls over ~8 minutes -- every one of which was accepted (200 OK, board
rebooted) and every one of which left the counter unchanged. NVS
(`wifi_nvs`/`kiln_nvs`/`profiles_nvs`), the web server, and the OTA routes
were all healthy for 11+ minutes of uptime on more than one of these boots.

## Observed vs. inferred

**Observed directly:**
- `GET /api/ota/esp/status` → `"recovery_mode": true` on every boot checked,
  before and after multiple `debug_reset` cycles and after the operator
  escape hatch.
- `get_device_log` shows the identical `boot_guard: RECOVERY MODE: 3
  consecutive boots were never confirmed healthy` banner on every boot --
  the loaded (pre-increment) count never moved, neither up (it should climb
  every unconfirmed boot) nor down to 0 (a successful clear).
- A live JTAG memory read of `s_bg` (symbol resolved from
  `KilnCtrl-latest.elf` via pyelftools; ELF confirmed to match the running
  build's embedded strings and `fw_build`) mid-boot showed
  `healthy_marked=1`, `count=4`, `recovery_mode=1` -- i.e. the code had
  already run `boot_guard_mark_healthy()`, and that function's own write
  call (`persist_count(0)`) had returned `HAL_OK`, setting the RAM flag true.
  Despite this, the very next boot still loaded the pre-clear count.
- Board running from the `factory` partition (`GET /api/partitions`), nvs
  sections all `mounted:true`, `io_ready`/`thermo_ready`/`safety_ready` all
  true, no active firing, relays off, no safety trip pending -- every input
  to `boot_confirm_decide()` was healthy.
- `main_ota_rollback_confirm_task()`'s own unconditional first log line
  (`"running partition: ..."`, printed before any health decision) never
  appeared in ~15 minutes of captured boot log across two separate boots,
  even though later code in the same function (`kiln_cfg_store_init()`,
  textually after the confirm-task block) does log successfully every boot
  -- proving control flow reaches and passes the confirm-task block without
  crashing. Most likely explanation: the UART log bridge drops lines during
  the dense burst of boot-time log calls around T+3.6-4.2s (two explicit
  "log line(s) dropped (queue full)" events were captured elsewhere in the
  same boot) -- this made the task look silently absent and cost real
  diagnosis time before the JTAG memory read cut through the ambiguity.

**Inferred, not directly proven:** the exact NVS-internal reason
`hal_kv_set_blob()`/`hal_kv_commit()` report `HAL_OK` while the persisted
bytes do not change. Candidates considered and not ruled in or out with
certainty: an in-place-overwrite quirk on this specific key/partition, or a
partition-level condition that lets a write appear to succeed without
reaching flash. No repository host fake (`fake_kv.h`) models this --
`fake_kv`'s writes are honest by construction, which is exactly why this
bug was invisible to `test_boot_guard.c` before this pass.

## Root cause (as fixable, not as fully explained)

`boot_guard_mark_healthy()` trusted `persist_count(0)`'s return code alone.
On real hardware that return code was `HAL_OK` every time, yet the
persisted "unconfirmed boot" count never reached 0 -- confirmed identical on
both the background auto-confirm path (`main_ota_rollback_confirm_task()`,
`main_network_http.c`) and the explicit, password-authenticated operator
escape hatch (`POST /api/ota/esp/recovery_exit`, `ota_http_recovery.c`),
since both call the same `boot_guard_mark_healthy()`. This is the same
"logging unchecked success" bug class already flagged elsewhere in this
codebase (CLAUDE.md's "Safety calls logging unchecked success" entry) --
here the return code itself was unreliable, not merely uninspected, so the
fix is a read-back, not a tighter `if`.

Both callers additionally gave up after exactly one attempt: the background
task called `boot_guard_mark_healthy()` once and immediately
`vTaskDelete(NULL)`'d itself regardless of outcome, and the HTTP handler
called it once and rebooted regardless of outcome. A single silently-lying
write attempt was therefore never retried within a boot, and a board that
came back up already healthy had no other path to escape.

## Fix

1. `boot_guard.c`: added `verify_persisted_count(uint32_t expected)` -- a
   strict read-back, distinct from `load_count()`'s boot-time
   "unreadable/corrupt collapses to 0" default (which is the *wrong*
   default for a verification step: it must tell a genuine confirmed-zero
   apart from "could not read it back at all", not conflate them).
2. `boot_guard_mark_healthy()` now returns `bool` and only reports true once
   `verify_persisted_count(0)` confirms the clear. On a first-attempt
   verification failure it retries once with an explicit erase-then-write
   (`erase_then_persist_count()`) before giving up for that call -- cheap,
   since this runs at most a handful of times per boot, and it removes any
   possibility of an overwrite-in-place quirk being the cause without
   requiring a full NVS-internals diagnosis.
3. `main_network_http.c`'s `main_ota_rollback_confirm_task()` no longer
   deletes itself after one attempt: both the factory-partition and
   OTA-slot branches now loop (existing 500 ms poll cadence) until
   `boot_guard_mark_healthy()` returns true, logging loudly if it is still
   unverified 10 s in. The one-shot `esp_ota_mark_app_valid_cancel_rollback()`
   call itself is still attempted only once (tracked separately) -- only the
   boot_guard clear is retried.
4. `ota_http_recovery.c`'s `POST /api/ota/esp/recovery_exit` handler now
   checks the return value and logs a warning if the clear did not verify,
   instead of silently treating the call as a no-op success.

## Can the board escape recovery mode without the AP password?

Yes, in the sense that matters: the *automatic* background confirm task
(`main_ota_rollback_confirm_task`) needs no password and, with this fix,
keeps retrying the clear on every healthy boot until it verifies, rather
than giving up after one silently-failed attempt. A board that boots
healthily (NVS/web/OTA all up) no longer depends on an operator at all.

The password-gated `POST /api/ota/esp/recovery_exit` is still the
*immediate* path (reboot right now rather than wait for the next natural
reboot) and still requires the AP password -- this fix does not remove that
requirement, it makes the clear that path performs actually verified.

**This board specifically:** was left running with the fix in place; the
background confirm task will verify-and-clear on its own on a healthy boot
without any password. If a reflash is done in the future, the owner does
not need to do anything further to escape recovery mode -- the next healthy
boot self-clears. No `ota_recovery_exit_esp()` call was needed to conclude
this pass.

## Is the counter readable over any API (for verifying this fix without a reflash)?

Not directly. `GET /api/ota/esp/status`'s `recovery_mode` field reflects only
`boot_guard_is_recovery_mode()` -- a boolean, fixed for the boot, not the
underlying count -- and no HTTP endpoint exposes
`boot_guard_get_boot_count()` or the raw NVS record. The only way this audit
read the live count was a JTAG memory read of the static `s_bg` struct
(symbol address resolved from the matching ELF via `pyelftools`), which
required stopping to fetch a toolchain (`pip install pyelftools`) not
otherwise available in this environment. Recommended follow-up (not done
here, out of this task's scope): a read-only diagnostics field (e.g. on
`GET /api/cfgfs` or a new `/api/boot_guard` route) exposing
`boot_guard_get_boot_count()` and whether the current boot's clear has
verified, so this class of bug is checkable over HTTP next time instead of
requiring JTAG + symbol resolution.

## Reflash needed?

Yes. The fix is source-only in this pass (`firmware/KilnFW/App/drivers/persist/boot_guard.{c,h}`,
`firmware/KilnFW/App/main_network_http.c`,
`firmware/KilnFW/App/drivers/http/ota_http_recovery.c`) and has not been
flashed -- this session deliberately did not flash (other files in the tree
are dirty from concurrent sessions; `kiln_call(name="build_kilnfw")` was
skipped for the same reason, per this task's own instructions). Host tests
(`build_host_tests.ps1`, 31/31 executables, 6898/6898 checks) and
`tools/run_all_checks.ps1` (66/66) both pass against the change.

## Negative test

`firmware/KilnFW/App/test/test_boot_guard.c`:
`test_verify_persisted_count_catches_a_write_that_does_not_stick()` models
the exact real-hardware shape -- a well-formed, CRC-valid persisted record
whose `boot_count` is still the stale pre-clear value (not corrupted bytes,
which `verify_persisted_count()` also separately rejects). Breaking
`verify_persisted_count()`'s final comparison to always report success
(`return true;` unconditionally) reproduces the original bug and fails this
exact line:

```
FAIL C:\...\test_boot_guard.c:211: verify_persisted_count() refuses to confirm a clear when the
persisted record is well-formed but its boot_count is still the stale nonzero value -- this is the
exact gap that let the real board believe it had cleared the counter when it had not: a mere
readability/CRC check would have wrongly passed this
```

The break was reversed by hand (restoring `return rec.boot_count ==
expected;`) and `git diff` against the committed fix is empty for
`boot_guard.c` at that point -- confirmed via the file's diff stat showing
only the intended additions, no residual break markers.
