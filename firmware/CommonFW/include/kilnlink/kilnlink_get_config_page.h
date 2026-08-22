#ifndef KILNLINK_GET_CONFIG_PAGE_H
#define KILNLINK_GET_CONFIG_PAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_GET_CONFIG_PAGE = 0x1F -- docs/LINK_PROTOCOL.md
 * sec 4, docs/COMMISSIONING.md sec 2. Fixed 2-byte request: cmd + a 0-based
 * page index. Same shared-id, distinguished-by-direction-and-length
 * convention as GET_PARAM/PARAM: the reply (kilnlink_config_page.h's
 * SAFETY_CMD_CONFIG_PAGE) rides the same command byte and is always longer
 * than this 2-byte request.
 *
 * Paging exists because a whole-record dump does not fit: CONFIG_REFERENCE.md
 * secs 1-5 is far more than the wire's 253-byte payload cap could carry in
 * one frame once every field is packed as (id, type, value). The Pico packs
 * as many (id, value) pairs as fit starting from wherever `page_index` says
 * to resume, and reports in the reply whether more remain -- see
 * kilnlink_config_page.h for that side. The ESP just asks for pages 0, 1, 2,
 * ... until a reply says no more follow.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_GET_CONFIG_PAGE_CMD 0x1Fu
#define KILNLINK_GET_CONFIG_PAGE_LEN 2u /* cmd(1) + page_index u8(1) */

typedef enum {
    KILNLINK_GET_CONFIG_PAGE_OK = 0,
    KILNLINK_GET_CONFIG_PAGE_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_GET_CONFIG_PAGE_LEN */
    KILNLINK_GET_CONFIG_PAGE_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_GET_CONFIG_PAGE_LEN (fixed-size frame) */
    KILNLINK_GET_CONFIG_PAGE_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_GET_CONFIG_PAGE_CMD */
} kilnlink_get_config_page_status_t;

typedef struct {
    uint8_t page_index; /* 0-based; the Pico resumes packing from wherever page (page_index - 1) left off */
} kilnlink_get_config_page_t;

/* Serializes `msg` (SAFETY_CMD_GET_CONFIG_PAGE payload, byte 0 = 0x1F
 * included) into `out`. Always exactly KILNLINK_GET_CONFIG_PAGE_LEN (2)
 * bytes. Returns 2, or 0 on KILNLINK_GET_CONFIG_PAGE_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_get_config_page_encode(const kilnlink_get_config_page_t *msg, uint8_t *out,
                                        size_t out_cap,
                                        kilnlink_get_config_page_status_t *status);

/* Parses a GET_CONFIG_PAGE payload (as extracted from
 * kilnlink_frame_t::payload) into `out`. `len` must be exactly
 * KILNLINK_GET_CONFIG_PAGE_LEN -- untrusted input from another processor
 * across an isolated link (CommonFW/README.md rule 6). */
kilnlink_get_config_page_status_t kilnlink_get_config_page_decode(const uint8_t *payload,
                                                                    size_t len,
                                                                    kilnlink_get_config_page_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_GET_CONFIG_PAGE_H */
