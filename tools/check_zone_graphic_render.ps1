# check_zone_graphic_render.ps1 -- the kiln graphic renders what the config says
#
# The graphic at the top of zones_page.html is a CONFIGURATION-VERIFICATION
# instrument (docs/ZONE_GRAPHIC_PLAN.md). Its whole value depends on it never
# rendering plausibly when the configuration is unknown or bad, because an
# operator who trusts a confident-looking picture stops reading the fields
# below it. That property cannot be established by looking at the picture --
# a wrong picture looks exactly as convincing as a right one -- so it is
# asserted mechanically here instead.
#
# zones_page.html is a single embedded asset with no module system, so this
# script extracts the render function from the page by its two sentinel
# comments (KILN_GRAPHIC_PURE_BEGIN / _END) and evaluates that block in node.
# The block is a PURE function of parsed JSON to an HTML string, which is the
# reason the DOM half lives outside the sentinels; this script also asserts
# that the extracted block references no DOM or network API, so that purity
# cannot quietly rot.
#
# Fixtures below are shaped after real captures from the bench board's
# GET /api/zones and GET /api/status, plus hand-built variants for states the
# bench cannot produce (extra relays, a live trip, an invalid config).
#
# Exit: 0 pass, 1 fail, 3 SKIP (node not installed).

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$page = Join-Path $repo 'firmware\KilnFW\App\drivers\http\zones_page.html'

if (-not (Test-Path $page)) {
  Write-Output "FAIL: zones_page.html not found at $page"
  exit 1
}

$node = Get-Command node -ErrorAction SilentlyContinue
if ($null -eq $node) {
  Write-Output "SKIP: node is not installed, so the kiln graphic render function cannot be evaluated."
  exit 3
}

$src = Get-Content $page -Raw
$beginTag = '/* KILN_GRAPHIC_PURE_BEGIN */'
$endTag = '/* KILN_GRAPHIC_PURE_END */'
$b = $src.IndexOf($beginTag)
$e = $src.IndexOf($endTag)
if ($b -lt 0 -or $e -lt 0 -or $e -le $b) {
  Write-Output "FAIL: could not find the KILN_GRAPHIC_PURE_BEGIN/_END sentinels in zones_page.html."
  Write-Output "      The kiln graphic's render function must stay extractable, or this check is vacuous."
  exit 1
}
$block = $src.Substring($b + $beginTag.Length, $e - $b - $beginTag.Length)

# Purity guard: anything DOM- or network-shaped inside the sentinels would make
# the block unevaluatable here and this check would silently stop covering it.
foreach ($bad in @('document.', 'window.', 'fetch(', 'localStorage', 'setInterval', 'Date.now')) {
  if ($block.Contains($bad)) {
    Write-Output "FAIL: the pure render block references '$bad'. Everything between the sentinels must be a"
    Write-Output "      pure function of parsed JSON; move that code outside KILN_GRAPHIC_PURE_END."
    exit 1
  }
}

$tests = @'

function fail(msg) { console.log('FAIL: ' + msg); process.exitCode = 1; }
function must(cond, msg) { if (!cond) { fail(msg); } }
function countOf(s, re) { var m = s.match(re); return m ? m.length : 0; }

function zone(i, opts) {
  var z = { index: i, name: 'Z' + i, relay_mask: (1 << i), thermo_mask: (1 << i),
            ct_mask: 0, normal_current_measured: false };
  for (var k in (opts || {})) { z[k] = opts[k]; }
  return z;
}
function cfg(n, opts) {
  var zs = [];
  for (var i = 0; i < n; i++) { zs.push(zone(i)); }
  var c = { thermo_count: n, relay_count: n, ct_warn_mask: 0, zones: zs,
            relay_names: ['Relay0', 'Relay1', 'Relay2', 'Relay3'],
            safety_wiring: { link_up: true, tc_fault: 0, tc_temp_valid: true } };
  for (var k in (opts || {})) { c[k] = opts[k]; }
  return c;
}
function status(opts) {
  var st = { zones_config_valid: true, zones_config_load_fault: false,
             diag_ever_received: true, diag_state: 2, diag_age_ms: 120,
             channels: [0, 1, 2].map(function (c) {
               return { channel: c, temp_c: 21.5, cj_c: 22.0, valid: true,
                        fault_status: 0, spi_failed: false, stale: false, age_ms: 300 };
             }) };
  for (var k in (opts || {})) { st[k] = opts[k]; }
  return st;
}

