// kilnCtl web UI sweep: every page, several viewports, every control.
//
// Checks per page/viewport:
//   - horizontal overflow of the document (the "goes out of frame on the
//     phone" class of bug the owner reported)
//   - any element whose right edge exceeds the viewport
//   - overlapping interactive controls (the topbar-overlap class of bug)
//   - tap targets smaller than 32px in either axis on touch widths
//   - console errors and failed network requests
//   - buttons that are present but do nothing detectable
//
// Buttons are clicked in a SEPARATE pass per page so one navigation cannot
// hide later checks; destructive controls are matched by label and skipped.
const { chromium } = require('playwright');

const BASE = process.env.KILN_BASE || 'http://192.168.1.156';

const ONLY = process.env.KILN_VP;
const ALL_VIEWPORTS = [
  { name: 'phone-320', width: 320, height: 568, touch: true },
  { name: 'phone-360', width: 360, height: 740, touch: true },
  { name: 'phone-390', width: 390, height: 844, touch: true },
  { name: 'tablet-768', width: 768, height: 1024, touch: true },
  { name: 'tablet-820', width: 820, height: 1180, touch: true },
  { name: 'desktop-1280', width: 1280, height: 800, touch: false },
  { name: 'desktop-1920', width: 1920, height: 1080, touch: false },
];
const VIEWPORTS = ONLY ? ALL_VIEWPORTS.filter(v => ONLY.split(',').includes(v.name)) : ALL_VIEWPORTS;

const PAGES = [
  ['/', 'Dashboard'],
  ['/readiness', 'Readiness'],
  ['/settings/zones', 'Zones'],
  // '/settings/relays' removed 2026-08-27 with the page itself -- the rule
  // engine was deleted; relay/IO control is now a firing profile segment.
  // '/settings/manual' removed 2026-08-27 with the page itself -- see
  // nav.js's removal comment.
  ['/profiles', 'Profiles'],
  ['/wifi', 'WiFi'],
  ['/ota', 'OTA'],
  // '/diagnostics/thermo' and '/board_temps' removed 2026-08-27, folded into
  // /diagnostics (owner request: "the board health page should be folded
  // into the diagnostics page" / "Thermocouple Faults should also be
  // combined into the diagnostic page") -- see nav.js's 2026-08-27 comment
  // and diagnostics_page.html's new "Board health"/"Thermocouple faults"
  // sections. The one /diagnostics entry above already sweeps that content.
  ['/diagnostics', 'Diagnostics'],
  ['/safety', 'Safety'],
  ['/safety/commissioning', 'Commissioning'],
  ['/settings/backup', 'Backup'],
  ['/settings', 'Settings'],
];

// Never click these, by accessible name (case-insensitive substring).
const DESTRUCTIVE = [
  'reset', 'erase', 'factory', 'restore', 'stop firing', 'start',
  'update', 'rollback', 'forget', 'delete', 'clear latched', 'reboot',
  'apply', 'save', 'commit', 'bench preset', 'download',
];

function isDestructive(label) {
  const l = (label || '').toLowerCase();
  return DESTRUCTIVE.some((d) => l.includes(d));
}

