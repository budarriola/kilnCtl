# Zone graphic — a configuration-verification instrument on the web zones page

> **Status: fully implemented, hardware verification pending.** All 5 stages
> landed 2026-09-18 — the relay device type/storage/migration data layer
> (stage 1); `"relay_types"` on the `GET`, `relay_type_N=` on the `POST`, and
> the device-type dropdown beside each unowned relay's name field feeding
> `kgDeviceType()` real types, with `UNSET` selectable and rendering as the
> unknown glyph, the array costing a measured 24 bytes with no buffer growth
> (stage 2); and the artwork, the unknown/fail-closed rules, and the badges
> with click popups (stages 3-5), covered by `tools/check_zone_graphic_render.ps1`.
> **2026-09-22:** the bench board now runs firmware that includes this page
> (`dde785bf`, the most recent commit touching these files, is an ancestor of
> the bench firmware commit `63a48ab3`); the on-hardware set/reboot/read-back
> round trip is still unexecuted — that is the only remaining item. Until
> then, the render function is verified against real captured `/api/zones`
> and `/api/status` JSON instead. **Opened:** 2026-09-18.
>
> **Visual target:** [`docs/images/zone_graphic_reference_stacked_rings.jpg`](images/zone_graphic_reference_stacked_rings.jpg),
> owner-supplied. The artwork stages build against that image rather than
> against a prose description: a vertically stacked kiln body on legs, no
> branding, one glowing ring section per zone, three rings matching the
> current three-zone setup. **The rings are the load-bearing idea — each zone
> is a ring, and the ring itself carries that zone's state**, so zone state is
> a property of the ring rather than a badge parked beside it. Ring count
> still follows `thermo_count`, never the reference image's own count.
> **Scope:** `firmware/KilnFW/App/drivers/http/zones_page.html` and the
> `GET`/`POST /api/zones` handlers, plus one storage change to the relay-names
> blob. **Out of scope, by owner decision: the on-board LCD zones page.** The
> panel is 480x320 and must not scroll, which is why it was excluded; no work
> item here may touch `App/drivers/ui/`.

## 1. What this is for, and the standard it has to meet

A cartoon of the kiln sits at the top of the web zones page. It draws exactly
as many stacked ring sections as `thermo_count`, and annotates each ring with
the heaters, current sensor and thermocouple the configuration says that zone
has, plus icons for any extra relays and for the device type each extra relay
drives. The operator glances at it and sees whether the machine's idea of the
kiln matches the kiln.

That purpose sets the acceptance standard, and it is a harsh one. **A graphic
that renders plausibly while the underlying configuration is wrong or unknown
is worse than no graphic**, because the operator stops checking the fields
below it. Section 6 is the controlling section of this plan; every rendering
decision elsewhere defers to it.

Zone physical order is fixed and the drawing must respect it: **z0 is the TOP
ring, z2 the BOTTOM.** The stack is drawn top-down in zone-index order.

Body: an octagonal brick-lined stack of ring sections on a welded tube-steel
stand, a hinged flat octagonal lid, a peephole and a small vent. **No
controller box, no brand plate, no model name, no logo, no trade dress.** The
ring count follows the configuration, never a real product's section count.

## 2. What already exists, and what the page must be built on

The page is `zones_page.html`, one self-contained HTML/CSS/JS file, 3490 lines,
embedded via ESP-IDF `EMBED_TXTFILES` in `firmware/KilnFW/App/drivers/CMakeLists.txt`,
pre-gzipped at configure time by a CMake `execute_process` step and served from
flash with `zones_http.c` checking `Accept-Encoding: gzip`. There is no inline
SVG and no canvas on the page today.

Every per-zone hardware association the graphic needs already exists as a
bitmask on `zone_cfg_t` and is already emitted by `GET /api/zones`:
`relay_mask`, `thermo_mask`, `ct_mask`. `thermo_count` is emitted too, and the
hard ceiling is `MAX31856_CHANNEL_COUNT` = 3 — **the graphic scales 1 to 3
rings, not an open-ended count.**

