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
 * RETROFITTED 2026-09-08 (review 5d03f8c2): safety_commissioning_page.html
 * now consumes these three exports too (loads this file via
 * <script src="/commissioning_shared.js">, same as setup_wizard_page.html;
 * both are served from the one shared httpd handle -- see
 * wifi_provision_http.c's commissioning_shared_js_uri, registered once for
 * the whole server). Its local findCriticalChanges()/
 * checkFiringOrAutotuneRunning()/commit-and-read-back copy is gone. Where
 * the two implementations had drifted (the empty-criticalChanges success
 * message wrongly claimed "confirmed by read-back" here when the safety
 * page correctly said plain "Committed."; the confirm-dialog wording and the
 * MAX31856-caveat wording differed cosmetically), the ORIGINAL,
 * longer-standing safety-page behaviour won: the "Committed." bug is fixed
 * here (so setup_wizard_page.html's step 7 gets the same fix), and the
 * safety-specific wording is now this function's default with the wizard's
 * differently-scoped writes (abs_max_temp_c/ct_installed/ct_topology, not
 * only tc_type/tc_offset_c) overriding via opts.confirmPrefix as before.
 * check_no_duplicate_commissioning_impl.ps1 fails the build if a second
 * confirm-and-read-back implementation reappears in any served page.
 *
 * Domain-specific checks that are
 * NOT generic (checkTcMaxContradiction's tc_type/abs_max_temp_c
 * relationship, the guided flow's own field set) stay local to each page,
 * since sharing those would mean generating one from the other across two
 * different UI shapes -- out of proportion to the risk, same call this
 * codebase already made for TC_MAX_C_BY_TYPE's two sources of truth.
 *
 * EXTENDED 2026-09-08 (setup wizard confirmation drift, docs/audits/
 * setup_wizard_review_2026-09-08.md) with kcConfirmConsequentialChange(),
 * a fourth export alongside the three above -- NOT a fourth bespoke
 * implementation. Steps 4 and 6 of the wizard write ZONE config
 * (/api/zones, the ESP's own store) rather than the Pico's
 * /api/safety/commissioning, so kcCommissioningCommitAndVerify() itself
 * doesn't fit them directly: there is no analogous "independent read-back
 * of the Pico" step for an ESP-only write, because unlike a Pico commit,
 * submitZonesConfig() has never had its own read-back verification at
 * ANY of the four steps that already call it (2, 4, 5, 6) -- adding one
 * only for 4/6 now would be a new, narrower inconsistency, not a fix of
 * this one, and touching submitZonesConfig()'s contract is out of scope
 * for a confirmation-drift fix. What DOES generalise is the confirm
 * dialog + busy-refusal shape itself (checkBusy(), named old->new lines,
 * a confirm prompt, cancel means nothing is sent) -- that part is shared
 * here so steps 4 and 6 use the same wording/structure/busy-check as step
 * 7 rather than growing their own.
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
    // 2026-09-08 retrofit (5d03f8c2): safety_commissioning_page.html's
    // longer-standing wording differs from this module's own defaults in a
    // few places (the confirm dialog's closing sentence, the busy-refusal's
    // description of what is unsafe, the verified-success caveat about the
    // MAX31856 chip). Preferring the original everywhere would mean this
    // module speaking as if every caller only ever writes tc_type/tc_offset_c,
    // which is not true for setup_wizard_page.html's step 7 (writes
    // abs_max_temp_c/ct_installed/ct_topology too) -- so the ORIGINAL text
    // stays the default (this function's oldest, proven-on-hardware caller),
    // and callers with a narrower or differently-worded claim override via
    // opts instead of this module silently drifting from either.
    var confirmSuffix = opts.confirmSuffix ||
      '\n\nA wrong value here silently misreads temperature. Continue?';
    var busyAction = opts.busyAction ||
      'changing the safety thermocouple configuration mid-run is not safe';
    var verifyCaveat = opts.verifyCaveat ||
      ' (this could not independently confirm the MAX31856 chip itself accepted the type -- ' +
      'only that the safety processor\'s config record now holds it; see THERMOCOUPLE.md\'s ' +
      'CR1-verify note).';

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
          return { ok: false, posted: false, message: 'Rejected: ' + reason, armed: /ARMED/i.test(reason) };
        }
        if (!criticalChanges || !criticalChanges.length) {
          // No critical field was actually changed by this save, so no
          // independent read-back was needed or performed -- do not claim
          // one happened. (2026-09-08: this branch used to say "Committed
          // and confirmed by read-back." unconditionally, which was simply
          // false whenever criticalChanges was empty -- caught comparing
          // against safety_commissioning_page.html's longer-standing
          // behaviour, which only ever said "Committed." here.)
          return { ok: true, posted: true, message: 'Committed.' };
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
              posted: true,
              message: 'FAILED: the safety processor reported success, but a fresh read of ' +
                bad.map(function (b) { return b.name; }).join(', ') +
                ' does NOT match what was just written -- treat the write as NOT confirmed. Reload ' +
                'this page and re-check before firing.',
            };
          }
          return {
            ok: true,
            posted: true,
            message: 'Committed and confirmed by read-back: ' +
              criticalChanges.map(function (c) { return c.name + '=' + c.newDisplay; }).join(', ') +
              verifyCaveat,
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
        return { ok: false, posted: false, message: 'Refused: ' + busyReason + ' -- ' + busyAction +
          '. Stop it first, then retry.' };
      }
      var lines = criticalChanges.map(function (c) {
        return c.name + ': ' + c.oldDisplay + ' -> ' + c.newDisplay;
      });
      var confirmed = confirmFn(
        (opts.confirmPrefix ||
          'This WRITES THE SAFETY PROCESSOR\'S (RP2040) FLASH and changes how its independent ' +
          'over-temperature protection interprets its own thermocouple:') +
        '\n\n' + lines.join('\n') + confirmSuffix
      );
      if (!confirmed) {
        return { ok: false, posted: false, message: 'Cancelled -- nothing was written.' };
      }
      return doPost();
    });
  }

  // Generic confirm-before-write, shared by any step that changes a
  // consequence-bearing setting but does NOT go through the Pico commit
  // path above (see the file-header note for why this is a sibling, not
  // a copy). Resolves to true only if the operator confirmed (or there
  // was nothing to confirm); false for "busy, refused" or "cancelled" --
  // callers must treat false as "do not write, and put the UI back the
  // way it was," never merely "don't advance."
  function confirmConsequentialChange(lines, opts) {
    opts = opts || {};
    var confirmFn = opts.confirmFn || global.confirm;
    if (!lines || !lines.length) return Promise.resolve(true);

    function ask() {
      return confirmFn(
        (opts.confirmPrefix || 'Confirm this change:') + '\n\n' + lines.join('\n') +
        (opts.confirmSuffix || '')
      );
    }

    if (opts.skipBusyCheck) return Promise.resolve(ask());

    return checkBusy().then(function (busyReason) {
      if (busyReason) {
        if (typeof opts.onBusy === 'function') {
          opts.onBusy('Refused: ' + busyReason + ' -- changing this setting mid-run is not safe. ' +
            'Stop it first, then retry.');
        }
        return false;
      }
      return ask();
    });
  }

  global.kcCommissioningCheckBusy = checkBusy;
  global.kcCommissioningFindCriticalChanges = findCriticalChanges;
  global.kcCommissioningCommitAndVerify = commitAndVerify;
  global.kcConfirmConsequentialChange = confirmConsequentialChange;
})(typeof window !== 'undefined' ? window : this);
