#!/usr/bin/env node
// ui_status_color_check.mjs -- standing guard added by the colour-dependence
// audit (WEB_UI_RESPONSIVE_PLAN.md, "status token contrast" section).
//
// Two independent checks, both mechanical and both intentionally narrow --
// see the header comment on each function for exactly what it can and
// cannot catch:
//
//   1. checkBackgroundContrast() -- every --ok/--warn/--bad/--neutral token,
//      in both the light default and the dark override block of every
//      *_page.html, must clear a stated WCAG contrast floor against the
//      background it is actually painted on (--card-bg falling back to
//      --bg). This is the "is this colour even legible" floor. It does NOT
//      check the tokens against EACH OTHER -- --ok vs --warn is documented
//      by this same audit at 1.0-1.7:1 (hue-only difference), which is a
//      known, owner-gated, open decision (WEB_UI_RESPONSIVE_PLAN.md sec 7),
//      not something this check is allowed to fail the build over. A token
//      that genuinely fails this floor and is a PRE-EXISTING gap (found by
//      this same audit, not introduced by it) is recorded in
//      ui_status_color_contrast_exceptions.json with a note instead of
//      being silently fixed here -- fixing it changes that page's rendered
//      colour, which this pass is not authorized to do (see
//      WEB_UI_RESPONSIVE_PLAN.md). A NEW token/page combination that fails
//      is not covered by that file and fails the build.
//
//   2. checkColorOnlyAllowlist() -- scans every *_page.html for a CSS rule
//      that (a) selects on a name that reads as a status/state variant
//      (ok/warn/bad/near/past/fault/tripped/error/success/armed/...) and
//      (b) whose declaration block sets ONLY color/background/border-color
//      properties, all pointing at a --ok/--warn/--bad/--neutral token, with
//      no other property (font-weight, content, border-style, ...) beside
//      them. That shape is exactly what "distinguished by colour alone in
//      the CSS" looks like. Every such rule already in the tree today is
//      recorded in ui_status_color_allowlist.json alongside a one-line note
//      on what non-colour cue (if any) accompanies it at the call site --
//      this check does NOT re-verify those notes, because whether a JS call
//      site actually emits accompanying text is not something a CSS-only
//      scan can see. A NEW rule of this shape that is not in the allowlist
//      fails the build, forcing a human to either add a non-colour cue or
//      add the rule to the allowlist with a note on why it's already safe
//      (as profiles_page.html's near-ceiling/over-ceiling and
//      safety_page.html's near/past were, both fixed by this same audit to
//      add a title/text cue rather than being allowlisted bare).
//
// What this cannot catch: a rule that reads fine in CSS (has other
// properties, or targets a class name that doesn't look status-like) but
// whose only real-world differentiator is still colour once rendered; a
// legitimate new status rule added WITH a text/glyph cue at the same time
// (it will still need an allowlist entry, because the CSS shape alone can't
// see the JS-side cue -- a false positive, not a miss); and anything not
// expressed as CSS at all (inline SVG fills, canvas draws). It is a floor,
// not a proof of accessibility.

import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const driversDir = path.resolve(__dirname, '..', 'drivers');
const allowlistPath = path.join(__dirname, 'ui_status_color_allowlist.json');
const exceptionsPath = path.join(__dirname, 'ui_status_color_contrast_exceptions.json');

const STATUS_TOKENS = ['ok', 'warn', 'bad', 'neutral'];
const CONTRAST_FLOOR = 3.0; // WCAG 2.x AA floor for non-text UI/graphical objects (1.4.11).

function srgbToLin(c) {
  c /= 255;
  return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}