Extra relays are *derived, not declared.* There is no extra-relay struct
anywhere: an extra relay is any relay bit unclaimed by every zone's
`relay_mask`, computed server-side by `zone_owned_relay_mask()` and re-derived
client-side by `clientOwnedRelayMask()`. Their operator-entered labels live in
a wholly separate NVS blob, `relay_names_cfg_t`, `RELAY_NAME_MAX_LEN` 15,
`KILN_IO_RELAY_COUNT` 4.

## 3. Where the relay device type is stored, and why not in the zones blob

**Nothing in this plan changes `ZONES_CFG_VERSION`, and the relay device type
does not go into `zones_cfg_t`.**

This is a choice, not a constraint. The owner has authorized a zones-schema
bump for the per-zone CT channel field landing in parallel
(`docs/CT_CHANNEL_MASK.md`), having been shown and accepted the rollback
hazard, so a bump is available to this feature too. It is declined anyway, for
the reasons below — and the version constant is cited by name throughout this
document rather than by number, because that parallel work may move it while
this plan is still open.

**What a zones bump would cost, stated plainly, since it is now a live
option.** A bump is normal, mechanical work — a frozen `zones_cfg_vN_t`
snapshot of the outgoing layout, its `case N:` converter branch, and its entry
in `zones_cfg_expected_len_for_version()`. The cost is entirely on rollback:
an older build meeting the newer blob refuses it outright and runs that boot on
**firmware-default PID gains, with no separate warning**, flash left untouched
so reflashing the newer firmware restores everything. A firing started between
the rollback and the reflash runs on defaults.

**The graphic itself needs no new `zones_cfg_t` field at all.** Every per-zone
association it draws — `relay_mask`, `thermo_mask`, `ct_mask`, `thermo_count` —
is already stored and already served (section 4). The only genuinely new
persistent data this feature introduces is the relay device type, and that is
per-relay data, not per-zone data.

The on-flash zones blob is not tolerant JSON despite the file names. It is a
strictly-typed versioned binary struct validated by *exact byte length per
version*, so an unrecognised-key-is-ignored escape hatch does not exist.
`zones_cfg_expected_len_for_version()` (`zones_config_convert.c`) is a switch
with `default: return 0;`, and `zones_config_json_decode_blob()`
(`zones_config_migrate.c`) turns both a length mismatch and an unknown version
straight into `ZONES_DECODE_CORRUPT`:

```c
    size_t expected = zones_cfg_expected_len_for_version(version);
    if (expected == 0) {
        *reason = "unknown/unsupported zones_cfg version";
        return ZONES_DECODE_CORRUPT;
    }
    if (len != expected) {
        *reason = "blob length does not match its claimed version -- treating as corrupt";
        return ZONES_DECODE_CORRUPT;
    }
```

Growing `zones_cfg_t` by one byte therefore changes `sizeof(zones_cfg_t)`, so
the new blob no longer matches the length recorded for the current version, and
the only way to keep it loadable is to bump. Symmetrically, the
older-firmware direction is the documented rollback hazard, and the code says
so in its own words:

```c
    if (version > ZONES_CFG_VERSION) {
        /* Firmware-rollback case (TODO.md 8.1) -- this build does not know
         * that layout and must not guess at it. ... */
        *reason = "this config was saved by newer firmware -- refusing rather than guessing";
        return ZONES_DECODE_NEWER;
    }
```

An older build meeting a v27 blob refuses it outright and runs that boot on
firmware defaults — the "silently runs on firmware-default PID gains" failure
`CLAUDE.md` records. Every historical field addition bumped the version,
including the ones whose new field was safe at zero. There is no precedent for
sneaking one in, because the format does not permit it — so the real question
is not "can this avoid a bump" but "does this data belong in that blob at all".
It does not.

**The right home.** `relay_names_cfg_t` is a separate NVS blob under its own key with
its own independent version constant, declared in
`firmware/KilnFW/App/drivers/persist/zones_http_internal.h`:

