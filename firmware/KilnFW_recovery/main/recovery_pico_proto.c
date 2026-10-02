// recovery_pico_proto.c -- see recovery_pico_proto.h.
#include "recovery_pico_proto.h"

#include <stdio.h>
#include <string.h>

// --- CRC32 (zlib) -----------------------------------------------------------
// Nibble-table variant: 16 entries instead of 256 keeps the footprint tiny and
// is fast enough for one pass over an 832 KB image.
uint32_t rpp_crc32(const uint8_t *data, size_t len)
{
    static const uint32_t tab[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        c = (c >> 4) ^ tab[c & 0x0Fu];
        c = (c >> 4) ^ tab[c & 0x0Fu];
    }
    return ~c;
}

// --- image validation ------------------------------------------------------

uint32_t rpp_slot_xip_base(int slot)
{
    if (slot == RPP_SLOT_A) {
        return RPP_XIP_BASE + RPP_SLOT_A_OFFSET;
    }
    if (slot == RPP_SLOT_B) {
        return RPP_XIP_BASE + RPP_SLOT_B_OFFSET;
    }
    return 0;
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *rpp_image_result_str(rpp_image_result_t r)
{
    switch (r) {
    case RPP_IMG_OK: return "ok";
    case RPP_IMG_EMPTY: return "image is empty or too short to hold a vector table";
    case RPP_IMG_TOO_BIG: return "image is larger than a Pico firmware slot (832 KB)";
    case RPP_IMG_BAD_SP: return "initial stack pointer is not in RP2040 SRAM -- not a Pico application image";
    case RPP_IMG_BAD_THUMB: return "reset vector has no Thumb bit -- not a Pico application image";
    case RPP_IMG_BAD_RESET:
        return "reset vector is not inside either firmware slot -- wrong file (use SaftyFW_slotA.bin or SaftyFW_slotB.bin)";
    case RPP_IMG_CRC_MISMATCH: return "CRC32 of the received image does not match the CRC32 the browser computed";
    }
    return "unknown image error";
}

rpp_image_result_t rpp_check_image(const uint8_t *img, size_t len, uint32_t claimed_crc,
                                   int *out_slot)
{
    if (out_slot) {
        *out_slot = RPP_SLOT_UNKNOWN;
    }
    if (!img || len < 8u) {
        return RPP_IMG_EMPTY;
    }
    if (len > RPP_SLOT_SIZE) {
        return RPP_IMG_TOO_BIG;
    }
    uint32_t sp = rd_u32_le(img);
    uint32_t reset = rd_u32_le(img + 4);
    if (sp < RPP_SRAM_BASE || sp > RPP_SRAM_END) {
        return RPP_IMG_BAD_SP;
    }
    if ((reset & 1u) == 0u) {
        return RPP_IMG_BAD_THUMB;
    }
    int slot = RPP_SLOT_UNKNOWN;
    for (int s = RPP_SLOT_A; s <= RPP_SLOT_B; s++) {
        uint32_t base = rpp_slot_xip_base(s);
        if (reset >= base && reset < base + RPP_SLOT_SIZE) {
            slot = s;
        }
    }
    if (slot == RPP_SLOT_UNKNOWN) {
        return RPP_IMG_BAD_RESET;
    }
    if (rpp_crc32(img, len) != claimed_crc) {
        return RPP_IMG_CRC_MISMATCH;
    }
    if (out_slot) {
        *out_slot = slot;
    }
    return RPP_IMG_OK;
}

// --- target-slot resolution ------------------------------------------------

rpp_target_t rpp_resolve_target(int app_active_slot, int operator_slot)
{
    rpp_target_t t;
    if (app_active_slot == RPP_SLOT_A || app_active_slot == RPP_SLOT_B) {
        t.target_slot = (app_active_slot == RPP_SLOT_A) ? RPP_SLOT_B : RPP_SLOT_A;
        t.source = RPP_TARGET_FROM_APP;
    } else if (operator_slot == RPP_SLOT_A || operator_slot == RPP_SLOT_B) {
        t.target_slot = operator_slot;
        t.source = RPP_TARGET_OPERATOR;
    } else {
        t.target_slot = RPP_SLOT_UNKNOWN;
        t.source = RPP_TARGET_UNRESOLVED;
    }
    return t;
}

bool rpp_image_matches_target(int image_slot, rpp_target_t t)
{
    return (image_slot == RPP_SLOT_A || image_slot == RPP_SLOT_B) &&
           (t.target_slot == RPP_SLOT_A || t.target_slot == RPP_SLOT_B) &&
           image_slot == t.target_slot;
}

static char slot_ch(int s)
{
    return s == RPP_SLOT_A ? 'A' : s == RPP_SLOT_B ? 'B' : '?';
}

bool rpp_describe_target_refusal(rpp_target_t t, int image_slot, bool bootloader, char *out,
                                 size_t cap)
{
    if (!out || cap == 0) {
        return false;
    }
    if (rpp_image_matches_target(image_slot, t)) {
        return false;
    }
    if (t.source == RPP_TARGET_UNRESOLVED) {
        if (bootloader) {
            snprintf(out, cap,
                     "target unverified: the Pico is in its bootloader, which cannot report "
                     "which slot it will write. Choose the target slot (A or B) on the page. A "
                     "wrong choice leaves the Pico unbootable until it is reprogrammed over SWD.");
        } else {
            snprintf(out, cap,
                     "target unverified: the Pico application did not report its active slot. "
                     "Choose the target slot (A or B) on the page. A wrong choice leaves the "
                     "Pico unbootable until it is reprogrammed over SWD.");
        }
        return true;
    }
    snprintf(out, cap,
             "image is linked for slot %c but the Pico will write slot %c (%s): use "
             "SaftyFW_slot%c.bin or choose the other slot",
             slot_ch(image_slot), slot_ch(t.target_slot),
             t.source == RPP_TARGET_FROM_APP ? "read from the Pico application"
                                             : "target unverified, chosen by the operator",
             slot_ch(t.target_slot));
    return true;
}

// --- frame building -------------------------------------------------------

size_t rpp_build_frame(uint16_t msg_index, const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t cap)
{
    if (!out || (payload_len > 0 && !payload) || payload_len > KILNLINK_FRAME_MAX_PAYLOAD ||
        cap < KILNLINK_FRAME_STUFFED_MAX) {
        return 0;
    }
    kilnlink_frame_t f;
    f.msg_type = KILNLINK_MSG_BROADCAST;
    f.msg_index = msg_index;
    f.src_device = RPP_DEVICE_ESP;
    f.src_task = RPP_TASK_SAFETY;
    f.dst_device = RPP_DEVICE_SAFETY;
    f.dst_task = RPP_TASK_SAFETY;
    f.length = (uint8_t)payload_len;
    f.payload = payload;
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t st;
    size_t raw_len = kilnlink_frame_encode_raw(&f, raw, sizeof(raw), &st);
    if (raw_len == 0) {
        return 0;
    }
    return kilnlink_stuff(raw, raw_len, out, cap);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

size_t rpp_pack_get_status(uint8_t *payload)
{
    payload[0] = RPP_CMD_GET_STATUS;
    return 1;
}

size_t rpp_pack_begin(uint8_t *payload, uint32_t length, uint32_t crc32, const char *version)
{
    // 36-byte header, ota_pico_relay.c's layout: magic "SAFU" u32 LE, target 1
    // (RP2040), header_version 1, protocol_version u16 LE, min_compatible u16
    // LE, requested_slot (advisory, sent 0), flags 0, length u32, crc32 u32,
    // 16-byte space-padded version. KILNLINK_PROTOCOL_VERSION 16 /
    // KILNLINK_MIN_COMPATIBLE 7 (CommonFW kilnlink_version.h) are mirrored here
    // so this pure file needs no extra include; the receiver only compares
    // protocol_version against its own minimum.
    const uint16_t protocol_version = 16;
    const uint16_t min_compatible = 7;
    uint8_t *h = payload + 1;
    payload[0] = RPP_CMD_UPDATE_BEGIN;
    put_u32_le(h, 0x53414655u);
    h[4] = 1;
    h[5] = 1;
    h[6] = (uint8_t)protocol_version;
    h[7] = (uint8_t)(protocol_version >> 8);
    h[8] = (uint8_t)min_compatible;
    h[9] = (uint8_t)(min_compatible >> 8);
    h[10] = 0;
    h[11] = 0;
    put_u32_le(h + 12, length);
    put_u32_le(h + 16, crc32);
    memset(h + 20, ' ', RPP_VERSION_LEN);
    if (version) {
        size_t n = strnlen(version, RPP_VERSION_LEN);
        memcpy(h + 20, version, n);
    }
    return 1u + RPP_BEGIN_HEADER_LEN;
}

size_t rpp_pack_announce(uint8_t *payload)
{
    // cmd, protocol u16, min_compatible u16, dirty, commit_len, datetime_len,
    // boot_id (CommonFW kilnlink_announce.h, empty strings).
    payload[0] = RPP_CMD_ANNOUNCE_VERSION;
    payload[1] = (uint8_t)RPP_ANNOUNCE_PROTOCOL;
    payload[2] = (uint8_t)(RPP_ANNOUNCE_PROTOCOL >> 8);
    payload[3] = (uint8_t)RPP_ANNOUNCE_MIN_COMPATIBLE;
    payload[4] = (uint8_t)(RPP_ANNOUNCE_MIN_COMPATIBLE >> 8);
    payload[5] = 1; // dirty/unknown
    payload[6] = 0;
    payload[7] = 0;
    payload[8] = 0;
    return 9;
}

size_t rpp_pack_data(uint8_t *payload, uint32_t offset, const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > RPP_CHUNK_LEN) {
        return 0;
    }
    payload[0] = RPP_CMD_UPDATE_DATA;
    put_u32_le(payload + 1, offset);
    memcpy(payload + 5, data, len);
    return 5u + len;
}

size_t rpp_pack_end(uint8_t *payload, uint32_t crc32)
{
    payload[0] = RPP_CMD_UPDATE_END;
    put_u32_le(payload + 1, crc32);
    return 5;
}

size_t rpp_pack_abort(uint8_t *payload)
{
    payload[0] = RPP_CMD_UPDATE_ABORT;
    return 1;
}

size_t rpp_pack_reboot(uint8_t *payload)
{
    payload[0] = RPP_CMD_REBOOT;
    return 1;
}

// --- receive deframer -------------------------------------------------------

void rpp_rx_init(rpp_rx_t *rx)
{
    memset(rx, 0, sizeof(*rx));
}

bool rpp_rx_push(rpp_rx_t *rx, uint8_t byte, kilnlink_frame_t *out)
{
    if (byte != KILNLINK_FRAME_DELIM) {
        if (rx->n < sizeof(rx->wire)) {
            rx->wire[rx->n++] = byte;
        } else {
            rx->overflow = true;
        }
        return false;
    }
    // Delimiter: close whatever was accumulated (back-to-back delimiters give
    // an empty body, silently ignored).
    size_t n = rx->n;
    bool overflow = rx->overflow;
    rx->n = 0;
    rx->overflow = false;
    if (n == 0 && !overflow) {
        return false;
    }
    if (overflow) {
        rx->frames_bad++;
        return false;
    }
    kilnlink_frame_status_t st = KILNLINK_FRAME_OK;
    size_t raw_len = kilnlink_unstuff(rx->wire, n, rx->raw, sizeof(rx->raw), &st);
    if (raw_len == 0 || st != KILNLINK_FRAME_OK) {
        rx->frames_bad++;
        return false;
    }
    kilnlink_frame_t f;
    if (kilnlink_frame_decode(rx->raw, raw_len, &f) != KILNLINK_FRAME_OK) {
        rx->frames_bad++;
        return false;
    }
    if (f.msg_type != KILNLINK_MSG_BROADCAST || f.src_device != RPP_DEVICE_SAFETY ||
        f.dst_device != RPP_DEVICE_ESP) {
        rx->frames_bad++;
        return false;
    }
    rx->frames_ok++;
    *out = f;
    return true;
}

// --- parsing --------------------------------------------------------------

bool rpp_parse_status(const uint8_t *p, size_t len, rpp_status_t *out)
{
    if (!p || !out || len < RPP_STATUS_HEADER_LEN || p[0] != RPP_CMD_UPDATE_STATUS) {
        return false;
    }
    uint8_t gap_count = p[15];
    if (gap_count > RPP_STATUS_MAX_GAPS) {
        gap_count = (uint8_t)RPP_STATUS_MAX_GAPS;
    }
    if (len < (size_t)RPP_STATUS_HEADER_LEN + (size_t)gap_count * 2u) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->state = p[1];
    out->err = p[2];
    out->bytes_received = rd_u32_le(p + 3);
    out->total_chunks = rd_u32_le(p + 7);
    out->received_chunks = rd_u32_le(p + 11);
    out->gap_count = gap_count;
    for (uint8_t i = 0; i < gap_count; i++) {
        out->gaps[i] = (uint16_t)(p[16 + 2u * i] | (p[17 + 2u * i] << 8));
    }
    return true;
}

int rpp_parse_frame_a_active_slot(const uint8_t *p, size_t len)
{
    // Frame A: command byte 0x01 then the CommonFW kilnlink_frame_a_offsets.h
    // layout; flags2 (offset 24, V3 = 26 bytes) bit 0x08 = ACTIVE_SLOT_KNOWN,
    // 0x10 = ACTIVE_SLOT_B (only meaningful when KNOWN).
    if (!p || len < 26u || p[0] != RPP_CMD_GET_STATUS) {
        return RPP_SLOT_UNKNOWN;
    }
    uint8_t flags2 = p[24];
    if ((flags2 & 0x08u) == 0u) {
        return RPP_SLOT_UNKNOWN;
    }
    return (flags2 & 0x10u) ? RPP_SLOT_B : RPP_SLOT_A;
}

void rpp_format_err_bits(uint8_t err, char *out, size_t cap)
{
    static const struct {
        uint8_t bit;
        const char *name;
    } bits[] = {
        {RPP_ERR_RELAY_CLOSED, "a safety relay is closed"},
        {RPP_ERR_TRIP_PENDING, "a safety trip is pending"},
        {RPP_ERR_TOO_HOT, "temperature is above the update ceiling"},
        {RPP_ERR_HEADER_INVALID, "BEGIN header rejected"},
        {RPP_ERR_VERSION_INCOMPATIBLE, "protocol version incompatible"},
        {RPP_ERR_RETRANSMIT_CAP, "retransmission round cap reached"},
        {RPP_ERR_CRC_MISMATCH, "CRC mismatch on the written image"},
        {RPP_ERR_INTERNAL, "Pico-side internal error"},
    };
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (err == 0) {
        snprintf(out, cap, "none");
        return;
    }
    bool first = true;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (err & bits[i].bit) {
            size_t used = strlen(out);
            if (used + 1 >= cap) {
                break;
            }
            snprintf(out + used, cap - used, "%s%s", first ? "" : ", ", bits[i].name);
            first = false;
        }
    }
}

