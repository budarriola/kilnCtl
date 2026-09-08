/* commissioning_shared.js -- extracted 2026-09-08 (SETUP_WIZARD_PLAN.md step
 * 8, "safety processor" step). Owner decision: embed the wizard's own
 * safety-processor step inline rather than linking out, but REUSE the
 * existing write path and its confirm_commit_landed() read-back rather than
 * writing a second implementation of confirm-and-verify. This file is the
 * "if you find yourself copying confirm-and-verify logic, extract and share
 * it instead" outcome: it holds the three pieces of client-side logic that
 * both safety_commissioning_page.html and setup_wizard_page.html's step 7
 * need identically --
 *
 *   kcCommissioningCheckBusy()          -- mirrors mcp_server_safety.py's
 *     _profile_or_autotune_running() refusal (a firing/autotune in progress
 *     is not a safe time to change safety-guard commissioning out from under
 *     it). NEVER rejects on a failed fetch -- the Pico's own ARMED refusal at
 *     COMMIT_CONFIG remains the real backstop either way.
 *
 *   kcCommissioningFindCriticalChanges(latestParams, criticalIds, bodyPairs)
 *     -- returns the subset of `bodyPairs` (an about-to-be-POSTed
 *     [{id,value}] array) whose id is in `criticalIds` AND whose value
 *     actually differs from what `latestParams` (the last GET's params[]
 *     array) last reported. Each entry carries old/new as display strings.
 *
 *   kcCommissioningCommitAndVerify(bodyPairs, criticalChanges, opts) --
 *     POSTs id=/value= pairs + commit=1 to /api/safety/commissioning,
 *     distinguishes a REJECTED commit (HTTP 200, body {ok:false,...} --
 *     res.ok alone is never enough) from a real success, and for any
 *     critical change does this page's OWN second, independent read-back
 *     (re-fetching GET /api/safety/commissioning fresh) on top of the
 *     server's own confirm_commit_landed() -- failing LOUDLY, naming the
 *     field, if the two ever disagree. `opts.confirmFn` defaults to
 *     window.confirm; `opts.namePrefix` customises the confirm dialog text.
 *     Resolves to {ok, message} -- never throws for an ordinary rejection.
 *
 * Neither page owns a second copy of this logic any more -- both `<script
 * defer src="/commissioning_shared.js">`. Domain-specific checks that are
 * NOT generic (checkTcMaxContradiction's tc_type/abs_max_temp_c
 * relationship, the guided flow's own field set) stay local to each page,
 * since sharing those would mean generating one from the other across two
 * different UI shapes -- out of proportion to the risk, same call this
 * codebase already made for TC_MAX_C_BY_TYPE's two sources of truth.
 */
