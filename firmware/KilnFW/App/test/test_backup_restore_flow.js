/* Executes backup_page.html's real restore click handler (vm, fake DOM) and
 * pins what the operator is told: dry-run refusal text, cancel, the
 * X-Kiln-Config-Ack-Delete header, success vs refusal wording, auth cancel,
 * network failure. Run: node firmware/KilnFW/App/test/test_backup_restore_flow.js */
'use strict';
const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const { runPageScript, flush, fakeResponse } = require('./_page_vm.js');
const HTML = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'backup_page.html'), 'utf8');

let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }

class FakeReader { readAsText(f) { this.result = f.body; setImmediate(() => this.onload()); } }

// responses: array of {status,text} | 'authcancel' | 'netfail', consumed per fetch.
async function scenario(o) {
  const calls = [];
  const queue = o.responses.slice();
  const confirms = [];
  const { els } = runPageScript(HTML, {
    elements: ['themeBtn', 'restoreBtn', 'restoreFile', 'restoreStatus', 'kcModeMirror'],
    FileReader: FakeReader,
    extra: {
      kcConfirm: (msg) => { confirms.push(msg); return Promise.resolve(o.confirm !== false); },
      kcIsAuthCancelled: (e) => !!(e && e.authCancelled),
      kcFetchWithSafetyAck: (url, init) => {
        calls.push({ url, headers: init.headers });
        const r = queue.shift();
        if (r === 'authcancel') return Promise.reject({ authCancelled: true });
        if (r === 'netfail') return Promise.reject(new Error('net'));
        return Promise.resolve(fakeResponse(r.status, r.text));
      },
    },
  });
  els.restoreFile.files = o.noFile ? [] : [{ body: '{"x":1}' }];
  els.kcModeMirror.checked = !!o.mirror;
  els.restoreBtn.fire('click');
  await flush();
  return { status: els.restoreStatus.textContent, calls, confirms };
}

(async () => {
  let s = await scenario({ noFile: true, responses: [] });
  ok(s.status === 'Choose a backup file first.' && s.calls.length === 0, 'no file: prompt shown, nothing sent');

  s = await scenario({ responses: [{ status: 409, text: 'profile is running' }] });
  ok(s.status === 'Restore refused: profile is running' && s.calls.length === 1,
    'dry-run refusal (409) shows the server text and never makes the real POST (got "' + s.status + '")');
  ok(s.calls[0].headers['X-Kiln-Config-Dry-Run'] === '1', 'first POST is the dry run');

  s = await scenario({ confirm: false, responses: [{ status: 200, text: 'delete "A"\n' }] });
  ok(s.status === 'Restore cancelled.' && s.calls.length === 1, 'declining the confirm cancels and sends no real POST');

  s = await scenario({ mirror: true, responses: [{ status: 200, text: 'delete "A"\nkeep active "B" (active slot is never deleted)\ndelete "C"\n' }, { status: 200, text: 'ok' }] });
  ok(s.status === 'Restore complete.', 'success: 200 -> "Restore complete."');
  ok(s.calls.length === 2 && s.calls[1].headers['X-Kiln-Config-Ack-Delete'] === '2'
    && s.calls[1].headers['X-Kiln-Config-Mode'] === 'mirror' && !s.calls[1].headers['X-Kiln-Config-Dry-Run'],
    'real POST carries Ack-Delete=2 (keep-active not counted), mode mirror, no dry-run header');
  ok(/delete "A"/.test(s.confirms[0]) && /Restore mode: mirror/.test(s.confirms[0]), 'confirm dialog lists the deletions and the mode');

  s = await scenario({ responses: [{ status: 200, text: '' }, { status: 200, text: 'ok' }] });
  ok(!('X-Kiln-Config-Ack-Delete' in s.calls[1].headers), 'no deletions: Ack-Delete header omitted');
  ok(/No kiln configuration slots are created/.test(s.confirms[0]), 'empty plan: dialog says nothing changes in kiln configs');

  for (const code of [400, 403, 409, 413, 428]) {
    s = await scenario({ responses: [{ status: 200, text: '' }, { status: code, text: 'reason-' + code }] });
    ok(s.status === 'Restore refused: reason-' + code, 'real POST ' + code + ' shows the server reason, never "complete"');
  }

  // A 500 is a partial write per backup_import: it must say so, never "refused" (D3).
  s = await scenario({ responses: [{ status: 200, text: '' }, { status: 500, text: 'partial write: zones committed' }] });
  ok(!/complete/i.test(s.status) && /partial write: zones committed/.test(s.status), '500 partial write: no success claim, server text shown');
  ok(/Restore failed partway -- some settings may have changed/.test(s.status) && !/refused/i.test(s.status),
    '500 partial write says it failed partway and settings may have changed, never "refused"');
  s = await scenario({ responses: [{ status: 200, text: '' }, { status: 409, text: 'busy now' }] });
  ok(/Restore refused: busy now/.test(s.status), '409 on the real POST is still a plain refusal');

  s = await scenario({ responses: ['authcancel'] });
  ok(s.status === 'Sign-in cancelled -- nothing was restored.', 'auth cancel during dry run -> quiet note, not stale "Checking..."');
  s = await scenario({ responses: [{ status: 200, text: '' }, 'authcancel'] });
  ok(s.status === 'Sign-in cancelled -- nothing was restored.', 'auth cancel during real POST -> quiet note, not stale "Uploading..."');
  s = await scenario({ responses: ['netfail'] });
  ok(/Could not check the restore plan/.test(s.status), 'network failure on dry run says the plan could not be checked');
  s = await scenario({ responses: [{ status: 200, text: '' }, 'netfail'] });
  ok(/Upload failed/.test(s.status), 'network failure on real POST says upload failed');

  console.log(failed ? failed + ' FAILED' : 'all passed');
  process.exit(failed ? 1 : 0);
})();
