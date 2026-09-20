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
4. **Add `test_profiles_capacity.c`**, parsing `partitions.csv`, asserted against the
   *current* 8 slots and the *current* 0x80000 `cfg`. It must pass before anything grows.
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
Tasks 1, 3, 6, 7, 8, 12 are NOT started.

**Task 1 is a hard, unstarted prerequisite for task 6** — do not raise
`PROFILES_MAX_COUNT` past 32 before it lands. Two sites already do a bit test
past what their current (narrower) types can hold once ids reach 32:
`profiles_catalog_http.c`'s `profiles_list_get_handler()`
(`s_profiles.used_bitmap & (1u << id)` — `used_bitmap` is a `uint8_t`) and
its `favorites_get_handler()` (`user_mask & (1u << i)` on a 32-bit mask,
undefined at `i` == 32). Both are inert at today's 8 slots but must not be
carried forward silently when task 6 runs.

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
live-edit slot already gets, extended to a second id. This pass does not implement it:
`PROFILES_MAX_COUNT` is still 8 (task 6 has not landed, see above), so there is no slot
layout yet for a bench-harness id to occupy one line of. When task 6 lands and the
100/live-edit id numbering is actually cut in, revisit whether adding id 101 alongside
it is still a one-line addition against that new layout, and if so add it then, with its
own visibility exclusions and a host test proving it is excluded from the catalogue/
favorites listing, the web page, and the LCD page -- not before.

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

## 8. Open owner questions

1. **Consume the entire 1.81 MiB tail, or stop at 2 MiB and keep 320 KiB spare?** Section 1
   recommends taking all of it: the 2 MiB option falls just under the partition's own 2x
   GC-headroom rule. The cost is that no further `app`-type partition can ever be added
   above `logs` without a restructure. If a second application image is ever contemplated,
   say so now -- it is much cheaper to decide here than after a flash.
