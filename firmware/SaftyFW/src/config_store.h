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
// --- Format version 2 (this pass) ------------------------------------------
//
// Version 1 held only `tc_type`, `calibration_missing` and `ct_cal[3]` in a
// 256 B record (16 slots/sector) -- nowhere near enough room for
// docs/CONFIG_REFERENCE.md sections 1-5's full commissioning surface. This
// pass:
//
//   - Grows CONFIG_STORE_RECORD_LEN 256 -> 512 B (8 slots/sector instead of
//     16 -- still ample, since a record is written only at commissioning,
//     not at runtime; docs/COMMISSIONING.md section 1.1).
//   - Adds every field in CONFIG_REFERENCE.md sections 1-5.
//   - Adds `fields_set`, a bitmask of CONFIG_STORE_SET_* flags marking which
//     of the handful of fields that have NO safe compiled-in default have
//     actually been commissioned. CONFIG_REFERENCE.md section 7 is explicit
//     that "no default may be a guess dressed as a value" -- for
//     `tc_source`, `borrowed_zone_index`, `tc_placement_mode`,
//     `abs_max_temp_c`, `ct_channel_map`, `max_rate_c_per_min` and
//     `mains_voltage_v`, every representable numeric value (including 0,
//     which is a perfectly plausible temperature or a deliberate "rate
//     limit off") is a value someone could have meant. A sentinel value
//     collides with that; a separate bit cannot, which is why this module
//     uses an explicit "is this set" bit per field rather than trying to
//     reserve a magic number out of each field's own range. Any consumer
//     that reads one of these fields MUST check the corresponding
//     CONFIG_STORE_SET_* bit before trusting the value; the raw bytes when
//     the bit is clear are documented in config_store_default()'s comment
//     but are not a promise -- only the bit is authoritative.
//     `max_rate_c_per_min` is a partial exception since 2026-09-05: the
//     compiled RECORD now carries CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN
//     (2x the fastest rising built-in-profile ramp) as its at-rest value
//     instead of a bare 0.0f. That does not make it an armed default,
//     though -- safety_core.c's safety_core_load_guard_cfg() (around lines
//     393-394) still forces the value the guard actually runs with to 0.0f
//     (S8 off) unless CONFIG_STORE_SET_MAX_RATE_C_PER_MIN has been set
//     through commissioning, so the "no safe compiled-in ARMED default"
//     rule this paragraph describes is otherwise unchanged for this field.
//   - Three-outcome load in config_store_unpack(), mirroring the discipline
//     KilnFW's NVS loaders use (see e.g. App/drivers/http/zones_http.c):
//       * stored format_version == CONFIG_STORE_FORMAT_VERSION (2): load
//         normally.
//       * stored format_version == 1 (the only older version that has ever
//         existed): migrate forward. `tc_type`, `calibration_missing`
//         (forced true regardless of what v1 held -- see below) and
//         `ct_cal[3]` are preserved; everything new is defaulted via
//         config_store_default(). `calibration_missing` STAYS true because a
//         migrated record was never commissioned against the new fields --
//         setting it false here would tell every guard added in this pass
//         "the operator confirmed these" when nobody ever did.
//       * stored format_version > CONFIG_STORE_FORMAT_VERSION (a future
//         record read by older/this firmware): REFUSE. Returns false, same
//         as a CRC failure, so the caller falls back to
//         config_store_default(). This is deliberately NOT "best effort
//         parse what we recognise": a v3 record's byte offsets are not
//         guaranteed to mean what v2 thinks they mean, so reinterpreting
//         them would produce a confident, plausible, WRONG threshold --
//         exactly the failure mode this outcome exists to prevent
//         (docs/COMMISSIONING.md section 1.1).
//   - Reserved tail still packed 0xFF (matching erased flash), so a later
//     field costs a pack/unpack change, not a layout move.
//
// What the original pass built: the record format, the pure pack/unpack/
// find-latest/next-write-slot logic, the pure ARMED-refusal decision, and
// tc_type wired as the first real consumer (replacing max31856.h's
// MAX31856_TC_TYPE_PLACEHOLDER call site in main.c), plus SAFETY_CMD_
// SET_CONFIG (0x16) as the wire command that actually sets tc_type.
//
// A later pass added `ct_cal`: three channels' worth of CT amps calibration
// constants, wired to SAFETY_CMD_SET_CT_CAL (0x19) / GET_CT_CAL (0x1A).
//
// This pass (v2) adds the record fields only -- staging/commit wire commands
// (`SET_PARAM`/`COMMIT_CONFIG`/`GET_PARAM`/`GET_CONFIG_PAGE`,
// docs/COMMISSIONING.md section 2) are a separate, later pass; nothing here
// wires a new getter for every new field, the same way v1's own landing did
// not wire getters for fields nothing consumed yet.
//
// --- 2026-08-24: `tc_type` joins the fields_set-gated set, for a DIFFERENT
// reason than the seven fields above ------------------------------------
//
// `tc_type` is not a "no safe default" field -- it keeps CONFIG_STORE_
// DEFAULT_TC_TYPE (K) exactly as before, and every read site
// (config_store_get_tc_type(), max31856_configure()'s caller) still gets a
// usable byte with no bit check required. What it lacked, until now, was any
// way to tell "an operator commissioned Type K" apart from "nobody has ever
// touched this, it defaulted to K" -- and those two states are NOT merely
// similar, they are byte-for-byte IDENTICAL in the record, which is a
// sharper problem than any of the seven no-safe-default fields have (an
// unset abs_max_temp_c at least reads back as the suspicious value 0.0).
// max31856_tc_range_policy.h's per-type plausibility band is the first real
// consumer that cares about this distinction: a genuinely commissioned type
// earns its own tight datasheet band, but asserting that same tight band
// against a value nobody ever confirmed is a check dressed as more precise
// than it can honestly be. CONFIG_STORE_SET_TC_TYPE exists so that consumer
// (and any future one) can tell the two states apart.
// config_params_all_required_set() below now includes CONFIG_STORE_SET_
// TC_TYPE in its required mask, for the same "surfaced, not silently
// permissive" reason the six/seven original fields are there: this is the
// existing, already-wired channel (calibration_missing -> DIAG's
// CALIBRATION_MISSING bit -> link_task.c) for telling an operator/GUI "this
// board has not been fully commissioned" -- not tc_type's own separate wire
// flag, because no such per-field flag exists on the wire and this pass adds
// none.
#ifndef SAFTYFW_CONFIG_STORE_H
#define SAFTYFW_CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash_layout.h" // bootloader/ -- SAFTYFW_CONFIG_STORE_FLASH_SIZE

