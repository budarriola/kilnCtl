// relay_cycles -- lifetime contact-cycle accounting per relay, persisted.
// TODO.md 6A.1's "contact-cycle accounting" bullet.
//
// Why this exists: the EE2-12NUH relays on this board are electromechanical,
// with a contact life budget on the order of 10^5 operations
// (docs/HARDWARE.md). A 60 s time-proportioning window can spend that in a
// few hundred hours of firing. "A kiln controller that silently eats a
// relay's contact life is a controller that fails mid-firing at cone
// temperature" -- so the count is kept, persisted, and shown to the
// operator. It also gives a data-backed answer, over time, to TODO.md 6A.0's
// open question about how long the duty window should be.
//
// heater_output.c already counts transitions per zone in RAM; this module is
// the part that survives a reboot and maps a zone's count onto the individual
// relays in that zone's mask.
//
// Flash wear: the counts are NOT written on every transition (that would
// trade relay life for flash life). Callers accumulate freely and call
// relay_cycles_maybe_persist() as often as they like; it writes at most once
// per RELAY_CYCLES_PERSIST_INTERVAL_S, and only when something changed.
// relay_cycles_flush() forces a write at a natural end point (a firing
// stopping, an autotune finishing).
#ifndef RELAY_CYCLES_H
#define RELAY_CYCLES_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "kiln_io.h"

/* cfg_fs relative path of this item's dual-write mirror (NVS side lives in kiln_nvs). */
#define RELAY_CYCLES_FILE_PATH "relay_cycles.dat"

