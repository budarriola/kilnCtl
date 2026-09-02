# Flash Budget Plan — 16 MB chip, reclaiming image and partition space

Plan doc for the flash side of KilnFW: what the 16 MB part is currently spent
on, why only 64 kB of it is unallocated, and what can be reclaimed.

Conventions this doc follows, matching `PID_EXPANSION_PLAN.md`: **the code is
truth, not the checkboxes.** Nothing is marked done until a commit is named.
Every number below is measured and attributed, or explicitly labelled as an
estimate.

Status at time of writing (2026-09-02): **Phase 0 (§4.1/§4.2/§4.3) is built.**
§5.1 and §5.2 are landed (`9ede138`, `0bbb21c`). §5.3 stays untouched by owner
decision. §5.4 is decided-not-pursued (§4.1's attribution found the string
pool is ordinary spread-out `ESP_LOG*` strings, not dead weight). §4.2's size
baseline is now recorded against `eb17ea5` — see 4.2. §7's checklist for the
single partition-table revision covering §5.1/§5.2 is still the remaining
hardware step; nothing has been reflashed under this plan yet.

**Phase 0, done (`699f5ab`):** `attribute_str_pool.py` +
`check_flash_partition_map.ps1`. Methodology correction for §4.1: the map
file's size column shows placement in the merged pool, not the input
section's own size — true per-file sizes come from the "(size before
relaxing)" lines. Top `.str1.1` contributors: `dashboard_http.c` 11,273 B,
`mesh_parent.o` 11,108 B, `main.c` 9,671 B. Reconciliation: 239,415 B merged
vs 309,165 B raw input, a 22.6% coalescing gap from string deduplication.
The script reproduces §1's 77,824 B reclaimable figure exactly.

**Owner decisions locked in for §5 (2026-09-02):**
1. §5.2 — the 1 MiB `logs` retention cap is excessive; target 256 kB. Owner's
   reasoning: logging that much means logging too often, so the *rate* is
   also in question, not just the cap.
2. §5.1 — `legacy_app`'s 1500 kB goes to relocating `pico_img` (896 kB), the
   only one of the three candidate uses that improves contiguity rather than
   just occupancy.

Companion doc: `DRAM_PSRAM_PLAN.md` covers internal SRAM. The two are
independent — neither blocks the other, and neither should be justified by the
other's numbers.

---

## 1. There is no flash emergency, but there is less slack than it looks

Nothing here is urgent. The purpose of this doc is that the obvious summary
("38.8% free") is measured against the wrong denominator, and acting on it
later without this context would produce a bad decision.

Two separate scarcities, which must not be conflated:

**Slot-level — comfortable.**

```
app slot (ota_0 / ota_1 / factory)   3,145,728 B
KilnCtrl.bin (commit eb17ea5)        1,936,320 B
free                                 1,209,408 B   (38.4%)
```

**Superseded:** the 2026-09-01 figures once here (1,924,496 B / 1,221,232 B /
38.8%, dated but not named to a commit) are stale — both because they predated
`9ede138`'s partition-table change and because a dated-only number is exactly
what this section warns against. See §4.2 for the full baseline, methodology,
and per-archive table, all pinned to `eb17ea5`. **Any size baseline recorded in
this doc must name the commit it was taken against.** A byte count with only a
date attached cannot be checked later.

**Chip-level — nearly exhausted.**

```
16 MB part                          16,777,216 B
unallocated, contiguous tail            65,536 B   (0xFF0000..0x1000000)
unallocated, boxed-in gap               12,288 B   (0x1FD000..0x200000)
forced 64 K-alignment pad               57,344 B   (0x202000..0x210000, not reclaimable)
```

Usable unallocated space is therefore **77,824 B**, of which only the 65,536 B
tail is contiguous and freely usable. The 12,288 B gap sits between
`profiles_nvs` and `otadata` and is boxed in by two immovable partitions — at
exactly three 4 K sectors it is the bare minimum NVS needs to operate, and
nothing else. The 57,344 B pad is structurally required: `gen_esp32part.py`
forces app partitions onto 64 K boundaries, and `otadata` is only 8 K.

The image has room to grow. The *partition table* does not have room for
another partition. Any future feature needing flash storage — a `web` partition
for OTA-able UI assets, a larger `logs`, a data-recorder — has 64 kB to work
with and will require reclamation before it can be added.

---

## 2. Where the 16 MB currently goes (measured, from `partitions.csv`)

| region | size | note |
|---|---:|---|
| bootloader (32 K) + partition table (4 K) + `nvs` (24 K) + `phy_init` (4 K) | 64 K | live data in `nvs` |
| **`legacy_app`** | **1500 K** | **dead hole — see 5.1** |
| `wifi_nvs` + `kiln_nvs` + `profiles_nvs` | 472 K | live data, immovable |
| *gap* (0x1FD000..0x200000) | 12 K | unallocated, boxed in |
| `otadata` (8 K) + forced 64 K-alignment pad (56 K) | 64 K | pad not reclaimable |
| `ota_0` + `ota_1` + `factory` | **9216 K** | 3 × 3072 K |
| `pico_img` | 896 K | RP2040 image relay staging; relocated to 0x10000 by 9ede138 |
| `coredump` | 1024 K | sized empirically, see 5.3 |
| `logs` | 768 K | shrunk from 3072 K by 9ede138, see 5.2 |
| *unallocated tail* (0xDB0000..0x1000000) | 2368 K | freed by 9ede138 |
| **total** | **16,384 K** | = 16,777,216 B ✓ |

