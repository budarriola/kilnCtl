# Wi-Fi provisioning — modes, AP fallback, and resilience guarantees

`App/drivers/wifi_prov.{c,h}` brings up Wi-Fi, runs the station/AP state
machine, and persists both the saved home network and the board's own access
point identity. `App/drivers/wifi_provision_http.{c,h}` is the HTTP face of
it (and, incidentally, the owner of the httpd instance every other page on
this board hangs off — see [`docs/WEB_UI.md`](WEB_UI.md)).

Wi-Fi is the **third** client of this board, alongside the UART PC link and
the opto-isolated safety-processor link — not a replacement for either.
TODO.md section 1 is the authoritative design doc and change log; this file
is the "what does it actually do" companion.

**HARDWARE STATUS**: this is one of the few subsystems on this board that
*has* run on real silicon. Per `docs/PROJECT_STATUS.md`, Wi-Fi provisioning —
AP fallback, station join, and mode persistence across a reflash — was
verified live on the bench ESP32-S3 as of 2026-08-10/11, and the
oversized-body / missing-field POST hardening was verified to return a clean
400 with the server still responsive afterward. The 2026-08-11 replacement of
the old `local_only` flag with the explicit home/AP toggle was verified the
same way. What has **not** been exercised is the legacy-NVS migration path
against a genuinely old blob written by pre-2026-08-11 firmware, and nothing
here has been tested at range, under interference, or over a long firing.

The 2026-08-12 move of the credentials into their own flash partition was
checked on the same bench unit, in both directions. The first boot after the
new table was flashed took the **adoption** path — it logged `migrating Wi-Fi
config from the default NVS partition to 'wifi_nvs' (ssid 'ATTFqf9g79', mode
home)` and joined the network on the first attempt. The board was then
reflashed (bootloader + partition table + app) and came back up joining the
same network with **no** migration line, i.e. reading the credentials straight
out of `wifi_nvs` — which is what "a reflash does not deprovision the board"
means in practice.

