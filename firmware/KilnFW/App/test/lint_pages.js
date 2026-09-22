/* Syntax-checks the KilnFW web pages: every inline <script> block, every
 * inline <style> block, and the standalone .js/.css files.
 *
 * Two REAL shipped bugs motivate this, both the same typo:
 *
 *   thermo_faults_page.html wrote "MAX31856_MASK_*" followed by "/" inside a
 *   JS comment. The "*"+"/" closed the comment early, the rest of the line
 *   parsed as code, and the syntax error killed the whole inline script -- so
 *   the page sat on "Loading..." forever while its endpoint answered fine.
 *
 *   main_page.html wrote "--ui-*" followed by "/--fg" inside a CSS comment.
 *   Same early close; the prose after it parsed as CSS; the parser gave up
 *   after 35 of the block's rules and silently discarded every rule below,
 *   including .relay-status and .relay-tag. The visible symptom was a missing
 *   space on the dashboard ("86.6 °FR1: off") -- three levels removed from
 *   the actual cause.
 *
 * Neither was visible to curl, to the host tests, or to a careful read of the
 * source: in both cases the file LOOKS correct, and the served bytes are
 * exactly what was written.
 */
const fs = require('fs'), path = require('path'), vm = require('vm');

const dir = process.argv[2];
let bad = 0, checked = 0;

/* firmware/KilnFW/App/drivers/ is being split into layer subdirectories
 * (drivers/<layer>/<name>): the .html/.js/.css pages this script scans, and
 * the four cross-check files named below (ota_interlock.h, app.js,
 * watchdog_cfg.h, main_page.html), will no longer sit directly in `dir` --
 * they'll be one level deeper, under drivers/http, drivers/net, etc. A
 * plain non-recursive fs.readdirSync(dir) would then silently find nothing
 * and report a vacuous "0 problems" pass instead of scanning any pages at
 * all, and the fs.existsSync(path.join(dir, name)) cross-checks below would
 * silently skip their comparison the same way. walk_files() recurses so
 * both keep finding the real files regardless of which layer they land in;
 * find_file() resolves one specific basename anywhere under `dir` and
 * throws loudly (not a silent skip) if it is ambiguous. */
function walk_files(root) {
  let out = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    if (entry.name === 'node_modules' || entry.name === '.git') continue;
    const full = path.join(root, entry.name);
    if (entry.isDirectory()) out = out.concat(walk_files(full));
    else out.push(full);
  }
  return out;
}

function find_file(root, basename) {
  const matches = walk_files(root).filter(p => path.basename(p) === basename);
  if (matches.length > 1) {
    throw new Error(`lint_pages.js: '${basename}' matched more than one file under ${root}: ` +
                     `${matches.join(', ')} -- cannot tell which one is the real file.`);
  }
  return matches.length === 1 ? matches[0] : null;
}

/* A comment terminator immediately followed by a non-space character.
 *
 * This is the shared signature of both bugs above: a legitimate comment end
 * is followed by a newline or whitespace, whereas an ACCIDENTAL one lands
 * mid-token, as in "MASK_" or "--ui-" followed by a star, a slash, and then
 * more identifier characters with no space. (Writing those two examples out
 * literally here would trip this very check -- and did, twice, while this
 * file was being written: it failed to parse for exactly the reason it
 * documents.) Checking the character after the terminator catches the mistake at the point it is made, instead
 * of leaving it to surface as a missing space three layers downstream.
 *
 * Applied to CSS only. JS is validated by actually parsing it below, which
 * is the stronger check; running this heuristic on JS as well produced only
 * false positives (regex literals end in a star and the closing delimiter).
 * The two bugs are nonetheless the same typo, and a linter that covered only
 * <script> is exactly what let the CSS one reach the owner. */
function glued_comment_ends(code, label, line0) {
  const out = [];
  const re = /\*\/(?=\S)/g;
  let m;
  while ((m = re.exec(code))) {
    const line = line0 + code.slice(0, m.index).split('\n').length - 1;
    const ctx = code.slice(Math.max(0, m.index - 40), m.index + 12).replace(/\n/g, ' ');
    out.push(`${label} (line ${line}): comment ends mid-token -- "...${ctx}". ` +
             `A "*" immediately before "/" closes the comment early; everything after it is parsed as code.`);
  }
  return out;
}

