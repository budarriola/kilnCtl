# LittleFS assessment — 2026-09-06

**Superseded, 2026-09-07 — see below the original analysis.** This
document's "not adopted" conclusion was the right answer to the question it
was asked (would LittleFS help *log retention*), but the owner came back
with a different question the next day — wear leveling — and after that was
answered too (`docs/audits/flash_endurance_review_2026-09-07.md`: no
endurance problem exists, by 100x-1000x), the owner directed the migration
to proceed anyway, for a reason neither document argues against: structured,
inspectable, diffable, backup/restore-able user data beats opaque
`_Static_assert`-pinned C structs in NVS blobs, independent of wear. See
`docs/FILESYSTEM_USER_DATA_PLAN.md` for the design and
`docs/CONFIG_FILESYSTEM.md` for what actually shipped. **Everything below
this line is the original analysis and its numbers are still correct** —
NVS wear/fragmentation genuinely was never the problem; it just stopped
being the deciding question.

Owner question: should flash writes go through LittleFS for wear leveling,
possibly relaxing the strict partition layout? Is that safe, does it
fragment? Answer below is from a read-only survey of every persistent write
path on both processors as of `05087f0`, plus upstream `DESIGN.md`.

## What writes to flash today

**ESP32-S3 (KilnFW)**, all through `hal_kv` → NVS, none per tick:

| Store | Partition | Shape | Cadence |
|---|---|---|---|
| zones, rules, `kiln_cfg_store`, `touch_cal`, `unit_pref`, `display_power_cfg`, `boot_guard` | `kiln_nvs` 64 K | small blobs / scalars | config edit, boot |
| `relay_cycles` | `kiln_nvs` | one struct | debounced (`relay_cycles.c:38-45`) |
| `ota_record` | `kiln_nvs` | fixed 216 B | per OTA |
| profiles | `profiles_nvs` 384 K | one blob per profile | profile edit |
| Wi-Fi credentials | `wifi_nvs` 24 K | scalars | provisioning |
| event log (`log_store.c`) | `logs` 768 K, **SPIFFS** | 32 B records, events only | per event, never per sample |
| crash reports | `coredump` 1 M | ESP-IDF core dump | per panic |

NVS is already a log-structured, page-rotating store: every NVS-backed write
already gets wear leveling and power-loss atomicity. `hal_kv.h`'s
PSRAM-stack rule (flash ops from a PSRAM-stacked task panic) is about the
SPI-flash cache-disable window and applies identically to any filesystem.

**RP2040 (SaftyFW)**: `config_store` is one CRC'd, sequence-numbered 512 B
record round-robinned across 8 slots of a **single 4 K sector**
(`config_store.h:31,143-145`), written only at commissioning and refused
while ARMED (`config_store.h:814-825`). The OTA slot layout is whole-image
writes gated by `flash_safe_execute()`. Nothing is file-shaped.

## LittleFS facts that matter here

- Dynamic wear leveling only (linear allocation, randomised start per
  mount); same "best effort" class NVS provides. `block_cycles` adds
  bad-block relocation.
- Power-loss safe via two-block metadata pairs and copy-on-write.
- Small-file cost: a file above 1/4 block costs whole blocks (a 4 B file
  ≈ 12 KiB unless inlined); metadata-pair updates cost 2x at 50 % fill, 4x
  at 75 %. "Fragmentation" is not a pathology — CTZ skip-lists never need
  defragmenting — but random writes into large files are not cheap.
- RAM is bounded and small (lookahead buffer + per-file pointers); no
  saving against SPIFFS, no loss either. Not the source of this board's
  internal-DRAM pressure.
- On ESP-IDF it is the `joltwallet/esp_littlefs` managed component — a
  network fetch on every clean build; SPIFFS is built in. This is why
  `logs` chose SPIFFS on 2026-09-01 (see the `partitions.csv` comment block),
  which also documents the swap path: subtype change plus
  `esp_vfs_spiffs_register()` → `esp_vfs_littlefs_register()`; `log_store.c`
  is stdio-only and filesystem-agnostic.

## What the partitioning protects

NVS corruption recovery is partition-wide. `wifi_nvs` was split from
`kiln_nvs`/`profiles_nvs` after a corrupt profile blob forced an erase that
took the Wi-Fi credentials with it — the board went off-network exactly
when it needed remote repair. `factory` stays separate from `ota_0/1` for
cable-free fallback; `pico_img` is a raw range by design. Merging
partitions under one filesystem re-opens the credential-loss coupling that
the layout was built to fix. This holds regardless of filesystem.

## Decision (as of 2026-09-06 — see supersession note at top)

**No change.** Not adopted as a submodule.

- NVS-backed data: no wear, fragmentation or power-loss problem observed or
  plausible at these write rates; LittleFS adds a dependency and no data.
- SaftyFW: a single sector holding one record is below what LittleFS is
  designed to manage, and `hal_kv.h` records why forcing `config_store`
  through a generic store would weaken the ARMED interlock.
- Partition layout: keep strict.

**Revisit trigger:** if `logs` retention is raised well past the current
256 KiB per kind (covers ~1.9 h of a firing that runs 8-12 h — an open
sizing concern), LittleFS's better leveling and atomicity become worth the
managed-component dependency. That migration is one partition subtype and
one register call; nothing else in the tree would move.
