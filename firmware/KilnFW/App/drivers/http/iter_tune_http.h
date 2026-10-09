// HTTP surface for the iter_tune persistence store (docs/
// ITER_TUNE_REDESIGN.md sec 8 row 7). Two ROUTE_TIER_ADMIN routes:
//   GET  /api/iter_tune/status              -- per-zone persisted state
//   POST /api/iter_tune/restore_commissioned -- apply the last-anchored (or
//        baseline, if never anchored) gains for one zone and persist that
//        as the new resting state.
// Still proposes nothing on hardware: this file never calls
// iter_tune_propose_perturbation()/iter_tune_process_comparison() -- see
// tools/PcTools/scripts/iter_tune_write_surface_check.py's allow-list.
#ifndef ITER_TUNE_HTTP_H
#define ITER_TUNE_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t iter_tune_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // ITER_TUNE_HTTP_H
