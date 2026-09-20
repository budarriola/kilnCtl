# CT topology "not fetched" false refusal + wizard mapping blind spot (2026-09-19)

## Report

Owner changed the CT topology / CT-channel-to-zone mapping on the safety
commissioning page and got:

> CT topology has not been fetched from the safety processor yet -- retry
> once the link has synced

with the link actually up and armed (board on `73c1da94`, both processors).
Separately, the setup wizard's CT step showed all three CT0/CT1/CT2 rows as
"not mapped yet" even though the same manual mapping had already been
committed.

## Root cause 1: verify-mapping refusal (reset-one-side class)

`zones_current_sweep_start()`
(`firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c`) computed
`ct_topology_unknown` from `safety_cfg_store_fetched_ms_ago() == UINT32_MAX`.
That accessor answers "has this boot performed a live UART round trip to the
Pico", not "is the cached value trustworthy". `safety_cfg_store`'s own header
documents the cache as fetch-on-change: a boot whose NVS-loaded cache already
agreed with the Pico's live `config_crc` at boot does zero further UART
traffic in steady state, so `fetched_ms_ago()` reads `UINT32_MAX` ("never
fetched, live, this boot") for the entire life of a perfectly healthy boot.

Live evidence taken on the bench board this session: `safety_get_link_stats`
showed a `cmd histogram` with `fw_version=1`, `config_page=0` -- exactly one
`FW_VERSION` frame (sent once, unsolicited, at Pico boot) and precisely zero
`CONFIG_PAGE` frames this ESP boot, while the link was up and armed. Any
config value an operator commits on the Pico's side after that point sits in
the Pico's own store with no ESP-side push and no ESP-side poll to notice it
-- the ESP only ever re-fetches on a CRC mismatch it happens to observe.

This is the "reset-one-side" bug class named in `CLAUDE.md`: the write path
(`/api/safety/commissioning` -> Pico) and the read path
(`zones_current_sweep_start()`'s refusal check) are joined by an implicit
contract ("the ESP's cache reflects the Pico's committed value") that nothing
enforces, and the specific signal chosen to represent "do we know it" was a
per-boot liveness timer instead of a per-value committed flag.

`zone_cfg_committed_ct_topology()`, a few lines above the refusal check in
the same file, already asks the right question via `row.set` -- true for any
parameter ever genuinely committed, whether read back this boot or loaded
from the persisted NVS cache. The fix makes the refusal check ask that same
question (`zone_cfg_ct_topology_row_set()`), and additionally performs one
bounded, blocking live refetch (`safety_cfg_store_refetch()`) from the
httpd-worker request context when the topology is genuinely unknown but the
link is up and the peer's live CRC is known -- turning an unconditional
"retry later" into an in-request self-heal whenever a live fetch would
actually succeed.

## Root cause 2: wizard mapping blind spot (second, independent instance)

The wizard's `/api/zones/ct_channel_map` handler
(`ct_channel_map_get_handler()` in `firmware/KilnFW/App/drivers/http/zones_http.c`)
read CT-channel-to-zone mapping only from `zones_ct_channel_map_derived()`, an
ESP-local NVS blob populated exclusively by the automatic current-sweep
tool's own derivation (`zone_ct_map_set()`). It never consulted the safety
processor's own committed `ct_channel_map[0..2]` (param ids 0x0106-0x0108) --
the exact field the commissioning page's manual-entry path writes. An
operator who typed the mapping in by hand, without ever running the
automatic sweep, was therefore invisible to the wizard: fully committed on
the hardware, reported as "not mapped yet" in the UI.

Same class as root cause 1 (a consumer reading only one of two disjoint
sources of the same fact), but a distinct code path with no shared fix.

## Fix

- `zones_current_sweep_task.c`: added `zone_cfg_ct_topology_row_set()`
  (checks `row.set` on param 0x031F) and used it in place of
  `fetched_ms_ago() == UINT32_MAX`; added a bounded live-refetch-and-retry
  when unknown but link-up.
- `zones_http.c`: `ct_channel_map_get_handler()` reads the safety processor's
  committed `ct_channel_map[0..2]` in addition to the sweep-derived record,
  without adding any new route -- but reports it as separate
  `committed_mask`/`committed_zone[0..2]` fields, never merged into
  `mask`/`zone[0..2]`. The first cut of this fix (same day, review round 2)
  did merge the two, which destroyed provenance: `safety_commissioning_page.html`
  renders a set `mask` bit as read-only "DERIVED from the sweep" text and can
  never surface its "safety processor disagrees with what the sweep measured"
  warning for a channel that was actually filled in from the committed side,
  not the sweep. `mask`/`zone[0..2]` are therefore sweep-only again, exactly
  as before this whole fix; `committed_mask`/`committed_zone[0..2]` are the
  new, separate channel for "what the Pico itself has confirmed". Per
  `config_params_finalize_ct_channel_map()` (SaftyFW `config_params.c`),
  which only ever sets the Pico's own group bit once all three of
  0x0106-0x0108 are committed, `committed_mask` mirrors that all-or-nothing
  rule: a 2-of-3 partial reports `committed_mask` 0, not a partial mask.
- Host tests added in `test_zones_http.c`
  (`test_zones_current_sweep_start_wired_refusals()`): uncommitted-row +
  link-down (still LINK_DOWN), uncommitted-row + link-up + peer unknown
  (CT_TOPOLOGY_UNKNOWN), uncommitted-row + link-up + peer known + refetch
  succeeds and sets the row (OK), and refetch fails (stays
  CT_TOPOLOGY_UNKNOWN, never a false OK).

## Negative test

Poisoned `zone_cfg_ct_topology_row_set()` to `return !row.set;`, deleted
`firmware/KilnFW/App/test/build`, forced a full host-test rebuild: the
`zones_http` test executable failed (only failure among 54). Restored the
source by hand, confirmed `git hash-object` matched the pre-poison hash
(a blob id captured at the time of this audit; the file has since changed,
so it no longer matches HEAD's current blob there and is not cited as a
resolvable hash), deleted the build directory again, and forced a second
full clean rebuild: 54/54 executables built and passed.
