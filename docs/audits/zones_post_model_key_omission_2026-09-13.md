# Audit: does an ordinary operator save silently delete a zone's plant model?

2026-09-13. Follow-up to a passing remark in `1142c73b` (adversarial review
of the adaptive_tune confidence-authority design), which noted, while
examining `zones_http_post_parse.c` for something else, that `model_k_dc` is
operator-writable via a `z%u_k` POST key and that the file's own comments
describe an "omit-deletes-the-model" edge. That remark was never itself
investigated. This audit does that.

## 1. What the source actually does

`firmware/KilnFW/App/drivers/http/zones_http_post_parse.c`'s per-zone parser
(`zones_http_parse_zone_fields()`) builds each in-range zone into a
caller-zeroed scratch struct (`zones_post_handler()`'s `memset(&tmp, 0,
...)`), field by field. Every field with no POST key of its own, or whose
key can legitimately be absent, needs an *explicit* line carrying the value
forward from `current_z` (the live stored zone) — otherwise it silently
becomes 0 for every save, regardless of whether the operator touched it.

The three model fields, `:701-730`:

```c
snprintf(key, sizeof(key), "z%u_k", i);
{
    char probe[24];
    if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
        if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_K_MAX, &z->model_k_dc)) {
            *err_reason = "zone model K out of range";
            return false;
        }
    }
}
```

(same shape for `z%u_tau` -> `model_tau_s` and `z%u_deadtime` ->
`model_dead_time_s`). **There is no `else` branch carrying `current_z`
through.** If the key is present, it parses and validates it. If the key is
absent, `z->model_k_dc` (etc.) is simply left at whatever the caller
zero-initialized it to: **0.0**, which is this field's own documented "no
model" sentinel (`zones_config_set_model()`'s own convention, referenced at
line 698). So: **omitting `z%u_k`/`z%u_tau`/`z%u_deadtime` does not preserve
the existing model — it deletes it**, on every single whole-page save that
doesn't happen to include those three keys.

The file's own comment (`:682-696`) says exactly this, in its own words:

> "The whole-page-submit semantics have real teeth here, though. These
> numbers are NOT typed by an operator; they are measured by a multi-hour
> step test. A client that omits them silently deletes that measurement,
> because z is zero-initialized by the caller and zero is the "no model"
> encoding. That is the established behaviour of this endpoint and is left
> as-is rather than special-cased into a merge-on-omit, which would make
> this one field group behave unlike every other one on the page — but it is
> why zones_page.html reads these back from GET and posts them straight
> through untouched, and why anything else driving this endpoint must do
> the same."

**The code matches the comment exactly.** This is not a discrepancy between
stated and actual behaviour — it is a deliberate, load-bearing design
decision, made and documented by whoever wrote this file, that trades
"could silently delete a real measurement" for "every field on this page
behaves the same way as every other field" (all-or-nothing whole-page
submit), on the explicit condition that the one client this endpoint is
designed for (`zones_page.html`) never actually omits these three keys. The
sharp edge is real; it is knowingly load-bearing rather than an oversight.

A second comment, at `:731-746`, right after the model block, contrasts it
explicitly with the *next* three optional fields (`fuzzy_strength_pct`,
`coupling_coeff[]`) which DO use omit-preserves, and calls out the model
block by name as "this file's OWN documented sharp edge, not a precedent to
repeat."

## 2. Can the shipped UI actually trigger it?

The question that matters is not "can a hand-crafted POST omit these keys"
(trivially yes) but "does any shipped client ever send a whole-page save
that omits them."

**`firmware/KilnFW/App/drivers/http/zones_page.html`**, `:2182-2187`:

```js
/* Echoed back exactly as loaded -- MEASURED per zone (autotune's plant
   model, PID_EXPANSION_PLAN.md 2c's coupling coefficient), never part of
   the inherited stack, so always this zone's own, not the terminal's. */