/* ---- 1. The ring count is driven by configuration, never hardcoded ---- */
[1, 2, 3].forEach(function (n) {
  var out = renderKilnGraphicHtml(cfg(n), status());
  must(countOf(out, /class="kg-body"/g) === n,
    'a ' + n + '-zone config must draw exactly ' + n + ' ring sections, got ' +
    countOf(out, /class="kg-body"/g));
  /* The small orange heater indicator stays one-per-ring as well. */
  must(countOf(out, /class="kg-port"/g) === n,
    'a ' + n + '-zone config must draw exactly ' + n + ' heater ports, got ' +
    countOf(out, /class="kg-port"/g));
});

/* ---- 1b. The large orange fills are GONE (owner request, 2026-09-18) and the
   lid is drawn in the body's own projection: same centre axis and same rx/ry
   as a ring arc, so it sits on the kiln instead of floating above it. ---- */
(function () {
  var out = renderKilnGraphicHtml(cfg(3), status());
  must(out.indexOf('kg-glow"') < 0,
    'the large orange ring fills must not be drawn any more');
  var lid = /<ellipse class="kg-lid-top" cx="(\d+)" cy="(\d+)" rx="(\d+)" ry="(\d+)"/.exec(out);
  must(lid !== null, 'the lid top face must be drawn');
  var body = /<path class="kg-body" d="M(-?\d+),(\d+) v(\d+) a(\d+),(\d+)/.exec(out);
  must(body !== null, 'a ring body must be drawn');
  /* Same rx/ry as the body arc == same perspective; same centre axis. */
  must(lid[3] === body[4] && lid[4] === body[5],
    'the lid must use the body arc rx,ry (' + body[4] + ',' + body[5] +
    '), got ' + lid[3] + ',' + lid[4]);
  must(parseInt(lid[1], 10) === parseInt(body[1], 10) + parseInt(body[4], 10),
    'the lid must share the body centre axis');
})();

/* ---- 2. Zone order is z0 at the TOP, z2 at the BOTTOM ---- */
(function () {
  var out = renderKilnGraphicHtml(cfg(3), status());
  var i0 = out.indexOf('>z0 '), i1 = out.indexOf('>z1 '), i2 = out.indexOf('>z2 ');
  must(i0 >= 0 && i1 > i0 && i2 > i1, 'zone labels must be emitted z0, z1, z2 in order');
  /* The labels carry a y coordinate; z0's must be the smallest (topmost). */
  var ys = [];
  var re = /<text class="kg-zonelabel" x="345" y="(\d+)"/g, m;
  while ((m = re.exec(out)) !== null) { ys.push(parseInt(m[1], 10)); }
  must(ys.length === 3 && ys[0] < ys[1] && ys[1] < ys[2],
    'z0 must be drawn above z1 above z2 (y increases downward), got ' + JSON.stringify(ys));
})();

/* ---- 3. NEGATIVE TEST A (plan section 6 rule 2): a missing field is UNKNOWN,
   never a default. This is the failure the graphic exists to prevent: the page
   already has a client-side default zone with thermo_mask = (1 << i), and
   reusing it here would draw a confident "TC ch 0" for a zone the board never
   described. ---- */
