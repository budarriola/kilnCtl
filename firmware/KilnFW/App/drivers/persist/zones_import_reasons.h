#pragma once
/* Reason strings zones_config_import_blob() reports, in a tiny header so tests that stub out
 * zones_config_accessors.h can share the REAL text instead of re-#defining it (mirror drift). */

/* Reported when the import refused ONLY because a profile or autotune run holds the claim. The blob itself was
 * not judged invalid, so a caller that would otherwise treat a refusal as "bad data" (boot restore clearing the
 * active id) must compare against this and keep its state (review 15 LOW-1). */
#define ZONES_IMPORT_REASON_RUN_CLAIMED "a profile or autotune run is active -- retry when it ends"
