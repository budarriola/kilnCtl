// qrcode_encoder_src.js -- canonical source for the small, self-written QR
// encoder used by security_page.html's TOTP enrollment card
// (docs/TOTP_PASSWORD_RESET_PLAN.md section 2/WT-B). No CDN, no external
// library: byte-mode-only QR encoding (RFC 6238 otpauth:// URIs always need
// byte mode -- they contain lowercase letters and '&'/'=', which are outside
// QR's alphanumeric character set), error-correction level L, versions 1-10
// (comfortably covers a ~100-140 char otpauth URI per the plan's own
// capacity note), full 8-pattern mask penalty scoring (matches what a real
// QR library picks, for maximum scanner compatibility) rather than a fixed
// mask.
//
// THIS FILE IS THE SINGLE SOURCE OF TRUTH. security_page.html embeds this
// exact text inside a `<script id="kcQrEncoderScript">` block (copy kept in
// sync deliberately -- see that file's comment at the embed site).
// test_qrcode_encoder.js extracts that same block from the HTML and runs it
// through Node's `vm` module, so a change to one without the other is
// caught by the test rather than silently drifting (CLAUDE.md's "reset one
// side of a pair" bug class -- this is the mirrored-copy variant of it,
// guarded by re-extraction instead of by hand-review).
//
// UMD-ish export: usable as an inline browser <script> (attaches
// `window.kcQrEncode`) or via Node `require()` (module.exports) for the
// test.
(function (root) {
  'use strict';

  // ---- Fixed capacity table, byte mode, EC level L, versions 1-10 -------
  // (max byte count per version; well-established public QR capacity
  // numbers, cross-checked here against the codeword table below: for each
  // version, (dataCodewords*8 - 4 - countBits) / 8, floored, reproduces
  // this exact table.)
  var BYTE_CAPACITY_L = [17, 32, 53, 78, 106, 134, 154, 192, 230, 271];

  // Total data codewords in the symbol (all blocks combined), EC level L.
  var DATA_CODEWORDS_L = [19, 34, 55, 80, 108, 136, 156, 194, 232, 274];

  // EC codewords per block, EC level L.
  var EC_PER_BLOCK_L = [7, 10, 15, 20, 26, 18, 20, 24, 30, 18];

  // Block structure, EC level L: [[count, dataCodewordsPerBlock], ...].
  // Only version 10 splits into two differently-sized groups in this range.
  var BLOCKS_L = [
    [[1, 19]],
    [[1, 34]],
    [[1, 55]],
    [[1, 80]],
    [[1, 108]],
    [[2, 68]],
    [[2, 78]],
    [[2, 97]],
    [[2, 116]],
    [[2, 68], [2, 69]],
  ];

  // Remainder bits appended after all codeword bits, per version.
  var REMAINDER_BITS = [0, 7, 7, 7, 7, 7, 0, 0, 0, 0];

  // Alignment pattern center coordinates per version (version 1 has none).
  var ALIGNMENT_COORDS = [
    [],
    [6, 18],
    [6, 22],
    [6, 26],
    [6, 30],
    [6, 34],
    [6, 22, 38],
    [6, 24, 42],
    [6, 26, 46],
    [6, 28, 50],
  ];

  // ---- GF(256) arithmetic, primitive polynomial 0x11D -------------------
  var GF_EXP = new Array(512);
  var GF_LOG = new Array(256);
  (function buildGfTables() {
    var x = 1;
    for (var i = 0; i < 255; i++) {
      GF_EXP[i] = x;
      GF_LOG[x] = i;
      x <<= 1;
      if (x & 0x100) x ^= 0x11d;
    }
    for (i = 255; i < 512; i++) GF_EXP[i] = GF_EXP[i - 255];
  })();
  function gfMul(a, b) {
    if (a === 0 || b === 0) return 0;
    return GF_EXP[GF_LOG[a] + GF_LOG[b]];
  }

  // Generator polynomial for `degree` EC codewords: product of (x - a^i)
  // for i in [0, degree), coefficients highest-degree-first.
  function rsGeneratorPoly(degree) {
    var poly = [1];
    for (var i = 0; i < degree; i++) {
      var next = new Array(poly.length + 1).fill(0);
      for (var j = 0; j < poly.length; j++) {
        next[j] ^= poly[j];
        next[j + 1] ^= gfMul(poly[j], GF_EXP[i]);
      }
      poly = next;
    }
    return poly;
  }

  // Reed-Solomon remainder (the EC codewords) for one block's data bytes.
  function rsEncodeBlock(dataBytes, ecLen) {
    var generator = rsGeneratorPoly(ecLen);
    var buf = dataBytes.concat(new Array(ecLen).fill(0));
    for (var i = 0; i < dataBytes.length; i++) {
      var coef = buf[i];
      if (coef === 0) continue;
      for (var j = 0; j < generator.length; j++) {
        buf[i + j] ^= gfMul(generator[j], coef);
      }
    }
    return buf.slice(dataBytes.length);
  }

  // ---- Bit buffer ---------------------------------------------------------
  function BitBuffer() {
    this.bits = [];
  }
  BitBuffer.prototype.put = function (value, length) {
    for (var i = length - 1; i >= 0; i--) {
      this.bits.push((value >>> i) & 1);
    }
  };
  BitBuffer.prototype.length = function () { return this.bits.length; };

  function utf8Bytes(str) {
    var bytes = [];
    for (var i = 0; i < str.length; i++) {
      var code = str.charCodeAt(i);
      if (code < 0x80) {
        bytes.push(code);
      } else if (code < 0x800) {
        bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
      } else {
        bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
      }
    }
    return bytes;
  }

  function chooseVersion(byteLen) {
    for (var v = 1; v <= 10; v++) {
      if (byteLen <= BYTE_CAPACITY_L[v - 1]) return v;
    }
    return -1; // caller refuses: too long for this encoder's supported range
  }

  function buildDataCodewords(bytes, version) {
    var buf = new BitBuffer();
    buf.put(0x4, 4); // byte-mode indicator
    var countBits = version < 10 ? 8 : 16;
    buf.put(bytes.length, countBits);
    for (var i = 0; i < bytes.length; i++) buf.put(bytes[i], 8);

    var totalDataBits = DATA_CODEWORDS_L[version - 1] * 8;
    // Terminator: up to 4 zero bits.
    for (var t = 0; t < 4 && buf.length() < totalDataBits; t++) buf.put(0, 1);
    // Pad to a byte boundary.
    while (buf.length() % 8 !== 0) buf.put(0, 1);
    // Pad bytes, alternating 0xEC/0x11, until the symbol's data capacity is full.
    var padToggle = true;
    while (buf.length() < totalDataBits) {
      buf.put(padToggle ? 0xec : 0x11, 8);
      padToggle = !padToggle;
    }
    var codewords = [];
    for (var b = 0; b < buf.bits.length; b += 8) {
      var byteVal = 0;
      for (var k = 0; k < 8; k++) byteVal = (byteVal << 1) | buf.bits[b + k];
      codewords.push(byteVal);
    }
    return codewords;
  }

  function interleave(version, dataCodewords) {
    var blocks = BLOCKS_L[version - 1];
    var ecLen = EC_PER_BLOCK_L[version - 1];
    var dataBlocks = [];
    var ecBlocks = [];
    var offset = 0;
    var maxDataLen = 0;
    blocks.forEach(function (group) {
      var count = group[0], len = group[1];
      maxDataLen = Math.max(maxDataLen, len);
      for (var i = 0; i < count; i++) {
        var block = dataCodewords.slice(offset, offset + len);
        offset += len;
        dataBlocks.push(block);
        ecBlocks.push(rsEncodeBlock(block, ecLen));
      }
    });
    var out = [];
    for (var col = 0; col < maxDataLen; col++) {
      for (var bi = 0; bi < dataBlocks.length; bi++) {
        if (col < dataBlocks[bi].length) out.push(dataBlocks[bi][col]);
      }
    }
    for (col = 0; col < ecLen; col++) {
      for (bi = 0; bi < ecBlocks.length; bi++) out.push(ecBlocks[bi][col]);
    }
    return out;
  }

  // ---- Module matrix construction ---------------------------------------
  function makeMatrix(version) {
    var size = version * 4 + 17;
    var m = [];
    for (var r = 0; r < size; r++) m.push(new Array(size).fill(null));
    return m;
  }

  function placeFinder(m, row, col) {
    for (var r = -1; r <= 7; r++) {
      for (var c = -1; c <= 7; c++) {
        var rr = row + r, cc = col + c;
        if (rr < 0 || rr >= m.length || cc < 0 || cc >= m.length) continue;
        var dark = (r >= 0 && r <= 6 && (c === 0 || c === 6)) ||
                   (c >= 0 && c <= 6 && (r === 0 || r === 6)) ||
                   (r >= 2 && r <= 4 && c >= 2 && c <= 4);
        m[rr][cc] = dark ? 1 : 0;
      }
    }
  }

  function placeAlignment(m, row, col) {
    for (var r = -2; r <= 2; r++) {
      for (var c = -2; c <= 2; c++) {
        var dark = Math.max(Math.abs(r), Math.abs(c)) !== 1;
        m[row + r][col + c] = dark ? 1 : 0;
      }
    }
  }

  function reservedForFormatOrVersion(size, version, row, col) {
    // Finder + separators (top-left 9x9, top-right 8x9, bottom-left 9x8).
    if (row < 9 && col < 9) return true;
    if (row < 9 && col >= size - 8) return true;
    if (row >= size - 8 && col < 9) return true;
    // Timing patterns.
    if (row === 6 || col === 6) return true;
    // Version info blocks (v>=7): 6x3 near top-right and bottom-left.
    if (version >= 7) {
      if (row < 6 && col >= size - 11 && col <= size - 9) return true;
      if (col < 6 && row >= size - 11 && row <= size - 9) return true;
    }
    return false;
  }

  function placeStaticPatterns(m, version) {
    var size = m.length;
    placeFinder(m, 0, 0);
    placeFinder(m, 0, size - 7);
    placeFinder(m, size - 7, 0);
    // Separators (already 0 by the placeFinder border loop above where in range).
    // Timing patterns.
    for (var i = 8; i < size - 8; i++) {
      var val = i % 2 === 0 ? 1 : 0;
      if (m[6][i] === null) m[6][i] = val;
      if (m[i][6] === null) m[i][6] = val;
    }
    // Alignment patterns: all combinations of the coordinate list, skipping
    // only centers that overlap one of the three finder-pattern zones
    // (NOT a generic "already non-null" check -- a center legitimately
    // coincides with the timing-pattern line for some versions, e.g.
    // version 10's (6, 28), and must still be placed there, overriding the
    // timing pixel; only the finder corners are truly forbidden).
    function overlapsFinderZone(r, c) {
      return (r < 9 && c < 9) || (r < 9 && c >= size - 9) || (r >= size - 9 && c < 9);
    }
    var coords = ALIGNMENT_COORDS[version - 1];
    for (var a = 0; a < coords.length; a++) {
      for (var b = 0; b < coords.length; b++) {
        var r = coords[a], c = coords[b];
        if (overlapsFinderZone(r, c)) continue;
        placeAlignment(m, r, c);
      }
    }
    // Dark module (always present, position depends on version).
    m[4 * version + 9][8] = 1;
    // Reserve (but do not fill) format-info cells and version-info cells --
    // filled later by fillFormatInfo/fillVersionInfo once the mask is chosen.
    for (var rr = 0; rr < size; rr++) {
      for (var cc = 0; cc < size; cc++) {
        if (m[rr][cc] === null && reservedForFormatOrVersion(size, version, rr, cc)) {
          m[rr][cc] = 'reserved';
        }
      }
    }
  }

  function dataPlacementOrder(size) {
    // Standard zigzag: two-columns-wide, moving bottom-to-top then
    // top-to-bottom, right to left, skipping the vertical timing column.
    var order = [];
    var col = size - 1;
    var upward = true;
    while (col > 0) {
      if (col === 6) col--; // skip the timing column
      for (var i = 0; i < size; i++) {
        var row = upward ? size - 1 - i : i;
        for (var dc = 0; dc < 2; dc++) {
          order.push([row, col - dc]);
        }
      }
      col -= 2;
      upward = !upward;
    }
    return order;
  }

  function maskFn(pattern, row, col) {
    switch (pattern) {
      case 0: return (row + col) % 2 === 0;
      case 1: return row % 2 === 0;
      case 2: return col % 3 === 0;
      case 3: return (row + col) % 3 === 0;
      case 4: return (Math.floor(row / 2) + Math.floor(col / 3)) % 2 === 0;
      case 5: return ((row * col) % 2) + ((row * col) % 3) === 0;
      case 6: return (((row * col) % 2) + ((row * col) % 3)) % 2 === 0;
      case 7: return (((row + col) % 2) + ((row * col) % 3)) % 2 === 0;
      default: return false;
    }
  }

  function placeData(matrix, size, codewordBits, maskPattern) {
    var m = matrix.map(function (row) { return row.slice(); });
    var order = dataPlacementOrder(size);
    var bi = 0;
    for (var i = 0; i < order.length; i++) {
      var row = order[i][0], col = order[i][1];
      if (m[row][col] !== null) continue; // static pattern or reserved cell
      var bit = bi < codewordBits.length ? codewordBits[bi] : 0;
      bi++;
      if (maskFn(maskPattern, row, col)) bit ^= 1;
      m[row][col] = bit;
    }
    return m;
  }

  // BCH(15,5) format-info encoding, generator 0x537, XOR mask 0x5412.
  function formatInfoBits(maskPattern) {
    // EC level L = 0b01 in the 2-bit field, per the standard's level->bits
    // mapping (L=01, M=00, Q=11, H=10).
    var data = (0x1 << 3) | maskPattern;
    var g = 0x537;
    var value = data << 10;
    for (var i = 4; i >= 0; i--) {
      if (value & (1 << (i + 10))) value ^= g << i;
    }
    var bits = ((data << 10) | value) ^ 0x5412;
    return bits & 0x7fff;
  }

  function fillFormatInfo(m, maskPattern) {
    var size = m.length;
    var bits = formatInfoBits(maskPattern);
    function bit(i) { return (bits >> i) & 1; }
    // Vertical strip at column 8: bits 0-5 at rows 0-5, bits 6-7 at rows
    // 7-8 (row 6 -- the timing row -- is skipped), bits 8-14 at rows
    // size-7..size-1 (bottom-left copy).
    for (var i = 0; i < 6; i++) m[i][8] = bit(i);
    m[7][8] = bit(6);
    m[8][8] = bit(7);
    for (i = 8; i < 15; i++) m[size - 15 + i][8] = bit(i);
    // Horizontal strip at row 8: bits 0-7 at columns size-1..size-8
    // (top-right copy), bit 8 at column 7, bits 9-14 at columns 5..0 (the
    // timing column, 6, is skipped) -- this is the top-left copy's other
    // half; column 8 at row 8 is already set by the vertical strip above.
    for (i = 0; i < 8; i++) m[8][size - 1 - i] = bit(i);
    m[8][7] = bit(8);
    for (i = 9; i < 15; i++) m[8][15 - i - 1] = bit(i);
  }

  // BCH(18,6) version-info encoding, generator 0x1F25, for versions >= 7.
  function versionInfoBits(version) {
    var value = version << 12;
    var g = 0x1f25;
    for (var i = 5; i >= 0; i--) {
      if (value & (1 << (i + 12))) value ^= g << i;
    }
    return (version << 12) | value;
  }

  function fillVersionInfo(m, version) {
    if (version < 7) return;
    var size = m.length;
    var bits = versionInfoBits(version);
    for (var i = 0; i < 18; i++) {
      var bit = (bits >> i) & 1;
      var row = Math.floor(i / 3);
      var col = i % 3;
      m[row][size - 11 + col] = bit;
      m[size - 11 + col][row] = bit;
    }
  }

  // ---- Mask penalty scoring (ISO/IEC 18004 4 rules) ----------------------
  function penaltyScore(m) {
    var size = m.length;
    var score = 0;
    // Rule 1: runs of 5+ same-color modules in a row/column.
    function runPenalty(getCell) {
      var p = 0;
      for (var i = 0; i < size; i++) {
        var run = 1;
        var prev = getCell(i, 0);
        for (var j = 1; j < size; j++) {
          var v = getCell(i, j);
          if (v === prev) {
            run++;
          } else {
            if (run >= 5) p += 3 + (run - 5);
            run = 1;
            prev = v;
          }
        }
        if (run >= 5) p += 3 + (run - 5);
      }
      return p;
    }
    score += runPenalty(function (i, j) { return m[i][j]; });
    score += runPenalty(function (i, j) { return m[j][i]; });
    // Rule 2: 2x2 blocks of the same color.
    for (var r = 0; r < size - 1; r++) {
      for (var c = 0; c < size - 1; c++) {
        var v = m[r][c];
        if (v === m[r][c + 1] && v === m[r + 1][c] && v === m[r + 1][c + 1]) score += 3;
      }
    }
    // Rule 3: finder-like patterns (1:1:3:1:1 dark-light ratio with 4 light
    // either side) found in a row or column.
    var patternA = [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0];
    var patternB = [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1];
    function matchesAt(getCell, i, start, pattern) {
      for (var k = 0; k < pattern.length; k++) {
        if (getCell(i, start + k) !== pattern[k]) return false;
      }
      return true;
    }
    function findPenalty(getCell) {
      var p = 0;
      for (var i = 0; i < size; i++) {
        for (var j = 0; j <= size - 11; j++) {
          if (matchesAt(getCell, i, j, patternA) || matchesAt(getCell, i, j, patternB)) p += 40;
        }
      }
      return p;
    }
    score += findPenalty(function (i, j) { return m[i][j]; });
    score += findPenalty(function (i, j) { return m[j][i]; });
    // Rule 4: overall dark-module proportion vs 50%. Spec method: take the
    // multiples of five immediately below and above the actual percentage,
    // score each candidate's deviation from 50, and use the smaller one
    // (NOT a single floor(...) of the raw deviation -- that under-counts
    // some percentages by one step and disagrees with reference encoders
    // on the resulting best-mask tie-break).
    var dark = 0;
    for (r = 0; r < size; r++) for (c = 0; c < size; c++) if (m[r][c]) dark++;
    var percent = (dark * 100) / (size * size);
    var prevMultiple = Math.floor(percent / 5) * 5;
    var nextMultiple = prevMultiple + 5;
    var prevDiff = Math.abs(prevMultiple - 50) / 5;
    var nextDiff = Math.abs(nextMultiple - 50) / 5;
    score += Math.min(prevDiff, nextDiff) * 10;
    return score;
  }

  // ---- Public entry point -------------------------------------------------
  // Returns { size, matrix } where matrix[row][col] is 1 (dark) or 0
  // (light). Throws if `text`'s byte length exceeds this encoder's
  // supported range (version 10 / EC level L / byte mode -- ample headroom
  // for a fixed-parameter otpauth:// URI per the plan's own capacity note).
  function kcQrEncode(text) {
    var bytes = utf8Bytes(String(text));
    var version = chooseVersion(bytes.length);
    if (version === -1) {
      throw new Error('kcQrEncode: input too long for this encoder (max ' +
        BYTE_CAPACITY_L[BYTE_CAPACITY_L.length - 1] + ' bytes, got ' + bytes.length + ')');
    }
    var dataCodewords = buildDataCodewords(bytes, version);
    var allCodewords = interleave(version, dataCodewords);
    var codewordBits = [];
    for (var i = 0; i < allCodewords.length; i++) {
      for (var b = 7; b >= 0; b--) codewordBits.push((allCodewords[i] >> b) & 1);
    }
    for (i = 0; i < REMAINDER_BITS[version - 1]; i++) codewordBits.push(0);

    var base = makeMatrix(version);
    placeStaticPatterns(base, version);

    var best = null, bestScore = Infinity, bestMask = 0;
    for (var mask = 0; mask < 8; mask++) {
      var candidate = placeData(base, base.length, codewordBits, mask);
      fillFormatInfo(candidate, mask);
      fillVersionInfo(candidate, version);
      var s = penaltyScore(candidate);
      if (s < bestScore) {
        bestScore = s;
        best = candidate;
        bestMask = mask;
      }
    }
    // Normalize 'reserved' placeholders (should not remain -- format/version
    // fill above overwrites every reserved cell) to 0 defensively.
    for (var r2 = 0; r2 < best.length; r2++) {
      for (var c2 = 0; c2 < best.length; c2++) {
        if (best[r2][c2] === 'reserved' || best[r2][c2] === null) best[r2][c2] = 0;
      }
    }
    return { size: best.length, version: version, mask: bestMask, matrix: best };
  }

  var api = { encode: kcQrEncode };
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  } else {
    root.kcQr = api;
  }
})(typeof window !== 'undefined' ? window : this);
