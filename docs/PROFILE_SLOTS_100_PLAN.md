# 100 user profile slots + 1 live-edit slot

Owner decision, 2026-09-19: the board carries **100 user fire-profile slots plus one
reserved live-edit slot**, and the space for them comes from growing the `cfg` LittleFS
partition into the unused flash directly above it. The 20-slot compromise and the
"cut firing-stats depth" option recorded in `docs/LIVE_PROFILE_EDIT_PLAN.md` section 12
item 1 are both rejected and that section should be amended to point here.

This plan is written against the tree at `8006ddd5`. Nothing below has been implemented.

---

## 1. New partition layout

Only the `cfg` row changes. Every other row keeps its offset, size and subtype, so no
data partition moves and `otadata` does not have to be erased for this revision.

```
  cfg,  data, littlefs,  0xDB0000, 0x250000,     (was 0x80000)
```

Arithmetic, in the same form `partitions.csv` already uses:

```
  logs:         0xCF0000 + 0x0C0000 = 0xDB0000          (unchanged)
  cfg (grown):  0xDB0000 + 0x250000 = 0x1000000         (end of the 16 MiB part)
  tail left:    0                                       (was 0x1D0000 = 1,900,544 B)
  0x250000 = 2,424,832 B = 2.3125 MiB
  0xDB0000 = 219 x 0x10000 and 0x250000 = 37 x 0x10000 -- both 64 KiB-aligned,
  matching every other row (a `data` partition only requires 4 KiB).
```

Why the whole tail and not a round 2 MiB: the worst-case inventory in section 3 is
~810 KiB logical, ~1,045 KiB block-rounded, and the sizing rule this partition was
originally built on (`partitions.csv` lines ~490-515) is "keep roughly 2x the live set
free for garbage collection". 2 MiB (0x200000) gives 1.96x -- just under the rule.
0x250000 gives 2.32x and consumes the fragment rather than stranding 320 KiB that
nothing else on this table can reach. If a future `app`-type partition is ever wanted
above `logs` this decision has to be revisited; that is the accepted cost, and it should
be written into the CSV comment block so the next reader sees it.

`logs` is **not** touched by this pass, for the same reason the 2026-09-07 pass gave:
a subtype/retention change there is its own flag day.

---

## 2. Code constants and structures that change

