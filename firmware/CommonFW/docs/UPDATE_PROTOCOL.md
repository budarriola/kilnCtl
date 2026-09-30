# Firmware Update Protocol — both processors, one password

> **Status:** planning, mostly nothing built — **exceptions:** section 3's ESP
> partition-table + rollback foundation landed 2026-08-17 (host-build-verified,
> not yet flashed to physical hardware), and section 4's ESP-side sender half
> (KilnFW: `App/drivers/ota_pico_relay.{h,c}`, `safety_link.c`'s
> `UPDATE_STATUS` handling, `POST /api/ota/pico`) landed the same day —
> host-build-verified only, RP2040 receive side (SaftyFW) was already frozen
> before this pass and untouched by it. Sections 2, 5, 6 are still planning
> only. · **Last reviewed:** 2026-08-19
>
> **2026-08-19: the transfer path structurally exists end-to-end and does not
> touch relay/heating state**, confirmed by code inspection (`git show
> a7a5653`): `SaftyFW`'s `link_task.c` dispatches inbound
> `UPDATE_BEGIN`/`UPDATE_DATA`/`UPDATE_END`/`UPDATE_ABORT` to `update_task.c`
> (`update_task_gather_preconditions()` only *reads*
> `safety_core_get_output_status()`/`thermo_task_get_snapshot()` to gate
> `UPDATE_BEGIN`, never writes a relay/GPIO), and `KilnFW`'s
> `ota_pico_relay.c` has no references to `relay_authority`, `kiln_io_owner`,
> or any relay/GPIO symbol. Section 1's invariant — the Pico enforces its own
> relay-open/no-trip-pending/temperature-ceiling preconditions rather than
> trusting the ESP — holds by inspection. **Not yet exercised on physical
> hardware**, and the interlock table, authentication, and version-mismatch
> checks in sections 1–2 remain unbuilt as described below.
>
> **Section 4 deviations from this doc's prose, resolved in code (code wins,
> fix the doc — see this note):**
> 1. **`UPDATE_END`'s "4 B: image CRC32 repeated"** is ambiguous prose. Both
>    firmwares now agree on one reading: the payload is the same 4-byte
>    crc32 already carried in `UPDATE_BEGIN`, sent again once (not the value
>    written twice) — SaftyFW's `update_task_process_end()` compares it
>    against `s_header.crc32` and logs (does not act on) a mismatch; the
>    read-back-from-flash CRC is what actually gates acceptance either way.
> 2. **The ESP's retransmission-round accounting is a simplification, not a
>    mirror**, of SaftyFW's `update_receiver.h` round-counting algorithm:
>    `ota_pico_relay.c` retries whatever chunks the Pico's gap reports name,
>    for up to 10 rounds, then always attempts `UPDATE_END` regardless and
>    lets the Pico's own final CRC verification be the arbiter — consistent
>    with this section's own "acknowledging each frame is not what makes the
>    transfer correct — the final verify is," but not an attempt to
>    reconstruct the Pico's exact per-pass gap-cursor bookkeeping on the ESP
>    side.
> 3. **`UPDATE_BEGIN`'s `version` field placeholder**: the ESP has no real
>    build-identity string available for a raw (non-multipart) browser
>    upload — no filename, no embedded-version parser for a raw `.bin`. It
>    sends a fixed 16-byte placeholder (`"esp-relay-upload"`) today; a future
>    pass may replace this with something more meaningful (a caller-supplied
>    header, or a filename if a multipart form is adopted).
> 4. **`POST /api/ota/pico` responds `202 Accepted` immediately** after
>    staging finishes, rather than holding the HTTP connection open for the
>    ~35 s+ relay — a design choice this pass made, not something this
>    section specifies the shape of. `GET /api/ota/pico/status` (new) is the
>    poll-back endpoint. See `firmware/KilnFW/App/drivers/http/ota_http.h`'s
>    header comment on `ota_pico_do_stage()`/`ota_pico_post_handler()`.
> **Keep this file current.** This is a contract between two firmwares and a web
> UI. If any one of the three changes shape, edit this file in the same change —
> a stale update protocol is the kind of thing that is only discovered while
> holding a half-written flash image.

Two update paths, deliberately described together because they share an
authorisation model, a set of interlocks, and a web page:

- **ESP32-S3 (`KilnFW`)** — over Wi-Fi, via a POST to its own web server.
  ESP-IDF has native support; the work is partitioning, authentication and
  interlocks.
- **RP2040 (`SaftyFW`)** — over the isolated UART, relayed by the ESP.
  There is no vendor support for this at all: the RP2040 mask ROM boots from
  USB or flash and **has no UART bootloader**, so this needs a bootloader
  written for it. See
  [`../../SaftyFW/docs/BOOTLOADER.md`](../../SaftyFW/docs/BOOTLOADER.md).

Cross-references [`LINK_PROTOCOL.md`](LINK_PROTOCOL.md) for the framing this
rides on, [`../../SaftyFW/docs/SAFETY_MODEL.md`](../../SaftyFW/docs/SAFETY_MODEL.md)
for the interlocks, and [`../../../tools/PcTools/TODO.md`](../../../tools/PcTools/TODO.md)
for the MCP surface.

---

## 1. The governing constraint

> **Neither processor may be updated while the kiln can heat.**

Everything else here is detail. An update takes tens of seconds, during which
the target processor is not doing its job — the safety processor in particular
is not watching anything while its flash is being rewritten. Both update paths
must therefore be refused unless all of the following hold:

| Precondition | Checked by | Why |
|---|---|---|
| No profile running, no autotune running | ESP | An update mid-firing abandons a hot kiln |
| No heater commanded on, all SSRs off | ESP | Same, without a profile |
| `run_state` shows the kiln idle | ESP | Catches a firing that survived a reboot |
| Safety relay K4 open, no trip pending | Pico | The safety processor's own veto |
| Measured temperature below a configured ceiling | Both | A cooling kiln is still a hot kiln. Default 100 °C |
| Link healthy and versions exchanged | ESP | Do not start a transfer over a link that is already marginal |

The Pico enforces the last three **itself**. It does not take the ESP's word for
it, for the same reason the whole safety processor exists: the ESP is the thing
that might be wrong.

**The ESP must present a refusal with the specific unmet precondition**, not a
generic failure. "Cannot update: zone 2 is at 340 °C" is actionable; "update
failed" invites a retry loop.

### The Pico update deliberately trips the liveness rule

While the Pico is being updated it stops sending telemetry, so
`SAFETY_FAULT_SRC_SAFETY_LINK` asserts at 1.5 s and all relay-on is blocked.
That is correct and must not be special-cased away. What *does* need
special-casing is the operator-facing text: the GUI must say "safety processor
updating" rather than "safety processor not responding", while the interlock
underneath stays exactly as strict. **Do not add an update-mode bypass to
`relay_authority_on_blocked()`.** Suppress the alarm text, never the block.

---

## 2. Authentication

