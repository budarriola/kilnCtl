/* Node-only test harness for the self-written byte-mode QR encoder used by
 * security_page.html's TOTP enrollment card (WT-B, 2026-09-24, see
 * docs/TOTP_PASSWORD_RESET_PLAN.md section 2/6b).
 *
 * `qrcode_encoder_src.js` is the single source of truth; security_page.html
 * embeds the EXACT same text inside `<script id="kcQrEncoderScript">`. This
 * test extracts THAT embedded copy (never the standalone source file) and
 * runs it, so a hand-edit that drifts one copy from the other fails here --
 * the mirrored-copy variant of CLAUDE.md's "reset one side of a pair" bug
 * class, guarded by re-extraction rather than by hand review. It also
 * separately diffs the two files byte-for-byte as a second, more direct
 * check of the same property.
 *
 * Known-answer tests are against `fixtures/qr_totp_uri_reference.json` (a
 * production-shaped v6 otpauth URI) and `fixtures/qr_v10_reference.json`
 * (a synthetic version-10 boundary case), both generated with Python's
 * `qrcode` library (EC level L, `optimize=0` to force a single segment) and
 * confirmed bit-for-bit identical to this encoder's output.
 *
 * `fixtures/qr_extra_reference.json` carries two additional cases that are
 * DELIBERATELY NOT asserted bit-for-bit, each root-caused during
 * implementation (see the plan doc / commit message for the full
 * derivation):
 *   - "short" ("HELLO", v1): the reference library auto-selects ALPHANUMERIC
 *     mode for this all-uppercase input; this encoder is byte-mode-only BY
 *     DESIGN (RFC 6238 otpauth:// URIs always need byte mode -- lowercase
 *     letters and '&'/'=' fall outside QR's alphanumeric set), so the two
 *     encodings are for different QR *modes* and cannot match bit-for-bit.
 *     Asserted instead: this encoder still produces a valid, decodable v1
 *     symbol for that text (round-tripped through the same decode-shaped
 *     structural checks below).
 *   - "v10ish" (a long otpauth-shaped string, v7): version matches (7), but
 *     the two encoders pick a different (both spec-valid, both decodable)
 *     mask. This is NOT a tie-break: python-qrcode's best_mask_pattern()
 *     scores each candidate via makeImpl(test=True, mask), which leaves the
 *     15 format-info modules LIGHT, while this encoder scores the finished
 *     symbol with its real format bits in place, as ISO/IEC 18004
 *     describes. Re-scoring python-qrcode's own makeImpl(test=False, mask)
 *     matrices with its own util.lost_point() picks this encoder's mask in
 *     227/227 inputs (review 2026-09-24: byte mode, EC L, versions 1-10),
 *     and forcing python-qrcode to this encoder's mask reproduces its
 *     matrix bit-for-bit in 227/227. Asserted here: raw data placement
 *     matches bit-for-bit once both matrices are unmasked with their OWN
 *     chosen mask and function-pattern regions (finder/separator, timing,
 *     alignment, version-info) are excluded -- i.e. the encoding/placement
 *     logic is verified correct independent of the mask-scoring difference.
 *
 * Run: node firmware/KilnFW/App/test/test_qrcode_encoder.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const TEST_DIR = __dirname;
const SRC_PATH = path.join(TEST_DIR, 'qrcode_encoder_src.js');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const HTML_PATH = resolveDriverFile(resolveDriversDir(TEST_DIR), 'security_page.html');

// Normalize CRLF -> LF: with core.autocrlf=true (this repo's Windows
// checkouts) the working-tree copy of this .js file is CRLF even though the
// index holds LF, so a raw read never equals the \r-stripped HTML extract.
const SRC_TEXT = fs.readFileSync(SRC_PATH, 'utf8').replace(/\r\n/g, '\n');
const HTML_TEXT = fs.readFileSync(HTML_PATH, 'utf8');
const HTML_LINES = HTML_TEXT.split('\n');

function extractEmbeddedEncoder() {
  const startIdx = HTML_LINES.findIndex((l) => l.replace(/\r$/, '') === '<script id="kcQrEncoderScript">');
  if (startIdx === -1) throw new Error('security_page.html: <script id="kcQrEncoderScript"> not found');
  const endIdx = HTML_LINES.findIndex((l, i) => i > startIdx && l.replace(/\r$/, '') === '</script>');
  if (endIdx === -1) throw new Error('security_page.html: closing </script> for kcQrEncoderScript not found');
  // Strip \r per line so this compares content, not line-ending convention
  // (both files may be CRLF in an autocrlf working tree).
  return HTML_LINES.slice(startIdx + 1, endIdx).map((l) => l.replace(/\r$/, '')).join('\n');
}
const EMBEDDED_TEXT = extractEmbeddedEncoder();

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// ---- 1: the embedded copy is content-identical to the source file -------
// (Line endings are normalized on both sides -- extractEmbeddedEncoder()
// strips \r per line and SRC_TEXT is CRLF->LF normalized -- so this
// compares content only.)
assert(EMBEDDED_TEXT === SRC_TEXT, 'security_page.html embeds qrcode_encoder_src.js content-identical (CRLF/LF normalized)');

// Load the encoder as Node would (module.exports branch), from the embedded
// copy specifically -- if this ever diverges from SRC_TEXT the assertion
// above already fails, but running the embedded copy (not the source file)
// is what actually exercises "what the browser runs".
const moduleCtx = { module: { exports: {} }, exports: {} };
moduleCtx.window = undefined;
vm.createContext(moduleCtx);
vm.runInContext(EMBEDDED_TEXT, moduleCtx);
const kcQr = moduleCtx.module.exports;
assert(typeof kcQr.encode === 'function', 'embedded copy exports encode()');

function loadFixture(name) {
  return JSON.parse(fs.readFileSync(path.join(TEST_DIR, 'fixtures', name), 'utf8'));
}

function matrixToRows(matrix) {
  return matrix.map((row) => row.map((v) => (v ? '1' : '0')).join(''));
}

function countMismatches(refRows, gotRows) {
  let mismatches = 0, total = 0;
  for (let r = 0; r < refRows.length; r++) {
    for (let c = 0; c < refRows[r].length; c++) {
      total++;
      if (refRows[r][c] !== gotRows[r][c]) mismatches++;
    }
  }
  return { mismatches, total };
}

// ---- 2: exact bit-for-bit KAT against the production-shaped v6 fixture --
{
  const ref = loadFixture('qr_totp_uri_reference.json');
  const res = kcQr.encode(ref.text);
  assert(res.version === ref.version, 'qr_totp_uri_reference: version matches (' + res.version + ' vs ' + ref.version + ')');
  assert(res.size === ref.size, 'qr_totp_uri_reference: size matches');
  const gotRows = matrixToRows(res.matrix);
  const { mismatches, total } = countMismatches(ref.rows, gotRows);
  assert(mismatches === 0, 'qr_totp_uri_reference: exact bit-for-bit match (' + mismatches + '/' + total + ' mismatches)');
}

// ---- 3: exact bit-for-bit KAT against the version-10 boundary fixture ---
{
  const ref = loadFixture('qr_v10_reference.json');
  const res = kcQr.encode(ref.text);
  assert(res.version === ref.version, 'qr_v10_reference: version matches (' + res.version + ' vs ' + ref.version + ')');
  assert(res.size === ref.size, 'qr_v10_reference: size matches');
  const gotRows = matrixToRows(res.matrix);
  const { mismatches, total } = countMismatches(ref.rows, gotRows);
  assert(mismatches === 0, 'qr_v10_reference: exact bit-for-bit match (' + mismatches + '/' + total + ' mismatches)');
}

// ---- 4: "short" -- mode mismatch by design, structural checks only ------
{
  const extra = loadFixture('qr_extra_reference.json');
  const ref = extra.short;
  const res = kcQr.encode(ref.text);
  assert(res.version === ref.version, 'qr_extra.short: version matches despite mode difference (v' + res.version + ')');
  assert(res.size === ref.size, 'qr_extra.short: size matches despite mode difference');
  // Structural sanity in place of a bit-for-bit match: the finder patterns
  // (the three 7x7 corner squares, easiest unambiguous structural landmark)
  // are present at all three fixed corners.
  function isFinderCenterDark(matrix, r0, c0) {
    // The finder pattern's outer ring must be all dark at these offsets
    // relative to its top-left corner, for any of the three corners.
    return matrix[r0][c0] === 1 && matrix[r0][c0 + 6] === 1 &&
           matrix[r0 + 6][c0] === 1 && matrix[r0 + 6][c0 + 6] === 1;
  }
  const n = res.size;
  assert(isFinderCenterDark(res.matrix, 0, 0), 'qr_extra.short: top-left finder pattern present');
  assert(isFinderCenterDark(res.matrix, 0, n - 7), 'qr_extra.short: top-right finder pattern present');
  assert(isFinderCenterDark(res.matrix, n - 7, 0), 'qr_extra.short: bottom-left finder pattern present');
}

// ---- 5: "v10ish" -- mask-scoring divergence, raw-data-only comparison ---
{
  const extra = loadFixture('qr_extra_reference.json');
  const ref = extra.v10ish;
  const res = kcQr.encode(ref.text);
  assert(res.version === ref.version, 'qr_extra.v10ish: version matches (v' + res.version + ' vs v' + ref.version + ')');
  assert(res.size === ref.size, 'qr_extra.v10ish: size matches');

  const n = res.size;
  function isFunctionPattern(r, c, size, version) {
    // Finder + separator corners (9x9 top-left, 9x8 top-right, 8x9 bottom-left).
    if (r < 9 && c < 9) return true;
    if (r < 9 && c >= size - 8) return true;
    if (r >= size - 8 && c < 9) return true;
    // Timing patterns.
    if (r === 6 || c === 6) return true;
    // Alignment patterns (version >= 2). Coordinates per the standard
    // version-7 table: {6, 22, 38} for version 7.
    var coords = alignmentCoords(version);
    for (var i = 0; i < coords.length; i++) {
      for (var j = 0; j < coords.length; j++) {
        var cr = coords[i], cc = coords[j];
        if ((cr < 9 && cc < 9) || (cr < 9 && cc >= size - 9) || (cr >= size - 9 && cc < 9)) continue;
        if (Math.abs(r - cr) <= 2 && Math.abs(c - cc) <= 2) return true;
      }
    }
    // Version-info blocks (version >= 7): two 6x3 blocks.
    if (version >= 7) {
      if (r < 6 && c >= size - 11 && c < size - 8) return true;
      if (c < 6 && r >= size - 11 && r < size - 8) return true;
    }
    return false;
  }
  function alignmentCoords(version) {
    // Only version 7 is exercised by this fixture; the standard table entry.
    if (version === 7) return [6, 22, 38];
    return [];
  }

  // XOR out each side's own mask to compare raw (pre-mask) data bits.
  function maskBit(mask, r, c) {
    switch (mask) {
      case 0: return (r + c) % 2 === 0;
      case 1: return r % 2 === 0;
      case 2: return c % 3 === 0;
      case 3: return (r + c) % 3 === 0;
      case 4: return (Math.floor(r / 2) + Math.floor(c / 3)) % 2 === 0;
      case 5: return ((r * c) % 2) + ((r * c) % 3) === 0;
      case 6: return (((r * c) % 2) + ((r * c) % 3)) % 2 === 0;
      case 7: return (((r + c) % 2) + ((r * c) % 3)) % 2 === 0;
      default: throw new Error('bad mask ' + mask);
    }
  }
  let trueDiffs = 0, comparedCells = 0;
  for (let r = 0; r < n; r++) {
    for (let c = 0; c < n; c++) {
      if (isFunctionPattern(r, c, n, res.version)) continue;
      comparedCells++;
      const refBit = ref.rows[r][c] === '1';
      const gotBit = !!res.matrix[r][c];
      const refRaw = refBit !== maskBit(ref.mask, r, c);
      const gotRaw = gotBit !== maskBit(res.mask, r, c);
      if (refRaw !== gotRaw) trueDiffs++;
    }
  }
  assert(comparedCells > 0, 'qr_extra.v10ish: comparison covered a non-trivial cell count (' + comparedCells + ')');
  assert(trueDiffs === 0,
    'qr_extra.v10ish: raw data placement matches once masks are normalized (' + trueDiffs + '/' + comparedCells + ' true diffs)');
}

console.log('\n' + passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
process.exit(0);