`PROFILES_MAX_COUNT` becomes **100**; the live-edit slot is id **100**, structural and
outside that count, exactly as `LIVE_PROFILE_EDIT_PLAN.md` section 4 describes.
Ids therefore run 0..100, and `PROFILE_BUILTIN_ID_BASE` is 128 -- **27 ids of headroom
left, and that base is the hard ceiling.** Any later raise past 127 user slots is a
protocol change (`profiles_builtin.h`, `devices_profiles.py`'s two disjoint ranges),
not a constant bump. Say so in `profiles_types.h` next to the new value.

| file | what changes |
|---|---|
| `App/drivers/persist/profiles_types.h` | `PROFILES_MAX_COUNT` 8 -> 100; add the 128-ceiling note. `profile_t` layout is **unchanged**, so `PROFILE_VERSION` stays 4 and no migration case is added. |
| `App/drivers/http/profiles_http_internal.h` | `profiles_state_t.used_bitmap` `uint8_t` -> `uint32_t used_bitmap[4]`, behind `profiles_slot_used()/_set()/_clear()` helpers -- there are ~12 `1u << id` sites and they must all go. `profile_t profiles[PROFILES_MAX_COUNT]` must stop being a `.bss` array: 100 x 424 B = 42,400 B of internal DRAM. Allocate once at init with `MALLOC_CAP_SPIRAM`, internal only as a logged fallback. Internal DRAM below ~11.9 kB resets sockets on this board. |
| `App/drivers/http/profiles_http.c` | every `1u << id`; `s_profile_rev[100]` (a 400 B blob under one `prof_rev` key, fine); `profile_nvs_key()` now produces `prof100` (7 chars, inside the 15-char limit -- add a `_Static_assert` on the max id rather than trusting the comment); the load loop and the one-time default-partition migration loop. |
| `profiles_catalog_http.c` | **`profiles_list_get_handler()`'s `char json[PROFILES_MAX_COUNT * 190 + 112]` becomes 19,112 B on an 8 KiB httpd stack -- the `httpd_stack_blob` class, and it will crash.** Convert to the chunked form the builtin-catalogue handler in the same file already uses. `favorites_list_get_handler()`'s `_Static_assert(PROFILES_MAX_COUNT + 32 <= 60)` fails by construction: chunk it too, and stop emitting a single-word `user_mask`. Prefer dropping `user_mask` entirely and emitting only `ids`, which both existing clients already read. |
| `persist/profiles_favorites.c/.h` | `uint32_t` user mask -> `uint32_t[4]`; the `_Static_assert(PROFILES_MAX_COUNT <= 32)` is replaced, not deleted. `profiles_favorites_masks()`'s signature changes -- grep its callers (catalog handler, LCD pages, the web page). |
| `profiles_edit_http.c`, `profiles_export_http.c` | id range checks only; no structural change. |
| `http/backup_import.c` | `candidates[PROFILES_MAX_COUNT]` is already `heap_caps_malloc(MALLOC_CAP_SPIRAM)` with a `malloc()` fallback -- at 100 that fallback would take 42.8 kB of internal heap. Make the fallback fail cleanly instead. |
| `http/backup_http_internal.h` | `BACKUP_BODY_MAX` 16384 is too small: a 100-profile compact export is ~18-20 kB of profiles alone plus ~3.2 kB of zones. Raise to 131072 and read the body into PSRAM. The export side is already chunked; confirm it does not buffer the whole document. |
| `http/diagnostics_http.c` | the literal `profile_names[PROFILES_MAX_COUNT]` table must go. **Do not emit 100 per-slot dual-write rows** -- replace them with one aggregate `profiles` row (slots in use, slots diverged, worst rev pair). `CFG_FS_STATUS_MAX_ITEMS` then stays at 18 instead of growing to 110. |
| `http/readiness_http.c`, `bridge/uart_bridge_ext_control.c` | loop bounds only; the bridge list is already paged by `start_id`, which is what makes 100 slots survivable over the link. Verify the paging terminates for `start_id > 100`. |
| `ui/ui_page_profiles_mine.c` | `PAGE_COUNT` would become 25 pages of 4. See section 4. |
| `ui/ui_page_profile_builder_review.c`, `ui_page_profile_detail.c` | linear scans over all slots; acceptable, but keep them off the LVGL render path. |
| `tools/PcTools/src/kilnctrl/protocol.py` | `PROFILES_MAX_COUNT = 8` -> 100. This is the mirror of the firmware constant and the one place PcTools reads it; `devices_profiles.py` derives everything from it. |
| `tools/PcTools/scripts/full_board_backup.py` | `FIRING_HISTORY_PROFILE_IDS = range(8)` -> derived from the imported constant, and skip ids absent from `/api/profiles` so a full backup is not 100 requests. |
| `firmware/KilnFW/docs/PROFILES.md`, the `partitions.csv` comment block, `docs/CONFIG_FILESYSTEM.md`, `docs/LIVE_PROFILE_EDIT_PLAN.md` §12.1 | documented counts, and the superseded 20-slot conclusion. |

`kiln_cfg_store`'s own 8 named-config slots are **not** changed by this pass.

---

## 3. Where profiles live, and capacity in each store

They live in **both**, unchanged: `profiles_nvs` is authoritative and unconditional,
`cfg` is a per-slot dual-write mirror (`profiles_cfg_fs.c`), and on this bench board
`cfg` is still unformatted and unmounted so the mirror is inert today. Raising the slot
count does not change that policy and must not be used as an excuse to change it.

**`profiles_nvs` (0x19D000, 0x60000 = 384 KiB) -- the tighter of the two.** It holds
both the profile blobs (namespace `kiln_cfg`, key `profN`) and the firing-stats rings
(namespace `fire_stats`, key `fs_<id>`, `PROFILE_FIRING_HISTORY_BLOB_SIZE_V1` = 1364 B),
plus the `fsr_<id>` rev keys. NVS stores a blob as a 32-byte-entry index plus 32-byte
data entries inside 4096-byte pages (126 usable entries per page), and a chunk cannot
span a page, so real cost is roughly 1.15-1.25x the payload:

```
  profile blobs   101 x ~429 B  -> ~52 KiB
  firing stats    101 x 1364 B  -> ~172 KiB   (user slots)
  firing stats     32 x 1364 B  -> ~55 KiB    (builtin ids also allocate fs_ keys)
  rev keys       ~133 x 32 B    -> ~5 KiB
  prof_rev blob   400 B, prof_used bitmap     -> <1 KiB
  ------------------------------------------------------
  worst case                       ~285 KiB of 384 KiB = ~74% full
```

That fits, but 74% is past the comfortable band for a partition whose compaction needs a
free page and which degrades exactly in the near-full regime. **Firing stats are the
whole problem, not the profiles.** Two cheap mitigations; at least the first should land:

1. Only allocate an `fs_<id>` key for a profile that has actually fired (already the
   documented intent in `profile_executor_firing_stats.c` -- verify it is true, and add a
   host test that a never-run slot writes no key). Realistically that caps stats at the
   handful of profiles anyone fires, and the 285 KiB figure becomes theoretical.
2. Prune a slot's stats on delete. `nvs_erase_slot()` is the hook; it does not do this today.

Do **not** shrink `PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH` -- the owner rejected that.

**`cfg` (grown to 0x250000 = 2,424,832 B).** Same inventory in the plan's own units:

```
  /cfg/profiles/<id>.json   101 x ~2K  = 202K
  /cfg/stats/<id>.json      133 x ~4K  = 532K   (101 user + 32 builtin)
  /cfg/zones.json                      =   8K
  /cfg/kilncfg/<slot>.json    8 x ~8K  =  64K   (unrelated to profile count)
  /cfg/{prefs,tune,hidden,relay_cycles}.json    =   4K
  -----------------------------------------------------
  logical worst case                     ~810K across ~250 files
  block-rounded (4 KiB sectors, ~1.29x)  ~1,045K
  with the partition's own 2x GC rule    ~2,090K   < 2,425K  OK (2.32x)
```

Two second-order effects of ~250 files that the 30-file sizing never faced: LittleFS
directory metadata grows (still small), and `CFG_FS_STATUS_MAX_FILES` = 32 caps the
`/api/cfgfs` file listing -- that listing becomes a truncated sample rather than an
inventory, which is acceptable but must be stated in the response, not silently true.

---

## 4. UI

**Web, `profiles_page.html`.** The page already renders saved profiles as a card list
from `/api/profiles`, already partitions favorites first, and already has a bulk select
mode. A 100-card list is usable on a phone only with a filter. Recommendation, in order:

1. A single text input above the list that filters by name, client-side, over the array
   the page already holds. No new endpoint and no URI-handler slot -- the cap has
   **one** spare (`check_uri_handler_cap.ps1`, 150 of 151), so a server-side search route
   would consume the last one and should not be added for this.
2. Favorites pinned at the top, stable while filtering.
3. A "recently fired" group under favorites, derived from the firing-stats records the
   board already has. This is the highest-value ordering at 100 slots and costs one extra
   field in the list JSON.

**Web, `main_page.html`'s `#profileSelect`.** A 100-option `<select>` is a native
scrolling picker on iOS and Android and does not break; it is merely tedious. Keep the
`<select>` (a custom combobox costs CSP-safe JS and an accessibility regression), keep
favorites-first, and group with `<optgroup>`: Favorites / Recent / All / Built-in.

**LCD, `ui_page_profiles_mine.c`.** 25 pages of 4 behind Prev/Next is not navigable, and
the 320x480 panel must not scroll. Recommendation: show only **favorites plus the
last-fired profiles**, capped at two pages, with the full list reachable only from the
web, and say so on the page. Authoring 100 profiles is a web activity; the LCD's job is
picking one to fire.

---

## 5. Test plan

Host tests (`firmware/KilnFW/App/test/`, run by `tools/run_all_checks.ps1`):

- `test_profiles_http.c`: several cases assert exact counts
  (`count_occurrences(...) == PROFILES_MAX_COUNT`). Re-derive every such assertion from
  the constant. Add: fill all 100 slots, list, delete slot 50, then save with
  `requested_id == PROFILES_MAX_COUNT` and confirm it lands in 50.
- **New `test_profiles_capacity.c` (host-side capacity test).** Compute worst-case encoded
  byte totals for `profiles_nvs` (blob x 101 + stats blob x 133, with the NVS
  entry-rounding factor) and for `cfg`, and fail if either exceeds the partition size
  **parsed from `partitions.csv`**. Parsing the CSV is what makes this a gate rather than
  a restated constant; `kilnctrl.partition_table.parse_partitions_csv` already exists and
  `check_recovery_image_size.py` is the precedent.
- `test_profiles_cfg_fs.c`: exercise slot ids above 31 and above 99, which the current
  fixtures cannot reach, to catch any surviving `1u << id`.
- `test_backup_import.c`: an import carrying 100 profiles, and one carrying 101 (refused
  by name, not by overflow).

Negative tests -- each must end with a **forced full rebuild**, never a hand-restore plus
an empty `git diff` (a poisoned `.exe` outliving a revert has already produced a committed
wrong verdict in this repo):

- Revert `used_bitmap` to `uint8_t` and confirm the capacity/slot tests go red. If they
  stay green the bitmap widening is untested.
- Set the capacity test's parsed `cfg` size to the old 0x80000 and confirm it fails.
- Leave `profiles_list_get_handler()` unchunked and confirm
  `check_httpd_task_stack_budget.py` reports the overflow. That checker already names
  `candidates[PROFILES_MAX_COUNT]`, so it has a hook into this constant -- verify it
  actually re-reads the new value rather than a cached number.
- Make `profile_nvs_key()` emit a 16-char key and confirm `check_nvs_key_length.ps1` fails.

On-bench, after migration: save 100 profiles over HTTP, reboot, confirm all 100 reload;
export and re-import a full backup; run one short firing and confirm the stats record
lands for that id only.

---

## 6. Bench migration

`cfg` is unformatted and not mounted at boot today, so **the resize loses nothing** --
there is no volume to preserve. Nothing else on the table moves.

What must be flashed: an ordinary `flash_firmware()` from a clean worktree at HEAD. That
tool writes exactly three images (`tools/PcTools/src/kilnctrl/mcp_server_flash.py`, the
OpenOCD TCL near line 965): `build/bootloader/bootloader.bin` @0x0,
`build/partition_table/partition-table.bin` @0x8000, and `build/KilnCtrl.bin` at the
offset it resolves from `<kiln_fw_root>/partitions.csv`'s `app` row. So **yes, it applies
a partition-table change**. Each write is `program_esp ... verify`, which erases only the
region written -- `nvs`, `wifi_nvs`, `kiln_nvs`, `profiles_nvs`, `coredump` and `logs` are
untouched by construction. It does **not** write `otadata`; that is the known, deliberate
gap. Because no `app`-type row moves in this revision, `otadata` does not need erasing.

Procedure:

1. Full backup first (`tools/PcTools/scripts/full_board_backup.py` plus
   `GET /api/backup/export`) -- mandatory even though nothing should be lost.
2. Build at HEAD in a clean worktree (`tools/worktree_mint.ps1`); a green build in this
   shared dirty tree proves nothing about HEAD.
3. `flash_firmware(kiln_fw_root=<worktree>/firmware/KilnFW, ap_password=...)`, cable
   attached, `verify=True`. The partition-offset guard compares the CSV's `app` row
   against the board's live table; `app` is unchanged, so it passes.
4. Confirm with `debug_check_partition_table()` that the board reports `cfg` at 0xDB0000
   size 0x250000.
5. `cfg` stays unmounted and unformatted until the separate mount-at-boot work lands --
   this pass must not also turn mounting on. Two changes, two flashes.

Risks, none of them caused by this change and all three in `CLAUDE.md`'s flash section: a
dual reset trips S6a (expected -- confirm `trip_mask` 0x0020, then `safety_clear_trip()`);
the SX1509 may fail post-reset init and latch a trip, cleared by a second `debug_reset`;
and a board whose `otadata` is blank boots `recovery`, not `app`, which
`flash_firmware()`'s own verification fails loud about.

---

## 7. Ordered task list

One commit each, sized for a sonnet implementer, each independently buildable and green.

1. **Widen the slot bitmaps, still at 8 slots.** `used_bitmap` -> `uint32_t[4]` behind
   `profiles_slot_used()/_set()/_clear()`; favorites mask likewise. No constant changes;
   every existing test stays green. This is the commit to review most carefully.
2. **Chunk `profiles_list_get_handler()` and `favorites_list_get_handler()`**, still at 8.
   The response bytes for 8 slots must be byte-identical to today's -- diff them.
3. **Move `s_profiles.profiles[]` off `.bss`** into a PSRAM allocation made at init, with
   an internal-DRAM fallback and a logged failure path. Still at 8.
4. **Add a profiles-capacity check**, parsing `partitions.csv`, asserted against the
   *current* 8 slots and the *current* 0x80000 `cfg`. It must pass before anything grows.
   (Shipped as `tools/check_profiles_capacity.ps1`/`.py`, not a host-test `.c` file --
   corrected 2026-09-19, see Status section below.)
5. **Grow `cfg` to 0x250000** in `partitions.csv`, with the full comment block
   (arithmetic, why the whole tail, what it costs). Source-only; no flash.
6. **Raise `PROFILES_MAX_COUNT` to 100** plus the live-edit slot at 100. Mechanical: loop
   bounds, `PROFILE_LIST_ENTRY_MAX` sizing, the diagnostics aggregate row, the LCD page
   cap, `protocol.py`, `full_board_backup.py`. Every host test re-derived from the constant.
7. **Raise `BACKUP_BODY_MAX` and PSRAM the import buffer**, with the 100/101-profile
   import tests.
8. **Web: name filter plus favorites/recent grouping** on `profiles_page.html`, and
   `<optgroup>` on `main_page.html`'s picker. No new routes.
9. **LCD: favorites plus recent only** on `ui_page_profiles_mine.c`, two pages max, with
   the on-page note.
10. **Firing-stats hygiene:** confirm/enforce "no `fs_` key until a profile has fired",
    prune stats on slot delete, host-test both.
11. **Docs:** `PROFILES.md`, `CONFIG_FILESYSTEM.md`, and amend `LIVE_PROFILE_EDIT_PLAN.md`
    §12.1 to record that its 20-slot conclusion is superseded by this plan.
12. **Bench migration** per section 6, in its own session, with the backup taken first.

### Status (2026-09-19, Opus review of phase A)

Phase A landed: tasks 2, 4, 5, 9 (reverted, see below), 10, 11 (this section).
Task 1 landed 2026-09-19 (see below). Task 3 landed 2026-09-19 (see below).
Task 6 landed 2026-09-19 (see below, in worktree `C:\wt\s100t6_0juqog`).
Tasks 7, 12 are NOT started (task 8 landed separately, see the Status section
below the historical block).

**Task 6 landed 2026-09-19.** `PROFILES_MAX_COUNT` raised 8 -> 100.
`PROFILE_BUILTIN_ID_BASE` (128) documented as a hard ceiling in
`profiles_types.h`. `LIVE_EDIT_WORKING_SLOT_ID` becomes 100 automatically
(`PROFILES_MAX_COUNT`). Both `used_bitmap` and the favorites masks already
used the 4-word `profiles_slot_bitmap_t` from task 1; the NVS-persisted
`prof_favusr`/`prof_favbi`/`prof_used` keys now write and read the full
4-word blob (previously only `word[0]`), with read-side migration accepting
the old single-word/single-byte legacy blob (probed by blob length; falls
back to `hal_kv_get_u8`/`hal_kv_get_u32` plus `profiles_slot_bitmap_from_u32`
on a length mismatch). `_Static_assert` added on `profile_nvs_key()`'s output
length (<=15 chars — NVS key length limit).

**Owner decision from the paragraph above implemented:** `PROFILE_BENCH_SLOT_ID`
= `LIVE_EDIT_WORKING_SLOT_ID + 1` = 101, in new file
`profiles_bench_slot.h`. Same visibility exclusion as the live-edit slot:
excluded from `profiles_catalog_http.c`'s catalogue loop bound (`<
PROFILES_MAX_COUNT`), from `profiles_favorites.c` (`profiles_favorites_set()`
returns `ESP_ERR_INVALID_ARG` for it, `profiles_favorites_is()` returns
`false`), and from the LCD picker's deletable range
(`ui_page_profile_picker_is_deletable()` returns `false` for it, since it is
not `< PROFILES_MAX_COUNT` and not a builtin id either).

Mechanical sweep to the new constant: `profiles_catalog_http.c`,
`diagnostics_http.c` (kept as ONE aggregate `profiles` row, never 100
per-slot rows; `CFG_FS_STATUS_MAX_ITEMS` untouched at 18), `protocol.py`,
`full_board_backup.py`, host tests re-derived from `PROFILES_MAX_COUNT`
instead of hardcoding 7/8 (`test_ui_page_profile_picker_format.c`).
`check_profiles_capacity.py` already derived the constant dynamically and
needed no change. Did NOT touch `kiln_cfg_store`'s 8 config slots,
`BACKUP_BODY_MAX`, `backup_import.c`, any stack ceiling in
`check_all_task_stack_budgets.py`, any task stack size, or any httpd stack
buffer, and added no new HTTP route (`check_uri_handler_cap.ps1` unchanged
at 150/151).

New host tests in `test_profiles_http.c`:
`test_slot_bitmap_legacy_u8_migrates_on_read()` (stages real decodable
profile blobs plus a legacy single-byte `prof_used`, confirms
`nvs_load_all_from()` migrates it into the widened bitmap — staging a
bitmap byte alone is insufficient since a slot's used-bit is cleared again
if no decodable profile blob backs it, per the existing "one bad slot
doesn't take down the others" design);
`test_profiles_http_save_fills_all_100_then_reuses_deleted_slot()` (fills
all 100 slots, confirms a first-free-slot save is refused when full, deletes
slot 50, confirms the next first-free-slot save lands back at 50); and
`test_bench_slot_id_excluded_from_catalogue_favorites_and_lcd_order()`.
`test_slot_bitmap_persisted_byte_identical_for_8slot_fixture()` was
re-scoped to a fixed `SLOT_BITMAP_FIXTURE_SLOTS = 8` (independent of
`PROFILES_MAX_COUNT`) and its post-save assertion switched from a legacy
`nvs_get_u8()` read to a `nvs_get_blob()` read of the full bitmap, since a
save now always writes the widened blob.

Negative-tested: sabotaged `profiles_bench_slot.h` (`+ 1` -> `- 1`),
confirmed 5 assertion failures in the new bench-slot exclusion test,
restored the exact inverse edit by hand (file is new/untracked, so no
`git cat-file blob HEAD:...` baseline existed), then forced a full rebuild
(deleted `App/test/build`) and confirmed clean.

Verified in `C:\wt\s100t6_0juqog`: `kilnctl_host_tests.exe` 9319/9319,
`kilnctl_host_tests_profiles_http.exe` 819/819 (both from a from-scratch
forced rebuild). `check_00_kilnfw_target_build.ps1`,
`check_profiles_capacity.ps1`, `check_lint_pages.ps1`,
`check_js_host_tests.ps1`, `check_uri_handler_cap.ps1` all PASS.
`check_all_task_stack_budgets.ps1`: **correction, review 2026-09-19.** Task 6
as first delivered DID breach the `bx_flash_worker` ceiling (4064 B vs
3792 B); the claim that the breach was pre-existing was wrong. A fresh
from-scratch build of `origin/main` (`eac35270`) in a separate minted
worktree reports `bx_flash_worker` at exactly 3792 B -- at its ceiling,
PASS, deepest dispatch target `safety_poll_pico_half_recapture_job`
(3744 B). On the branch the deepest dispatch target became
`profiles_handle_message` (4016 B), and a per-symbol frame diff isolated the
whole +368 B to one frame: `nvs_erase_slot()` grew 144 B to 512 B because of
its `uint32_t rev_snapshot[PROFILES_MAX_COUNT]` local, which scaled with the
8 -> 100 widening. Fixed by dropping the snapshot copy entirely and
assigning `s_profile_rev[id] = new_rev` in place before persisting
`s_profile_rev` directly (identical persisted bytes) in both
`nvs_save_slot()` and `nvs_erase_slot()`. `bx_flash_worker` is back to
3792 B, 0 of 28 tasks over budget. No ceiling and no stack size was raised.

**Task 1 landed 2026-09-19.** Both `used_bitmap` (`profiles_http.c`'s
`s_profiles`) and the favorites user mask (`profiles_favorites.c`'s
`s_fav_user`) are now `profiles_slot_bitmap_t` (4x`uint32_t`, ids 0..127) --
see `profiles_slot_bitmap.h` for the type and its
test/set/clear/from_u32/to_u32 accessors. Every consumer (NVS load/save in
`profiles_http.c`, the two sites named below, `profiles_edit_http.c`'s free-
slot search/save/delete, and the host test fixtures) goes through the
accessors; nothing bit-tests the raw scalar directly any more. Persistence is
unchanged: `nvs_save_slot()`/`nvs_load_all_from()` and
`favorites_save()`/`profiles_favorites_start()` only ever read/write
`word[0]` via `profiles_slot_bitmap_to_u32()`/`_from_u32()`, so the persisted
NVS bytes and the 8-slot HTTP response bytes are byte-identical to before.
The two sites the paragraph below used to warn about are now fixed:
`profiles_catalog_http.c`'s `profiles_list_get_handler()` and
`favorites_list_get_handler()` both call the accessors instead of shifting
into a narrow scalar. `PROFILES_MAX_COUNT` was unchanged at the time (still 8,
task 6's job -- task 6 has since landed, see above; raised to 100). New host tests in `test_profiles_http.c`:
`test_slot_bitmap_persisted_byte_identical_for_8slot_fixture()` (asserts the
persisted `prof_used` byte for a full 8-slot fixture stays exactly `0xFF`,
and that the real loader reconstructs all 8 slots as used) and
`test_slot_bitmap_round_trips_high_ids()` (ids 31/32/33/99 -- the 32-bit
boundary that made the old scalar test undefined behavior -- set/test/clear
independently without disturbing each other or leaking into `word[0]`).
Negative-tested: deliberately broke `profiles_slot_bitmap_set()`'s word index
(`id / 16` instead of `id / 32`), confirmed a forced full rebuild failed
(10 failures in `test_profiles_http.c`), restored the source by hand, confirmed
a SHA-256 match against the pre-break file, then forced another full rebuild
and confirmed 54/54 host test executables green again.

**Task 3 landed 2026-09-19.** `s_profiles` (`profiles_state_t`, dominated by
`profiles[PROFILES_MAX_COUNT]`) is no longer a `.bss` global. It is now
lazily allocated by `profiles_storage_ensure()` (`profiles_http.c`) on first
use: PSRAM first (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`),
falling back to internal RAM (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`) if
PSRAM is unavailable/exhausted, and finally to a small static struct with a
logged failure if both allocations fail -- the store then starts empty but
never crashes. Every existing call site keeps compiling and behaving
unchanged: `profiles_http_internal.h` now declares
`profiles_state_t *profiles_storage_ensure(void);` and `#define s_profiles
(*profiles_storage_ensure())`, so `s_profiles.foo`, `&s_profiles`, and
`memset(&s_profiles, 0, sizeof(s_profiles))` (used ~30 times across
`test_profiles_http.c`) all still work verbatim -- the macro expands to a
dereference of the lazily-allocated pointer, never a plain global. The
host-test build's `esp_heap_caps.h` stub already backed `heap_caps_malloc()`
with real `malloc()` (caps ignored), so no new test infrastructure was
needed. NVS's write path was confirmed to copy the caller's buffer
synchronously (`nvs_set_blob()`/`hal_kv_set_blob()`), never DMA it, so a
heap/PSRAM-resident data buffer (as opposed to a PSRAM-resident task stack,
a distinct and unrelated hazard) is safe to pass to it. No new task was
registered or needed -- `check_all_task_stack_budgets.ps1`'s task count is
unaffected by this change. Negative-tested: deliberately made
`profiles_storage_ensure()` never cache its allocation (returning a fresh,
freshly-zeroed buffer on every call instead of caching it in
`s_profiles_ptr`), confirmed a forced full rebuild failed (46 failures in
`test_profiles_http.c`), restored the source by hand, confirmed a SHA-256
match against the pre-break file, then forced another full rebuild and
confirmed 54/54 host test executables green again.

Previously (now historical): **Task 1 was a hard, unstarted prerequisite for
task 6** — do not raise `PROFILES_MAX_COUNT` past 32 before it lands. Two
sites already do a bit test past what their current (narrower) types can
hold once ids reach 32: `profiles_catalog_http.c`'s
`profiles_list_get_handler()` (`s_profiles.used_bitmap & (1u << id)` —
`used_bitmap` is a `uint8_t`) and its `favorites_get_handler()` (`user_mask &
(1u << i)` on a 32-bit mask, undefined at `i` == 32). Both are inert at
today's 8 slots but must not be carried forward silently when task 6 runs.

**Task 9 was superseded, not completed as originally planned** and its
commit was reverted: `docs/UI_PLAN.md` section 6 Wave 2 E (already
owner-decided, on `origin/main` before task 9 was attempted) deletes
`ui_page_profiles_mine.c` entirely and replaces it with a full-list picker
built on a shared ordering module (`ui_profile_list_order.c`, built by a
separate concurrent session). Trimming the doomed page to favorites+recent
only would have been thrown away by that replacement; UI_PLAN owns the LCD
profile list going forward, not this plan.

**Owner decision, 2026-09-19 (post phase-A review):** beyond the 100 user slots and the
1 live-edit slot, there is **one additional hidden bench-harness slot** for
`docs/BENCH_TEST_SYSTEM_PLAN.md`'s bench test system to use. It is never listed,
exported, or shown on any web page or the LCD -- the same visibility exclusion the
live-edit slot already gets, extended to a second id. This pass did not implement it:
`PROFILES_MAX_COUNT` was still 8 (task 6 had not landed, see above), so there was no slot
layout yet for a bench-harness id to occupy one line of.

**Implemented 2026-09-19 as part of task 6**, once the 100/live-edit id numbering was
actually cut in: `PROFILE_BENCH_SLOT_ID` = 101, with its own visibility exclusions and a
host test proving it is excluded from the catalogue/favorites listing and the LCD picker's
deletable range. Full detail in the task 6 status block above.

---

## Status

Section 7 task 8 (web: name filter plus favorites/recent grouping) done.

- `profiles_page.html`: name filter input above the list (case-insensitive
  substring, client-side only); favorites stay pinned/stable while filtering
  (`renderFavorites()`/`renderRecent()` never read the filter query, verified
  structurally by the new test); new "Recently fired" section between
  Favorites and the filterable list, capped at 5, excluding favorites and
  never-fired profiles.
- `main_page.html`'s `#profileSelect`: `<optgroup>` grouping, order Favorites
  / Recently fired / All; an empty group renders no `<optgroup>`.
  `orderProfilesByFavorite()`/`isFavoriteProfile()`/`profileOptionLabel()`
  left untouched (mirrored by LCD C code, independently tested) -- the new
  `groupProfilesForPicker()` is additive.
- No new HTTP route: `last_run_started_unix_s` added to the two existing
  profile-list JSON responses (`profiles_list_get_handler`,
  `send_builtin_summary`) via a new thin accessor,
  `profile_executor_last_run_started_unix_s()`, forward-declared (not via
  `profile_executor.h`) in `profiles_catalog_http.c` so `test_profiles_http.c`
  keeps faking it instead of linking the heavy control-loop headers.
- New tests: `test_profiles_page_filter.js` (filter + recent, favorites
  pinned, 8 and 100 simulated slots), `test_profile_picker_optgroups.js`
  (optgroup order/exclusivity, 8 and 100 simulated slots). `PROFILES_MAX_COUNT`
  itself untouched (still 8).
- Negative-tested: inverted `filterProfilesByName`'s match sense, confirmed 5
  failures, restored by hand, re-confirmed 29/29 pass and restored file
  matches original.
- Verified: `check_js_host_tests.ps1`, `check_lint_pages.ps1`,
  `check_ui_responsive_sweep.ps1`, `check_00_kilnfw_target_build.ps1`, full
  C host-test suite (`test_profiles_http.c`'s 337/337) all green.

Section 7 task 7 (raise `BACKUP_BODY_MAX`, PSRAM the import buffer, 100/101-profile
tests) done, minus the `BACKUP_BODY_MAX` raise itself, which had already landed as
part of an earlier ("bkfinish") pass (`backup_http_internal.h:103`, already 131072,
body already read into a `content_len`-sized `MALLOC_CAP_SPIRAM` buffer in
`backup_import_post_handler()` -- confirmed unchanged here). `backup_export.c`'s
export side was confirmed to already stream via `backup_stream_printf()`/a small
`BACKUP_STREAM_BUF` chunk buffer, never buffering the whole document.

- `backup_import.c`'s `candidates[PROFILES_MAX_COUNT]` (`profile_candidate_t`, ~428 B
  each -- ~42.8 KB at the 100 slots a concurrent sibling worktree is raising
  `PROFILES_MAX_COUNT` to) no longer falls back to `malloc()` (internal DRAM) when the
  `MALLOC_CAP_SPIRAM` allocation fails. It now logs `ESP_LOGE` naming the byte count
  and refuses cleanly with the same "out of memory (profile candidates)" failure the
  caller already handled (a `*partial_write_out` 500, not a 400: kiln_configs[] has
  already committed by this point) -- no behavior change at today's 8 slots (PSRAM allocation
  never fails on this board), only the failure path at high slot counts. The sibling
  `zone_candidates`/`timing_profile_candidates` arrays are sized by
  `MAX31856_CHANNEL_COUNT` (3-4 entries), not `PROFILES_MAX_COUNT`, so their existing
  small `malloc()` fallback is not the hazard this task named and was left as-is.