#ifdef __cplusplus
extern "C" {
#endif

/* At most one NVS write per this interval, per the flash-wear note above. */
#define RELAY_CYCLES_PERSIST_INTERVAL_S 600

/* A fifth counted slot, alongside the four heater relays (index 0..3 =
 * Relay1..Relay4), for the safety relay K4. RELAY_LIFE_BUDGET.md:
 * the ESP observes K4's state on every safety-status frame and can count its
 * own transitions even though K4 itself is driven by the Pico (relay_owner.c)
 * -- see relay_cycles_note_safety_edge()'s comment. The edge-counting call
 * site (the safety-link status consumer) is a LATER step; this step only
 * adds the slot, the increment API, and persistence for it. */
#define RELAY_CYCLES_SAFETY_INDEX KILN_IO_RELAY_COUNT
#define RELAY_CYCLES_COUNT (KILN_IO_RELAY_COUNT + 1u)

/* Relay type, used to pick a rated contact-life budget
 * (RELAY_LIFE_BUDGET.md "Design" section). Persisted per relay so a
 * budget survives reboot before the zones/safety config steps that will
 * actually set it (relay_cycles_set_type()) land. */
typedef enum {
    RELAY_TYPE_SSR = 0,       /* no rated-life budget: never shown as a percent */
    RELAY_TYPE_CONTACTOR = 1,
    RELAY_TYPE_MERCURY = 2,
} relay_type_t;

/* Table values are industry-typical placeholders, not datasheet numbers for
 * a specific part -- RELAY_LIFE_BUDGET.md "What exists" is explicit that
 * no contactor/mercury datasheet is in this tree. `rated_override` (set via
 * relay_cycles_set_type()) exists so a real number can be typed in per relay
 * without touching this table. */
#define RELAY_RATED_LIFE_CONTACTOR 100000u
#define RELAY_RATED_LIFE_MERCURY   1000000u

/* Budget tiers, per RELAY_LIFE_BUDGET.md's 80%/90% thresholds. Computed
 * on read in relay_cycles_budget(); never stored. */
typedef enum {
    RELAY_BUDGET_TIER_NONE = 0,   /* no budget (ssr), or below 80% */
    RELAY_BUDGET_TIER_WARN = 1,   /* >= 80%, < 90% */
    RELAY_BUDGET_TIER_ERROR = 2,  /* >= 90% */
} relay_budget_tier_t;

typedef struct {
    bool     has_budget;   /* false for ssr -- percent/tier below are meaningless */
    uint32_t cycles;
    uint32_t rated;        /* the value actually used: override if nonzero, else the table */
    float    percent;      /* 0..100+, saturates past 100 rather than wrapping */
    relay_budget_tier_t tier;
} relay_cycles_budget_t;

/* Loads the persisted counts. Safe to call more than once; a missing or
 * wrong-sized blob starts everything at zero rather than failing (same
 * load-tolerant convention as zones_http.c/wifi_prov.c). */
esp_err_t relay_cycles_init(void);

/* Adds `cycles` transitions to every relay named in `relay_mask` (bit N-1 =
 * relay N, the same convention zone_cfg_t.relay_mask uses). A zone's relays
 * switch together as one group, so they all take the same count. Does not
 * touch RELAY_CYCLES_SAFETY_INDEX -- that slot is only ever touched by
 * relay_cycles_note_safety_edge(). */
void relay_cycles_add(uint8_t relay_mask, uint32_t cycles);

/* Adds one transition to the safety relay (K4) slot. The edge-counting call
 * site (the safety-link status consumer noticing an observed K4 transition)
 * is covered elsewhere -- see docs/RELAY_LIFE_BUDGET.md; this is just the increment API
 * and the slot it writes. */
void relay_cycles_note_safety_edge(void);

/* Copies the current counts out. out must hold KILN_IO_RELAY_COUNT entries
 * -- unchanged from before this slot existed, since dashboard_http.c's only
 * caller passes a KILN_IO_RELAY_COUNT-sized buffer and this step leaves
 * dashboard/HTTP files untouched. Does not include the safety relay slot;
 * see relay_cycles_get_all() for that. */
void relay_cycles_get(uint32_t *out);

/* Copies ALL current counts out, including the safety relay slot. out must
 * hold RELAY_CYCLES_COUNT entries (index RELAY_CYCLES_SAFETY_INDEX is the
 * safety relay). For new callers -- a later step wires this into the
 * dashboard/LCD indication. */
void relay_cycles_get_all(uint32_t *out);

/* Sets relay `relay`'s type (RAM only until the next persist) and an optional
 * rated-life override (0 = use the type's table value). `relay` is
 * 0..RELAY_CYCLES_COUNT-1 (kiln_io.h's 0-based relay indices plus
 * RELAY_CYCLES_SAFETY_INDEX). Later steps (zones/safety config) will call
 * this once their own per-relay type field exists; until then every relay
 * defaults to RELAY_TYPE_SSR (index < KILN_IO_RELAY_COUNT) as-is. */
void relay_cycles_set_type(uint8_t relay, relay_type_t type, uint32_t rated_override);

/* Reads relay `relay`'s type and rated-life override as currently held (RAM,
 * loaded from the persisted blob at init). */
void relay_cycles_get_type(uint8_t relay, relay_type_t *type, uint32_t *rated_override);

/* Computes relay `relay`'s budget state from its current count, type, and
 * override. Never fails: an out-of-range `relay` or a NULL `out` is a no-op
 * (SSR-typed relays, and any relay past RELAY_CYCLES_COUNT, come back with
 * has_budget == false). */
void relay_cycles_budget(uint8_t relay, relay_cycles_budget_t *out);

/* The maximum budget tier over every relay that has a budget -- the value
 * the LCD/web indication (a later step) will actually gate on. */
relay_budget_tier_t relay_cycles_max_budget_tier(void);

/* Zeroes relay `relay`'s cycle count (type/rated_override are left alone --
 * a reset is "this contact was replaced", not "forget what kind it is") and
 * persists immediately through the same guarded path relay_cycles_flush()
 * uses. `relay` is 0..RELAY_CYCLES_COUNT-1, same indexing as
 * relay_cycles_get_all()/relay_cycles_set_type(). Returns false for an
 * out-of-range relay or if the persist write itself fails (the count is
 * still zeroed in RAM in that case -- same "keep going, retry later"
 * contract relay_cycles_maybe_persist() uses elsewhere in this module -- but
 * the caller is told the write did not land yet). See docs/RELAY_LIFE_BUDGET.md:
 * the LCD two-tap reset and the web `POST /api/relay_cycles/reset` route
 * both call this one function -- keep the signature exactly this simple
 * (a single unsigned relay index, a bool result) so neither caller has to
 * special-case the other's needs. MUST be called from a task with an
 * internal-SRAM stack -- see persist_locked()'s PSRAM guard comment; an LVGL
 * callback on this board's PSRAM-backed task stack (DRAM_PSRAM_PLAN.md
 * section 7.2) cannot call this directly and must dispatch through the
 * flash worker the same way uart_bridge_ext.c's flash-safe worker pattern
 * does. */
bool relay_cycles_reset(unsigned relay);

/* Bounded-wait sibling of relay_cycles_reset() above -- for a caller (the
 * LCD diagnostics page's Relay Life Reset control, lvgl_task) that must not
 * block indefinitely behind some OTHER caller's long flash-worker job, same
 * hazard and same fix shape as crash_report_acknowledge_timeout()
 * (docs/audits/review_crash_gate_medium_fixes_aa2c484d_2026-09-15.md).
 * `timeout_ms` bounds only ACQUIRING the flash worker (see uart_bridge_ext_
 * run_on_flash_worker_timeout()'s doc comment) -- once acquired, the persist
 * itself runs to completion same as the unbounded version. `*out_timed_out`
 * (if non-NULL) is set true only when the worker could not be acquired in
 * time, i.e. nothing was read or written -- distinct from every other
 * false-returning outcome (a dispatched write that failed), which the
 * caller should treat the same way relay_cycles_reset()'s failure is
 * treated today. Same relay-index/return-value contract as relay_cycles_
 * reset() otherwise, and the same internal-SRAM-stack task requirement. */
bool relay_cycles_reset_timeout(unsigned relay, uint32_t timeout_ms, bool *out_timed_out);

/* Per-relay outcome of a relay_cycles_restore_all() call, filled in
 * regardless of the overall return value so a caller (diagnostics_http.c's
 * restore handler) can name every affected relay in its response -- see
 * relay_cycles_restore_all()'s own comment for why this exists: a silent
 * downward move of a wear counter is exactly the failure class this project
 * has been bitten by repeatedly (CLAUDE.md's "reset one side of a pair" /
 * unchecked-success classes), so this struct makes "was anything clamped"
 * mechanically visible rather than something a caller has to notice on its
 * own by re-deriving it from the request. */
typedef struct {
    uint32_t requested; /* the value the caller asked to restore */
    uint32_t applied;   /* the value actually written -- equals `requested`
                          * unless `clamped` is true */
    bool     clamped;   /* true iff `requested` was below the board's live
                          * count and this relay's bit was not set in
                          * `allow_lower_mask`, so `applied` was raised back
                          * up to the live count instead of accepted verbatim */
} relay_cycles_restore_entry_t;

typedef struct {
    relay_cycles_restore_entry_t entries[RELAY_CYCLES_COUNT];
} relay_cycles_restore_result_t;

/* Restores all RELAY_CYCLES_COUNT counts from a backup (e.g.
 * /api/status.relay_life previously captured by full_board_backup.py),
 * validates every value against a sanity ceiling BEFORE writing anything
 * (all-or-nothing -- a single out-of-range value refuses the whole call so a
 * truncated/corrupt backup field cannot land a partial restore), and
 * persists immediately through the same flash-worker path
 * relay_cycles_reset() uses. Idempotent: calling this twice with the same
 * `counts` (and the same `allow_lower_mask`) produces the same on-disk blob
 * both times. types/rated_overrides are left untouched, same as
 * relay_cycles_reset()'s convention. MUST be called from a task with an
 * internal-SRAM stack, same constraint as every other write path in this
 * module.
 *
 * MONOTONIC GUARD (2026-09-20 backup/restore review finding): a relay's wear
 * count is safety-relevant, irreversible-in-spirit data -- understating it is
 * the error that gets a worn contactor treated as fresh rather than replaced
 * (RELAY_LIFE_BUDGET.md). Before this fix, only the upper sanity ceiling
 * below was enforced, so a stale/wrong archive could silently LOWER a live
 * count. Now, for every relay whose bit is NOT set in `allow_lower_mask`, a
 * requested value below the board's current live count is CLAMPED UP to the
 * live count rather than either being applied verbatim or refusing the whole
 * request -- the rest of the restore still proceeds, matching this project's
 * standing "make it visible, never silent" rule rather than adding a second,
 * more surprising all-or-nothing failure mode. `allow_lower_mask` is the
 * deliberate, explicit override for the one legitimate case (a physically
 * replaced relay restarting at/near zero): set the corresponding bit to
 * accept that relay's requested value exactly, even if it is below the live
 * count, still subject to the sanity ceiling below. Pass 0 for the default,
 * safe behaviour (never move any counter downward). `out_result`, if
 * non-NULL, is filled with every relay's requested/applied/clamped outcome
 * regardless of the overall return value, so a caller can report exactly
 * what happened; on a refused call (out-of-range value) `out_result` is left
 * untouched -- nothing was decided per-relay since nothing was written.
 * Returns false for a NULL `counts` pointer, an out-of-range count, or a
 * persist failure (in which case the in-RAM counts ARE updated but not yet
 * durable -- same "keep going, retry later" contract relay_cycles_reset()
 * documents). */
bool relay_cycles_restore_all(const uint32_t counts[RELAY_CYCLES_COUNT], uint8_t allow_lower_mask,
                               relay_cycles_restore_result_t *out_result);

/* Writes to NVS if anything changed and the interval has elapsed. Cheap to
 * call every control tick. */
void relay_cycles_maybe_persist(void);

/* Writes now if anything changed, ignoring the interval. */
esp_err_t relay_cycles_flush(void);

/* GET /api/cfgfs dual-write picture for this bridge -- 2026-09-08, moving
 * this item's reporting out of cfg_fs_status.c's stale "nvs_only" hardcoded
 * list (docs/FILESYSTEM_USER_DATA_PLAN.md's relay-cycles bridge, step 6,
 * landed in 762bb29e). Read-only: does NOT call pref_cfg_fs_resolve() or any
 * other function capable of a resync write, matching every sibling
 * *_get_dualwrite_status() (unit_pref.c etc.) -- a status GET must never
 * itself heal or mask a divergence. `diverged` uses
 * cfg_fs_status_item_diverged() (real content compare via memcmp of the
 * whole relay_cycles_blob_t, not a rev-only guess), same discipline every
 * other accessor here follows. All five output pointers accept NULL. */
void relay_cycles_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                        bool *diverged);

/* True iff relay_cycles_init()'s boot-time migrate-on-load write was
 * attempted before the flash-safe worker existed and the bounded wait
 * (flash_worker_wait.h) gave up -- see that write call site's comment in
 * relay_cycles.c. Surfaced into GET /api/cfgfs's dual_write.items[]
 * ("migration_deferred") so a dropped migration is one query away instead
 * of requiring a hardware flash to notice (2026-09-08, 3e226f28). */
bool relay_cycles_migration_worker_wait_deferred(void);

#ifdef __cplusplus
}
#endif

#endif // RELAY_CYCLES_H
