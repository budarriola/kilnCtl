# Independent review of the 2026-09-08 unreviewed safety-relevant changes

Reviewed 2026-09-08 from the code, not the commit messages or the authoring
agent's self-reports. Nothing in this pass was changed except this file. Suite:
`tools/run_all_checks.ps1` -- **71 passed, 0 skipped, 0 failed**.

Scope: `24090c9a` (RP2040 config-store A/B sectors, FLASHED to the Pico),
`bc0befd0`+`1f741635` (flash-worker boot ordering), `d6b643a4` (cold junction
carried independently on the safety link), and the httpd stack pass
(`d2f6fb02`, `0d4bb8eb`, `eb92592c`, `3a20a560`, `4bbfcfbe`).

## Verdicts

| Item | Verdict |
| --- | --- |
| 1. config_store A/B sectors | **Sound.** Atomicity, arbitration and migration all hold. Two pre-existing weaknesses in the same failure class are named below (D2, D4). |
| 2. flash-worker boot ordering | **Sound.** One stale comment (D5). |
| 3. cold junction / `flags2` bit | **Defect found (D1).** The wire bit itself is genuinely free and S5 is untouched. |
| 4. httpd stack heap moves | **Sound.** Every allocation is freed on every return path. One robustness gap (D3) and one wrong comment (D6). |

## Findings, worst first

### D1 (MEDIUM-LOW, real defect, introduced by `d6b643a4`) -- a third producer of `thermo_snapshot_t` was missed

`firmware/SaftyFW/src/tasks/thermo_task.c:261-271`
(`thermo_task_inject_reading()`) sets every field of `s_inject_snapshot`
*except* the `cj_valid` field `d6b643a4` added to `thermo_snapshot_t`
(`firmware/SaftyFW/src/snapshots.h:52`). The commit updated both live-hardware
branches in `thermo_task_fn()` (`thermo_task.c:413`, `:449`) and missed this one.

`s_inject_snapshot` is a file static, so the field is zero-initialised and stays
`false` forever. `link_task_send_status()`
(`firmware/SaftyFW/src/tasks/link_task.c:782-783`) therefore computes
`cj_valid = th_present && th.cj_valid` = false and NaNs `cj_c` on every injected
reading, even one injected with `tc_valid == true` and a finite `cj_c`.

Operator experience: on a board with the safety TC declared not-installed and
injection in use (bench / commissioning), the diagnostics page's safety TC state
reads `not_converting` and the cold junction reads blank, where the injected
value should appear. It fails in the SAFE direction (false, not a fabricated
true) and no guard consumes it -- S12 reads the snapshot's `cj_c` directly on the
Pico (`safety_guards.c:233-235`, gated on `tc_valid`), not the link's copy -- but
this is exactly the "field added to a struct, one producer left behind" class.

Handed back, not fixed: the one-line fix
(`s_inject_snapshot.cj_valid = tc_valid && !isnan(cj_c);`, mirroring the existing
`cj_c = tc_valid ? cj_c : NAN` contract two lines up) changes what a safety
producer puts on the wire, which wants the owner's call rather than a reviewer's.

### D2 (LOW, pre-existing, not introduced by `24090c9a`) -- a committed config change can report `ok` and silently revert

`config_store_write()` (`firmware/SaftyFW/src/config_store_flash.c:468-538`)
never reads back what it programmed, and `config_store_next_write_slot()` returns
`latest + 1`, not "first free slot". So if a slot's earlier write was torn
mid-program (partially programmed, CRC-invalid), the winning record after a boot
is the last CRC-VALID slot, and the next write lands on whichever slot follows
THAT one, which may still hold torn bytes. Programming NOR flash over non-erased
bytes can only clear more bits, so the record fails its own CRC,
`config_store_write()` still returns `true` / `"ok"`, the RAM cache is updated,
and the change vanishes at the next boot.

This is orthogonal to the A/B change (it existed identically in the single-sector
design) and A/B neither widens nor fixes it. Worth noting because
`config_store.h`'s NEW "A/B sector arbitration" comment asserts the write path
"always finishes writing (and CRC-verifying) the new record" -- **there is no
readback verification anywhere in this file.** Either add one, or correct the
claim; a comment promising verification that does not exist is how this stays
unfound.

### D3 (LOW, `3a20a560`) -- "internal DRAM" is stated but not enforced

`firmware/KilnFW/App/drivers/safety/safety_cfg_write.c:707-714` argues explicitly
that every heap buffer in a handler with any flash-writing path must be internal
DRAM "rather than depending on 'this particular buffer's lifetime doesn't overlap
the write' staying true after a future edit" -- and then allocates with
`heap_caps_malloc(sizeof(*st), MALLOC_CAP_8BIT)` (also `:1240`, `:1367`).
`MALLOC_CAP_8BIT` alone does **not** mean internal: with `CONFIG_SPIRAM=y` and
`CONFIG_SPIRAM_USE_MALLOC=y` it is satisfiable from PSRAM. These land in internal
DRAM today only because `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192`
(`firmware/KilnFW/sdkconfig:1886`) exceeds their ~550-1400 B sizes. Same in
`firmware/KilnFW/App/drivers/control/profile_executor_firing_stats.c:423-424`.
`firing_stats_load()` at `:324-325` gets it right (`MALLOC_CAP_INTERNAL |
MALLOC_CAP_8BIT`). Spell `MALLOC_CAP_INTERNAL` out at the other five sites so the
stated invariant does not depend on an sdkconfig threshold nobody will re-check
when a struct grows.

