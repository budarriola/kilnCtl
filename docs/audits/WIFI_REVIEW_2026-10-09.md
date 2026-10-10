# Wi-Fi provisioning and networking review, 2026-10-09

Scope: KilnFW Wi-Fi provisioning and networking on `origin/dev`:
`firmware/KilnFW/App/drivers/net/wifi_prov.c`, `wifi_prov_api.c`,
`wifi_prov_link.c`, `wifi_prov_nvs.c`, `wifi_prov_internal.h`,
`drivers/http/wifi_provision_http.c`, the captive-portal DNS responder, the
saved-networks list, AP fallback, static IP config, the DHCP hostname, legacy
`wifi_cfg` migration, the factory_reset wifi scope, and the UART bridge Wi-Fi
commands (`drivers/bridge/uart_bridge_ext_wifi.c`) where they reach the same
setters. Code review plus negative tests. No board access. Findings only, no
code changed.

Owner decisions taken as the expected behavior: STA probing continues while
someone is logged in; with auth off the AP stays up while stations are
associated; the AP-password HMAC is retired and the former HMAC routes are
web-admin.

Not repeated here (already covered): review 5 M2 (one-shot legacy migration),
review 6 M1/L1 (adopt legacy until a verified saved_nets record exists),
review 8's migration notes, review 2 #1, `wifi_factory_reset_driver_storage_2026-09-21.md`,
`ROUTE_TIER_REVIEW_2026-10-09.md`, and review 11's STA-in-192.168.4.0/24 issue.

## Findings

### MED-1: captive-portal DNS responder answers responses and every interface, so it can loop or reflect

Where: `wifi_prov_link.c` `dns_hijack_task()`, about lines 1067-1161.

The task binds UDP `INADDR_ANY:53` and replies to every datagram of 12 bytes or
more with QDCOUNT 1. It never looks at the QR bit (`buf[2] & 0x80`), so a DNS
*response* gets a response back. It never checks the source port or source
address, and it never checks which interface the datagram arrived on, so it
also answers on the STA (LAN) interface for the whole life of the board. The
task starts unconditionally in `wifi_prov_start()` and never stops.

What happens:

- A single spoofed datagram with source `boardA:53` sent to `boardB:53` starts
  an endless ping-pong between two kilnCtl boards on the same LAN. Each reply
  has QR=1 and QDCOUNT=1, which the other side accepts as a query. The same
  works against any other DNS responder that answers responses, and against
  a resolver that sends back an error for an unsolicited response.
- A spoofed datagram whose source is the board's own address and port 53 can
  self-loop through lwIP loopback (`CONFIG_LWIP_NETIF_LOOPBACK=y` in the
  bench sdkconfig). This one is plausible from reading the code but has not
  been reproduced on hardware.
- On the LAN, any host can use the board as a small reflector. The answer is
  only about 16 bytes bigger than the query, so amplification is low, but the
  board spends CPU and internal-SRAM pbufs on each one.
- Any LAN client that is configured to use the board as a resolver is told
  every name is 192.168.4.1. That is harmless only on the AP.

Fix: drop the datagram when `buf[2] & 0x80` is set, or when the opcode is not
0. Drop it when the source port is 53. Answer only on the AP netif, either by
checking that the source is in 192.168.4.0/24 or by binding to the AP address
instead of `INADDR_ANY`. A per-second reply cap would bound the remaining
reflector use.

### LOW-1: saved_nets loader trusts `count` and NUL termination from flash

Where: `wifi_prov_nvs.c` `nvs_load_saved_nets_from()`, about lines 313-377.
Users: `wifi_prov_api.c` around lines 59, 176 and 194-198 (forget
compaction), `wifi_prov_link.c` around line 292 (join-candidate loop),
`wifi_prov.c:632`.

`saved_nets_blob_t` is `{uint8 version; uint8 count; saved_net_t nets[8]}`.
The loader checks blob size and version but does not check `count <= 8`, and
does not force a NUL into the last byte of each `ssid[33]` or `password[65]`.
The sibling `nvs_saved_nets_record_present()` does check `count <= 8`, so the
two disagree about what a valid record is.