(function () {
  var c = cfg(3);
  delete c.zones[0].thermo_mask;
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('Thermocouple assignment unknown') >= 0,
    'a zone with no thermo_mask must raise the unknown badge');
  must(out.indexOf('TC ch not reported') >= 0,
    'a zone with no thermo_mask must render its channel as "not reported"');
  /* z0 would default to channel 0 under the form's default object. */
  must(out.indexOf('TC ch 0') < 0,
    'a zone with no thermo_mask must NOT be drawn as channel 0 -- that is the form default, not stored data');
})();
(function () {
  /* A zone missing from the array entirely is unknown, not absent. */
  var c = cfg(3);
  c.zones = c.zones.filter(function (z) { return z.index !== 1; });
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('z1 Zone 1 (not reported)') >= 0,
    'a zone the response never described must be labelled "not reported"');
})();
(function () {
  /* Deliberately-none must be visibly different from unknown (three states). */
  var c = cfg(3);
  c.zones[0].thermo_mask = 0;
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('TC ch none') >= 0, 'an empty thermo_mask must render as "none"');
  must(out.indexOf('No thermocouple channel assigned') >= 0,
    'an empty thermo_mask must raise the no-channel badge');
  must(out.indexOf('TC ch not reported') < 0,
    '"none" and "not reported" must not collapse into the same rendering');
})();

/* ---- 4. NEGATIVE TEST B (rule 3): the whole graphic fails closed. No <svg>
   may be emitted at all when the configuration cannot be trusted. ---- */
[
  ['invalid stored config', cfg(3), status({ zones_config_valid: false })],
  ['a config load fault', cfg(3), status({ zones_config_load_fault: true })],
  ['a missing thermo_count', { zones: [] }, status()],
  ['a missing zones array', { thermo_count: 3 }, status()],
  ['no response at all', null, status()],
  ['a thermo_count above the 3-channel ceiling', cfg(3, { thermo_count: 4 }), status()],
  ['no zones configured yet', cfg(3, { thermo_count: 0 }), status()]
].forEach(function (row) {
  var out = renderKilnGraphicHtml(row[1], row[2]);
  must(out.indexOf('<svg') < 0, 'with ' + row[0] + ' the graphic must draw NO svg at all');
  must(out.indexOf('kg-failclosed') >= 0, 'with ' + row[0] + ' the fail-closed panel must name the reason');
});

/* ---- 5. Extra relays are derived, and their device type is honestly unknown
   until the concurrent relay device-type storage change lands. ---- */
(function () {
  var c = cfg(3, { relay_count: 4 });   /* zones claim R0-R2, so R3 is extra */
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('R3 Relay3') >= 0, 'an unclaimed relay must be drawn as an extra relay under its stored name');
  must(out.indexOf('Relay R3 device type not known') >= 0,
    'an extra relay of unknown device type must carry the unknown badge, not a guessed default');
  must(out.indexOf('#kgIconUnknown') >= 0, 'the unknown device type must use the unknown glyph');
})();
(function () {
  /* With no stored name either, the placeholder must still not read as a type. */
  var c = cfg(3, { relay_count: 4 });
  delete c.relay_names;
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('R3 type ?') >= 0, 'an unnamed extra relay of unknown type must say so rather than guess');
})();
(function () {
  /* Every relay claimed: "no extra relays" is a real answer, distinct from unknown. */
  var out = renderKilnGraphicHtml(cfg(3, { relay_count: 3 }), status());
  must(out.indexOf('no extra relays') >= 0, 'a fully claimed relay set must say so explicitly');
})();
(function () {
  /* Forward compatibility with the concurrent change: once relay_types
     arrives, the same artwork renders the real type with no other edit. */
  var c = cfg(3, { relay_count: 4, relay_types: [0, 0, 0, 4] });  /* 4 = fan */
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('#kgIcon_fan') >= 0, 'a reported relay device type must select its own glyph');
  must(out.indexOf('Relay R3 device type not known') < 0,
    'a reported relay device type must clear the unknown badge');
})();
(function () {
  /* Ownership underivable -> unknown, never "none". */
  var c = cfg(3, { relay_count: 4 });
  c.zones.forEach(function (z) { delete z.relay_mask; });
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('extra relays not reported') >= 0,
    'with no relay_mask anywhere, extra relays must be unknown rather than an empty list');
})();