uint32_t rpp_chunk_count(uint32_t len)
{
    return (len + RPP_CHUNK_LEN - 1u) / RPP_CHUNK_LEN;
}

void rpp_describe_state(uint8_t state, uint8_t err, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    char bits[160];
    rpp_format_err_bits(err, bits, sizeof(bits));
    switch (state) {
    case RPP_STATE_REFUSED:
        if (err & RPP_ERR_TRIP_PENDING) {
            snprintf(out, cap,
                     "Pico refused the update: %s. Recovery cannot reset the trip: power-cycle the "
                     "board, then retry",
                     bits);
        } else if (err & (RPP_ERR_RELAY_CLOSED | RPP_ERR_TOO_HOT)) {
            snprintf(out, cap, "Pico refused the update: %s", bits);
        } else {
            snprintf(out, cap, "Pico refused the update (%s)", bits);
        }
        break;
    case RPP_STATE_FAILED:
        snprintf(out, cap, "Pico update failed: %s", bits);
        break;
    case RPP_STATE_ABORTED:
        snprintf(out, cap, "Pico aborted the update");
        break;
    case RPP_STATE_REJECTED_SLOT_LINKAGE:
        snprintf(out, cap, "Pico rejected the image: it is not linked for the slot it would write");
        break;
    case RPP_STATE_REFUSED_RUNNING_IMAGE_OVERLAP:
        snprintf(out, cap, "Pico refused: the target slot overlaps its running image");
        break;
    default:
        snprintf(out, cap, "Pico reported state %u (%s)", (unsigned)state, bits);
        break;
    }
}