#ifdef __cplusplus
extern "C" {
#endif

// One flash-programming page family, and the record size -- same reasoning
// as BOOTLOADER_METADATA_RECORD_LEN (metadata.h): page-aligned writes are
// the simplest correct thing on NOR flash. 512 B is enough for every field
// in CONFIG_REFERENCE.md sections 1-5 with ~300 B of reserved room to spare
// (see config_store.c's REC_OFF_* layout comment) -- see `reserved` below.
#define CONFIG_STORE_RECORD_LEN 512u
#define CONFIG_STORE_SLOTS_PER_SECTOR \
    (SAFTYFW_CONFIG_STORE_FLASH_SIZE / CONFIG_STORE_RECORD_LEN) // 8

// Sentinel: no record slot chosen / nothing found -- same convention as
// BOOTLOADER_METADATA_NO_SLOT.
#define CONFIG_STORE_NO_SLOT ((size_t)-1)

// "KLC1" ("KiLn Config 1") packed little-endian, same convention as
// BOOTLOADER_METADATA_MAGIC's "KLN1" -- distinct from that magic and from
// any kilnlink/link_frame magic. The magic does NOT change with
// format_version -- it identifies "this is a config_store record of some
// version", and format_version is what says which one.
#define CONFIG_STORE_MAGIC 0x4B4C4331u
#define CONFIG_STORE_FORMAT_VERSION 2u

// The one prior format version this module knows how to migrate FORWARD
// from. Not "the previous version" in a generic sense -- if a v3 ever
// exists, this module's job is still only "migrate v1, load v2, refuse
// anything newer than v2"; a hypothetical future pass that bumps
// CONFIG_STORE_FORMAT_VERSION again also grows this migration chain, it does
// not just redefine this macro.
#define CONFIG_STORE_FORMAT_VERSION_V1 1u

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

// S8 sanity-rate guard code default -- 2x the fastest RISING ramp_c_per_hr
// found across all 28 KilnFW built-in profiles (firmware/KilnFW/App/drivers/
// profiles_builtin_table.inc). S8 (safety_guards.c) is a strictly rising-rate
// guard -- it compares the signed temperature delta against the threshold,
// so it can only ever trip while the setpoint is climbing, never while it is
// falling. That means only segments whose target_c is ABOVE the previous
// segment's target_c (or above the start temperature, for a profile's first
// segment) are legitimate basis candidates; a segment's declared
// ramp_c_per_hr is the same field regardless of direction, so a fast COOLING
// segment (target_c dropping) must be excluded even though its number is
// large. The four tied 9999.0 C/hr segments (e.g. "FSCGCL" -- Shimbo Crystal
// Celestite Schedule) are all crash-COOL segments (target_c below the
// previous segment's), so none of them qualify. The fastest RISING segments
// across all 28 profiles are two tied 999.0 C/hr steps in "FSCGB1" (Shimbo
// Crystal Holding Pattern 2), both part of its holding-pattern dwell
// cycling: 1050 C to 1075 C, and 1075 C to 1100 C -- so 999.0 / 60 * 2 =
// 33.3 C/min. Unlike CONFIG_STORE_DEFAULT_TC_TYPE
// this is NOT a "safe, documented default" in the same sense -- it is
// deliberately permissive (every shipped profile's actual rising ramp, at
// 2x, cannot trip it), decided 2026-09-05 to replace the previous "ships
// disabled" (0.0f) default. Still overridable through the normal
// commissioning path (0x0204 / CONFIG_STORE_SET_MAX_RATE_C_PER_MIN); a bench
// rig with a real measured max heating rate should commission its own
// tighter value rather than rely on this one.
#define CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN 33.3f

// Highest tc_type/borrowed_type_expected byte that is a real, linearized
// MAX31856 thermocouple type (MAX31856_TC_TYPE_T) -- duplicated as a literal
// for the same dependency-free reason CONFIG_STORE_DEFAULT_TC_TYPE's comment
// gives; config_store_flash.c asserts the two agree. Bytes above this select
// the part's Voltage Mode (CR1 TC TYPE[3:0] 0x08-0x0F, datasheet page 20's
// field table) rather than a linearized temperature -- see
// max31856_tc_type_policy.h's header comment for why max31856_configure()
// refuses them outright. config_store_unpack() (config_store.c) clamps any
// out-of-range byte it decodes from a CRC-valid record back to
// CONFIG_STORE_DEFAULT_TC_TYPE using this bound, as a defense-in-depth
// backstop independent of whatever wrote the record: today's two writers
// (link_task.c's SAFETY_CMD_SET_CONFIG, config_params.c's SET_PARAM/
// COMMIT_CONFIG for param 0x0105/0x0210) already bound the byte themselves
// before it reaches flash, but a CRC-valid record whose tc_type byte was
// garbled by something that still happens to leave the CRC intact, or a
// future writer that forgets to validate, must not be able to hand
// max31856_configure() a voltage-mode code just because the record it came
// from otherwise checks out.
#define CONFIG_STORE_TC_TYPE_MAX_REAL 0x07u // MAX31856_TC_TYPE_T

// Number of current-sense channels a config record carries calibration for --
// must equal current_task.c's channel count (3, one per ADC0/1/2). Not
// #included from anywhere hardware-specific: this module stays as
// dependency-free as the rest of it.
#define CONFIG_STORE_CT_CAL_NUM_CHANNELS 3u

// --- tc_source (CONFIG_REFERENCE.md section 1) ------------------------------
#define CONFIG_STORE_TC_SOURCE_OWN_J7        0u
#define CONFIG_STORE_TC_SOURCE_BORROWED_ZONE 1u
#define CONFIG_STORE_TC_SOURCE_BOTH          2u

// --- tc_placement_mode (CONFIG_REFERENCE.md section 1) ----------------------
// "Forced to CHAMBER_AGREED when tc_source is BORROWED_ZONE" and "rejected
// (not silently reconciled) if it contradicts tc_source" are both
// cross-field validation rules -- CONFIG_REFERENCE.md section 1 and
// docs/COMMISSIONING.md section 2 place that check at COMMIT_CONFIG, a later
// pass's wire command. This module stores whatever combination is asked of
// it; it does not itself referee tc_source vs tc_placement_mode.
#define CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED    0u
#define CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT 1u

// --- fields_set bits ---------------------------------------------------------
//
// One bit per field in CONFIG_REFERENCE.md section 1 (plus max_rate_c_per_min
// and mains_voltage_v from sections 2/3) that ships with NO compiled-in
// default. See this file's header comment above for why a bit, not a
// sentinel value, is the only correct way to represent "unset" for a field
// whose entire numeric range is plausible.
//
// ct_channel_map is one bit covering all CONFIG_STORE_CT_CAL_NUM_CHANNELS
// entries, not one bit per channel: CONFIG_REFERENCE.md section 1 describes
// it as confirmed by a single "one-relay-at-a-time" commissioning pass
// (CURRENT_SENSE.md section 5 step 2) that either has been done or has not --
// there is no meaningful "channel 0 confirmed, channel 1 not" partial state
// this store needs to represent as far as any GUARD is concerned.
//
// The wire, however, addresses each of the three channels as its OWN
// param_id (docs/COMMISSIONING.md section 2.1: 0x0106-0x0108), so SET_PARAM
// necessarily stages them one at a time. CONFIG_STORE_SET_CT_CHANNEL_MAP_0/
// _1/_2 below are that per-channel bookkeeping -- set by config_params.c's
// SET_PARAM handling as each channel arrives, never exposed on the wire and
// never consulted by any guard directly. CONFIG_STORE_SET_CT_CHANNEL_MAP
// itself (the bit guards and config_params_all_required_set() actually
// check) is DERIVED, not set directly by SET_PARAM: config_params.c's
// COMMIT_CONFIG handling sets it only once all three per-channel bits are
// present, so committing with only two of three channels staged leaves it
// unset (and therefore calibration_missing stays true) -- a half-populated
// map is exactly the "S3/S4 correlate against a channel nobody confirmed"
// failure CONFIG_REFERENCE.md section 1 warns is worse than no guard at
// all, since it "trips on healthy firings and stays quiet on the failure it
// exists to catch." Like every other fields_set bit, all four of these are
// monotonic -- a commit that fails to newly confirm the group does not
// un-confirm one a prior commit already did.
#define CONFIG_STORE_SET_TC_SOURCE           (1u << 0)
#define CONFIG_STORE_SET_BORROWED_ZONE_INDEX (1u << 1)
#define CONFIG_STORE_SET_TC_PLACEMENT_MODE   (1u << 2)
#define CONFIG_STORE_SET_ABS_MAX_TEMP_C      (1u << 3)
#define CONFIG_STORE_SET_CT_CHANNEL_MAP      (1u << 4)
#define CONFIG_STORE_SET_MAX_RATE_C_PER_MIN  (1u << 5)
#define CONFIG_STORE_SET_MAINS_VOLTAGE_V     (1u << 6)
#define CONFIG_STORE_SET_CT_CHANNEL_MAP_0    (1u << 7)
#define CONFIG_STORE_SET_CT_CHANNEL_MAP_1    (1u << 8)
#define CONFIG_STORE_SET_CT_CHANNEL_MAP_2    (1u << 9)

// tc_type -- added 2026-08-24, NOT for the "no safe default" reason the nine
// bits above exist. tc_type already has a real, safe compiled default (K,
// CONFIG_STORE_DEFAULT_TC_TYPE) and every existing reader (config_store_get_
// tc_type(), max31856_configure()) keeps working exactly as before whether
// or not this bit is set. What this bit exists for: a commissioned Type K
// and a never-touched, defaulted-to-K record are the SAME byte in the
// record, at every layer below this one -- there is no sentinel value to
// distinguish them the way abs_max_temp_c == 0 distinguishes "unset" (a
// plausible temperature IS 0, but at least it is a suspicious one; K is not
// a suspicious tc_type, it is the single most common real answer). The one
// consumer that cares about the distinction today is
// max31856_tc_range_policy.h's per-type plausibility band (see that file's
// header comment): a genuinely commissioned type earns its own tight
// datasheet band; an uncommissioned one gets only the widest band across all
// eight types, because asserting a tight band against a value nobody ever
// confirmed is precision this check cannot honestly claim. See this file's
// header comment above ("2026-08-24: tc_type joins...") for the full
// argument, including why config_params_all_required_set() below folds this
// bit into `calibration_missing` rather than inventing a second, tc_type-
// specific wire-visible flag.
#define CONFIG_STORE_SET_TC_TYPE             (1u << 10)

// max_expected_power_w -- added ROADMAP.md M12 "maximum expected kiln power"
// pass. Gated for the SAME reason tc_type is (config_store.h header comment
// above), not the "no safe default" reason the first ten bits exist: there is
// no guard reading this field today (it is a sanity/plausibility input for
// the commissioning UI only -- breakers are assumed sized for full load at
// 100% duty, this is NOT a derating input), so a stale/garbage byte at this
// offset in a record written before this field existed cannot silently
// mis-trip anything. It is still bit-gated rather than trusted unconditionally
// because 0 W (and the NaN/Inf garbage an old record's now-repurposed reserved
// bytes could otherwise decode as) is a nonsensical value for a real kiln, and
// this codebase's convention (config_store.h's own header comment) is that an
// old record's bytes at a newly-added offset are never trustworthy without an
// explicit bit saying an operator actually wrote them.
#define CONFIG_STORE_SET_MAX_EXPECTED_POWER_W (1u << 11)

// i_normal_a[0..2] -- COMMISSIONING_UX.md section 3.3, S14 over-current
// guard. THREE separate bits, deliberately NOT one group bit like
// CT_CHANNEL_MAP -- see that field's own comment and i_normal_a's struct
// comment above for why the two cases are opposite: a half-populated set of
// normals is a strictly correct partial result (the guard is honestly
// active on the channels measured so far), not an unsafe subset like a
// half-populated CT map would be.
#define CONFIG_STORE_SET_I_NORMAL_A_0         (1u << 12)
#define CONFIG_STORE_SET_I_NORMAL_A_1         (1u << 13)
#define CONFIG_STORE_SET_I_NORMAL_A_2         (1u << 14)

// ct_installed -- ROADMAP.md M12 "CTs are optional hardware" pass. ASKED
// (COMMISSIONING_UX.md section 3): required by config_params_all_required_set()
// like the no-safe-default fields, but for a different reason than either
// group above. ct_installed HAS a safe compiled default (1, installed) and
// every guard keeps working unchanged at that default -- what it does not
// have is a way for the firmware to observe the truth. Silently defaulting
// to "installed" would leave every CT-less board permanently uncommissionable
// (the state this pass exists to fix); silently defaulting to "not installed"
// would disarm S3/S9/S14 on a board that really does have CTs, which is far
// worse. So the question must be ANSWERED, not defaulted, and the bit is what
// records that an answer was given.
//
// THIS IS THE LAST FREE BIT of the uint16_t fields_set. A future field needs
// fields_set widened to uint32_t, which is a record-layout change
// (REC_OFF_FIELDS_SET is 2 bytes) and therefore a format_version bump --
// not a drop-in the way bits 11-15 were.
#define CONFIG_STORE_SET_CT_INSTALLED         (1u << 15)

// True iff every bit in `mask` (some OR of CONFIG_STORE_SET_* above) is set
// in `rec->fields_set`. Small enough to inline; exists so call sites read as
// "is X commissioned" rather than repeating the `& / ==` bit-test idiom
// everywhere a caller wants to check one of these fields.
static inline bool config_store_field_is_set(const void *fields_set_ptr, uint16_t mask)
{
    // Takes the bitmask by pointer-to-uint16_t-castable-int rather than by
    // value so callers can write config_store_field_is_set(&rec.fields_set,
    // CONFIG_STORE_SET_TC_SOURCE) or, just as easily,
    // config_store_field_is_set((void*)(uintptr_t)rec.fields_set, ...) --
    // but the simplest and intended usage is the boolean expression form
    // below, kept for direct field reads without indirection:
    //   (rec.fields_set & CONFIG_STORE_SET_TC_SOURCE) != 0
    // This helper exists only for symmetry with future call sites that pass
    // the mask around; most code in this codebase uses the inline `&`
    // expression directly, which is why this helper is not exercised by its
    // own dedicated unit test -- it is a trivial, header-only convenience
    // wrapper over that same expression, not independent logic.
    uint16_t fields_set = *(const uint16_t *)fields_set_ptr;
    return (fields_set & mask) == mask;
}

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

    // Bitmask of CONFIG_STORE_SET_* -- see the block comment above. MUST be
    // checked before trusting any of the seven fields it covers; the raw
    // bytes underneath an unset bit are whatever config_store_default() or a
    // migrated v1 record happened to leave there (typically 0), never a
    // value to act on.
    uint16_t fields_set;

    // --- CONFIG_REFERENCE.md section 1: commissioning ----------------------
    uint8_t  tc_source;           // CONFIG_STORE_TC_SOURCE_*; gated by _SET_TC_SOURCE
    uint8_t  borrowed_zone_index; // 0-2; gated by _SET_BORROWED_ZONE_INDEX
    uint8_t  tc_placement_mode;   // CONFIG_STORE_TC_PLACEMENT_*; gated by _SET_TC_PLACEMENT_MODE
    float    abs_max_temp_c;      // degC; gated by _SET_ABS_MAX_TEMP_C
    uint8_t  tc_type;             // one of MAX31856_TC_TYPE_* (max31856.h) --
                                   // this module does not validate the value
                                   // is a recognised type; that is
                                   // config_store_flash.c's job. STILL keeps
                                   // its v1 compiled default
                                   // (CONFIG_STORE_DEFAULT_TC_TYPE) -- every
                                   // existing reader gets a usable byte
                                   // whether or not commissioned -- but,
                                   // as of 2026-08-24, ALSO gated by
                                   // CONFIG_STORE_SET_TC_TYPE (see that
                                   // bit's own comment above for why a
                                   // compiled-default field still needed a
                                   // bit): a consumer that needs to tell
                                   // "operator commissioned K" apart from
                                   // "nobody ever touched this, it defaulted
                                   // to K" MUST check that bit; a consumer
                                   // that only wants a plausible byte to
                                   // hand max31856_configure() may keep
                                   // reading this field unconditionally, as
                                   // every call site already does.
    uint8_t  ct_channel_map[3];   // zone/relay id watched by each CT channel;
                                   // gated by _SET_CT_CHANNEL_MAP as a whole
    uint8_t  ct_installed;        // 0/1, param 0x0109 -- "are current
                                   // transformers physically fitted to this
                                   // board." Modelled on safety_tc_installed
                                   // above (same 0/1 shape, same "unknown
                                   // decodes to the strict state" marker
                                   // encoding in config_store.c) with ONE
                                   // deliberate difference: this one IS
                                   // fields_set-gated (_SET_CT_INSTALLED),
                                   // because COMMISSIONING_UX.md classifies
                                   // it ASKED -- whether a CT is bolted
                                   // around a wire is a hardware fact only
                                   // the person commissioning the board
                                   // knows, and neither answer is safe to
                                   // assume. The VALUE's default is 1
                                   // (installed = strict: ct_channel_map
                                   // stays required, every CT-fed guard
                                   // stays armed), so a record that never
                                   // answers behaves exactly as builds
                                   // before this field did.
                                   //
                                   // Only an EXPLICIT 0 (bit set AND value
                                   // 0) disables the CT-fed guards. See
                                   // safety_guard_input_t::current_sensing_
                                   // disabled for what "disabled" does to
                                   // S3/S4/S9/S14, and what it deliberately
                                   // does NOT do to S6b.
    uint8_t  safety_tc_installed; // 0/1, param 0x0211 -- "is the Pico's own
                                   // safety thermocouple physically wired
                                   // up." Default 1 (installed): the safe
                                   // default is "I expect a sensor and I
                                   // will trip if it is missing", not the
                                   // reverse. NOT fields_set-gated -- see
                                   // config_params.c's 0x0211 comment for
                                   // why 1 is a real, safe compiled default
                                   // rather than an unset-until-commissioned
                                   // flag. When 0, S5 (safety_guards.c)
                                   // downgrades a persistent bad-read streak
                                   // from TRIP to a permanent WARN (never
                                   // auto-promoted), and safety_core_request_
                                   // enable() unconditionally refuses the ON
                                   // direction -- the trade documented on
                                   // that function: S5 is only allowed to
                                   // stop trying to protect a sensor that
                                   // does not exist because the enable path
                                   // is the one refusing unconditionally
                                   // instead.
    bool     calibration_missing; // true until a real commissioning pass
                                   // clears it. A blank/corrupt/unreadable
                                   // store, and any record migrated forward
                                   // from v1, must also read back as true
                                   // here -- see config_store_default() and
                                   // config_store_unpack()'s v1 migration
                                   // path.

    // --- CONFIG_REFERENCE.md section 2: temperature guards ------------------
    float    firing_margin_c;         // degC, S1
    float    overshoot_margin_c;      // degC, S2
    uint32_t overshoot_time_s;        // s, S2
    float    max_rate_c_per_min;      // degC/min, S8; gated by _SET_MAX_RATE_C_PER_MIN --
                                       // "0 = off" in CONFIG_REFERENCE.md's
                                       // table is the FUNCTIONAL behaviour
                                       // (S8 stays disabled), but section 7
                                       // is explicit this ships genuinely
                                       // UNSET, not merely zero, so a GUI can
                                       // tell "nobody has looked at this yet"
                                       // apart from "someone measured and
                                       // deliberately left it off".
    uint32_t rate_window_s;           // s, S8
    uint32_t blind_grace_s;           // s, S5
    uint32_t frozen_window_s;         // s, S11
    float    tc_disagreement_c;       // degC, S10
    uint32_t tc_disagreement_time_s;  // s, S10
    float    tc_expected_offset_c;    // degC, S10
    float    cj_warn_c;               // degC, S12
    float    cj_max_c;                // degC, S12
    uint32_t cj_time_s;               // s, S12
    uint32_t borrowed_stale_s;        // s, S13
    uint32_t borrowed_stale_trip_s;   // s, S13
    uint8_t  borrowed_type_expected;  // MAX31856_TC_TYPE_*, S13 -- no
                                      // documented default in
                                      // CONFIG_REFERENCE.md beyond "the type
                                      // the borrowed channel is expected to
                                      // report"; reuses tc_type's own
                                      // documented K placeholder rather than
                                      // inventing a second one, and is NOT
                                      // one of the fields_set-gated fields:
                                      // CONFIG_REFERENCE.md section 1 does
                                      // not list it among the four/six
                                      // no-safe-default fields, and S13 is
                                      // itself only armed in BORROWED_ZONE/
                                      // BOTH mode (also fields_set-gated via
                                      // tc_source), so an uncommissioned
                                      // borrowed_type_expected cannot arm a
                                      // guard on its own.

    // --- CONFIG_REFERENCE.md section 3: current channels --------------------
    float    i_present_a;             // A, S3/S4/S9
    uint16_t zero_counts[3];          // ADC counts, S3/S4/S9 -- "measured",
                                       // re-measured at runtime after idle;
                                       // 0 is the documented placeholder,
                                       // same convention as k_ct_v_per_a
                                       // below, not fields_set-gated because
                                       // CONFIG_REFERENCE.md section 1 does
                                       // not list it among the no-safe-
                                       // default fields (it is expected to
                                       // be re-measured routinely, not set
                                       // once at commissioning).
    uint32_t correlation_window_s;    // s, S3/S4
    uint32_t stuck_on_time_s;         // s, S3
    uint32_t trip_verify_s;           // s, S9
    float    k_ct_v_per_a[3];         // V/A, power estimate only -- no
                                       // documented default ("--")
    float    gain[3];                 // dimensionless, R46/R43 trim
    float    mains_voltage_v;         // V; gated by _SET_MAINS_VOLTAGE_V --
                                       // same "0 is plausible, ships
                                       // genuinely unset" reasoning as
                                       // max_rate_c_per_min above
    uint32_t power_window_s;          // s

    // --- CONFIG_REFERENCE.md section 4: link and liveness --------------------
    uint32_t context_max_age_s;       // s, S2/S3/S4
    uint32_t link_timeout_s;          // s, S6b
    uint32_t link_dead_hard_s;        // s, S6b
    uint32_t mainfault_debounce_ms;   // ms, S6a
    uint32_t telemetry_period_ms;     // ms

    // --- CONFIG_REFERENCE.md section 5: timing and system ---------------------
    uint32_t startup_grace_s;         // s
    uint32_t estop_debounce_ms;       // ms
    uint32_t watchdog_timeout_ms;     // ms
    uint32_t config_check_period_s;   // s

    config_store_ct_channel_cal_t ct_cal[CONFIG_STORE_CT_CAL_NUM_CHANNELS];

    // Sanity/plausibility input only, ROADMAP.md M12 "maximum expected kiln
    // power" -- the operator's own estimate of the kiln's expected power draw
    // in watts, used only by the KilnFW commissioning UI to sanity-check
    // itself against wiring/breaker assumptions. Breakers are assumed sized
    // for full load at 100% duty per the owner's own framing; this is NOT a
    // derating input and NO SaftyFW guard trips on it -- it is captured and
    // persisted here only so it round-trips through GET_PARAM/GET_CONFIG_PAGE
    // like every other commissioning field. Gated by
    // CONFIG_STORE_SET_MAX_EXPECTED_POWER_W -- see that bit's own comment
    // above for why a "no guard reads this" field still needs the gate.
    float    max_expected_power_w;    // W; gated by _SET_MAX_EXPECTED_POWER_W

    // --- COMMISSIONING_UX.md section 3.3: S14 over-current guard (NEW) ------
    // i_normal_a[ch]: A, the per-channel current measured while that
    // channel's zone was the only one energized (ROADMAP M12's per-zone
    // measurement, zones_http.c). Individually fields_set-gated --
    // CONFIG_STORE_SET_I_NORMAL_A_0/_1/_2, deliberately THREE separate bits
    // rather than one group bit like CT_CHANNEL_MAP: a half-populated set of
    // normals is a strictly correct partial result (S14 active on the
    // channels measured so far, inactive on the rest), unlike a
    // half-populated CT map (which would be a guard watching a channel
    // nobody confirmed). 0.0f with the bit clear means "never measured" --
    // S14 must skip that channel entirely, never treat 0 as a real normal.
    float    i_normal_a[3];
    // overcurrent_pct/overcurrent_time_s: NOT fields_set-gated -- both have
    // real, documented defaults (150%, 30s) and follow the same "0 means
    // not configured, substitute the default" convention as gain[]/
    // power_window_s above, not the no-safe-default convention i_normal_a
    // uses.
    uint16_t overcurrent_pct;         // %, 0 -> 150 default
    uint32_t overcurrent_time_s;      // s, 0 -> 30 default

    // Reserved, unused, packed as 0xFF (matches the erased-flash background,
    // same convention as metadata.h's per-slot reserved bytes). ~276 B of
    // headroom (config_store.c's REC_OFF_RESERVED..REC_OFF_CRC; one byte of
    // the original 300 was carved off the FRONT of this block for
    // safety_tc_installed, 4 more for max_expected_power_w, and 16 more
    // (3xF32 i_normal_a + U16 overcurrent_pct + U32-on-wire-but-U16-tagged
    // overcurrent_time_s) for S14, and 1 more for ct_installed -- see REC_OFF_SAFETY_TC_INSTALLED /
    // REC_OFF_MAX_EXPECTED_POWER_W / REC_OFF_I_NORMAL_A in config_store.c)
    // -- adding a field later is a struct/pack/unpack/host-test change, not
    // a layout change, same as metadata.h's own signature/sig_required
    // reservation.
    uint8_t  reserved[276];
} config_store_record_t;

