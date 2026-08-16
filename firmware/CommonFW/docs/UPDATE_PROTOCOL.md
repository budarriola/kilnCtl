# Firmware Update Protocol — both processors, one password

> **Status:** planning, nothing built · **Last reviewed:** 2026-08-16
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

Worse, `partitions.csv` carries a long comment explaining why the first three
entries must not move: live NVS data sits at `0x9000`, and the `wifi_nvs` /
`kiln_nvs` / `profiles_nvs` split above the app was done specifically so a
config wipe cannot strand the board off Wi-Fi. **Those must keep their current
offsets.** Only the app region between `0x10000` and `0x187000` may be re-carved.

The space available for app slots is `0x177000` = 1500 KB. The current image is
about 301 KB, roughly 20 % of one slot. A workable split:

```
otadata   data, ota,   0x010000, 0x002000     8K
ota_0     app,  ota_0, 0x012000, 0x0B0000   704K
ota_1     app,  ota_1, 0x0C2000, 0x0B0000   704K
                                            (84K spare below wifi_nvs at 0x187000)
```

- [ ] **Confirm these offsets against the real table before flashing anything.**
      Getting this wrong erases the profiles partition.
- [ ] Keep a copy of the pre-change table so a rollback to pre-OTA firmware is
      possible over serial.
- [ ] 704 KB is 2.3× the current image. Check the headroom against what
      sections 6A and 8 of `KilnFW/TODO.md` are still going to add.

### Rollback is not optional

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is currently **off**. With it on, a new
image boots as `PENDING_VERIFY` and the bootloader reverts to the previous slot
unless the app calls `esp_ota_mark_app_valid_cancel_rollback()`.

Do not call it at the end of `app_main()`. Call it only once the things that
matter have actually come up: NVS partitions readable, safety link exchanging
frames, web server answering. An image that boots but cannot talk to the safety
processor is exactly the image that must be rolled back automatically, and it
would pass a naive "we reached the end of main" check.

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
- [ ] Decide whether to accept the image into a temporary file in a
      `profiles_nvs`-sized spare partition instead, trading flash wear and a
      layout change for a fast upload followed by a slow, resumable relay.
      Streaming is simpler and is the recommendation; this is the fallback if
      browser timeouts prove unworkable.

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

- [ ] `ota_status()` — both processors: versions, slots, interlock state, and
      *why* an update is currently refused
- [ ] `ota_update_esp(image_path, password)` — streams, returns the new version
- [ ] `ota_update_pico(image_path, password)` — same, over the link
- [ ] `ota_rollback(processor)` — explicit, and refused under the same interlocks
- [ ] Every one of these logged, with the image hash

---

## 7. Completion checklist

**Preconditions and interlocks**
- [ ] Interlock table above implemented on the ESP, each refusal naming its blocker
- [ ] Pico independently enforces relay-open, no-trip-pending, and the temperature ceiling
- [ ] GUI distinguishes "updating" from "not responding" **without** weakening the relay block
- [ ] Temperature ceiling configurable, default 100 °C

**Authentication**
- [ ] Nonce endpoint: 16 random bytes, single use, 30 s expiry
- [ ] `HMAC-SHA256(HMAC-SHA256(ap_password, "kilnctl-ota-v1"), nonce || context)`
- [ ] Constant-time comparison; nonce invalidated on both paths
- [ ] Lockout after 3 failures, doubling to 15 minutes, every attempt logged
- [ ] Documented: this does not defend against someone who knows the AP password

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
- [ ] New partition table with `otadata` + two app slots, first three entries unmoved
- [ ] Offsets confirmed against the live table before the one-time serial flash
- [ ] Pre-change table archived for rollback
- [ ] **`nvs`, `wifi_nvs`, `kiln_nvs` and `profiles_nvs` read out with esptool and
      saved before the table is flashed.** The one irreversible step in this whole
      plan is writing a wrong partition table over live config
- [ ] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
- [ ] `esp_ota_mark_app_valid_cancel_rollback()` called only after NVS, safety link
      and web server are all confirmed up — never at the end of `app_main()`
- [ ] Streamed `esp_ota_ops` POST handler, no whole-image buffering

**Pico update**
- [ ] Five frames added to `LINK_PROTOCOL.md` and to `CommonFW`'s codecs
- [ ] **Isolated-link error rate measured at 115200 before this is built**
- [ ] `UPDATE_DATA` sent unacknowledged; Pico keeps a received-range bitmap and
      emits a gap report every 500 ms; ESP retransmits only the named ranges
- [ ] Retransmission rounds capped, with a clean failure rather than a loop
- [ ] Erase handled asynchronously, not inside a frame handler
- [ ] Streamed to flash; no whole-image RAM buffer
- [ ] CRC verified by reading back from flash, not from the received stream
- [ ] Progress reported at least every 2 s

**Relaying through the ESP**
- [ ] HTTP body read no faster than the link drains — TCP backpressure, no
      read-ahead buffer with nowhere to go
- [ ] Socket timeout covers the whole ~35 s transfer, not one chunk
- [ ] Documented that a browser or proxy may time out where the MCP path will not

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
- [ ] Four MCP tools, each logging the image hash

**Verification**
- [ ] Power pulled mid-transfer, both processors, both still boot the old image
- [ ] Corrupt image rejected, both processors
- [ ] An image that boots but fails to come up properly is rolled back automatically
- [ ] Update attempted while firing — refused, with the blocker named
- [ ] Wrong password — refused, locked out, logged
- [ ] SWD recovery from a deliberately bricked Pico
