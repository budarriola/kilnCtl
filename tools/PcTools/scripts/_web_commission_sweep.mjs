// Stale kc-web-commission-* profile sweep, split out so it can be unit-tested
// (the driver itself runs main() on import).
import { readdirSync, statSync, rmSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import os from 'node:os';

// Newest mtime anywhere inside dir (bounded by deadline). A live driver's
// Chrome keeps writing inside its profile, while the top-level mtime can sit
// still for hours. Returns null if the deadline hit before the walk finished
// (caller must treat that as "unknown, keep").
export function newestMtimeMs(dir, deadline) {
  let newest = statSync(dir).mtimeMs;
  const stack = [dir];
  while (stack.length) {
    if (Date.now() > deadline) return null;
    const d = stack.pop();
    let ents;
    try { ents = readdirSync(d, { withFileTypes: true }); } catch { continue; }
    for (const e of ents) {
      const p = path.join(d, e.name);
      try {
        const m = statSync(p).mtimeMs;
        if (m > newest) newest = m;
        if (e.isDirectory()) stack.push(p);
      } catch { /* vanished or locked: ignore */ }
    }
  }
  return newest;
}

function grantProfileDirAcl(p, timeoutMs) {
  if (process.platform !== 'win32' || timeoutMs < 100) return;
  try { spawnSync('icacls', [p, '/grant', `${os.userInfo().username}:(OI)(CI)F`, '/T', '/C', '/Q'], { stdio: 'ignore', timeout: timeoutMs }); } catch { /* best effort */ }
}

// Best-effort; never runs a new step after `budgetMs` has elapsed, and each
// icacls gets only the time left (capped at 1 s).
export function sweepStaleProfileDirs({ tmp = os.tmpdir(), budgetMs = 2000, maxAgeMs = 3600 * 1000, now = Date.now } = {}) {
  const deadline = now() + budgetMs;
  const removed = [];
  try {
    for (const name of readdirSync(tmp)) {
      if (now() > deadline) break;
      if (!name.startsWith('kc-web-commission-')) continue;
      const p = path.join(tmp, name);
      try {
        const newest = newestMtimeMs(p, deadline);
        if (newest === null || now() - newest < maxAgeMs) continue;
        const left = deadline - now();
        if (left <= 0) break;
        grantProfileDirAcl(p, Math.min(1000, left));
        if (now() > deadline) break;
        rmSync(p, { recursive: true, force: true, maxRetries: 2, retryDelay: 50 });
        removed.push(name);
      } catch { /* best effort */ }
    }
  } catch { /* best effort */ }
  return removed;
}