// Compile-time budget check, mirroring bootloader/metadata.c's
// bootloader_metadata_record_budget_check: the fixed header + every field +
// reserved room must fit inside CONFIG_STORE_RECORD_LEN with the trailing
// CRC still present. Enforced in config_store.c (needs the byte-offset
// macros defined there); declared here only as documentation that such a
// check exists.

// --- Load-time rejection diagnostics (2026-08-27 fail-open fix) -----------
//
// config_store_unpack()/config_store_find_latest() both collapse two VERY
// different inputs into the same `false`/CONFIG_STORE_NO_SLOT return:
//   1. "nothing here" -- bad magic, a CRC mismatch, or an unrecognised
//      format_version. This is the ordinary shape of a fresh, never-
//      committed board (an erased sector reads back as all-0xFF, which is
//      exactly a bad-magic failure) and is not, by itself, news.
//   2. "something here, and it's wrong" -- magic/CRC/format_version all
//      checked out (the bytes are provably intact) but
//      config_params_validate_ranges() refused a field's VALUE. This can
//      only happen to a record that was actually written and committed at
//      some point -- by this build, an older build, or bit rot that
//      happened to preserve the CRC-32 -- and it is the one case where
//      falling back to config_store_default() is a DOWNGRADE from what the
//      board was supposed to be running, not a neutral "nothing to load
//      yet." abs_max_temp_c defaults to 0.0f, which safety_guards.h
//      documents as "0 = not commissioned, guard never trips" -- silently
//      substituting that for a real, previously-committed ceiling is a
//      fail-OPEN of S1, the primary absolute overtemperature guard, and it
//      must never look the same in a log (or to an operator) as a board
//      that was simply never commissioned.
//
// This struct is how case 2 is reported back out of the _ex() variants
// below, so config_store_flash.c's config_store_boot_load() can (a) log the
// specific field/rule/seq a human needs to debug this from a log line alone,
// and (b) keep the board from claiming "commissioned" -- see
// config_store_get_config_crc()'s own comment for how that claim is denied.
typedef struct {
    bool        rejected; // true iff a magic+CRC(+format_version)-valid
                           // record was found but config_params_validate_
                           // ranges() refused one of its fields -- case 2
                           // above. Left false (with the rest of this struct
                           // zeroed) for case 1 -- an ordinary "nothing valid
                           // here", ordinary for a fresh board and not worth
                           // a log line.
    const char *field;    // config_params_validate_ranges()'s out_field for
                           // the rejected record -- a static string, valid
                           // only when rejected == true.
    const char *rule;     // config_params_validate_ranges()'s out_rule --
                           // same validity contract as `field`.
    uint32_t    seq;      // the rejected record's own seq, so the log line
                           // names WHICH commit was refused. Also used by
                           // config_store_find_latest_ex() to prefer the
                           // highest-seq rejection when more than one slot in
                           // the sector fails validation, matching the
                           // "highest seq wins" rule the valid-record path
                           // already follows.
} config_store_reject_info_t;

