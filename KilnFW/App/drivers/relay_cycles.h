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

/* Loads the persisted counts. Safe to call more than once; a missing or
 * wrong-sized blob starts everything at zero rather than failing (same
 * load-tolerant convention as zones_http.c/wifi_prov.c). */
esp_err_t relay_cycles_init(void);

/* Adds `cycles` transitions to every relay named in `relay_mask` (bit N-1 =
 * relay N, the same convention zone_cfg_t.relay_mask uses). A zone's relays
 * switch together as one group, so they all take the same count. */
void relay_cycles_add(uint8_t relay_mask, uint32_t cycles);

/* Copies the current counts out. out must hold KILN_IO_RELAY_COUNT entries. */
void relay_cycles_get(uint32_t *out);

/* Writes to NVS if anything changed and the interval has elapsed. Cheap to
 * call every control tick. */
void relay_cycles_maybe_persist(void);

/* Writes now if anything changed, ignoring the interval. */
esp_err_t relay_cycles_flush(void);

#ifdef __cplusplus
}
#endif

#endif // RELAY_CYCLES_H