```c
#define RELAY_NAMES_CFG_VERSION 1
#define NVS_KEY_RELAY_NAMES "relay_names_cfg"
NVS_KEY_LEN_CHECK(NVS_KEY_RELAY_NAMES);

typedef struct {
    uint8_t version;
    char names[KILN_IO_RELAY_COUNT][RELAY_NAME_MAX_LEN + 1];
    uint32_t crc32;
} relay_names_cfg_t;
```

The relay device type is per-relay data that belongs beside the relay name, not
beside the zone PID gains. Adding `uint8_t types[KILN_IO_RELAY_COUNT];` to this
struct bumps `RELAY_NAMES_CFG_VERSION` 1 → 2 and **leaves `ZONES_CFG_VERSION`
untouched.** That is worth having even now that a zones bump is permitted:
folding relay labels into the zones schema would put cosmetic per-relay data on
the same rollback fate as the PID gains for no gain. The blast radius stays
"relay labels and icons" instead of "the whole
commissioned zones config, including PID gains and guard thresholds" — and
relay labels are data the codebase already treats as non-safety-relevant.

**NVS key lengths, explicitly.** No new NVS key is created by this plan. The
existing `"relay_names_cfg"` is reused, and it is exactly 15 characters —
already at the ceiling, which is why a new key was never an option:
`"relay_types_cfg"` is 15 but `"relay_dev_types"`-style alternatives and
anything more descriptive overflow, and a second key would duplicate the
dual-write and rev-counter machinery for no benefit. The companion
`"relnames_rev"` (12) and the `cfg`-filesystem path `"relay_names.dat"` are
unchanged. `NVS_KEY_LEN_CHECK` already guards both keys at compile time and
keeps guarding them.

### 3.1 The migration is mandatory, and it is a named deliverable

`relay_names_validate()` (`zones_config_store.c`) checks exact length, then
version equality, then CRC32, and on **any** mismatch silently discards the
entire blob — every relay name resets to blank with no operator-visible notice:

```c
static bool relay_names_validate(const void *bytes, size_t len)
{
    if (len != sizeof(relay_names_cfg_t)) {
        return false;
    }
    relay_names_cfg_t cand;
    memcpy(&cand, bytes, sizeof(cand));
    if (cand.version != RELAY_NAMES_CFG_VERSION) {
        return false;
    }
```

Only one version of this blob has ever existed, so **no migration function
exists for it.** Bumping 1 → 2 without writing one would wipe every
operator-entered relay name on the next firmware update, silently, on every
board in the field. The migration itself is trivial — freeze the v1 layout as
`relay_names_cfg_v1_t`, accept its length, copy the names across, default every
`types[]` entry to the unset value — but it is a blocking deliverable of stage
1, not a footnote, and it needs its own host test (section 8).

Same discipline as the zones blob: freeze the old struct as a named type, never
`memcpy` one shape over another, and length-check before interpreting.

## 4. The data contract

**Decision: extend `GET`/`POST /api/zones`. No new route.** Everything the
graphic needs except one array is already on that response, the page already
fetches it on load and after every save, and a second route would double the
round trips, need its own `json_cap`, its own auth gate and its own
staleness-versus-`/api/zones` skew. The graphic must never draw from a mix of
two independently-fetched snapshots — that is a "reset one side of a pair"
shape, and it would let the picture disagree with the form directly under it.

Already present on `GET /api/zones`, and sufficient:

| Need | Field |
|---|---|
| Ring count | `thermo_count` |
| Which relays heat this zone | per-zone `relay_mask` |
| Which thermocouple channels this zone reads | per-zone `thermo_mask` |
| Which CT channels this zone uses | per-zone `ct_mask` |
| Extra relays | derived: relay bits unclaimed by any `relay_mask` |
| Extra relay labels | `relay_names[]` |
| Safety-processor wiring | `safety_wiring{link_up,tc_temp_valid,tc_fault,relay_energized,tc_is_separate_sensor}` |
| Live CT mismatch per zone | `ct_warn_mask` |
| Whether a zone's normal current was ever measured | per-zone `normal_current_measured`, `normal_current_a` |