async function auditLayout(page, vp) {
  return page.evaluate((vpWidth) => {
    const problems = [];
    const de = document.documentElement;
    if (de.scrollWidth > vpWidth + 1) {
      problems.push({
        kind: 'document-overflow',
        detail: `scrollWidth ${de.scrollWidth} > viewport ${vpWidth}`,
      });
    }
    const describe = (el) => {
      const id = el.id ? `#${el.id}` : '';
      const cls = (typeof el.className === 'string' && el.className)
        ? '.' + el.className.trim().split(/\s+/).slice(0, 2).join('.') : '';
      const txt = (el.textContent || '').trim().slice(0, 28).replace(/\s+/g, ' ');
      return `${el.tagName.toLowerCase()}${id}${cls}${txt ? ` "${txt}"` : ''}`;
    };
    const all = Array.from(document.querySelectorAll('body *'));
    for (const el of all) {
      const cs = getComputedStyle(el);
      if (cs.display === 'none' || cs.visibility === 'hidden' || cs.opacity === '0') continue;
      const r = el.getBoundingClientRect();
      if (r.width === 0 && r.height === 0) continue;
      /* Skip anything inside a horizontal scroll container. A wide table in a
       * .table-scroll wrapper is the CORRECT design here, not a bug: the
       * wrapper scrolls, the page does not. Without this, /settings/relays
       * reported its 394px table as an overflow on a 390px phone while
       * document.scrollWidth was exactly the viewport width -- a false alarm
       * that costs real time to re-diagnose every time it appears. */
      let inScroller = false;
      for (let a = el.parentElement; a && a !== document.body; a = a.parentElement) {
        const ox = getComputedStyle(a).overflowX;
        if (ox === 'auto' || ox === 'scroll') { inScroller = true; break; }
      }
      if (inScroller) continue;

      if (r.right > vpWidth + 1 && cs.position !== 'fixed') {
        // Only report the innermost offenders: skip if a child also overflows.
        const childOverflows = Array.from(el.children).some((c) => {
          const cr = c.getBoundingClientRect();
          return cr.right > vpWidth + 1;
        });
        if (!childOverflows) {
          problems.push({
            kind: 'element-overflow',
            detail: `${describe(el)} right=${Math.round(r.right)} > ${vpWidth}`,
          });
        }
      }
    }
    // Overlapping interactive controls.
    const interactive = all.filter((el) => {
      const cs = getComputedStyle(el);
      if (cs.display === 'none' || cs.visibility === 'hidden') return false;
      return ['BUTTON', 'A', 'INPUT', 'SELECT'].includes(el.tagName);
    });
    for (let i = 0; i < interactive.length; i++) {
      for (let j = i + 1; j < interactive.length; j++) {
        const a = interactive[i].getBoundingClientRect();
        const b = interactive[j].getBoundingClientRect();
        if (a.width === 0 || b.width === 0) continue;
        if (interactive[i].contains(interactive[j]) || interactive[j].contains(interactive[i])) continue;
        const ox = Math.min(a.right, b.right) - Math.max(a.left, b.left);
        const oy = Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top);
        if (ox > 2 && oy > 2) {
          problems.push({
            kind: 'control-overlap',
            detail: `${describe(interactive[i])} overlaps ${describe(interactive[j])} by ${Math.round(ox)}x${Math.round(oy)}px`,
          });
        }
      }
    }
    return problems;
  }, vp.width);
}

async function auditTapTargets(page, vpWidth) {
  return page.evaluate(() => {
    const small = [];
    const els = Array.from(document.querySelectorAll('button, a, input[type=checkbox], select'));
    for (const el of els) {
      const cs = getComputedStyle(el);
      if (cs.display === 'none' || cs.visibility === 'hidden') continue;
      const r = el.getBoundingClientRect();
      if (r.width === 0 || r.height === 0) continue;
      // Two classes of false positive this used to report, both verified by
      // hand against the live board before being excluded:
      //
      //  1. An <input> wrapped in a <label>. The label is the real hit area --
      //     measured 358x35 for a 13x13 checkbox on the zones page -- so the
      //     input's own box says nothing about whether a finger can hit it.
      //  2. A link inside running prose. "Fix this ->" in a sentence is not a
      //     button, and padding it to 32px would wreck the paragraph. Only
      //     links that stand alone (block/flex, or the sole child of their
      //     parent) are judged as tap targets.
      const label = el.closest('label');
      if (label && label !== el) {
        const lr = label.getBoundingClientRect();
        if (lr.height >= 32 && lr.width >= 24) continue;
      }
      if (el.tagName === 'A') {
        const disp = cs.display;
        const aloneInParent = el.parentElement &&
          el.parentElement.childElementCount === 1 &&
          (el.parentElement.textContent || '').trim() === (el.textContent || '').trim();
        const standalone = disp === 'block' || disp === 'flex' ||
                           disp === 'inline-block' || disp === 'inline-flex' || aloneInParent;
        if (!standalone) continue; // a link in a sentence, not a control
      }
      if (r.height < 32 || r.width < 24) {
        const txt = (el.textContent || el.value || '').trim().slice(0, 24);
        small.push(`${el.tagName.toLowerCase()} "${txt}" ${Math.round(r.width)}x${Math.round(r.height)}`);
      }
    }
    return small;
  });
}

