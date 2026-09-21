// ota_pico_relay.c -- see ota_pico_relay.h.
//
// --- UPDATE_BEGIN's 36-byte wire layout (mirrored from SaftyFW) -----------
//
// SaftyFW's src/update/image_header.h is the frozen source of truth (its
// own header comment: "If firmware/KilnFW ever sends a real UPDATE_BEGIN
// frame, THIS layout is what it must match"). KilnFW cannot #include that
// file (separate repository/build target), so the layout is reproduced
// byte-for-byte here instead:
//
//   Offset  Size  Field
//        0     4  magic               ("SAFU", 0x53414655 LE)
//        4     1  target              (1 = RP2040)
//        5     1  header_version      (1)
//        6     2  protocol_version    (u16 LE)
//        8     2  min_compatible      (u16 LE)
//       10     1  requested_slot      (advisory only -- sent as 0, see below)
//       11     1  flags               (reserved, 0)
//       12     4  length              (u32 LE)
//       16     4  crc32               (u32 LE)
//       20    16  version             (ASCII, NOT null-terminated)
//       36  total
//
// requested_slot is sent as 0 unconditionally: image_header.h's own header
// comment states the Pico computes which slot it actually stages into
// itself and treats this field as advisory-only, logging (not obeying) a
// mismatch -- so there is no benefit to this side guessing, and a wrong
// guess costs nothing but a log line on the Pico. Both target and
// header_version have exactly one legal value today (UPDATE_IMAGE_TARGET_RP2040
// / UPDATE_IMAGE_HEADER_VERSION in that file), reproduced here as plain
// literals rather than shared #defines for the same "separate build target"
// reason as the rest of this layout.
//
// --- UPDATE_END's payload: "4 B: image CRC32 repeated" ---------------------
//
// UPDATE_PROTOCOL.md section 4's frame table describes UPDATE_END's payload
// with exactly that phrase. Read plainly, "repeated" means the same crc32
// value already carried in UPDATE_BEGIN's header is sent again here as a
// redundant confirmation (SaftyFW's update_task_process_end() reads it
// this way too: it compares the END frame's crc32 against s_header.crc32
// and logs, but does not act on, a mismatch -- the read-back-from-flash CRC
// is what actually gates acceptance). This file follows that reading: the
// 4-byte payload is the image crc32 once, not the crc32 value written
// twice. Flagged here because the doc phrase is genuinely ambiguous and
// this is the interpretation this code commits to.
//
// --- FreeRTOS task shape -----------------------------------------------
//
// One relay at a time (ota_http.h's cross-processor update mutex already
// guarantees this at the HTTP layer; s_relay_running below is this file's
// own, cheaper, second guard against ota_pico_relay_start() being called
// twice by a bug rather than by a legitimate second HTTP request). Modest
// priority/stack, same convention as safety_link.c's own background task
// (SAFETY_POLL_TASK_PRIORITY/_STACK) -- see the #defines below.
#include "ota_pico_relay.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "hal_time.h" // hal_time_now_us() -- ota_record_t's uptime_s, same source ota_http.c uses

#include "kilnlink/kilnlink_version.h"

#include "ota_state.h"
#include "ota_record.h"
#include "uart_task_ids.h"

static const char *TAG = "ota_pico_relay";

// --- UPDATE_BEGIN wire constants (see this file's header comment) ---------
#define UPDATE_IMAGE_HEADER_MAGIC     0x53414655u // "SAFU"
#define UPDATE_IMAGE_TARGET_RP2040    1u
#define UPDATE_IMAGE_HEADER_VERSION   1u
#define UPDATE_IMAGE_HEADER_WIRE_LEN  36u
#define UPDATE_IMAGE_VERSION_LEN      16u

// UPDATE_DATA's per-frame image payload -- UPDATE_PROTOCOL.md section 4:
// "UART_PROTO_MAX_PAYLOAD is 253, so 248 bytes of image per frame after the
// 4-byte offset". Confirmed against SaftyFW's src/update/received_ranges.h
// UPDATE_CHUNK_LEN, same value.
#define UPDATE_CHUNK_LEN 248u

// No real build identity is available for a Pico image relayed through a
// raw (non-multipart) HTTP body -- there is no filename, and this code does
// not parse the RP2040 binary looking for an embedded version string
// (SaftyFW has no fixed-offset "read my own version out of a raw .bin"
// convention this pass could target). This placeholder is exactly 16 bytes
// so no padding/truncation logic is even exercised in the common case --
// flagged as a design decision a future pass may want to replace with
// something more meaningful (e.g. a version string supplied by the caller
// via a request header, or read from the browser's original filename if a
// multipart form is adopted later).
#define OTA_PICO_RELAY_DEFAULT_VERSION "esp-relay-upload"
_Static_assert(sizeof(OTA_PICO_RELAY_DEFAULT_VERSION) - 1u == UPDATE_IMAGE_VERSION_LEN,
               "OTA_PICO_RELAY_DEFAULT_VERSION must be exactly 16 bytes");

