// snapshots.h -- shared producer/consumer snapshot structs, exactly as
// specified in docs/ARCHITECTURE.md section 6 ("Data flow and snapshots").
// Each producer task publishes a complete, self-consistent, timestamped
// snapshot; consumers take the newest whole snapshot, never a partial read.
//
// This file is shared across independently-developed tasks (thermo_task,
// current_task, ...). If you are adding a struct here and another one
// already exists, ADD to this file rather than replacing it, and re-read it
// immediately before editing in case another in-flight change landed first.
#ifndef SAFTYFW_SNAPSHOTS_H
#define SAFTYFW_SNAPSHOTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// thermo_snapshot_t -- thermo_task / max31856.c (docs/ARCHITECTURE.md
// section 6, docs/THERMOCOUPLE.md). valid == false implies tc_c and cj_c are
// NaN (never 0, never a cached reading -- the MAX31856 driver's "failure
// honesty" discipline, firmware/KilnFW/docs/MAX31856.md). While valid ==
// true, tc_c and/or cj_c can still independently be NaN when the part's own
// SR fault bits say that specific half of the reading is meaningless (e.g.
// OPEN leaves tc_c NaN while cj_c, from the same successful transaction, is
// still good) -- see max31856.c's MAX31856_TC_INVALIDATING_FAULTS/CJRANGE
// handling. fault_bits mirrors the MAX31856 SR register layout that
// safety_guards.h's SAFETY_THERMO_FAULT_* macros and max31856.h's
// MAX31856_FAULT_* macros both already document -- this struct just carries
// the raw SR byte, it does not redefine the bits a third time.
typedef struct {
    uint32_t timestamp_ms;
    bool     valid;
    float    tc_c, cj_c;      /* NaN when !valid */
    uint8_t  fault_bits;      /* THERMO_FAULT_* */
    bool     spi_failed;
} thermo_snapshot_t;

// current_snapshot_t -- current_task / adc_owner (docs/ARCHITECTURE.md
// section 6, docs/CURRENT_SENSE.md). `amps[n]` is the UNFILTERED,
// 16x-oversample-averaged reading a future guard must consume (S3/S4/S9,
// Phase 7) -- never the slow tau=0.5s reporting filter from
// docs/CURRENT_SENSE.md section 4. The filtered, reporting-only quantities
// (`i_conducting_a`, `conduction_fraction`, `p_avg_w`, docs/CURRENT_SENSE.md
// section 3b) live in current_sense_power_t (src/current_sense.h)
// deliberately OUTSIDE this struct, so nothing that reads current_snapshot_t
// can accidentally end up consuming a delayed value.
typedef struct {
    uint32_t timestamp_ms;
    float    amps[3];
    bool     clipped[3];
    bool     calibrated;
} current_snapshot_t;

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_SNAPSHOTS_H