// --- Record pack/unpack ---------------------------------------------------

// Packs `rec` into a CONFIG_STORE_RECORD_LEN-byte record, including the
// trailing CRC over everything before it. Pure byte layout, no validation of
// `rec`'s own field values. Always writes the CURRENT (v2) layout -- there
// is no "pack as v1" entry point; v1-shaped bytes are only ever produced by
// test code that needs to exercise the migration path (see
// test_config_store.c) or by real flash written before this pass shipped.
void config_store_pack(const config_store_record_t *rec,
                        uint8_t out[CONFIG_STORE_RECORD_LEN]);

// Unpacks and validates a single record, with the three-outcome load this
// file's header comment describes:
//   - format_version == CONFIG_STORE_FORMAT_VERSION (2) and CRC valid: loads
//     every v2 field, returns true.
//   - format_version == CONFIG_STORE_FORMAT_VERSION_V1 (1) and its (legacy,
//     shorter) CRC valid: migrates forward -- `tc_type`, `ct_cal[3]` and
//     `seq` are preserved from the v1 bytes, everything else comes from
//     config_store_default(), `calibration_missing` is forced true
//     regardless of what the v1 record held, and `format_version` in `*out`
//     reads back as 2 (the record is now v2-shaped in RAM). Returns true.
//   - Anything else -- wrong magic, an unrecognised format_version (0,
//     anything > CONFIG_STORE_FORMAT_VERSION, or any value that is neither
//     1 nor 2), or a CRC mismatch at whichever layout `format_version`
//     implies -- returns false and leaves `*out` COMPLETELY untouched, same
//     as bootloader_metadata_unpack(). An erased (all-0xFF) or torn-write
//     record falls in here (bad magic). A record from some hypothetical v3
//     also falls in here DELIBERATELY: see the "REFUSE" outcome in this
//     file's header comment for why reinterpreting it would be worse than
//     refusing it.
bool config_store_unpack(const uint8_t in[CONFIG_STORE_RECORD_LEN],
                          config_store_record_t *out);

