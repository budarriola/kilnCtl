# bx_flash_worker panic after `cfgfs_format` — 2026-09-21

**Verdict: task stack overflow in `bx_flash_worker` (8192 B), on the first
`cfg` LittleFS write after a format. Not heap corruption, not a stale mount
handle, not a `tz.dat` inconsistency.**

## Facts

Board: ESP32-S3 at 192.168.1.156, firmware commit `7098b2ee`, running ELF
`firmware/KilnFW/elf_archive/KilnCtrl-f19b7b7f8b0e.elf` (archived
2026-09-21T17:56:57Z; matched by `find_crash_elf`).

Sequence that produced the panic (bench agent, 2026-09-21):

1. `cfgfs_format(confirm=True)` — `cfg` LittleFS partition formatted, 9 files
   to 0, remounted OK.
2. `control_set_zone_pid(zone=0, ...)` with z0's existing gains, to force a
   zones resave. ACKed with no reply; the board rebooted.

`GET /api/crash_report` (read only; **not** acknowledged):

```
reset_reason='PANIC' exc_task='bx_flash_worker' exc_cause_str='IllegalInstruction'
exc_pc='0xfffffffd' exc_addr='0x00000000' already_acknowledged=False
```

`read_esp_coredump` (831648 B, archived as
`firmware/KilnFW/coredump_archive/coredump-0e79eb1d2b4f.bin`), symbolized
against the `7098b2ee` ELF:

```
Crashed task handle: 0x3fce7af8
Panic reason: ***ERROR*** A stack overflow in task bx_flash_worker has been detected.

#0  panic_abort (details=... "A stack overflow in task bx_flash_worker has been detected.")
        at components/esp_system/panic.c:471
#1  0x4037ff8c in esp_system_abort            at port/esp_system_chip.c:87
#2  0x4215ccb3 in vApplicationStackOverflowHook (xTask=0x3fce7af8)
        at FreeRTOS-Kernel/portable/xtensa/port.c:577
#3  0x40380f6a in vTaskSwitchContext ()       at FreeRTOS-Kernel/tasks.c:3698
#4  0x403807e7 in _frxt_dispatch ()           at portable/xtensa/portasm.S:451
#5  0x403807dd in _frxt_int_exit ()           at portable/xtensa/portasm.S:246
```

`a15 = 0x2000` (8192) in the panic frame corroborates the checked stack size.
The `IllegalInstruction` / `exc_pc=0xfffffffd` pair in `/api/crash_report` is
the *reported* summary of the deliberate `abort()` the overflow hook takes,
not an independent fault — the coredump names the cause outright. Per-thread
backtraces in the dump are empty (no ROM ELF loaded when running the utility
out of IDF); the panic frame above is the decisive evidence and is sufficient.

Post-reboot state (`GET /api/cfgfs`): `mounted=True`, 8 files,
`tmp_entries_now=0`, `dual_write.zones: not file-backed yet (NVS only)`.
`zones.json` (900 B) IS present.

## Root cause

`control_set_zone_pid` arrives over benchproto and is handled **on
`bx_flash_worker` itself** (`uart_bridge_ext_control.c:84`,
`control_handle_message`). The whole save chain therefore nests in one frame
stack on that task's 8192 B internal-SRAM stack:

```
bx_worker_task                                             48 B
  control_handle_message   (uint8_t reply[BRIDGE_REPLY_MAX])           608 B
    zones_config_set_pid                                   32 B
      nvs_save                                            112 B  + NVS internals (unmeasured)
        zones_config_cfg_fs_save                           48 B
          cfg_fs_write_atomic_device                       48 B  (re-entrancy guard: runs INLINE,
          cfg_fs_write_job_run                             32 B   already on the worker)
            cfg_fs_write_atomic                          2000 B  <-- four CFG_FS_PATH_MAX (600 B) buffers
              esp_vfs_* -> vfs_littlefs_rename           1616 B  (indirect; static walk cannot follow)
```

Measured out of the flashed ELF with
`firmware/KilnFW/App/test/stack_budget_lib.py`. That already totals **~4.5 kB
of the 8 kB**, before any of the NVS or LittleFS internals at which the static
walk stops.

`check_all_task_stack_budgets.py` grades `bx_flash_worker` at **3792 B** and
says so honestly in its own comment: *"Still INDETERMINATE… the jobs themselves
call into NVS/esp_partition/LittleFS, whose internals dispatch indirectly. This
is a real, source-derived lower bound over the actual work the task performs,
not a full measurement."* The check was green and is still green; it simply
does not measure the half of the chain that overflowed. This is the identical
failure mode that raised `pico_auto_update` 4096 → 8192 the same day.

**Why a format was the trigger.** The write itself is routine; what a fresh
format changes is which LittleFS path it takes. Immediately after a format the
volume is empty, so this one write is simultaneously:

- an `lfs_mkdir` of `<base>/.tmp` (`ensure_dir`; 192 B frame plus its commit
  chain),
- a **create** of the temp file rather than an overwrite of an existing one,
  and
- an `lfs_rename` (224 B frame — the deepest single lfs frame in the image)
  onto a name that does not yet exist,

