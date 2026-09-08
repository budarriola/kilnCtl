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

---

## Pass 3 (2026-09-08): the mechanism, observed rather than reasoned

Two earlier passes both failed to free the board. This pass instrumented the
firmware and read the answer off the hardware. Every claim below is marked
OBSERVED (seen in a device log or an HTTP response) or INFERRED.

### The four hypotheses that were on the table

1. **Boot-time read and the write target different storage.** REFUTED
   (OBSERVED). Instrumented `load_count()` and `persist_count()` log their
   partition/namespace/key and every step's `hal_status_t`. Both use
   `kiln_nvs` / `boot_guard` / `count`, and both reported `HAL_OK`.
2. **The counter is re-incremented after being cleared, on the same boot.**
   REFUTED (OBSERVED). Exact order in one boot's log: `persist_count(4)` at
   3016 ms (from `boot_guard_init()`), then `mark_healthy` entered at
   4036 ms and `persist_count(0)` at 4076 ms. Nothing writes the record
   after that; the confirm task then deletes itself.
3. **The NVS write fails at a layer that still reports success.** PARTLY, and
   for none of the usual reasons (OBSERVED): key and namespace lengths are
   legal (`check_nvs_key_length.ps1` passes; both are well under 15 chars);
   the partition mounts (`hal_kv_init_partition('kiln_nvs')` -> `HAL_OK`);
   it is nowhere near full (`nvs_get_stats`: **used 306, free 1710, total
   2016 entries**); `nvs_commit` is reached and returns `HAL_OK`.
   Freshly-created probe keys written on the *same boots* to the *same
   partition* -- a `u32` and a 4-byte blob in the `kiln_cfg` namespace --
   incremented correctly across every reboot (`probe READ kiln_cfg/p_blob
   ... val=7`, then written as 8). A user-visible scalar behaved the same:
   `POST /api/ramp_assist enabled=1` came back after a reboot as
   `ramp assist: ENABLED (source=NVS, rev=1)`. **So NVS, the partition, the
   key lengths and the write path are all healthy; it is this one record
   that is frozen.**
4. **The read-back verified a cached value, not flash.** **CONFIRMED
   (OBSERVED) -- this is the mechanism.** In one boot:

   ```
   E (4076) boot_guard: BGDIAG persist_count(0): set_blob=HAL_OK
   E (4076) boot_guard: BGDIAG persist_count(0): commit=HAL_OK
   E (4106) boot_guard: BGDIAG readback: open=HAL_OK get=HAL_OK len=12 ver=1 count=0 crc_ok=1
   ```

   and then, on the very next boot:

   ```
   E (2996) boot_guard: BGDIAG load_count: get=HAL_OK len=12 ver=1 count=3
   ```

   The read-back is not a flash read. `nvs_open()` does **not** re-read
   flash: NVS builds one in-RAM index per partition at
   `nvs_flash_init_partition()` time, and every handle on that partition --
   a new handle, a READ_ONLY handle, a different namespace -- is served from
   it. A read-back performed in the same boot as the write therefore returns
   what was just written whether or not it ever reached flash.

### Why this is the third instance, and the rule to take away

`0b6e82b7` added `verify_persisted_count()` specifically to stop trusting the
write's return code. It reopens a handle and compares the value -- and it is
*structurally incapable* of detecting this failure, for the reason above. It
verified `0`, and the next boot read `3`, on hardware, repeatedly.

> **An in-boot read-back of an NVS write proves nothing about flash.** The
> only honest verification of "did this survive" is a read on a *later boot*.
> Anything that must be checked across a reboot needs state that is not the
> thing being checked -- RTC slow memory, or a second, independent record.