#define OTA_PICO_RELAY_TASK_STACK    6144
#define OTA_PICO_RELAY_TASK_PRIORITY 4 // below SAFETY_POLL_TASK_PRIORITY (5) -- the link's own
                                        // poll/health task must never be starved by this one

// --- Timeouts/pacing --------------------------------------------------------
// All generous relative to UPDATE_PROTOCOL.md's own honest throughput math
// (~35 s minimum for a 200 KB image) and SaftyFW's own erase-time reasoning
// (update_task.c: 208 * 4K sector erases -- see RELAY_ERASE_TIMEOUT_MS below).
#define RELAY_BEGIN_REPLY_TIMEOUT_MS 4000u   // Pico's precondition+header check is synchronous
// Was 15000u, derived from SaftyFW erasing the 832K slot as 13 * 64K blocks at
// "hundreds of milliseconds" each. 2026-09-18
// (docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md) changed that: the
// 64K block erase stalled both RP2040 cores past its own 1000 ms hardware
// watchdog and reset the safety processor partway through every update, so
// update_task.c's UPDATE_TASK_ERASE_CHUNK_SIZE dropped to one 4K sector. The
// slot is now erased as 0xD0000 / 4096 = 208 sector erases.
//
// Sector erase is less efficient per byte than block erase, so the whole-slot
// erase got slower: roughly 9-10 s at typical per-sector times against roughly
// 2 s before. 120000 ms is sized against the WORST case instead, which is the
// number that has to fit here -- a timeout that only covers typical silently
// turns a slow-but-healthy erase into a reported update failure. At the
// W25Q16JV-family datasheet MAXIMUM sector-erase time of 400 ms, 208 sectors
// is 83.2 s; 120000 ms covers that with ~1.44x margin for the per-sector
// safe-execute handshake and link latency on top.
//
// That 400 ms figure is INFERRED, not verified from this repo: the flash die
// is not named anywhere in-tree (the schematic says only "2MB QSPI flash" on a
// stock Pico module), so it comes from that module's standard part family
// rather than from a datasheet this repo owns. It is used here only to widen a
// timeout on the WAITING side, which is the safe direction to be wrong in --
// this delays the report of a genuinely stuck Pico and weakens no guard. The
// Pico's own 1000 ms watchdog, not this constant, is what actually bounds a
// wedged safety processor.
#define RELAY_ERASE_TIMEOUT_MS       120000u // 208 * 4K sector erases, sized at datasheet maximum
#define RELAY_END_REPLY_TIMEOUT_MS   10000u  // read-back CRC over up to 832K
#define RELAY_STATUS_POLL_MS         150u    // how often this task re-checks the cached status
#define RELAY_GAP_ROUND_WAIT_MS      2000u   // > SaftyFW's 500ms periodic-status cadence, with margin
#define RELAY_MAX_RETRANSMIT_ROUNDS  10u     // mirrors SaftyFW's UPDATE_MAX_RETRANSMIT_ROUNDS
                                              // (src/update/update_receiver.h) -- not shared, same
                                              // "separate build target" reason as everything else here

// --- pico_img partition lookup, cached ---------------------------------

static const esp_partition_t *s_pico_img_partition = NULL;

const esp_partition_t *ota_pico_img_partition(void)
{
    if (s_pico_img_partition) {
        return s_pico_img_partition;
    }
    s_pico_img_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, "pico_img");
    if (!s_pico_img_partition) {
        ESP_LOGE(TAG, "pico_img partition not found -- board flashed with an unexpected partition table?");
    }
    return s_pico_img_partition;
}

// --- Status (phase/percent/last_error) ----------------------------------
// A plain `volatile` per field (ota_http.c's ota_http_get_esp_progress()
// pattern) is not enough here because last_error is a string: a reader
// could observe a torn write mid-strncpy. portMUX-guarded critical
// sections are cheap (no blocking, no allocation) and need no explicit
// create step, unlike a semaphore -- this status can legitimately be read
// before any relay has ever started.
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static ota_pico_relay_phase_t s_phase = OTA_PICO_RELAY_PHASE_IDLE;
static uint8_t s_percent = 0;
static char s_last_error[OTA_PICO_RELAY_ERROR_MAX] = "";
static bool s_relay_running = false;
static TaskHandle_t s_relay_task = NULL;

