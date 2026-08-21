// config_store.h -- the safety processor's versioned, CRC'd configuration
// record. TODO.md Phase 9's "Config store: versioned, CRC'd, safe defaults,
// calibration_missing flag" and "Config writes refused while ARMED".
//
// Deliberately the SAME discipline as bootloader/metadata.h: a small
// append-only log of fixed-size, magic+CRC'd records inside one flash
// sector, "scan every slot, keep the highest seq among those whose own CRC
// validates" to find the current record, safe defaults (never a hard
// failure) when nothing in the sector validates. See metadata.h's own header
// comment for why an append-only log, not "two copies alternately
// overwritten", is the only scheme that is actually safe on NOR flash within
// a single erase-granularity sector.
//
// Split the same way metadata.h/.c vs bootloader/main.c and
// src/tasks/update_task.c are split: this file (config_store.c) is pure --
// no RTOS, no pico-sdk, no direct flash I/O, operates only on caller-supplied
// buffers -- so it is host-testable exactly like metadata.c
// (test/test_config_store.c). The real flash reads/writes, the ARMED check
// against relay_owner, and the tc_type/calibration_missing getters main.c
// and future callers actually use live in config_store_flash.c, which is
// NOT host-tested for the same reason update_task.c and bootloader/main.c
// are not: it needs a real RP2040 (XIP-mapped reads, flash_safe_execute()).
//
// What the original pass built: the record format, the pure pack/unpack/
// find-latest/next-write-slot logic, the pure ARMED-refusal decision, and
// tc_type wired as the first real consumer (replacing max31856.h's
// MAX31856_TC_TYPE_PLACEHOLDER call site in main.c), plus SAFETY_CMD_
// SET_CONFIG (0x16) as the wire command that actually sets tc_type.
//
// A later pass (this one) adds `ct_cal`: three channels' worth of CT amps
// calibration constants (config_store_ct_channel_cal_t above), the wire
// commands SAFETY_CMD_SET_CT_CAL (0x19) and SAFETY_CMD_GET_CT_CAL/CT_CAL
// (0x1A) that set/read them (src/tasks/link_task.c), and current_sense.c's
// ct_amps_cal.h consumer -- closing the gap firmware/SimFW/tools/
// ct_calibration/README.md's "Readback path" section documented: a bench
// calibration run could compute per-channel constants but had no way to push
// them into SaftyFW's own flash. Still NOT implemented: S8's implausible-
// rate-of-rise threshold -- config_store_record_t's `reserved` bytes still
// hold room for it, but nothing reads or writes that room yet.
#ifndef SAFTYFW_CONFIG_STORE_H
#define SAFTYFW_CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash_layout.h" // bootloader/ -- SAFTYFW_CONFIG_STORE_FLASH_SIZE