// Same contract as config_store_unpack() above, plus `out_reject` (optional,
// NULL-safe): on a `false` return, `*out_reject` says WHICH of the two
// failure shapes this file's "Load-time rejection diagnostics" block comment
// describes just happened -- bad magic/CRC/format_version (out_reject->
// rejected left false) versus a structurally-intact record whose field
// values config_params_validate_ranges() refused (out_reject->rejected true,
// with ->field/->rule/->seq naming the specific offender). `*out_reject` is
// left fully zeroed (rejected == false) on a `true` return, and is always
// written (never left uninitialised) whenever `out_reject != NULL`.
// config_store_unpack() is a thin wrapper over this with out_reject == NULL,
// so every existing caller (including test_config_store.c's) is unaffected --
// same "thin wrapper adds one out-param" pattern config_params_validate_ex()
// already uses in this codebase.
bool config_store_unpack_ex(const uint8_t in[CONFIG_STORE_RECORD_LEN],
                             config_store_record_t *out,
                             config_store_reject_info_t *out_reject);

// Fills `*out` with the safe, documented default record: format_version
// current (2), seq 0, fields_set 0 (nothing commissioned), tc_type
// CONFIG_STORE_DEFAULT_TC_TYPE (K, with CONFIG_STORE_SET_TC_TYPE clear --
// see that bit's comment above: K here is a real, usable, safe value, just
// not a COMMISSIONED one), calibration_missing true, every section 2-5 field
// at the compiled default CONFIG_REFERENCE.md documents, and the seven
// no-safe-default fields_set-gated fields left at 0/0.0f -- which is
// IRRELEVANT to any caller since fields_set says they are unset, but
// documented here so the zero is never mistaken for "the default is zero":
// it is "the bit says don't look at this byte". This is what every caller must fall back to
// when the flash region has never held a valid record or its latest record
// is corrupt -- "a missing part must not abort boot" (max31856_configure()'s
// own doc comment) applies just as much to configuration as to a missing
// sensor.
void config_store_default(config_store_record_t *out);