**The one addition: `"relay_types":[t0,t1,t2,t3]`**, one small integer per
relay, mirrored by a `relay_type_N=` parameter on the POST, alongside the
existing relay-name parameters. Roughly 24 bytes on the wire.

**The `json_cap` budget is not touched.** `zones_get_handler()`'s buffer is
`const size_t json_cap = 7360;` and this plan adds a single short array to it.
**No work item here may enlarge that buffer, or any httpd stack buffer** — if a
future addition does not fit, that is a design input and the addition gets
trimmed, not the buffer grown. The reason the budget survives at all is
section 5: the artwork is generated in the browser from numbers the response
already carries, so the *graphic itself* contributes zero bytes to any JSON
response.

`GET /api/status` is already fetched by this page and supplies the
thermocouple-channel health and config-fault state used in section 7. It is
read as-is; nothing is added to it.

### 4.1 Current sensors: plan against the concept, not today's encoding

Work is concurrently reworking CT presence and the
`current_sensing_commissioned` gate in `firmware/SaftyFW/`, moving the meaning
of "is this channel fitted" from a declared topology toward a derived per-zone
selection — `docs/CT_CHANNEL_MASK.md` replaces the binary `ct_topology`
with a per-zone CT channel selection, so any split of zones across the three
channels becomes expressible. **This plan therefore specifies the CT annotation against the
concept — "which CT does this zone use, and is it fitted?" — and not against
today's `ct_topology` encoding.** The front end asks that question of one
accessor and renders three states (uses a fitted CT / uses a CT that is not
fitted / not reported); how the firmware answers it is free to change
underneath without a redraw of the artwork.

`ct_mask` overlapping between zones is legitimate and expected, and the
direction of travel makes a shared channel the *normal* case: zones sharing a
channel are summed. So the CT is **not drawn as a per-ring badge.** It is drawn
once, as a labelled bracket down the left side of the stack spanning exactly
the rings that use it, with its channel number at the bracket's elbow. One CT
covering all three rings is one tall bracket, not three identical icons
implying three sensors — which is precisely the false-confidence failure
section 6 exists to prevent.

## 5. Rendering

**Inline SVG, built in JavaScript from `thermo_count`, inside
`zones_page.html`.** Confirmed against how the page is actually stored and
served, and it is the right answer for three reasons that all point the same
way.

The page is one `EMBED_TXTFILES` blob. A separate `.svg` asset would need a new
embed entry, its own gzip step and a new HTTP route, and the CMake machinery
expects one blob per named page — so a separate asset costs firmware plumbing
to save bytes it would not actually save. Static markup for every ring count is
worse still: 1, 2 and 3-ring variants would triple the artwork and leave three
copies to drift apart. And a generated stack is the only form where "draw
exactly `thermo_count` rings" is structurally true rather than something three
hand-drawn variants have to be trusted to honour.

Mechanically: a `<svg>` shell and a `<defs>` block of `<symbol>` icons sit in
the page body as static markup; one `renderKilnGraphic(data)` function emits
the ring geometry and the per-ring annotation groups. Rings are a fixed height
each, so the viewBox grows with the count and the stand and lid are positioned
from it. Vector-drawn only — **no raster images, no data: URIs, no external
resources of any kind**: the board serves one flash-resident page and cannot
reach a CDN.

**Payload — MEASURED, and the budget is now the measurement.** The figure this
section originally carried (no more than 12 KB raw and 4 KB gzipped) was a
guess written before the feature had a shape; the delivered feature costs
roughly 2.4x that gzipped. **The budget is therefore reset to the measured
cost: +9.5 KB gzipped for the graphic, with the whole page at ~76 KB
gzipped.** Those are the numbers to hold, and future growth is measured
against them, not against the old guess.

The measurements, all `gzip -9` (the same compressor and level the CMake
configure step uses), taken over committed file content:

| Point | Raw | Gzipped |
|---|---|---|
| Before the graphic (`ed9b678c^`) | 206,166 | 66,597 |
| Graphic landed (`ed9b678c`) | 234,952 | 76,058 |
| **Delta attributable to the graphic** | **+28,786** | **+9,461** |
| Page today (`1baa828c`, relay device types) | 236,587 | 76,570 |

Re-measured 2026-09-18 by piping
`git show <rev>:firmware/KilnFW/App/drivers/http/zones_page.html` through
`gzip -9 -c | wc -c`. An earlier pass recorded +29,273 raw / +9,480 gzipped
from the on-disk working copy; the difference is line endings (the checkout is
CRLF, the committed blob LF) plus the working copy not sitting exactly at the
commit. Either way the gzipped delta is ~9.5 KB.

**Owner decision, 2026-09-18 — accept the size, keep every click-popup
explanation string.** The overrun was reviewed and accepted as delivered: the
popups are the largest single cost and are the feature rather than decoration.
**The page size is not an open defect and must not be re-opened as one** — it
is a recorded, accepted cost.

The old budget was written before the feature had a shape, and it did not survive
three of the owner's own requirements. The click-popups are the largest single
cost: the owner asked for icons that "pop up information when clicked", so each
badge carries the explanatory prose it shows, and that prose is the feature
rather than decoration. Section 6 is the second: a third rendering everywhere a
two-state rendering would have done costs roughly half again in both branches
and strings. The third is the icon set, twelve symbols rather than the nine
planned, because the six device-type glyphs must exist for the seam to close
without re-touching the artwork. A comment-trimming pass recovered about 1 KB
gzipped; the rest is not reducible without deleting one of those three.
Recorded here as a measured overrun rather than quietly restated — putting a
number in this section was worth it precisely because it found this.

The gzipped delta is what matters, since that is what is stored in flash and
pushed through the socket. The icon set is
drawn to share a single 24x24 viewBox and a single stroke width so the whole
set compresses against itself.

Theme: colours come from the existing `--ui-*` / `--ok` / `--warn` / `--bad`
custom properties the page already defines, so the graphic follows dark and
light mode with no second palette. **Colour is never the sole carrier of
meaning** — every state also has a distinct glyph and a text label, because a
red-versus-amber distinction on a workshop screen is not a safety-grade
channel, and this project has already closed one "status colour contrast is
impossible" investigation.

### 5.1 The icon set

Nine icons, all hand-drawn inline SVG `<symbol>`s in the page, 24x24, single
stroke weight, no fills that depend on theme:

- **heater** — a coil element, with the relay count for that zone as a numeral
  beside it
- **current sensor** — a split-core clamp around a conductor, at the elbow of
  the span bracket
- **thermocouple** — a probe with a junction bead, channel number beside it
- **damper**, **outlet**, **valve**, **fan**, **light**, **other** — one per
  relay device type

Plus **error** and **warning** — an octagon and a triangle respectively, so
they differ in silhouette and not only in colour — and one **unknown** glyph, a
hollow dashed square, which section 6 spends itself on.

### 5.2 Numbers, not just icons

The owner asked for icons *and numbers*, and the numbers are the half that
makes it a verification instrument. Each ring carries the zone index and name,
the heater relay count, the thermocouple channel number(s) and the CT channel
number. A zone reading two thermocouple channels shows both numbers; it does
not show one icon and hide the discrepancy.

## 6. Drawing "unknown" so it can never read as "healthy"

**This is the controlling section.** The failure this feature can cause is not
a crash, it is a confident picture of a configuration nobody verified. Three
rules, and they are testable.

