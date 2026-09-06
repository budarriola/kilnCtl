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

#ifdef __cplusplus
extern "C" {
#endif

/* At most one NVS write per this interval, per the flash-wear note above. */
#define RELAY_CYCLES_PERSIST_INTERVAL_S 600

/* A fifth counted slot, alongside the four heater relays (index 0..3 =
 * Relay1..Relay4), for the safety relay K4. RELAY_LIFE_BUDGET_PLAN.md step 1:
 * the ESP observes K4's state on every safety-status frame and can count its
 * own transitions even though K4 itself is driven by the Pico (relay_owner.c)
 * -- see relay_cycles_note_safety_edge()'s comment. The edge-counting call
 * site (the safety-link status consumer) is a LATER step; this step only
 * adds the slot, the increment API, and persistence for it. */
#define RELAY_CYCLES_SAFETY_INDEX KILN_IO_RELAY_COUNT
#define RELAY_CYCLES_COUNT (KILN_IO_RELAY_COUNT + 1u)

/* Relay type, used to pick a rated contact-life budget
 * (RELAY_LIFE_BUDGET_PLAN.md "Design" section). Persisted per relay so a
 * budget survives reboot before the zones/safety config steps that will
 * actually set it (relay_cycles_set_type()) land. */
typedef enum {
    RELAY_TYPE_SSR = 0,       /* no rated-life budget: never shown as a percent */
    RELAY_TYPE_CONTACTOR = 1,
    RELAY_TYPE_MERCURY = 2,
} relay_type_t;

/* Table values are industry-typical placeholders, not datasheet numbers for
 * a specific part -- RELAY_LIFE_BUDGET_PLAN.md "What exists" is explicit that
 * no contactor/mercury datasheet is in this tree. `rated_override` (set via
 * relay_cycles_set_type()) exists so a real number can be typed in per relay
 * without touching this table. */
#define RELAY_RATED_LIFE_CONTACTOR 100000u
#define RELAY_RATED_LIFE_MERCURY   1000000u

/* Budget tiers, per RELAY_LIFE_BUDGET_PLAN.md's 80%/90% thresholds. Computed
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
 * is a later RELAY_LIFE_BUDGET_PLAN.md step; this is just the increment API
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
 * the caller is told the write did not land yet). RELAY_LIFE_BUDGET_PLAN.md
 * step 5's LCD two-tap reset and step 4's web `POST /api/relay_cycles/reset`
 * both call this one function -- keep the signature exactly this simple
 * (a single unsigned relay index, a bool result) so neither caller has to
 * special-case the other's needs. MUST be called from a task with an
 * internal-SRAM stack -- see persist_locked()'s PSRAM guard comment; an LVGL
 * callback on this board's PSRAM-backed task stack (DRAM_PSRAM_PLAN.md
 * section 7.2) cannot call this directly and must dispatch through the
 * flash worker the same way uart_bridge_ext.c's flash-safe worker pattern
 * does. */
bool relay_cycles_reset(unsigned relay);

/* Writes to NVS if anything changed and the interval has elapsed. Cheap to
 * call every control tick. */
void relay_cycles_maybe_persist(void);

/* Writes now if anything changed, ignoring the interval. */
esp_err_t relay_cycles_flush(void);

#ifdef __cplusplus
}
#endif

#endif // RELAY_CYCLES_H