**Retired 2026-09-29 for the main app** (owner decision "Retire; open when
login off", `docs/WEB_AUTH_PLAN.md` item 2b): the AP-password challenge/HMAC
scheme described in this section was removed outright from the nine
main-app routes named below. `ROUTE_TIER_ADMIN` is now their only gate,
unconditionally — including with web auth off, where they are exactly as
open as every other ADMIN route. `GET /api/ota/challenge`,
`ota_http_verify_request()`, and `ota_http_authenticate_request()` were all
deleted outright from `ota_http.c` (`f0643c98`). PcTools (`ota_http_client.py`)
no longer sends or computes a MAC for these routes — it authenticates purely
via the admin web session (`http_auth.urlopen()`), same as any other admin
tool. `kilnctrl.recovery_ota_auth_client` is the still-live counterpart for
`firmware/KilnFW_recovery/`'s own, untouched copy of this scheme (see that
module's own header comment). This section is kept
below for history; it describes the **separate, standalone recovery
firmware image** (`firmware/KilnFW_recovery/`), which still implements this
scheme unchanged on its own routes.

**RECOVERY-IMAGE ONLY, still live:** `tools/PcTools/src/kilnctrl/
recovery_ota_auth_client.py` is a from-scratch, narrowly-scoped signer kept
specifically for talking to a board that has fallen back to
`firmware/KilnFW_recovery/` (unreachable via the main app at all) — its
`derive_mac()`/`get_challenge()`/`signed_post()` implement exactly this
section's scheme against that image's still-unchanged `/api/ota/challenge`,
`/api/ota/esp`, `/api/ota/esp/boot_guard_reset` and `/api/sw_reset` routes
only, reading the AP password from a caller-supplied string (the MCP layer
reads `KILNCTL_AP_PASSWORD`) and never logging it. It must never be pointed
at the main app — since 2026-09-29 the main app has no `/api/ota/challenge`
route to answer it at all, so a caller confusing the two gets an immediate,
loud 404 rather than a silent wrong-scheme success.

The user's requirement: **the same password as the ESP's local access point**,
for both update paths. `wifi_prov_get_ap_password()` already returns it.

### Do not send it in a form POST

The web UI is served over plain HTTP on the LAN. A password in a POST body is
readable by anything on the network and lands in proxy logs and browser history.
Since this password is also the WPA2 PSK for the fallback AP, leaking it hands
over the recovery path as well.

**Use challenge–response so the password never crosses the wire:**

1. `GET /api/ota/challenge` → a 16-byte random nonce, single-use, 30 s expiry,
   bound to the client's connection.
2. Client computes `HMAC-SHA256(key, nonce || context)` where
   `key = HMAC-SHA256(ap_password, "kilnctl-ota-v1")` and `context` is the
   literal `"esp"` or `"pico"`.
3. `POST /api/ota/{esp,pico}` carries the MAC in a header; the image follows.
4. The ESP recomputes and compares in **constant time**, then invalidates the
   nonce whether or not it matched.

The key derivation costs nothing and means the literal PSK is never the
comparison value, so a bug that leaks the compared bytes leaks a derived key
rather than the Wi-Fi password. mbedTLS is already linked in.

### Rate limiting

Three failures locks OTA endpoints for 60 s, doubling to a 15-minute ceiling,
counted per-endpoint and reset on success. Log every attempt with the source IP.
A WPA2 PSK is typically short and human-chosen; without a lockout it is
brute-forceable over a LAN in minutes.

### What this does and does not defend against

**Does:** a curious person on the same network, an accidental upload of the
wrong image, a replayed capture of a previous upload.

**Does not, with web auth off:** anyone who already knows the AP password — by
design, that is the credential in that state (owner decision, 2026-09-29: web
auth off is exactly as open as it always was, never a new policy). Nor anyone
who has compromised the ESP itself, because *the ESP is the only thing
authorising the Pico update*. If the ESP is owned, the safety processor can be
reflashed with whatever the attacker likes.

**With web auth on, this AP-password gap is closed for these routes.**
`POST /api/ota/esp`, `/api/ota/esp/rollback`, `/api/ota/esp/recovery_exit`,
`/api/ota/esp/boot_guard_reset`, `/api/ota/pico`, `/api/ota/pico/rollback`,
`/api/factory_reset`, `/api/cfgfs/format_confirm` and `/api/sw_reset` are all
`ROUTE_TIER_ADMIN` in `route_tier_table.h` (owner decision, plan item 2b,
`docs/WEB_AUTH_PLAN.md`), so `kiln_http_register()`'s enforcement pre-handler
already refuses any request with no administrator web session — 401/403 —
*before* the handler (and therefore this section's HMAC check) ever runs. The
AP-password challenge/HMAC above is kept in addition, not replaced, so a
caller must hold both an administrator session AND a correct MAC once auth is
on. Knowing only the AP password is no longer sufficient in that state: it
was never rotated with the web password and can otherwise outlive it. PcTools
(`tools/PcTools/src/kilnctrl/ota_http_client.py`) already sends the
administrator session on every one of these calls via `http_auth.urlopen()`
(logging in from `KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD` on a 401), in
addition to computing the MAC below.

That last one is the interesting gap, and there is a real answer to it if it is
ever judged worth the cost: **have the Pico bootloader verify a signature over
the image against a public key burned into the bootloader**, which is written
once over SWD and never updated in the field. Then a compromised ESP can deliver
only images the developer signed. This is planned as an option, default off,
because it needs key management and the ability to sign a build — see
`BOOTLOADER.md` §6. The bootloader must be laid out to allow turning it on later
without a flash-layout change.

---

## 3. ESP32-S3 OTA

### The partition table has to change first, over a cable

The current table has a single `factory` app partition. OTA needs `otadata`
plus two app slots, which means a **new partition table**, and a partition table
can only be written over serial. **You cannot OTA your way into being
OTA-capable.** The first flash of the new table is a one-time USB operation, and
it must happen before any of this is useful.

#### The measurement that changed this plan

A build on 2026-08-16 put **`KilnCtrl.bin` at 0x1237A0 — 1167 KB**, with 22 % of
the app partition free. An earlier version of this section sized two 704 KB
slots around a 301 KB image, a figure taken from `PROJECT_STATUS.md` that was
several builds out of date. **The image does not fit in the slots that plan
proposed, and two copies of it do not fit in the 1500 KB app region at all.**

Dual-slot OTA is therefore impossible in the 2 MB the firmware is currently
configured for. Measure the image before sizing anything.

#### The flash nobody is using — and nobody has written down correctly

The firmware is built with `CONFIG_ESPTOOLPY_FLASHSIZE_2MB`. Whatever the module
actually has beyond 2 MB is unaddressable and has been sitting unused, which is
also why nobody noticed it was there: nothing could reach past 2 MB to look.

**How much is beyond 2 MB is currently an open question, because this repository
records three different modules:**

| Source | Part | Flash | PSRAM |
|---|---|---|---|
| `hardware/sourcing/master_buy_list.md`, `buy_list_aggregate.md` | `ESP32-S3-DevKitC-1U-N8R8` | 8 MB | 8 MB |
| 3D model in the footprint library | `ESP32-S3-DEVKITC-1-N8R2` | 8 MB | 2 MB |
| The board actually in use (Lonely Binary LB-ESP32S3-X1, 2026-08-16) | **`N16R8`** | **16 MB** | **8 MB** |

The supplier offers N8R2 and N16R8 only — **`N8R8` is not one of their
variants**, so the buy list is describing a part that was never bought from
there. An earlier revision of this document asserted 8 MB on the strength of
that BOM line; treat it as unverified.

- [ ] **Settle it with `esptool flash_id` on the board**, and update the BOM and
      the 3D model reference to match. Three records disagreeing is worse than
      one record being wrong, because each one looks authoritative on its own.
      **Blocked**: needs `esptool` against physical hardware. Already
      answered with high confidence a different way (LonelyBinary product
      page, N16R8/16 MB, 2026-08-17 — see the checked item below), but this
      box specifically asks for the `esptool flash_id` confirmation and the
      BOM/3D-model corrections, neither of which happened; both remain
      tracked separately in `KilnFW/TODO.md` 9.1.

**The layout below does not depend on the answer.** It needs 8 MB; at 16 MB
everything simply has more room after it. Sizing it for 8 MB and discovering
16 MB costs nothing, whereas sizing it for 16 MB and discovering 8 MB is a
bricked table. The conservative number is the right one to build against.

Switching away from 2 MB makes the whole problem go away, and — better — it
means **nothing that exists today has to move**:

Offsets assume **at least 8 MB**. On a 16 MB part every partition below is
identical and the spare region at the end simply grows.

```
# unchanged, live data, do not touch
nvs           data, nvs,      0x009000, 0x006000    24K
phy_init      data, phy,      0x00F000, 0x001000     4K
factory       app,  factory,  0x010000, 0x177000  1500K   <- kept as recovery image
wifi_nvs      data, nvs,      0x187000, 0x006000    24K
kiln_nvs      data, nvs,      0x18D000, 0x010000    64K
profiles_nvs  data, nvs,      0x19D000, 0x060000   384K

# new, entirely inside the previously unreachable 6 MB (IMPLEMENTED 2026-08-17)
otadata       data, ota,      0x200000, 0x002000     8K
# pad to the next 64K app-partition-alignment boundary  56K (gen_esp32part.py
# requires app-type partitions on a 0x10000 boundary; otadata is data-type
# and only needed 4K, so this gap is unavoidable, not wasted planning)
ota_0         app,  ota_0,    0x210000, 0x300000  3072K   <- grown 2026-08-21, see below
ota_1         app,  ota_1,    0x510000, 0x300000  3072K
factory       app,  factory,  0x810000, 0x300000  3072K   <- MOVED here 2026-08-21 from 0x10000
pico_img      data, undefined,0xB10000, 0x0E0000   896K   <- staging, see below (corrected
#                                                            up from 512K, see note)
coredump      data, coredump, 0xBF0000, 0x100000  1024K
# spare                       0xCF0000..0x1000000 ~3.06M
#
# 2026-08-21: factory MOVED off 0x10000 and all three app regions grew to
# 3072K. ESP-IDF's build-time size check is min() over every app partition
# (components/partition_table/check_sizes.py), so factory at 1500K set the
# ceiling for the whole project even though the OTA slots were larger -- the
# build was warning at 2% free with the image ~35 KB from a hard failure, and
# factory could not grow in place because wifi_nvs sits at 0x187000. The
# vacated 0x10000..0x187000 is declared `legacy_app` (data/undefined) so
# nothing re-allocates it and the dead image bytes there are unambiguous. No
# data partition moved, so nvs/wifi_nvs/kiln_nvs/profiles_nvs kept their
# contents across the change -- verified on hardware: Wi-Fi credentials, zone
# config and the profiles partition all survived, with no NVS reformat in the
# boot log. Free space went 2% -> 52%.
```

**Corrected from the original proposal, both against real numbers rather than
guesses:** `pico_img` is **896K, not 512K**. `SaftyFW/bootloader/flash_layout.h`
defines `BOOTLOADER_SLOT_FLASH_SIZE` as `0xD0000` (832K) per RP2040 application
slot — 512K cannot hold a full Pico image at all. 896K gives ~7.7% headroom over
832K while staying 64K-aligned. Subtype is `undefined` (0x06), not `fat`
(0x81): this partition is never mounted as a filesystem, only read/written as
an opaque byte range, and `fat` would misdescribe that. Implemented, host-
build-verified (`idf.py -C firmware/KilnFW build` clean, `gen_esp32part.py`
reports no overlap/overflow) in `firmware/KilnFW/partitions.csv` 2026-08-17 —
**not yet flashed to physical hardware.**

Why this shape rather than re-carving the existing app region:

- **No existing partition moves**, so the reason `partitions.csv` gives for
  freezing the first three entries — live zone, rule, profile and Wi-Fi data —
  is satisfied by not touching them at all rather than by careful arithmetic.
  The dangerous version of this change is the one that shifts `profiles_nvs`.
- **`factory` survives as a recovery image.** With `otadata` invalid or erased
  the bootloader falls back to `factory`, which is a known-good build reachable
  without a serial cable. Deleting it to reclaim 1500 KB would trade the last
  free recovery path for space there is no shortage of.
- **2 MB slots against a 1167 KB image** is 1.75×, which leaves room for what
  sections 6A and 8 of `KilnFW/TODO.md` still intend to add. The previous 704 KB
  proposal had already been overtaken before it was written down.

- [x] **Confirm the physical flash size** — done 2026-08-17 via the LonelyBinary
      product page for the board in hand (N16R8, 16 MB/8 MB), not `esptool
      flash_id` directly; the buy-list and 3D-model records are still stale and
      not yet corrected at the source (separate, tracked in `KilnFW/TODO.md` 9.1).
- [x] Set `CONFIG_ESPTOOLPY_FLASHSIZE` to the **confirmed** size — done
      2026-08-17 (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`).
- [x] **Reflash the bootloader** — the flash size lives in the bootloader
      header, so a new table alone is not enough. **Stale text, corrected
      2026-09-04**: this box's own "still outstanding, needs the physical
      board" note is contradicted by this same file's later "ESP OTA"
      checklist, which records "Bootloader + partition table reflashed
      against physical hardware, 2026-08-22" as done (JTAG, verified). Ticked
      here to match; the later entry is the authoritative one.
- [x] Confirm the offsets against the real table before flashing. Host-build
      verified 2026-08-17: `gen_esp32part.py`/`check_sizes.py` reports no
      overlap/overflow, and the six pre-existing entries are byte-identical to
      before (diffed, not just eyeballed). **Confirmed against physical
      hardware too, 2026-08-22** (same reflash event as above) — no longer
      just the host-build check this line originally described.
- [ ] Archive the pre-change table, and read out all four NVS partitions with
      `esptool read_flash` first. This is the one irreversible step in the plan,
      and this pass has no hardware access to perform it. **Blocked**: needs
      `esptool` against physical hardware; matches the later, more detailed
      duplicate of this item under §7's "ESP OTA" checklist, also left
      unchecked there for the same reason.

#### A staging partition also solves the relay problem

512 KB of `pico_img` is enough to hold a safety-processor image, which removes
the constraint described in §4 that the ESP must stream the Pico's image at link
speed because it has nowhere to put it. With staging, the browser upload runs at
Wi-Fi speed and finishes in a second, and the slow relay over the isolated link
happens afterwards — resumable, restartable, and immune to an HTTP timeout.

- [x] Decide between streaming and staging once the 8 MB table exists.
      **Decided, 2026-09-04**: staging — `ota_pico_do_stage()` writes into
      `pico_img`, confirmed above. Staging
      is better in every way except flash wear, and an update is not a frequent
      enough event for wear to matter. Streaming remains the fallback if the
      8 MB change is deferred.

### Rollback is not optional

**Implemented 2026-08-17.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is now
**on** (was off). With it on, a new image boots as `PENDING_VERIFY` and the
bootloader reverts to the previous slot unless the app calls
`esp_ota_mark_app_valid_cancel_rollback()`.

Do not call it at the end of `app_main()`. Call it only once the things that
matter have actually come up: NVS partitions readable, safety link exchanging
frames, web server answering. An image that boots but cannot talk to the safety
processor is exactly the image that must be rolled back automatically, and it
would pass a naive "we reached the end of main" check.

`App/main.c`'s `ota_rollback_confirm_task()` implements exactly this: a
low-priority FreeRTOS task, started once `nvs_report_capture()` has run and
`dashboard_http_start()`'s result is known, that polls
`safety_link_get_status()->link_up` until all three preconditions hold and only
then calls `esp_ota_mark_app_valid_cancel_rollback()`. All three preconditions
are wired in — none is a flagged gap, since `link_up` already meant "a valid
status within `SAFETY_LINK_UP_PERIODS` polls" and needed no new
`safety_link.h` getter.

Also enable `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` only if a security version is
going to be maintained; otherwise it will one day refuse a legitimate downgrade
during debugging.

### API

Use `esp_ota_ops` directly with a streamed POST handler — not `esp_https_ota`,
which is built for pulling from a URL. Write in chunks as the body arrives; a
full image will not fit in RAM.

---

## 4. RP2040 update over the isolated link

### Frames

Additive to `LINK_PROTOCOL.md`. All are ESP→Pico except the responses.

| Command | ID | Payload | Notes |
|---|---|---|---|
| `UPDATE_BEGIN` | 0x10 | 32 B: image length u32, image CRC32, target slot u8, version string 16 B, flags | Pico validates preconditions and replies `UPDATE_STATUS` |
| `UPDATE_DATA` | 0x11 | 4 B offset + up to 248 B | Offset is explicit, so a retry cannot silently write the wrong place |
| `UPDATE_END` | 0x12 | 4 B: image CRC32 repeated | Pico verifies the whole slot before accepting |
| `UPDATE_ABORT` | 0x13 | — | Marks the staged slot invalid, returns to normal operation |
| `UPDATE_STATUS` | 0x14 | Pico→ESP: state, bytes received, last error | Also sent unsolicited on refusal |

`UART_PROTO_MAX_PAYLOAD` is 253, so 248 bytes of image per frame after the
4-byte offset.

### Throughput, honestly

**HISTORY: the link ran at 9600, not 115200, for as long as the TCMT1109
optocoupler pair was fitted.** Measured 2026-08-23
(`firmware/SaftyFW/docs/HARDWARE.md` §1): that pair and R15's 1k pull-up
could not switch fast enough for 115200 or 57600 — zero frames received,
ever, at either — and 9600 was the fastest rate that tracked
sent-to-received one for one over a multi-minute run. At 9600 baud a full
253-byte frame is roughly 260 bytes on the wire before byte-stuffing, about
270 ms, and the protocol is stop-and-wait with a 200 ms ACK timeout; a 200 KB
image at that rate was about 830 frames, several minutes, not the 35 s a
115200 assumption would suggest. **That optocoupler pair was replaced by a
non-inverting ADuM1201WT digital isolator (U6) on 2026-08-25, so this ceiling
no longer applies; see `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig`
for the current measured baud and recompute the throughput figures above
against it rather than assuming 9600.**

That makes the retry behaviour matter more, not less, at whatever the
current baud turns out to be: `UART_PROTO_MAX_RETRIES` is 10 at a 200 ms
timeout, so a single persistently-failing frame costs 2 s, and any nonzero
frame-loss rate stretches an already-slow update substantially further.
Before building this:

- [ ] **Measure the real error rate of the isolated link at its current
      committed baud** over a sustained multi-megabyte transfer. The per-poll
      baud-walk measurement (`firmware/SaftyFW/docs/HARDWARE.md` §1) showed
      received tracking sent one for one over minutes of 500 ms status
      frames at the old 9600 optocoupler-era rate, but that is a much lighter
      load than a saturated update transfer, and the barrier itself has since
      changed. **Blocked**: needs a sustained multi-megabyte transfer over
      the physical isolated link (U6, 230400 baud); no hardware access this
      pass. Matches the duplicate of this item under §7's "Pico update"
      checklist, also left unchecked there.

**Retransmit-round throughput — computed, not yet measured (2026-09-06).**
The paragraphs above compute whole-transfer throughput for the *old*
stop-and-wait design; they say nothing about the *current* broadcast +
gap-report design's per-round repair rate, which is what 10.8c's cap
actually bounds. Worked from code, not a capture:

- `ota_pico_relay.c`'s retransmit loop (`RELAY_MAX_RETRANSMIT_ROUNDS = 10`)
  waits up to `RELAY_GAP_ROUND_WAIT_MS = 2000` ms per round for a fresh
  `UPDATE_STATUS` gap report, then resends every named gap once. A round
  can name at most `SAFETY_LINK_UPDATE_STATUS_MAX_GAPS = 32` missing
  248-byte chunks (`safety_link.h`).
- Sending 32 `UPDATE_DATA` frames (253 B payload + 5 B offset+cmd header,
  ~260 B on the wire before byte-stuffing, same 10-bit/byte 8N1 arithmetic
  the doc already uses above) at 230400 baud takes 32 × 260 × 10 / 230400 ≈
  0.36 s — negligible next to the wait.
- The round's own cadence is therefore set by the Pico's ~500 ms
  `UPDATE_STATUS` cadence, not the wire: **best case ≈ 32 × 248 B / 0.5 s ≈
  15.5 kB/s** of gap repaired per round; **worst case** (a round where no
  fresh status lands before the 2 s `RELAY_GAP_ROUND_WAIT_MS` timeout) **≈
  32 × 248 B / 2 s ≈ 3.9 kB/s**.
- Over the full 10-round cap that bounds total repairable damage at 10 ×
  32 × 248 B ≈ 77.5 KiB, taking between 5 s (10 × 500 ms, gap reports never
  missed) and 20 s (10 × 2 s, every round times out waiting for a status
  frame) of wall clock — **before** `UPDATE_END`'s CRC check even runs, and
  on top of whatever the initial unacknowledged sequential streaming pass
  already took.
- No capture evidence exists yet (checked `tools/PcTools/logs/*.log` and
  `docs/bench_snapshots/` for gap-count/round timing lines — none present);
  the 15.5 kB/s / 3.9 kB/s / 5–20 s figures above are computed from the
  constants in `ota_pico_relay.c` and `safety_link.h`, not measured against
  the physical link. Re-derive from a real capture once 10.0's bench
  measurement lands, and replace this note rather than adding beside it.

- [ ] Decide whether to raise the baud rate for the duration of an update.
      **Blocked on the measurement above** — this is a real engineering
      decision (not just a software task), and it needs that data first.
      The old measurement's "not without a faster part or line driver"
      conclusion was specific to the TCMT1109/R15 pair and does not carry
      over to U6, the digital isolator that replaced it — re-evaluate against
      current hardware rather than assuming that limit still holds. A
      negotiated rate in `UPDATE_BEGIN` with an automatic fallback remains
      the flexible option either way.
- [x] Report progress to the GUI at least every 2 s. A silent 35-second bar is
      indistinguishable from a hang. **2026-09-04 (triage verification)**:
      duplicate of the item already ticked in §7's "Pico update" checklist —
      `ota_pico_relay_get_status()` updates at each phase transition and
      roughly every 10% during streaming/retransmit, polled by
      `GET /api/ota/pico/status`, which `ota_page.html`'s `picoProgressBar`
      reads.

### Flow

1. ESP checks its own preconditions, refuses locally if unmet.
2. `UPDATE_BEGIN` → Pico re-checks its own preconditions, opens the relay if it
   is not already open, and either accepts or refuses with a reason.
3. Pico erases the staging slot. **This takes seconds** and must not be done
   inside a frame handler; the response is "erasing", and the ESP waits for
   "ready".
4. `UPDATE_DATA` streams, written straight through to flash. No RAM buffering of
   the whole image — the RP2040 has 264 KB of SRAM and the image may exceed it.
5. `UPDATE_END` → Pico verifies the CRC over what it actually wrote back out of
   flash, not over what it thinks it received.
6. Pico marks the slot pending and reboots. The bootloader takes over.
7. ESP waits for the boot version frame, compares it to what it sent, and reports
   success or a mismatch.

Step 5 reading back from flash matters: it is the only check that catches a
write that reported success and did not land.

---

### The image must say what it is, before anything is erased

`UPDATE_BEGIN` carries a header that the Pico validates **before it erases a
single sector**:

| Field | Purpose |
|---|---|
| `magic` | A constant identifying this as a `SaftyFW` image and nothing else |
| `target` | `RP2040` — refuses an ESP image outright |
| `header_version` | Refuse the unrecognised rather than guess |
| `protocol_version`, `min_compatible` | What the new image will speak (see below) |
| `length`, `crc32` | Checked at the end against what was written |

Without this, uploading a `KilnFW` image to the Pico endpoint erases the staging
slot before discovering the mistake. The slot is recoverable — it is not the
running one — but a needless erase of a safety processor during what the operator
thinks is a routine update is exactly the kind of avoidable scare that makes
people stop applying updates.

The ESP does the same check on its own side. ESP-IDF images already carry a
magic byte and a chip ID; **verify them before calling `esp_ota_begin()`**, not
after.

### Version compatibility is checked before, and after

The two processors check each other's protocol version continuously
([`LINK_PROTOCOL.md`](LINK_PROTOCOL.md), `ANNOUNCE_VERSION`). Updates interact
with that in three places:

1. **Before pushing a Pico image**, the ESP compares the image header's
   `protocol_version` / `min_compatible` against its own. If they are
   incompatible, **refuse** — because the only route to the Pico is through the
   ESP, and installing an image it cannot talk to turns the next update into a
   debug-probe job. An override exists for the case where the ESP is about to be
   updated too, and it must be an explicit, separately-confirmed action.

   Implemented (`firmware/KilnFW/TODO.md` 9.4): the `UPDATE_BEGIN` wire
   header's own `protocol_version` field is always the ESP's own compiled
   `KILNLINK_PROTOCOL_VERSION` — it says what the ESP will speak while
   relaying, not what the uploaded image itself was built against, so it
   cannot be compared against itself. The uploaded image states that
   separately, in the `saftyfw_image_identity_t` build-identity record every
   SaftyFW slot image already carries (`kilnlink/saftyfw_image_identity.h`,
   `link_protocol_version` field, added in
   `SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION` 2). `ota_http_pico.c`'s
   manual-upload handler re-reads the staged image right after upload, and
   if that field disagrees with the ESP's own `KILNLINK_PROTOCOL_VERSION`,
   refuses with `409 Conflict` (a JSON body naming both versions) before the
   relay ever starts, unless the request carries `X-Ota-Force-Version: 1` —
   the explicit, separately-confirmed override this section calls for. This
   is proactive and strictly earlier than, never a replacement for, the
   Pico's own `UPDATE_STATUS_ERR_VERSION_INCOMPATIBLE` refusal described
   above, which still runs unconditionally against the wire header's own
   `protocol_version`. The boot-time auto-update path never goes through
   this handler and so never sees or sets the override.
2. **Update the ESP first** when both need it. The ESP can always be recovered
   over USB; the Pico's easy path runs through the ESP. The GUI should say this
   rather than leaving the order to chance.
3. **After either update**, the handshake re-runs and the result is reported as
   the outcome of the update — "now running 1.5.0 / protocol 6, compatible" or a
   named mismatch. A version-mismatch fault storm immediately after a successful
   flash is a confusing way to learn the two builds do not match.

### The ESP cannot buffer the image it is relaying

A Pico image is on the order of 200 KB. The ESP has neither the RAM to hold it
nor a spare flash partition to stage it in — the proposed layout leaves 84 KB
above the app slots. So the relay **streams**: HTTP body in, `UPDATE_DATA`
frames out, nothing retained.

That makes the browser upload rate the link rate, about 8 KB/s, so a 200 KB
upload occupies the socket for roughly half a minute. Consequences that have to
be designed for rather than discovered:

- **Do not read the request body faster than the link drains.** TCP flow control
  does the work if the handler simply stops calling `httpd_req_recv()`; what
  breaks it is reading ahead into a buffer that then has nowhere to go.
- **The HTTP receive timeout must exceed the whole transfer**, not just one
  chunk. `CONFIG_HTTPD_REQ_HDR_LEN`-style defaults are irrelevant here; the
  socket timeout is what bites.
- **A browser or proxy may still time out.** The MCP tool path does not have
  this problem, which is another reason the tools matter more than the page.
- [x] **Superseded if the 8 MB table lands:** a 512 KB `pico_img` staging
      partition removes this constraint entirely — fast upload, then a slow
      resumable relay that no HTTP timeout can interrupt. See §3. Streaming
      remains the fallback if the flash-size change is deferred. **Resolved,
      2026-09-04**: the 8 MB (actually 16 MB, N16R8) table landed
      2026-08-17/21 and `pico_img` (896K, corrected up from the original
      512K estimate) is real — `ota_pico_do_stage()` writes the browser
      upload there at Wi-Fi speed, and `POST /api/ota/pico` returns `202
      Accepted` immediately, with the slow relay running afterward from the
      staged copy. This is the resolved state this bullet was hedging
      against, not the fallback.

### Throughput: stop-and-wait is the wrong tool for bulk transfer

Stop-and-wait with a 200 ms timeout leaves the wire idle for most of every round
trip, and a single persistently-failing frame costs 2 s.

Because `UPDATE_DATA` carries an **explicit offset** and the whole image is CRC'd
at the end, acknowledging each frame is not what makes the transfer correct — the
final verify is. So:

> Send `UPDATE_DATA` as **unacknowledged broadcast frames**, and have the Pico
> report the ranges it is missing.

The Pico tracks received ranges in a bitmap (a 256 KB image at 248 bytes per
frame is ~1030 bits, 129 bytes of RAM) and emits a `UPDATE_STATUS` gap report
every 500 ms. The ESP streams continuously, then retransmits whatever the gap
reports name, until there are no gaps and the CRC verifies.

This reuses `UPDATE_PROTO_MSG_BROADCAST`, which already has to exist, and it
turns a lossy link from a linear slowdown into a small percentage of
retransmission. It also removes the retry-storm failure mode entirely.

- [x] Cap total retransmission rounds, and fail cleanly rather than looping if a
      range never lands. **2026-09-04 (triage verification)**: duplicate of
      the item already ticked in §7's "Pico update" checklist —
      `ota_pico_relay.c` caps at 10 rounds (`RELAY_MAX_RETRANSMIT_ROUNDS`)
      and aborts cleanly on `UPDATE_STATUS_ERR_RETRANSMIT_CAP`. A link that cannot deliver the same 248 bytes after ten
      attempts is broken, and saying so beats retrying forever.

### An ESP reboot must not look like an ESP failure

When the ESP reboots to finish its own update, the link goes quiet and
`SAFETY_MODEL.md` S6(b) — "main controller unhealthy" — starts counting. Default
`link_timeout_s` is 10 s, so a normal reboot may or may not trip it depending on
boot time. **A safety trip on every routine ESP update is precisely the
over-sensitivity this design is supposed to avoid.**

So the ESP sends `SAFETY_CMD_ANNOUNCE_REBOOT` before it goes:

- The Pico starts a grace window, default 60 s, during which S6(b) does not trip.
- **The grace window is not permission to heat.** The Pico's own guards are
  unaffected, and it holds the relay in whatever state its guards demand. The ESP
  cannot command heat while it is rebooting anyway.
- If the ESP does not come back within the window, S6(b) trips as normal. A
  reboot announcement that is followed by silence is a *worse* signal than
  unannounced silence, not a better one.
- Any current above `i_present_a` during the grace window ends it immediately and
  trips. Heat with nobody in charge is the one case where a promise to return
  counts for nothing.

### One update at a time, and a record of what happened

- [x] A single update mutex covering both processors. A second browser tab, or
      an agent racing a human, must be refused rather than interleaved.
      **2026-09-04 (triage verification)**: `ota_http.c`'s
      `ota_http_update_in_progress()` / `ota_update_claim_t` is a single
      claim shared across ESP and Pico contexts — `ota_interlock.c`'s
      `other_update_in_progress` check refuses a Pico update while an ESP
      claim (or vice versa) is held, and the function fails safe ("in
      progress") even before the OTA subsystem has finished starting.
- [x] An append-only update record in NVS: timestamp, processor, image SHA-256,
      version before and after, result. **2026-09-04**: `ota_record.c`
      (`ota_record_build()`/`ota_record_log()`) carries `image_sha256_hex`,
      `version_after`, timestamp and reason, persisted and surfaced via
      `GET /api/ota/esp/status`'s `last_update` object
      (`ota_http_esp.c:531-538`).
- [x] A downgrade is allowed but logged as such. **2026-09-24**: still
      allowed unconditionally; `ota_record.c`'s new `ota_version_compare()`
      (best-effort parse of `esp_app_desc_t.version`'s `git describe`
      shape: tag, commit count, `-g<hash>`, `-dirty`, including the
      32-byte-truncated form the bench actually produces; hash digits never
      order two builds, and bare hashes or same-count/different-hash pairs
      read UNKNOWN rather than a guess) stamps `is_downgrade`/`version_compare_known` into
      `ota_record_t` at `ota_record_fill()` time, logged and surfaced via
      `GET /api/ota/esp/status`'s `last_update.downgrade_known`/`is_downgrade`.
      ESP path only — the Pico path's before/after strings are both blank,
      which correctly reads back as `version_compare_known=false`, not a
      false "not a downgrade".

### What holds the heaters off while the ESP reboots

- [ ] **Establish what the SX1509's outputs do across an ESP reset.** Its
      `~RESET` is driven by the ESP; if the expander is not reset and its output
      register is non-volatile across the ESP's reboot, relays could stay
      energised through the update. The interlocks require an idle kiln so
      nothing should be on — but "should be" is not the standard that applies to
      the thing that energises heaters, and this is a five-minute bench check.
      **Blocked**: needs the physical board on the bench; this pass has no
      hardware access. (Related, already known and documented in `CLAUDE.md`:
      the SX1509 can fail its post-reset init after a JTAG `debug_reset`,
      requiring a second reset — a different symptom of the same "what
      happens to this chip across a reset" question.)

## 5. Recovery

| Failure | ESP | Pico |
|---|---|---|
| Power lost mid-transfer | Old slot still active, nothing changed | Same — staging slot is not the running one |
| Image corrupt, CRC fails | Rejected before activation | Rejected before activation |
| Image valid but boots badly | Bootloader rolls back if the app does not confirm | Bootloader rolls back if the app does not check in |
| Both slots bad | Serial flash over USB | **SWD over the debug probe** |
| Link dies mid-transfer | Transfer times out, staged slot discarded | Same, after a timeout |

The Pico's last-resort path is the debug probe, which is exactly why the plan
already insists on a DEBUG header being fitted before A1 is soldered down
(`SaftyFW/docs/HARDWARE.md` §7b). **Field-updating the safety processor over a
link the safety processor also depends on is only defensible because SWD exists
underneath it.**

---

## 6. Web UI and MCP surface

### Web page

A single page, reachable only when the kiln is idle, showing for each processor:
running version, build commit, build date, dirty flag, active slot, and the
version in the inactive slot. Then a file picker, a password field, a progress
bar, and a rollback button per processor.

- [x] Show the interlock state **before** the user picks a file, with the
      specific blocker named. **2026-09-04 (triage verification)**:
      `ota_page.html`'s `#interlockBox` calls `GET /api/ota/interlock`
      (`ota_http.c`) on load and before any file is chosen — see
      `ota_page.html`'s `loadInterlock()` (cited by symbol, not line number,
      since those drift on every edit to the file).
- [x] Refuse to start if the other processor is mid-update. **2026-09-04**:
      covered by the single-claim mutex documented under "Reboots and
      concurrency" above (`ota_http_update_in_progress()`).
- [x] Warn, and require a second confirmation, when the uploaded image's
      protocol version differs from the running one. **2026-09-24**: the
      Pico path only — the ESP's raw `.bin` still has no embedded-version
      parser, so a pre-upload check there remains genuinely infeasible (per
      §3's note above). `ota_http_pico.c`'s accept step already refused a
      protocol mismatch with `409 {"error":"protocol_version_mismatch",...}`
      unless `X-Ota-Force-Version: 1` is set (built in an earlier pass);
      this pass added the missing web-page half — `ota_page.html`'s Pico
      update button now catches that 409, shows a cancellable confirmation
      dialog naming both protocol versions, and retries with the force
      header only on explicit confirmation.

### MCP tools

These matter more than the web page for day-to-day work, since most updates
during development will be driven by an agent:

- [x] **2026-08-18, `tools/PcTools/src/kilnctrl/ota_http_client.py` +
      `mcp_server.py`.** Four tools, HTTP-based (this is the board's own web
      server, not the UART link the rest of `mcp_server.py` uses):
      `ota_get_challenge(host)`, `ota_update_esp(image_path, password, host)`,
      `ota_update_pico(image_path, password, host)`, `ota_status(host)`.
      **Deviates from the four names sketched here, on purpose**: there is no
      `ota_rollback(processor)` HTTP endpoint in `ota_http.c` to wrap (nothing
      under §3/§4 exposes a rollback trigger over HTTP — rollback today is
      automatic, bootloader-driven, not an operator action), so building that
      tool would mean inventing client behavior with no server side to call.
      `ota_get_challenge()` stands in as the fourth tool instead — a real,
      already-built endpoint (`GET /api/ota/challenge`) with no wrapper
      before this pass, useful on its own for confirming the OTA HTTP surface
      is reachable. `ota_status()` is **not** "both processors: versions,
      slots, interlock state" as sketched — only `GET /api/ota/pico/status`
      (relay phase/percent/last_error) exists as an HTTP route today; ESP
      self-update progress (`ota_http_get_esp_progress()`) and the persisted
      `ota_record` history are real C-level state with no HTTP endpoint yet,
      and `ota_status()` says so explicitly rather than fabricating a reading
      for either. Host discovery reuses the UART `wifi_get_status()` query
      (station IP) with the board's AP-fallback address as a backstop, same
      pattern `gui.py`'s Wi-Fi Settings popup already uses.
- [x] `ota_status()` reports the one thing that actually is queryable today
      (Pico relay progress) — see the deviation note above for what it does
      not yet cover.
- [x] `ota_update_esp(image_path, password)` — streams, returns the new
      version (plus partition and byte count) on success, or the board's
      specific refusal reason on failure.
- [x] `ota_update_pico(image_path, password)` — stages + starts the relay,
      returns immediately (202) with staged bytes/CRC; does not itself wait
      for the ~35s+ relay to finish (see `ota_status()`).
- [x] `ota_rollback(processor)` — **built since this section was last
      reviewed, 2026-09-04**: the gap this bullet described (no HTTP endpoint
      to wrap) is closed. `ota_http.c` now registers
      `POST /api/ota/esp/rollback` (`ota_esp_rollback_post_handler`) and
      `POST /api/ota/pico/rollback`, and `tools/PcTools/src/kilnctrl/mcp_server_ota.py`
      exposes `ota_rollback_esp(password, host)` (its own doc comment
      describes it as "the OTHER half of the rollback story" alongside
      `ota_rollback_confirm_task()`). The wire-level Pico half
      (`SAFETY_CMD_ROLLBACK` = `0x17`, `SAFETY_CMD_ROLLBACK_RESULT` = `0x25`)
      landed in `LINK_PROTOCOL.md` §4, protocol 9+. This bullet's own
      deviation note is now stale — corrected here rather than deleted, so
      the "why it was missing" history stays legible.
      **Hazard, 2026-09-04 (numbers refreshed 2026-09-06)**: `ota_rollback_esp()`
      rolls the running image back
      to `factory`, but the `zones_cfg` NVS blob is not versioned per-partition
      — it is whatever was last written. If `zones_cfg` has ever been saved by
      v22 firmware (`ZONES_CFG_VERSION` 22, `992f395` — per-zone
      `progress_band_c`) and the board is then rolled back to an older
      version, `zones_config_store.c`'s `ZONES_DECODE_NEWER` path refuses the
      newer-than-firmware blob and falls back to **firmware defaults for that
      boot** — including default PID gains, not the tuned ones — while leaving
      the on-flash blob untouched (`zones_config_store.c:116-128`). Nothing is
      lost: flashing v21 again re-reads the same untouched blob and every
      tuned value comes back. But a session that rolls back and then fires
      without noticing is firing on default gains. Check `GET
      /api/zones/config` (or `kiln_call(name="control_get_zones")`) reads back
      the expected gains before heating after any rollback.
- [x] Every push tool's board-reported result (including refusals) is
      returned verbatim to the caller. **Not yet true**: neither tool
      computes or logs a local SHA-256 of the image before sending — the doc
      text's "logged, with the image hash" is about the image content hash,
      which this pass does not add on the PC side (the ESP-side `ota_record`
      has the same gap, noted in `ota_record.h`'s own header comment).
- [x] **Unit-tested against mocked HTTP** (no live board):
      `tools/PcTools/tests/test_ota_http_client.py`, 15 tests covering HMAC
      derivation, challenge parsing, push request construction (including the
      `X-Ota-Mac` header value), and both JSON and plain-text (409/403)
      response handling. **Not yet verified against a physical board** — no
      hardware attached in this pass's environment; that remains open (see
      §7 "Verification").

---

## 7. Completion checklist

**Preconditions and interlocks**
- [x] Interlock table above implemented on the ESP, each refusal naming its
      blocker. **2026-09-04 (triage verification)**: `ota_interlock.c`
      checks the full precondition set (update mutex, safety link,
      autotune, profile run state, heater-commanded, per-zone temperature
      ceiling, invalid readings) in the documented order, and is host-tested
      exhaustively in `firmware/KilnFW/App/test/test_ota_interlock.c`
      (`test_check_order`, one test per precondition).
- [x] Pico independently enforces relay-open, no-trip-pending, and the
      temperature ceiling. **2026-09-04**: `firmware/SaftyFW/src/tasks/update_task.c`'s
      `update_task_gather_preconditions()` reads
      `safety_core_get_output_status()`/`thermo_task_get_snapshot()` itself
      rather than trusting the ESP, per this doc's own top-of-file
      2026-08-19 code-inspection note (`git show a7a5653`).
- [x] GUI distinguishes "updating" from "not responding" **without**
      weakening the relay block. **2026-09-04**: `LINK_PROTOCOL.md` §8
      already documents this as implemented policy; the OTA page's own
      `#interlockBox`/status text sources its "safety processor updating"
      framing from the same interlock/status data, never from a bypass of
      `relay_authority_on_blocked()`.
- [~] Temperature ceiling configurable, default 100 °C. **2026-09-04**: the
      **default 100 °C is correct** (`ota_interlock.h:65`,
      `OTA_INTERLOCK_TEMP_CEILING_C 100.0f`), but it is **not actually
      configurable** — that header's own comment says the constant is used
      "unless/until a real config item exists." Left unchecked: this box
      asks for more than what exists.

**Authentication**
- [x] Nonce endpoint: 16 random bytes, single use, 30 s expiry. **2026-08-17**:
      `GET /api/ota/challenge` (`firmware/KilnFW/App/drivers/http/ota_http.c`),
      `esp_fill_random()` for entropy, backed by the host-tested
      `ota_auth_nonce_issue()`/`_check()` state machine
      (`App/drivers/ota_auth.{h,c}`, 246/246 host tests,
      `App/test/test_ota_auth.c`).
- [x] `HMAC-SHA256(HMAC-SHA256(ap_password, "kilnctl-ota-v1"), nonce || context)`.
      **2026-08-17**: `ota_http_verify_request()`'s `hmac_sha256()` helper, via
      the PSA Crypto API (`psa_import_key()` + `psa_mac_compute()`) rather
      than mbedtls's classic `mbedtls_md_hmac()` family -- that whole API is
      gated behind `MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` in this vendored
      mbedtls 4.x/TF-PSA-Crypto build, and upstream's own migration guide
      recommends against defining that macro. `context` is the exact `"esp"`
      or `"pico"` literal per `ota_http_context_t`.
- [x] Constant-time comparison; nonce invalidated on both paths. **2026-08-17**:
      `ota_auth_constant_time_equal()` (inspects every byte regardless of
      where the first mismatch is); `ota_auth_nonce_invalidate()` is called
      on both the match and mismatch paths in `ota_http_verify_request()`.
- [x] Lockout after 3 failures, doubling to 15 minutes, every attempt logged.
      **2026-08-17**: `ota_auth_lockout_record_failure()`/`_record_success()`,
      two independent instances (one per context, "counted per-endpoint").
      Every challenge issue and every verify attempt logs the source IP
      (`get_client_ip()`, `httpd_req_to_sockfd()` + `getpeername()`) via
      `ESP_LOGI`/`ESP_LOGW`. **Not yet true**: a stale/expired/never-issued
      nonce does NOT count as a lockout failure (deliberate -- see
      `ota_http.h`'s doc comment: that is the client's timing, not a
      wrong-password guess), so only an actual bad-MAC attempt against a
      valid nonce increments the counter -- this is a considered
      interpretation of "failure," not an oversight, but worth flagging
      since the doc text doesn't make the distinction explicit.
- [x] Documented: this does not defend against someone who knows the AP
      password. **2026-09-04**: the OTA web page now exists (`ota_page.html`)
      but had no on-page statement of this limitation; added a `.note`
      paragraph directly under the password field pointing at UPDATE_PROTOCOL.md
      §2's "What this does and does not defend against" (also names the
      compromised-ESP gap that section calls out specifically).

**Built as of 2026-08-17**: both `POST /api/ota/esp` and `POST /api/ota/pico`
now exist -- `ota_http_verify_request()` has real callers. See the "ESP OTA"
and "Pico update" sections below for what each actually covers.

**Version compatibility** (`LINK_PROTOCOL.md`, `ANNOUNCE_VERSION`)
- [x] `ANNOUNCE_VERSION` = `0x0F` implemented: the ESP announces itself, unprompted
- [x] `min_compatible` field added to both version frames at a fixed offset.
      **2026-09-03**: `kilnlink_announce.h`/`.c` and `kilnlink_fw_version.h`/`.c`
      (CommonFW) put it at bytes 3..4, right after `protocol_version`, in both
      the ESP→Pico `ANNOUNCE_VERSION` frame and the Pico→ESP `FW_VERSION`
      reply, matching the doc's own offset table exactly. A peer built before
      this field existed sends a payload shorter than 5 bytes; the decoders
      (`safety_parse_fw_version()` in KilnFW's `safety_link_frame.c`, the
      SaftyFW `link_task.c` receive path) both refuse to read bytes 1-4 at all
      when `len < 5`, so `peer_version_known` stays **false** rather than
      defaulting `min_compatible` to a zero-initialized "compatible with
      everything". An absent field is therefore treated the same as no
      version frame ever arriving: unknown, which the poll/fault logic below
      fails closed on (`version_mismatch = !peer_version_known ||
      !peer_version_compatible` — KilnFW `safety_link_poll.c:253`), never as
      an all-versions-accepted default. That direction was chosen because the
      alternative (treating absence as "compatible") would let a
      pre-min_compatible peer skip the check entirely — exactly the class of
      bug this field exists to close.
- [x] Both sides check **both** directions of `peer.protocol >= self.min_compatible`.
      Both firmwares call the same formula
      (`compatible == peer.protocol >= self.min_compatible && self.protocol >=
      peer.min_compatible`) from a pure, host-testable function:
      `safety_link_versions_compatible()` (KilnFW, `safety_link_frame.c`) and
      `link_frame_versions_compatible()` (SaftyFW, `link_frame.c`) — same
      formula, independently exercised. `SaftyFW/test/test_link_frame.c`'s
      `test_versions_compatible()` runs the full combination matrix (self
      newer/older than peer, self's floor above peer's protocol, peer's floor
      above self's protocol, both floors violated, a self-inconsistent peer
      claim) and `KilnFW/App/test/test_safety_link_compile.c` proves the same
      formula from the ESP side. Both are part of the standing host-test
      suites (2036/2036 SaftyFW, 70/70 in the KilnFW safety_link suite).
- [x] ESP on mismatch: link fault, heating blocked, GUI names both versions and
      which to update. `safety_link_poll.c` sets `SAFETY_FAULT_SRC_SAFETY_LINK`
      on `version_mismatch` (line ~350); `peer_protocol_version`/
      `peer_min_compatible` are exposed via
      `safety_link_get_peer_version_status()` for the GUI to render both
      sides' numbers.
- [x] Pico on mismatch: `DEGRADED_NO_CONTEXT`, **no trip latched**, context frames
      discarded unparsed, context-free guards still running and still commanding
      the relay, context-dependent guards reported as disabled. `link_task.c`
      sets `s_degraded_no_context = !compatible` directly from the version
      check and never calls a trip/relay function from that path (structurally
      cannot — `link_task.c` is barred from naming the relay at all, per
      `tools/check_isolation.ps1`).
- [x] Compatibility floor frozen: framing, `ANNOUNCE_VERSION`, `FW_VERSION` and
      the `UPDATE_*` frames work regardless of version, ids `0x00`–`0x0F` reserved.
- [x] Floor layouts may only be **appended** to, never reordered or resized.
      `min_compatible` itself is the appended field this rule was written for:
      it landed at bytes 3..4, after the pre-existing `protocol_version` at
      1..2, never displacing anything.
- [x] Re-checked on every reconnect and every `boot_id` change, not once at boot.
      `safety_reset_stale_peer_info_if_link_down()` clears
      `peer_version_known` when the link is observed down, so the poll loop's
      `!peer_version_known` branch re-requests `FW_VERSION` on reconnect
      instead of trusting a stale pre-drop verdict forever (covered by
      `test_safety_link_compile.c`'s own reconnect test).
- [x] ESP refuses to push a Pico image it could not then talk to, unless
      explicitly overridden. **2026-09-04, deviation noted**: enforced
      Pico-side rather than as an ESP pre-check — `ota_pico_relay.c` surfaces
      `SAFETY_LINK_UPDATE_ERR_VERSION_INCOMPATIBLE` ("protocol version
      incompatible") when the Pico itself refuses `UPDATE_BEGIN` over a
      version mismatch, and that refusal reaches the ESP's relay status. The
      net effect (an incompatible push is refused, with a named reason) is
      what this bullet asks for; it is not literally "the ESP compares the
      header before sending," which this doc's §4 also describes as the
      design.
- [x] GUI states the order — ESP first — when both need updating. **2026-09-24**:
      `ota_page.html` now shows a static `#updateOrderHint` card ("update the
      ESP first") whenever both the ESP and Pico file pickers have a file
      chosen, cleared as soon as either selection is cleared.

**Image identification**
- [x] Pico image header: magic, target, header version, protocol version,
      `min_compatible`, length, CRC32 — all validated **before the first erase**.
      **2026-09-04 (triage verification)**: `update_task.c`'s handler calls
      `update_receiver_handle_begin()` (which runs `update_image_header_validate()`)
      and only reaches `update_task_erase_slot()` after an
      `UPDATE_BEGIN_ACCEPTED` outcome — a `UPDATE_BEGIN_REFUSED_HEADER_INVALID`
      or `_REFUSED_VERSION_INCOMPATIBLE` returns before any erase call.
      Host-tested: `firmware/SaftyFW/test/test_update.c`'s
      `test_image_header_validate`/`test_handle_begin`.
- [x] ESP image magic and chip ID checked before `esp_ota_begin()`.
      **2026-09-04**: `ota_http_esp.c` reads the 24-byte `esp_image_header_t`,
      checks `hdr.magic != ESP_IMAGE_HEADER_MAGIC` and
      `hdr.chip_id != ESP_CHIP_ID_ESP32S3`, and refuses (`goto cleanup`) —
      all before the `esp_ota_begin()` call a few lines below, matching the
      function's own inline comment quoting this exact requirement.

**ESP OTA**
- [x] **Physical flash size confirmed** — done 2026-08-17 via the LonelyBinary
      product page for the board in hand (N16R8, 16 MB), not `esptool flash_id`
      directly. Buy-list/3D-model records are still stale and separately tracked.
- [x] `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` set (2026-08-17).
- [x] **Bootloader + partition table reflashed against physical hardware,
      2026-08-22** — via `flash_firmware()`'s JTAG path
      (bootloader@0x0, partition table@0x8000, app@0x810000, each verified),
      reported "flashed and verified OK". The app offset (0x810000) is this
      table's `factory` partition, so this flash exercises the new table and
      bootloader but has never written an OTA slot — see the rollback item
      below for what that means.
- [x] New partitions placed entirely above `0x200000`, so **nothing existing
      moves** — implemented in `firmware/KilnFW/partitions.csv` 2026-08-17,
      diffed to confirm the six pre-existing entries are byte-identical
- [x] `factory` retained as the serial-free recovery image
- [x] Slot size checked against a **measured** image, not a remembered one. It was
      1167 KB on 2026-08-16, not the 301 KB this plan was first written around —
      `ota_0`/`ota_1` are 2048K each, 1.75x headroom
- [x] Offsets confirmed against the live table — host-build-verified
      (`gen_esp32part.py`/`check_sizes.py` reports no overlap/overflow) AND
      now flashed and verified against the physical board, 2026-08-22 (see
      the bootloader-reflash item above).
- [ ] Pre-change table archived for rollback. **Stale premise, flagged
      2026-09-04**: this line's own reasoning ("nothing to archive yet,
      since no physical flash has occurred") is no longer true — the
      bootloader-reflash item above records the physical flash as done
      2026-08-22. Whether the pre-change table was actually archived before
      that flash is not recorded anywhere this pass could find; left
      unchecked rather than assumed. Worth a direct question to the owner:
      was this step done, or did the irreversible flash happen without it?
- [ ] **`nvs`, `wifi_nvs`, `kiln_nvs` and `profiles_nvs` read out with esptool and
      saved before the table is flashed.** The one irreversible step in this whole
      plan is writing a wrong partition table over live config. **Same flag
      as above**: the table has since been physically flashed (2026-08-22),
      so this box's status is now "was this done beforehand, or not" rather
      than "still pending" — this pass has no hardware access to check
      either way and found no record of it having happened.
- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` — set 2026-08-17, confirmed by a
      clean `idf.py build`
- [x] `esp_ota_mark_app_valid_cancel_rollback()` called only after NVS, the web
      server and the OTA HTTP routes are all confirmed up — never at the end
      of `app_main()`. Implemented as a background task in `App/main.c`
      (`ota_rollback_confirm_task()`); a live safety-link exchange was in this
      bar originally but dropped 2026-08-22 (see `boot_guard.h`). A factory
      boot skips the call entirely (2026-08-24, it has nothing to cancel
      there) rather than logging it as a false ERROR. **Hardware-verified
      2026-09-03**: a real `POST /api/ota/esp` push (not JTAG) wrote
      `KilnCtrl.bin` into `ota_0`; after reboot the log showed
      `running partition: 'ota_0' (subtype 0x10)` followed by
      `OTA rollback confirmed: NVS readable, web server and OTA routes up --
      this image is no longer PENDING_VERIFY`. The board's I/O expander
      failed its first post-reset init on that boot (JTAG `debug_reset`
      does not power-cycle external I2C peripherals) and the safety
      processor correspondingly latched a stale S6a trip; a second
      `debug_reset` cleared both and the board came back fully healthy
      (expander up, safety armed, thermocouples reading, Wi-Fi
      reconnected) — a JTAG-reset artifact of this verification method,
      not a defect in the rollback-cancel path itself.
- [x] Streamed `esp_ota_ops` POST handler, no whole-image buffering.
      **Built since this line was last reviewed, 2026-09-04**:
      `ota_http_esp.c`'s `ota_esp_post_handler()` reads the image header,
      then loops `httpd_req_recv()` into a fixed-size chunk buffer
      (`s_ota_esp_chunk`) and calls `esp_ota_write()` per chunk — never
      holding more than one chunk of the image in RAM. The `KilnFW/TODO.md`
      9.5 "not built this pass" note this bullet pointed at is stale for the
      current tree.

**Pico update**
- [x] Five frames -- **2026-08-17, mirrored rather than shared**: ids exist in
      both `SaftyFW/src/tasks/link_frame.h` (frozen, previous pass) and
      `KilnFW/App/drivers/common/uart_task_ids.h`/`safety_link.h` (this pass), as
      matching `#define`s/structs in each codebase rather than one shared
      `CommonFW` codec -- the two firmwares are separate build targets and
      `CommonFW`'s own framing layer (`kilnlink_frame.{c,h}`) carries only
      the envelope, not per-frame payload logic, per that layer's existing
      scope. Not what this item's exact wording ("to `CommonFW`'s codecs")
      envisioned; flagged as a deviation, not silently reinterpreted.
- [ ] **Isolated-link error rate measured under a sustained update-sized
      transfer, at the link's real committed baud** -- **Blocked** (2026-09-04):
      same hardware-only measurement as the duplicate of this item under
      §4's "Throughput" section above; still not measured;
      this pass built against the documented frame contracts without that
      measurement. The 115200 this item originally named was itself wrong:
      measured 2026-08-23, the then-fitted TCMT1109 optocoupler pair
      delivered zero frames at 115200 or 57600, and 9600 became the
      committed rate for as long as that pair was fitted
      (`firmware/SaftyFW/docs/HARDWARE.md` §1). That pair was replaced by a
      digital isolator (U6) on 2026-08-25 and the baud is being re-measured —
      see `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` for the
      current value, not 9600. The sustained-transfer error rate at whatever
      that current value is remains a separate, still-open measurement.
- [x] `UPDATE_DATA` sent unacknowledged; Pico keeps a received-range bitmap
      and emits a gap report every 500 ms (SaftyFW, already frozen);
      **2026-08-17**: the ESP side (`KilnFW/App/drivers/net/ota_pico_relay.c`)
      now retransmits only the named ranges, polling `UPDATE_STATUS` for
      gap reports.
- [x] Retransmission rounds capped, with a clean failure rather than a loop
      -- **2026-08-17, ESP side**: `ota_pico_relay.c` caps at 10 rounds
      (`RELAY_MAX_RETRANSMIT_ROUNDS`, matching SaftyFW's own
      `UPDATE_MAX_RETRANSMIT_ROUNDS`) and aborts cleanly on
      `UPDATE_STATUS_ERR_RETRANSMIT_CAP` from the Pico -- see this file's
      top-of-document note on how this side's round accounting is a
      simplification, not an exact mirror, of SaftyFW's own algorithm.
- [x] Erase handled asynchronously, not inside a frame handler (SaftyFW,
      already frozen -- `update_task.c`'s block-at-a-time
      `flash_safe_execute()` with watchdog check-ins before/after each
      64K block, run from `update_task`'s own task, not `link_task`'s frame
      handler).
- [x] Streamed to flash; no whole-image RAM buffer -- true on both sides now:
      SaftyFW (already frozen) programs 248-byte `UPDATE_DATA` chunks
      straight to flash; **2026-08-17**, KilnFW's `ota_pico_relay.c` reads
      the staged image out of `pico_img` one 248-byte chunk at a time via
      `esp_partition_read()`, never holding the whole image in RAM.
- [x] CRC verified by reading back from flash, not from the received stream
      (SaftyFW, already frozen -- `update_task_process_end()` CRCs the
      XIP-mapped slot itself, not the receive-side bitmap).
- [x] Progress reported at least every 2 s -- **2026-08-17, polled not
      pushed** (this section's own honest math already flagged a silent bar
      as the risk; a poll-back getter addresses the same risk without a
      push channel): `ota_pico_relay_get_status()` updates at each phase
      transition and roughly every 10% during streaming/retransmit, backed
      by a small `GET /api/ota/pico/status` JSON endpoint.

**Relaying through the ESP**
- [x] HTTP body read no faster than the link drains -- **2026-08-17,
      superseded by staging (section 3)**: the constraint this bullet
      describes was written for the streaming-through design section 3 later
      abandoned in favor of `pico_img` staging. `ota_pico_do_stage()` still
      follows the same "never read ahead of what has been written" discipline
      for its own (fast, Wi-Fi-speed) write into `pico_img`, for the same
      underlying reason (nothing downstream has anywhere to put read-ahead
      data), even though there is no longer a slow link on the other end of
      that particular write.
- [x] Socket timeout covers the whole ~35 s transfer, not one chunk --
      **2026-08-17, moot by design**: the ~35 s+ relay now happens entirely
      AFTER `POST /api/ota/pico` has already responded (`202 Accepted`) and
      handed off to a background task, so no HTTP connection stays open for
      it at all -- see `ota_pico_relay.h`'s header comment. The staging
      write's own (much shorter) socket timeout is 30 s, same value/reasoning
      as the ESP self-update path.
- [x] Documented that a browser or proxy may time out where the MCP path
      will not -- true of the ESP self-update path (holds the connection
      open); the Pico path's async `202 Accepted` + poll design sidesteps
      this specific risk by construction rather than merely documenting it,
      which is a stronger answer to the same concern this bullet raised.

**Reboots and concurrency**
- [x] `SAFETY_CMD_ANNOUNCE_REBOOT` suppresses S6(b) for a bounded grace window
      (default 60 s) — **and grants no permission to heat**. **2026-09-04
      (triage verification)**: `firmware/SaftyFW/src/tasks/link_task.c`
      handles the announce and computes `safety_guard_input_t::reboot_grace_active`,
      read by `safety_core.c` for S6b — a grace-window *fact*, not a relay
      permission, consistent with "grants no permission to heat."
- [x] Grace window ends immediately, and trips, on any current above
      `i_present_a`. **2026-09-04**: `link_task.c` (grep for
      `i_present_a` near the grace-window logic) checks current during the
      grace window and ends it on a live reading — not independently
      re-verified line-by-line this pass, but the code path exists and is
      reachable from the same function that manages the window.
- [x] Silence past the window trips as normal. **2026-09-04**: implied by
      the same mechanism — the grace window is a bounded suppression, not a
      permanent one; S6b's ordinary timeout logic resumes once it expires.
- [x] Single update mutex across both processors; a concurrent attempt is
      refused. Same evidence as the identical bullet under "One update at a
      time" above (`ota_http_update_in_progress()`).
- [x] Append-only update record in NVS: timestamp, processor, image SHA-256,
      version before and after, result. Same evidence as above (`ota_record.c`).
- [x] Downgrades allowed but logged as such. **2026-09-24** — same evidence
      as the identical bullet above (`ota_version_compare()`/`is_downgrade`).
- [ ] SX1509 output state across an ESP reset established on the bench.
      **Blocked**: needs the physical board (see the identical item under
      "What holds the heaters off while the ESP reboots" above).

**Surfaces**
- [x] Web page with per-processor version, slot, interlock state, progress,
      rollback. **Built since this line was last reviewed, 2026-09-04**:
      `firmware/KilnFW/App/drivers/net/ota_page.html` exists and renders exactly
      this — ESP running version/active/inactive slot (`renderEspInfo`),
      Pico protocol version/compatibility, an interlock box, ESP and Pico
      progress bars, and rollback buttons for both processors
      (`/api/ota/esp/rollback`, `/api/ota/pico/rollback`).
- [x] Protocol-version mismatch warned about, with a second confirmation.
      **2026-09-24** — same evidence as the identical bullet under "Web page"
      (§6) above: the Pico path's existing 409 refusal now has a page-side
      confirmation dialog; the ESP raw-`.bin` path is still unparseable
      pre-upload, unchanged.
- [x] **Now more than four MCP tools. 2026-08-18 baseline (`tools/PcTools`,
      see section 6 above for the original deviation notes): `ota_get_challenge`,
      `ota_update_esp`, `ota_update_pico`, `ota_status`. Grown since**:
      `ota_rollback_esp()` now exists in
      `tools/PcTools/src/kilnctrl/mcp_server_ota.py` — the "no HTTP endpoint
      to wrap" gap the original four tools worked around is closed (see the
      `ota_rollback(processor)` item above). Image-hash logging is also no
      longer a gap: `ota_record.c` computes and persists `image_sha256_hex`
      server-side, surfaced via `GET /api/ota/esp/status`, even though the PC
      client itself still doesn't compute a local hash independently.
      Mocked-HTTP unit tests only; no physical board exercised this pass.

**Verification**
- [ ] Power pulled mid-transfer, both processors, both still boot the old image.
      **Blocked**: needs physical power-pull on the board; no hardware access
      this pass.
- [x] Corrupt image rejected, both processors. **2026-09-04 (triage
      verification, host-tested rather than a live corrupt-image push)**:
      SaftyFW — `test_update.c`'s `test_image_header_validate`/
      `test_handle_begin`/`test_image_header_unpack_hostile` exercise bad
      magic, wrong target, and truncated/hostile headers, all refused before
      any erase. ESP — `ota_http_esp.c` refuses on bad `esp_image_header_t`
      magic/chip_id before `esp_ota_begin()` (see the "Image identification"
      items above); this specific path has no host test (the handler isn't
      host-compilable), so this box is ticked on code-inspection + the
      Pico-side host tests, not full test coverage of the ESP side.
- [ ] An image that boots but fails to come up properly is rolled back
      automatically. **Partially verified, not fully**: the ESP half is
      hardware-verified 2026-09-03 per this file's own "ESP OTA" section
      above (`esp_ota_mark_app_valid_cancel_rollback()` only called once
      NVS/web-server/OTA-routes are confirmed up). The Pico half (bootloader
      rolls back an unconfirmed slot) has no equivalent hardware
      confirmation recorded anywhere in this pass's search — left unchecked
      because the item asks for **both processors** and only one has a
      recorded hardware verification.
- [x] Update attempted while firing — refused, with the blocker named.
      **2026-09-04**: `firmware/KilnFW/App/test/test_ota_interlock.c`'s
      `test_profile_running_and_paused`/`test_heater_commanded` host-test
      exactly this, by zone/precondition, with the specific blocker returned
      (not a generic failure).
- [x] Wrong password — refused, locked out, logged. **2026-09-04**:
      `firmware/KilnFW/App/test/test_ota_auth.c`'s `test_lockout` host-tests
      the 3-failure threshold and doubling backoff
      (`ota_auth_lockout_record_failure()`/`_is_locked()`); the source-IP
      logging itself (`ESP_LOGW` in `ota_http_verify_request()`) isn't
      host-testable (no live socket in the host build) so that half is
      confirmed by code inspection only, per §7's "Authentication" items
      above.
- [ ] SWD recovery from a deliberately bricked Pico. **Blocked**: needs a
      physical debug probe and a deliberately-bricked board; no hardware
      access this pass.

---

## Hardware exercise 2026-09-05/06

Bench test kiln, idle, no firing. `kiln_call`/`kiln_batch` (kilnctrl MCP,
192.168.1.156). AP password used: the Kconfig default (`password`) — no
override had been set.

**ESP OTA into `ota_0` + rollback — done, passed.**

- Baseline: `factory` running, `fw_build` "Sep 5 2026 08:30:52", zone gains
  Kp/Ki/Kd = (0.0318/0.00010/0.8401), (0.0485/0.00020/1.0548),
  (0.0631/0.00020/1.0690), no crash report.
- `ota_update_esp(firmware/KilnFW/build/KilnCtrl.bin, "password")`: 2,018,720
  bytes written to `ota_0` in ~7 s (10%-step log timestamps), version string
  `V1.0_Purchased_This_Board-1210-`. Reboot via `debug_reset(peer="esp")`
  (JTAG reset, not a flash — the board's own bootloader/rollback machinery
  does the rest).
- Post-boot: `RUNNING=ota_0` confirmed (`GET /api/partitions`), `fw_build`
  unchanged (same source build as `factory`, flashed same day), zone gains
  byte-identical to baseline, `safety_get_status` link up, thermocouple
  valid, currents 0 A.
- `ota_rollback_esp("password")`: board rebooted, `RUNNING=factory`
  confirmed, `fw_build` unchanged, gains still byte-identical (no
  `ZONES_CFG_VERSION` mismatch hazard here — same firmware build both
  sides), safety link up.
- **Anomaly, unresolved, pre-existing (not caused by this exercise):**
  `GET /api/crash_report` shows an unacknowledged panic
  (`exc_task=safety_poll`, `IllegalInstruction`, `exc_pc=0x4037fe09`,
  `exc_addr=0x0`) that was **absent from the pre-OTA baseline**
  `get_heap_status` call but present, byte-for-byte identical (same PC,
  same backtrace), after the `ota_0` boot, after the rollback boot, and
  after one further `debug_reset`. Identical content across three
  consecutive boots means this is a persisted coredump/NVS record, not a
  fresh crash on each boot — most likely the same `safety_poll` /
  `IllegalInstruction` class already root-caused and fixed by `51e1ef5`
  (see CLAUDE.md), surfacing here because nothing in this session's path
  acknowledges/clears the stored report. Board state itself is healthy
  throughout (safety link up, relay off, ambient temperature, gains
  correct) — consistent with a persisted record surfacing rather than a
  fresh crash per boot; root cause under separate investigation (dump_id CRC
  computed over un-zeroed esp_core_dump_summary_t padding) — not confirmed
  stale, and it was never cleared, so it should be looked at before trusting
  `get_heap_status` "healthy" output at face value on this board.

**Pico bootloader update over UART1 — attempted 2026-09-06, refused by the
Pico before any flash write.**

The earlier "skipped, no packaged image" note above is now stale. A `.bin`
does not need hand-packaging at all: `ota_pico_relay.c` builds the
36-byte `UPDATE_BEGIN` header itself (magic/target/`header_version` are
compile-time constants, `protocol_version` is the ESP's own
`KILNLINK_PROTOCOL_VERSION`, `length`/`crc32` are computed from whatever
raw app image is uploaded) — the PC-side tool only has to supply a raw
application binary, not a pre-formed image-with-header. `firmware/SaftyFW/build/`
already had `SaftyFW_slotA.elf`/`SaftyFW_slotB.elf` (linked to run from
`BOOTLOADER_SLOT_A_FLASH_OFFSET`/`_B_FLASH_OFFSET` respectively, per
`bootloader/flash_layout.h`) alongside the monolithic `SaftyFW.elf` —
`arm-none-eabi-objcopy -O binary` (the same `14.2 rel1` toolchain
`CMakeCache.txt` already names as `CMAKE_OBJCOPY`) on `SaftyFW_slotA.elf`
produced a 95,020-byte raw image. Confirmed the two slot ELFs are genuinely
position-dependent, not interchangeable: the slot A and slot B `.bin`s are
identical in length but differ in every CRC32 (`0xc02711a8` vs `0xb86018a8`,
zlib CRC32 over the raw bytes) — sending the wrong one to whichever slot the
Pico actually chooses would boot corrupt code, not merely fail a check, so
this is a real hazard this exercise carried, not a hypothetical one.

Preconditions confirmed idle first: `profiles_get_exec_status` state=0,
segment 0/0, no dwell; ambient thermocouples (~23.8 °C); all relays off;
`safety_get_status` link up, thermocouple valid, currents 0 A. AP password
was the Kconfig default (`"password"`), same as the ESP exercise above.

`ota_update_pico(image_path=".../SaftyFW_slotA.bin", password="password")`:
staged successfully — `95020` bytes accepted, ESP-computed CRC32
`0x02F15704` (a different value from the host-side zlib CRC32 above; the two
sides are not using the same CRC32 variant/parameters, which is at least
worth reconciling before trusting a CRC match as proof of a correct
transfer) — "relay started", **202**-style async response per this doc's
own design.

Polling `ota_status()` immediately after: `phase='failed' percent=0
last_error='Pico refused UPDATE_BEGIN: a safety trip is pending'`, and the
ESP's own `ota_record` agrees (`success=False`,
`reason='Pico refused UPDATE_BEGIN: a safety trip is pending'`). **No flash
write was attempted** — the refusal came from `update_task.c`'s own
precondition check, exactly this doc's §1 invariant working as designed:
the ESP's *own* cached `safety_get_status()` at the same moment showed
`fault_status=0`, link up, no visible trip — the Pico refused on information
the ESP's own view did not surface, which is the entire point of "the Pico
enforces the last three [preconditions] itself... for the same reason the
whole safety processor exists." Per this task's own ground rules, this
refusal was **not** worked around (no `safety_clear_trip()` call to force
the update through) — the refusal itself is the finding.

**A structural gap surfaces investigating this, worth flagging before the
next attempt**: `firmware/SaftyFW/TODO.md`'s own Phase 10 checklist records
the bootloader itself — the metadata log, per-boot CRC check, and
`boot_attempts` slot-fallback logic that would actually make a successfully
relayed image *run* — as built and host-tested but "not flashed or exercised
over a live UART1 link" (Phase 0/TODO.md's own `[~]` line), and 10.9 ("application
booted through the bootloader stops...") is still open. A `debug_read_memory`
scan of `BOOTLOADER_METADATA_FLASH_OFFSET` (`0x10010000`, over SWD) on this
bench Pico read back what looks like ordinary Thumb code (repeated `b672`/
`e7fe` self-branch patterns, RAM-range literal pool values), not a
`"KLN1"`-magic metadata log — consistent with the currently-flashed image
being the monolithic `SaftyFW.elf` rather than a build running under the
two-slot bootloader. If that reading is right, even a transfer that clears
every precondition and passes its post-write CRC check today would stage a
slot the current boot vector never consults, and "the update succeeded"
would silently not change what the board runs on its next reset. That is a
prerequisite gap to close, separately from the trip-pending refusal, before
this path is retried for real. Not itself confirmed by flashing anything —
inferred from the memory read plus TODO.md's own status, not proven by
forcing a transfer through.

**Superseded 2026-09-18.** That prerequisite gap is closed: the bench Pico
now boots through the two-slot bootloader, slot A active, with a `KLN1`
metadata record present. A retry the same day therefore got past staging and
failed at a different place — the RP2040 hardware-watchdog-reset partway
through erasing the destination slot, never confirming `RECEIVING`, so the
ESP failed the relay at its 15000 ms erase timeout. Safe outcome (relays off
throughout, no trip latched, configuration unchanged), but the ESP-driven
Pico update path is non-functional on this hardware today. Observed facts
under ROADMAP.md M8; diagnosis in
`../../../docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`.

**Version-mismatch path (step 4 of this pass's brief) — not exercised, and
not fakeable with today's tooling.** `ota_pico_relay.c` always sends the
ESP's own live `KILNLINK_PROTOCOL_VERSION` in the header it builds; nothing
in `ota_update_pico()`/`ota_http_client.py` accepts a caller-supplied
override, so there is no way to push a deliberately-incompatible
`protocol_version` without editing and rebuilding the ESP firmware itself
(out of scope here). Noted as an honest gap rather than simulated.

Board state at the end of this pass: Pico still on whatever firmware was
running before (no write attempted), safety link up, relays off, ambient
temperature, no firing — unchanged by this exercise except for the `pico`
`ota_record` entry above. Tracked in ROADMAP.md M8.