### D4 (INFO, item 1) -- stale "no caller" comment on the flashed write path

`firmware/SaftyFW/src/config_store_flash.c:~462` states "No caller exists yet ...
It is built and ready for that follow-on work, not exercised by anything today."
There are three live callers: `link_task.c:1473` (SET_CONFIG), `:1538`
(SET_CT_CAL) and `:2017` (COMMIT_CONFIG). A reviewer who trusts that comment
would conclude the whole A/B change is inert. It is not -- it runs on every
commissioning commit.

### D5 (INFO, item 2) -- stale LVGL claim

`firmware/KilnFW/App/drivers/bridge/uart_bridge_ext.c:~356` still says app_main
calls the starter "well before LVGL takes its own internal stack". LVGL starts in
`main_boot_early.c`, i.e. before `main_control_bringup()` at either the old or
new call site -- as `1f741635`'s own new comment in `main_control_bringup.c`
correctly acknowledges.

### D6 (INFO, item 4) -- one comment describes its sibling backwards

`firmware/KilnFW/App/drivers/http/profiles_catalog_http.c:~382` says "contrast
backup_import.c's candidate arrays, which DO reach flash and are therefore
internal DRAM". They are `MALLOC_CAP_SPIRAM`-preferred
(`backup_import.c:1180-1193`). The code is fine (see below); the comment is not.

## Question-by-question

**Is `24090c9a`'s atomicity real on RP2040 flash?** Yes. `config_store_write_cb()`
(`config_store_flash.c:439-455`) is handed exactly one `hal_flash_region_t`
(`args.region`), chosen by `config_store_plan_write()` (`config_store.c`), which
on a switch always returns the complement of the sector holding the current
record. The callback has no reference to the other region, so a crash mid-erase,
mid-program or between them physically cannot have touched it. There is no
separate active-sector pointer on flash to tear -- the reader is the sole arbiter.
The claim that matters is verified: at every instant at least one sector holds a
complete CRC-valid record.

**Is the reader's arbitration correct across a torn write and a wrapped counter?**
Yes. `config_store_find_latest_multi_ex()` (`config_store.c:941-991`) scans all 16
slots and keeps the strictly-highest-`seq` CRC-valid record; a torn slot fails
`config_store_unpack_ex()` exactly as any other corrupt slot always did and is
skipped, so the reader falls back to the other sector's lower-`seq` record with
no switch-in-progress special case. The rejected-record bookkeeping is correctly
deferred: a range-rejected record in A is only reported when NEITHER sector holds
a good one. `seq` is `uint32_t` compared absolutely with no wrap handling, which
is fine -- 2^32 writes is unreachable against 100k-cycle flash endurance, and the
wrap that actually occurs (the 8-slot round robin) is what the switch handles.
Ties are impossible: every write is `cached.seq + 1` into a slot no live record
occupies.

**Does the legacy single-sector migration preserve an existing config?** Yes, by
construction rather than by a migration step. Sector A keeps its offset
(`SAFTYFW_CONFIG_STORE_FLASH_OFFSET`, unmoved) and its record format; sector B is
a fresh 4K inside the already-reserved 64K `BOOTLOADER_CONFIG` region
(`flash_layout.h`, 0x1B2000, no overlap with anything else). On first boot of the
new firmware, B scans as an ordinary blank sector and A's existing highest-`seq`
record wins unchanged. The path does not depend on B being 0xFF either: the first
switch into B sets `needs_erase = true` unconditionally, so a non-blank B is
erased before slot 0 is programmed.

**Does anything now hold a lock across a flash erase?** No new one.
`config_store_write()`'s only serialisation is pico-sdk's own core lockout inside
`hal_flash_safe_execute()`, which is inherent to any flash write here. Its three
callers are `link_task.c` wire handlers holding no module mutex. Erase FREQUENCY
is unchanged (one per 8 writes) -- A/B alternates which sector absorbs it, it does
not add erases.

**Can `1f741635`'s reorder start the worker before something it depends on?** No.
`uart_bridge_ext_worker_ensure_started()` (`uart_bridge_ext.c:296-357`) creates a
queue, two semaphores and a task, and registers stack-margin; it depends on
nothing initialised between the new and old call sites. Nothing between the two
sites has its order changed relative to anything else -- the call was lifted out
and reinserted, with `relay_cycles_init()` and `profile_executor_start()` now
strictly after it. The internal-DRAM argument holds in the right direction too:
the new site is earlier, so strictly more contiguous internal DRAM is available
than at the old one, which succeeded.

