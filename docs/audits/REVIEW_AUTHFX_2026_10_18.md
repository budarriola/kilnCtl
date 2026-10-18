# Review: auth fix F2-02 self-heal (2026-10-18)

Scope: 57abd7d4f (`storage_reset_evaluate_impl` in
`firmware/KilnFW/App/drivers/persist/web_auth_store.c`, host test in
`test_web_auth_store.c`) and docs commit c8454aa9e (F2-02/F2-05 fixed,
F2-04/FS-03 accepted, FS-05 closed). Rule checked: owner FS-02, wiped auth
storage with surviving Wi-Fi = dashboards only until an admin is set up again.

Verdict: no HIGH. HIGH 0, MED 1, LOW 2, INFO 2. No fix landed.

## AF-01 MED: self-heal keys on "web blob present", not "admin password configured"
The new branch calls `storage_reset_resolve()` when
`load_blob(WEB_AUTH_KEY_WEB) != HAL_NOT_FOUND`. The physical reset gesture
(`web_auth_store_clear_for_physical_reset_impl`) writes exactly such a blob with
the admin record `configured=false`, and it can run while latched. The next boot
then writes an enabled policy and the sentinel and clears the latch, the LCD
"login storage was reset" banner and the readiness auth item, with no admin
credential. Not fail-open compared with the latched state: the enabled policy with
an unconfigured admin still routes to the admin bootstrap, the same
exposure as before. But the board loses its FS-02 signal while it still needs an
admin to be set up. The test makes this worse: it plants a zeroed blob (no role
configured), so it passes on the very state that should *not* heal. A check of
`load_password(ADMIN) == OK && rec.configured` would fail it.
Fix: heal only when the admin record loads OK and is configured; change the
test to plant a configured admin record, and add a case where an unconfigured
(gesture) blob stays latched.

## AF-02 LOW: corrupt or wrong-size web blob also heals
`HAL_INVALID_SIZE`/CRC-bad blobs are `!= HAL_NOT_FOUND`, so they also trigger
resolve. Result: enabled policy and an admin that reads UNREADABLE (counted as
configured), so there is no bootstrap and no login until the physical gesture
rewrites the blob. Fails closed, but the latch and banner are cleared and the
admin is locked out with no hint. Covered by the AF-01 fix (require load OK).

## AF-03 LOW: F2-05 says F2-02 coverage is negtested; no record
c8454aa9e marks F2-05 fixed, "covered and negtested", including 57abd7d4f. Neither
commit carries a negtest log or mutation list for the new branch. Any mutant that
drops the `configured` check would survive (see AF-01). Re-run
`tools/negtest.ps1` on the corrected test.

## AF-04 INFO: ordering and power-cut windows are sound
`storage_reset_resolve` writes the policy first, then the nvs sentinel, then the
mirror. A cut after the policy write: next boot finds the policy (pre-FS-02
path), writes the sentinels, healthy and auth enabled. A cut before it: the blob is
still there and the boot retries. A `set_policy` failure stays latched and
load_policy stays UNREADABLE (fail closed). No window reads as auth off.

## AF-05 INFO: accept reasons
F2-04 is sound: a format that erases `nvs` and `wifi_nvs` also drops the Wi-Fi
credentials, so the FS-02 condition (Wi-Fi survived) does not hold and the board
is factory-fresh. FS-05 closure is sound: the loaders treat the format window as
untrustworthy. FS-03 cites an "owner decision" with no date or memory/decision
reference; the reasoning (a valid superblock always blocks, planting needs
physical access) holds, but the citation should be filled in.
