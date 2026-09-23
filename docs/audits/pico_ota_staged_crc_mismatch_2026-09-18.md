# The ESP's staged Pico-image CRC32 is computed with the wrong parameterization

Audit date: 2026-09-18. Analysis only — no source was modified, nothing was
built or flashed, and no hardware MCP tool was invoked. The one thing executed
was a pure-Python CRC computation over a local file, described in section 3.

## 1. Verdict

**IDENTIFIED.** The bytes the ESP hashed are the same bytes that are in the
file. The ESP's CRC differs because it runs the CRC-32 machine with the wrong
initial value and an extra final XOR, producing a *different CRC-32 variant* of
the *same, correct* data.

`firmware/KilnFW/App/drivers/http/ota_http_pico.c:75` seeds the accumulator with
`0xFFFFFFFFu`, and `:206` applies `crc ^= 0xFFFFFFFFu` as a "final XOR", on top
of `esp_rom_crc32_le()` (`:193`) — a function that **already performs both of
those inversions internally**. The two extra inversions do not cancel; they
shift the computation to init `0x00000000` with no output XOR.

This is a genuine defect with real consequences (section 5), not a
measurement error on the reporting side. The uploaded file, and the measurement
of it as CRC-32/zlib `0x83C472EF`, are both correct.

## 2. What was ruled out, and how

Each of the four alternative hypotheses in the brief is eliminated from source.

