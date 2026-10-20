# Flash wear review: field-debug writers (FD-02..FD-05), 2026-10-19

Owner request 2026-10-19: make sure the new field-debug features do not wear out flash.
Review only, against origin/dev (base of this commit). Prefix **FW**.
Summary: HIGH 0, MED 1, LOW 3, INFO 3.

Assumptions: ESP32-S3 NOR flash, about 100k erase cycles per 4 KiB sector. Service life 10 years.
Workload: 1 firing per day of 24 h, a boot loop of 1 boot per minute for a day, a trip storm of
1 trip per minute for a day.

## Partitions and wear-leveling

| Partition | Type | Size | Wear-leveling |
|---|---|---|---|
| `cfg` 0xDB0000 | LittleFS (joltwallet/littlefs ^1) | 0x250000 = 2,424,832 B = 592 blocks | Dynamic wear-leveling. Copy-on-write data blocks come from a lookahead allocator that walks the whole volume, so writes spread over every **free** block. Static data is not moved. `block_cycles` = CONFIG_LITTLEFS_BLOCK_CYCLES; not set in sdkconfig, so the Kconfig default of **512** applies: a metadata pair moves after 512 erases. |
| `kiln_nvs` 0x18D000 | NVS | 0x10000 = 16 pages | Log-structured. Entries (32 B each) append to the active page. A full page is GC'd: live entries are copied out, then the page is erased. Every page rotates, including pages that hold static zones/rules. |
| `logs` 0xCF0000 | SPIFFS | 0xC0000 = 768 KiB | SPIFFS dynamic page wear-leveling. Rotation-bounded at 8 x 32 KiB segments per kind. |

## Per-writer numbers

### FD-02 firing_trace (cfg LittleFS, trace/ft0..3.bin)

Confirmed: every flush calls `cfg_fs_write_atomic` (firing_trace_glue.c). It writes the **whole** file
(24 B header + n x 20 B) to a temp file and renames it. Flushes happen at n = 1, then every 10 samples
up to 1440, plus the end flush (CLOSED flag): 146 writes per 24 h firing.

- Bytes per firing: 44 + sum over k = 1..144 of (24 + 200k) + 28,824 = **2,120,324 B (about 2.1 MB)**.
  This is quadratic: about 73x the 28.8 KB the trace actually holds.
- Block erases: about 520 data blocks plus about 0.5 partial block per write, plus metadata commits
  (create, sync, rename: about 3 per flush). Estimate **about 600 block erases per firing**.
- Free blocks: worst-case cfg content is about 124 KB, plus 115 KB of traces, plus a 29 KB temp file,
  about 66 blocks in use. That leaves **about 526 free blocks**.
- 1 firing/day: 600 x 365 / 526 = **about 420 cycles per block per year, about 4,200 in 10 years (4 % of 100k)**.
  Even 3 firings/day for 10 years is about 12 %.
- Metadata: the trace directory's pair takes about 440 commits per firing. Compaction then erases it
  about 10 times per firing. With block_cycles = 512 the pair relocates about every 50 firings, so this
  is spread too.
- Not rate-limited by wall time, but bounded by the sample count: at most 146 writes per firing.
  An ESP boot loop during a firing adds at most one write per boot (the first-sample flush).

**Verdict: safe (4 % of endurance in 10 years). Wasteful (LOW): quadratic rewrite.**

### FD-03 boot_history (kiln_nvs, namespace kiln_cfg, 524 B blob)

One `hal_kv_set_blob` per boot (main_boot_early.c -> boot_history_record_boot). It also writes once
more per boot after the first wall-time sync (telemetry_time_sync.c, once per boot by
`s_wall_backfilled`). A 524 B blob costs about 19 NVS entries (header, 17 data entries, blob index),
so 38 entries per boot with the backfill.

- Normal use (about 5 boots/day): about 190 entries/day, 1.5 page fills/day. Negligible.
- Boot loop of 1 boot/min for 1 day: 1440 x 38 = 54,720 entries = 434 page fills over 16 pages,
  **about 27 erases per page per day**. With live-data copy amplification at 50 % occupancy, about 54.