// --- Sector scan -----------------------------------------------------------

// Scans all CONFIG_STORE_SLOTS_PER_SECTOR records in `sector` (exactly
// SAFTYFW_CONFIG_STORE_FLASH_SIZE bytes, as read straight from the XIP-mapped
// config region -- this function does no flash I/O itself), unpacks each,
// and keeps the one with the highest `seq` among those that unpack
// successfully (config_store_unpack()'s three-outcome load applies per slot:
// a v1 slot that migrates cleanly is a valid candidate, seq and all; a slot
// whose format_version is newer than this firmware understands is NOT a
// candidate at all, exactly as if it were corrupt). Corrupt, erased
// (all-0xFF), or too-new slots are silently skipped.
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

// Same contract as config_store_find_latest() above, plus `out_reject`
// (optional, NULL-safe). Only meaningful when this function returns
// CONFIG_STORE_NO_SLOT: `*out_reject` is then filled with the highest-seq
// slot in the sector that was found structurally intact (magic/CRC/
// format_version all valid) but refused by config_params_validate_ranges()
// -- see config_store_unpack_ex() and this file's "Load-time rejection
// diagnostics" comment above for why that case (`rejected == true`) must not
// be reported, logged, or treated the same as an ordinary empty/fresh
// sector (`rejected == false`, every slot's magic/CRC itself was bad).
// When a valid slot IS found (return != CONFIG_STORE_NO_SLOT), `*out_reject`
// is always zeroed (rejected == false) regardless of whether some OTHER,
// lower-seq slot in the sector was also rejected -- a board that has a real,
// trustworthy current config is not in the state this diagnostic exists to
// surface. `*out_reject` is always written (never left uninitialised) when
// `out_reject != NULL`. config_store_find_latest() is a thin wrapper over
// this with out_reject == NULL.
size_t config_store_find_latest_ex(const uint8_t sector[SAFTYFW_CONFIG_STORE_FLASH_SIZE],
                                    config_store_record_t *out_rec,
                                    config_store_reject_info_t *out_reject);

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

