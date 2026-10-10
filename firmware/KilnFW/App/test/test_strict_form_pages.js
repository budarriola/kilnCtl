/* Source-contract test for the strict-form-parsing page follow-ups:
 * wizard step 11 posts all four set_policy fields, the zones autotune duty box
 * omits step_duty when blank, and a profile on/off rule with comparison none
 * posts temp_c=0. Run: node firmware/KilnFW/App/test/test_strict_form_pages.js */
'use strict';
const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const dir = resolveDriversDir(__dirname);
const rd = (f) => fs.readFileSync(resolveDriverFile(dir, f), 'utf8');
let fail = 0;
function ok(c, l) { if (!c) { fail++; console.error('FAIL ' + l); } else console.log('ok ' + l); }

const wiz = rd('setup_wizard_page.html');
const i = wiz.indexOf("'cmd=set_policy&web_enabled='");
const blk = wiz.slice(Math.max(0, i - 700), i + 500);
ok(i > 0 && /fetch\('\/api\/auth\/config'\)/.test(blk), 'wizard reads /api/auth/config before set_policy');
ok(/lcd_enabled=/.test(blk) && /web_timeout_min=/.test(blk) && /lcd_timeout_min=/.test(blk), 'wizard sends all four policy fields');

const zp = rd('zones_page.html');
ok(/\(duty === '' \? '' : '&step_duty=' \+ duty\)/.test(zp), 'autotune omits step_duty when blank');
ok(!/'&step_duty=' \+ duty \+/.test(zp), 'autotune has no unconditional step_duty');

const pp = rd('profiles_page.html');
ok(/var tempC = tempCmp === 0 \? 0 :/.test(pp), 'profile rule sends temp_c=0 when comparison is none');
process.exit(fail ? 1 : 0);