- No other per-slot array sized by `PROFILES_MAX_COUNT` was found on the httpd stack
  or in `.bss` within the import/export path itself; the literal `profile_names[]`
  table in `diagnostics_http.c` and the several `PROFILES_MAX_COUNT`-sized locals in
  `profiles_http.c` are section 6/task 6's own scope (the constant raise), not
  touched here.
- New tests in `test_backup_import.c`: `test_import_exactly_max_count_profiles_succeeds`
  (a backup with exactly `PROFILES_MAX_COUNT` profiles imports every one),
  `test_import_over_max_count_profiles_refused` (`PROFILES_MAX_COUNT`+1 refused by the
  existing "backup has more than N profiles" name, not an overflow, nothing written),
  `test_import_live_edit_slot_id_rejected` (an id equal to `LIVE_EDIT_WORKING_SLOT_ID`,
  which is defined as `PROFILES_MAX_COUNT` itself, is rejected by the existing id-range
  check, never reaching `profiles_http_save()`), and
  `test_import_id_past_max_count_rejected` (an id further out, `PROFILES_MAX_COUNT`+5,
  same rejection). All four counts/ids are derived from `PROFILES_MAX_COUNT`/
  `LIVE_EDIT_WORKING_SLOT_ID` rather than hardcoded, so they pass unchanged whether the
  constant is today's 8 or the 100 the concurrent sibling worktree is landing.