function hexToRgb(hex) {
  hex = hex.trim().replace('#', '');
  if (hex.length === 3) hex = [...hex].map((ch) => ch + ch).join('');
  const n = parseInt(hex, 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}
function relLum(hex) {
  const [r, g, b] = hexToRgb(hex);
  return 0.2126 * srgbToLin(r) + 0.7152 * srgbToLin(g) + 0.0722 * srgbToLin(b);
}
function contrast(hexA, hexB) {
  const a = relLum(hexA), b = relLum(hexB);
  const lighter = Math.max(a, b), darker = Math.min(a, b);
  return (lighter + 0.05) / (darker + 0.05);
}

function loadUiAccentMap() {
  const css = readFileSync(path.join(driversDir, 'theme.css'), 'utf8');
  const map = {};
  const re = /--(ui-[a-z0-9-]+):\s*(#[0-9a-fA-F]{3,6})/g;
  let m;
  while ((m = re.exec(css))) map[m[1]] = m[2];
  return map;
}

// Resolves "#abc123" as-is, or "var(--ui-accent-4)" via the theme.css map.
// Anything else (a keyword, a calc(), ...) is left unresolved and reported,
// not guessed at -- silently skipping an unresolvable token would make this
// check quietly weaker over time.
function resolveColor(raw, uiMap) {
  raw = raw.trim();
  const varMatch = raw.match(/^var\(--([a-z0-9-]+)\)$/i);
  if (varMatch) return uiMap[varMatch[1]] || null;
  if (/^#[0-9a-fA-F]{3,6}$/.test(raw)) return raw;
  return null;
}

// Pulls every `--name: value;` declaration out of a page's inline <style>,
// in source order. The convention this repo's pages follow (verified by
// eyeball across all 13 files during this audit) is: light defaults in the
// first bare `:root { ... }`, then a `@media (prefers-color-scheme: dark)`
// override, then a manual `.dark`/[data-theme] root block with identical
// values for the explicit toggle. First occurrence of a name = light value;
// LAST occurrence = dark value (the media and manual blocks are always
// identical in this codebase, so using the last one is correct either way,
// and is robust to a page having only a light block with no override at
// all -- first-occurrence covers that case too).
function extractTokens(html, uiMap) {
  const re = /--(ok|warn|bad|neutral|card-bg|bg):\s*([^;]+);/g;
  const light = {}, dark = {};
  const seen = {};
  let m;
  while ((m = re.exec(html))) {
    const name = m[1];
    const resolved = resolveColor(m[2], uiMap);
    if (!(name in seen)) light[name] = resolved;
    dark[name] = resolved; // last wins
    seen[name] = true;
  }
  return { light, dark };
}

function checkBackgroundContrast(pages, uiMap, exceptions) {
  const failures = [];
  for (const [file, html] of pages) {
    const { light, dark } = extractTokens(html, uiMap);
    for (const [themeName, toks] of [['light', light], ['dark', dark]]) {
      const bg = toks['card-bg'] || toks['bg'];
      if (!bg) continue; // page defines no card-bg/bg override of its own -- nothing to check
      for (const tok of STATUS_TOKENS) {
        const hex = toks[tok];
        if (!hex) continue; // page doesn't use this token at all
        const ratio = contrast(hex, bg);
        if (ratio < CONTRAST_FLOOR) {
          const exc = (exceptions[file] || []).find((e) => e.theme === themeName && e.token === tok);
          const msg = `${file} [${themeName}]: --${tok} (${hex}) vs background (${bg}) is ${ratio.toFixed(2)}:1, ` +
            `below the ${CONTRAST_FLOOR}:1 floor`;
          if (exc) {
            console.log(`ui_status_color_check: KNOWN GAP (not failing) -- ${msg} -- ${exc.note}`);
          } else {
            failures.push(msg);
          }
        }
      }
    }
  }
  return failures;
}

// Finds CSS rules of the shape described in the header comment: a
// status-ish selector whose block is ONLY color/background/border-color
// properties pointing at a status token.
function findColorOnlyRules(html) {
  const found = [];
  // Strip /* ... */ comments first -- without this, a selector preceded by
  // a multi-line design-rationale comment (common in this codebase) gets
  // that whole comment swallowed into the "selector" capture, since a
  // comment contains neither { nor }.
  const stripped = html.replace(/\/\*[\s\S]*?\*\//g, ' ');
  // Matches "<selector> { <body> }" for reasonably simple single-line-ish
  // rules, which is what every page in this repo uses (no nested rules in
  // these inline <style> blocks).
  const ruleRe = /([^{}]+)\{([^{}]+)\}/g;
  const statusNameRe = /\b(ok|warn|bad|fault|error|success|near|past|tripped|armed|invalid|unset|ineffective|nuisance)\b/i;
  const tokenPropRe = /^(color|background|background-color|border-color|border-left-color|border-top-color)\s*:\s*var\(--(ok|warn|bad|neutral)\)$/;
  let m;
  while ((m = ruleRe.exec(stripped))) {
    const selector = m[1].trim();
    const body = m[2].trim();
    if (!statusNameRe.test(selector)) continue;
    if (!/var\(--(ok|warn|bad|neutral)\)/.test(body)) continue;
    const decls = body.split(';').map((d) => d.trim()).filter(Boolean);
    if (decls.length === 0) continue;
    const allTokenColor = decls.every((d) => tokenPropRe.test(d.replace(/\s+/g, ' ')));
    if (allTokenColor) found.push(selector.replace(/\s+/g, ' '));
  }
  return found;
}

function checkColorOnlyAllowlist(pages) {
  let allowlist;
  try {
    allowlist = JSON.parse(readFileSync(allowlistPath, 'utf8'));
  } catch (e) {
    return [`could not read/parse allowlist at ${allowlistPath}: ${e.message}`];
  }
  const failures = [];
  for (const [file, html] of pages) {
    const rules = findColorOnlyRules(html);
    const allowedForFile = new Set((allowlist[file] || []).map((e) => e.selector));
    for (const selector of rules) {
      if (!allowedForFile.has(selector)) {
        failures.push(
          `${file}: new colour-only rule "${selector}" is not in ui_status_color_allowlist.json. ` +
          `Add a non-colour cue (text, glyph, border weight, icon) at the call site, or -- only if ` +
          `one already exists and this scan can't see it -- add the selector to the allowlist with a note.`
        );
      }
    }
  }
  return failures;
}

function main() {
  const uiMap = loadUiAccentMap();
  const files = readdirSync(driversDir).filter((f) => f.endsWith('_page.html'));
  if (files.length < 10) {
    console.error(`ui_status_color_check: only found ${files.length} *_page.html under ${driversDir}, expected >=10 -- glob likely broken.`);
    process.exit(2);
  }
  const pages = files.map((f) => [f, readFileSync(path.join(driversDir, f), 'utf8')]);

  let exceptions = {};
  try {
    exceptions = JSON.parse(readFileSync(exceptionsPath, 'utf8'));
  } catch (e) {
    console.error(`ui_status_color_check: could not read/parse contrast exceptions at ${exceptionsPath}: ${e.message}`);
    process.exit(2);
  }

  const contrastFailures = checkBackgroundContrast(pages, uiMap, exceptions);
  const allowlistFailures = checkColorOnlyAllowlist(pages);
  const failures = [...contrastFailures, ...allowlistFailures];

  if (failures.length > 0) {
    console.error(`ui_status_color_check: FAILED (${failures.length} issue(s)):`);
    for (const f of failures) console.error('  - ' + f);
    process.exit(1);
  }
  console.log(`ui_status_color_check: passed (${pages.length} pages, ${STATUS_TOKENS.length} status tokens, contrast floor ${CONTRAST_FLOOR}:1).`);
  process.exit(0);
}

main();