What happens with a size-correct v1 blob that has `count > 8`: the
join-candidate loop and the add-network upsert loop read past `nets[8]` into
the next `s_wifi` fields. `do_forget_network()`'s compaction writes `nets[i]`
for `i` up to `count - 1` and then `memset`s `nets[count]`, which writes past
the array into the rest of `s_wifi`. An unterminated `ssid` or `password` makes
`strcmp`/`strlen`/`strncpy` run into the next field. NVS checks a CRC on each
entry, so this needs a buggy writer or a hand-built blob rather than plain
flash wear. It is defence in depth, but the fix is two lines.

Fix: in the loader, treat `count > 8` as a corrupt record (same branch as a
wrong size), and set `nets[i].ssid[32] = '\0'` and `nets[i].password[64] = '\0'`
for every entry.

### LOW-2: an unreadable, wrong-size or newer-version saved_nets blob reads as "no networks", opens the setup routes and is overwritten on the next save

Where: `wifi_prov_nvs.c` `nvs_load_saved_nets_from()` (it returns `ESP_OK`
with an empty list on read error, size mismatch, or a version newer than this
firmware knows); `wifi_prov.c:775` `wifi_prov_is_unprovisioned()`;
`wifi_prov_api.c` add/forget, which call `nvs_save_saved_nets()` with the
in-RAM list.

Review 8 judged the *migration* side of this safe, and that still holds. This
finding is about the two other effects:

- With no legacy single credential to fall back on, the board boots with
  `saved_nets.count == 0` and state UNPROVISIONED. `wifi_prov_is_unprovisioned()`
  is then true, which opens the WIFI_SETUP routes with no session even though
  the flash still holds credentials. A transient read error at boot produces
  the same result.
- The first add or forget after that writes the in-RAM list, which is empty or
  holds only the new entry, over the blob. A board rolled back from newer
  firmware therefore loses every saved network that newer firmware stored. The
  code comment says the flash data is "left untouched", which is true only
  until the first write.

Fix: keep a `saved_nets_load_refused` flag. While it is set,
`wifi_prov_is_unprovisioned()` returns false and add/forget refuse with a clear
error, or they read the raw blob again and preserve it. Distinguish a read
error, which should be retried, from a record that really does not exist.

### LOW-3: UART bridge can set an AP password with an embedded NUL, which leaves the AP open while reporting success

Where: `uart_bridge_ext_wifi.c:326-347` (`WIFI_CMD_SET_AP_IDENTITY`),
`wifi_prov_api.c` `wifi_prov_set_ap_password()` (around line 492) and
`wifi_prov_set_ap_ssid()` (around line 435), `wifi_prov_link.c:56`
`apply_ap_config()`.

`wifi_prov_add_network()` refuses an embedded NUL (`memchr`, audit L15), but
the two AP setters do not. HTTP cannot reach this, because
`http_form_url_decode()` refuses `%00`. The UART bridge copies raw payload
bytes with an explicit length, so a 9-byte password `ab\0cdefgh` passes the
8-63 length check in the producer. `do_set_ap_password()` then `strncpy`s it
into `s_wifi.ap_password`, which stores `"ab"`. `apply_ap_config()` sees
`strlen(pw) < 8` and brings the AP up with `WIFI_AUTH_OPEN`. The UART reply is
success, and the log line "AP password changed (WPA2-PSK)" uses the
two-character length and says WPA2-PSK. A leading NUL gives the same open AP,
and that log line says "open". The stored value persists, so the AP stays open
across reboots. An embedded NUL in the AP SSID only truncates the name.

Reaching the bridge needs the USB serial link, so this is LOW, but it is the
one path found where an AP becomes open while the caller is told it worked.

Fix: add the same `memchr` refusal to both AP setters, or check `strlen` of the
copied value against `password_len` in the producer.

### LOW-4: static IP validation does not check that the configuration makes sense

