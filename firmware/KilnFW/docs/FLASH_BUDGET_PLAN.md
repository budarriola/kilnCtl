# Flash Budget Plan — 16 MB chip, reclaiming image and partition space

Plan doc for the flash side of KilnFW: what the 16 MB part is currently spent
on, why only 64 kB of it is unallocated, and what can be reclaimed.

Conventions this doc follows, matching `PID_EXPANSION_PLAN.md`: **the code is
truth, not the checkboxes.** Nothing is marked done until a commit is named.
Every number below is measured and attributed, or explicitly labelled as an
estimate.

Status at time of writing (2026-09-01): **nothing in this plan is built.**
Planning document only.

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
KilnCtrl.bin (measured 2026-09-01)   1,924,496 B
free                                 1,221,232 B   (38.8%)
```

**Chip-level — nearly exhausted.**

```
16 MB part                          16,777,216 B
unallocated                             65,536 B   (0.4%)
```

The image has room to grow. The *partition table* does not have room for
another partition. Any future feature needing flash storage — a `web` partition
for OTA-able UI assets, a larger `logs`, a data-recorder — has 64 kB to work
with and will require reclamation before it can be added.

---

## 2. Where the 16 MB currently goes (measured, from `partitions.csv`)

| region | size | note |
|---|---:|---|
| bootloader, partition table, `nvs`, `phy_init` | 60 K | live data in `nvs` |
| **`legacy_app`** | **1500 K** | **dead hole — see 5.1** |
| `wifi_nvs` + `kiln_nvs` + `profiles_nvs` | 472 K | live data, immovable |
| `otadata` + 64 K-alignment pad | 64 K | |
| `ota_0` + `ota_1` + `factory` | **9216 K** | 3 × 3072 K |
| `pico_img` | 896 K | RP2040 image relay staging |
| `coredump` | 1024 K | sized empirically, see 5.3 |
| `logs` | 3072 K | added 2026-09-01, see 5.2 |
| **unallocated** (0xFF0000..0x1000000) | **64 K** | |

`0x1000000 − 0xFF0000 = 0x10000`. The ~3.06 MB of spare that this table's own
comments describe was consumed when `logs` was added.

Three items dominate: **9 MB of app slots** (56% of the chip), **1.5 MB dead**
in `legacy_app`, and **3 MB of `logs`**.

The 9 MB is the price of dual-OTA plus a factory recovery image, and all three
app partitions must be the same size — an OTA image has to fit either slot, and
`factory` must stay a meaningful recovery target after an update. ESP-IDF's
`check_sizes.py` takes `min()` across all app partitions, which is what forced
all three to 3 MB together on 2026-08-21. **This 9 MB is not a candidate for
reclamation** and should not be treated as one; it is the cost of the recovery
guarantees the partition table exists to provide.

---

## 3. Where the 1.92 MB image goes (measured, `esp_idf_size --archives`)

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

### 4.2 Record a size baseline in this doc

Capture `esp_idf_size --archives` output and `KilnCtrl.bin` size against a named
commit. Every later phase compares against it. Without a committed baseline,
"this saved 40 kB" is unverifiable a week later.

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

### 5.1 `legacy_app` — 1500 kB, highest yield

Flash that `factory` occupied before it moved to 0x810000 on 2026-08-21. It is
declared `data`/`undefined`, is never read or written by this firmware, and
exists purely to give the hole a name in the CSV. It is boxed in by `phy_init`
below (0xf000) and `wifi_nvs` above (0x187000), neither of which may move.

Constraints on reuse: the region is 0x10000..0x187000, which *is* 64 kB-aligned
at its start, but at 1500 kB it cannot host an app partition (those need 3 MB
here, per section 2). It is therefore usable only for **data**. Candidates, in
rough order of usefulness:

- a `web` SPIFFS partition, if OTA-able UI assets are ever wanted;
- relocating `pico_img` (896 kB) into it, freeing 896 kB of contiguous
  high flash;
- absorbing a future data-recorder or expanded statistics partition.

This is the cleanest 1.5 MB available and it carries no data-loss risk, because
nothing lives there.

### 5.2 `logs` — 3072 kB backing a 1 MiB cap

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

### 5.3 `coredump` — 1024 kB, examine but probably leave alone

Reachable in principle, but this partition was sized *empirically* and painfully:
64 kB failed silently, 512 kB was insufficient once `CONFIG_ESP_COREDUMP_CAPTURE_DRAM`
was enabled, and 1024 kB is the value that works. `CONFIG_ESP_COREDUMP_MAX_TASKS_NUM`
is 64 and this firmware runs a few dozen tasks.

Its failure mode is the worst kind: silent, and manifesting only at the moment
the diagnostic was needed. **Recommendation: leave it.** If it is ever shrunk,
the change must be validated by forcing a real panic under full task load and
confirming the dump decodes — not by reasoning about sizes.

### 5.4 Image-size reclamation

Lower priority than the above, because the app slot has 1.22 MB free and
shrinking the image reclaims nothing at chip level — it only widens
already-adequate slot headroom. Worth doing only if 4.1 shows the string pool
is dominated by log strings that are genuinely dead weight.

If pursued, the levers are `CONFIG_LOG_DEFAULT_LEVEL` / `CONFIG_LOG_MASTER_LEVEL`
and pruning verbose logging in the noisiest modules. **Caution:** this codebase's
log strings are a primary debugging asset, and several documented incidents in
`docs/` were diagnosed from exactly these messages. Trading them for flash that
is not currently scarce is a bad trade. Attribute first (4.1), then decide.

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

## 8. Suggested order

1. Phase 0 (4.1–4.3) — cheap, and 4.1 determines whether 5.4 exists at all.
2. Decide the `logs` retention question (5.2) — product question, gates the
   second-largest reclamation.
3. Single partition-table revision covering `legacy_app` (5.1) and `logs` (5.2)
   together, with section 7's checklist worked through in order.
4. Leave `coredump` (5.3) alone unless something forces the issue.
5. Revisit 5.4 only if 4.1 justifies it.
