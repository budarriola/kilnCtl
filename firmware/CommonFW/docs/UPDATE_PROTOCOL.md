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

**ESP OTA**
- [ ] New partition table with `otadata` + two app slots, first three entries unmoved
- [ ] Offsets confirmed against the live table before the one-time serial flash
- [ ] Pre-change table archived for rollback
- [ ] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
- [ ] `esp_ota_mark_app_valid_cancel_rollback()` called only after NVS, safety link
      and web server are all confirmed up — never at the end of `app_main()`
- [ ] Streamed `esp_ota_ops` POST handler, no whole-image buffering

**Pico update**
- [ ] Five frames added to `LINK_PROTOCOL.md` and to `CommonFW`'s codecs
- [ ] **Isolated-link error rate measured at 115200 before this is built**
- [ ] Erase handled asynchronously, not inside a frame handler
- [ ] Streamed to flash; no whole-image RAM buffer
- [ ] CRC verified by reading back from flash, not from the received stream
- [ ] Progress reported at least every 2 s

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