**Rule 1 — absent is not zero, and unknown is not absent.** Every annotation
has three renderings, never two: *configured* (solid icon plus number),
*deliberately none* (the icon's outline, struck through, with the word "none"),
and *not reported* (the dashed hollow unknown glyph plus "?"). A zone with no
CT assigned and a zone whose CT state the firmware did not report must look
different at a glance. The existing JSON already honours this distinction where
it matters — `normal_current_measured` is emitted as a separate boolean
precisely so an unmeasured zone is not rendered as 0.0 A, and
`safety_wiring.tc_temp_valid` and the `ceiling_pico_known` flag exist for the
same reason. The graphic must not collapse any of them.

**Rule 2 — a missing field is unknown, never a default.** The render function
takes no default for any hardware association. If a key is absent from the
response, the annotation renders unknown. It must not fall back to a
plausible-looking value, and specifically it must not reuse
`zones_page.html`'s existing client-side default-zone object, which fills
`thermo_mask: (1 << i)` for the editing form. That default is correct for a
form the operator is about to fill in and is actively wrong for an instrument
that claims to report what is stored.

**Rule 3 — the whole graphic fails closed.** If `GET /api/zones` fails, or
returns something the render function cannot parse, or `zones_config_valid` is
false, or a config load fault is reported, the artwork is **not drawn at all**.
In its place goes a plain bordered panel reading that the configuration could
not be read and the graphic is therefore not shown, with the underlying reason.
A partially-drawn kiln with three healthy-looking rings on a board that failed
to load its config is the exact outcome this feature must never produce.
Stale data is covered by the same rule: the graphic is re-rendered from each
fetch's result, and it is cleared to the fail-closed panel at the *start* of a
refetch, not left showing the previous snapshot while a new one is in flight.

A negative test enforces all three (section 8).

## 7. Errors and warnings

Each badge names where it comes from, what it is scoped to, and what its
click-popup says. Badges attach only at the scope their source actually has —
a per-ring badge for a global fault is a lie about which zone is in trouble.

| Badge | Source | Scope | Popup shows |
|---|---|---|---|
| Thermocouple fault | `/api/status` per-channel `fault_status`, `spi_failed`, `stale`, `valid` | per channel → drawn on the ring(s) whose `thermo_mask` selects it | decoded fault bits, last valid reading and its age, SPI state |
| Safety thermocouple fault | `safety_wiring.tc_fault` in `/api/zones` | the safety processor's own sensor — **global**, drawn on the stand, not a ring | fault value, `tc_temp_valid`, whether it is a separate sensor |
| CT mapping mismatch | `ct_warn_mask` in `/api/zones` | per zone — a warning, never a trip | that the zone is commanded on and its live current disagrees with its measured normal; the measured normal and when it was taken |
| Normal current never measured | per-zone `normal_current_measured` false | per zone | that the CT check for this zone is dormant until the sweep is run, with a link to the sweep panel already on this page |
| Safety link down | `safety_wiring.link_up` | **global** | that no safety-processor data is current; every safety-sourced badge on the graphic is simultaneously downgraded to unknown |
| Safety trip latched | `/api/status` trip state, `trip_reason`, `trip_mask`, cause/remedy strings | **global — one state machine** | the existing cause and remedy strings, unmodified |
| Config invalid / load fault | `/api/status` `zones_config_valid`, the `zones_config_load_fault` family | **global** | triggers the rule-3 fail-closed panel instead of a badge |

**Deliberate consequence: there is no per-ring trip badge.** Trip state is one
global state machine with a single `trip_reason`; drawing it against one ring
would invent a per-zone attribution the firmware does not have. It renders as a
banner across the whole graphic.

**Two faults the front end cannot currently see, stated plainly rather than
faked.** Neither is fixed by this plan.

1. **There is no genuine per-zone heater-load fault.** Grepping the per-zone
   status array and the current-sweep task, the only live per-zone signal about
   a heater actually drawing current is `ct_warn_mask`, which compares live
   current against a *previously measured normal* and is therefore silent on
   any zone whose normal was never measured. `zones_config_load_fault` is about
   the config blob, not about a heater. So "this zone's element is open" is not
   a state the graphic can show; the closest honest rendering is the
   never-measured badge above, which says the check is dormant rather than
   implying it passed. On this roughly 4 W bench fixture every zone's current
   is structurally below the sweep's noise floor, so `ct_warn_mask` cannot
   assert here at all — the badge will be permanently dormant on the bench and
   only becomes live on a kiln-scale load. See open question 3.
2. **There is no live per-channel CT presence or calibration-health flag.**
   What is exposed is configuration and provenance —
   `ct_installed`/`ct_topology`, and `/api/safety/config`'s `ct_cal[ch]` with
   `has_value` and `source`. A channel with a calibration recorded and no CT
   physically on it reads identically to a healthy one. The graphic therefore
   annotates the CT bracket with *configured and calibration-provenance* state
   only, and labels it as such in the popup — it must not claim the sensor is
   present. This is also why section 4.1 plans against the concept: the
   concurrent CT work is the thing that may eventually make a real presence
   answer available, at which point the bracket gains a fourth state and
   nothing else changes.

## 8. Staging

Each stage is independently verifiable and independently shippable. Nothing in
stage 1 depends on any artwork existing.

**Stage 1 — relay device type storage. LANDED 2026-09-18.** `relay_device_type_t`
enum (unset, damper, outlet, valve, fan, light, other — `unset` is enum 0 per
the owner answer in section 10), the `types[]` array on `relay_names_cfg_t`,
`RELAY_NAMES_CFG_VERSION` 1 → 2, the frozen `relay_names_cfg_v1_t` and its
migration, and the dual-write path to the `cfg` filesystem kept in step.
*Verified by:* host tests — a v1 blob migrates with every name preserved and
every type defaulted; a v2 blob round-trips; a truncated or CRC-broken blob is
still refused. Plus a build.

Three things stage 2 needs to know about how stage 1 actually landed:

- **The accessors are named `zones_config_get_relay_device_type()` /
  `_set_relay_device_type()`, not `..._relay_type()`.** That shorter name was
  already taken, and by something unrelated:
  `zones_config_get/set_relay_type(zone_index, uint8_t)` is the *zone's
  switching hardware* (SSR / contactor / mercury) that drives
  `relay_cycles_budget()`. Two different meanings of "relay type" now coexist
  in this codebase — the switching hardware per zone, and the driven device
  per extra relay. The collision was caught only by a duplicate-definition
  compile error. Stage 2's JSON field is still `relay_types`, per section 4,
  but any new *function* here needs the longer name.
- **Version discrimination is by byte length**, which is why
  `sizeof(relay_names_cfg_v1_t) != sizeof(relay_names_cfg_t)` (72 vs 76) is
  pinned by a `_Static_assert`. A future field that accidentally equalised the
  two sizes would make v1 padding readable as `types[]`; it now fails to
  compile instead. The NVS read path passes the blob's *actual stored length*,
  never `sizeof(raw)`, for the same reason.
- **The `cfg` filesystem needed its own separate v1 migration.** The generic
  `pref_cfg_fs` bridge is parameterised by one fixed item size and rejects any
  file whose length is not `4 + item_size`, so a v1-length file would have been
  dropped silently — the same data-loss hazard as the NVS one, through the
  other door. `relay_names_load()` therefore runs a v1 file pre-pass that
  upgrades the file in place *at the same rev*, before `pref_cfg_fs_resolve()`
  runs, leaving the divergence tie-break untouched. It has its own host test.
  Inert on every board today, since no board mounts `cfg` yet.

**Stage 2 — the type on the wire and in the form. LANDED 2026-09-18.**
`"relay_types"` on the `GET`, `relay_type_N=` on the `POST`, and a dropdown
beside each unowned relay's name field (a zone-owned relay echoes its stored
type in a hidden field instead, so no page can wipe it). An out-of-range or
non-numeric `relay_type_N` is refused with a 400 and commits nothing — never
coerced to `UNSET`, which is itself a legitimate selectable, persisted value
rendering as the unknown glyph. Host tests cover every enum value's
serialization, the `POST`→`GET` round trip, omitted-means-keep, and the
refusal. *Still owed:* the on-hardware half of the original verification — set
a type over HTTP, reboot the board, read it back unchanged — which needs a
board flashed with this firmware.