/* Off-theme white/black BACKGROUND literal on a button or card (owner audit,
 * 2026-09-08/09): diagnostics_page.html's danger-zone buttons hardcoded
 * `background: #fff`, which is invisible against the page's own background
 * in light mode and reads as a jarring, unthemed white patch in dark mode --
 * every other themed surface in these pages goes through a var(--bg)/
 * var(--button-bg)/var(--card-bg)/etc token specifically so it flips with
 * the theme instead of staying pinned to one literal colour.
 *
 * Scoped to `background`/`background-color` only, not `color`/`border` --
 * this codebase has a separate, large, and deliberate existing convention of
 * literal `color: #fff`/`border: 1px solid #fff` used as high-contrast text/
 * outline ON TOP of a solid, already-themed fill (var(--warn), var(--ok),
 * var(--ui-accent-5), the .danger-card/.card.ineffective treatment, every
 * .kc-*-btn in theme.css, etc.) -- white text reads fine on both this
 * palette's light-mode and dark-mode saturated accent colours, so that
 * pattern is not the bug class the owner flagged and is intentionally left
 * alone here; a rule broad enough to also catch it would fail on ~30
 * pre-existing, correct declarations across nearly every page in this
 * directory, which is exactly the kind of over-broad check CLAUDE.md warns
 * against (flags the common correct case, not the specific defect).
 *
 * A custom-property TOKEN DEFINITION (e.g. `--bg: #fff;` inside a page's own
 * :root block, or theme.css's --ui-* palette) is not a violation -- it is
 * the one place a literal is supposed to live, and it's automatically
 * excluded here because its property name starts with "--", not
 * "background". Anything actually named `background`/`background-color` on
 * a real rule is a hardcoded USE of a colour, not a definition of one.
 *
 * Two narrow, commented allowances:
 *   - rgba(0,0,0,<alpha>) / rgba(255,255,255,<alpha>) -- a translucent
 *     black/white overlay (modal backdrops: main_page.html's #pidPopupOverlay,
 *     theme.css's .kc-menu-overlay) composites correctly over either theme's
 *     background the same way --ui-shadow-1/2 already do (see theme.css's
 *     own comment on why plain rgba(0,0,0,...) was chosen over
 *     color-mix(var(--ui-bg))) -- it is not "a white/black background",
 *     it's a dimming veil, and it is required to look the same in both
 *     themes rather than switch with one.
 *   - an inline `/* lint-color-ok: <reason> *\/` comment on the SAME line as
 *     the declaration, for the rare case a literal really is required (the
 *     wifi_provision_page.html QR canvas, which must render true white for a
 *     phone camera to scan it regardless of theme, and safety_page.html's
 *     .breaker-banner, deliberately pinned to black so the loudest safety
 *     banner in the app can never be mistaken for an ordinary themed card).
 *     Keep this allowlist minimal -- it is an escape hatch for a handful of
 *     reviewed exceptions, not a way to silence the rule.
 */
function hardcoded_background_literals(code, label, line0) {
  const out = [];
  const lines = code.split('\n');
  const declRe = /(^|[;{}])\s*([a-zA-Z-]+)\s*:\s*([^;{}]+);/g;
  const targetProp = /^background(-color)?$/;
  let m;
  while ((m = declRe.exec(code))) {
    const prop = m[2];
    if (prop.startsWith('--')) continue; // token definition, not a use -- allowed
    if (!targetProp.test(prop)) continue;
    const value = m[3];
    const hasHex = /#[0-9a-fA-F]{3,8}\b/.test(value);
    const hasNamedLiteral = /\b(white|black)\b/i.test(value);
    const rgbMatch = value.match(/\brgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)/);
    let isOverlayRgb = false;
    if (rgbMatch) {
      const [r, g, b] = [rgbMatch[1], rgbMatch[2], rgbMatch[3]].map(Number);
      const isBlack = r === 0 && g === 0 && b === 0;
      const isWhite = r === 255 && g === 255 && b === 255;
      isOverlayRgb = isBlack || isWhite;
    }
    if (!hasHex && !hasNamedLiteral && !(rgbMatch && !isOverlayRgb)) continue;
    const lineIdx = code.slice(0, m.index).split('\n').length - 1;
    const line = line0 + lineIdx;
    const lineText = lines[lineIdx] || '';
    if (/lint-color-ok/.test(lineText)) continue;
    out.push(`${label} (line ${line}): hardcoded colour literal in '${prop}: ${value.trim()};' -- ` +
      `off-theme background (owner audit 2026-09-08/09: diagnostics_page.html's ` +
      `"background: #fff" buttons). Use an existing theme token (var(--bg), ` +
      `var(--button-bg), var(--card-bg), etc.) instead of a literal colour outside ` +
      `a token definition, or add "/* lint-color-ok: <reason> */" on this same line ` +
      `if a literal really is required.`);
  }
  return out;
}

