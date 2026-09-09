# Flash endurance review — 2026-09-07

**Status update, 2026-09-08: R2 implemented AND flashed to the Pico**
(`b7af9ebe`, bench-verified: commissioning config — `abs_max_temp_c`, S8 rate,
`tc_source`, CT install/topology, CT cal — read back byte-for-byte preserved
across the sector-layout migration, CRC unchanged at 25042). **A separate,
later fix on top of it — the config_store RAM cache seqlock, `b202fe56` +
`5671ee03`, 2026-09-09 — has NOT yet been flashed.** Sector B
(`SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B`, immediately after sector A inside the
already-reserved 64K `BOOTLOADER_CONFIG_FLASH_SIZE` region) is now live in
source: `config_store_flash.c` writes always target the sector that is NOT
current, erasing-then-programming it and only then updating the in-RAM
(sector, slot) cache; `config_store_find_latest_multi_ex()` (config_store.c)
is the sole arbiter, scanning both sectors' 16 slots every boot and keeping
the single highest-seq, CRC-valid record -- there is no separate "which
sector is active" pointer anywhere to tear. Sector A keeps its original
offset and 8-slot format unchanged, so a board already running the old
single-sector firmware needs no migration step: this firmware's first boot
against that board's existing image finds sector A's real, already-
committed record (TC type, `abs_max_temp_c`, CT cal all intact) exactly as
before, with sector B simply still blank. 169/169 config_store host tests
pass (up from 60), including power-cut injection at mid-erase, mid-program,
and between-erase-and-program of the switch target (each leaves the OTHER,
untouched sector's record valid), a fixture built from the CURRENT on-flash
single-sector format proving migration is seamless, a corrupt-sector
fallback, and a negative test that reintroduces the old erase-in-place
behaviour and shows `test_power_loss_between_erase_and_program_of_target_
leaves_old_sector_valid` fail with zero valid copies at that instant (then
reverted by hand, `git diff` empty). Wear is now spread across both
sectors as the free side effect this review named. **This landed on the
bench Pico via `b7af9ebe`** (built from a clean detached worktree, flashed
via `debug_program(peer="pico")`). The RAM-cache seqlock fix layered on top
of it the next day (`b202fe56`/`5671ee03`, see `docs/CONFIG_FILESYSTEM.md`)
has not yet been flashed.

Owner directive: *"we must have wear leveling and this is a standard way to
get it."* The 2026-09-06 `LITTLEFS_ASSESSMENT.md` declined a filesystem, but
it argued from **log retention size**. That is a different question from
**flash endurance**. This review answers the endurance question directly,
with write rates taken from the code and erase-cycle arithmetic against the
parts actually fitted. It does not re-run the retention argument.

**Headline:** there is no endurance problem on either processor — the worst
sector on this board wears out in ~1,700 years at realistic use and ~170
years at absurd use. LittleFS on `logs` would not touch the highest-rate
writer. But the RP2040 config store has a real, non-endurance single-point
failure that a wear-levelling conversation is the right moment to fix.

---

## 1. Every flash writer, with its actual rate

### ESP32-S3 (KilnFW) — partition table `firmware/KilnFW/partitions.csv`

| Partition | Off / Size | Sectors | Type |
|---|---|---|---|
| `nvs` | 0x9000 / 0x6000 | 6 | NVS (legacy/default; IDF Wi-Fi PHY + net80211) |
| `phy_init` | 0xF000 / 0x1000 | 1 | PHY blob, write-once at flash |
| `pico_img` | 0x10000 / 0xE0000 | 224 | raw, per Pico OTA |
| `wifi_nvs` | 0x187000 / 0x6000 | 6 | NVS |
| `kiln_nvs` | 0x18D000 / 0x10000 | **16** | NVS — **all hot keys live here** |
| `profiles_nvs` | 0x19D000 / 0x60000 | 96 | NVS |
| `otadata` | 0x200000 / 0x2000 | 2 | IDF OTA select, per OTA |
| `ota_0/1/factory` | 0x210000+ | 3x768 | app images, per flash/OTA |
| `coredump` | 0xBF0000 / 0x100000 | 256 | per panic |
| `logs` | 0xCF0000 / 0xC0000 | 192 | **SPIFFS** |

Writers, by module, with the cadence constant found in the source:

| Writer | Target | Cadence (source) | Writes / 12 h firing |
|---|---|---|---|
| `run_state.c` | `kiln_nvs`, 104 B blob | `RUN_STATE_REFRESH_INTERVAL_S 300` (`run_state.h:126`) + transitions | **~144 periodic + <30 transitions ~= 175** |
| `relay_cycles.c` | `kiln_nvs`, ~52 B blob | `RELAY_CYCLES_PERSIST_INTERVAL_S 600` (`relay_cycles.h:38`), **and only when dirty** | 72 |
| `profile_executor_firing_stats.c` | `kiln_nvs` | once at run end (`firing_stats_persist`) | 1 |
| `adaptive_tune.c` | `kiln_nvs`, Ki baseline | once at run end; baseline "once persisted, never changes again" (`adaptive_tune.c:489`) | <=1 |
| `zones_config_store.c`, `kiln_cfg_store.c`, `safety_cfg_store.c`, `unit_pref.c`, `display_power_cfg.c`, `touch_cal_store.c`, `ramp_assist_cfg.c`, `watchdog_cfg.c` | `kiln_nvs` | per operator config edit only | 0 |
| `boot_guard.c` | `kiln_nvs` | ~2 per boot (arm + `boot_guard_mark_healthy`) | 0 (per boot, not per firing) |
| `crash_report.c` | `kiln_nvs` | per panic + per acknowledgement | 0 |
| `ota_record.c` | `kiln_nvs`, 216 B | per OTA | 0 |
| `profiles_http.c` | `profiles_nvs` | per profile edit | 0 |
| `wifi_prov_nvs.c` | `wifi_nvs` | per provisioning | 0 |
| `time_sync.c` | TZ string | per TZ change | 0 |
| `event_log.c` / `log_store.c` | `logs` (SPIFFS) | **transitions only** — 32 B records | tens |
| `telemetry_log.c` | **nothing** | 5 s / 10 s tick goes to **UART only**; flash persistence was removed 2026-09-02 (`telemetry_log.c:155-165`) | **0** |
| ESP-IDF Wi-Fi | `nvs` (default) | PHY-cal / `nvs.net80211` on association changes | a handful per boot |

Note the correction to the prior assessment's table: it listed the `logs`
event log as "per event", which is right, but the file that *used* to write
per-tick (`telemetry_log.c`) is now UART-only — the one candidate for a
high-rate flash writer on this board was already removed. **Nothing on the
ESP writes flash per control tick.**

**Highest-rate writer: `run_state.c`, one 104 B blob every 300 s while a
firing is RUNNING** — ~175 writes per 12 h firing, ~14.6/hour. Everything
else is at least 2.4x slower or event-driven.

### RP2040 (SaftyFW)

| Writer | Target | Cadence |
|---|---|---|
| `config_store_flash.c` | **one 4 KiB sector** at `0x1B1000` (`SAFTYFW_CONFIG_STORE_FLASH_OFFSET`, `bootloader/flash_layout.h:90-91`), 8 x 512 B slots | per commissioning param write, via link cmd only; **refused while ARMED** (`config_store_decide_write`) |
| bootloader metadata | one 4 KiB sector at `0x10000` | per Pico OTA |
| OTA slots A/B | 832 K each | per Pico OTA |

`config_store` has **no periodic writer at all** — `config_store_write()` is
reached only from `link_task.c:1465` and `:1530`, both command handlers. CT
cal (`i_normal_a[0..2]`, `config_params.c:119-121`) is a settable parameter,
not a learned-and-rewritten one; nothing in the tree writes it on a schedule.

---

## 2. What is already wear-levelled

| Partition | Wear levelling? | Mechanism |
|---|---|---|
| `kiln_nvs`, `nvs`, `wifi_nvs`, `profiles_nvs` | **Yes** | ESP-IDF NVS is log-structured: entries are append-only 32 B slots in 4 KiB pages, pages rotate through the whole partition, and compaction erases the oldest page. Updating a key never rewrites in place. |
| `logs` (SPIFFS) | **Yes** | SPIFFS static wear levelling across its 192 sectors. |
| `otadata` | **Yes (2-way)** | IDF alternates the two sectors. |
| `ota_0/1/factory`, `coredump`, `pico_img` | No — and correctly so | whole-image / whole-region writes at human cadence. |
| **RP2040 `config_store`** | **No.** | 8-slot round-robin *within one fixed sector*. That is an 8x write-amplification reduction, not wear levelling: the same physical sector absorbs 100 % of erases forever, and it cannot move. |
| RP2040 bootloader metadata | No | single fixed 4 KiB sector, per-OTA only. |

So the owner's instinct is structurally right about *where* the gap is: the
RP2040 side is the only place with a fixed, un-levelled, repeatedly-erased
sector. The question is whether its rate makes that matter.

---

## 3. Erase cycles per sector per year — the number that decides this

Parts and ratings:

- **ESP32-S3-N16R8 module**, 16 MB in-package SPI NOR. Rated **100,000
  P/E cycles/sector**, 20-year retention (universal for the qualified
  W25Q128/GD25Q128-class dies used in these modules).
- **Raspberry Pi Pico**, stock board, 2 MB QSPI NOR
  (`firmware/SaftyFW/docs/HARDWARE.md:295` — `PICO_BOARD=pico`, so
  `PICO_FLASH_SIZE_BYTES = 2 MiB`, a fixed board fact, W25Q16JV-class).
  Rated **100,000 P/E cycles/sector minimum**, 20-year retention.

### `kiln_nvs` — the worst ESP sector

NVS geometry: page = 1 sector = 4096 B; 64 B of header/state bitmap leaves
**126 entries x 32 B**. A variable-length blob costs 1 index entry +
`ceil(len/32)` data entries.

- `run_state` 104 B -> 1 + 4 = **5 entries/write**
- `relay_cycles` ~52 B -> 1 + 2 = **3 entries/write**

Per 12 h firing into `kiln_nvs`:

```
run_state      175 writes x 5 = 875 entries
relay_cycles    72 writes x 3 = 216 entries
firing_stats     1 write  x 5 =   5 entries
adaptive_tune    1 write  x 3 =   3 entries
                              ---------------
                                1,099 entries / firing
```

`kiln_nvs` = 16 pages; NVS reserves at least one page free for compaction ->
**15 usable x 126 = 1,890 entries per full sweep of the partition**. One
sweep costs each sector exactly one erase.

```
sweeps per firing = 1,099 / 1,890 = 0.58 erases per sector per firing
```

| Use rate | Erases/sector/year | **Years to 100,000** |
|---|---|---|
| 100 firings/yr (heavy hobby, ~2/week) | 58 | **1,720 years** |
| 365 firings/yr (one every day) | 212 | **472 years** |
| 1,000 firings/yr (2.7/day, implausible) | 580 | **172 years** |

Pathological cross-check, discarding NVS levelling entirely and pretending
every write hit one sector (~25 writes fill a page): 249 writes/firing x 100
firings = 24,900/yr / 25 = 996 erases/yr -> **100 years**. Even the
no-wear-levelling bound clears the product's life by an order of magnitude.

### RP2040 `config_store` — the un-levelled sector

Erase fires on every 8th write (`config_store_next_write_needs_erase`,
`config_store.c:953-959`).

- Realistic: ~20 param writes per commissioning session x ~4 sessions/year
  = 80 writes/yr -> **10 erases/yr -> 10,000 years**.
- Hypothetical future feature writing the record once per firing, 100
  firings/yr -> 12.5 erases/yr -> **8,000 years**.
- **Break-even:** wearing this sector out inside 10 years requires 10,000
  erases/yr = 80,000 writes/yr = **219 config writes every single day.**
  Nothing in the tree comes within four orders of magnitude, and the ARMED
  refusal structurally forbids writing during the only long-running activity.

**Answer to the question as posed: the worst sector on this board wears out
in ~1,700 years (ESP `kiln_nvs`, at 100 firings/yr) / ~10,000 years (RP2040
config sector). There is no endurance problem, on either processor, by a
margin of roughly 100x-1000x.**

---

## 4. Does a filesystem solve the owner's concern?

**No — and it is worth being precise about why, because the concern itself is
reasonable.**

A filesystem provides wear levelling **inside its own partition only**. The
proposed change (`FILESYSTEM_PLAN.md`) swaps SPIFFS -> LittleFS on `logs`.
Consequences for the writers actually identified above:

- `run_state`, `relay_cycles`, `firing_stats`, zones/rules/profiles/Wi-Fi —
  all NVS, in `kiln_nvs`/`profiles_nvs`/`wifi_nvs`. **Unaffected.** These
  are the highest-rate writers on the board.
- `logs` — already wear-levelled by SPIFFS, and now carries only transition
  events since `telemetry_log.c`'s per-tick flash write was removed.
  LittleFS would swap one wear-levelling scheme for a comparable one
  (LittleFS is *dynamic* levelling; SPIFFS is *static* — arguably a step
  sideways, not forwards, for levelling specifically).
- RP2040 `config_store` — a different processor, a different flash chip.
  **Completely unaffected.** No ESP-side filesystem can reach it.

So: the change addresses **0 of the 3** places wear could plausibly
concentrate. If the goal is wear levelling, LittleFS-on-`logs` is not a way
to get it here.

What *would* address wear levelling, if it were needed (it is not):

1. Round-robin the RP2040 config record across multiple sectors — the only
   genuinely un-levelled repeatedly-erased sector in the system.
2. Reduce write rate: raise `RUN_STATE_REFRESH_INTERVAL_S` from 300 s. Going
   to 600 s halves the dominant writer at the cost of doubling post-power-cut
   uncertainty from 5 to 10 minutes. **Not recommended** — the margin is
   1,700 years; spending operator information to buy 3,400 years is a bad
   trade, and `run_state.c:35-66` already argues this correctly.
3. Enlarge `kiln_nvs` (2.31 MiB of unmapped flash tail exists above `logs`).
   Linear improvement on a benefit that is not needed.

---

## 5. Recommendation, ranked

**R1 — Do not adopt LittleFS for wear-levelling reasons. (cost: 0, risk: 0)**
The decision stands, but on *this* basis, which the prior assessment did not
establish: measured write rates give 100x-1000x endurance margin, and the
proposed change does not reach any of the identified writers. Keep
`FILESYSTEM_PLAN.md`'s retention-driven trigger as the only reason to
revisit; that trigger has still not fired.

**R2 — Fix the RP2040 config store's real defect, which is *atomicity*, not
wear. (cost: ~half a day + host tests; risk: low, bounded to one module)**
This is the finding worth the review. When `config_store_write()` reaches the
8th slot it calls `hal_flash_erase()` over the **entire 4 KiB sector**, then
programs slot 0 (`config_store_flash.c:339-348`). Between those two calls the
board holds **zero valid copies of the safety configuration** — TC type,
`abs_max_temp_c`, `max_rate_c_per_min`, CT cal. A power cut or watchdog
inside that window is not a lost sample; it is a safety processor that boots
uncommissioned. The 8-slot round-robin defends the other 7 writes and
defends nothing on the 8th, and every 8th write is a scheduled encounter
with this window, not a rare one.

The fix is not a filesystem and not wear levelling: use **two sectors, A/B**.
Write the new record into the sector that is *not* current; erase the old one
only after the new record's CRC verifies. There is always a valid copy.
`BOOTLOADER_CONFIG_FLASH_SIZE` is already `0x10000` = 64 KiB while
`SAFTYFW_CONFIG_STORE_FLASH_SIZE` uses only `0x1000` — **15 sectors are
already reserved and unused**, so this needs no flash-layout change, no
bootloader change, and no coordination with the ESP. As a free side effect it
also halves the per-sector erase rate and gives the store the wear levelling
the owner asked for, at the one place where it costs nothing.

Guard rails for whoever implements it: `config_store.h`'s ARMED-write refusal
must survive unchanged; add a host test that injects failure *between* erase
and program and asserts the old record still loads (a negative test that
breaks the production function, not a test-local copy); and check
`firmware/SaftyFW/tools/check_flash_layout_sync.cmake` — the sector geometry
is mirrored there, and updating one side and not the other is exactly the
"reset one side of a pair" shape CLAUDE.md warns about.

**R3 — Record the endurance numbers where the next reviewer finds them.
(cost: minutes, risk: 0)** `run_state.c:47-58` already carries this
arithmetic and is correct; `relay_cycles.h:20` gestures at it. Nothing states
the *whole-partition* figure. Point `LITTLEFS_ASSESSMENT.md` and
`FILESYSTEM_PLAN.md` at this file so the endurance question is not
re-litigated from the retention document a third time.

**R4 — Explicitly rejected: raising `RUN_STATE_REFRESH_INTERVAL_S`, enlarging
`kiln_nvs`, or moving any NVS key to another mechanism.** All three trade
real operator value for endurance margin that is already ~1,700 years.

### One monitoring trigger

Re-open this file if any future feature writes NVS or the RP2040 config
record **on the control tick, per sample, or per PID update**. That is the
only shape that could move these numbers; it is the shape `telemetry_log.c`
already had and shed; and it would have to appear before anything here
changes. A write rate below one-per-minute is not worth measuring again.
