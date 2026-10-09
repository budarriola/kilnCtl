// saftyfw_image_identity_record.h -- SaftyFW's own instance of the
// build-identity record every slot image carries
// (firmware/CommonFW/include/kilnlink/saftyfw_image_identity.h has the
// layout, the rationale and the ESP-side scanner that reads it).
//
// The accessor exists for two reasons, both load-bearing:
//
//  1. It gives the record a real caller. A `const` object nothing references
//     is exactly what -ffunction-sections/-fdata-sections plus --gc-sections
//     exists to delete, and a record garbage-collected out of the image is a
//     record the ESP can never find. link_task.c calls this.
//
//  2. It makes the record the SINGLE SOURCE of the identity this firmware
//     reports. link_task_send_fw_version() packs FW_VERSION's commit/dirty
//     fields straight out of this record rather than out of
//     saftyfw_build_info.h's macros, so "what the Pico says it is" and "what
//     the image says it is" are the same bytes, not two readings of the same
//     macro that a future edit could separate. The ESP's whole out-of-date
//     decision rests on those two being comparable
//     (docs/PICO_AUTO_UPDATE.md G2).
#ifndef SAFTYFW_IMAGE_IDENTITY_RECORD_H
#define SAFTYFW_IMAGE_IDENTITY_RECORD_H

#include "kilnlink/saftyfw_image_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Never NULL. Points at the one record compiled into this image. */
const saftyfw_image_identity_t *saftyfw_image_identity_get(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_IMAGE_IDENTITY_RECORD_H