- Negative-tested: widened the pass-1 profile-count guard from
  `candidate_count >= PROFILES_MAX_COUNT` to `candidate_count > PROFILES_MAX_COUNT`
  (an off-by-one letting one profile past the array's true capacity write into
  `candidates[PROFILES_MAX_COUNT]`, one element past the heap allocation), confirmed a
  forced full rebuild failed (the `test_backup_import` host-test executable crashed
  rather than reporting a clean assertion failure -- a heap buffer overflow, not a
  benign logic miss), restored `backup_import.c` byte-exact via
  `git cat-file blob HEAD:... > ...` (`git hash-object` match confirmed), reapplied
  this task's own PSRAM-fallback fix by hand on top of the restored file, then forced
  another full rebuild and confirmed 55/55 host test executables green again.
- Verified (this task): full C host-test suite (55/55 executables, including the four
  new `test_backup_import.c` cases), `check_00_kilnfw_target_build.ps1` (PASS, in
  `C:\wt\checkbuild_cc4e4b468a`), `check_all_task_stack_budgets.py` (0 of 28 tasks over
  budget), `check_httpd_task_stack_budget.ps1` (OK, ceiling unchanged at 4832 B,
  `backup_import_post_handler` still the deepest handler path at 4544 B),
  `check_uri_handler_cap.ps1` (150 of 151 -- unchanged, no new routes added).

