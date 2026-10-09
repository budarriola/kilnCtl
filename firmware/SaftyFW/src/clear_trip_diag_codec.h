// clear_trip_diag_codec.h -- pure bit-packing for the CLEAR_TRIP crash
// checkpoint (clear_trip_diag.h), split out the same way watchdog_gate.h is
// split out of watchdog_task.c and uart_owner_tx_policy.h out of
// uart_owner.c: free of pico-sdk/FreeRTOS/hardware includes so it is
// directly host-testable (test/test_clear_trip_diag_codec.c links this .c
// file with no stub layer needed), unlike clear_trip_diag.c itself, which
// needs real hardware/structs/watchdog.h.
//
// clear_trip_diag_encode() packs one checkpoint into the single 32-bit word
// watchdog_hw->scratch[7] holds (see clear_trip_diag.h's own header comment
// for why only one register was available); clear_trip_diag_decode() is its
// exact inverse, including the magic-tag validity check. clear_trip_diag.c
// is a thin wrapper: encode-then-write, read-then-decode, nothing else.
#ifndef SAFTYFW_CLEAR_TRIP_DIAG_CODEC_H
#define SAFTYFW_CLEAR_TRIP_DIAG_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLEAR_TRIP_DIAG_STAGE_NONE            0u // never written this session -- either not reached, or a stale/absent read
#define CLEAR_TRIP_DIAG_STAGE_PRE_TRY_CLEAR   1u // about to call safety_guards_try_clear()
#define CLEAR_TRIP_DIAG_STAGE_POST_TRY_CLEAR  2u // safety_guards_try_clear() returned
#define CLEAR_TRIP_DIAG_STAGE_PRE_OUTCOME_LOG 3u // about to snprintf() the outcome message
#define CLEAR_TRIP_DIAG_STAGE_POST_LOG_TASK   4u // log_task_log() returned -- the whole drain block completed

typedef struct {
    bool     magic_ok;
    uint8_t  stage;       // one of CLEAR_TRIP_DIAG_STAGE_*
    uint8_t  reason;      // safety_trip_t, as of the checkpoint that wrote it
    uint8_t  fault_bits;  // SAFETY_THERMO_FAULT_* bits (safety_guards.h), full byte preserved
    bool     tc_valid;
    bool     spi_failed;
    bool     tc_c_is_nan; // isnan(in->tc_c) -- the exact float bits didn't fit this word's budget, this bit is what s5_bad_read_now() actually branches on
    uint8_t  outcome;     // safety_clear_trip_outcome_t, 0 (NONE) until the checkpoint that computed it
} clear_trip_diag_t;

// Packs every field into one word. `stage` is masked to 4 bits, `reason` to
// 4 bits, `outcome` to 3 bits, `fault_bits` used verbatim (a full byte) --
// no other field can silently truncate. Always includes the magic tag; there
// is no "encode without the tag" mode, since an untagged word is exactly the
// ambiguous case this format exists to avoid.
uint32_t clear_trip_diag_encode(uint8_t stage, uint8_t reason, uint8_t fault_bits, bool tc_valid,
                                 bool spi_failed, bool tc_c_is_nan, uint8_t outcome);

// Exact inverse of clear_trip_diag_encode(). magic_ok is false (and every
// other field zeroed/false) iff the high byte does not match the tag --
// power-on-uninitialised SRAM, a stale value from an unrelated reset, or
// (before this module ever ran once) a genuinely never-written register all
// collapse to the same "nothing usable here" verdict, same as
// boot_reason.c's own trip_reason_valid contract.
clear_trip_diag_t clear_trip_diag_decode(uint32_t word);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_CLEAR_TRIP_DIAG_CODEC_H
