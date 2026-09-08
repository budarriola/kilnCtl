# `GET /api/firing_history` panicked the board — httpd_worker stack overflow

2026-09-08. Reproducible panic on a plain read endpoint, found by the
hardware verification in `d2ecf5eb`.

## Reproduction

Board idle, `profiles_get_exec_status` → `state=0` (no firing), relays off.

```
$ curl "http://192.168.1.156/api/firing_history?profile_id=0"
HTTP 000            <- connection dies, board resets
```

Crash record read back afterwards, verbatim:

```json
{"present":true,"acknowledged":false,"exc_cause":65535,"exc_cause_str":"",
 "exc_pc":"0xfffffffd","exc_addr":"0x00000000","exc_a0":"0x3fca0098",
 "exc_a1_sp":"0x3fcb15e4","exc_task":"@��?",
 "found_on_boot_reset_reason":"PANIC","frame_trustworthy":false,
 "backtrace":["0xfffffffd"],"backtrace_corrupted":true}
```

The record captured before this pass's own reproduction (same endpoint, same
boot lifetime) differed only in the garbage: `exc_pc 0x00000420`,
`exc_a0 0x3fc9ff08`, `exc_a1_sp 0x3fca2da4`, `exc_task "�����"`.

## Is the frame trustworthy? No — and it is not symbolized here

`crash_report_frame_trustworthy()` (`7f7e3d0e`) reports **false**, and the
record is self-consistently untrustworthy rather than merely odd:
`exc_cause=65535` is not a real Xtensa cause code, `exc_pc` is `0xfffffffd`
in one capture and `0x420` in the other (neither is in any text section),
and `exc_task` is not a string. The two captures disagree on every register
while the *trigger* is identical, which is itself the signal: what was
recorded is post-corruption memory, not a fault frame.

**No symbolization was attempted.** Symbolizing `0xfffffffd` would have
produced a confident, fictional line number. (Separately: the ELF named in
the assignment, `build/elf_archive/KilnCtrl-a524789a1675.elf`, does not
exist, and no archived ELF contains the running image's build timestamp
`Sep 8 2026 14:58:00` — so no matching ELF was available either way.)

The garbled task name *is* diagnostic, though, without any symbolization: a
smashed TCB is what a FreeRTOS stack overflow looks like from the outside.

## Root cause: four 1364 B blobs stacked on an 8192 B stack

`profile_firing_history_blob_t` is 1364 B (`_Static_assert` in
`profile_executor_internal.h`). The read path put **four** copies of it on
the caller's stack, and the caller is the shared `httpd_worker` task, whose
live high-water mark was measured at **632–468 B free of 8192 B** the same
day (`check_httpd_task_stack_budget.py`'s docstring).

Measured statically out of the built ELF, before the fix:

```
    96 B      96  firing_history_get_handler
  1408 B    1504  profile_executor_get_firing_history   <- blob
  1424 B    2928  firing_stats_load                     <- nvs_blob
  1456 B    4384  firing_stats_cfg_fs_resolve           <- file_blob
  1488 B    5872  firing_stats_cfg_fs_load_raw          <- raw[1368]
   640 B    6512  cfg_fs_read
    32 B    6544  path_join_ok
```

6544 B, and that is an under-estimate (the script models neither ISR/window
spill nor the ESP-IDF dispatch frames above the handler). The handler itself
was already careful — it heap-allocates its records array and its response
buffer — but the accessor it calls threw that discipline away one frame
down, and the three frames below it each did the same.

Not a null dereference, not an unvalidated `profile_id`: the handler already
rejects a non-numeric or out-of-range id with a 400, and `profile_id=0` is
a legitimate id whose history simply does not exist yet. The size-mismatch
and missing-file paths are both handled and both return empty. The stack was
the whole story.

## Fix

Heap-allocate every one of those buffers, internal DRAM
(`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` — this path reaches NVS/flash, so
PSRAM would be the wrong pool), free on every return path, degrade cleanly
on OOM:

| Frame | On OOM |
| --- | --- |
| `profile_executor_get_firing_history()` | returns 0 entries (handler answers 200 with an empty array) |
| `firing_stats_load()` | returns false, `*out` zeroed |
| `firing_stats_cfg_fs_resolve()` | behaves as a missing file: the NVS candidate decides, nothing is discarded |
| `firing_stats_cfg_fs_load_raw()` | behaves as an absent/unreadable file |
| `firing_stats_cfg_fs_save()` | `ESP_ERR_NO_MEM`, logged (already a best-effort write) |

After the fix, the same measurement: **`firing_history_get_handler` = 1088 B**,
down from 6544. `check_httpd_task_stack_budget` and
`check_main_task_stack_budget` both pass.

## Regression test

`test_fscf_history_read_uses_the_heap_not_the_httpd_stack()` in
`test_profile_executor_prestart.c`. A host test cannot measure stack depth,
so it pins the fix by its observable consequences:

1. With the heap stub refusing every allocation, the read reports **no
   history** instead of quietly succeeding — a stacked buffer cannot fail to
   allocate, so the pre-fix code returns the run and the test fails.
2. It counts blob-sized allocations across one read and requires at least
   two — the two frames this translation unit compiles in. The OOM check
   alone cannot see a *single* frame regressing, because one surviving heap
   frame short-circuits the whole read. (The two `_cfg_fs_` frames live in a
   separately-compiled object with its own copy of the stub counter, so they
   are not visible to this count; the counter's `static` scope is why.)
3. The OOM path discards nothing — the run reads back once the heap recovers.

`heap_caps_malloc_test_reset_count()` / `_test_count()` were added to
`test/stubs/esp_heap_caps.h` for check 2.

**Negative test.** The heap allocation in
`profile_executor_get_firing_history()` was replaced by a stack local and its
`free()`s removed; the suite failed with exactly one failure:

```
FAIL test_profile_executor_prestart.c:7960: both 1364 B blob frames visible
from this translation unit allocate -- profile_executor_get_firing_history()'s
and firing_stats_load()'s. A lower count means one of them is back on the
8192 B httpd_worker stack
```

The edit was then reversed by hand (no `git checkout`/`restore`) and the
suite returned to 32/32 executables green.

## Siblings with the same shape

Ranked by statically-measured depth from `check_httpd_task_stack_budget.py`
after this fix, on the same 8192 B stack:

* `backup_export_get_handler` — **7472 B**, the current ceiling. Pre-existing
  and already tracked; its depth comes from calling `profile_detail_get_handler`'s
  5184 B frame.
* `profile_detail_get_handler` — **6720 B**, a 5184 B single frame. This is
  the next `firing_history` waiting to happen and is the one worth fixing.
* `ct_auto_zero_post_handler` — 6576 B.
* `ct_cal_post_handler` — 5056 B.

None of these is an unvalidated-id bug; all are large stack locals, the same
class as this one. `firing_history`'s own id handling (400 on unparseable or
out-of-0..255, 200-with-empty-array on an id that never fired) was reviewed
and is correct as written.