**Does a worker that genuinely fails to start still degrade rather than hang?**
Yes. `flash_worker_wait_until_started()`
(`firmware/KilnFW/App/drivers/persist/flash_worker_wait.c:13-27`) polls with
`vTaskDelay()` and returns false at the ceiling; the two callers then load
NVS-only and report `migration_deferred: true` through `GET /api/cfgfs`. Worst
case is ~10 s of extra boot time (two sequential 5 s waits) with the idle task
still running -- slow, not wedged.

**Is the `flags2` bit genuinely unused?** Yes. Across both firmware trees exactly
two `flags2` bits are defined: `LINK_FLAG2_BORROWED` /
`SAFETY_LINK_STATUS_FLAG2_BORROWED` (0x01) and the new `..._CJ_VALID` (0x02),
with matching numeric values on both sides (`link_frame.h:153,168`;
`safety_link.h:231,241`). A stale peer is handled correctly in both directions:
the Pico only writes the bit inside the `peer_supports_status_v3` branch
(`link_frame.c:126-131`), and the ESP only reads it inside the V3-length branch
(`safety_link_frames.c:675-681`), setting `cj_valid_known = false` otherwise and
falling back to the OLD `TEMP_VALID`-tied NaN gate (`:684-696`). An older Pico's
frame never reaches the read at all.

**Can `cj_valid` be true for a stale or uninitialised reading?** Not from the
hardware path. `snap` is declared inside the loop (`thermo_task.c:386`) and BOTH
branches assign `cj_valid` explicitly; the value is computed from the current
transfer only (`ok && !reading.spi_failed && !isnan(snap.cj_c)`), before the
CR1-verify/plausibility block downgrades `snap.valid`, which is the intended
independence. The injection path is D1 above, and it errs false. One residual, for
the record: a dead/absent MAX31856 whose SPI reads happen to decode to a finite
`cj_c` would report `cj_valid` true and show as `probe_fault` rather than
`not_converting` -- display-only, no guard consequence.

**Is S5's latch/debounce untouched?** Yes. The guard path consumes
`thermo_snapshot_t.valid` via `safety_core.c:1111`'s `tc_valid`; nothing in
`safety_guards.c` reads `cj_valid` or the link's cached copy. The commit changes
only what is reported over the wire.

**Is every httpd allocation freed on every return path?** Yes -- all nine sites
walked by hand:
`setup_progress_http.c:69` (4 frees, incl. all three overflow arms);
`backup_import.c:1180/1189` (the second alloc's failure arm frees the first;
success frees both) and `:1252` (freed on the short-read arm and after apply);
`profiles_catalog_http.c:387` (every `APPEND` overflow `goto send:`, which is the
single exit that frees);
`safety_cfg_write.c:708/719` and `safety_cfg_http.c:681` (three narrow braced scopes, freed before the
scope ends, so none is live across the handler's many later returns);
`profile_executor_firing_stats.c:324` (freed on both arms) and `:423-424`
(the partial-failure arm frees both -- `free(NULL)` is well-defined -- and the
loop's single exit frees once). No `sizeof()` on a decayed pointer was introduced:
every converted site uses `sizeof(*p)` or an explicit element count, and
`backup_import.c` never took `sizeof(candidates)`.

**Does a PSRAM buffer sit on a path that writes flash?** No. The two SPIRAM
allocations that reach a flash write are `backup_import.c`'s candidate arrays, and
both are copied into internal-RAM stores before any write:
`profiles_http.c:1296` does `s_profiles.profiles[target_id] = *candidate;` before
`nvs_save_slot()`, and the zone commit loop copies each field into the live zones
config before the single `zones_config_save_now()`. Nothing dereferences PSRAM
inside a flash-cache-disabled window. `profiles_catalog_http.c`'s SPIRAM JSON
buffer writes no flash at all. See D3 for the sites where the internal-DRAM intent
is real but unenforced.

**Does allocation failure ever leave a partial config write to the Pico?** No. Both
`backup_import_apply()` allocation failures return false BEFORE
`backup_import_apply_locked()` runs, so nothing is touched, and
`backup_import_post_handler`'s existing `!ok -> 400` path reports it.
`safety_cfg_store_refetch_locked()`'s `malloc` failure
(`safety_cfg_store.c:1009-1014`) returns false with the cache untouched -- the
same outcome as any failed page fetch -- and the wrapper still gives the lock back.
Critically, `ct_auto_zero_post_handler`'s third allocation
(`safety_cfg_http.c:680`) sits BEFORE the postcondition check and well before the
`apply_pairs()` commit, so its failure aborts with a 500 having written nothing to
the Pico.

**Did `backup_import`'s all-or-nothing survive the `_locked()` split?** Yes.
`backup_import_apply_locked()` is the previous body with the two arrays passed in
as parameters; the two-pass structure (validate everything, then commit) is intact,
and the wrapper adds only allocate-before / free-after. The body read loop still
completes fully before apply is called, so a truncated upload changes nothing. Note
the honest limit, which is pre-existing and documented in the code
(`backup_import.c:~911`): pass 2 is not transactional across profile slots -- a
commit-time failure leaves earlier candidates saved. The `settings_source` loop
does restore its pre-import snapshot on failure. The split neither improved nor
degraded this.