**Stage 3 — the artwork, static states only.** The SVG shell, the icon set, the
render function, ring count from `thermo_count`, heater/thermocouple/CT
annotations, extra-relay icons by type. No badges yet. *Verified by:* rendering
at 1, 2 and 3 zones and confirming ring count and top-to-bottom zone order
against the form below it; and the gzipped-size budget from section 5.

**Stage 4 — unknown and fail-closed.** Rules 1 to 3 of section 6, including the
fail-closed panel. *Verified by:* the negative test below. **Stage 4 is not
optional and may not be deferred past stage 3 into a later release** — stage 3
alone is exactly the plausible-but-unverified graphic this feature exists to
avoid, so stages 3 and 4 ship together or not at all.

**Stage 5 — badges and popups.** Section 7's table, each badge at its true
scope. *Verified by:* driving each source to its fault state where the bench
permits, and confirming the two unreachable ones render as dormant rather than
healthy.

## 9. Test strategy

Host tests for everything that is not the browser: the relay-names migration
(stage 1), the `json_cap` worst-case fit, and the `GET`/`POST` round trip for
`relay_types`, alongside the existing zones-HTTP tests. The migration test is
the one that earns its keep — it is guarding operator-entered data against a
silent wipe.

The rendering is JavaScript in an embedded page, which this repo has no browser
test harness for. Rather than inventing one, **the render function is written
as a pure function from the parsed JSON to an SVG string**, with the DOM
insertion separate. That makes the interesting half testable by a check script
that feeds it fixture JSON and asserts on the output string — ring count, zone
order, and the presence of the unknown glyph — without a browser.