Where: `wifi_prov_api.c` `wifi_prov_set_static_ip()`, about lines 668-724;
`wifi_prov_link.c:85` `parse_ipv4()` (lwIP `ip4addr_aton()`).

The only checks are a successful parse of ip, netmask and gateway, the
192.168.4.0/24 refusal, and the dns/dns2 checks. Not checked:

- the netmask is a contiguous run of ones (for example `255.0.255.0` is
  accepted);
- the gateway is inside `ip & netmask`;
- the address is not 0.0.0.0, 255.255.255.255, 127.x, multicast, or the
  network or broadcast address of its own subnet.

`ip4addr_aton()` also accepts shorthand forms such as `10.1` and the bare decimal integer 167772161,
so the stored string may not be the dotted quad the operator meant, although
the parsed value is used consistently.

Mitigation in place: under an unconfirmed static IP, GOT_IP keeps the AP up
(`wifi_prov_link.c:840`) until a request arrives on the static address, so a
bad configuration does not strand the board. It does persist across reboots,
and the board then relies on the AP every boot.

Fix: refuse a non-contiguous netmask, a gateway outside the subnet, and the
special addresses above. Require strict four-octet dotted decimal.

### LOW-5: STA auth threshold accepts WPA (TKIP)

Where: `wifi_prov_link.c:181` (`apply_sta_config()`,
`threshold.authmode = WIFI_AUTH_WPA_PSK`).

A rogue AP advertising the saved SSID with WPA/TKIP is accepted, which allows a
downgrade from WPA2. PMF is not requested.

Fix: `WIFI_AUTH_WPA2_PSK` as the threshold, and `pmf_cfg.capable = true`. If
an owner still needs a WPA-only router, make that an explicit setting.

### LOW-6: `dns_hijack` task is exempt from stack-margin reporting under a reason that is no longer true

Where: `tools/check_stack_margin_registration.ps1:560`; `wifi_prov_link.c:1170`
creates the task with a 3072 B PSRAM stack and a NULL handle.

The exemption says "provisioning-only captive-portal DNS, self-deletes". The
task only deletes itself when `socket()` or `bind()` fails. Otherwise it runs
`while (true)` for the life of the board, on every interface (see MED-1). It
keeps two 512-byte buffers on that 3072 B stack and calls `recvfrom`/`sendto`.
Its high-water mark is never measured. This is the same stale-exemption shape
that `wifi_prov.c:699-711` already fixed for `wifi_prov_owner`.

Fix: register it with `stack_margin_register()` and drop the exemption, or make
the task really provisioning-only and delete it when the AP goes down.

### INFO

- **Unlocked string getters.** `wifi_prov_get_saved_ssid()`,
  `wifi_prov_get_ap_password()` and the `wifi_prov_get_static_*()` getters
  return pointers into `s_wifi`, which `owner_task` rewrites with `strncpy`.
  A reader can see a torn value during a concurrent change. This matches the
  module's documented convention. The worst effect is one wrong status
  response, never an overrun, because every buffer keeps its final NUL.
- **`/provision` applies `ap_ssid` and then `ap_password`, not atomically.** If
  the SSID succeeds and the password fails, the handler returns 400 while the
  new SSID is already live.
- **Shared static result arrays.** `scan_get_handler` and
  `networks_get_handler` fill file-scope arrays. That is safe with today's
  single httpd worker. It becomes a data race if httpd ever runs concurrent
  workers.

## Checked, no defect

- `wifi_prov_add_network()`: SSID 1-32, password at most 64, embedded NUL
  refused. Producers validate and the owner task does the work.
- Scan results: SSID copied with `strncpy(..., 32)` and terminated explicitly;
  driver records live in a static PSRAM array of 20.
- SSIDs in JSON go through `json_escape`.
- `/api/wifi/status` (OPEN tier): `ap_password` is emitted only when the
  request arrived on the AP; `ssid` and static fields are gated on
  `http_auth_may_disclose()`; `sta_ip` on `may_disclose || !on_ap`. The JSON is
  built in a heap scratch buffer.