**The PC-side upload path is clean.** `_push_image()`
(`tools/PcTools/src/kilnctrl/ota_http_client.py:192`) is the shared body behind
`push_pico_image()` (`:286`, `:297`). It opens the file in binary mode and reads
it whole — `with open(path, "rb") as f: data = f.read()`
(`ota_http_client.py:214-215`) — and hands that `bytes` object directly to
`urllib.request.Request(..., data=data, method="POST")`
(`:224-233`) with `Content-Type: application/octet-stream` and
`Content-Length: str(len(data))` (`:229-230`). There is no text mode, no
encode/decode round trip, no chunking loop of the client's own, and no
`requests` multipart wrapper — `urllib` sends a `bytes` body verbatim. The only
hash the client computes is a SHA-256 for logging (`:220`), which is never
compared against the ESP's CRC. Item 1: clean. Item 4: definitively **not**
multipart, on the client side by construction and on the ESP side by the
handler reading the body as an opaque stream with no boundary parsing at all
(`ota_http_pico.c:169-217`); `ota_http.h:387-388` states the same ("same raw
(non-multipart) byte-stream body").

**`written` and the CRC are computed over exactly the same bytes.** This was the
inversion the brief asked me to rule in or out precisely, and it is ruled out.
`written` is *not* the Content-Length value passed through. It is an accumulator
initialized to 0 (`ota_http_pico.c:76`) and advanced only by the byte count
actually returned by each `httpd_req_recv()`: `written += (size_t)ret`
(`:197`). `content_len` is used solely as the loop bound (`:169`,
`while (written < content_len)`) and to size each request (`:170-173`). Crucially
the CRC is updated from the *same* `ret` on the *same* buffer, on the line
immediately before the write of that count and adjacent to the partition write:

- `ota_http_pico.c:186` — `esp_partition_write(part, written, s_ota_pico_chunk, (size_t)ret)`
- `ota_http_pico.c:193` — `crc = esp_rom_crc32_le(crc, s_ota_pico_chunk, (uint32_t)ret)`
- `ota_http_pico.c:197` — `written += (size_t)ret`

All three consume the identical `ret` bytes of the identical buffer in the
identical iteration. Any short read exits the loop via the `ret <= 0` failure
path (`:175-181`), and the loop cannot terminate with `written != content_len`.
So the reported length is an honest count of received-and-written bytes, and the
CRC covers precisely that same region. Item 2: the length is not the lying
number. Item 3: there is no buffered/aligned write path that could diverge from
the CRC region — the write is unbuffered and per-recv-chunk, sector alignment is
handled entirely by the up-front erase (`:137-141`) and never by re-chunking the
data, so no tail flush, double-CRC, or padding-inclusion is possible.

That the two numbers are computed over the same bytes is exactly why the length
agreeing while the CRC disagrees is so diagnostic: it rules out every
data-corruption explanation and leaves only "the algorithm differs".

## 3. The actual cause, confirmed numerically

`esp_rom_crc32_le(crc, buf, len)` is the ESP-IDF ROM routine whose contract is
that it complements its seed on entry and complements its result on exit. The
idiomatic call for a standard zlib CRC-32 is therefore a seed of **0** and **no**
final XOR, with chaining done by feeding the previous return value straight back
in. Every other call site in this repository uses exactly that idiom — for
example `esp_crc32_le(0, ...)` at
`firmware/KilnFW/App/drivers/persist/kiln_board_identity.c:29`,
`firmware/KilnFW/App/drivers/persist/kiln_package.c:159`,
`firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c:83`,
`firmware/KilnFW/App/drivers/http/profiles_http.c:379`, and the chained pair at
`firmware/KilnFW/App/drivers/persist/zones_config_migrate.c:935-936`. **This
handler is the sole outlier in the codebase.**

Let `R(init, data)` denote the bare reflected table loop over poly `0xEDB88320`
with no inversions at either end. The ROM function is
`f(c, d) = ~R(~c, d)`. With the handler's seed of `0xFFFFFFFF`
(`ota_http_pico.c:75`), the first chunk computes `~R(0, d1)` — the raw loop
starts at 0 rather than at `0xFFFFFFFF`, i.e. the initial value is already lost.
Chaining across chunks is self-consistent (`f(prev, d2) = ~R(R(0,d1), d2)`), so
after the whole body the accumulator holds `~R(0, ALL)`. The final
`crc ^= 0xFFFFFFFFu` at `:206` then undoes that last complement, yielding
`R(0, ALL)` — CRC-32 with init `0x00000000` and no output XOR. The correct
CRC-32/zlib is `R(0xFFFFFFFF, ALL) ^ 0xFFFFFFFF`.

I computed both parameterizations over the real file,
`C:\wt\picoimg_fcnr26\firmware\SaftyFW\build\SaftyFW_slotB.bin` (114796 bytes),
with a hand-written table loop in Python:

| parameterization | value |
| --- | --- |
| `zlib.crc32` — init `0xFFFFFFFF`, final XOR (correct CRC-32/zlib) | `0x83C472EF` |
| `R(0, data)` — init 0, **no** final XOR (what the code computes) | **`0xBA38A716`** |
| `R(0, data) ^ 0xFFFFFFFF` | `0x45C758E9` |

`0xBA38A716` is bit-for-bit the value the ESP reported. This is not an
inference — the predicted wrong parameterization reproduces the observed number
exactly, on the first try, over the actual uploaded file. The diagnosis is
confirmed.

The code's own doc comment at `ota_http_pico.c:58-65` asserts the opposite: it
claims the value is reached "via `esp_rom_crc32_le(0xFFFFFFFF, ...)` chained
across chunks then a final XOR, per esp_rom_crc.h's own 'add ~ at the beginning
and the end' chaining recipe". That is a misreading of the header. The header's
"add ~" note describes what the ROM function does *for* the caller; the comment
took it as an instruction *to* the caller and applied the inversions a second
time. `ota_http.h:393-397` repeats the same false claim, stating the value is
"byte-for-byte what the Pico's own read-back CRC will compute" — it never is.

The Pico side is correct and is not implicated. `bootloader_crc32()`
(`firmware/SaftyFW/bootloader/crc32.c:19-30`) is a textbook implementation —
init `0xFFFFFFFFu` at `:25`, `return crc ^ 0xFFFFFFFFu` at `:29` — over a table
built from poly `0xEDB88320u` (`:12`), and it is pinned by a known-answer test:
`firmware/SaftyFW/test/test_bootloader_metadata.c:31` asserts
`bootloader_crc32("123456789", 9) == 0xCBF43926`, the standard CRC-32/zlib check
value. The Pico computes the true zlib CRC; the ESP does not.

## 4. Where the wrong value goes

The staged CRC is not recomputed anywhere downstream — it is carried verbatim
into the wire protocol. `ota_pico_do_stage()` passes it to
`ota_pico_relay_start(ota_http_safety, (uint32_t)written, crc, ...)`
(`ota_http_pico.c:224`); that stores it as `s_relay_args.image_crc32`
(`firmware/KilnFW/App/drivers/net/ota_pico_relay.c:739`, field declared at
`:369`), and the relay writes it into the UPDATE_BEGIN header at
`ota_pico_relay.c:435` and repeats it in the UPDATE_END frame at `:616`. The
same wrong number also reaches the operator through the 202 response body
(`ota_http_pico.c:213-215`), which is the string the MCP tool surfaces
(`tools/PcTools/src/kilnctrl/mcp_server_ota.py:378-379`).

On the Pico, `update_receiver_handle_end()` reads the freshly written slot back
out of XIP-mapped flash and CRCs what actually landed —
`actual_crc = bootloader_crc32((const uint8_t *)slot_data_ptr, s_header.length)`
(`firmware/SaftyFW/src/tasks/update_task.c:850`) — then compares it against the
ESP-supplied header value: `if (actual_crc != s_header.crc32)` →
`UPDATE_STATUS_ERR_CRC_MISMATCH` and `update_task_revert_target_slot()`
(`:863-867`).

Those two values are computed by different algorithms over what should be
identical bytes. **They can therefore never match**, for any image, even a
perfectly transferred one.

## 5. Severity

**This is not a reporting-only defect, and it is not a silent-corruption hole
either. It is a fail-closed defect that renders Pico OTA entirely
non-functional.**

Taking the two possibilities the brief asked me to rank between:

*Can a corrupted image reach the Pico's active slot?* **No — not through this
bug.** The check fails in the safe direction. A mismatch aborts the update and
reverts the target slot (`update_task.c:864-866`), and the bootloader
independently re-verifies a slot's CRC before booting it
(`firmware/SaftyFW/bootloader/main.c:231`, `bootloader_crc32(slot_data, length) == expected_crc`)
against metadata that is only ever written *after* a passing check
(`update_task.c:869-876`). The safety processor is not at risk of running a
corrupt image because of this.

*Is it cosmetic?* **No.** Because the comparison can never succeed, every
ESP-driven Pico firmware update must fail at the verify step with
`ERR_CRC_MISMATCH`, regardless of link quality. The entire OTA path to the
independent safety processor is inoperable. That this has not been observed yet
is only because the relay has been failing earlier, at the destination-slot
erase — see `docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`, which
establishes the Pico reboots on its watchdog mid-erase and never reaches
VERIFYING. **These are two independent, serially-blocking defects on the same
path: fixing the watchdog/erase problem alone will not produce a working
update — it will merely advance the failure to the CRC check.** That is worth
stating plainly so the erase fix is not mistaken for a regression when the
update still fails.

There is one genuine weakness in the integrity story, which the brief asked
about directly (item 5). The Pico's check compares its read-back against a value
**supplied by the ESP in the same transfer**, not against any independently
derived reference. Its real coverage is therefore "the bytes that landed in the
slot match the bytes the ESP intended to send" — it validates the isolated-link
relay and the flash write, which is what
`update_task.c:837-841`'s own comment claims for it. It does **not** validate the
ESP's staging step: had the staged copy in `pico_img` been corrupted before the
CRC was taken, the ESP would have CRC'd the corrupt copy, sent that CRC, and the
Pico would have agreed with it. Nothing in the pipeline cross-checks the ESP's
staged image against a value derived from the original file — the client computes
a SHA-256 (`ota_http_client.py:220`) but only logs it, and never sends it for
comparison. As it happens, this particular bug is *not* an instance of that hole
(the staged bytes are fine; only the arithmetic is wrong), but the hole is real
and adjacent, and on a safety processor it is worth closing.

## 6. Proposed fix — NOT implemented

Three lines in `firmware/KilnFW/App/drivers/http/ota_http_pico.c`, bringing this
call site into line with every other CRC call site in the repo:

1. `:75` — seed with `0` instead of `0xFFFFFFFFu`.
2. `:206` — delete the `crc ^= 0xFFFFFFFFu;` final-XOR line entirely.
3. `:58-65` — correct the doc comment, which currently asserts the wrong recipe
   and would otherwise re-teach the same error. Correct the matching claim at
   `ota_http.h:393-397` too.

The chaining at `:193` is already right and needs no change.

Two further recommendations, offered but out of scope for the minimal fix:

- **Add a known-answer test on the ESP side.** SaftyFW's
  `test_bootloader_metadata.c:31` caught nothing here only because the ESP had
  no equivalent. A host test asserting the ESP's staging CRC over `"123456789"`
  equals `0xCBF43926` would have failed loudly the day this was written, and
  would pin the fix. Note the constraint recorded in this project's memory that
  HTTP handlers are target-build-only and mostly do not link into host tests —
  so this likely wants the CRC step factored into a small linkable helper rather
  than tested through the handler.
- **Close the staging-integrity gap** by having the client send its SHA-256 (it
  already computes one at `ota_http_client.py:220`) in a header, and having the
  ESP compare it against the digest it already computes over the staged bytes
  (`ota_http_pico.c:191-192`, finished at `:200-208`) before starting the relay.
  That would make the end-to-end check independent of the ESP's own view for the
  first time.

## 7. What was verified versus inferred

Verified from source, with the citations above: the client's binary read and raw
octet-stream POST; the absence of multipart on both sides; that `written` counts
actually-received bytes and covers the same region as the CRC; the handler's
seed, chaining call and final XOR; that the CRC is passed unchanged into
UPDATE_BEGIN/UPDATE_END; the Pico's correct CRC implementation and its
known-answer test; and the comparison site that would reject the mismatch.
Verified numerically: that the predicted wrong parameterization reproduces the
observed `0xBA38A716` exactly over the real 114796-byte file.

Inferred, though not seriously in doubt: the internal behaviour of
`esp_rom_crc32_le()` (its source is in the ESP-IDF ROM, not this repository).
The inference is load-bearing for the *explanation*, but not for the
*conclusion* — the numerical reproduction of the exact reported value, plus the
fact that every other CRC call site in this repo uses the seed-0/no-final-XOR
idiom, independently confirm the ROM function's contract and the diagnosis.
