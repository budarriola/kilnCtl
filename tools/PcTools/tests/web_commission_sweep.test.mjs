// Unit test for scripts/_web_commission_sweep.mjs. Run: node this-file
import { mkdtempSync, mkdirSync, writeFileSync, utimesSync, existsSync, rmSync } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { sweepStaleProfileDirs } from '../scripts/_web_commission_sweep.mjs';

let failed = 0;
const ok = (c, l) => { console.log(`${c ? 'OK:  ' : 'FAIL:'} ${l}`); if (!c) failed++; };
const tmp = mkdtempSync(path.join(os.tmpdir(), 'sweeptest-'));
const old = new Date(Date.now() - 3 * 3600 * 1000);
const mk = (name, innerFresh) => {
  const d = path.join(tmp, name);
  mkdirSync(path.join(d, 'Default'), { recursive: true });
  const f = path.join(d, 'Default', 'x');
  writeFileSync(f, 'x');
  if (!innerFresh) utimesSync(f, old, old);
  utimesSync(path.join(d, 'Default'), old, old);
  utimesSync(d, old, old);
  return d;
};
const stale = mk('kc-web-commission-stale', false);
const live = mk('kc-web-commission-live', true);   // old top-level, fresh inner file
const other = mk('unrelated', false);
const removed = sweepStaleProfileDirs({ tmp });
ok(!existsSync(stale), 'stale profile removed');
ok(existsSync(live), 'profile with a fresh inner file kept (top-level mtime old)');
ok(existsSync(other), 'non-matching dir untouched');
ok(removed.length === 1, 'exactly one removal reported');

// Deadline: an already-expired budget must remove nothing.
const stale2 = mk('kc-web-commission-stale2', false);
const r2 = sweepStaleProfileDirs({ tmp, budgetMs: -1 });
ok(r2.length === 0 && existsSync(stale2), 'expired budget does no work');
rmSync(tmp, { recursive: true, force: true });
console.log(failed ? `${failed} failed` : 'all assertions passed');
process.exit(failed ? 1 : 0);