- The UART bridge status reply reduces the AP password to a `"[set]"`/`""`
  marker (`uart_bridge_ext_wifi.c:44-59`).
- `do_get_saved_networks()` never copies a password; `/networks` and the UART
  `GET_NETWORKS` reply carry SSIDs only.
- Backup export: no Wi-Fi credential is exported (no backup source references
  `saved_nets`, `ap_password` or the STA password).
- Legacy migration: the default-partition copy is erased only after the new
  record is written and read back, on both the saved_nets path and the AP
  identity path.
- DHCP hostname: set to `kilnctl` on the STA netif before the DHCP client
  starts (`wifi_prov.c:546`).
- factory_reset wifi scope: erases `wifi_nvs` and the legacy default-partition
  keys, and brackets `esp_wifi_restore()` as described in the 2026-09-21 audit.
- httpd stack use: request bodies for forget/provision are at most
  `PROV_BODY_MAX + 1` and ip_config at most 128 bytes on the stack; the larger
  buffers are on the heap. Nothing found near the 4496 B ceiling.
- The `wifi_prov_owner` task is registered for stack-margin reporting.

## Host tests

`test_wifi_prov.c` covers the AP-teardown policy (session and AP-client
deferral, auth on and off), static-IP confirmation, the AP-subnet refusal,
DNS validation, the reply-slot pool, legacy migration failures, and
`is_unprovisioned`. `test_wifi_prov_status_disclosure.c` covers the redaction
helper and the disclosure gate, but not `wifi_provision_http.c` itself, which
host tests cannot link.

Not covered by any test: the DNS responder, the saved_nets loader's handling
of a bad `count` or an unterminated field, the AP setters' NUL handling, and
the `on_ap` gate on `ap_password` in the status handler.

### Negative tests

Run with `tools\negtest.ps1` in a throwaway worktree, command
`build_host_tests.ps1 -OutDir {OUT} -Only test_wifi_prov` (selects
`test_wifi_prov` and `test_wifi_prov_status_disclosure`), expect pattern
`FAIL .*test_wifi_prov`.

The unmutated baseline passed. Results:

| Mutation | Result | Caught by |
|---|---|---|
| M1 `wifi_prov_is_unprovisioned()` drops `&& saved_nets.count == 0` | CAUGHT | `test_wifi_prov.c:1099` |
| M2 `wifi_prov_add_network()` embedded-NUL refusal removed | CAUGHT | `test_wifi_prov.c:714-715` |
| M3 `wifi_prov_set_static_ip()` 192.168.4.0/24 refusal removed | CAUGHT | `test_wifi_prov.c:431, 435` |
| M4 saved_nets migration read-back mismatch no longer clears `s_legacy_erase_pending` | CAUGHT | `test_wifi_prov.c:703, 707` |
| M5 `/api/wifi/status` emits `ap_password` whether or not the request came in on the AP | MISSED | none |

M5 is missed because `wifi_provision_http.c` cannot be host-compiled (GCC-only
`asm("_binary_...")` blob externs and `<sys/socket.h>`).
`test_wifi_prov_status_disclosure.c` tests the redaction helper and the
`may_disclose` gate, not the handler. Its header says the `on_ap`/`ap_password`
narrowing is "already covered by test_wifi_prov.c's own AP-password tests".
That is not accurate: those tests (`test_arrived_on_ap_*`) cover
`wifi_prov_request_arrived_on_ap()`, not the handler's use of its result. The `ap_password` gate is the most sensitive line in this area and has
no automated coverage. A host test that links the status handler with a stubbed
`wifi_prov_request_arrived_on_ap()` would close the gap, as would a bench judge
that reads `/api/wifi/status` over the LAN and asserts `ap_password` is empty.

The run ended with `verdict: ERROR` only because this review document was
written into the worktree while the run was in progress, which the script
correctly reported as a change to the real tree. `git status --porcelain`
afterwards showed only this file in the worktree. negtest confirmed that no
mutated file changed in either tree; the shared main tree's status changes
came from other sessions.
