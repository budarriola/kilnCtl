#ifndef SAFETY_STACK_MARGIN_HTTP_H
#define SAFETY_STACK_MARGIN_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "kilnlink/kilnlink_stack_margin.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure JSON builder, exposed for host testing (max-width test, sentinel/
 * unmeasured rendering, link-down rendering) -- see safety_stack_margin_
 * http.c's own doc comment for the exact shape and the units/floor
 * caveats it is required to carry. Returns bytes written (< out_cap), or
 * 0 if the buffer was too small to hold the (fixed-shape, 9-task) body. */
size_t safety_stack_margin_build_json(bool link_up, esp_err_t link_result,
                                       const kilnlink_stack_margin_t *m, char *out, size_t out_cap);

/* GET /api/saftyfw_stack_margin -- surfaces SaftyFW's nine live per-task
 * stack high-water marks (kilnlink_stack_margin.h /
 * safety_link_get_stack_margin(), KILNLINK_PROTOCOL_VERSION 13) the same
 * way the ESP's own ~29-task registry is already readable from the PC side
 * (see kilnctrl's get_stack_margin over the info UART bridge). Deliberately
 * its own route rather than folded into /api/zones or /api/zones_diag --
 * this is safety-processor task data, unrelated to zone config, and
 * /api/zones already had its own headroom incident (f59b21c8) from growing
 * an unrelated payload onto it.
 *
 * `link_or_null` may be NULL (no safety link compiled/initialized this
 * boot) -- the handler then reports link_up:false rather than failing to
 * register. See safety_stack_margin_http.c's header comment for the JSON
 * shape and the FLOOR-not-worst-case caveat this endpoint is required to
 * repeat in its own output. */
esp_err_t safety_stack_margin_http_start(SafetyLinkClass *link_or_null);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_STACK_MARGIN_HTTP_H */