// True iff config_store_boot_load() found NO valid slot in the sector AND at
// least one slot it scanned was structurally intact but refused by
// config_params_validate_ranges() -- case 2 of this file's "Load-time
// rejection diagnostics" comment. False for every other outcome, including
// "never called config_store_boot_load() yet" and the ordinary fresh-board
// case (no slot decodes at all). This is a strictly narrower condition than
// `config_store_is_calibration_missing()`, which is also true here but is
// ALSO true for a perfectly ordinary uncommissioned board -- this getter
// exists because "board is uncommissioned" and "board's committed config was
// just thrown out for being invalid" call for different operator responses,
// and calibration_missing alone cannot tell them apart. No consumer is wired
// to this yet (see config_store_flash.c's config_store_boot_load(), which
// logs the same condition over the console UART); it is exposed here so a
// future consumer -- a DIAG bit, a boot-reason report -- does not have to
// reconstruct the distinction from scratch.
bool config_store_is_config_rejected(void);

// True iff the cached record's tc_type has actually been commissioned
// (CONFIG_STORE_SET_TC_TYPE, see that bit's own comment) rather than merely
// holding its compiled default. Returns false (the safe default -- "treat
// it as uncommissioned") if called before config_store_boot_load(). The one
// caller today is thermo_task.c, choosing between max31856_tc_range_
// policy.h's per-type band (this true) and its widest-band garbage floor
// (this false) -- see that header's file comment.
bool config_store_is_tc_type_set(void);

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

// Copies the ENTIRE cached record into `*out` -- the read side of
// COMMISSIONING.md section 2's staging model: SET_PARAM (config_params.c,
// via link_task.c) stages a RAM copy that starts as whatever is currently
// committed, and GET_PARAM/GET_CONFIG_PAGE report what is currently
// committed (CONFIG_REFERENCE.md section 7: "which thresholds is the safety
// processor actually enforcing... answerable... without trusting a separate
// record of what was uploaded" -- the read-back path must reflect the
// enforced record, not a caller's in-progress edits). Same "safe default
// before boot_load()" contract as every other getter here: if called before
// config_store_boot_load(), `*out` is config_store_default()'s record.
void config_store_get_full_record(config_store_record_t *out);

// Writes `rec` as the new current config record, refusing while ARMED (see
// config_store_decide_write()). Returns false and fills `*out_reason` (if
// non-NULL) with a human-readable explanation on refusal or flash failure.
// `rec->format_version`/`rec->seq` are overwritten internally -- callers
// only need to set the rest of the record's fields.
//
// Wired to SAFETY_CMD_SET_CONFIG (0x16, LINK_PROTOCOL.md sec 4) via
// link_task_handle_set_config() (src/tasks/link_task.c).
bool config_store_write(const config_store_record_t *rec, const char **out_reason);

// The cached record's `seq`, mapped through config_store_seq_to_version()
// below -- what SAFETY_CMD_FW_VERSION's `config_version` byte carries
// (LINK_PROTOCOL.md sec 4). Bumped by every accepted config_store_write(),
// so a GUI watching FW_VERSION can tell a commissioning write actually
// landed. Returns 0 (config_store_default()'s own seq, mapped: see
// config_store_seq_to_version()'s own comment for why 0 maps to 0) if called
// before config_store_boot_load().
uint8_t config_store_get_config_version(void);

// Pure mapping from the cached record's `seq` (a uint32_t log counter -- one
// commit can run for the life of a board, so it must never wrap back onto a
// meaningful value in any human timeframe) to the u8 byte SAFETY_CMD_
// FW_VERSION's `config_version` field actually carries on the wire.
// config_store_get_config_version() above is just this function applied to
// the cache; exposed separately so it is pure/host-testable on its own,
// matching config_store_record_crc()'s split for the same reason.
//
// `config_version` is a FROZEN u8 field (LINK_PROTOCOL.md sec 4; see
// link_frame.c's link_frame_build_fw_version()/safety_link.c's frame C
// parser, both of which read/write exactly one byte at this position) --
// widening it to u16/u32 is a wire-format change this module cannot make
// unilaterally, so the fix for a truncating map has to be a different map,
// not a wider field.
//
// The old `(uint8_t)(seq & 0xFFu)` truncation could report 0 for a real,
// committed config -- every 256th commit (seq == 256, 512, 768, ...) landed
// back on the one value config_store_confirm_crc_ok() treats as "no valid
// config was ever loaded" (see that function's own header comment). This
// mapping keeps seq == 0 (config_store_default()'s own seq -- a board that
// never committed, or booted with a blank/corrupt sector) at 0, exactly as
// before, and folds every seq >= 1 into {1, ..., 255} instead of {0, ...,
// 255}: a 255-wide range that, by construction, can never land back on 0.
// This does not give every seq a globally unique byte forever (impossible in
// 8 bits regardless of the mapping) -- it only has to, and does, keep every
// REAL commit's byte out of the one reserved "unloaded" value, which is the
// entire property config_store_confirm_crc_ok() and update_task.c's
// PENDING_VERIFY -> VALID gate depend on.
uint8_t config_store_seq_to_version(uint32_t seq);

