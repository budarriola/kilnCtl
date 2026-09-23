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
  //   2. the reassigned function actually READS the X-Kiln-Auth-Reason
  //      response header -- matched as `headers.get('X-Kiln-Auth-Reason')`,
  //      the code shape, not the bare name: review fix 2026-09-21, the
  //      original /X-Kiln-Auth-Reason/ pattern also matched this file's own
  //      explanatory comments in app.js, so renaming the header in the live
  //      check left the guard green (negative-tested: it did). The header
  //      http_auth_http.c sets ONLY on HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
  //      the signed-in-wrong-role case) -- without this, a 403 from an
  //      unrelated route (ota_http.c's verify failure, etc.) would wrongly
  //      pop the admin-login modal.
  //   3. exactly one retry call follows that check, guarded by a one-shot
  //      "already retried" marker (`__kcAuthRetried`) so a second
  //      insufficient_role 403 on the retry itself does not loop into a
  //      second modal. The retry re-enters `window.fetch` rather than
  //      `nativeFetch` so a 401 on the retry still gets the login
  //      redirect; see app.js's own comment at that call site for what
  //      re-entry does NOT buy (kcFetchWithSafetyAck/kcOtaAuthedFetch sit
  //      ABOVE this wrapper, so nothing re-signs the retried request)
  //      (review fix, 2026-09-21 (b); corrected by review 2026-09-21).
  //   4. the login modal is a real dialog: role="dialog" (+ aria-modal) so
  //      assistive tech announces it as one, and Escape cancels it the same
  //      as the Cancel button (review fix, 2026-09-21 (a)).
  const appJsPath = find_file(dir, 'app.js');
  if (appJsPath) {
    checked++;
    const src = fs.readFileSync(appJsPath, 'utf8');
    if (!/window\.fetch\s*=\s*function/.test(src)) {
      bad++;
      console.log(`app.js: window.fetch is not reassigned -- the global 401/403 login-escalation ` +
                   `wrapper (owner report, 2026-09-21) is missing or was refactored away from the ` +
                   `one interception point every page's fetch() calls rely on.`);
    } else if (!/headers\.get\(['"]X-Kiln-Auth-Reason['"]\)/.test(src)) {
      bad++;
      console.log(`app.js: the fetch wrapper no longer checks X-Kiln-Auth-Reason -- it would treat ` +
                   `every 403 (including unrelated ones from ota_http.c/diagnostics_http.c/` +
                   `profiles_live_http.c) as "wrong role" and pop the admin-login modal on all of them.`);
    } else {
      // Matches only the retry-after-login call site (`window.fetch(inputForRetry,
      // retryInit)`), not the wrapper's other, unrelated nativeFetch(input, init)
      // call sites (the auth-exempt pass-through, the initial request) --
      // those are expected to appear exactly once each and are not what
      // this guard is protecting.
      const retryCount = (src.match(/window\.fetch\(inputForRetry,\s*retryInit\)/g) || []).length;
      if (retryCount !== 1) {
        bad++;
        console.log(`app.js: expected exactly one retry-after-login call in the fetch wrapper ` +
                     `(the single retry the owner's plan requires), found ${retryCount}.`);
      }
      // Both halves of the one-shot guard, matched separately: a READ
      // (init.__kcAuthRetried, checked before treating a 403 as
      // insufficient_role) and a SET (retryInit.__kcAuthRetried = true,
      // stamped onto the retried request) -- a sabotage that removes only
      // the set (leaving the string in a comment or the read behind) would
      // pass a bare substring count, so each half needs its own pattern.
      const hasGuardRead = /init\s*&&\s*init\.__kcAuthRetried/.test(src);
      const hasGuardSet = /\.__kcAuthRetried\s*=\s*true\s*;/.test(src);
      if (!hasGuardRead || !hasGuardSet) {
        bad++;
        console.log(`app.js: the retry-after-login call is missing its one-shot __kcAuthRetried ` +
                     `guard (needs both a check and a set) -- without it a second insufficient_role ` +
                     `403 on the retry itself would loop into a second modal.`);
      }
    }
    // Both attributes are matched in their setAttribute() form specifically.
    // A bare /aria-modal/ substring test passed VACUOUSLY (review,
    // 2026-09-21): the explanatory comment above that code in app.js
    // contains the literal text "aria-modal", so deleting the
    // setAttribute('aria-modal', 'true') call left this check green.
    if (!/setAttribute\(\s*['"]role['"]\s*,\s*['"]dialog['"]\s*\)/.test(src) ||
        !/setAttribute\(\s*['"]aria-modal['"]\s*,\s*['"]true['"]\s*\)/.test(src)) {
      bad++;
      console.log(`app.js: the login modal is missing role="dialog"/aria-modal -- assistive tech ` +
                   `would not announce it as a dialog.`);
    }
    if (!/(evt\.key\s*===\s*['"]Escape['"]|evt\.keyCode\s*===\s*27)/.test(src)) {
      bad++;
      console.log(`app.js: the login modal does not appear to handle Escape -- it should cancel the ` +
                   `same way the Cancel button does.`);
    }
    // A2 (opus review, 2026-09-21): once the login POST is dispatched,
    // Escape and Cancel must be ignored until it settles -- otherwise a
    // slow login racing an impatient Escape/Cancel press can finish(false)
    // the modal moments before a successful login would have finish(true)'d
    // it, silently turning a login that DID succeed into one the caller
    // (and kcOtaAuthedFetch's own retry, see below) treats as declined.
    // Matched as a `submitting` guard read inside onKeydown/onCancel, not
    // just anywhere in the file (a stray `if (submitting)` in an unrelated
    // function would pass a bare substring test vacuously).
    if (!/function onKeydown\(evt\) \{\s*\n\s*if \(evt\.key === 'Escape'[^}]*\{\s*\n\s*if \(submitting\) return;/.test(src)) {
      bad++;
      console.log(`app.js: the login modal's Escape handler does not check the 'submitting' guard -- ` +
                   `Escape during an in-flight login POST could cancel a login that is about to succeed.`);
    }
    if (!/function onCancel\(evt\) \{\s*\n\s*if \(submitting\) return;/.test(src)) {
      bad++;
      console.log(`app.js: the login modal's Cancel handler does not check the 'submitting' guard -- ` +
                   `Cancel during an in-flight login POST could cancel a login that is about to succeed.`);
    }
    // Login POST timeout (opus review, 2026-09-21): a stalled board would
    // otherwise leave the modal stuck (Escape/Cancel ignored while
    // `submitting`) until the browser's own timeout. Matched as an
    // AbortController wired into the login fetch's `signal`, not just an
    // AbortController appearing anywhere in the file.
    if (!/new AbortController\(\)/.test(src) ||
        !/signal:\s*controller\.signal/.test(src)) {
      bad++;
      console.log(`app.js: the login modal's POST has no AbortController/signal wired up -- a stalled ` +
                   `board would leave the modal stuck until the browser's own timeout.`);
    }
    if (!/setTimeout\(function \(\) \{\s*\n\s*timedOut = true;\s*\n\s*controller\.abort\(\);/.test(src)) {
      bad++;
      console.log(`app.js: the login modal's AbortController does not appear to be driven by a timer -- ` +
                   `it must abort the POST after a bounded timeout, not just be constructible.`);
    }
    // Buttons must actually be disabled while submitting, not merely have
    // `submitting` read elsewhere -- matched on the disabled= assignment
    // itself, gated by `on` (the setSubmitting(on) parameter), so a stray
    // `.disabled = true` elsewhere in the file cannot pass this vacuously.
    if (!/loginCancelEl\.disabled = on;/.test(src) ||
        !/loginSubmitEl\.disabled = on;/.test(src)) {
      bad++;
      console.log(`app.js: the login modal's Cancel/Submit buttons are not disabled while a login POST ` +
                   `is in flight.`);
    }
    // The modal DOM is built once and reused, so finish() MUST clear the
    // disabled/"Signing in..." state -- finish(true) (success) has no other
    // settle step, and a leftover disabled default submit button blocks even
    // implicit (Enter-key) submission the next time the modal opens. Matched
    // inside finish() specifically, not just anywhere in the file.
    if (!/function finish\(ok\) \{[\s\S]{0,800}?setSubmitting\(false\);/.test(src)) {
      bad++;
      console.log(`app.js: the login modal's finish() does not clear the submitting state -- the ` +
                   `reused modal would reopen with Cancel/Submit still disabled after a successful login.`);
    }
    if (!/loginSubmitEl\.textContent = on \? 'Signing in/.test(src)) {
      bad++;
      console.log(`app.js: the login modal's Submit button does not show a "Signing in..." label while ` +
                   `a login POST is in flight.`);
    }
  }
}

{
  // kcOtaAuthedFetch's own insufficient_role retry (opus review A1,
  // 2026-09-21): the generic window.fetch wrapper above cannot safely
  // replay a signed OTA request after its login modal -- the prehandler
  // denies before the route consumes the single-use nonce, so a replayed
  // X-Ota-Mac is only accepted inside the 30 s OTA_AUTH_NONCE_EXPIRY_MS
  // window, which a human typing a password routinely exceeds. So
  // kcOtaAuthedFetch (a) opts its own request out of the wrapper's retry
  // via the `__kcCallerHandlesAuth` marker, and (b) owns a single
  // login-then-RE-SIGN retry of its own (a fresh challenge + a freshly
  // derived MAC, not a replay of the old one).
  //
  // Scoped to just the kcOtaAuthedFetch function body (from its own
  // assignment to the next top-level section header) so these patterns
  // cannot accidentally match the unrelated window.fetch wrapper above,
  // which has its own, differently-shaped retry.
  const appJsPath = find_file(dir, 'app.js');
  if (appJsPath) {
    checked++;
    const src = fs.readFileSync(appJsPath, 'utf8');
    const startIdx = src.indexOf('window.kcOtaAuthedFetch = function');
    const endIdx = startIdx >= 0 ? src.indexOf('\n  // ----', startIdx) : -1;
    if (startIdx < 0 || endIdx < 0) {
      bad++;
      console.log(`app.js: could not locate window.kcOtaAuthedFetch's function body to check its ` +
                   `insufficient_role retry.`);
    } else {
      const body = src.slice(startIdx, endIdx);
      if (!/__kcCallerHandlesAuth\s*=\s*true\s*;/.test(body)) {
        bad++;
        console.log(`app.js: kcOtaAuthedFetch no longer sets __kcCallerHandlesAuth -- the generic ` +
                     `window.fetch wrapper would try to replay its signed request after login, which ` +
                     `fails once the OTA nonce has expired.`);
      }
      if (!/ensureAdminLogin\(/.test(body)) {
        bad++;
        console.log(`app.js: kcOtaAuthedFetch no longer calls ensureAdminLogin -- a signed OTA request ` +
                     `denied for insufficient_role would surface the stale 403 with no way to elevate.`);
      }
      // "attempt()" is the re-signing call (fetches a fresh challenge, derives
      // a fresh MAC); it must be CALLED exactly twice -- the first send and
      // the one retry -- never looped and never dropped. The negative
      // lookbehind excludes the "function attempt() {" declaration itself,
      // which also matches a bare /attempt\(\)/ substring test.
      const attemptCallCount = (body.match(/(?<!function )attempt\(\)/g) || []).length;
      if (attemptCallCount !== 2) {
        bad++;
        console.log(`app.js: expected kcOtaAuthedFetch to call its re-signing attempt() exactly twice ` +
                     `(the first send and the one retry), found ${attemptCallCount}.`);
      }
    }
  }
}

{
  // Logout control: nav.js must build a Log out button that starts hidden
  // (it must not appear before app.js's session poll confirms a live
  // session -- see setAuthState()'s own comment) and POST /api/auth/logout
  // before navigating away, and app.js's session poll must be the thing
  // that reveals it. Matched narrowly against each file's own real shape,
  // not a loose substring test, so a button that merely EXISTS somewhere
  // (e.g. left permanently visible, or wired to the wrong route) cannot
  // pass vacuously -- same discipline as the login-modal checks above.
  const navJsPath = find_file(dir, 'nav.js');
  if (navJsPath) {
    checked++;
    const src = fs.readFileSync(navJsPath, 'utf8');
    if (!/logoutBtn\.hidden = true;/.test(src)) {
      bad++;
      console.log(`nav.js: the Log out button does not start hidden -- it must only appear once a live ` +
                   `session is confirmed, never by default (web auth may be off entirely, or the caller ` +
                   `may not be logged in).`);
    }
    if (!/fetch\('\/api\/auth\/logout',\s*\{\s*method:\s*'POST'\s*\}\)/.test(src)) {
      bad++;
      console.log(`nav.js: the Log out button's click handler does not POST /api/auth/logout.`);
    }
    if (!/window\.location\.href = '\/login';/.test(src)) {
      bad++;
      console.log(`nav.js: the Log out button's click handler does not navigate to /login afterward.`);
    }
    if (!/function setAuthState\(role, webAuthEnabled\)/.test(src) ||
        !/logoutBtnEl\.hidden = !webAuthEnabled \|\| !role \|\| role === 'none';/.test(src)) {
      bad++;
      console.log(`nav.js: no setAuthState(role, webAuthEnabled) function that hides the Log out button ` +
                   `unless web auth is on AND a role is reported -- with auth off, ` +
                   `GET /api/auth/session reports role "admin" and the button would show on a board ` +
                   `that has no sessions at all.`);
    }
    if (!/window\.kcNav = \{ updateBodyPadding: updateBodyPadding, setAuthState: setAuthState \};/.test(src)) {
      bad++;
      console.log(`nav.js: window.kcNav does not expose setAuthState -- app.js cannot reach nav.js's Log ` +
                   `out button toggle at all.`);
    }
  }

  const appJsPath2 = find_file(dir, 'app.js');
  if (appJsPath2) {
    checked++;
    const src = fs.readFileSync(appJsPath2, 'utf8');
    // Scoped to pollSession()'s own body (same reasoning as the
    // kcOtaAuthedFetch scoping above): a bare substring match for
    // "kcNav.setAuthState" could pass even if it were called from some
    // unrelated, non-session-poll code path that never actually reflects
    // the server's own view of the session.
    const startIdx = src.indexOf('function pollSession()');
    const endIdx = startIdx >= 0 ? src.indexOf('\n  function init()', startIdx) : -1;
    if (startIdx < 0 || endIdx < 0) {
      bad++;
      console.log(`app.js: could not locate pollSession()'s function body to check the logout button wiring.`);
    } else {
      const body = src.slice(startIdx, endIdx);
      if (!/window\.kcNav\.setAuthState\(role, !!\(st && st\.auth_enabled\)\)/.test(body)) {
        bad++;
        console.log(`app.js: pollSession() does not call ` +
                     `window.kcNav.setAuthState(role, !!(st && st.auth_enabled)) -- the Log out button would ` +
                     `never reflect the server's own session state, or would show with web auth off.`);
      }
    }
  }
}

console.log(`\nchecked ${checked} script/style blocks, ${bad} problem(s)`);
process.exit(bad ? 1 : 0);