/* ---- 6. CT is one bracket spanning the rings that share a channel ---- */
(function () {
  var c = cfg(3);
  c.zones[0].ct_mask = 1; c.zones[1].ct_mask = 1; c.zones[2].ct_mask = 2;
  var out = renderKilnGraphicHtml(c, status());
  must(countOf(out, /class="kg-bracket"/g) === 2,
    'two CT channels in use must draw two brackets, got ' + countOf(out, /class="kg-bracket"/g));
  must(out.indexOf('>z0,z1<') >= 0, 'a shared CT channel must name every ring it spans');
})();
(function () {
  var out = renderKilnGraphicHtml(cfg(3), status());   /* every ct_mask 0 */
  must(out.indexOf('>no CT<') >= 0, 'zones with an empty ct_mask must be shown as having no CT');
  must(countOf(out, /class="kg-bracket"/g) === 0, 'no CT channel in use must draw no bracket');
})();
(function () {
  var c = cfg(3);
  c.zones.forEach(function (z) { delete z.ct_mask; });
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('>CT not<') >= 0, 'a missing ct_mask must read as not reported, not as "no CT"');
})();

/* ---- 7. A trip is ONE global banner, and only when the trip is LIVE ---- */
(function () {
  /* The bench board routinely reports a tens-of-minutes-old trip event with a
     healthy live diag_state (a dual reflash trips S6a by design). Keying the
     banner off the event would paint a permanent false trip. */
  var st = status({ trip_event_ever_received: true, trip_reason: 6,
                    trip_reason_cause: 'main controller fault', diag_state: 2 });
  var out = renderKilnGraphicHtml(cfg(3), st);
  must(out.indexOf('SAFETY PROCESSOR TRIPPED') < 0,
    'a stale trip EVENT with a healthy live diag_state must not draw a trip banner');
})();
(function () {
  var st = status({ diag_state: 4, diag_age_ms: 200, diag_trip_reason: 6,
                    diag_trip_reason_words: 'S6a main controller fault' });
  var out = renderKilnGraphicHtml(cfg(3), st);
  must(countOf(out, /kg-trip/g) === 1, 'a live trip must draw exactly ONE banner for the whole kiln');
  must(out.indexOf('S6a main controller fault') >= 0, 'the trip banner must name the reason');
})();
(function () {
  /* diag_state TRIPPED read off a frame older than the staleness window is the
     "link went silent" case, not a live trip. */
  var st = status({ diag_state: 4, diag_age_ms: 9000, diag_trip_reason: 6 });
  var out = renderKilnGraphicHtml(cfg(3), st);
  must(out.indexOf('SAFETY PROCESSOR TRIPPED') < 0, 'a stale diag frame must not assert a live trip');
})();

/* ---- 8. Absent live data is unknown, never healthy ---- */
(function () {
  var c = cfg(3);
  c.zones.forEach(function (z, i) { z.thermo_mask = (1 << i); });
  var out = renderKilnGraphicHtml(c, null);
  must(out.indexOf('state unknown') >= 0,
    'with no /api/status yet, thermocouple health must render as unknown, not as healthy');
})();
(function () {
  var st = status();
  st.channels[1].fault_status = 0x20;
  var out = renderKilnGraphicHtml(cfg(3), st);
  must(out.indexOf('Thermocouple channel 1 fault') >= 0, 'a MAX31856 fault must raise an error badge on its channel');
  must(out.indexOf('#kgIconError') >= 0, 'a fault must use the error glyph, not colour alone');
})();
(function () {
  var out = renderKilnGraphicHtml(cfg(3), status({ }));
  must(out.indexOf('Heater load check is DORMANT') >= 0,
    'an unmeasured normal current must render the heater-load check as DORMANT, never as healthy');
})();
(function () {
  var c = cfg(3, { ct_warn_mask: 2 });
  c.zones[1].normal_current_measured = true;
  c.zones[1].normal_current_a = 3.2;
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('Heater load disagrees') >= 0,
    'a measured zone flagged in ct_warn_mask must raise the heater-load warning');
})();
(function () {
  var c = cfg(3);
  var out = renderKilnGraphicHtml(c, status());
  must(out.indexOf('safety_wiring') < 0, 'internal field names must not leak into the artwork');
  var out2 = renderKilnGraphicHtml(cfg(3, { safety_wiring: { link_up: false } }), status());
  must(out2.indexOf('Safety link DOWN') >= 0, 'a down safety link must be reported globally');
})();