void rpp_begin_start(rpp_begin_t *b, int64_t now_ms)
{
    memset(b, 0, sizeof(*b));
    b->sends = 1;
    b->last_send_ms = now_ms;
    b->first_send_ms = now_ms;
}

uint32_t rpp_data_pace_ms(bool bootloader)
{
    return bootloader ? RPP_DATA_PACE_MS : RPP_DATA_PACE_APP_MS;
}

rpp_begin_action_t rpp_begin_step(rpp_begin_t *b, const rpp_status_t *st, int64_t now_ms)
{
    if (st) {
        if (st->state == RPP_STATE_RECEIVING) {
            return RPP_BEGIN_RECEIVING;
        }
        if (st->state == RPP_STATE_ERASING) {
            b->idle_beacons = 0;
            return RPP_BEGIN_ERASING;
        }
        if (st->state == RPP_STATE_IDLE) {
            b->idle_beacons++;
        }
    }
    if (now_ms - b->first_send_ms >= (int64_t)RPP_ERASE_TIMEOUT_MS) {
        return RPP_BEGIN_FAIL;
    }
    if (b->idle_beacons >= RPP_BEGIN_IDLE_BEACONS &&
        now_ms - b->last_send_ms >= (int64_t)RPP_BEGIN_RESEND_MIN_MS) {
        if (b->sends >= RPP_BEGIN_MAX_SENDS) {
            return RPP_BEGIN_FAIL;
        }
        b->sends++;
        b->idle_beacons = 0;
        b->last_send_ms = now_ms;
        return RPP_BEGIN_RESEND;
    }
    return RPP_BEGIN_WAIT;
}