Same shape as CLAUDE.md's "reset one side of a pair" class: two pieces of
state (the flash record, and the module's belief about it) joined by a
contract nothing enforces, both sides internally consistent, only the
*relationship* broken -- and therefore silent.

### Also learned along the way (all OBSERVED, all worth knowing)

* **The first boot after `flash_firmware()` has no partition table.** That
  boot logs `nvs_report: partition 'kiln_nvs' not present in flashed
  partition table`, `hal_kv_init_partition('kiln_nvs') failed:
  ESP_ERR_NOT_FOUND`, `SPIFFS: spiffs partition could not be found`, and a
  nonsense `largest=838860800` heap figure. `boot_confirm_is_healthy()` is
  false for the whole of it, so `boot_guard_mark_healthy()` is never called
  and nothing is persisted. **Never judge persistence behaviour from the
  boot immediately after a flash -- reboot once more first.**
* **`debug_reset` (JTAG) clears RTC slow memory.** `RTC_NOINIT_ATTR` state
  survives a panic, a watchdog reset and `esp_restart()`, but not an
  OpenOCD-driven reset. Every reboot in this investigation was a JTAG reset
  or a panic (`reset_reason` was `panic/exception` on every single boot),
  which is why the RTC-backed escape below could not be demonstrated from
  the bench.
* **`ESP_LOGI` from the `ota_confirm` task does not reach the captured log**
  (uart_log_bridge drops lines). Its `running partition:` and `boot-guard
  counter cleared and VERIFIED` INFO lines are absent from every clean-build
  boot log, which made the task look dead; raising the same lines to
  `ESP_LOGE` in the diagnostic build showed it running normally every boot.
  **Do not conclude a task never ran from missing INFO lines.**
* **OpenOCD cannot read this board's flash**: `flash probe 0` fails with
  `Failed to get flash maps` / `Failed to probe flash, size 0 KB`, so the
  NVS pages cannot be dumped and parsed offline. That is the one measurement
  that would have named the flash-level defect exactly.

### What was changed

1. **The stuck-counter escape** (`boot_guard.c`,
   `boot_guard_counter_is_stuck()`). A one-bit marker in RTC slow memory
   records "the previous boot verified a clear". If the next boot still
   loads a non-zero count, the persisted counter is provably not being
   updated, so the loaded count is treated as 0 (with a loud `ESP_LOGE`).
   This cannot release a genuinely reset-looping board: the marker is only
   ever set by a boot that reached `boot_confirm_is_healthy()` *and*
   verified its own clear -- exactly the boots whose counter the existing
   code already zeroes. A power cycle clears RTC memory, so the escape
   re-arms from scratch (one recovery boot at worst) rather than permanently
   disabling the guard.
2. **The record moved out of the frozen location**: `boot_guard`/`count` ->
   `kiln_cfg`/`bootguard`, with a one-time read of the old location so an
   upgrading board keeps its counter history, and a best-effort erase of the
   old record. A first attempt that moved only the *key* (`count` ->
   `bootcnt2`) inside the `boot_guard` namespace was built and flashed and
   made no difference at all, which is why the namespace moved too.

Host tests: `test_stuck_counter_escape()` and
`test_legacy_record_is_read_once_then_retired()` in `test_boot_guard.c`, both
driving the real `boot_guard_init()` / `boot_guard_mark_healthy()`.
Negative test (escape logic deleted from the production function, not from a
test-local copy):

```
FAIL test_boot_guard.c:434: the boot after a verified-but-lost clear does NOT
enter recovery mode -- without the RTC-backed stuck-counter escape the board
is trapped forever, which is exactly what happened on hardware
```

Restored by hand afterwards (no `git checkout --`): the file's md5 checksum
returned to the exact value recorded before the break, and no `NEGATIVE TEST`
marker remains anywhere in the file.
All 31 host-test executables pass; `tools/run_all_checks.ps1` reports
66 passed, 0 failed.

### STILL OPEN -- needs the owner

**The board has NOT left recovery mode.** After both changes were flashed and
the board rebooted several times, the banner is still, verbatim:

```
E (2936) boot_guard: RECOVERY MODE: 3 consecutive boots were never confirmed healthy (threshold 3).
```

The count is pinned at exactly 3 -- a value no boot has written since this
started -- through a key change and a namespace change, while other modules'
writes to the same partition on the same boots persist normally. INFERRED:
something at the flash level under `kiln_nvs` is serving a stale snapshot for
this record specifically, which no change inside `boot_guard.c` can reach.

Two owner decisions, least destructive first:

1. **Power-cycle the board.** Every boot in this whole investigation was a
   JTAG reset or a panic (`reset_reason: panic/exception`, every time);
   there has been no true power-on reset. It costs nothing, is the one
   untried action, and also re-arms the RTC escape cleanly. Do this first
   and check the banner.
2. **If that does not clear it: a one-time erase.** The narrow target is the
   NVS namespace `boot_guard` in the `kiln_nvs` partition (key `count`); the
   only tool available is a whole-partition erase,
   `nvs_flash_erase_partition("kiln_nvs")` (reachable today only through
   `factory_reset.c`'s scoped erase path). **That is destructive**: it also
   discards `zones_cfg` (the tuned PID gains, currently on-disk v19),
   `relay_cyc` (contact cycle counts 2201/2994/3214), `kiln_cfg_store`,
   `unit_pref`, `ramp_assist`, `touch_cal` and `dwwin`. Take a
   `GET /api/backup/export` first. **Nothing was erased by this pass; this
   is a proposal awaiting approval.**

Note for whoever picks this up: `kiln_cfg_store blob is the wrong size --
treating as corrupt`, `zones_cfg from 'kiln_nvs' loaded (on-disk version 19)
as v23` and `relay_cycles: migrated relay cycle blob v1 -> v2` appear on
*every* boot. All three are in-RAM migrations only written back on an
explicit save, so they are not (yet) evidence of further frozen records --
but if the erase happens, they are the first things to re-check afterwards.