const char *ota_pico_relay_phase_str(ota_pico_relay_phase_t phase)
{
    switch (phase) {
        case OTA_PICO_RELAY_PHASE_IDLE: return "idle";
        case OTA_PICO_RELAY_PHASE_BEGIN: return "begin";
        case OTA_PICO_RELAY_PHASE_ERASING: return "erasing";
        case OTA_PICO_RELAY_PHASE_SENDING: return "sending";
        case OTA_PICO_RELAY_PHASE_RETRANSMIT: return "retransmit";
        case OTA_PICO_RELAY_PHASE_FINISHING: return "finishing";
        case OTA_PICO_RELAY_PHASE_DONE: return "done";
        case OTA_PICO_RELAY_PHASE_FAILED: return "failed";
        default: return "unknown";
    }
}

static void relay_set_percent(uint8_t percent)
{
    taskENTER_CRITICAL(&s_status_mux);
    s_percent = percent;
    taskEXIT_CRITICAL(&s_status_mux);
}

static void relay_set_phase(ota_pico_relay_phase_t phase)
{
    taskENTER_CRITICAL(&s_status_mux);
    s_phase = phase;
    taskEXIT_CRITICAL(&s_status_mux);
}

// Formats outside the critical section (vsnprintf is not guaranteed
// allocation/blocking-free on every libc), then copies the finished string
// in under the lock -- same "format first, copy under lock" split
// ota_http.c's set_fail_reason() uses, adapted for a critical section
// instead of a semaphore.
static void relay_set_error(ota_pico_relay_phase_t phase, const char *fmt, ...)
{
    char tmp[OTA_PICO_RELAY_ERROR_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    taskENTER_CRITICAL(&s_status_mux);
    s_phase = phase;
    strncpy(s_last_error, tmp, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
    taskEXIT_CRITICAL(&s_status_mux);
}

void ota_pico_relay_get_status(ota_pico_relay_status_t *out)
{
    if (!out) {
        return;
    }
    taskENTER_CRITICAL(&s_status_mux);
    out->phase = s_phase;
    out->percent = s_percent;
    strncpy(out->last_error, s_last_error, sizeof(out->last_error) - 1);
    out->last_error[sizeof(out->last_error) - 1] = '\0';
    taskEXIT_CRITICAL(&s_status_mux);
}

// --- Little-endian packers (this file's own -- small enough not to be
// worth sharing with safety_link.c's identical helpers across a translation
// unit boundary for two call sites) ---------------------------------------

static void put_u16_le(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32_le(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Translates SAFETY_LINK_UPDATE_ERR_* bits into a comma-joined human string
// -- same "specific, never generic" convention as ota_interlock_check()'s
// refusal reasons. "none" if err == 0.
static void format_update_error(uint8_t err, char *out, size_t cap)
{
    static const struct {
        uint8_t bit;
        const char *name;
    } bits[] = {
        { SAFETY_LINK_UPDATE_ERR_RELAY_CLOSED, "safety relay closed" },
        { SAFETY_LINK_UPDATE_ERR_TRIP_PENDING, "a safety trip is pending" },
        { SAFETY_LINK_UPDATE_ERR_TOO_HOT, "measured temperature above the update ceiling" },
        { SAFETY_LINK_UPDATE_ERR_HEADER_INVALID, "UPDATE_BEGIN header rejected" },
        { SAFETY_LINK_UPDATE_ERR_VERSION_INCOMPATIBLE, "protocol version incompatible" },
        { SAFETY_LINK_UPDATE_ERR_RETRANSMIT_CAP, "retransmission round cap reached" },
        { SAFETY_LINK_UPDATE_ERR_CRC_MISMATCH, "CRC mismatch on the written image" },
        { SAFETY_LINK_UPDATE_ERR_INTERNAL, "Pico-side internal error" },
    };
    if (err == 0) {
        snprintf(out, cap, "none");
        return;
    }
    out[0] = '\0';
    bool first = true;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (err & bits[i].bit) {
            size_t len = strlen(out);
            snprintf(out + len, cap - len, "%s%s", first ? "" : ", ", bits[i].name);
            first = false;
        }
    }
}

// --- Polling the cached UPDATE_STATUS --------------------------------------

// Polls safety_link_get_update_status() until it observes a FRESH frame
// (one that arrived after `since_ms`, per the age_ms/elapsed comparison
// below) whose state is in `accept_state_mask` (bit N = 1 << state) OR is
// one of the three terminal-but-not-explicitly-accepted states
// (REFUSED/FAILED/ABORTED, which always end the wait so the caller can
// react to them rather than spin until its own timeout), or until
// `timeout_ms` (measured from `since_ms`) elapses.
//
// "Fresh" matters because the cache holds whatever the last UPDATE_STATUS
// said regardless of when it arrived -- without a freshness check, a status
// left over from a PREVIOUS relay attempt (or none at all, if the struct
// is still zeroed) could be mistaken for a reply to THIS attempt. age_ms is
// "how long ago this cached frame arrived"; elapsed is "how long ago
// since_ms was"; a frame that arrived after since_ms therefore satisfies
// age_ms <= elapsed.
//
// Returns true iff a matching frame was observed before the deadline
// (`*out` is filled; caller inspects out->state to see which case fired).
// Returns false on timeout (`*out` is NOT touched).
static bool relay_wait_for_states(SafetyLinkClass *link, uint32_t since_ms, uint32_t timeout_ms,
                                   uint32_t accept_state_mask, safety_link_update_status_t *out)
{
    uint32_t deadline = since_ms + timeout_ms;
    for (;;) {
        safety_link_update_status_t st;
        uint32_t age_ms = 0;
        if (safety_link_get_update_status(link, &st, &age_ms) == ESP_OK) {
            uint32_t elapsed = now_ms() - since_ms;
            if (age_ms <= elapsed) {
                uint32_t bit = (st.state < 32u) ? (1u << st.state) : 0u;
                bool terminal = (st.state == SAFETY_LINK_UPDATE_STATE_REFUSED) ||
                                 (st.state == SAFETY_LINK_UPDATE_STATE_FAILED) ||
                                 (st.state == SAFETY_LINK_UPDATE_STATE_ABORTED) ||
                                 (st.state == SAFETY_LINK_UPDATE_STATE_REJECTED_SLOT_LINKAGE) ||
                                 (st.state == SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP);
                if ((bit & accept_state_mask) || terminal) {
                    *out = st;
                    return true;
                }
            }
        }
        if (now_ms() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(RELAY_STATUS_POLL_MS));
    }
}

// Formats into a generously-sized scratch buffer, then copies (truncating
// if needed, never overflowing) into the caller's smaller `dst` -- same
// split ota_http.c's set_fail_reason() uses. Several of the call sites below
// interpolate two %s values (a state name plus format_update_error()'s own
// output) whose combined width the compiler cannot bound at `reason`'s
// fixed size, which -Werror=format-truncation (correctly) refuses to build
// against a raw snprintf(reason, sizeof(reason), ...) -- formatting into
// `tmp` first, sized generously enough that no realistic message here
// actually truncates, sidesteps that without shortening the messages.
static void format_reason(char *dst, size_t dst_cap, const char *fmt, ...)
{
    char tmp[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    strncpy(dst, tmp, dst_cap - 1);
    dst[dst_cap - 1] = '\0';
}

// --- The relay task itself --------------------------------------------------

typedef struct {
    SafetyLinkClass *link;
    uint32_t image_length;
    uint32_t image_crc32;
    char version16[UPDATE_IMAGE_VERSION_LEN];
    bool have_sha256;
    uint8_t sha256[32];
} relay_args_t;

// Single instance: s_relay_running (checked-and-set in ota_pico_relay_start())
// guarantees at most one relay task reads this at a time, and it is fully
// populated before the task that reads it is created.
static relay_args_t s_relay_args;

// Sends UPDATE_ABORT best-effort -- used on every failure path below so a
// half-accepted transfer on the Pico side is told to give up rather than
// left waiting for data that is never coming (UPDATE_PROTOCOL.md: "on any
// unrecoverable failure, send UPDATE_ABORT"). Harmless/no-op on the Pico
// side if it never accepted a transfer in the first place, or already
// reached a terminal state on its own (update_task_process_abort() is a
// no-op when !s_transfer_active) -- so this is safe to call unconditionally
// rather than tracking exactly how far the Pico got.
static void relay_send_abort(SafetyLinkClass *link)
{
    uint8_t frame[1] = { SAFETY_CMD_UPDATE_ABORT };
    (void)safety_link_send_update_frame(link, frame, sizeof(frame));
}

static void relay_task_fn(void *arg)
{
    (void)arg;
    relay_args_t args = s_relay_args; // local copy -- safe, single relay at a time
    bool ok = false;
    char reason[OTA_PICO_RELAY_ERROR_MAX] = "unknown failure";
    char err_str[96];

    relay_set_phase(OTA_PICO_RELAY_PHASE_BEGIN);
    relay_set_percent(0);

    // TODO.md 9.6: from here until the `done:` label below, the link
    // legitimately going quiet is an EXPECTED consequence of this relay
    // (the Pico erases/reboots), not a fault -- see safety_link.h's own
    // comment on this setter for what it does and does not affect (text
    // only; SAFETY_FAULT_SRC_SAFETY_LINK keeps asserting, untouched).
    (void)safety_link_set_update_in_progress(args.link, true);

    const esp_partition_t *part = ota_pico_img_partition();
    if (!part) {
        format_reason(reason, sizeof(reason), "pico_img partition not found");
        goto abort_and_fail;
    }
    if (args.image_length == 0 || args.image_length > part->size) {
        format_reason(reason, sizeof(reason), "image length %u invalid for pico_img partition (%u bytes)",
                 (unsigned)args.image_length, (unsigned)part->size);
        goto abort_and_fail;
    }

    // --- UPDATE_BEGIN ---
    {
        uint8_t hdr[UPDATE_IMAGE_HEADER_WIRE_LEN];
        size_t i = 0;
        put_u32_le(&hdr[i], UPDATE_IMAGE_HEADER_MAGIC); i += 4;
        hdr[i++] = UPDATE_IMAGE_TARGET_RP2040;
        hdr[i++] = UPDATE_IMAGE_HEADER_VERSION;
        put_u16_le(&hdr[i], (uint16_t)KILNLINK_PROTOCOL_VERSION); i += 2;
        put_u16_le(&hdr[i], (uint16_t)KILNLINK_MIN_COMPATIBLE); i += 2;
        hdr[i++] = 0; // requested_slot -- advisory only, see this file's header comment
        hdr[i++] = 0; // flags, reserved
        put_u32_le(&hdr[i], args.image_length); i += 4;
        put_u32_le(&hdr[i], args.image_crc32); i += 4;
        memcpy(&hdr[i], args.version16, UPDATE_IMAGE_VERSION_LEN); i += UPDATE_IMAGE_VERSION_LEN;
        // i == UPDATE_IMAGE_HEADER_WIRE_LEN here, by construction.

        uint8_t frame[1 + UPDATE_IMAGE_HEADER_WIRE_LEN];
        frame[0] = SAFETY_CMD_UPDATE_BEGIN;
        memcpy(&frame[1], hdr, sizeof(hdr));

        uint32_t t0 = now_ms();
        esp_err_t err = safety_link_send_update_frame(args.link, frame, sizeof(frame));
        if (err != ESP_OK) {
            format_reason(reason, sizeof(reason), "could not send UPDATE_BEGIN: %s", esp_err_to_name(err));
            goto abort_and_fail;
        }

        safety_link_update_status_t st;
        bool got = relay_wait_for_states(
            args.link, t0, RELAY_BEGIN_REPLY_TIMEOUT_MS,
            (1u << SAFETY_LINK_UPDATE_STATE_ERASING) | (1u << SAFETY_LINK_UPDATE_STATE_RECEIVING), &st);
        if (!got) {
            format_reason(reason, sizeof(reason), "no reply to UPDATE_BEGIN within %u ms -- link down?",
                     (unsigned)RELAY_BEGIN_REPLY_TIMEOUT_MS);
            goto abort_and_fail;
        }
        if (st.state == SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP) {
            format_reason(reason, sizeof(reason),
                     "Pico refused: update would overwrite its running flat image; reflash via SWD");
            goto abort_and_fail;
        }
        if (st.state == SAFETY_LINK_UPDATE_STATE_REFUSED) {
            format_update_error(st.last_error, err_str, sizeof(err_str));
            format_reason(reason, sizeof(reason), "Pico refused UPDATE_BEGIN: %s", err_str);
            goto abort_and_fail;
        }
        if (st.state == SAFETY_LINK_UPDATE_STATE_FAILED || st.state == SAFETY_LINK_UPDATE_STATE_ABORTED) {
            format_update_error(st.last_error, err_str, sizeof(err_str));
            format_reason(reason, sizeof(reason), "Pico reported %s after UPDATE_BEGIN: %s",
                     st.state == SAFETY_LINK_UPDATE_STATE_FAILED ? "FAILED" : "ABORTED", err_str);
            goto abort_and_fail;
        }

        if (st.state == SAFETY_LINK_UPDATE_STATE_ERASING) {
            relay_set_phase(OTA_PICO_RELAY_PHASE_ERASING);
            bool got_recv = relay_wait_for_states(args.link, t0, RELAY_ERASE_TIMEOUT_MS,
                                                    (1u << SAFETY_LINK_UPDATE_STATE_RECEIVING), &st);
            if (!got_recv) {
                format_reason(reason, sizeof(reason), "Pico did not confirm RECEIVING within %u ms of erase",
                         (unsigned)RELAY_ERASE_TIMEOUT_MS);
                goto abort_and_fail;
            }
            if (st.state == SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP) {
                format_reason(reason, sizeof(reason),
                         "Pico refused: update would overwrite its running flat image; reflash via SWD");
                goto abort_and_fail;
            }
            if (st.state != SAFETY_LINK_UPDATE_STATE_RECEIVING) {
                format_update_error(st.last_error, err_str, sizeof(err_str));
                format_reason(reason, sizeof(reason), "Pico did not reach RECEIVING (state=%u): %s",
                         (unsigned)st.state, err_str);
                goto abort_and_fail;
            }
        }
        // st.state == RECEIVING here either way -- fall through to streaming.
    }

    // --- UPDATE_DATA: sequential first pass, straight from flash -----------
    relay_set_phase(OTA_PICO_RELAY_PHASE_SENDING);
    {
        uint32_t total_chunks = (args.image_length + UPDATE_CHUNK_LEN - 1u) / UPDATE_CHUNK_LEN;
        uint8_t chunk_buf[UPDATE_CHUNK_LEN];
        uint8_t frame[1 + 4 + UPDATE_CHUNK_LEN];
        frame[0] = SAFETY_CMD_UPDATE_DATA;

        for (uint32_t idx = 0; idx < total_chunks; idx++) {
            uint32_t offset = idx * UPDATE_CHUNK_LEN;
            size_t len = (offset + UPDATE_CHUNK_LEN <= args.image_length)
                             ? UPDATE_CHUNK_LEN
                             : (args.image_length - offset);

            esp_err_t rerr = esp_partition_read(part, offset, chunk_buf, len);
            if (rerr != ESP_OK) {
                format_reason(reason, sizeof(reason), "pico_img read failed at offset %u: %s",
                         (unsigned)offset, esp_err_to_name(rerr));
                goto abort_and_fail;
            }

            put_u32_le(&frame[1], offset);
            memcpy(&frame[5], chunk_buf, len);
            esp_err_t serr = safety_link_send_update_frame(args.link, frame, 5u + len);
            if (serr != ESP_OK) {
                format_reason(reason, sizeof(reason), "UPDATE_DATA send failed at offset %u: %s",
                         (unsigned)offset, esp_err_to_name(serr));
                goto abort_and_fail;
            }

            // Reserve the top 30% of the progress bar for retransmission +
            // verification/finalization below -- the first pass alone is
            // not "done", so it should not report 100%.
            if ((idx % 32u) == 0u || idx + 1u == total_chunks) {
                relay_set_percent((uint8_t)(((uint64_t)(idx + 1u) * 60u) / total_chunks));
            }
        }
    }

    // --- Retransmission rounds ----------------------------------------------
    // UPDATE_PROTOCOL.md section 4's "Throughput: stop-and-wait is the wrong
    // tool" design: the sequential pass above is unacknowledged, and the
    // Pico's periodic UPDATE_STATUS gap reports (every ~500 ms while a
    // transfer is active) name exactly what is still missing. This loop
    // retransmits named chunks for up to RELAY_MAX_RETRANSMIT_ROUNDS rounds.
    // The final CRC check inside UPDATE_END below is the true arbiter of
    // correctness (per that same section: "acknowledging each frame is not
    // what makes the transfer correct -- the final verify is"), so this
    // loop does not need to reconstruct the Pico's own round-counting
    // algorithm exactly -- it just keeps retrying named gaps, bounded, and
    // lets UPDATE_END's reply be the final word.
    relay_set_phase(OTA_PICO_RELAY_PHASE_RETRANSMIT);
    {
        uint8_t chunk_buf[UPDATE_CHUNK_LEN];
        uint8_t frame[1 + 4 + UPDATE_CHUNK_LEN];
        frame[0] = SAFETY_CMD_UPDATE_DATA;
        uint32_t total_chunks = (args.image_length + UPDATE_CHUNK_LEN - 1u) / UPDATE_CHUNK_LEN;

        for (uint32_t round = 0; round < RELAY_MAX_RETRANSMIT_ROUNDS; round++) {
            uint32_t t_round = now_ms();
            safety_link_update_status_t st;
            bool got = relay_wait_for_states(
                args.link, t_round, RELAY_GAP_ROUND_WAIT_MS,
                (1u << SAFETY_LINK_UPDATE_STATE_RECEIVING) | (1u << SAFETY_LINK_UPDATE_STATE_VERIFYING) |
                    (1u << SAFETY_LINK_UPDATE_STATE_COMPLETE),
                &st);
            if (!got) {
                // No fresh status this round -- link may be momentarily
                // quiet (the Pico only sends one every ~500 ms). Not fatal
                // by itself; try again next round rather than aborting on
                // one missed cycle.
                continue;
            }
            if (st.state == SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP) {
                format_reason(reason, sizeof(reason),
                         "Pico refused: update would overwrite its running flat image; reflash via SWD");
                goto abort_and_fail;
            }
            if (st.state == SAFETY_LINK_UPDATE_STATE_FAILED || st.state == SAFETY_LINK_UPDATE_STATE_ABORTED) {
                format_update_error(st.last_error, err_str, sizeof(err_str));
                format_reason(reason, sizeof(reason), "Pico reported %s during retransmission: %s",
                         st.state == SAFETY_LINK_UPDATE_STATE_FAILED ? "FAILED" : "ABORTED", err_str);
                goto abort_and_fail;
            }
            if (st.gap_count == 0u) {
                break; // nothing named missing this round -- proceed to UPDATE_END
            }

            for (uint8_t g = 0; g < st.gap_count; g++) {
                uint32_t idx = st.gap_chunk_indices[g];
                if (idx >= total_chunks) {
                    continue; // defensive -- untrusted wire value
                }
                uint32_t offset = idx * UPDATE_CHUNK_LEN;
                size_t len = (offset + UPDATE_CHUNK_LEN <= args.image_length)
                                 ? UPDATE_CHUNK_LEN
                                 : (args.image_length - offset);
                if (esp_partition_read(part, offset, chunk_buf, len) != ESP_OK) {
                    continue; // best-effort -- next round (or UPDATE_END's CRC check) catches a persistent miss
                }
                put_u32_le(&frame[1], offset);
                memcpy(&frame[5], chunk_buf, len);
                (void)safety_link_send_update_frame(args.link, frame, 5u + len);
            }

            uint8_t pct = (uint8_t)(60u + ((round + 1u) * 25u) / RELAY_MAX_RETRANSMIT_ROUNDS);
            relay_set_percent(pct > 85u ? 85u : pct);
        }
    }

    // --- UPDATE_END --------------------------------------------------------
    relay_set_phase(OTA_PICO_RELAY_PHASE_FINISHING);
    relay_set_percent(90);
    {
        uint8_t frame[1 + 4];
        frame[0] = SAFETY_CMD_UPDATE_END;
        put_u32_le(&frame[1], args.image_crc32);

        uint32_t t_end = now_ms();
        esp_err_t err = safety_link_send_update_frame(args.link, frame, sizeof(frame));
        if (err != ESP_OK) {
            format_reason(reason, sizeof(reason), "could not send UPDATE_END: %s", esp_err_to_name(err));
            goto abort_and_fail;
        }

        safety_link_update_status_t st;
        bool got = relay_wait_for_states(args.link, t_end, RELAY_END_REPLY_TIMEOUT_MS,
                                          (1u << SAFETY_LINK_UPDATE_STATE_COMPLETE), &st);
        if (!got) {
            format_reason(reason, sizeof(reason), "no reply to UPDATE_END within %u ms",
                     (unsigned)RELAY_END_REPLY_TIMEOUT_MS);
            goto abort_and_fail;
        }
        if (st.state == SAFETY_LINK_UPDATE_STATE_COMPLETE) {
            ok = true;
            format_reason(reason, sizeof(reason), "ok");
            relay_set_percent(100);
        } else if (st.state == SAFETY_LINK_UPDATE_STATE_RECEIVING ||
                   st.state == SAFETY_LINK_UPDATE_STATE_VERIFYING) {
            format_reason(reason, sizeof(reason),
                     "image still incomplete after %u retransmission rounds -- gave up",
                     (unsigned)RELAY_MAX_RETRANSMIT_ROUNDS);
            goto abort_and_fail;
        } else if (st.state == SAFETY_LINK_UPDATE_STATE_REJECTED_SLOT_LINKAGE) {
            format_reason(reason, sizeof(reason),
                     "Pico rejected the image: vector table not linked for the slot it was written into");
            goto abort_and_fail;
        } else if (st.state == SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP) {
            format_reason(reason, sizeof(reason),
                     "Pico refused: update would overwrite its running flat image; reflash via SWD");
            goto abort_and_fail;
        } else {
            format_update_error(st.last_error, err_str, sizeof(err_str));
            format_reason(reason, sizeof(reason), "Pico reported %s after UPDATE_END: %s",
                     st.state == SAFETY_LINK_UPDATE_STATE_FAILED ? "FAILED" : "ABORTED", err_str);
            goto abort_and_fail;
        }
    }

    goto done;

abort_and_fail:
    relay_send_abort(args.link);

done:
    // Cleared unconditionally, on every exit path, before anything else --
    // the point of this flag is that it never outlives the relay it
    // describes, success or failure alike.
    (void)safety_link_set_update_in_progress(args.link, false);

    relay_set_error(ok ? OTA_PICO_RELAY_PHASE_DONE : OTA_PICO_RELAY_PHASE_FAILED, "%s", reason);
    if (ok) {
        ESP_LOGI(TAG, "Pico relay complete: %u bytes, crc32=0x%08X", (unsigned)args.image_length,
                 (unsigned)args.image_crc32);
    } else {
        ESP_LOGE(TAG, "Pico relay failed: %s", reason);
    }

    // ota_record_t's own header comment used to flag this: no record was
    // EVER written for a Pico update, not even a failed one -- only the ESP
    // self-update path called ota_record_append(). Closed here, in the one
    // place every relay attempt (success, refusal, timeout, internal
    // failure) funnels through. version_before/version_after are left ""
    // rather than guessed: this ESP-side code has no trustworthy source for
    // either (it does not parse a real version out of the raw uploaded
    // image -- see OTA_PICO_RELAY_DEFAULT_VERSION's own header comment --
    // and the Pico's OWN running version is not something this module reads
    // back after a relay finishes). A blank version field is honest; a
    // guessed one would not be.
    {
        char sha_hex[65] = "";
        if (args.have_sha256) {
            static const char digits[] = "0123456789abcdef";
            for (size_t i = 0; i < sizeof(args.sha256); i++) {
                sha_hex[2 * i] = digits[(args.sha256[i] >> 4) & 0xFu];
                sha_hex[2 * i + 1] = digits[args.sha256[i] & 0xFu];
            }
            sha_hex[2 * sizeof(args.sha256)] = '\0';
        }
        ota_record_t rec;
        ota_record_fill(&rec, (uint32_t)(hal_time_now_us() / 1000000), "pico", "", "", ok, reason,
                         sha_hex);
        ota_record_append(&rec); // best-effort, logs its own failure -- see ota_record.h
    }

    taskENTER_CRITICAL(&s_status_mux);
    s_relay_running = false;
    taskEXIT_CRITICAL(&s_status_mux);

    // Ownership handoff documented in ota_pico_relay.h's header comment:
    // the HTTP handler that called ota_pico_relay_start() already returned
    // its response and does NOT hold the mutex anymore -- this task is the
    // sole remaining owner of releasing it, exactly once, on every exit
    // path (success, refusal, timeout, internal failure all funnel through
    // this one `done:` label).
    ota_http_update_end();

    s_relay_task = NULL;
    vTaskDelete(NULL);
}

bool ota_pico_relay_start(SafetyLinkClass *link, uint32_t image_length, uint32_t image_crc32,
                           const char *version16_or_null, const uint8_t image_sha256_or_null[32])
{
    if (!link || image_length == 0) {
        return false;
    }

    bool won = false;
    taskENTER_CRITICAL(&s_status_mux);
    if (!s_relay_running) {
        s_relay_running = true;
        won = true;
    }
    taskEXIT_CRITICAL(&s_status_mux);
    if (!won) {
        ESP_LOGW(TAG, "ota_pico_relay_start refused: a relay is already running");
        return false;
    }

    s_relay_args.link = link;
    s_relay_args.image_length = image_length;
    s_relay_args.image_crc32 = image_crc32;
    s_relay_args.have_sha256 = (image_sha256_or_null != NULL);
    if (s_relay_args.have_sha256) {
        memcpy(s_relay_args.sha256, image_sha256_or_null, sizeof(s_relay_args.sha256));
    } else {
        memset(s_relay_args.sha256, 0, sizeof(s_relay_args.sha256));
    }
    memset(s_relay_args.version16, ' ', sizeof(s_relay_args.version16));
    const char *v = version16_or_null ? version16_or_null : OTA_PICO_RELAY_DEFAULT_VERSION;
    size_t n = strnlen(v, sizeof(s_relay_args.version16));
    memcpy(s_relay_args.version16, v, n);

    relay_set_phase(OTA_PICO_RELAY_PHASE_BEGIN);
    relay_set_percent(0);
    taskENTER_CRITICAL(&s_status_mux);
    s_last_error[0] = '\0';
    taskEXIT_CRITICAL(&s_status_mux);

    if (xTaskCreate(relay_task_fn, "ota_pico_relay", OTA_PICO_RELAY_TASK_STACK, NULL,
                     OTA_PICO_RELAY_TASK_PRIORITY, &s_relay_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create ota_pico_relay task");
        taskENTER_CRITICAL(&s_status_mux);
        s_relay_running = false;
        taskEXIT_CRITICAL(&s_status_mux);
        relay_set_error(OTA_PICO_RELAY_PHASE_FAILED, "internal: could not start the relay task");
        return false;
    }

    ESP_LOGI(TAG, "Pico relay started: %u bytes, crc32=0x%08X", (unsigned)image_length,
             (unsigned)image_crc32);
    return true;
}