#ifdef __cplusplus
extern "C" {
#endif

// One flash-programming page, and the record size -- same reasoning as
// BOOTLOADER_METADATA_RECORD_LEN (metadata.h): page-aligned writes are the
// simplest correct thing on NOR flash, and 256 B is far more than the fields
// below need, which is the point -- see `reserved` below.
#define CONFIG_STORE_RECORD_LEN 256u
#define CONFIG_STORE_SLOTS_PER_SECTOR \
    (SAFTYFW_CONFIG_STORE_FLASH_SIZE / CONFIG_STORE_RECORD_LEN) // 16

// Sentinel: no record slot chosen / nothing found -- same convention as
// BOOTLOADER_METADATA_NO_SLOT.
#define CONFIG_STORE_NO_SLOT ((size_t)-1)

// "KLC1" ("KiLn Config 1") packed little-endian, same convention as
// BOOTLOADER_METADATA_MAGIC's "KLN1" -- distinct from that magic and from
// any kilnlink/link_frame magic.
#define CONFIG_STORE_MAGIC 0x4B4C4331u
#define CONFIG_STORE_FORMAT_VERSION 1u

// Safe, documented default -- matches max31856.h's own doc comment on
// MAX31856_TC_TYPE_PLACEHOLDER: Type K is the "does not crash, does not
// claim precision it cannot have" placeholder, not a claim that K is correct
// for any given installation.
#define CONFIG_STORE_DEFAULT_TC_TYPE 0x03u // MAX31856_TC_TYPE_K -- duplicated
                                            // as a literal, not #included,
                                            // because max31856.h is an
                                            // application-layer driver header
                                            // and this module must stay as
                                            // dependency-free as metadata.h
                                            // is; config_store_flash.c (which
                                            // DOES know about max31856.h)
                                            // asserts the two agree.

// Number of current-sense channels a config record carries calibration for --
// must equal current_task.c's channel count (3, one per ADC0/1/2). Not
// #included from anywhere hardware-specific: this module stays as
// dependency-free as the rest of it.
#define CONFIG_STORE_CT_CAL_NUM_CHANNELS 3u

// One channel's CT amps calibration -- a linear correction applied on top of
// current_sense.c's own physics-based conversion (zero_counts/k_ct_v_per_a/
// gain), fit by firmware/SimFW/tools/ct_calibration/'s bench sweep-and-fit
// runner against THIS firmware's own reported current_a (SAFETY_CMD_
// GET_STATUS's current field, read back over the existing kilnctrl UART
// link -- see that tool's README, "Readback path").
//
// `calibrated == false` means "no bench run has ever set this channel" and
// `gain`/`offset` are then IGNORED ENTIRELY -- ct_amps_cal.c falls back to
// the compiled-in IDENTITY (the raw reading, unmodified), never to any
// number stored here. This must be an explicit flag, not a gain==1/
// offset==0 convention, for exactly the reason firmware/SimFW/src/sim/
// ct_calibration.h's header comment gives for its own `calibrated` bool: a
// zero-initialized struct (gain 0, offset 0) would otherwise silently
// force every reading on that channel to 0 A, and "nobody calibrated this"
// would be indistinguishable from "this channel was measured to need
// gain=0" -- the single worst way an uncommissioned channel could fail,
// since S3/S4/S9 read current_a to decide whether current is flowing where
// it should not be. A blank/corrupt/unreadable config record (see
// config_store_default()) and any record written before this field existed
// (config_store_default()'s memset leaves it exactly this way) both decode
// as calibrated == false on every channel -- see config_store_unpack()'s
// handling of the wire bytes for exactly why 0x00 AND 0xFF both mean "not
// calibrated", not just one of them.
//
// `gain`/`offset` are meant to be stored ALREADY INVERTED, the same way
// firmware/SimFW/tools/gen_ct_cal_table.py inverts its fit before writing a
// compiled table: firmware/SimFW/tools/ct_calibration/calibrate_ct.py fits
// `measured_a = fit_gain * commanded + fit_offset` (commanded = the
// fixture's known true amps, measured_a = this firmware's own reported
// current_a). To correct a RAW reading back to true amps, the PC-side
// sender of SAFETY_CMD_SET_CT_CAL must invert that fit before transmitting:
// `gain = 1 / fit_gain`, `offset = -fit_offset / fit_gain` -- so that
// ct_amps_cal_apply()'s `corrected = gain * raw + offset` undoes the
// measured error exactly. This module has no way to enforce that inversion
// happened; it only stores and applies whatever two floats arrive.
typedef struct {
    bool  calibrated;
    float gain;
    float offset;
} config_store_ct_channel_cal_t;

typedef struct {
    uint16_t format_version;
    uint32_t seq; // log sequence number; config_store_find_latest() keeps the highest
    uint8_t  tc_type; // one of MAX31856_TC_TYPE_* (max31856.h) -- this module
                       // does not validate the value is a recognised type;
                       // that is config_store_flash.c's job, same "this
                       // function does not decide what any field means"
                       // discipline as bootloader_metadata_pack().
    bool     calibration_missing; // true until a real commissioning pass
                                   // clears it. No clearing mechanism exists
                                   // yet (TODO.md Phase 9's later bullets) --
                                   // this field and its always-true-until-
                                   // cleared semantics is all this pass
                                   // builds. A blank/corrupt/unreadable store
                                   // must also read back as true here: see
                                   // config_store_default(). Untouched by
                                   // SAFETY_CMD_SET_CT_CAL -- CT calibration
                                   // and thermocouple commissioning are
                                   // separate concerns, see
                                   // link_task_handle_set_ct_cal()'s comment.
    config_store_ct_channel_cal_t ct_cal[CONFIG_STORE_CT_CAL_NUM_CHANNELS];
    // Reserved, unused, packed as 0xFF (matches the erased-flash background,
    // same convention as metadata.h's per-slot reserved bytes). Room for
    // S8's implausible-rate-of-rise threshold (TODO.md Phase 9's later
    // bullets) without moving anything else or changing the record size --
    // adding that field later is a struct/pack/unpack/host-test change, not
    // a layout change, same as metadata.h's own signature/sig_required
    // reservation. (ct_cal above used to live in this same reserved room;
    // this is the remainder after claiming CONFIG_STORE_CT_CAL_NUM_CHANNELS
    // * 9 = 27 of the original 64 bytes.)
    uint8_t  reserved[37];
} config_store_record_t;

// Compile-time budget check, mirroring bootloader/metadata.c's
// bootloader_metadata_record_budget_check: the fixed header + reserved room
// must fit inside CONFIG_STORE_RECORD_LEN with the trailing CRC still
// present. Enforced in config_store.c (needs the byte-offset macros defined
// there); declared here only as documentation that such a check exists.

// --- Record pack/unpack ---------------------------------------------------

// Packs `rec` into a CONFIG_STORE_RECORD_LEN-byte record, including the
// trailing CRC over everything before it. Pure byte layout, no validation of
// `rec`'s own field values.
void config_store_pack(const config_store_record_t *rec,
                        uint8_t out[CONFIG_STORE_RECORD_LEN]);

// Unpacks and validates a single record: magic, format_version, and the
// trailing CRC must all check out. Returns false (and leaves `*out`
// completely untouched) on ANY of: NULL args, wrong magic, unrecognised
// format_version, or a CRC mismatch -- an erased (all-0xFF) or torn-write
// record included, same as bootloader_metadata_unpack().
bool config_store_unpack(const uint8_t in[CONFIG_STORE_RECORD_LEN],
                          config_store_record_t *out);

// Fills `*out` with the safe, documented default record: format_version
// current, seq 0, tc_type CONFIG_STORE_DEFAULT_TC_TYPE (K), calibration_missing
// true, reserved bytes zeroed. This is what every caller must fall back to
// when the flash region has never held a valid record or its latest record
// is corrupt -- "a missing part must not abort boot"
// (max31856_configure()'s own doc comment) applies just as much to
// configuration as to a missing sensor.
void config_store_default(config_store_record_t *out);

// --- Sector scan -----------------------------------------------------------

// Scans all CONFIG_STORE_SLOTS_PER_SECTOR records in `sector` (exactly
// SAFTYFW_CONFIG_STORE_FLASH_SIZE bytes, as read straight from the XIP-mapped
// config region -- this function does no flash I/O itself), unpacks each,
// and keeps the one with the highest `seq` among those that unpack
// successfully. Corrupt or erased (all-0xFF) slots are silently skipped.
//
// Returns the winning slot's index, or CONFIG_STORE_NO_SLOT (leaving
// `*out_rec` untouched) if not a single slot in the sector holds a valid
// record. Callers that want "a real record, or safe defaults" rather than a
// slot index should check for CONFIG_STORE_NO_SLOT and call
// config_store_default() themselves (config_store_flash.c does exactly
// this) -- this function stays a pure, honest "what did I find", matching
// bootloader_metadata_find_latest()'s own contract.
size_t config_store_find_latest(const uint8_t sector[SAFTYFW_CONFIG_STORE_FLASH_SIZE],
                                 config_store_record_t *out_rec);

// Given the slot index config_store_find_latest() returned (or
// CONFIG_STORE_NO_SLOT), returns the index the NEXT write should target.
// Same wrap/never-written semantics as bootloader_metadata_next_write_slot().
size_t config_store_next_write_slot(size_t latest_slot_index);

// True iff the write slot config_store_next_write_slot() just returned
// requires a sector erase first. Same semantics as
// bootloader_metadata_next_write_needs_erase().
bool config_store_next_write_needs_erase(size_t latest_slot_index);

// --- Write-while-ARMED refusal ---------------------------------------------

typedef enum {
    CONFIG_STORE_WRITE_OK = 0,
    CONFIG_STORE_WRITE_REFUSED_ARMED, // TODO.md Phase 9: "Config writes refused while ARMED"
} config_store_write_decision_t;

// Pure decision: given whether the relay is currently ARMED (relay_owner's
// RELAY_OWNER_STATE_ARMED -- the caller reads that and passes the bool in,
// this module never includes relay_owner.h so it stays as dependency-free as
// metadata.c/safety_guards.c), decides whether a config write may proceed.
// A write is refused unconditionally while ARMED, regardless of what the
// write would change -- there is no "this field is harmless" carve-out,
// matching this codebase's general preference for a simple, honest rule
// over a permissive one that has to be reasoned about per field.
config_store_write_decision_t config_store_decide_write(bool armed);

// Human-readable reason for a config_store_write_decision_t, for surfacing
// over HTTP/PC UART the same way other refusal reasons in this codebase are
// surfaced as plain strings rather than caller-decoded enums.
const char *config_store_write_decision_reason(config_store_write_decision_t decision);

// --- Real flash glue (config_store_flash.c -- NOT host-tested, see that
// file's header comment) -------------------------------------------------

// Reads the config store's flash sector once and caches the result. Must be
// called early in main()'s boot sequence, before any of the getters below
// are relied on -- see config_store_flash.c.
void config_store_boot_load(void);

// The cached tc_type -- one of MAX31856_TC_TYPE_* (max31856.h). Safe to call
// from any task (single cached struct, written only by config_store_boot_load()
// and config_store_write(), both expected to run before/without concurrent
// readers in this build's current call pattern). Returns
// CONFIG_STORE_DEFAULT_TC_TYPE if called before config_store_boot_load().
uint8_t config_store_get_tc_type(void);

// The cached calibration_missing flag. Returns true (the safe default) if
// called before config_store_boot_load().
bool config_store_is_calibration_missing(void);

// Copies the cached record's CT calibration into `out[0..CONFIG_STORE_CT_
// CAL_NUM_CHANNELS-1]`. Same "safe default before boot_load()" contract as
// every other getter here: if called before config_store_boot_load(), every
// channel comes back calibrated == false (config_store_default()'s shape),
// never uninitialised memory. Used by current_task.c (boot-time load) and
// link_task_handle_set_ct_cal() (read-modify-write: fetch every channel's
// current constants before overwriting just the one SAFETY_CMD_SET_CT_CAL
// named, so setting channel 1 can never disturb channel 0 or 2's stored
// values).
void config_store_get_ct_cal(config_store_ct_channel_cal_t out[CONFIG_STORE_CT_CAL_NUM_CHANNELS]);

// Writes `rec` as the new current config record, refusing while ARMED (see
// config_store_decide_write()). Returns false and fills `*out_reason` (if
// non-NULL) with a human-readable explanation on refusal or flash failure.
// `rec->format_version`/`rec->seq` are overwritten internally -- callers
// only need to set tc_type/calibration_missing/reserved.
//
// Wired to SAFETY_CMD_SET_CONFIG (0x16, LINK_PROTOCOL.md sec 4) via
// link_task_handle_set_config() (src/tasks/link_task.c).
bool config_store_write(const config_store_record_t *rec, const char **out_reason);

// The cached record's `seq`, truncated to a u8 -- what
// SAFETY_CMD_FW_VERSION's `config_version` byte carries (LINK_PROTOCOL.md
// sec 4). Bumped by every accepted config_store_write(), so a GUI watching
// FW_VERSION can tell a commissioning write actually landed. Returns 0
// (config_store_default()'s own seq) if called before
// config_store_boot_load().
uint8_t config_store_get_config_version(void);

// A CRC over the cached record's active fields -- what
// SAFETY_CMD_FW_VERSION's `config_crc` field carries. The low 16 bits of
// config_store_record_crc() applied to the cached record; returns 0 if
// called before config_store_boot_load(). Pure/host-testable via
// config_store_record_crc() itself -- this getter only adds the cache read.
uint16_t config_store_get_config_crc(void);

// Pure: packs `rec` (config_store_pack()) and returns the CRC-32 that ends
// up in its trailing field. Exposed so callers that only want the
// checksum -- not a full pack/unpack round trip -- don't have to carry a
// scratch CONFIG_STORE_RECORD_LEN buffer themselves. Host-testable exactly
// like the rest of this file's pure logic.
uint32_t config_store_record_crc(const config_store_record_t *rec);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_CONFIG_STORE_H
