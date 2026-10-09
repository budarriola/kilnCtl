// Topbar consistency check: Home and Menu must sit at the same height on
// every page, and each label must be optically centred in its own button.
//
// Exists because of two real, owner-reported bugs that static review missed.
// nav.js injects this topbar into every page, but Home is an <a> and Menu is
// a <button>, so any page-local rule targeting bare `button` hits one and not
// the other -- zones_page.html's `button { margin-top: 1em; }` pushed Menu
// 13.6px below Home. Separately, a <button> does not inherit the page font,
// and `line-height: normal` resolves taller in a button than in a link, which
// left the Menu label 3px low on EVERY page.
//
// Usage:  node topbar_check.js            (needs a reachable board)
//         KILN_BASE=http://1.2.3.4 node topbar_check.js
// Exits non-zero if any page is misaligned.
const { chromium } = require('playwright');

// Board address: override with KILN_BASE when the board is not on its usual IP.
const BASE = process.env.KILN_BASE || 'http://192.168.1.156';
// 2026-08-28: /diagnostics/thermo and /board_temps were removed -- both pages
// were folded into /diagnostics at the owner's request ("the board health page
// should be folded into the diagnostics page", "Thermocouple Faults should also
// be combined into the diagnostic page"). /settings/manual went the same day
// ("you can remove the manual relay controle page from the web gui").
//
// A sweep that keeps requesting a deleted route does not fail -- it just
// records a 404 page as having no misaligned topbar, which is true and
// useless. That is the shape this repo keeps hitting: a check whose subject
// disappeared, still reporting success. Kept as a list of LIVE routes only.
const pages = ['/settings/zones','/wifi','/readiness','/safety','/diagnostics','/profiles',
               '/settings','/settings/display','/settings/backup','/ota',
               '/safety/commissioning','/settings/safety'];
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({viewport:{width:520,height:800}});
  let bad = 0;
  for (const path of pages) {
    try {
      await p.goto(BASE+path, {waitUntil:'domcontentloaded', timeout:20000});
      await p.waitForTimeout(700);
    } catch (e) { console.log(`${path.padEnd(24)} LOAD FAIL ${e.message.split('\n')[0]}`); bad++; continue; }
    const r = await p.evaluate(() => {
      const h=document.querySelector('.kc-home-btn'), m=document.querySelector('.kc-menu-btn');
      if(!m) return {none:true};
      const gap = el => { const b=el.getBoundingClientRect(); const rg=document.createRange();
        rg.selectNodeContents(el); const t=rg.getBoundingClientRect();
        return {top:+b.top.toFixed(1), h:+b.height.toFixed(1),
                gt:+(t.top-b.top).toFixed(1), gb:+(b.bottom-t.bottom).toFixed(1),
                mt:getComputedStyle(el).marginTop, ff:getComputedStyle(el).fontFamily.slice(0,12)}; };
      return {home: h?gap(h):null, menu: gap(m)};
    });
    if (r.none) { console.log(`${path.padEnd(24)} no topbar`); continue; }
    const dTop = r.home ? Math.abs(r.home.top - r.menu.top) : 0;
    const centerOff = Math.abs(r.menu.gt - r.menu.gb);
    const homeOff = r.home ? Math.abs(r.home.gt - r.home.gb) : 0;
    const ok = dTop <= 0.6 && centerOff <= 1.5 && homeOff <= 1.5;
    if (!ok) bad++;
    console.log(`${path.padEnd(24)} ${ok?'OK  ':'BAD '} topDelta=${dTop.toFixed(1)} menuCenterOff=${centerOff.toFixed(1)} homeCenterOff=${homeOff.toFixed(1)} menuMT=${r.menu.mt} menuFont=${r.menu.ff}`);
  }
  console.log(bad ? `\n${bad} page(s) still wrong` : '\nALL TOPBARS ALIGNED');
  process.exitCode = bad ? 1 : 0;
  await b.close();
})();