/* ---- 9. Every badge is clickable and carries its own explanation ---- */
(function () {
  var out = renderKilnGraphicHtml(cfg(3), status());
  var badges = countOf(out, /class="kg-badge /g);
  must(badges > 0, 'the bench-shaped config must raise at least one badge');
  must(countOf(out, /data-kgbody="/g) === badges, 'every badge must carry popup text');
  must(countOf(out, /role="button"/g) === badges, 'every badge must be reachable as a button');
  must(countOf(out, /tabindex="0"/g) === badges, 'every badge must be keyboard focusable');
})();

/* ---- 10. No branding of any kind ---- */
(function () {
  var out = renderKilnGraphicHtml(cfg(3), status());
  ['L&L', 'L&amp;L', 'e23', 'Skutt', 'Paragon', 'logo', 'brand'].forEach(function (w) {
    must(out.toLowerCase().indexOf(w.toLowerCase()) < 0, 'the graphic must carry no branding (found "' + w + '")');
  });
})();

/* ---- 11. Spare relays (hardware relays at/after relay_count) show a device-type
   selector; zone-owned ones do not (ZONE_GRAPHIC_PLAN M17, bench finding e35f2108) ---- */
(function () {
  var esc = function (x) { return String(x); };
  var L = ['Relay0', 'Relay1', 'Relay2', 'Relay3'];
  var out = relayRowsHtml(L, 3, 7, ['', '', '', 'Vent'], [0, 0, 0, 3], esc);
  must(countOf(out, /class="relayname[ "]/g) === 4, 'one row per hardware relay (4), got ' + countOf(out, /class="relayname[ "]/g));
  must(countOf(out, /<select class="relaytypeinput"/g) === 1, 'only the spare relay gets a type select');
  must(out.indexOf('Relay3 device type') >= 0, 'relay 4 (spare) must offer a device type control');
  must(/data-relay="3"[\s\S]*<option value="3" selected>valve/.test(out), 'relay 4 must preselect its stored type (valve)');
  must(countOf(out, /type="hidden" class="relaytypeinput"/g) === 3, 'zone-owned relays echo type hidden');
  var out2 = relayRowsHtml(L, 4, 0, [], [], esc);
  must(countOf(out2, /<select class="relaytypeinput"/g) === 4 && out2.indexOf('relayspare') < 0,
    'with all 4 relays in use and unowned, 4 selects and no spare rows');
  var out3 = relayRowsHtml(L, 0, 0, [], [], esc);
  must(countOf(out3, /<select class="relaytypeinput"/g) === 4, 'relay_count 0: all four are spare with selects');
})();

/* ---- 12. Aux-output rows cover every relay the firmware accepts (1..4), including
   spares at/after relay_count; the helper takes no relay_count at all ---- */
(function () {
  var rs = auxRowRelays(4);
  must(JSON.stringify(rs) === '[1,2,3,4]', 'aux rows must cover relays 1-4 (spare relay 4 included), got ' + JSON.stringify(rs));
  must(auxRowRelays.length === 1, 'auxRowRelays must not depend on relay_count');
})();

if (process.exitCode) {
  console.log('kiln graphic render check FAILED');
} else {
  console.log('PASS: kiln graphic renders from configuration, fails closed, and never defaults an unknown field.');
}
'@

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kg_render_check_" + [System.Guid]::NewGuid().ToString('N') + ".js")
try {
  # No BOM: node treats a UTF-8 BOM in a .js file as leading garbage on some versions.
  [System.IO.File]::WriteAllText($tmp, ($block + "`n" + $tests), (New-Object System.Text.UTF8Encoding($false)))
  $out = & $node.Source $tmp
  $rc = $LASTEXITCODE
  $out | ForEach-Object { Write-Output $_ }
  if ($rc -ne 0) { exit 1 }
} finally {
  if (Test-Path $tmp) { Remove-Item $tmp -Force }
}
exit 0
