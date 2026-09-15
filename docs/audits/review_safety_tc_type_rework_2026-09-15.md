# Review: safety tc_type rework (commits 6311675f, 34779f0a, ab625bcc)

2026-09-15. Adversarial, read-only review. No flashing. It follows up on
`review_safety_tc_type_settable_2026-09-15.md` (commit 5871326b, findings F1-F7).
It checks the rework against the owner's decisions:

- tc_type is settable while ARMED only when heat-enable is off AND no firing is running.
- The Pico stays armed throughout and reapplies the type immediately.
- Every other parameter is still refused while ARMED.
- The commissioning page owns the type. The ESP only reads it back.

## Verdict

**Not shippable.** HEAD does not build. The "heat is safe" test does not
implement the owner's rule, and the ESP "read-back" displays the ESP's own stale
copy of the value, not the Pico's.

## History check (the "revert")

No `git revert` commit exists. The event the implementer described is 6f7f462a.
That commit belongs to another session. It rolled `safety_link_poll.c` back to
its 1c8d7f6e content, because that session's earlier `commit -o` had swept in
this task's uncommitted removal of the push.

ab625bcc then re-applied that removal. `git diff 1c8d7f6e ab625bcc` and
`git diff HEAD` for that file are both empty. `safety_sync_tc_type` is gone.
The `heat_enable.h` include and the `heat_enable_service_pending_release()`
hunks from 1c8d7f6e are still present.

So nothing was lost or duplicated in that file. The actual loss is elsewhere
(B1).

## Findings

### B1 (BLOCKER): HEAD does not build
The following three files are **untracked** in the main tree and missing from
every commit:
- `firmware/SaftyFW/src/tasks/tc_type_reapply_policy.c`
- `firmware/SaftyFW/src/tasks/tc_type_reapply_policy.h`
- `firmware/SaftyFW/test/test_tc_type_reapply_policy.c`

HEAD still depends on them:
- `link_task.c:94` includes the header.
- `link_task.c:1555`, `:2260` and `:2359` call `tc_type_reapply_policy_should_reapply()`.
- 6311675f added the .c file to `CMakeLists.txt` and `build_host_tests.ps1`, and
  added `run_test_tc_type_reapply_policy()` to `test_main.c`.

This was reproduced in a clean worktree at HEAD (`C:\wt\tcrv`).
`build_host_tests.ps1` fails with `C1083: Cannot open source file ...
tc_type_reapply_policy.c`. The SaftyFW target build fails the same way.

The commit message says "CMakeLists.txt already listed tc_type_reapply_policy.c;
confirmed". That is false: this same commit adds the line. The "94/94 green" and
"from-scratch target build" results came from the dirty tree, which is the
pattern in the "green build in dirty tree" memory.

The F2/F4/F5 fixes (the reapply calls from COMMIT and volatile, and the policy
test) exist only as uncommitted work. Any other session's `git clean` or stash
would lose them.

### H1 (HIGH): "heat safe" is true inside a firing's relay-OFF window
`link_task_heat_is_safe_for_tc_type_change()` (`link_task.c:1504-1507`) is:

```
!s_relay_on_continuous && !current_task_any_current_present()
```

`s_relay_on_continuous` is an instantaneous copy of the ESP's last
`relay_now_mask`. It is set false on any context frame whose mask is 0
(`link_task.c:1251-1259`). CT current is also absent whenever the contactor is
open.

A running firing time-proportions its relays, so between pulses both inputs
read "off". No firing-running input is consulted anywhere: not `REQUEST_ENABLE`
(`link_task.c:1329`, which forwards and stores nothing), not the firing ceiling
(`s_firing_ceiling_have`), and not relay_owner's energize request.

The owner's rule was "heat-enable off AND no firing running". The code
implements "the relay happened to be off in the last context frame".

**Scenario:** a firing at 1000 C sits in a PWM off-phase. Someone commits K->S
from the commissioning page, and the write is accepted.

- thermo_task reconfigures and reports invalid readings until the CR1 readback
  passes.
- If the operator picked the wrong type for the fitted probe, readings are
  implausible, and S5 eventually trips mid-firing.
- If the new type's band still covers the misread value, readings are
  plausible-but-wrong under the new curve. The Pico's over-temperature backstop
  then evaluates a wrongly linearised temperature for the rest of the firing.

There is also a staleness hole. If context frames stop, `s_relay_on_continuous`
keeps its last value. It is also reset to false on link re-init
(`link_task.c:2874`), so a link that has just come back reads "heat safe".

**Fix:** require an explicit "no firing / heat-enable released" state that the
Pico owns. The latest REQUEST_ENABLE(false) with no later enable, or
relay_owner's energize request being false, fits. Combine it with a fresh
context frame and no relay-on within the last N seconds, not just the latest
instant.

### H2 (HIGH): the ESP "read-back" shows the ESP's own copy, which now never updates
- `zones_http_get.c:305` serialises `s_zones.cfg.safety_tc_type`, the ESP's zones
  config value.
- `zones_page.html:1816-1823` renders that value as the safety type.

After 34779f0a nothing ever copies the Pico's value into that field. The POST
path keeps the old value (`zones_http_post.c:239`), and there is no GET_PARAM
0x0105 mirror-back.

So a type committed on the commissioning page leaves the zones page showing the
old type indefinitely. The only other writer is backup import
(`backup_import.c:1208`, `zones_config_set_safety_tc_type`), which can set the
displayed value to anything, again without touching the Pico.

