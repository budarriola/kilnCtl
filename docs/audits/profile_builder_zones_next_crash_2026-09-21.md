# Profile-builder "Next" crash on the LCD, 2026-09-21

## Symptom

During the M18 LCD commissioning class, tapping **Next** on the LCD
`profile_builder_zones` page (hit-box centre 427,201) rebooted the ESP32-S3.
Panic reason recorded by the coredump: `Task watchdog got triggered` (IDLE0 did
not reset in time), with the crashing task being `lvgl`; `exccause` 0x0
(IllegalInstructionCause). The board was left with the crash report
UNACKNOWLEDGED until this fix is flashed.

## Coredump evidence

Board at 1045e542, symbolized against
`firmware/KilnFW/elf_archive/KilnCtrl-01aa46885266.elf` (matched by
`find_crash_elf`), coredump archived as
`firmware/KilnFW/coredump_archive/coredump-5adbf73e84db.bin`.

    pc  0x420a39f9  <lv_label_set_text+5>

    #0 lv_label_set_text (obj=0x0, text=... "0 C") at lv_label.c:133
    #1 refresh () at ui_page_profile_builder_segment.c:90
    #2 ui_page_profile_builder_segment_prepare () at ui_page_profile_builder_segment.c:153
    #3 next_btn_cb () at ui_page_profile_builder_zones.c:99
    #5 lv_event_send / indev_proc_release / lv_timer_handler / lvgl_port_task

`obj=0x0` is decisive: this is not a stack overflow (the `lvgl` task showed
8232 used / 1504 free) and not a reentrant-invalidate case. The illegal
instruction is what a NULL-object LVGL call degenerates into here; the watchdog
reset followed because the fault happened inside the LVGL timer task.

## Root cause

`firmware/KilnFW/App/drivers/ui/ui_page_profile_builder_segment.c:153` -
`ui_page_profile_builder_segment_prepare()` calls the page's `refresh()`, which
writes into `s_target_val_label`, `s_ramp_val_label`, `s_dwell_val_label`,
`s_cards_row`, `s_add_btn`, `s_del_btn` and `s_caption`.

`kiln_ui.c` builds page screens lazily and caches them (`kiln_ui_page_t::screen`
is "NULL until first shown"), so on the FIRST tap of Next from
`profile_builder_zones` the segment page has never been built and every one of
those statics is still NULL. `next_btn_cb()`
(`ui_page_profile_builder_zones.c:99`) calls `prepare()` BEFORE
`kiln_ui_show("profile_builder_segment")`, so the first store lands on a NULL
label at `ui_page_profile_builder_segment.c:90`.

The two sibling pages already guard against exactly this:
`ui_page_profile_builder_review_prepare()` returns early on `!s_name_label`
("not built yet -- build() calls this itself once it is"), and
`ui_page_profile_segments.c`'s `render_page()` returns early on `!s_list`. The
segment page was the one member of the trio missing the guard. Top-bar setters
(`ui_topbar_set_title`/`set_prev_enabled`) are already NULL-safe, which is why
`ui_page_profile_segments_prepare()` survives calling them pre-build.

## Fix

One early-return guard at the top of `refresh()` in
`ui_page_profile_builder_segment.c`, matching the review page's idiom verbatim.
`ui_page_profile_builder_segment_build()` calls `refresh()` itself at the end,
so the first real render is unaffected; every later `prepare()` (page already
built and cached) refreshes as before.

No task was added, no stack size changed. `.dram0.bss` is unchanged at 94264
bytes (the change adds no statics).

## Verification

- KilnFW target build: clean (`idf.py fullclean` then `build`) in worktree
  `C:\wt\pbzcrash_fevlg1`. SaftyFW slot images built first (the embed
  dependency).
- `tools/run_all_checks.ps1 -Fast -AllowFewerChecks -Only 'ui|stack|lvgl'`:
  22 passed, 0 skipped, 0 failed.
- Negative test: guard removed by hand, full rebuild, disassembly of
  `refresh()` at 0x4202987c shows `entry` immediately followed by
  `call8 ui_page_profile_builder_draft` with no `s_target_val_label` test -
  i.e. the crashing path. Guard restored by hand (not `git checkout`), then
  `idf.py fullclean` + rebuild: the same symbol now starts
  `l32r a3, s_target_val_label` / `l32i` / `beqz -> refresh+0x188`.
- NOT verified on hardware: nothing was flashed and the tap was not repeated.
  The crash report stays unacknowledged until the fix is flashed.

## Web reachability

Not reachable from the web UI. `ui_page_profile_builder_segment_prepare()` has
exactly one caller in the tree (`ui_page_profile_builder_zones.c:99`), reached
only from an LVGL `LV_EVENT_CLICKED` handler on the physical panel. The web
profile editor goes through the HTTP profile routes and never touches these
LVGL statics.