params.push('z' + i + '_k=' + div.dataset.modelK);
params.push('z' + i + '_tau=' + div.dataset.modelTau);
params.push('z' + i + '_deadtime=' + div.dataset.modelDead);
```

`div.dataset.modelK/modelTau/modelDead` are populated when the page loads
each zone's block from `GET /api/zones` and are never left blank or skipped
— every render of the save button's parameter list includes all three keys,
for every zone the page renders a block for. **The shipped web UI has no
code path that omits these three keys from a save.** There is no partial
"just change one field and submit" form on this page either — the save
button always serializes the whole per-zone parameter list built by this
same function.

**`tools/PcTools/src/kilnctrl/zones_http_client.py`**'s `_encode_zone()`
(`:520-574`) takes the opposite, and even stricter, approach: it iterates
every key present in the *live* zone dict (as returned by `GET /api/zones`)
and looks each one up in `_ZONE_FIELD_FORM_KEY`; an unrecognised key raises
`ZonesHttpUnknownFieldError` rather than being silently dropped (`:567-571`).
`model_k_dc`/`model_tau_s`/`model_dead_time_s` are in that mapping
(`:225-227`, to `"k"`/`"tau"`/`"deadtime"`). So a whole-zone write built by
this module for *any* Python-side caller (`apply_zone_preset()` etc.) also
always re-encodes all three model keys from whatever it read — it is
structurally incapable of silently dropping a key it doesn't recognise.

There is also no narrower "set just this one field" HTTP endpoint for the
model triple — the only other writer is `zones_config_set_model()`, called
directly from `backup_import.c` (full-triple import) and from
`autotune_engine_guard.c`'s accept path (which always sets all three
together after a completed step test). Neither goes through
`zones_http_post_parse.c` at all.

**Conclusion for item 2, stated separately as asked:**
- *Can a hand-crafted POST trigger it?* Yes, trivially — omit `z0_k`,
  `z0_tau`, `z0_deadtime` from an otherwise-valid whole-page body and the
  model is deleted.
- *Can the shipped UI (web page or PC/MCP client) trigger it?* No. Both
  shipped writers always re-serialize all three keys from what they most
  recently read, by construction, not by convention someone could forget —
  the web page hardcodes the three `params.push()` lines unconditionally,
  and the Python client's whole-zone encoder has no way to drop a
  recognised key silently (it would raise on an *unrecognised* one, but
  these keys are always present in the dict it iterates).

So the review's remark was correct about the code's behaviour, but the
follow-up question it left open — "does this matter in practice" — resolves
to "not currently, because both real clients happen to always echo these
fields back," which is exactly what the code comment already claimed.

## 3. Live board state (read-only)

Read via `kiln_call(name="control_get_zones")` (MCP `kilnctrl` server) —
no writes performed. This tool's UART-sourced per-zone summary
(`Kp`/`Ki`/`Kd`/`cal`/`ramp`/`range`) does not carry HTTP-only fields at
all, and — a tooling gap surfaced by this audit, see item 7's coda below —
`control_get_zones`'s own HTTP-sourced section (`_describe_coupling_matrix`/
`_describe_http_only_zone_fields` in `mcp_server_control.py`) does not
surface `model_k_dc`/`model_tau_s`/`model_dead_time_s`/`tuning_valid`
either, even though its docstring promises "every zone's current PID/model
config" and the raw `GET /api/zones` response it already fetches
(`zones_http_client.get_zones()`) does contain them. No currently-published
MCP tool renders those four fields directly, so this audit could not read
back their exact values without either adding a tool (out of this task's
declared ownership — `zones_http_post_parse.c` and its tests only) or
issuing an HTTP call outside the MCP facade, which the project's standing
instruction is to avoid.

What the board's live PID gains DO confirm: all three zones carry non-zero,
non-default-looking gains (`z0: Kp=0.0371 Ki=0.00015 Kd=0.7476`, `z1:
Kp=0.0639 Ki=0.00025 Kd=0.9989`, `z2: Kp=0.0703 Ki=0.00028 Kd=0.9126`), and
`coupling_diag_k_dc` — a related but DISTINCT field from `model_k_dc` (the
coupling matrix's own measured diagonal, not the feedforward model) — reads
`z0=42.7310 z1=32.3969 z2=33.8493`, all real measured values, not the 0.0
"unmeasured" sentinel. This is consistent with (but not proof of) these
zones also still holding real `model_k_dc`/`tau`/`dead_time` values, since
both are produced by the same autotune history and neither has apparently
been zeroed by an intervening bare-bones save. It is not a substitute for
reading the actual fields; recommend a follow-up tool
(`control_get_zones` extended to print them, or a new read-only
`control_get_zone_model`) so this doesn't require going outside the MCP
facade next time.

## 4. Host test added

`firmware/KilnFW/App/test/test_zones_http.c`,
`test_post_omitting_model_fields_deletes_them()` (registered in `main()`
alongside the other `parse_zone_fields` tests), exercises
`zones_http_parse_zone_fields()` directly (the real parser, not a
reimplementation), via the file's existing `post_body_with_extra()` helper
and `make_stored_zone()` fixture (which already seeds `model_k_dc=12.0`,
`model_tau_s=300.0`, `model_dead_time_s=30.0` before this audit — no
existing test exercised the omit case against it, only the narrow
`/api/zones/pid` endpoint's own confirmation that it ignores `z0_k`/`z0_tau`
entirely, which is a different code path).

The new test asserts, and confirms passing against the current unmodified
source:
- Omitting all three keys from an otherwise-valid whole-page body is
  **accepted** (not refused) and reads back with all three model fields at
  **0.0** — the deletion, pinned down as a real assertion rather than a
  comment.
- An explicit `z0_k` alongside omitted `z0_tau`/`z0_deadtime` is honoured
  for the field sent and independently zeroes the two that were not — each
  key is evaluated on its own presence, confirming this is a per-field rule
  and not a single all-or-nothing group.
- Re-posting all three keys with their prior values (`zones_page.html`'s
  actual behaviour) round-trips the model unchanged — the reason the
  shipped UI does not hit the edge in practice.

Ran via `firmware/KilnFW/App/test/build_host_tests.ps1`
(`kilnctl_host_tests_zones.exe`): **1832/1832 checks passed, all passed**,
including the three new checks above. No negative-test/break-then-fix cycle
was performed, because item 6 below applies: this is documented, intentional
behaviour that this audit is confirming rather than a defect being fixed —
breaking it on purpose to watch the new test fail would only be proving that
`current_z` carry-through is missing, which everyone (including the file's
own comments) already knows.

## 5/6. Was anything broken? Was anything fixed?

**Nothing was fixed.** Per item 6 of this task's brief: omitting
`model_k_dc`/`model_tau_s`/`model_dead_time_s` does NOT correctly carry the
existing value through, but this is not the "reset one side of a pair" bug
class this repo has hit four times before — those were unintentional,
undocumented, silent breaks in a producer/consumer relationship. This one
is the opposite: **the file explicitly documents, in its own words, exactly
the behaviour this audit confirmed, exactly why it exists (uniform
whole-page-submit semantics across every field), and exactly which client
behaviour makes it safe in practice (always echo these three keys back).**
Changing it now to omit-preserves would itself be a design change, not a
bug fix — and per the file's own words, would make this one field group
behave differently from `z%u_xzone` (:663-680) and would need its own new
reasoning for how a client update, an old client, or a hand-typed omission
should be told apart from a deliberate clear.

The correct action here is exactly what item 6 describes: report the
finding plainly, pin it down with a real test that would catch a future
accidental regression in either shipped client (should either ever stop
echoing the three keys back), and change nothing else. That is what this
audit and its test do.

## 7. Siblings checked

Every other model/tuning-adjacent field in `zones_http_post_parse.c` was
checked for the same "present optional field with no explicit `current_z`
carry-through" gap:

| Field | POST key | Omit behaviour | Correct? |
|---|---|---|---|
| `model_k_dc` | `z%u_k` | **deletes (0.0)** | Documented sharp edge, not a bug — see above |
| `model_tau_s` | `z%u_tau` | **deletes (0.0)** | Same |
| `model_dead_time_s` | `z%u_deadtime` | **deletes (0.0)** | Same |
| `coupling_diag_k_dc` | `z%u_coupling_diag_k_dc` | preserves (`current_z`, `:772`) | Yes |
| `ease_off_window_mult` | `z%u_easeoffmult` | preserves (`:801`) | Yes |
| `approach_rate_cap_c_per_hr` | `z%u_approachratecap` | preserves (`:828`) | Yes |
| `error_band_c` | `z%u_errorband` | preserves (`:855`) | Yes |
| `rate_band_c_per_s` | `z%u_rateband` | preserves (`:868`) | Yes |
| `progress_band_c` | `z%u_progressband` | preserves (`:885`) | Yes |
| `fuzzy_strength_pct` | `z%u_fuzzy_strength` | preserves (`:755`) | Yes |
| `coupling_coeff[]` | `z%u_coupling_c%u` | preserves (per-cell) | Yes |
| `tuning_valid`/`tuning_method`/`tuning_rule`/`tuning_settled`/ `tuning_extrapolation_converged`/`tuning_tau_consistent`/ `tuning_baseline_c`/`tuning_step_ambient_c`/`tuning_raw_rise_c`/ `tuning_rise_inf_c`/`tuning_seq` | none (read-only) | preserves (`:443-453`), invalidated only on an actual gain change (`:490-493`) | Yes |
| `model_fit_temp_c` / `model_fit_ambient_c` | none | preserves (`:464-465`) | Yes |
| `autotune_baseline_k_dc` | none | preserves (`:478`) | Yes |
| `adaptive_tune_enabled` | none | preserves (`:489`) | Yes |

**Finding: `model_k_dc`/`model_tau_s`/`model_dead_time_s` are the ONLY three
fields in this file with omit-deletes semantics; every other model- or
tuning-adjacent field — including several added later, after this comment
was written — correctly uses the omit-preserves carry-through.** This
confirms the file's own framing: the model triple is a single, deliberate,
called-out exception, not the first instance of a wider pattern that was
merely caught late in three places and missed in a fourth. No sibling gap
was found.

## Verified hashes

- `36f88d62bf1495ddaa43334898f7d944463155a6` — `git cat-file -t` -> `commit`
  (last touch of `zones_http_post_parse.c` before this audit, adding the
  `autotune_baseline_k_dc` carry-through referenced in section 1).
- `63ceb361` — `git cat-file -t` -> `commit` (last touch of
  `zones_page.html`).
- `dbd8ff52` — `git cat-file -t` -> `commit`.
- `1142c73b` — `git cat-file -t` -> `commit` (the adversarial review that
  raised this question).

## Bottom line

- Omitting `z%u_k`/`z%u_tau`/`z%u_deadtime` from a whole-page zones POST
  **does delete the stored plant model** (reads back as 0.0/0.0/0.0),
  exactly as the file's own comment says.
- Neither shipped client — `zones_page.html`'s save button nor
  `tools/PcTools/src/kilnctrl/zones_http_client.py`'s whole-zone
  encoder — can produce a POST that omits these keys; both always
  re-serialize them from the most recently read state.
- The live board's three zones show non-zero, non-default PID gains and a
  non-zero measured `coupling_diag_k_dc` for every zone, consistent with
  intact plant models, though no currently-published MCP tool renders
  `model_k_dc`/`model_tau_s`/`model_dead_time_s`/`tuning_valid` directly —
  a follow-up tooling gap, not this audit's defect to fix.
- No sibling field has the same gap; every other model/tuning field
  correctly preserves on omit.
- Nothing was changed in production code. A new host test
  (`test_post_omitting_model_fields_deletes_them`) pins the documented
  behaviour down as an executable assertion, all 1832 host-test checks in
  this executable pass, and this document is the requested writeup.
