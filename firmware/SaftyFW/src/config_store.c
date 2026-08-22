// config_store.c -- see config_store.h.
#include "config_store.h"

#include <string.h>

#include "crc32.h" // bootloader/ -- same CRC-32 used for metadata records

// --- Byte layout (little-endian, same convention as bootloader/metadata.c)
//
// Offset  Size  Field
//      0     4  magic
//      4     2  format_version
//      6     2  reserved0 (0)
//      8     4  seq
//     12     1  tc_type
//     13     1  calibration_missing (0/1)
//     14     2  reserved1 (0)
//     16    27  ct_cal: 3 channels x 9 B each (calibrated u8 + gain f32 LE +
//               offset f32 LE) -- see config_store.h's header comment on
//               config_store_ct_channel_cal_t for what these mean and why
//               `calibrated` must be its own explicit byte.
//     43    37  reserved (0xFF -- room for S8's threshold; the rest of what
//               used to be one 64-byte reserved block, before ct_cal above
//               claimed the first 27 bytes of it)
//     80   168  reserved, 0xFF-filled (further headroom)
//    248     4  record_crc32, over bytes [0, 248)
//    252     4  reserved, 0xFF-filled (pad to CONFIG_STORE_RECORD_LEN)
//    256  total = CONFIG_STORE_RECORD_LEN
//
// (CRC placed at 248 rather than 252 so the whole reserved region plus extra
// pad both stay contiguous and untouched by anything the CRC covers being
// relocated later -- see the budget check below.)
#define REC_OFF_MAGIC               0u
#define REC_OFF_FORMAT_VERSION      4u
#define REC_OFF_SEQ                 8u
#define REC_OFF_TC_TYPE             12u
#define REC_OFF_CALIBRATION_MISSING 13u
#define REC_OFF_CT_CAL              16u
#define REC_CT_CAL_CHANNEL_LEN      9u /* calibrated u8(1) + gain f32(4) + offset f32(4) */
#define REC_OFF_RESERVED \
    (REC_OFF_CT_CAL + CONFIG_STORE_CT_CAL_NUM_CHANNELS * REC_CT_CAL_CHANNEL_LEN) /* 43 */
#define REC_RESERVED_LEN            37u
#define REC_OFF_CRC                 248u

// Compile-time budget check, mirroring
// bootloader_metadata_record_budget_check: the fixed header plus ct_cal plus
// reserved room must fit inside the record before the CRC field -- if a
// future field addition breaks this, it must fail the build, not silently
// overrun into the CRC.
typedef char config_store_record_budget_check
    [(REC_OFF_RESERVED + REC_RESERVED_LEN <= REC_OFF_CRC) ? 1 : -1];

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