for (const fullPath of walk_files(dir).filter(p => /\.(html|js|css)$/.test(p))) {
  const f = path.relative(dir, fullPath).split(path.sep).join('/');
  let src = fs.readFileSync(fullPath, 'utf8');
  const scripts = [], styles = [];

  if (f.endsWith('.js')) {
    scripts.push({ code: src, line: 1 });
  } else if (f.endsWith('.css')) {
    styles.push({ code: src, line: 1 });
  } else {
    /* Blank out HTML comments first, preserving newlines so line numbers stay
     * right. Several pages mention the literal words "<script>"/"<style>"
     * inside a comment; matching from there captures nonsense and reports a
     * syntax error in a file that is fine. */
    src = src.replace(/<!--[\s\S]*?-->/g, m => m.replace(/[^\n]/g, ' '));

    const sre = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
    let m;
    while ((m = sre.exec(src))) {
      scripts.push({ code: m[1], line: src.slice(0, m.index).split('\n').length });
    }
    const yre = /<style[^>]*>([\s\S]*?)<\/style>/gi;
    while ((m = yre.exec(src))) {
      styles.push({ code: m[1], line: src.slice(0, m.index).split('\n').length });
    }
  }

  for (const b of scripts) {
    checked++;
    try {
      new vm.Script(b.code);
    } catch (e) {
      bad++;
      console.log(`${f} (inline script at line ${b.line}): ${e.message}`);
    }
    /* No glued-comment-end heuristic for JS: new vm.Script() above already
     * PARSES the code, which is what catches the real bug (the faults-page
     * one showed up there as "Unexpected identifier 'layout'"). Running the
     * heuristic here as well only produced false positives, because a regex
     * literal legitimately ends in a star followed by the closing slash (a
     * whitespace class with a star quantifier, then the delimiter, then a
     * flag). CSS gets the heuristic because there is no CSS parser available
     * here to do the same job. */
  }

  for (const b of styles) {
    checked++;
    for (const msg of glued_comment_ends(b.code, `${f} style`, b.line)) {
      bad++;
      console.log(msg);
    }
    /* Cheap structural check on top of the comment rule: with comments
     * stripped, braces must balance and must never go negative. */
    const stripped = b.code.replace(/\/\*[\s\S]*?\*\//g, '');
    let depth = 0, neg = false;
    for (const ch of stripped) {
      if (ch === '{') depth++;
      else if (ch === '}') { depth--; if (depth < 0) neg = true; }
    }
    if (neg || depth !== 0) {
      bad++;
      console.log(`${f} (style at line ${b.line}): unbalanced braces (final depth ${depth}${neg ? ', went negative' : ''})`);
    }
    for (const msg of hardcoded_background_literals(b.code, `${f} style`, b.line)) {
      bad++;
      console.log(msg);
    }
  }
}

/* The operator warning shown for a 428 "safety processor not answering"
 * response is duplicated in two languages: the C macro that would back an
 * LCD/serial copy of the same sentence, and the JS copy actually shown by
 * app.js's kcFetchWithSafetyAck. Nothing enforces they match at compile time
 * -- they live in unrelated files, in unrelated languages -- so a wording
 * edit to one without the other ships two different descriptions of the same
 * risk. Extract each by concatenating its adjacent string literals in order
 * and compare.
 */
function extract_c_macro_string(src, macroName) {
  const re = new RegExp(`#define\\s+${macroName}\\b([\\s\\S]*?)(?:\\n(?!\\s*")|$)`);
  const m = re.exec(src);
  if (!m) return null;
  let body = m[1];
  // Strip line-continuation backslashes (and the newline they attach to).
  body = body.replace(/\\\r?\n/g, ' ');
  let out = '';
  const lre = /"((?:[^"\\]|\\.)*)"/g;
  let lm;
  while ((lm = lre.exec(body))) {
    out += lm[1].replace(/\\"/g, '"').replace(/\\n/g, '\n');
  }
  return out;
}

