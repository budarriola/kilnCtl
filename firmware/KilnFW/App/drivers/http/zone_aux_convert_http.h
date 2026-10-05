// zone_aux_convert_http -- installs the `move_zone_to_aux` action on POST /api/zones
// (docs/SPARE_RELAY_ONOFF_PLAN.md section 10). No URI route is added.
#ifndef ZONE_AUX_CONVERT_HTTP_H
#define ZONE_AUX_CONVERT_HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Boot wiring; called from aux_outputs_http_start() so it follows the aux store start. */
void zone_aux_convert_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // ZONE_AUX_CONVERT_HTTP_H