void rpp_fin_start(rpp_fin_t *f)
{
    memset(f, 0, sizeof(*f));
}

bool rpp_status_proves_bootloader(const rpp_status_t *st)
{
    return st->state == RPP_STATE_IDLE;
}

bool rpp_fin_status_beats_stop(const rpp_status_t *fresh)
{
    return fresh->state == RPP_STATE_COMPLETE;
}

bool rpp_fin_stop_is_unknown(const rpp_fin_t *f, bool pico_terminal)
{
    return f->end_sent_once && !pico_terminal;
}

void rpp_fin_end_sent(rpp_fin_t *f)
{
    f->end_outstanding = true;
    f->end_sent_once = true;
    f->end_sends++;
}

static rpp_fin_action_t fin_fail(rpp_fin_t *f, const char *why)
{
    f->why = why;
    return RPP_FIN_FAIL;
}

// The result was lost after END had (or may have) been accepted: the Pico can
// be COMPLETE, mid-verify or untouched, and the ESP cannot tell which. Never
// reported as FAILED and never followed by an ABORT (ABORT cannot revert a
// committed slot -- it is a no-op once the receiver is inactive -- so sending
// it would only let the report claim "aborted" for an image that may be live). Before any END
// was sent there is nothing that could have been accepted, so that case stays
// an ordinary failure with `fail_why`.
static rpp_fin_action_t fin_lost(rpp_fin_t *f, const char *fail_why)
{
    if (!f->end_sent_once) {
        return fin_fail(f, fail_why);
    }
    f->why = f->verifying_seen
                 ? "outcome unknown: the Pico reported verifying but its final result was lost - "
                   "power-cycle and check the Pico version; do NOT retry blindly"
                 : "outcome unknown: the Pico stopped answering after END - power-cycle and check "
                   "the Pico version; do NOT retry blindly";
    return RPP_FIN_UNKNOWN;
}