### 9.1 The negative test

The one that matters is for section 6, because section 6 is the requirement
that can pass vacuously. **Feed the render function a response with the CT and
thermocouple associations deleted, and assert the output contains the unknown
glyph and does not contain a configured-state icon.** A correct implementation
fails this test the moment rule 2 is weakened to a default — which is the exact
regression to guard, since a well-meaning "sensible fallback" is how this
feature turns into a confident lie.

Second negative test, for rule 3: feed it a response with `zones_config_valid`
false and assert **no `<svg>` is emitted at all**, only the fail-closed panel.

Both must be negative-tested themselves: deliberately break the render function
in the corresponding way, confirm each test goes red, then **restore by hand
and force a full rebuild before measuring anything** — an empty `git diff`
proves the source is restored and says nothing about a stale build artifact,
which this project has already been bitten by.

## 10. Owner answers — all three settled 2026-09-18

These are decided. They are recorded here so they are not re-litigated by a
later stage; each recommendation was accepted as written.

1. **What does an un-set relay type look like? — ANSWERED: it gets its own enum
   value 0, spelled `unset`.** A migrated board must render as "nobody has told
   me what this relay does", never as a silent claim of `other`, because
   `other` is a choice the operator made and a never-configured relay is not
   the same thing. **Implemented in stage 1**: `RELAY_DEVICE_TYPE_UNSET = 0`,
   which is deliberately the value a migrated v1 blob, a never-configured
   relay and a `memset`-zeroed struct all land on. The getter also degrades an
   unrecognised stored number to `UNSET` rather than surfacing a value with no
   icon (reachable only by rolling back past a future appended enum value),
   per section 6. The enum is fixed and code-defined, never
   operator-extensible, and its numbers are on flash, so values may only ever
   be appended — never reused.
2. **Is a global safety trip drawn as a banner across the whole graphic? —
   ANSWERED: yes, one banner across the whole graphic, never a per-zone
   badge.** Trip state is one state machine and per-ring attribution would be
   invented. Stage 5 work; recorded here so section 7's only judgement call
   stays closed.
3. **Is a permanently dormant heater-load badge acceptable on the bench? —
   ANSWERED: yes, ship it dormant and labelled "not measured", do not defer
   it.** Dormant-and-labelled is the accurate rendering on this roughly 4 W
   fixture; suppressing the badge would itself be a silent gap. Stage 5 work.