(async () => {
  const browser = await chromium.launch();
  const results = [];

  for (const vp of VIEWPORTS) {
    const ctx = await browser.newContext({
      viewport: { width: vp.width, height: vp.height },
      hasTouch: vp.touch,
      isMobile: false,
    });
    for (const [path, name] of PAGES) {
      const page = await ctx.newPage();
      const consoleErrors = [];
      const netFails = [];
      page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text().slice(0, 160)); });
      page.on('requestfailed', (r) => netFails.push(`${r.url().replace(BASE, '')} ${r.failure()?.errorText || ''}`));
      page.on('response', (r) => { if (r.status() >= 400) netFails.push(`${r.url().replace(BASE, '')} HTTP ${r.status()}`); });

      let problems = [];
      let small = [];
      let loadErr = null;
      /* Progress goes to stderr as each page finishes, NOT to stdout with the
       * report at the end. An earlier version printed nothing until the whole
       * run completed, so when it hung there was no way to see WHICH page it
       * hung on -- it just produced an empty file after a 15-minute timeout.
       * A check you cannot watch stall is a check you stop trusting. */
      process.stderr.write(`  ${vp.name} ${path} ... `);
      const started = Date.now();
      try {
        await page.goto(BASE + path, { waitUntil: 'domcontentloaded', timeout: 15000 });
        /* 1800ms, not 700: several pages build their real content from a
           fetch (zones' checkbox labels, the readiness list, the faults
           table), and measuring too early judges a half-built page. That
           produced a standing false positive -- zones' checkbox label reads
           358x35 once loaded, comfortably above the tap-target floor, but
           was reported as a 13x13 target when measured mid-load. */
        await page.waitForTimeout(1800); // let fetches land and pollers paint
        problems = await auditLayout(page, vp);
        small = await auditTapTargets(page, vp.width);
      } catch (e) {
        loadErr = String(e).split('\n')[0].slice(0, 160);
      }
      process.stderr.write(`${Date.now() - started}ms${loadErr ? ' LOAD ERROR' : ''}\n`);
      results.push({
        viewport: vp.name, page: name, path,
        loadErr, problems, small,
        consoleErrors: [...new Set(consoleErrors)],
        netFails: [...new Set(netFails)],
      });
      await page.close();
    }
    await ctx.close();
  }

  /* Bounded teardown. browser.close() has been observed to hang here after
   * the last page -- the pages poll on timers, and a stuck close meant the
   * whole run produced NO report despite every page having been audited
   * successfully. The results are already in hand at this point, so a
   * teardown that will not finish must never cost us the report. */
  await Promise.race([
    browser.close(),
    new Promise((res) => setTimeout(res, 5000)),
  ]);

  // ---- report ----
  const byKind = {};
  for (const r of results) {
    for (const p of r.problems) {
      const key = `${p.kind} | ${r.page} | ${p.detail}`;
      (byKind[key] = byKind[key] || []).push(r.viewport);
    }
  }
  console.log('=== LAYOUT PROBLEMS (viewports affected) ===');
  const keys = Object.keys(byKind).sort();
  if (!keys.length) console.log('  none');
  for (const k of keys) console.log(`  ${k}\n      @ ${byKind[k].join(', ')}`);

  console.log('\n=== LOAD ERRORS ===');
  const le = results.filter((r) => r.loadErr);
  if (!le.length) console.log('  none');
  for (const r of le) console.log(`  ${r.page} @ ${r.viewport}: ${r.loadErr}`);

  console.log('\n=== CONSOLE ERRORS / FAILED REQUESTS ===');
  const seen = new Set();
  for (const r of results) {
    for (const e of r.consoleErrors) {
      const k = `${r.page}: ${e}`;
      if (!seen.has(k)) { seen.add(k); console.log(`  [console] ${k}`); }
    }
    for (const e of r.netFails) {
      const k = `${r.page}: ${e}`;
      if (!seen.has(k)) { seen.add(k); console.log(`  [network] ${k}`); }
    }
  }
  if (!seen.size) console.log('  none');

  console.log('\n=== SMALL TAP TARGETS (touch viewports) ===');
  const tt = {};
  for (const r of results) {
    if (!r.viewport.startsWith('phone') && !r.viewport.startsWith('tablet')) continue;
    for (const s of r.small) {
      const k = `${r.page} | ${s}`;
      (tt[k] = tt[k] || []).push(r.viewport);
    }
  }
  const tks = Object.keys(tt).sort();
  if (!tks.length) console.log('  none');
  for (const k of tks.slice(0, 40)) console.log(`  ${k}\n      @ ${tt[k].join(', ')}`);
  if (tks.length > 40) console.log(`  ... and ${tks.length - 40} more`);

  /* Explicit exit for the same reason: if a browser process is still winding
   * down, node would otherwise sit on a live handle after the report is
   * printed and look like a hang to whoever ran this. */
  process.exit(0);
})();