Owner request 2026-09-19 (name uniqueness on save) done. `profiles_http_save()`
and `profile_post_handler()` (`profiles_edit_http.c` -- the actual `/api/profile`
POST handler, which bypassed `profiles_http_save()` entirely and needed its own
copy of the check) now both refuse a save whose name, case/whitespace-normalized,
collides with any other user slot or any read-only builtin catalogue entry, right
before the slot write. Both reuse `live_edit_name_collides()` (`live_profile.c`,
previously only reached from the live-edit SAVE_AS path), each via its own small
`name_at` seam backed by `s_profiles` -- matching the existing per-file-copy
convention (`profiles_live_http.c`'s `live_http_name_at()`) rather than one
shared symbol. `exclude_id` is the slot being written (or none, for a new slot),
so overwriting a slot with its own unchanged name stays legal. Every other save
path (`backup_import.c`'s batch commit, `profiles_export_http.c`'s single-profile
import, `profiles_live_http.c`'s SAVE_AS/OVERWRITE, the profile builder review
page) already funnels through `profiles_http_save()` and is covered without
further changes. `profiles_page.html`'s save and import flows already surfaced
`result.data.error`/`res.data.error`, so no client change was needed. New tests
in `test_profiles_http.c`: exact-duplicate rejected, case/whitespace variant
rejected, same-slot self-overwrite allowed, builtin-name collision rejected.
Negative-tested (short-circuited the new check with `false &&`, confirmed 6
failures, removed the sabotage, forced full rebuild, confirmed 830/830 clean).
Full host-test suite (56/56 executables) and
`run_all_checks.ps1 -Fast -Only "profile|lint|isolation|include" -AllowFewerChecks
-AllowSkips` (9/9) both green.

Opus review of that commit (5dd23944), fixed 2026-09-20: (A) the collision
message embedded the operator-supplied name unescaped in a hand-built JSON
body (`profile_post_handler()` and `profiles_export_http.c`'s single-profile
import), so a name containing `"` produced invalid JSON and the page showed
"could not reach the board" instead of the real refusal -- both now run it
through `json_escape()` after truncation; `profiles_page.html`'s save-error
path now also HTML-escapes the message before writing it into `innerHTML`.
(B) the paragraph above was wrong: a duplicate name inside a batch import did
NOT reject only that entry -- `backup_import.c`'s pass 2 committed profiles
one at a time, so a same-named pair (or a name colliding with an existing,
non-overwritten board slot) aborted the import half-applied, with entries
`0..i-1` already written. Fixed with a new pass-1 pre-check that scans every
named candidate for both an intra-batch duplicate and an existing-slot
collision (excluding every id the import will itself write) via
`live_edit_name_collides()`, refusing the WHOLE import before pass 2 writes
anything -- now genuinely all-or-nothing. New tests in `test_profiles_http.c`
(well-formed-JSON collision response, plain and quote-in-name) and
`test_backup_import.c` (intra-batch duplicate, existing-slot collision,
same-slot self-overwrite still allowed). Full host-test suite (56/56) and
`check_test_c_files_wired`/`check_c_files_in_cmakelists`/`check_js_host_tests`/
`check_lint_pages`/`check_host_embed_symbols_defined` all green.

## 8. Open owner questions

1. **Consume the entire 1.81 MiB tail, or stop at 2 MiB and keep 320 KiB spare?** Section 1
   recommends taking all of it: the 2 MiB option falls just under the partition's own 2x
   GC-headroom rule. The cost is that no further `app`-type partition can ever be added
   above `logs` without a restructure. If a second application image is ever contemplated,
   say so now -- it is much cheaper to decide here than after a flash.