rpp_fin_action_t rpp_fin_step(rpp_fin_t *f, rpp_fin_event_t ev, const rpp_status_t *st,
                              uint32_t since_end_ms)
{
    if (ev == RPP_EV_QUIET || !st) {
        if (!f->end_outstanding) {
            // Receivers go silent once every chunk is in: silence means "send END".
            return f->end_sends >= RPP_MAX_END_SENDS ? fin_lost(f, "Pico stopped answering")
                                                     : RPP_FIN_SEND_END;
        }
        if (since_end_ms < RPP_END_REPLY_TIMEOUT_MS) {
            return RPP_FIN_WAIT;
        }
        if (f->end_sends >= RPP_MAX_END_SENDS) {
            return fin_lost(f, "Pico did not answer END");
        }
        return RPP_FIN_SEND_END;
    }
    switch (st->state) {
    case RPP_STATE_COMPLETE:
        return RPP_FIN_DONE;
    case RPP_STATE_FAILED:
    case RPP_STATE_ABORTED:
    case RPP_STATE_REFUSED:
    case RPP_STATE_REJECTED_SLOT_LINKAGE:
    case RPP_STATE_REFUSED_RUNNING_IMAGE_OVERLAP:
        return fin_fail(f, "Pico ended the update");
    case RPP_STATE_IDLE:
        // After END an IDLE beacon is ambiguous: the bootloader beacons IDLE
        // every second once its transfer ended, COMPLETE included
        // (recovery_update.c:435-437), so a dropped COMPLETE looks exactly
        // like a reset Pico.
        return fin_lost(f, "Pico left the transfer (reset?)");
    case RPP_STATE_VERIFYING:
        f->verifying_seen = true;
        f->restart_timer = true;
        f->end_outstanding = true;
        f->end_sends = 0;
        return RPP_FIN_WAIT;
    case RPP_STATE_RECEIVING:
        break;
    default: // ERASING etc.: keep waiting
        return RPP_FIN_WAIT;
    }
    if (f->batches >= RPP_MAX_GAP_BATCHES) {
        return fin_fail(f, "too many gap reports");
    }
    if (st->gap_count > 0) {
        f->batches++;
        if (st->received_chunks > f->last_received) {
            f->stalled = 0;
        } else {
            f->stalled++;
        }
        f->last_received = st->received_chunks;
        if (f->stalled >= RPP_MAX_RETRANSMIT_ROUNDS) {
            return fin_fail(f, "Pico did not accept the missing chunks (no progress)");
        }
        f->end_outstanding = false;
        f->end_sends = 0;
        return RPP_FIN_RETRANSMIT;
    }
    // RECEIVING with no listed gaps: either everything is in (and END is due)
    // or the status only lists gaps behind its cursor.
    if (f->end_outstanding) {
        // END reply may still be in flight -- but a receiver that keeps beaconing
        // without ever answering END must still hit the reply timeout.
        return rpp_fin_step(f, RPP_EV_QUIET, NULL, since_end_ms);
    }
    return RPP_FIN_SEND_END;
}

uint32_t rpp_pace_delay_ticks(uint32_t wait_us, uint32_t tick_us)
{
    if (wait_us == 0 || tick_us == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)wait_us + tick_us - 1u) / tick_us);
}

uint32_t rpp_pace_wait_us(int64_t now_us, int64_t next_send_us)
{
    if (now_us >= next_send_us) {
        return 0;
    }
    int64_t d = next_send_us - now_us;
    return d > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)d;
}
