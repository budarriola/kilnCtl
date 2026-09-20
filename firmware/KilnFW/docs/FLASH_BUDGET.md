# Flash Budget — 16 MB chip, partition table and image size

Reference doc for the flash side of KilnFW: what the 16 MB part is spent on
and how it got there. All phases below landed; kept as reference for the
partition layout, the size-measurement tooling, and the hazards any future
partition-table change must respect.

As of `9ede138`+`0bbb21c` the chip-level squeeze this doc was originally
written to fix is gone — 3880 K is unallocated, 2368 K of it contiguous.
`attribute_str_pool.py` + `check_flash_partition_map.ps1` (`699f5ab`) are the
size/layout tooling this doc's measurements were taken with. §4.2's size
baseline is recorded against `eb17ea5`. §5.3 (`coredump`) stays untouched by
owner decision — see that section. §5.4 (image-size reclamation) was decided
not pursued — §4.1's attribution found the string pool is ordinary
spread-out `ESP_LOG*` strings, not dead weight. §7's checklist for the single
partition-table revision covering §5.1/§5.2 is closed: a live
`GET /api/partitions` read on 2026-09-03 confirmed the on-chip table matches
`partitions.csv`, 12/12 entries — see §8 item 3.

Companion doc: `DRAM_PSRAM_STATUS.md` covers internal SRAM. The two are
independent — neither blocks the other, and neither should be justified by the
other's numbers.

---

## 1. Two scarcities, and where each stands

Nothing here is urgent. The point of this section is that "38% free" is
measured against the app slot, not the chip, and the two must not be
conflated.

**Slot-level — comfortable.**

```
app slot (ota_0 / ota_1 / factory)   3,145,728 B
KilnCtrl.bin (commit eb17ea5)        1,936,320 B
free                                 1,209,408 B   (38.4%)
```

**Any size baseline recorded in this doc must name the commit it was taken
against.** A byte count with only a date attached cannot be checked later.
See §4.2 for the full baseline and per-archive table, pinned to `eb17ea5`.

