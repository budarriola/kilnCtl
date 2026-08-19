# Firmware Update Protocol — both processors, one password

> **Status:** planning, mostly nothing built — **exceptions:** section 3's ESP
> partition-table + rollback foundation landed 2026-08-17 (host-build-verified,
> not yet flashed to physical hardware), and section 4's ESP-side sender half
> (KilnFW: `App/drivers/ota_pico_relay.{h,c}`, `safety_link.c`'s
> `UPDATE_STATUS` handling, `POST /api/ota/pico`) landed the same day —
> host-build-verified only, RP2040 receive side (SaftyFW) was already frozen
> before this pass and untouched by it. Sections 2, 5, 6 are still planning
> only. · **Last reviewed:** 2026-08-17
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
>    poll-back endpoint. See `firmware/KilnFW/App/drivers/ota_http.h`'s
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
- **RP2040 (`SaftyFW`)** — over the opto-isolated UART, relayed by the ESP.
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

**Does not:** anyone who already knows the AP password — by design, that is the
credential. Nor anyone who has compromised the ESP itself, because *the ESP is
the only thing authorising the Pico update*. If the ESP is owned, the safety
processor can be reflashed with whatever the attacker likes.

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
ota_0         app,  ota_0,    0x210000, 0x200000  2048K   <- 1.75x the measured 1167K image
ota_1         app,  ota_1,    0x410000, 0x200000  2048K
pico_img      data, undefined,0x610000, 0x0E0000   896K   <- staging, see below (corrected
#                                                            up from 512K, see note)
# spare                       0x6F0000..0x1000000 ~9.29M  (16 MB part; still comfortably
#                                                           under the 8 MB floor too)
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
- [ ] **Reflash the bootloader** — the flash size lives in the bootloader
      header, so a new table alone is not enough. Still outstanding: this needs
      the physical board over serial, which this pass did not have.
- [x] Confirm the offsets against the real table before flashing. Host-build
      verified 2026-08-17: `gen_esp32part.py`/`check_sizes.py` reports no
      overlap/overflow, and the six pre-existing entries are byte-identical to
      before (diffed, not just eyeballed). **Not yet confirmed against the
      physical board** — that is still a one-time serial step.
- [ ] Archive the pre-change table, and read out all four NVS partitions with
      `esptool read_flash` first. This is the one irreversible step in the plan,
      and this pass has no hardware access to perform it.

#### A staging partition also solves the relay problem

512 KB of `pico_img` is enough to hold a safety-processor image, which removes
the constraint described in §4 that the ESP must stream the Pico's image at link
speed because it has nowhere to put it. With staging, the browser upload runs at
Wi-Fi speed and finishes in a second, and the slow relay over the isolated link
happens afterwards — resumable, restartable, and immune to an HTTP timeout.

- [ ] Decide between streaming and staging once the 8 MB table exists. Staging
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

At 115200 baud a full 253-byte frame is roughly 260 bytes on the wire before
byte-stuffing, about 23 ms, and the protocol is stop-and-wait with a 200 ms ACK
timeout. A 200 KB image is about 830 frames — call it 35 s at 40 ms per
round trip.

That is fine. What is **not** fine is the retry behaviour: `UART_PROTO_MAX_RETRIES`
is 10 at a 200 ms timeout, so a single persistently-failing frame costs 2 s, and
a link that is dropping 5 % of frames turns a 35 s update into minutes. Before
building this:

- [ ] **Measure the real error rate of the isolated link at 115200** over a
      sustained multi-megabyte transfer. The TCMT1109 optocouplers are the
      bandwidth limit and nobody has characterised them yet.
- [ ] Decide whether to raise the baud rate for the duration of an update, and
      whether the optocouplers can take it. A negotiated rate in `UPDATE_BEGIN`
      with an automatic fallback is the flexible option.
- [ ] Report progress to the GUI at least every 2 s. A silent 35-second bar is
      indistinguishable from a hang.

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
- [ ] **Superseded if the 8 MB table lands:** a 512 KB `pico_img` staging
      partition removes this constraint entirely — fast upload, then a slow
      resumable relay that no HTTP timeout can interrupt. See §3. Streaming
      remains the fallback if the flash-size change is deferred.

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

- [ ] Cap total retransmission rounds, and fail cleanly rather than looping if a
      range never lands. A link that cannot deliver the same 248 bytes after ten
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

- [ ] A single update mutex covering both processors. A second browser tab, or
      an agent racing a human, must be refused rather than interleaved.
- [ ] An append-only update record in NVS: timestamp, processor, image SHA-256,
      version before and after, result. For a device that can start a fire,
      "which firmware was running when that happened" should not depend on
      someone remembering.
- [ ] A downgrade is allowed but logged as such. Blocking it would eventually
      block a legitimate rollback during debugging.

### What holds the heaters off while the ESP reboots

- [ ] **Establish what the SX1509's outputs do across an ESP reset.** Its
      `~RESET` is driven by the ESP; if the expander is not reset and its output
      register is non-volatile across the ESP's reboot, relays could stay
      energised through the update. The interlocks require an idle kiln so
      nothing should be on — but "should be" is not the standard that applies to
      the thing that energises heaters, and this is a five-minute bench check.

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

- [ ] Show the interlock state **before** the user picks a file, with the
      specific blocker named.
- [ ] Refuse to start if the other processor is mid-update.
- [ ] Warn, and require a second confirmation, when the uploaded image's
      protocol version differs from the running one — that is the case where a
      successful update leaves the two processors unable to talk.

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
- [ ] `ota_rollback(processor)` — **not built**, see the deviation note above:
      no HTTP endpoint exists for this to wrap. Would need a new
      `ota_http.c` route first.
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
- [ ] Interlock table above implemented on the ESP, each refusal naming its blocker
- [ ] Pico independently enforces relay-open, no-trip-pending, and the temperature ceiling
- [ ] GUI distinguishes "updating" from "not responding" **without** weakening the relay block
- [ ] Temperature ceiling configurable, default 100 °C

**Authentication**
- [x] Nonce endpoint: 16 random bytes, single use, 30 s expiry. **2026-08-17**:
      `GET /api/ota/challenge` (`firmware/KilnFW/App/drivers/ota_http.c`),
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
- [ ] Documented: this does not defend against someone who knows the AP
      password. Still just prose in this file (section 2, "What this does
      and does not defend against") -- no code checkbox to earn here, but
      leaving unchecked since nothing new was added to say so anywhere a
      user would see it (e.g. the eventual OTA web page, not yet built).

**Built as of 2026-08-17**: both `POST /api/ota/esp` and `POST /api/ota/pico`
now exist -- `ota_http_verify_request()` has real callers. See the "ESP OTA"
and "Pico update" sections below for what each actually covers.

**Version compatibility** (`LINK_PROTOCOL.md`, `ANNOUNCE_VERSION`)
- [ ] `ANNOUNCE_VERSION` = `0x0F` implemented: the ESP announces itself, unprompted
- [ ] `min_compatible` field added to both version frames at a fixed offset
- [ ] Both sides check **both** directions of `peer.protocol >= self.min_compatible`
- [ ] ESP on mismatch: link fault, heating blocked, GUI names both versions and
      which to update
- [ ] Pico on mismatch: `DEGRADED_NO_CONTEXT`, **no trip latched**, context frames
      discarded unparsed, context-free guards still running and still commanding
      the relay, context-dependent guards reported as disabled
- [ ] Compatibility floor frozen: framing, `ANNOUNCE_VERSION`, `FW_VERSION` and
      the `UPDATE_*` frames work regardless of version, ids `0x00`–`0x0F` reserved
- [ ] Floor layouts may only be **appended** to, never reordered or resized
- [ ] Re-checked on every reconnect and every `boot_id` change, not once at boot
- [ ] ESP refuses to push a Pico image it could not then talk to, unless
      explicitly overridden
- [ ] GUI states the order — ESP first — when both need updating

**Image identification**
- [ ] Pico image header: magic, target, header version, protocol version,
      `min_compatible`, length, CRC32 — all validated **before the first erase**
- [ ] ESP image magic and chip ID checked before `esp_ota_begin()`

**ESP OTA**
- [x] **Physical flash size confirmed** — done 2026-08-17 via the LonelyBinary
      product page for the board in hand (N16R8, 16 MB), not `esptool flash_id`
      directly. Buy-list/3D-model records are still stale and separately tracked.
- [x] `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` set (2026-08-17). **Bootloader reflash
      still outstanding** — that is a one-time serial step against physical
      hardware this pass did not have access to; leave unchecked.
- [x] New partitions placed entirely above `0x200000`, so **nothing existing
      moves** — implemented in `firmware/KilnFW/partitions.csv` 2026-08-17,
      diffed to confirm the six pre-existing entries are byte-identical
- [x] `factory` retained as the serial-free recovery image
- [x] Slot size checked against a **measured** image, not a remembered one. It was
      1167 KB on 2026-08-16, not the 301 KB this plan was first written around —
      `ota_0`/`ota_1` are 2048K each, 1.75x headroom
- [x] Offsets confirmed against the live table — **host-build-verified only**
      (`gen_esp32part.py`/`check_sizes.py` reports no overlap/overflow). The
      one-time serial flash against the physical board has NOT happened.
- [ ] Pre-change table archived for rollback — nothing to archive yet, since no
      physical flash has occurred
- [ ] **`nvs`, `wifi_nvs`, `kiln_nvs` and `profiles_nvs` read out with esptool and
      saved before the table is flashed.** The one irreversible step in this whole
      plan is writing a wrong partition table over live config. This pass has no
      hardware access and could not perform it — stays unchecked.
- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` — set 2026-08-17, confirmed by a
      clean `idf.py build`
- [x] `esp_ota_mark_app_valid_cancel_rollback()` called only after NVS, safety link
      and web server are all confirmed up — never at the end of `app_main()`.
      Implemented as a background task in `App/main.c`
      (`ota_rollback_confirm_task()`); all three preconditions are genuinely
      wired in, none is a placeholder
- [ ] Streamed `esp_ota_ops` POST handler, no whole-image buffering — **not
      built this pass**, deliberately out of scope (see `KilnFW/TODO.md` 9.5)

**Pico update**
- [x] Five frames -- **2026-08-17, mirrored rather than shared**: ids exist in
      both `SaftyFW/src/tasks/link_frame.h` (frozen, previous pass) and
      `KilnFW/App/drivers/uart_task_ids.h`/`safety_link.h` (this pass), as
      matching `#define`s/structs in each codebase rather than one shared
      `CommonFW` codec -- the two firmwares are separate build targets and
      `CommonFW`'s own framing layer (`kilnlink_frame.{c,h}`) carries only
      the envelope, not per-frame payload logic, per that layer's existing
      scope. Not what this item's exact wording ("to `CommonFW`'s codecs")
      envisioned; flagged as a deviation, not silently reinterpreted.
- [ ] **Isolated-link error rate measured at 115200 before this is built** --
      still not measured; this pass built against the documented frame
      contracts without that measurement, same gap this item already named.
- [x] `UPDATE_DATA` sent unacknowledged; Pico keeps a received-range bitmap
      and emits a gap report every 500 ms (SaftyFW, already frozen);
      **2026-08-17**: the ESP side (`KilnFW/App/drivers/ota_pico_relay.c`)
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
- [ ] `SAFETY_CMD_ANNOUNCE_REBOOT` suppresses S6(b) for a bounded grace window
      (default 60 s) — **and grants no permission to heat**
- [ ] Grace window ends immediately, and trips, on any current above `i_present_a`
- [ ] Silence past the window trips as normal
- [ ] Single update mutex across both processors; a concurrent attempt is refused
- [ ] Append-only update record in NVS: timestamp, processor, image SHA-256,
      version before and after, result
- [ ] Downgrades allowed but logged as such
- [ ] SX1509 output state across an ESP reset established on the bench

**Surfaces**
- [ ] Web page with per-processor version, slot, interlock state, progress, rollback
- [ ] Protocol-version mismatch warned about, with a second confirmation
- [x] **Four MCP tools — 2026-08-18, `tools/PcTools`, see section 6 above for
      the full deviation notes**: `ota_get_challenge`, `ota_update_esp`,
      `ota_update_pico`, `ota_status` (not `ota_rollback` — no HTTP endpoint
      exists to wrap; not image-hash-logging — no local SHA-256 computed this
      pass). Mocked-HTTP unit tests only; no physical board exercised.

**Verification**
- [ ] Power pulled mid-transfer, both processors, both still boot the old image
- [ ] Corrupt image rejected, both processors
- [ ] An image that boots but fails to come up properly is rolled back automatically
- [ ] Update attempted while firing — refused, with the blocker named
- [ ] Wrong password — refused, locked out, logged
- [ ] SWD recovery from a deliberately bricked Pico