- Boot loop left running (an unattended kiln, nobody notices): 10k to 20k erases per page per year.
  **That reaches 100k in 5 to 10 years of continuous looping.** All of kiln_nvs wears, and that
  partition also holds zones, rules, relay_cycles and run_state.
- No rate limit, and no coalescing of identical consecutive boots.

**Verdict: safe for realistic loops of hours to days. Unbounded in a persistent boot loop (MED).**

### FD-05 trip_history (kiln_nvs, 588 B blob)

One write per new trip (dedupe on pico_trip_seq + reason), flushed from the telemetry tick. The
RAM queue holds 4 pending trips, and overflow is dropped and counted. Pico trips latch and clearing
them needs an operator, so a real storm is short. A Pico reboot loop that re-reports trips is the
plausible worst case. About 21 entries per write.

- Storm of 1 trip/min for a day: 30,240 entries = 240 page fills, **about 15 erases per page**.
- The ceiling is the telemetry tick (about 5 to 10 s) x 4 queued trips. A sustained storm at the
  tick rate needs new Pico trip sequence numbers each time.

**Verdict: safe (LOW: no rate limit beyond the tick and the queue).**

### FD-04 TIME_SYNC / SYS_BOOT events (logs SPIFFS)

2 SYS_BOOT lines per boot (FIRING + AUTOTUNE kinds). 1 TIME_SYNC per SNTP sync (hourly), plus an
AUTOTUNE copy only when the offset steps. About 80 B per line.

- Hourly resync: about 2 KB/day. Telemetry writes about 136 KiB/hour while firing, so this is noise.
- Boot loop of 1/min for a day: about 4 lines x 80 B x 1440 = about 460 KB, comparable to 3.4 h of FIRE
  telemetry. Rotation is bounded, and wear spreads over 768 KiB.

**Verdict: safe (INFO).**

### Diag bundle

diag_bundle.c / diag_bundle_http.c only read stores and stream them with `httpd_resp_send_chunk`.
There is no cfg_fs, hal_kv or log_store write on that path.

**Verdict: read-only, no wear (INFO).**

## Findings

- **FW-01 MED**: boot_history writes 2 NVS blobs (about 1.2 KB) on every boot with no coalescing.
  A persistent boot loop wears all of kiln_nvs (zones/rules/run_state pages included) at 10k to 20k
  erases per page per year. Fix: if the new entry matches the newest entry's reset reason and fw hash,
  increment a `repeat` count in the newest entry. Persist that at most once per N boots (e.g. 1 in 16)
  or when the reason changes. Alternative: write only when the previous boot's uptime was at least
  60 s, or when the reason differs.
- **FW-02 LOW**: the wall-time backfill doubles the boot-loop cost. Fix: skip the backfill when the
  boot was coalesced (FW-01). Otherwise defer it until uptime reaches 60 s, so a short-lived boot
  never writes it.
- **FW-03 LOW**: firing_trace rewrites the whole file through a temp copy on every flush: 2.1 MB per
  24 h firing for 28.8 KB of data (quadratic). This is safe (4 % of endurance in 10 years) but is
  73x amplification. Fix: append periodic batches, each with its own CRC (a commit record), to the slot
  file. Keep the atomic rewrite only for the end flush, and read back with the rule "trust the last
  CRC-valid batch". LittleFS append still copies the last partial block, so expect about 0.6 MB per
  firing.
- **FW-04 LOW**: trip_history has no rate limit beyond the telemetry tick and the 4-deep queue. A Pico
  reboot loop that reports fresh trip sequence numbers could write every tick. Fix: coalesce repeated
  identical (reason, threshold) trips within 60 s into a count on the newest entry.
- **FW-05 INFO**: LittleFS block_cycles is the Kconfig default of 512 (not pinned in sdkconfig.defaults).
  Fine for wear. Pin it explicitly so a component update cannot change it silently.
- **FW-06 INFO**: FD-04 log events and the diag bundle are negligible / read-only (see above).
- **FW-07 INFO**: kiln_nvs live-data occupancy was not measured on the bench. The boot-loop figure
  assumes 50 % or less. If the partition is nearly full of static data, GC amplification rises and
  FW-01 gets worse. Measure with nvs_get_stats on the bench.