static uint16_t get_u16_le(const uint8_t *in)
{
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

// Float <-> 4-byte-LE via a union, same type-punning convention CommonFW's
// kilnlink_bytes.h uses for its f32 helpers -- this module does not depend
// on kilnlink, so it carries its own copy rather than reaching across a
// layer boundary for two small functions.
static void put_f32_le(uint8_t *out, float v)
{
    union {
        float    f;
        uint32_t u;
    } conv;
    conv.f = v;
    put_u32_le(out, conv.u);
}

static float get_f32_le(const uint8_t *in)
{
    union {
        float    f;
        uint32_t u;
    } conv;
    conv.u = get_u32_le(in);
    return conv.f;
}

void config_store_pack(const config_store_record_t *rec,
                        uint8_t out[CONFIG_STORE_RECORD_LEN])
{
    memset(out, 0xFF, CONFIG_STORE_RECORD_LEN); // matches erased-flash background
    put_u32_le(&out[REC_OFF_MAGIC], CONFIG_STORE_MAGIC);
    put_u16_le(&out[REC_OFF_FORMAT_VERSION], rec->format_version);
    put_u16_le(&out[6], 0); // reserved0
    put_u32_le(&out[REC_OFF_SEQ], rec->seq);
    out[REC_OFF_TC_TYPE] = rec->tc_type;
    out[REC_OFF_CALIBRATION_MISSING] = rec->calibration_missing ? 1u : 0u;
    put_u16_le(&out[14], 0); // reserved1
    for (unsigned ch = 0; ch < CONFIG_STORE_CT_CAL_NUM_CHANNELS; ch++) {
        size_t off = REC_OFF_CT_CAL + (size_t)ch * REC_CT_CAL_CHANNEL_LEN;
        out[off] = rec->ct_cal[ch].calibrated ? 1u : 0u;
        put_f32_le(&out[off + 1u], rec->ct_cal[ch].gain);
        put_f32_le(&out[off + 5u], rec->ct_cal[ch].offset);
    }
    memcpy(&out[REC_OFF_RESERVED], rec->reserved, sizeof(rec->reserved));
    // bytes [REC_OFF_RESERVED + REC_RESERVED_LEN, REC_OFF_CRC) already 0xFF
    // from the initial memset -- further headroom.
    uint32_t crc = bootloader_crc32(out, REC_OFF_CRC);
    put_u32_le(&out[REC_OFF_CRC], crc);
    // bytes [REC_OFF_CRC + 4, CONFIG_STORE_RECORD_LEN) already 0xFF -- pad.
}

bool config_store_unpack(const uint8_t in[CONFIG_STORE_RECORD_LEN],
                          config_store_record_t *out)
{
    if (in == NULL || out == NULL) {
        return false;
    }

    if (get_u32_le(&in[REC_OFF_MAGIC]) != CONFIG_STORE_MAGIC) {
        return false; // erased flash (0xFFFFFFFF) or garbage -- not this format
    }
    if (get_u16_le(&in[REC_OFF_FORMAT_VERSION]) != CONFIG_STORE_FORMAT_VERSION) {
        return false; // refuse the unrecognised rather than guess
    }

    uint32_t stored_crc = get_u32_le(&in[REC_OFF_CRC]);
    uint32_t computed_crc = bootloader_crc32(in, REC_OFF_CRC);
    if (stored_crc != computed_crc) {
        return false; // corrupted, or a torn write caught mid-program
    }

    // Well-formed -- only now do we touch *out.
    out->format_version = get_u16_le(&in[REC_OFF_FORMAT_VERSION]);
    out->seq = get_u32_le(&in[REC_OFF_SEQ]);
    out->tc_type = in[REC_OFF_TC_TYPE];
    out->calibration_missing = in[REC_OFF_CALIBRATION_MISSING] != 0u;
    for (unsigned ch = 0; ch < CONFIG_STORE_CT_CAL_NUM_CHANNELS; ch++) {
        size_t off = REC_OFF_CT_CAL + (size_t)ch * REC_CT_CAL_CHANNEL_LEN;
        // Explicit: only wire byte value 1 means calibrated. Any other byte
        // -- 0 (config_store_default()'s zero-init, and every record ever
        // written before this field existed) or 0xFF (erased flash) --
        // decodes as NOT calibrated. gain/offset are still copied through
        // even when calibrated is false; ct_amps_cal_apply() ignores them
        // in that case, but there is no reason to hide them from a caller
        // that wants to inspect a stale value. See config_store.h's header
        // comment on config_store_ct_channel_cal_t for why this must be an
        // explicit byte, never inferred from the numbers.
        out->ct_cal[ch].calibrated = (in[off] == 1u);
        out->ct_cal[ch].gain = get_f32_le(&in[off + 1u]);
        out->ct_cal[ch].offset = get_f32_le(&in[off + 5u]);
    }
    memcpy(out->reserved, &in[REC_OFF_RESERVED], sizeof(out->reserved));
    return true;
}

void config_store_default(config_store_record_t *out)
{
    memset(out, 0, sizeof(*out));
    out->format_version = CONFIG_STORE_FORMAT_VERSION;
    out->seq = 0;
    out->tc_type = CONFIG_STORE_DEFAULT_TC_TYPE;
    out->calibration_missing = true; // always true until a real commissioning
                                      // pass clears it -- see config_store.h
    // ct_cal: left at the memset(0) above -- calibrated == false on every
    // channel, gain/offset == 0 but IGNORED (never read) as a consequence.
    // This is the "uncalibrated is explicit" default: ct_amps_cal_apply()
    // treats calibrated == false as pass-the-raw-reading-through, not as
    // "gain 0, offset 0" -- see config_store.h's header comment.
}

size_t config_store_find_latest(const uint8_t sector[SAFTYFW_CONFIG_STORE_FLASH_SIZE],
                                 config_store_record_t *out_rec)
{
    if (sector == NULL || out_rec == NULL) {
        return CONFIG_STORE_NO_SLOT;
    }

    size_t best_slot = CONFIG_STORE_NO_SLOT;
    config_store_record_t best_rec;
    memset(&best_rec, 0, sizeof(best_rec));
    bool have_best = false;

    for (size_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        const uint8_t *rec_bytes = &sector[i * CONFIG_STORE_RECORD_LEN];
        config_store_record_t candidate;
        if (!config_store_unpack(rec_bytes, &candidate)) {
            continue; // erased or corrupt -- skip, not an error
        }
        if (!have_best || candidate.seq > best_rec.seq) {
            best_rec = candidate;
            best_slot = i;
            have_best = true;
        }
    }

    if (!have_best) {
        return CONFIG_STORE_NO_SLOT;
    }

    *out_rec = best_rec;
    return best_slot;
}

size_t config_store_next_write_slot(size_t latest_slot_index)
{
    if (latest_slot_index == CONFIG_STORE_NO_SLOT) {
        return 0;
    }
    size_t next = latest_slot_index + 1;
    if (next >= CONFIG_STORE_SLOTS_PER_SECTOR) {
        return 0;
    }
    return next;
}

bool config_store_next_write_needs_erase(size_t latest_slot_index)
{
    if (latest_slot_index == CONFIG_STORE_NO_SLOT) {
        return false; // never written -- slot 0 may already be erased, don't assume otherwise
    }
    return (latest_slot_index + 1) >= CONFIG_STORE_SLOTS_PER_SECTOR;
}

config_store_write_decision_t config_store_decide_write(bool armed)
{
    return armed ? CONFIG_STORE_WRITE_REFUSED_ARMED : CONFIG_STORE_WRITE_OK;
}

uint32_t config_store_record_crc(const config_store_record_t *rec)
{
    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(rec, packed);
    return get_u32_le(&packed[REC_OFF_CRC]);
}

const char *config_store_write_decision_reason(config_store_write_decision_t decision)
{
    switch (decision) {
        case CONFIG_STORE_WRITE_OK:
            return "ok";
        case CONFIG_STORE_WRITE_REFUSED_ARMED:
            return "refused: relay is ARMED, config writes are refused while ARMED";
        default:
            return "unknown";
    }
}

bool config_store_confirm_crc_ok(uint8_t config_version)
{
    // See config_store.h's header comment on this function for the full
    // reasoning -- version 0 is the one value a genuinely written, CRC-
    // verified record can never produce, so it is the only safe "not
    // confirmed" signal, covering both "never loaded" and "loaded but
    // nothing valid found" without needing to tell those two apart.
    return config_version != 0u;
}

const char *config_store_flash_rc_reason(int rc)
{
    switch (rc) {
        case CONFIG_STORE_FLASH_RC_OK:
            return "ok";
        case CONFIG_STORE_FLASH_RC_TIMEOUT:
            return "flash write failed: flash_safe_execute() timed out waiting for the "
                   "other core to answer the lockout request";
        case CONFIG_STORE_FLASH_RC_NOT_PERMITTED:
            return "flash write failed: flash_safe_execute() reports safe execution is "
                   "not possible (other core not initialised for lockout, or called from "
                   "an unsafe context)";
        case CONFIG_STORE_FLASH_RC_INSUFFICIENT_RESOURCES:
            return "flash write failed: flash_safe_execute()'s lockout handshake could "
                   "not allocate the resources it needed";
        default:
            return "flash write failed: flash_safe_execute() returned an unrecognised "
                   "error code";
    }
}