(function (global) {
  'use strict';

  function checkBusy() {
    return Promise.all([
      fetch('/api/profile_exec').then(function (r) { return r.json(); }).catch(function () { return null; }),
      fetch('/api/autotune').then(function (r) { return r.json(); }).catch(function () { return null; }),
    ]).then(function (results) {
      var exec = results[0], at = results[1];
      if (exec && exec.state === 'running') {
        return 'a profile is currently firing (state: running)';
      }
      if (exec && exec.state === 'paused') {
        return 'a profile is currently paused mid-firing (state: paused)';
      }
      if (at && at.state && ['idle', 'done', 'aborted'].indexOf(at.state) === -1) {
        return 'an autotune run is currently in progress (state: ' + at.state + ')';
      }
      return null;
    });
  }

  function findCriticalChanges(latestParams, criticalIds, bodyPairs, enumLabelFor) {
    var out = [];
    var paramsById = {};
    (latestParams || []).forEach(function (p) { paramsById[p.id] = p; });
    (criticalIds || []).forEach(function (id) {
      var pair = (bodyPairs || []).filter(function (b) { return b.id === id; })[0];
      if (!pair) return;
      var oldP = paramsById[id];
      var oldDisplay = (oldP && oldP.set) ? String(oldP.value) : '(unset)';
      var newDisplay = String(pair.value);
      if (typeof enumLabelFor === 'function') {
        oldDisplay = enumLabelFor(id, oldDisplay) || oldDisplay;
        newDisplay = enumLabelFor(id, newDisplay) || newDisplay;
      }
      if (oldDisplay === newDisplay) return;
      out.push({ id: id, name: pair.name || ('id ' + id), oldDisplay: oldDisplay, newDisplay: newDisplay });
    });
    return out;
  }

  function commitAndVerify(bodyPairs, criticalChanges, opts) {
    opts = opts || {};
    var confirmFn = opts.confirmFn || global.confirm;
    var setMsg = opts.setMsg || function () {};

    function doPost() {
      var body = bodyPairs.map(function (p) {
        return 'id=' + encodeURIComponent(p.id) + '&value=' + encodeURIComponent(p.value);
      });
      body.push('commit=1');
      setMsg('Staging and committing…');
      return fetch('/api/safety/commissioning', {
        method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body.join('&'),
      }).then(function (r) {
        return r.json().catch(function () { return {}; }).then(function (j) { return { httpOk: r.ok, j: j }; });
      }).then(function (res) {
        // A REJECTED commit is HTTP 200 with {"ok":false,"reason":...} --
        // res.httpOk alone never distinguishes an ARMED refusal, a
        // RANGE/CONTRADICTION rejection, or a confirm_commit_landed()
        // read-back failure from a real success.
        if (!res.httpOk || !res.j || res.j.ok !== true) {
          var reason = (res.j && res.j.field ? res.j.field + ' -- ' : '') +
            ((res.j && (res.j.reason || res.j.error)) || 'commit failed');
          return { ok: false, message: 'Rejected: ' + reason, armed: /ARMED/i.test(reason) };
        }
        if (!criticalChanges || !criticalChanges.length) {
          return { ok: true, message: 'Committed and confirmed by read-back.' };
        }
        // The server's own confirm_commit_landed() already forced a live
        // read-back before reporting {"ok":true}. This is this page's OWN,
        // second, independent confirmation of that guarantee: re-fetch
        // fresh and check the exact fields this save changed read back as
        // what was sent, failing loudly -- naming the field -- on mismatch.
        setMsg('Committed. Verifying read-back…');
        return fetch('/api/safety/commissioning').then(function (r) { return r.json(); }).then(function (fresh) {
          var freshById = {};
          (fresh.params || []).forEach(function (p) { freshById[p.id] = p; });
          var bad = criticalChanges.filter(function (c) {
            var p = freshById[c.id];
            var pair = bodyPairs.filter(function (b) { return b.id === c.id; })[0];
            return !p || !p.set || String(p.value) !== String(pair.value);
          });
          if (bad.length) {
            return {
              ok: false,
              message: 'FAILED: the safety processor reported success, but a fresh read of ' +
                bad.map(function (b) { return b.name; }).join(', ') +
                ' does NOT match what was just written -- treat the write as NOT confirmed. Reload ' +
                'and re-check before firing.',
            };
          }
          return {
            ok: true,
            message: 'Committed and confirmed by read-back: ' +
              criticalChanges.map(function (c) { return c.name + '=' + c.newDisplay; }).join(', ') +
              ' (this cannot independently confirm the MAX31856 chip itself accepted a changed ' +
              'thermocouple type -- only that the safety processor\'s config record now holds it; ' +
              'max31856_tc_type_verified() is not on the wire).',
          };
        }).catch(function () {
          return { ok: false, message: 'Committed, but this page\'s own re-verification fetch failed -- ' +
            'reload to confirm before firing.' };
        });
      });
    }

    if (!criticalChanges || !criticalChanges.length) {
      return doPost();
    }
    setMsg('Checking it is safe to write the safety processor\'s flash…');
    return checkBusy().then(function (busyReason) {
      if (busyReason) {
        return { ok: false, message: 'Refused: ' + busyReason + ' -- changing safety-processor ' +
          'commissioning mid-run is not safe. Stop it first, then retry.' };
      }
      var lines = criticalChanges.map(function (c) {
        return c.name + ': ' + c.oldDisplay + ' -> ' + c.newDisplay;
      });
      var confirmed = confirmFn(
        (opts.confirmPrefix || 'This WRITES THE SAFETY PROCESSOR\'S (RP2040) FLASH:') +
        '\n\n' + lines.join('\n') +
        '\n\nA wrong value here silently changes what the independent over-temperature protection ' +
        'relies on. Continue?'
      );
      if (!confirmed) {
        return { ok: false, message: 'Cancelled -- nothing was written.' };
      }
      return doPost();
    });
  }

  global.kcCommissioningCheckBusy = checkBusy;
  global.kcCommissioningFindCriticalChanges = findCriticalChanges;
  global.kcCommissioningCommitAndVerify = commitAndVerify;
})(typeof window !== 'undefined' ? window : this);