This is exactly the "ESP believes X, Pico runs Y" state F3 flagged, moved from
the sync logic into the UI. The divergence check does catch a Pico that differs
from the saved kiln package (`safety_cfg_store.c:184`). The zones page itself is
never corrected.

**Fix:** render the Pico-mirrored 0x0105 value from the safety config page
cache, or drop the field from zones_cfg. Also stop backup import from writing it.

### M1 (MEDIUM): a tc_type-only COMMIT while ARMED persists volatile-installed fields to flash
`config_store_write_ex()` (`config_store_flash.c:1019`) compares the candidate
against `s_cached_record`, not the flash record. After an
`APPLY_CONFIG_VOLATILE`, the RAM cache holds values that were never persisted.
`s_staged_config` and `config_store_get_full_record()` both derive from the
cache.

A later tc_type-only COMMIT or SET_CONFIG therefore passes the "only tc_type
differs" test and writes the whole volatile record to flash while ARMED. That
also persists fields that were deliberately volatile, which is an ARMED flash
write of non-tc_type content.

**Fix:** compare against the last flash-committed record, or refuse the
relaxation while the cache is volatile-dirty.

### M2 (MEDIUM): the check and the apply are not atomic
The heat-safe sample is taken on link_task before `config_store_write_ex()`
schedules the flash erase and program, and the reapply happens later on
thermo_task. A REQUEST_ENABLE or relay-on that arrives in between is not
re-checked.

This is narrow (tens to hundreds of ms), but combined with H1 it means nothing
holds heat off during the reconfigure window. The Pico does stay ARMED, as
intended.

**Fix:** once H1 has an owned firing/enable state, refuse enable while a
reapply is pending, or re-check after the write.

### M3 (MEDIUM): the SET_CONFIG and COMMIT_CONFIG comparators differ in practice
F2 parity is only partial:
- **SET_CONFIG** builds its record by read-modify-write of the committed record
  (`link_task.c:1532-1535`). Its diff really is tc_type-only.
- **COMMIT_CONFIG** writes `s_staged_config` after `config_params_finalize_i_present_a()`
  and a recomputed `calibration_missing` (`link_task.c:2234-2235`).

If there is any staged but uncommitted SET_PARAM, or the finalize and recompute
change a byte, the commissioning page's tc_type change is refused with the
generic ARMED reason. The page is the owner's chosen path, and the operator
gets no hint about which other field blocked it. That is fail-safe, but the
feature can look broken.

A test that drives COMMIT_CONFIG through link_task with only 0x0105 staged
while ARMED is needed. No such test exists: the new tests exercise only the
pure decision function and `config_store_write()`.

### L1 (LOW): static comparator buffers
The two static `CONFIG_STORE_RECORD_LEN` buffers in
`config_store_only_tc_type_differs()` (`config_store.c:1175-1176`) are safe
today:
- `config_store_write()` and `config_store_write_ex()` are called only from
  link_task (`link_task.c:1540`, `:1620`, `:2240`).
- The function itself has no other caller.

Nothing enforces this, though. A future caller on the other core, such as
current_task's CT-cal path, would race silently.

**Fix:** add an assert that the caller is link_task, or a check script.

The comparator logic itself is sound:
- It packs both records and zeroes format_version, seq and tc_type.
- It ORs in only the `SET_TC_TYPE` bit.
- It then compares everything before the CRC.

So a mixed change cannot pass it: any other byte, including any other
fields_set bit, differs. Clamped out-of-range tc_type values are rejected
upstream (`link_task.c:1521`).

### L2 (LOW): an existing test was edited to expect the new reason
`test_config_store_flash.c:173-191` changed its expectation from
`REFUSED_ARMED` to `REFUSED_ARMED_HEAT_ON` via the plain `config_store_write()`
wrapper. That wrapper now returns a "heat is on" message when heat may well be
off. Its reason is really "caller did not supply heat state".

This is misleading for the SET_CT_CAL path (`link_task.c:1620`) if its record
ever differed only in tc_type.

## Checked and OK
- **ARMED is kept.** No path disarms or drops to GRACE, and the reapply never
  touches relay_owner.
- **Invalid readings during reapply.** Readings are marked invalid while
  `max31856_tc_type_verified()` is false (`thermo_task.c:612-613`). S5 needs
  streak, time and blind grace, so a quick reconfigure does not trip, and a
  wrong type fails safe to TRIPPED. Nothing disarms.
- **The other ARMED refusals are intact.** `config_store_decide_write_ex()`
  (`config_store.c:1126-1145`) returns `REFUSED_ARMED` for any non-tc_type-only
  change. The volatile path keeps its own stricter check.
- **Persistence (F4).** Accepted writes go through the existing verified flash
  write, and the boot read at `main.c:525` uses the stored type.
- **F5/F6.** The divergence test now drives the real 0x0105/U8 row
  (`test_safety_ceiling_sync_divergence.c`, via c1d2c526). The F5 policy test
  exists only uncommitted (B1).
- **abs_max_temp_c** equality and the divergence handling are untouched by
  these commits.
- **Leftover ESP writers.** `safety_link_send_set_config()` remains reachable
  only via the PC bridge (`uart_bridge_safety.c:165`), which is an explicit
  operator/PC path gated by the same Pico rule. The push from the poll loop is
  gone.
- **PcTools.** `zones_http_client.py:624-634` moves `safety_tc_type` to
  read-only, which matches `zones_http_post.c`.
- **F7.** The stale `main.c` comment is fixed.