That first boot also settled a question this document had wrong: the board had
been reporting `no saved credentials: AP 'kilnCtl' for first-boot
provisioning` and falling back to its AP, yet the migration read the *same*
default-partition namespace minutes later and found working credentials. The
two read paths disagree about what counts as provisioned — see
[What's still open](#whats-still-open). Both boot logs that day also carried an
`esp_wifi_set_config(AP)` error that is **not** yet explained; see
[What's still open](#whats-still-open).

## Where the credentials live: a flash partition of their own

Since 2026-08-12 the Wi-Fi settings are **not** in the default `nvs`
partition. They are in `wifi_nvs`, a second NVS partition declared by
`partitions.csv` at the KilnFW root and selected by `sdkconfig`
(`CONFIG_PARTITION_TABLE_CUSTOM=y`,
`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`,
`CONFIG_PARTITION_TABLE_OFFSET=0x8000`, `CONFIG_ESPTOOLPY_FLASHSIZE="16MB"` as
of 2026-08-17 — the partition *table* itself is unaffected and still only maps
the first 2 MB of flash; see `partitions.csv`'s header).

| Name | Type/SubType | Offset | Size | Holds |
|---|---|---|---|---|
| `nvs` | data / nvs | `0x9000` | `0x6000` (24K) | zones, rules, profiles, run-state, relay cycles |
| `phy_init` | data / phy | `0xf000` | `0x1000` (4K) | RF calibration |
| `legacy_app` | data / undefined | `0x10000` | `0x177000` (1500K) | the hole `factory` left behind when it moved (2026-08-21); declared so nothing re-allocates it |
| `factory` | app / factory | `0x810000` | `0x300000` (3072K) | the application image — **moved 2026-08-21**, see `partitions.csv` |
| `wifi_nvs` | data / nvs | `0x187000` | `0x6000` (24K) | Wi-Fi credentials, mode, AP identity — **nothing else** |

The first two rows (and the `nvs`/`phy_init` offsets) are **byte-identical to ESP-IDF's stock
`partitions_singleapp_large.csv`**, deliberately: this board had already been
flashed with the stock layout and had live data sitting in `nvs`, so moving or
resizing any of those three would have silently invalidated every stored blob
the moment the new table was written. The only change is the fourth row,
appended into flash above the app image that the stock table left unused
(`0x18D000`..`0x200000`, 460K, is still free). 24K is the same size as the
default `nvs` — far more than the handful of strings and u8s stored here, but
the space was free, and the spare sectors mean the compaction that produces
`NO_FREE_PAGES` essentially never has to run.

### Why a partition and not just another namespace

Because **NVS corruption recovery is partition-wide, not namespace-wide.**
`ESP_ERR_NVS_NO_FREE_PAGES` and `ESP_ERR_NVS_NEW_VERSION_FOUND` have exactly
one cure — erase the whole partition — and every version of `wifi_prov.c`
before 2026-08-12 answered that condition with a blanket `nvs_flash_erase()`.
That took zones, rules, profiles, the run-state breadcrumb *and* the
credentials together. A corrupt zone-config blob therefore knocked the board
off the network at precisely the moment the operator needed the network to go
fix it, and the board's only remaining face was the fallback AP with whatever
identity the defaults gave it.

`nvs_partition_init()` now takes a partition name, and both partitions are
brought up through it separately: `nvs_flash_erase_partition()` replaces
`nvs_flash_erase()`, so a wipe of one can never reach the other in either
direction. The two failure paths are both non-fatal and both say what was
lost:

- default `nvs` unmountable → logged as `zones/rules/profiles/run_state will
  not persist`, and Wi-Fi still comes up. This is the whole point of the
  split: the board stays reachable while its kiln config is broken.
- `wifi_nvs` unmountable → logged as `Wi-Fi credentials cannot persist`, and
  the code falls through rather than returning, leaving `s_wifi` zeroed
  (unprovisioned/home) so the AP still starts and the board can be
  reprovisioned.

It also makes "reset the kiln's configuration" — erase `nvs` — a survivable
operation instead of one that strands the board.

`wifi_prov_start()` is still the de-facto owner of **default**-partition NVS
bring-up for the whole firmware: `zones_http`, `rules_http`, `profiles_http`,
`run_state` and `relay_cycles` all call `nvs_open()` without ever initializing
the partition themselves, and `app_main` calls `wifi_prov_start()` before any
of them. The split did not change that; it only gave the two partitions
separate recovery paths.

### One-time migration out of the default partition

A board provisioned by pre-split firmware has its `wifi_cfg` namespace in the
default `nvs`. `migrate_from_default_partition()` handles that on the first
boot of the new firmware:

- `nvs_load_from()` is parameterized on the partition precisely so the same
  reader — legacy-key handling and all — can be pointed at the old copy
  without duplicating any of it. It reports whether the namespace *existed*,
  which is distinct from "existed but `has_creds == 0`".
- Because that reader writes straight into `s_wifi`, the migration snapshots
  the whole struct first, so a speculative read of an empty old namespace
  cannot clobber a good new-partition config.
- The old copy wins only when the new home has nothing at all (the true
  first-boot-after-the-split case), or when the new home exists but has no
  credentials while the old one does. Otherwise the new location wins
  outright — it is the source of truth from the moment it holds credentials.
- Everything is written through, not just the credentials: mode and both AP
  identity overrides too, since they are part of the same persisted config and
  would otherwise silently revert once this function stops adopting the old
  copy. A failed migration write is logged and retried next boot; `s_wifi` is
  correct for the current boot either way.

**The old copy is never deleted.** Someone rolling back to pre-split firmware
to chase a regression should still find a board that can join its network. The
cost is a stale duplicate this build never reads again after the migration and
never writes at all — cheap, and strictly safer than the alternative. Every
writer (`nvs_save_creds`, `nvs_save_mode`, `nvs_save_ap_ssid`,
`nvs_save_ap_password`) targets `wifi_nvs` unconditionally.

The migration is logged: `migrating Wi-Fi config from the default NVS
partition to 'wifi_nvs' (ssid '...', mode ...)`. Its **absence** on a reflash
is the positive signal that the credentials were read from `wifi_nvs`
directly — that is what was observed on the bench on 2026-08-12.

### Survival matrix — three cases, stated honestly

| Operation | Wi-Fi credentials | Kiln config (`nvs`) |
|---|---|---|
| `program_esp` / normal reflash of bootloader + partition table + app | **survive** | survive |
| erase the default `nvs` partition ("reset the kiln config") | **survive** | destroyed |
| `esptool erase_flash` (full chip) | **destroyed** | destroyed |

The first row is the common case and the reason the first three
`partitions.csv` rows are frozen; it is the one verified on hardware
(2026-08-12: reflashed, board rejoined the saved network from `wifi_nvs`, no
migration line in the log).

The third row is not a limitation of this layout and no layout can change it.
`esptool erase_flash` erases the entire chip — bootloader, partition table,
and every partition including `wifi_nvs`. **Everything is destroyed**, and a
board erased that way comes back unprovisioned and has to be set up from the
fallback AP again. Do not read the first two rows as a guarantee against the
third.

### The first flash after this change must include the partition table

The new table lives at `CONFIG_PARTITION_TABLE_OFFSET` = **0x8000**. Flashing
only the app leaves the *old* (stock) table on the chip, which has no
`wifi_nvs` entry at all — `nvs_open_from_partition()` then returns
`ESP_ERR_NVS_PART_NOT_FOUND`. That is handled rather than fatal
(`nvs_load_from()` treats it the same as "nothing saved yet"), so the symptom
is not a crash: the board simply comes up **unprovisioned**, with no
credentials and no migration, every boot. Flash `partition-table.bin` at
0x8000 along with the bootloader and app.

## Mode: one explicit choice, not three overlapping flags

`wifi_prov_mode_t` (2026-08-11 redesign):

| Mode | Meaning |
|---|---|
| `WIFI_PROV_MODE_HOME` (0) | try to join the saved home network; the board's own AP is a fallback, not a destination |
| `WIFI_PROV_MODE_AP` (1) | the board's own AP only, permanently, by explicit user choice. Never attempts a station join, never prompts for one |

This replaced a boolean `local_only` flag. The rename matters because the
concept changed: the AP is no longer a fixed thing nobody configures, it has
its own runtime-editable SSID and password (below), so "AP mode" is a real
destination rather than a degraded state.

Persisted as NVS namespace `wifi_cfg`, key **`mode`** (u8: 0 = HOME,
1 = AP) — in the `wifi_nvs` partition since 2026-08-12, along with every other
key this module owns.

### Read-only migration from `local_only`

If `mode` is absent at load time, the blob was written by pre-2026-08-11
firmware. `nvs_load()` then reads the legacy key **`local_only`**, and if it
is set, boots into `WIFI_PROV_MODE_AP` — its closest equivalent — rather than
silently defaulting to home mode and trying to join a network the user had
explicitly opted out of.

The migration is strictly **read-only**: `nvs_save_mode()` only ever writes
`mode`, and nothing in this build ever writes `local_only`. The old key is
simply left behind and ignored once `mode` exists, which happens the first
time the mode is saved for any reason. An `err` other than
`ESP_ERR_NVS_NOT_FOUND` on the `mode` read is a real failure and aborts the
load.

This is a *different* migration from the partition move above, and the two
compose: `nvs_load_from()` applies the `local_only` fallback to whichever
partition it is reading, so an old blob found in the default partition is
translated to `mode` and then carried into `wifi_nvs` by
`migrate_from_default_partition()`. A pre-2026-08-11 board therefore crosses
both in one boot.

## State machine

`wifi_prov_state_t` — the finer-grained "where is the state machine right
now" detail, distinct from `mode`, which is the persisted user choice. Both
are reported over `GET /status` so the page can show one coherent toggle plus
a status line without inferring either from the other.

| `state` | Radio | Reached when |
|---|---|---|
| `WIFI_PROV_STATE_AP_MODE` | AP only | `mode == AP` |
| `WIFI_PROV_STATE_UNPROVISIONED` | AP only | `mode == HOME`, no saved credentials (first boot) |
| `WIFI_PROV_STATE_CONNECTING` | AP+STA | `mode == HOME`, first join attempt in flight |
| `WIFI_PROV_STATE_CONNECTED` | STA only | `IP_EVENT_STA_GOT_IP` landed |
| `WIFI_PROV_STATE_RECONNECTING` | AP+STA | was connected, disconnected, retrying |

Transitions:

- **Boot** (`wifi_prov_start()`): AP config applied first, then the mode
  decides. AP mode → `WIFI_MODE_AP`. Home mode with credentials →
  `WIFI_MODE_APSTA` + the fallback timer armed. Home mode without → 
  `WIFI_MODE_AP`, `UNPROVISIONED`.
- **Join confirmed** (`on_ip_event`, `IP_EVENT_STA_GOT_IP`): the fallback
  timer is cancelled and the radio drops to `WIFI_MODE_STA` — the AP goes
  away **only** here, only after an IP actually arrived.
- **Disconnect** (`on_wifi_event`, `WIFI_EVENT_STA_DISCONNECTED`): ignored
  entirely in AP mode or with no credentials. Otherwise state becomes
  `RECONNECTING` (if it had been `CONNECTED`) or stays `CONNECTING`, and
  `esp_wifi_connect()` is retried immediately.
- **Fallback timer** (`ap_fallback_timer_cb`, one-shot,
  `KILNCTL_WIFI_STA_CONNECT_TIMEOUT_MS`, default 15000): if the join still
  has not landed, the AP is brought back up (`WIFI_MODE_APSTA`) and state
  becomes `RECONNECTING`. It returns immediately without doing anything if
  the station reconnected first.

The disconnect handler re-arms that timer only if it is not already counting
down. `esp_timer_start_once()` on a running timer returns
`ESP_ERR_INVALID_STATE`, which is harmless to ignore — but re-arming on every
disconnect would keep pushing the AP fallback further out on a *flapping*
link, exactly the case where the operator most needs a way back in.

If `esp_timer_create()` fails at boot the module logs it and continues with
`ap_fallback_timer == NULL`; the board simply has no
fallback-on-reconnect-timeout that boot.

Every handler above runs on the default event loop's own task, never on a
caller's stack. That is what makes `wifi_prov_start()` non-blocking: it
returns as soon as the driver is configured and started, without waiting for
association or DHCP.

## The board's own access point

Two independent overrides, each with its own NVS pair so an operator can
change the AP's identity without touching (or needing) any saved station
network:

| Setting | NVS keys | Compile-time default |
|---|---|---|
| AP SSID | `ap_ssid` (str), `has_ap_ssid` (u8) | `KILNCTL_WIFI_AP_SSID`, default `"kilnCtl"` |
| AP password | `ap_pass` (str), `has_ap_pass` (u8) | `KILNCTL_WIFI_AP_DEFAULT_PASSWORD`, default `"password"` |

`apply_ap_config()` uses the override if its `has_*` flag is set, otherwise
the Kconfig value. Channel is `KILNCTL_WIFI_AP_CHANNEL` (default 6, range
1-13), `max_connection` is 4.

Validation, in `wifi_prov_set_ap_ssid()` / `wifi_prov_set_ap_password()`:

- SSID must be 1-32 bytes. An **empty AP SSID is refused** with
  `ESP_ERR_INVALID_SIZE` — the AP always has to be reachable by something, so
  "empty" is not a valid "leave it alone" here the way it is for a station
  password.
- Password must be empty (open network) or 8-63 bytes, per WPA2-PSK. A 1-7
  character password can never work on real hardware, so it is refused
  outright rather than silently degrading the AP to open.

Both take effect **immediately** if the AP radio is currently up. This needs
a deliberate extra step: `esp_wifi_set_config()` alone does not kick
already-associated clients off a running AP, so both setters re-apply the
current `wifi_mode_t` to force the radio to pick up the new SSID/PSK now
instead of at the next boot. The practical consequence is that whoever is
editing the AP's identity **loses their own connection** and must rejoin
under the new name/password — the page says so after a successful save. In
APSTA the station side is untouched.

NVS write failure is logged and the change is applied live anyway, with an
explicit "will not survive a reboot" log line — the same convention every
config writer in this firmware uses.

### Why there is a default AP password at all

A fresh board with no NVS overrides and no saved network has to be reachable
by a phone in order to be provisioned at all. That is the entire bootstrap
path, so the AP cannot start unsecured-by-accident or refuse to start.
`KILNCTL_WIFI_AP_DEFAULT_PASSWORD` exists so a never-configured board comes
up as WPA2-PSK rather than as an open network anyone in range can join and
switch relays on.

The default is `"password"`, and its Kconfig help text says plainly: change
it before fielding a board outside a bench/lab setting. It is a
known-weak bench default, not a security measure.

`apply_ap_config()` has a matching failure mode worth knowing: if the
configured password is shorter than 8 characters, WPA2-PSK cannot be
configured at all, so it logs `AP password is shorter than 8 chars -- AP is
OPEN` and starts an **open** AP. Open is better than an AP that refuses to
start, because that AP is the only way to reach the board and fix the
mistake. This can only be triggered by a misconfigured Kconfig default —
Kconfig strings are not validated the way a request is — or by a deliberate
empty override.

## HTTP endpoints

Wire shapes are in [`docs/WEB_UI.md`](WEB_UI.md); the semantics are here.
Note all three live at the server root rather than under `/api/`.

### `GET /status`

Reports `mode`, `state`, the saved station `ssid`, `sta_connected`, `sta_ip`
(empty unless connected), and the AP's currently effective `ap_ssid`. It is
this module's own tracked state, not a device read, and it never fails — a
truncated buffer is sent as-is rather than erroring, because a status readout
that errors is worse than one that is slightly short.

The page polls this every 2 s **only while** `state` is
`connecting`/`reconnecting`, and stops once the outcome is known.

### `GET /scan`

Up to 20 networks as `{ssid, rssi, secure}`, blocking, run on the httpd
worker task (50-150 ms per channel active scan, `show_hidden` false).

**Refused with 400 in AP mode.** `wifi_prov_scan()` returns
`ESP_ERR_NOT_SUPPORTED` when `mode == WIFI_PROV_MODE_AP`, and the handler
turns that into `400 local-only mode: scanning is disabled` (the message
string still carries the pre-redesign name). The reason is that AP mode's
guarantee is *no station-radio activity at all*, and a scan — even though it
never joins anything — still requires bringing the STA interface up. Bending
that guarantee for a settings-page convenience would make the mode mean
something weaker than what it says.

From `UNPROVISIONED` (home mode, AP-only radio) a scan **is** allowed, and
the function temporarily switches to `WIFI_MODE_APSTA` to do it. That does
not connect to anything: `on_wifi_event()` only calls `esp_wifi_connect()`
when credentials exist, and unprovisioned means they do not.

### `POST /provision`

One endpoint, three intents, resolved in that order — the first group present
wins and the handler returns immediately. The field names are deliberately
distinct so the two "SSID and password" intents can never collide in one
request.

**`mode=home|ap`** — sets and persists the mode via `wifi_prov_set_mode()`.
Switching to AP cancels the fallback timer, forces `WIFI_MODE_AP`, and
abandons any in-flight join. Switching to HOME does **not** require new
credentials: with a network already saved it starts a join immediately (AP
stays up alongside), and with nothing saved it just sits at `UNPROVISIONED`.
Anything other than the two literals is a 400.

**`ap_ssid=` and/or `ap_password=`** — changes the board's **own** AP
identity, per the section above. Unlike `mode`, these two are not mutually
exclusive: the page submits both together in one POST, so both are applied if
both are present.

**`ssid=` plus optional `password=`** — station credentials for a *different*
network. `password` may be absent or empty (open network); a missing `ssid`
is a 400. `wifi_prov_set_credentials()` re-validates lengths against
`WIFI_PROV_SSID_MAX_LEN` (32) / `WIFI_PROV_PASSWORD_MAX_LEN` (64) rather than
trusting the handler, saves, and starts a join.

Submitting credentials **while in AP mode switches the board to home mode**.
This is treated as an explicit choice to join a network, and it exists
because without it AP mode had no way back out through the HTTP API at all:
the toggle could set it, but nothing else ever cleared it.

## Resilience guarantees

These are TODO.md section 1's two hard requirements, both stated by the user,
and they are the reason this module is shaped the way it is.

### 1. AP and STA coexist until a home-mode join is confirmed

The fallback AP is **never** torn down before `IP_EVENT_STA_GOT_IP` proves
the join actually worked. Every home-mode path that attempts a station join
(`wifi_prov_start()`, `wifi_prov_set_credentials()`, `wifi_prov_set_mode()`)
goes to `WIFI_MODE_APSTA`, not `WIFI_MODE_STA`, and only `on_ip_event()`
drops to STA-only. If the join never lands, the fallback timer brings the AP
back.

The failure this prevents is concrete: a typo'd password would otherwise
strand the phone that just submitted it with no way back into the board.

### 2. Losing Wi-Fi never affects the control loop or relay safety

`wifi_prov.c` **never touches `kiln_io`, `relay_authority`, or
`safety_link`** — not in its API, not in its event handlers, not on any error
path. Those symbols appear in the file only inside comments saying exactly
that. Wi-Fi state is reported only over
the existing log link (`uart_log_bridge`, task 5 in
[`docs/UART_PROTOCOL.md`](UART_PROTOCOL.md)).

A running firing continues through an AP change, a disconnect, a reconnect
storm, or the Wi-Fi never coming up at all. The profile executor's 1 Hz tick,
the thermal guards, the relay-authority gate and the UART/safety links are
all independent of it. `wifi_prov_start()` failing is logged and non-fatal,
same as every other bring-up step in `app_main`;
`wifi_provision_http_start()` failing is likewise non-fatal to Wi-Fi itself
(the board is up but not provisionable over HTTP that boot) — and it means
**no** web page or API exists that boot, since every other module registers
on that same server.

### Wi-Fi loss is deliberately not a `SAFETY_FAULT_SRC_*`

**Settled, explicit non-fault** (TODO.md section 1's last item). There is no
new safety fault source bit for Wi-Fi, and Wi-Fi loss specifically does not
trip `SAFETY_FAULT_SRC_PC_LINK` — that source is the UART link.

The reasoning: the RP2040 safety processor does not need to know about a link
whose entire purpose is "may or may not exist". A fault source that fires
every time a phone walks out of range would either be ignored in practice or
would stop firings for no safety reason, and both outcomes are worse than
treating Wi-Fi as what it is — an optional, best-effort view onto a kiln that
runs fine without it.

Contrast with the UART link, where loss *does* assert
`SAFETY_FAULT_SRC_PC_LINK` and drop relays: that link is the one the safety
processor and the PC tooling depend on, and its absence genuinely means the
board has lost a supervisor.

### 3. A bad request never crashes the firmware

Also a hard requirement from section 1, and verified: an oversized body and a
missing-field POST both return a clean 400 with the httpd server still
responsive afterward. `Content-Length` is checked before a single byte is
read, bodies land in fixed stack buffers, a short read is a rejection rather
than a partial trust, and `http_form_find_field()`'s "present but too long"
return is treated as a rejected request, never a truncated one — silently
truncating a password is worse than refusing it. The full handler discipline
is in [`docs/WEB_UI.md`](WEB_UI.md).

## What's still open

See TODO.md section 1 for the itemized checklist. Notable gaps as built:

- **No authentication anywhere.** Anyone who can reach the board's IP, or
  join its AP with the (possibly default) password, can switch relays and
  start a firing. Section 1 never asked for auth and this does not build one.
- **No captive portal.** A phone joining the fallback AP has to browse to the
  board's address; nothing redirects it there.
- **No mDNS/hostname.** The station IP is reported over `GET /status` and in
  the boot log, and that is the only way to find the board on a home network.
- **The legacy `local_only` migration path has never run against a real old
  blob** — it is straight-line code that reads one key, but it is untested in
  the field.
- **The default-partition → `wifi_nvs` migration has run, once, and worked.**
  On the first boot after the new partition table was flashed (2026-08-12)
  the bench unit logged `migrating Wi-Fi config from the default NVS
  partition to 'wifi_nvs' (ssid 'ATTFqf9g79', mode home)` and joined the
  network on the first attempt — so the adoption path read a real pre-split
  blob, credentials *and* password, and wrote them through correctly. The
  following reflash then took the steady-state path with no migration line.
  What remains untested is adoption from a board carrying an *AP-mode* or
  AP-identity-override configuration, since this one was in home mode.
- **Open defect: a provisioned board reported itself unprovisioned.** On
  2026-08-12, before the partition split, the bench unit booted with
  `no saved credentials: AP 'kilnCtl' for first-boot provisioning` and fell
  back to its AP — while the default NVS partition in fact held a valid SSID
  and password, as the migration proved minutes later by reading that same
  namespace and joining first try. So `nvs_load()`'s notion of "provisioned"
  (which gates on the `has_creds` flag) and the migration's (which adopts
  when credentials are present) disagree, and the disagreement is silent.
  Not diagnosed. Its symptom is the worst kind: a board that quietly stops
  appearing on the network and looks like it was never set up.
- **Open defect: `esp_wifi_set_config(AP)` fails at startup with
  `ESP_ERR_WIFI_MODE`.** Both boot logs taken on 2026-08-12 contain:

  ```
  E wifi_prov: esp_wifi_set_config(AP) failed: ESP_ERR_WIFI_MODE
  ```

  `wifi_prov_start()` calls `apply_ap_config()` *before* the `esp_wifi_set_mode()`
  that selects AP/APSTA/STA for this boot, so the AP configuration is being
  applied while the driver is in a mode that does not have an AP interface —
  that ordering is the obvious suspect, but it has **not** been diagnosed and
  no fix has been attempted.

  It is **not blocking**, and is not being treated as one: the station join
  still succeeds on the same boot (`apply_sta_config()` runs *after* the mode
  is set), and the fallback AP works when it is needed. Every other caller of
  `apply_ap_config()` — `ap_fallback_timer_cb()`, `wifi_prov_set_mode(AP)`,
  and both AP-identity setters — re-applies it after a mode that includes the
  AP interface is already in effect, so the boot-time failure does not stick.
  The visible consequence today is an ERROR line in every boot log. The risk
  worth naming, and the reason this is recorded rather than shrugged off, is
  that a boot **straight** into AP mode or into `UNPROVISIONED` has only this
  one call to configure the AP with, and neither path has been re-checked on
  hardware since the error appeared.