The ~3.06 MB of spare that this table's own comments describe was consumed when
`logs` was added.

An earlier draft of this table stated the first row as 60 K and omitted the
12 K gap entirely, so it summed 16 K short of the chip and understated usable
free space. Recomputed row-by-row above; the totals now reconcile exactly.

Three items dominate: **9 MB of app slots** (56% of the chip), **1.5 MB dead**
in `legacy_app`, and **3 MB of `logs`**.

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

**But that path has never executed on hardware.** `UPDATE_PROTOCOL.md` §7 records
it as host-test-verified only, because the JTAG flash path always writes
`factory` — so every bench boot takes the `BOOT_CONFIRM_SKIP_FACTORY` branch and
the `PENDING_VERIFY` → confirm sequence is untested code. A two-slot scheme is
only self-healing if that sequence works; dropping `factory` would remove the
safety net while standing on the unproven mechanism.

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

**Action item, not a flash item:** perform one real OTA into `ota_0` on hardware
and confirm the app marks itself valid. Until that runs, OTA-updating this board
carries more risk than the partition table implies.

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
relocated to 0x10000, `logs` 3072K→768K, ~3.71 MiB freed) and the board has
been flashed with it; `check_flash_partition_map.ps1`'s "Expected-map check"
(§4.3) confirms the checked-out `partitions.csv` matches that post-`9ede138`
shape.

No new check was needed for this section — §4.3's `check_flash_partition_map.ps1`
already runs in `tools/run_all_checks.ps1` and already validates the current
partition shape; this section is a doc-only baseline capture using existing
tooling (`attribute_str_pool.py`, `esp_idf_size`), per the task's instruction
to reuse rather than write new.

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

### 5.1 `legacy_app` — 1500 kB, highest yield — DECIDED: relocate `pico_img` (see top-of-doc note)

Flash that `factory` occupied before it moved to 0x810000 on 2026-08-21. It is
declared `data`/`undefined`, is never read or written by this firmware, and
exists purely to give the hole a name in the CSV. It is boxed in by `phy_init`
below (0xf000) and `wifi_nvs` above (0x187000), neither of which may move.

Constraints on reuse: the region is 0x10000..0x187000, which *is* 64 kB-aligned
at its start, but at 1500 kB it cannot host an app partition (those need 3 MB
here, per section 2). It is therefore usable only for **data**.

**These candidates are mutually exclusive — 1500 kB is one hole, not three.**
Pick one before proposing a table revision:

- a `web` SPIFFS partition, if OTA-able UI assets are ever wanted
  (`WEB_UI_RESPONSIVE_PLAN.md` §8 would be the consumer);
- relocating `pico_img` (896 kB) into it, which frees 896 kB of *contiguous
  high* flash — worth more than the 1500 kB itself, since the high region is
  where a future app-sized partition could go;
- absorbing a future data-recorder or expanded statistics partition.

The second option is the only one that improves contiguity rather than just
occupancy, and is the default recommendation absent a specific need for the
other two.

This is the cleanest 1.5 MB available and it carries no data-loss risk, because
nothing lives there.

### 5.2 `logs` — 3072 kB backing a 1 MiB cap — DECIDED: cap drops to 256 kB (see top-of-doc note)

`log_store.h` sets `LOG_STORE_MAX_TOTAL_BYTES` to 1 MiB. The partition is 3 MB,
i.e. 3× its own rotation ceiling.

Some headroom over the cap is correct — SPIFFS degrades at near-full, with
slower mounts and more GC, which is precisely why the cap sits below the
partition size. But 3× is more than that reasoning requires. Roughly 1.5 MB is
plausibly reclaimable while still leaving ~50% headroom over the cap.

**Do not shrink this without first deciding whether the 1 MiB cap is itself
right.** If the intent is longer retention, the partition is correctly sized and
the cap should rise instead. That is a product question, not a flash question,
and it should be answered before the partition is touched.

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
`DRAM_PSRAM_PLAN.md` section 1.

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
  `DRAM_PSRAM_PLAN.md` §6 restates.
- **`UPDATE_PROTOCOL.md` §3 and §7** own the OTA layout and its open items; §2.1
  above summarises but does not supersede them.

## 8. Suggested order

1. ~~Phase 0 (4.1–4.3)~~ — done. 4.1/4.3 landed at `699f5ab`; 4.2's baseline
   is recorded against `eb17ea5` (see 4.2).
2. ~~Decide the `logs` retention question (5.2)~~ — decided and landed
   (`0bbb21c`).
3. Single partition-table revision covering `legacy_app` (5.1, landed
   `9ede138`) and `logs` (5.2, landed `0bbb21c`) together, with section 7's
   checklist worked through in order — **this is the remaining hardware
   step**: reflash the partition table and bootloader, archive the four NVS
   partitions first, erase `otadata`, and do not touch `coredump` in the
   process.
4. Leave `coredump` (5.3) alone unless something forces the issue.
5. ~~Revisit 5.4 only if 4.1 justifies it~~ — 4.1 does not justify it;
   decided not pursued.