**Chip-level — no longer tight.** Before `9ede138`/`0bbb21c` only 77,824 B of
the 16 MB part was unallocated (65,536 B contiguous), which is what this plan
was written to fix. After them, 3880 K is free in four pieces (§2): a 2368 K
contiguous tail at 0xDB0000, an 896 K fragment boxed between `factory` and
`coredump` (pico_img's old slot), a 604 K fragment boxed between `phy_init`
and `wifi_nvs`, and the 12 K gap below `otadata`. The 57,344 B pad above
`otadata` is structurally required — `gen_esp32part.py` forces app partitions
onto 64 K boundaries — and is not reclaimable.

The partition table now has room for another partition: a `web` partition for
OTA-able UI assets, a data-recorder, or a larger `logs` all fit in the tail
without further reclamation.

---

## 2. Where the 16 MB currently goes (measured, from `partitions.csv`)

| region | offset | size | note |
|---|---|---:|---|
| bootloader (32 K) + partition table (4 K) + `nvs` (24 K) + `phy_init` (4 K) | 0x0 | 64 K | live data in `nvs` |
| `pico_img` | 0x10000 | 896 K | RP2040 image relay staging; moved here by 9ede138 |
| *unallocated* | 0xF0000 | 604 K | boxed in by `phy_init` / `wifi_nvs` (what is left of `legacy_app`) |
| `wifi_nvs` + `kiln_nvs` + `profiles_nvs` | 0x187000 | 472 K | live data, immovable |
| *unallocated* | 0x1FD000 | 12 K | boxed in; three sectors, the NVS minimum |
| `otadata` (8 K) + forced 64 K-alignment pad (56 K) | 0x200000 | 64 K | pad not reclaimable |
| `ota_0` + `ota_1` + `factory` | 0x210000 | 9216 K | 3 × 3072 K |
| *unallocated* | 0xB10000 | 896 K | `pico_img`'s old slot, boxed in by `factory` / `coredump` |
| `coredump` | 0xBF0000 | 1024 K | sized empirically, see 5.3 |
| `logs` | 0xCF0000 | 768 K | shrunk from 3072 K by 9ede138, see 5.2 |
| *unallocated tail* | 0xDB0000 | 2368 K | contiguous, the largest this table has ever had |
| **total** | | **16,384 K** | = 16,777,216 B ✓ |

**Stale as of 2026-09-19**: this table predates both `docs/OTA_SINGLE_SLOT_PLAN.md`'s
`ota_0`/`ota_1`/`factory` -> `app`/`recovery` redesign and `cfg`, the LittleFS
partition that now occupies the "unallocated tail" row above -- `cfg` was
grown to take the entire remaining tail 2026-09-19
(docs/PROFILE_SLOTS_100_PLAN.md section 7 task 5), `0xDB0000`, size
`0x250000` (2.31 MiB), per `partitions.csv`'s own comment block. A full
re-measurement against the current table is a separate task, not done here.

One item dominates what is *spent*: **9 MB of app slots**, 56% of the chip.

The 9 MB is the price of dual-OTA plus a factory recovery image, and all three
app partitions must be the same size — an OTA image has to fit either slot, and
`factory` must stay a meaningful recovery target after an update. ESP-IDF's
`check_sizes.py` takes `min()` across all app partitions, which is what forced
all three to 3 MB together on 2026-08-21. **This 9 MB is not a candidate for
reclamation.**

### 2.1 "Why not two slots instead of three?" — asked and answered

The obvious reclamation is to drop `factory` and keep only "one known-good
image to fall back to, one that gets updated." It would free 3 MB, more than
every other target in this plan combined. It was investigated and **rejected**;
recorded here so it is not re-proposed as though new.

Rollback *is* implemented: `main.c:241` calls
`esp_ota_mark_app_valid_cancel_rollback()` from `ota_rollback_confirm_task()`,
gated by `boot_confirm_decide()` (`boot_guard.c:270`) on
`nvs_ok && web_ok && ota_routes_ok`. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
(`sdkconfig:698`); anti-rollback deliberately off (`sdkconfig:700`), matching
`UPDATE_PROTOCOL.md` §3.

**Hardware-verified 2026-09-03.** A real `ota_update_esp()` push (`POST
/api/ota/esp`, not JTAG) wrote `KilnCtrl.bin` into `ota_0`; after reboot the
board ran `BOOT_CONFIRM_CONFIRM_OTA_SLOT`, logged
`running partition: 'ota_0' (subtype 0x10)`, and
`esp_ota_mark_app_valid_cancel_rollback()` returned `ESP_OK` ("OTA rollback
confirmed" in the log) once NVS/web/OTA-routes were up. See
`UPDATE_PROTOCOL.md` §7 for the full account, including a JTAG-reset artifact
(I/O expander failed its first post-reset init, tripping a stale S6a) that
cleared on a second reset and is unrelated to the rollback-cancel path itself.

With `factory` present and `otadata` erased or corrupt, the bootloader boots
`factory` — recovery with no serial cable. Without it, that degrades to "boot
whatever is in `ota_0`", possibly the very image that failed. **Caveat:** the
no-factory fallback order is general ESP-IDF behaviour and was *not* verified
against IDF v6.0.2's bootloader source in this tree. Verifying it is a
prerequisite to ever revisiting this.

Naming trap worth knowing, since three things here sound related and are not:
`factory_reset.c` is a scoped **NVS wipe** with no reference to app partitions;
`boot_button.c` and `danger_mode.c` contain zero partition references. Only
`boot_guard.c`, `main.c` and `test_boot_guard.c` touch
`ESP_PARTITION_SUBTYPE_APP_FACTORY`.

**Action item — done, 2026-09-03:** one real OTA into `ota_0` on hardware, app
confirmed valid. See above.

### 2.2 "Shrink factory to a minimal recovery image?" — DECIDED: no, keep as-is

A follow-on idea: keep `factory` but fill it with a recovery-only app (boot,
Wi-Fi or AP, upload page, write to `ota_0`/`ota_1`) instead of a full copy of
KilnCtrl, letting the partition shrink from 3072 K to ~1024 K and freeing ~2 MB
— more than `legacy_app` and `logs` combined.

**Owner decision 2026-09-01: keep `factory` at 3072 K for now.** Not rejected on
merit; deferred. Recorded so the arithmetic does not have to be redone.

Estimated floor for such an image, from this build's per-archive numbers:
Wi-Fi (`net80211`+`pp`+`phy`+`wpa_supplicant`) 317,062 B, `lwip` 90,418 B,
`esp_http_server`+`http_parser` 20,874 B, core (`spi_flash`, `nvs_flash`,
`freertos`, `esp_system`, `esp_hw_support`) ~100,738 B, plus netif/wifi/heap/
libc/vfs and the recovery app's own code — **roughly 700–800 KB, estimated, not
measured.** Droppable: `liblvgl.a` (267,303 B), the embedded web assets
(261,777 B), most of `libdrivers.a` code (302,052 B), and mbedtls/tfpsacrypto
(~280 KB) if recovery serves plain HTTP.

**The size check is NOT the obstacle — `partitions.csv` overstates it.** That
file's own note calls the 2026-08-21 episode a "build hard-failure fix". Read
against the actual tool (`check_sizes.py:88-102` in the IDF v6.0.2 checkout),
the hard `SystemExit` fires only when **every** app partition is too small for
the binary:

```python
too_small_partitions = [p for p in partitions if p.size < bin_size]
if not allow_failures and len(partitions) == len(too_small_partitions):
    raise SystemExit(...)   # only if ALL are too small
else:
    print('Warning: ' + msg)
```

It is invoked with `--type app` and no subtype
(`post_build_validation.cmake:24-33`), so `partitions` = {ota_0, ota_1,
factory} and `min()` does span all three — but a 1 MB `factory` beside two 3 MB
OTA slots yields one too-small partition out of three, i.e. **a warning and a
passing build.** The 2026-08-21 event was, by this logic, also only ever a
warning. There is no per-partition opt-out in Kconfig (`sdkconfig:964-975`);
`--allow_failures` exists only on the CLI and is not wired to any option — and
per the above it is not needed.

**The real obstacle is the build graph.** ESP-IDF cannot produce two
independently-linked app images from one `idf.py build`. A minimal recovery app
therefore means a **second IDF project** sharing `partitions.csv`, with its own
build and flash step and its own drift risk against the main firmware. No
official IDF example exists for this; the `recovery_bootloader` variants under
`examples/system/ota/partitions_ota/` are the backup *second-stage bootloader*
feature, which is a different thing entirely.

**And it would collide with an existing mechanism.** This firmware *already has*
a recovery mode — `boot_guard.c:102` sets `recovery_mode` from a boot counter
past `RECOVERY_MODE_BOOT_THRESHOLD`, exposed via `boot_guard_is_recovery_mode()`
(`boot_guard.c:253`), with a full HTTP surface in `ota_http.c` (status JSON at
1636-1666, a `/api/ota/esp/recovery_exit` handler at 1710-1761 with its own HMAC
context and lockout, plus a BOOT-button bypass at 355-366). That is a degraded
mode of the *same* image, not a separate binary. The two are complementary
rather than redundant, but any minimal-factory design has to decide whether the
crash-loop counter should ever route the device into `factory` at all. **That
integration question, plus the second project, is the real cost** — not the
partition-size check, and not the byte count.

---

## 3. Where the 1.92 MB image goes (measured, `esp_idf_size --archives`)

**Superseded by §4.2's table**, which pins the same measurement to a named
commit (`eb17ea5`) — the numbers below predate that and drift slightly (e.g.
`libdrivers.a` was 637,022 B here, 645,138 B at `eb17ea5`). Kept for the
narrative discussion below the table, which still holds.

Top contributors to the ELF:

| archive | total | of which |
|---|---:|---|
| `libdrivers.a` | 637,022 B | 302,052 code + **261,777 rodata** (the embedded web assets) |
| `liblvgl.a` | 267,303 B | 238,823 code |
| `libesp_stdio.a` | 239,887 B | **239,329 B is the merged string-literal pool — see 4.1** |
| `libnet80211.a` | 150,770 B | Wi-Fi MAC |
| `libtfpsacrypto.a` | 96,916 B | ┐ |
| `liblwip.a` | 90,418 B | │ networking + TLS, ~255 K combined |
| `libwpa_supplicant.a` | 66,997 B | ┘ |
| `libpp.a` | 64,816 B | |
| `libphy.a` | 34,479 B | |

Build is already `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`-Os`), with
`CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_SIZE=y` as well. The easy global switches
are already thrown; what remains is content, not flags.

---

## 4. Phase 0 — measurement discipline (do first, cheap)

### 4.1 Confirm what the 239 kB string pool actually contains

`esp_idf_size --archive_details libesp_stdio.a` attributes 239,329 B of the
image to a single entry, `.rodata.console_access.str1.1`.

**This is not esp_stdio's data.** `.str1.1` is GCC's mergeable string-literal
section; the linker pools every such section from every object into one output
section, and the map file attributes the whole pool to whichever input object
contributed first — here, `console_access` in esp_stdio. What the 239 kB
actually holds is *every string literal in the firmware*, which for this
codebase means predominantly `ESP_LOG*` format strings and tags.

A prior read of the raw map file mis-attributed this to printf
floating-point formatting. It is not that, and the corresponding fix
(`NEWLIB_NANO_FORMAT`) would have reclaimed nothing. Recorded here so the
wrong diagnosis does not get rediscovered.

**Task:** attribute the pool properly — per-source-file string-literal volume —
before deciding anything in 5.4 is worth doing. Until that attribution exists,
239 kB is a number without an owner.

### 4.2 Size baseline — recorded against `eb17ea5` (2026-09-02)

**Commit:** `eb17ea5` ("Give the coupling matrix a real preset and a client
that can post it"). Working tree was clean (`git status` showed no tracked
changes) and `build_kilnfw` passed before this build; `tools/run_all_checks.ps1`
was 23/23 both before and after. This is a clean-tree baseline, unlike the
2026-09-02 attempt recorded lower in this section's history, which was blocked
by a concurrent agent's in-progress work and produced only non-authoritative
numbers.

**Method:** `idf.py build` from a clean `eb17ea5` checkout, then
`esp_idf_size` (the IDF v6.0.2 venv's `python -m esp_idf_size`, same tool the
CLI's `idf.py size` / `size-components` wrap) against
`build/KilnCtrl.map` — both the summary and `--archives` forms. Per §4.1's
methodology note, the per-source-file string-pool breakdown instead came from
`App/test/attribute_str_pool.py`, which reads the map's "(size before
relaxing)" lines rather than the merged-pool placement column; `esp_idf_size
--archives` totals below are per-*archive*, not per-file, and are not subject
to that trap — the trap is specific to the single merged `.str1.1` pool
section discussed in §4.1.

**Per-archive contributions (`esp_idf_size --archives`, top 10 by total size):**

| archive | total | flash code | flash data (rodata) |
|---|---:|---:|---:|
| `libdrivers.a` | 645,138 B | 303,924 B | 267,501 B |
| `liblvgl.a` | 267,303 B | 238,823 B | 27,916 B |
| `libesp_stdio.a` | 241,142 B | 394 B | 240,732 B (merged string pool, §4.1) |
| `libnet80211.a` | 150,770 B | 123,414 B | 14,845 B |
| `libtfpsacrypto.a` | 96,924 B | 80,509 B | 15,763 B |
| `liblwip.a` | 90,450 B | 82,158 B | 3,684 B |
| `libwpa_supplicant.a` | 66,997 B | 63,788 B | 1,838 B |
| `libpp.a` | 64,816 B | 42,381 B | 3,853 B |
| `libphy.a` | 34,479 B | 27,966 B | 0 B |
| `libespressif__mdns.a` | 31,677 B | 28,528 B | 987 B |

Consistent with §3's table and the same rank order; absolute values differ
slightly because §3 predates this commit (now noted there).

**String-pool per-file attribution** (`attribute_str_pool.py`, top 5, raw
pre-dedup bytes): `dashboard_http.c.obj` 11,350 B, `mesh_parent.o` 11,108 B,
`main.c.obj` 9,671 B, `ota_http.c.obj` 9,510 B, `autotune_engine.c.obj`
7,611 B. Merged pool 240,584 B; raw sum 310,317 B; 69,733 B (22.5%) dedup gap.
Close to but not identical to §4.1's `699f5ab` figures — expected drift over
26 intervening commits, same top contributors.

**Image size and free space, smallest app partition:**

```
KilnCtrl.bin (eb17ea5)                1,936,320 B
  (esp_idf_size total image, unpadded 1,936,206 B)
smallest app partition (ota_0 / ota_1 / factory, all equal)  3,145,728 B
free                                   1,209,408 B   (38.4%)
```

`ota_0`, `ota_1`, and `factory` are all 3,145,728 B (`check_flash_partition_map.ps1`
confirms equal-sized app partitions, per §2's constraint), so "smallest app
partition" and "the app slot" are the same figure post-`9ede138`.

**Superseded by this baseline:** §1's old 2026-09-01 slot-level figures
(1,924,496 B / 1,221,232 B / 38.8%, dated but not commit-named) and §3's
per-archive table (predates `9ede138`'s partition-table change, though the
*image* contents that table describes were not directly affected by the
partition move — only the slot-level percentage in §1 was). The partition
table itself changed at `9ede138` (`legacy_app` reclaimed, `pico_img`
relocated to 0x10000, `logs` 3072K→768K, ~3.71 MiB freed).
`check_flash_partition_map.ps1`'s "Expected-map check" (§4.3) confirms the
checked-out `partitions.csv` matches that post-`9ede138` shape — **that is a
repo check, not a board check.** The reflash HAS happened: `flash_firmware()` was run on 2026-09-02 and
reported "flashed and verified OK (bootloader + partition table + app),
board reset and running" — that tool writes and verifies all three images,
so the post-`9ede138` table is on the chip. Nothing readable over HTTP
reports the on-chip table, so a later session wanting independent
confirmation must read it back with esptool/OpenOCD rather than inferring
it from the running app's build commit.

No new check was needed for this section — §4.3's `check_flash_partition_map.ps1`
already runs in `tools/run_all_checks.ps1` and already validates the current
partition shape; this section is a doc-only baseline capture using existing
tooling (`attribute_str_pool.py`, `esp_idf_size`), per the task's instruction
to reuse rather than write new.

### 4.2a `CONFIG_LV_USE_TJPGD` cost — measured 2026-09-03

DISPLAY_ST7796_PLAN.md Phase 7 ("`LV_USE_TJPGD` if images are wanted").
Flipped on in `sdkconfig.defaults` (default OFF in upstream LVGL); the
decoder itself is already vendored under `components/lvgl/src/libs/tjpgd`,
so this costs nothing to add beyond the Kconfig flip. Measured with
`build_kilnfw`, same tree, only the flag toggled:

```
KilnCtrl.bin, CONFIG_LV_USE_TJPGD off   0x1e3f90 (1,982,352 B)
KilnCtrl.bin, CONFIG_LV_USE_TJPGD on    0x1e52c0 (1,987,264 B)
delta                                   +4,912 B  (+0.25% of the 3,145,728 B app slot)
```

Confirmed the decoder actually compiled into the tree, not just accepted by
Kconfig: `build/esp-idf/lvgl/CMakeFiles/__idf_lvgl.dir/src/libs/tjpgd/
tjpgd.c.obj` and `lv_tjpgd.c.obj` are present after the ON build. No caller
decodes a JPEG yet (no page uses `lv_image`/`lv_img` with a `.jpg` source) --
this only registers the decoder at LVGL init, so the ~4.9 KB is paid whether
or not anything ever calls it. Negligible next to §2's ~38% free headroom on
the app slot; not worth gating behind its own flag the way the panel-facing
Kconfig options in `App/drivers/Kconfig` are, since it has no wire-behavior
or bring-up risk -- it is pure decoder-table/code, inert until an image path
calls into it.

### 4.3 A partition-map check

The arithmetic in section 2 was done by hand from the CSV comments. That is
exactly the kind of thing that silently goes stale — the table has already been
restructured three times (OTA, the `factory` move, `logs`). A small script that
prints the computed map and the unallocated remainder, runnable from the build,
makes the next restructure's arithmetic checkable instead of asserted.

---

## 5. Phase 1 — partition reclamation

Ordered by yield per unit of risk. Every item here requires reflashing the
partition table, which carries fixed hazards listed in section 7 — so if more
than one is done, do them in a single table revision, not several.

### 5.1 `legacy_app` — 1500 kB — LANDED (`9ede138`): `pico_img` relocated into it

The 1500 kB hole `factory` left when it moved to 0x810000 on 2026-08-21.
`pico_img` (896 kB) now sits at its start (0x10000); the remaining 604 kB
stays unallocated and boxed in by `phy_init` below and `wifi_nvs` above. This
was the one candidate use that improved contiguity rather than occupancy — it
freed `pico_img`'s old high slot (896 kB at 0xB10000). No data-loss risk:
nothing ever lived there, and `pico_img` is looked up by name, not offset.

The alternatives not taken, if the 604 kB is ever wanted: a `web` SPIFFS
partition for OTA-able UI assets (`WEB_UI_RESPONSIVE.md` §8 would be the
consumer), or a data-recorder / expanded statistics partition. It is too small
for an app partition (3 MB here) and is therefore data-only.

### 5.2 `logs` — LANDED: cap 1 MiB -> 256 kB/kind (`0bbb21c`), partition 3072 K -> 768 K (`9ede138`)

`log_store.h` now sets `LOG_STORE_MAX_TOTAL_BYTES` to 256 KiB per kind
(8 × 32 KiB segments) against a 768 K partition — the same ~2/3 fill ratio the
3 MB/1 MiB pair had. Reasoning kept below, since it governs any future resize.

Headroom over the cap is deliberate: SPIFFS degrades at near-full, with slower
mounts and more GC. Any future resize must keep that ratio, and must decide the
retention question (how long a history is wanted) before the partition size —
that is a product question, not a flash one. §5.2a is the answer that made
256 kB workable.

### 5.2a Follow-on, 2026-09-02 — DECIDED: flash holds binary events only, never per-tick text

Landing the 256 kB/kind cap (5.2 above) exposed the real problem it was
masking: the store held one text FIRE/TUNE line every 5s/10s (~136 KiB/hour),
truncating any firing over ~2h against the new cap. Owner decision, verbatim:
*"dont log the temps to flash, log errors,warnings,infos that are nessary for
debug. be frugal. dont do it in human readable form. loging of temps for
debug should be done over the uart interface"* — that UART path already
existed (`telemetry_log.c`'s `ESP_LOGI` feed, opt-in, unchanged by this).

Implemented: `log_store.c` is now a generic binary length-prefixed record
store; `event_log.h`/`.c` define a fixed 32-byte record (magic+version,
severity, source, code, zone, uptime, arg, short note) written only on a
genuine state transition (run started/paused/resumed/done/faulted, autotune
started/done/aborted) — never per-tick. `GET /api/logs/{firing,autotune}`
now streams raw binary; decode with `tools/PcTools/src/kilnctrl/
event_log_decoder.py`, which refuses (does not misread) any log written
before this change. Result: 256 kB/kind now holds thousands of firings'
worth of events, where the old scheme filled it in under 2 hours of one.

### 5.3 `coredump` — 1024 kB, examine but probably leave alone

Reachable in principle, but this partition was sized *empirically* and painfully:
64 kB failed silently, 512 kB was insufficient once `CONFIG_ESP_COREDUMP_CAPTURE_DRAM`
was enabled, and 1024 kB is the value that works. `CONFIG_ESP_COREDUMP_MAX_TASKS_NUM`
is 64 and this firmware runs a few dozen tasks.

Its failure mode is the worst kind: silent, and manifesting only at the moment
the diagnostic was needed. **Recommendation: leave it.** If it is ever shrunk,
the change must be validated by forcing a real panic under full task load and
confirming the dump decodes — not by reasoning about sizes.

### 5.4 Image-size reclamation — DECIDED: not pursued

Lower priority than the above, because the app slot has 1.22 MB free and
shrinking the image reclaims nothing at chip level — it only widens
already-adequate slot headroom. Worth doing only if 4.1 shows the string pool
is dominated by log strings that are genuinely dead weight.

**4.1's attribution (`699f5ab`) answers this: the pool is not dead weight.**
The top per-file contributors are `dashboard_http.c` (11,273 B),
`mesh_parent.o` (11,108 B), and `main.c` (9,671 B) — ordinary `ESP_LOG*`
format strings and tags spread across the driver set, not a concentration in
one droppable module. Combined with the caution below, this closes 5.4
without a code change: attribute-then-decide (4.1) was the whole task, and
the decision is not to trade the strings.

If ever revisited, the levers would be `CONFIG_LOG_DEFAULT_LEVEL` /
`CONFIG_LOG_MASTER_LEVEL` and pruning verbose logging in the noisiest
modules. **Caution:** this codebase's log strings are a primary debugging
asset, and several documented incidents in `docs/` were diagnosed from
exactly these messages. Trading them for flash that is not currently scarce
is a bad trade.

The 261,777 B of embedded web assets in `libdrivers.a` are **not** a target —
they are already gzipped, and they are the reason the pages cost zero RAM. See
`DRAM_PSRAM_STATUS.md` section 1.

**Re-verified 2026-09-02, not reopened:** `App/drivers/CMakeLists.txt`
pre-gzips every embedded page/`theme.css` at build time (`gzip.open(...,
'wb', 9)`, level 9) into `KILNCTL_GZIP_ASSET_FILES`, which is what
`EMBED_TXTFILES` actually embeds; `page_get_handler` serves the `.gz` body
directly with a `Content-Encoding: gzip` header via
`web_client_accepts_gzip()`/`web_send_gzip_not_acceptable()`, same as every
other `*_http.c` handler. The one exception is `tuning_recommendations`
(new in `333dd4e`), served plain because it is a ~400 B–few-KB JSON blob —
not worth a second content-negotiation path, per that commit's own
reasoning. There is no remaining uncompressed asset to gzip.

---

## 6. What this plan does not propose

- Shrinking `ota_0` / `ota_1` / `factory`. See section 2.
- Removing the `factory` recovery image.
- Moving `nvs`, `wifi_nvs`, `kiln_nvs`, or `profiles_nvs`. These hold live data
  and the whole partition table is built around never moving them.
- Dropping the embedded web assets to a filesystem purely to shrink the image.
  That may be worth doing for OTA-able UI (5.1), but not for size.

---

## 7. Hazards common to any partition-table change

Applies to every item in section 5. These are drawn from the table's own
history and are not hypothetical — each has already caused a problem here once.

1. **`otadata` must be erased** when app partition offsets or sizes change.
   Stale slot-selection state against a changed layout risks the bootloader
   trusting an index that no longer means what it did.
2. **A core dump sitting in flash is orphaned** by any move of `coredump`. Read
   it out with `espcoredump.py` before flashing a new table, or lose it.
3. **The bootloader must be reflashed** against the new table.
4. **Archive the NVS partitions first.** The 2026-08-17 pass flagged reading and
   archiving `nvs`/`wifi_nvs`/`kiln_nvs`/`profiles_nvs` off the physical board
   as a one-time, irreversible-if-skipped step. Confirm this has actually been
   done before any table revision, not assumed.
5. **Append-only discipline.** Every revision since the OTA pass has kept
   existing entries byte-identical and appended. Reclaiming `legacy_app`
   breaks that pattern for the first time — it is a *re-use of a declared
   hole*, which is safe, but it should be called out explicitly in the commit
   and diffed entry-by-entry rather than eyeballed.

---

## 7a. Related prior work — read before starting

- **`TODO.md` §14** already investigated an HTTP-reset pattern that looks
  exactly like DRAM exhaustion and **ruled that cause out** — it was an
  uncounted permanent socket defeating `lru_purge_enable`'s recovery
  (`a5567ae`). Do not re-diagnose it as a memory problem.
- **`TODO.md` §13** already began the stack candidate/classification work that
  `DRAM_PSRAM_STATUS.md` §6 restates.
- **`UPDATE_PROTOCOL.md` §3 and §7** own the OTA layout and its open items; §2.1
  above summarises but does not supersede them.

## 8. Suggested order

1. ~~Phase 0 (4.1–4.3)~~ — done. 4.1/4.3 landed at `699f5ab`; 4.2's baseline
   is recorded against `eb17ea5` (see 4.2).
2. ~~Decide the `logs` retention question (5.2)~~ — decided and landed
   (`0bbb21c`).
3. **The remaining hardware step**, for the single table revision covering
   5.1 (`9ede138`) and 5.2 (`0bbb21c`): confirm what table is actually on the
   chip (read it back — no repo check and no HTTP endpoint can tell you), and
   if it is still the old one, work §7's checklist in order — archive the four
   NVS partitions first, reflash the table and bootloader, erase `otadata`,
   and do not touch `coredump` in the process.

   **Read-back mechanism — REVISED 2026-09-02.** The original approach (raw
   partition-table bytes over JTAG at flash offset 0x8000, via
   `debug_probe.read_memory()`) **does not work**: confirmed against the
   real board with

   ```
   failed to read 4096 B from esp flash at 0x8000
   DEPRECATED! use 'read_memory' not 'mem2array'
   failed to read memory
   ```

   0x8000 is a FLASH offset, not a memory-mapped address on the ESP32-S3 —
   OpenOCD's `read_memory` cannot reach it. The parse/diff logic that read
   fed into was always correct (proven against synthetic blobs), but the
   read itself was the one piece no test exercised for real, because the
   tests inject a fake `read_memory_fn` in place of the OpenOCD call.

   **Current mechanism**: `GET /api/partitions`
   (`firmware/KilnFW/App/drivers/http/partition_info_http.c`), a new endpoint
   that reports the RUNNING firmware's own live partition table via
   ESP-IDF's `esp_partition_find()`/`esp_partition_next()` iterator, called
   from inside the app itself — no JTAG, no core halt, works while the
   board is busy serving other requests. It answers a strictly better
   question than a raw flash dump: not "what bytes sit at 0x8000" but "what
   table is the firmware actually using", plus which OTA slot
   (`esp_ota_get_running_partition()`) is running. Response is chunked
   (`httpd_resp_send_chunk()`, one chunk per partition entry) so there is no
   fixed-size buffer to overrun regardless of table size.

   `tools/PcTools/src/kilnctrl/partition_table.py` keeps its parse/diff
   core unchanged — `parse_partitions_csv()`, `diff_partition_tables()`,
   `PartitionDiff` — only the chip-side read is re-pointed at this
   endpoint's JSON (`partition_http_client.py`,
   `read_chip_partition_table_from_http()`,
   `check_chip_partition_table_via_http()`). The old JTAG-based
   `read_chip_partition_table_bytes()`/`check_chip_partition_table()` are
   KEPT in the module (their logic is sound and still unit-tested) but are
   explicitly marked deprecated in their own docstrings — nothing calls
   them by default any more, and nobody should expect a real chip read from
   them to succeed. Exposed two ways:
   - MCP tool: `kiln_call(name="debug_check_partition_table")` (optional
     `host`, `csv_path` args — same host-resolution order as every
     `ota_*`/`adaptive_tune_*` tool).
   - CLI: `uv run --project tools/PcTools python
     tools/PcTools/scripts/check_chip_partition_table.py [--host ...]`.

   Reports `MATCH` or a `MISMATCH` naming exactly the partition(s) and
   field(s) that differ (type/subtype/offset/size), or any partition present
   on only one side. Unit-tested against synthetic blobs/mocked HTTP
   responses (`tools/PcTools/tests/test_partition_table.py` and
   `test_partition_http_client.py`, no board required) — including a proof
   that a single mutated field (`coredump` size changed 0x100000 ->
   0x200000 in a scratch CSV copy) is reported as exactly that one
   entry/field and nothing else, then the mutation was reverted. Host-side
   handler JSON emission is covered by
   `firmware/KilnFW/App/test/test_partition_info_http.c`
   (`build_host_tests.ps1`).

   **DONE — 2026-09-03.** The board was reflashed 2026-09-03 07:36:20 and a
   live `GET /api/partitions` read reported `MATCH`, 12/12 entries against
   `partitions.csv` (running partition `factory`; `ota_0`/`ota_1` each
   3,145,728 B at 0x210000/0x510000). The on-chip table is the post-`9ede138`
   table this plan targeted. §8's remaining hardware step is closed; nothing
   in this plan is still open.
4. Leave `coredump` (5.3) alone unless something forces the issue.
5. ~~Revisit 5.4 only if 4.1 justifies it~~ — 4.1 does not justify it;
   decided not pursued.

## Embedded SaftyFW slot images (2026-09-20)

Owner decision 2026-09-20 (overriding `docs/PICO_AUTO_UPDATE_PLAN.md` sec 2's
"CORRECTION 2026-09-18"): `App/drivers/CMakeLists.txt` now `EMBED_FILES`s
both SaftyFW two-slot bootloader binaries
(`firmware/SaftyFW/build/SaftyFW_slotA.bin` /
`SaftyFW_slotB.bin`) directly into `KilnCtrl.bin`, refusing to configure if
either is missing. Measured this pass, same worktree/toolchain/sdkconfig,
`check_00_kilnfw_target_build.ps1`:

```
KilnCtrl.bin, with both embedded slots (this commit)   2,698,672 B
  SaftyFW_slotA.bin                                       119,044 B
  SaftyFW_slotB.bin                                       119,044 B
  raw embedded payload                                    238,088 B
KilnCtrl.bin, without embedding (computed: measured total
  minus the exact raw embedded payload above)          ~2,460,584 B
app partition (ota_0 / ota_1 / factory, all equal)     3,145,728 B
free with both slots embedded                            447,056 B  (14.2%)
```

The "without embedding" figure is computed by subtracting the two slot
files' exact on-disk byte counts from this pass's measured total, not from a
second twin build with `EMBED_FILES` removed — the two `.bin`s are raw,
uncompressed payloads (`EMBED_FILES`, not `EMBED_TXTFILES`/gzip), so no
compression ratio or other nonlinearity is in play, and the only material
difference from an actual twin build is a few bytes of section-alignment
padding. 14.2% headroom remains on the smallest app partition with both
slots embedded — comfortable, but this is the single largest jump this
document has recorded from one change (prior baselines moved by tens of KB
across many commits; this one is +238,088 B in one step) and any second
embedded artifact proposed later should be weighed against this section, not
just against the raw partition size.