// A CRC over the cached record's active fields -- what
// SAFETY_CMD_FW_VERSION's `config_crc` field carries. The low 16 bits of
// config_store_record_crc() applied to the cached record; returns 0 if
// called before config_store_boot_load().
//
// ALSO returns 0 -- never the default record's own (non-zero) packed CRC --
// whenever the cached record's `seq` is 0, i.e. whenever config_store_get_
// config_version() would also read back 0 (see that function's own comment
// for the two inputs that produce seq == 0: never committed, or committed-
// then-rejected-at-load). This mirrors config_store_confirm_crc_ok()'s
// deliberate choice not to distinguish those two cases in the unsafe
// direction, for a reason specific to THIS getter: KilnFW's safety_page.html
// and diagnostics_page.html (App/drivers/*.html; not owned by this module)
// both test `safety_config_crc === 0` as their ONLY "UNCOMMISSIONED" signal
// -- no calibration_missing check backs it up on those pages. Before this
// fix, a board running entirely on config_store_default() -- including one
// whose only committed record was just refused by config_params_validate_
// ranges() at load, config_store.h's "Load-time rejection diagnostics" case
// 2 -- still packed and returned the DEFAULT record's own real, non-zero
// CRC-32 here, so those two pages read a rejected/never-committed board as
// "commissioned." Returning 0 for seq == 0 closes that: the one operator-
// visible signal those pages already have now actually fires for both
// halves of case 2's hazard (never told, AND told the wrong thing), without
// requiring any KilnFW-side change. Pure/host-testable via config_store_
// record_crc() and config_store_seq_to_version()'s own seq == 0 handling --
// this getter only adds the cache read and the same sentinel check.
uint16_t config_store_get_config_crc(void);

// Pure: packs `rec` (config_store_pack()) and returns the CRC-32 that ends
// up in its trailing field. Exposed so callers that only want the
// checksum -- not a full pack/unpack round trip -- don't have to carry a
// scratch CONFIG_STORE_RECORD_LEN buffer themselves. Host-testable exactly
// like the rest of this file's pure logic.
uint32_t config_store_record_crc(const config_store_record_t *rec);

// --- update_task's PENDING_VERIFY -> VALID gate (TODO.md Phase 10.8) -------

// Pure decision for src/update/confirm.h's `config_crc_ok` checklist bit.
// `config_version` is whatever config_store_get_config_version() just
// returned. That getter returns 0 in exactly two situations, and this
// codebase must not tell them apart in the unsafe direction:
//   1. config_store_boot_load() has never run (s_loaded == false in
//      config_store_flash.c) -- the cache is not even populated yet.
//   2. It HAS run, but config_store_find_latest() found no slot in flash
//      whose magic/format_version/CRC all validated (blank sector, torn
//      write, bit rot, or every slot newer than this firmware understands),
//      so the cache fell back to config_store_default(), whose seq is 0.
// A genuinely written, CRC-verified record can never read back as version 0:
// config_store_write() always assigns `s_cached_record.seq + 1u`, and the
// cached seq starts at 0 (the default's), so the first real write already
// produces version 1, and every subsequent one is strictly higher. So
// version == 0 is an unambiguous, permanent signal that no CRC-verified
// config record has ever been loaded -- config_crc_ok MUST be false in that
// case, and version != 0 is the only condition under which it may be true.
//
// This is the deliberate safe-direction choice for TODO.md Phase 10.8: the
// two failure inputs above (never loaded; loaded-but-nothing-valid-found)
// both resolve to `false`, which keeps update_confirm_missing() non-zero and
// the slot stuck at BOOTLOADER_SLOT_PENDING_VERIFY rather than letting it
// reach BOOTLOADER_SLOT_VALID. Getting this backwards -- treating "I don't
// know" as "confirmed good" -- would let a freshly-flashed image be marked
// VALID on a board whose config store was never actually re-validated,
// which is exactly the "sails through any check that just proves main()
// ran" failure docs/BOOTLOADER.md section 5 warns against. A slot stuck at
// PENDING_VERIFY is recoverable (it can still be confirmed once config_store
// loads a real record, or rolled back); a slot wrongly marked VALID cannot be
// un-confirmed except by pushing yet another update.
//
// Pure/host-testable: this is only the version==0 test, not the flash read
// itself (config_store_flash.c, NOT host-tested, is the only caller).
bool config_store_confirm_crc_ok(uint8_t config_version);

// --- flash_safe_execute() failure surfacing (TODO.md Phase 2) --------------
//
// config_store.c is pure and must stay dependency-free of pico-sdk (this
// file's own header comment), so it cannot #include pico/error.h and use
// PICO_OK/PICO_ERROR_* directly. These duplicate pico/error.h's
// `enum pico_error_codes` values as plain integer literals -- the exact same
// "duplicate the literal, assert numeric agreement at the call site"
// convention CONFIG_STORE_DEFAULT_TC_TYPE's header comment documents for
// MAX31856_TC_TYPE_K. config_store_flash.c (which DOES include pico/error.h)
// carries a compile-time assert that each of these still matches the SDK's
// own enum value, so a future pico-sdk upgrade that renumbers them fails the
// build instead of silently mismatching every reason string below.
#define CONFIG_STORE_FLASH_RC_OK                      0
#define CONFIG_STORE_FLASH_RC_TIMEOUT                (-2)
#define CONFIG_STORE_FLASH_RC_NOT_PERMITTED          (-4)
#define CONFIG_STORE_FLASH_RC_INSUFFICIENT_RESOURCES (-9)

// Turns the int flash_safe_execute() (pico/flash.h) returned into a human,
// honest reason string for config_store_write()'s `*out_reason`. Pure --
// this is only the string mapping, not the flash call itself.
//
// pico/flash.h's own doc comment on flash_safe_execute() names three
// distinct non-OK outcomes, and collapsing all of them into one opaque
// "flash write failed" would hide exactly the distinction a bench log or a
// retry policy needs:
//   - PICO_ERROR_TIMEOUT: the other core never answered the lockout
//     handshake within the timeout passed to flash_safe_execute() -- e.g. it
//     is spinning with interrupts disabled somewhere else, or is stuck. The
//     callback was NOT invoked (pico/flash.h: "the function may have been
//     called" is TIMEOUT's only caveat, but in this specific lockout path it
//     means the other core did not even reach the rendezvous). Worth a
//     retry and worth asking what the other core was doing.
//   - PICO_ERROR_NOT_PERMITTED: safe execution is not possible at all --
//     e.g. flash_safe_execute_core_init() was never called on the other
//     core, or this is being invoked before the scheduler starts or from an
//     unsafe context. This is a bug in THIS firmware's own init order, not a
//     transient condition, and must not be reported the same way as a
//     timeout.
//   - PICO_ERROR_INSUFFICIENT_RESOURCES: the lockout handshake's own dynamic
//     allocation failed.
// Any other/unrecognised negative value still returns a non-NULL, honest
// "unrecognised error code" string rather than silently reusing one of the
// three named reasons above for a code that does not actually mean that.
const char *config_store_flash_rc_reason(int rc);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_CONFIG_STORE_H