function extract_js_var_string(src, varName) {
  const re = new RegExp(`\\b${varName}\\s*=([\\s\\S]*?);`);
  const m = re.exec(src);
  if (!m) return null;
  let body = m[1];
  let out = '';
  const lre = /'((?:[^'\\]|\\.)*)'|"((?:[^"\\]|\\.)*)"/g;
  let lm;
  while ((lm = lre.exec(body))) {
    const lit = lm[1] !== undefined ? lm[1] : lm[2];
    out += lit.replace(/\\(['"])/g, '$1').replace(/\\n/g, '\n');
  }
  return out;
}

{
  const hPath = find_file(dir, 'ota_interlock.h');
  const jsPath = find_file(dir, 'app.js');
  if (hPath && jsPath) {
    checked++;
    const cText = extract_c_macro_string(fs.readFileSync(hPath, 'utf8'), 'OTA_INTERLOCK_NO_SAFETY_WARNING');
    const jsText = extract_js_var_string(fs.readFileSync(jsPath, 'utf8'), 'NO_SAFETY_WARNING');
    if (cText === null) {
      bad++;
      console.log(`ota_interlock.h: could not find OTA_INTERLOCK_NO_SAFETY_WARNING macro`);
    } else if (jsText === null) {
      bad++;
      console.log(`app.js: could not find NO_SAFETY_WARNING variable`);
    } else if (cText !== jsText) {
      bad++;
      console.log(`OTA_INTERLOCK_NO_SAFETY_WARNING (ota_interlock.h) and NO_SAFETY_WARNING (app.js) disagree:\n` +
                  `  C:  ${JSON.stringify(cText)}\n` +
                  `  JS: ${JSON.stringify(jsText)}`);
    }
  }
}

{
  // watchdog_cfg.h's WATCHDOG_CFG_FIRING_WARNING vs main_page.html's
  // WATCHDOG_CFG_FIRING_WARNING_JS -- same drift-prevention mechanism as the
  // OTA_INTERLOCK_NO_SAFETY_WARNING/NO_SAFETY_WARNING pair just above, for
  // the extra firing confirmation shown when the task-watchdog panic is
  // disabled (also mirrored, verbatim, in the LCD's ui_page_home.c dialog).
  const hPath = find_file(dir, 'watchdog_cfg.h');
  const jsPath = find_file(dir, 'main_page.html');
  if (hPath && jsPath) {
    checked++;
    const cText = extract_c_macro_string(fs.readFileSync(hPath, 'utf8'), 'WATCHDOG_CFG_FIRING_WARNING');
    const jsText = extract_js_var_string(fs.readFileSync(jsPath, 'utf8'), 'WATCHDOG_CFG_FIRING_WARNING_JS');
    if (cText === null) {
      bad++;
      console.log(`watchdog_cfg.h: could not find WATCHDOG_CFG_FIRING_WARNING macro`);
    } else if (jsText === null) {
      bad++;
      console.log(`main_page.html: could not find WATCHDOG_CFG_FIRING_WARNING_JS variable`);
    } else if (cText !== jsText) {
      bad++;
      console.log(`WATCHDOG_CFG_FIRING_WARNING (watchdog_cfg.h) and WATCHDOG_CFG_FIRING_WARNING_JS (main_page.html) disagree:\n` +
                  `  C:  ${JSON.stringify(cText)}\n` +
                  `  JS: ${JSON.stringify(jsText)}`);
    }
  }
}

{
  // Guard against re-introducing the exact bug this de-duplication pass
  // fixed (2026-09-18): net/ota_page.html used to carry its own inline
  // sha256()/hmacSha256() and set 'X-Ota-Mac' by hand; settings_page.html
  // needed the identical handshake and had NO copy at all, so its
  // Reboot/factory-reset buttons always failed with "missing or malformed
  // X-Ota-Mac header". The fix moved the one true implementation into
  // app.js (window.kcOtaAuthedFetch et al.) -- this check is what stops a
  // future page from quietly growing a second copy instead of calling it.
  //
  // Mechanical and narrow, same shape as the two string-drift checks above:
  // any page/script under `dir` other than app.js itself that (a) defines
  // its own sha256/hmacSha256 function, or (b) references the literal
  // header name 'X-Ota-Mac' (case-insensitive, either quote style) is
  // failing, full stop -- the only sanctioned place either of those may
  // appear is inside app.js's kcOta* helpers.
  const appJsPath = find_file(dir, 'app.js');
  const reOwnCrypto = /\bfunction\s+(sha256|hmacSha256)\s*\(/;
  const reOtaMacHeader = /['"]x-ota-mac['"]/i;
  for (const fullPath of walk_files(dir).filter(p => /\.(html|js)$/.test(p))) {
    if (appJsPath && path.resolve(fullPath) === path.resolve(appJsPath)) continue;
    checked++;
    const src = fs.readFileSync(fullPath, 'utf8');
    const f = path.relative(dir, fullPath).split(path.sep).join('/');
    if (reOwnCrypto.test(src)) {
      bad++;
      console.log(`${f}: defines its own sha256/hmacSha256 instead of using ` +
                   `window.kcOtaCrypto (app.js) -- the OTA handshake must have exactly one implementation.`);
    }
    if (reOtaMacHeader.test(src)) {
      bad++;
      console.log(`${f}: sets the X-Ota-Mac header directly instead of going through ` +
                   `window.kcOtaAuthedFetch (app.js) -- that is exactly the duplication this check exists to catch.`);
    }
  }
}

{
  // The global 401/403 login-escalation wrapper in app.js (owner report,
  // 2026-09-21: "ask me for [a password], not just show a page"). This is
  // a narrow, mechanical guard against silently losing the wrapping or the
  // single retry in a future edit -- it does not exercise the browser
  // behaviour itself (that needs check_ui_responsive_sweep.ps1 / a live
  // click), only that the source still has the shape it must have:
  //   1. window.fetch is reassigned (the interception point for every
  //      caller in every page, including app.js's own kcFetchWithSafetyAck/
  //      kcOtaAuthedFetch, which call the bare `fetch` identifier and so
  //      pick up whatever window.fetch currently is).
  //   2. the reassigned function checks X-Kiln-Auth-Reason (the header
  //      http_auth_http.c sets ONLY on HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
  //      the signed-in-wrong-role case) -- without this, a 403 from an
  //      unrelated route (ota_http.c's verify failure, etc.) would wrongly
  //      pop the admin-login modal.
  //   3. exactly one retry call follows that check (nativeFetch(input,
  //      init)) -- the SAME request, once, on a successful login. A retry
  //      count of zero silently drops the operator's action after they log
  //      in.
  const appJsPath = find_file(dir, 'app.js');
  if (appJsPath) {
    checked++;
    const src = fs.readFileSync(appJsPath, 'utf8');
    if (!/window\.fetch\s*=\s*function/.test(src)) {
      bad++;
      console.log(`app.js: window.fetch is not reassigned -- the global 401/403 login-escalation ` +
                   `wrapper (owner report, 2026-09-21) is missing or was refactored away from the ` +
                   `one interception point every page's fetch() calls rely on.`);
    } else if (!/X-Kiln-Auth-Reason/.test(src)) {
      bad++;
      console.log(`app.js: the fetch wrapper no longer checks X-Kiln-Auth-Reason -- it would treat ` +
                   `every 403 (including unrelated ones from ota_http.c/diagnostics_http.c/` +
                   `profiles_live_http.c) as "wrong role" and pop the admin-login modal on all of them.`);
    } else {
      // Matches only the retry-after-login call site (`ok ? nativeFetch(...) :
      // resp`), not the wrapper's other, unrelated nativeFetch(input, init)
      // call sites (the auth-exempt pass-through, the initial request) --
      // those are expected to appear exactly once each and are not what
      // this guard is protecting.
      const retryCount = (src.match(/ok\s*\?\s*nativeFetch\(input,\s*init\)\s*:\s*resp/g) || []).length;
      if (retryCount !== 1) {
        bad++;
        console.log(`app.js: expected exactly one retry-after-login call in the fetch wrapper ` +
                     `(the single retry the owner's plan requires), found ${retryCount}.`);
      }
    }
  }
}

console.log(`\nchecked ${checked} script/style blocks, ${bad} problem(s)`);
process.exit(bad ? 1 : 0);