each of which runs `lfs_dir_orphaningcommit` → `lfs_dir_relocatingcommit` →
`lfs_dir_compact` → **`lfs_dir_traverse` (recursive, 224 B per level)**, and,
on a just-formatted volume, an empty allocator lookahead that forces a full
`lfs_fs_rawtraverse`. That recursion is bounded only at run time; nothing in
the static ceiling accounts for it. Every subsequent write to an already
existing file takes a far shallower path — which is why this has never fired
before, and why the *first* write after the format is the one that died.

## Hypotheses tested and rejected

- **`59ae974c` would have prevented it — NO; it would have made it marginally
  worse.** `59ae974c` ("cfg_fs: stream write_atomic's readback verify instead
  of malloc'ing the whole blob") is on `origin/main` but not in `7098b2ee`
  (`git merge-base --is-ancestor` confirms). It replaces a whole-blob `malloc`
  with a **256 B stack** `chunk[]` inside the same `cfg_fs_write_atomic` frame.
  It removes a heap allocation, not stack pressure; it adds up to 256 B to the
  very frame that overflowed. It is a good change for the DRAM low-water mark
  and irrelevant-to-negative for this panic.
- **Stale LittleFS handle / state across format-while-mounted — rejected.**
  `cfg_fs_confirm_format_job_run` (`cfg_fs_mount.c:541`) unregisters the VFS
  (`esp_vfs_littlefs_unregister`), calls `cfg_fs_deinit()`, formats,
  `register_cfg_vfs()`, then `finish_mount_after_register()`. No `FILE*` is
  cached anywhere in `cfg_fs.c` (every helper opens and closes within one
  call), and the format runs on the same single-threaded worker, so no other
  job can hold a descriptor across it. Nothing survives the remount.
- **Missing `tz.dat` indicates an inconsistent format — rejected.** The format
  removed all 9 files. The 8 present now were rewritten *after* the reboot by
  each store's lazy NVS→file migration on first load (the
  `zones_config_cfg_fs_resolve` "no usable file → save the NVS candidate"
  path, and its siblings). `tz.dat` has no load-time migrate-on-read path, so
  it is simply not rewritten until the next time-zone write. `tmp_entries_now=0`
  and a well-formed 900 B `zones.json` are both consistent with a clean
  filesystem.
- **`dual_write.zones: not file-backed yet (NVS only)` is NOT a fault.** It
  records that *this boot's* zones load came from NVS (`out_used_file=false`),
  which is exactly what the post-format migration path reports when it is the
  thing that wrote the file. It flips on the next boot.
- **Heap corruption / corrupted return address — rejected.** `exc_pc
  0xfffffffd` never executed: the coredump shows a clean, fully symbolized
  `panic_abort` frame reached from `vApplicationStackOverflowHook` via
  `vTaskSwitchContext`. The overflow was detected by the scheduler's watermark
  check at a context switch, not by a wild branch. Both heaps read healthy
  post-reboot (internal free 37419 B, min_free 20091 B).

## Proposed fix

Two parts, both in this repo's existing idiom.

1. **Take `cfg_fs_write_atomic`'s path buffers off the stack.** Its four
   `CFG_FS_PATH_MAX` (600 B) arrays plus `flat[]` are ~2 kB of a single frame —
   the largest first-party contributor and the only one under our control. Move
   them into one heap-allocated struct, exactly the "HEAP, never the stack"
   convention `cfg_fs_mount.c:393` and `zones_config_cfg_fs.c` already apply for
   the same reason, with the same "alloc failure degrades, never panics"
   contract (return `ESP_ERR_NO_MEM` before touching the final path).
2. **Raise `BX_WORKER_STACK` 8192 → 10240** (`uart_bridge_ext.c:257`). The
   LittleFS recursion above cannot be bounded statically, so the frame
   reduction alone must not be trusted as the whole margin. Owner standing
   authorization covers measured-too-small task stacks.

Net: ~4 kB more worst-case headroom for 2048 B of permanent internal DRAM
(free internal heap is 37419 B, largest free block 14848 B; the worker is
started deliberately at the "executor+autotune" stage, where the largest free
internal block is ~31744 B, so the larger allocation still fits — see the
sizing note above `BX_WORKER_STACK`).

Not proposed: shrinking `BRIDGE_REPLY_MAX`, or dispatching the cfg write to a
second task (the re-entrancy guard exists precisely because dispatching from
the worker onto itself deadlocks).

## Is the board safe to reflash now?

**Yes.** The failure is a stack overflow on a deterministic, reproducible code
path, not flash or filesystem damage. `cfg` is mounted and consistent, NVS is
untouched and remains authoritative, no firing was running, and the atomic
write sequence leaves the final path alone on any pre-rename failure. The one
standing caution is unchanged: the crash report is still **unacknowledged** (by
instruction), so `capability_preflight` will refuse a run until someone reviews
it, and `flash_firmware()`'s own guards apply as usual.

Do not re-run `cfgfs_format` followed by a config write on the *current*
`7098b2ee` image — that is the reproducer.
