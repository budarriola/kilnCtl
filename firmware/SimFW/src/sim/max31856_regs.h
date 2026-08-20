// max31856_regs -- pure, host-testable MAX31856 register-file emulator for
// SimFW. docs/PLAN.md section 3.2 is authoritative for the register map and
// behavioral rules; this header/.c implement it.
//
// Register addresses, bit layouts, and fixed-point scales below are
// cross-checked against this repo's own MAX31856 masters, which must not
// need a single driver change to talk to the emulator (PLAN.md 3.2's own
// requirement):
//   - firmware/KilnFW/App/drivers/MAX31856.h (ESP32-S3, three channels, J6)
//   - firmware/SaftyFW/src/max31856.h (RP2040 A1, one channel, J7 -- a
//     verbatim port of the KilnFW driver's register map per that file's own
//     header comment)
// Both agree byte-for-byte on addresses (00h-0Fh), CR0/MASK/SR bit
// positions, and the four fixed-point scales; this file mirrors them.
//
// Byte-level model: a caller (in the real firmware, the PIO/spi_emu task; in
// host tests, the test itself) drives a simulated SPI transaction through
// max31856_regs_cs_assert() / _clock_read_byte() / _clock_write_byte() /
// _cs_deassert(), exactly matching the datasheet's one-address-byte-then-
// N-data-bytes, auto-incrementing shape. Convenience burst helpers wrap that
// for callers that do not need per-byte control (most host tests).
//
// Coherency guarantee (PLAN.md 3.2.1): "a multi-byte LTCB read is always
// internally consistent -- same guarantee the real chip gives." This is
// implemented by snapshotting the live register image at CS-assert time for
// a read transaction; nothing mutates that snapshot until CS deasserts, no
// matter how many conversions max31856_regs_advance_conversion() completes
// while a caller holds a read transaction open (which a well-behaved caller
// never does anyway, since this module has no concurrency of its own -- the
// snapshot exists so the invariant is enforced and testable, not just true
// by accident of single-threaded host tests).
#ifndef SIMFW_SIM_MAX31856_REGS_H
#define SIMFW_SIM_MAX31856_REGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Register addresses (datasheet Table 6), read (0Xh) address space --- */
#define MAX31856_REG_CR0     0x00u
#define MAX31856_REG_CR1     0x01u
#define MAX31856_REG_MASK    0x02u
#define MAX31856_REG_CJHF    0x03u
#define MAX31856_REG_CJLF    0x04u
#define MAX31856_REG_LTHFTH  0x05u
#define MAX31856_REG_LTHFTL  0x06u
#define MAX31856_REG_LTLFTH  0x07u
#define MAX31856_REG_LTLFTL  0x08u
#define MAX31856_REG_CJTO    0x09u
#define MAX31856_REG_CJTH    0x0Au
#define MAX31856_REG_CJTL    0x0Bu
#define MAX31856_REG_LTCBH   0x0Cu
#define MAX31856_REG_LTCBM   0x0Du
#define MAX31856_REG_LTCBL   0x0Eu
#define MAX31856_REG_SR      0x0Fu
#define MAX31856_REG_COUNT   0x10u

#define MAX31856_WRITE_BIT   0x80u
#define MAX31856_WRITE_ADDR(reg) ((uint8_t)((reg) | MAX31856_WRITE_BIT))
#define MAX31856_ADDR_MASK(byte) ((uint8_t)((byte) & 0x0Fu)) /* strips write bit + reserved */

/* --- CR0 bits --- */
#define MAX31856_CR0_CMODE       0x80u
#define MAX31856_CR0_ONESHOT     0x40u /* self-clearing after one conversion */
#define MAX31856_CR0_OCFAULT1    0x20u
#define MAX31856_CR0_OCFAULT0    0x10u
#define MAX31856_CR0_CJ_DISABLE  0x08u
#define MAX31856_CR0_FAULT_INT   0x04u /* 0 = comparator mode, 1 = interrupt mode */
#define MAX31856_CR0_FAULTCLR    0x02u /* self-clearing */
#define MAX31856_CR0_FILTER_50HZ 0x01u

/* --- MASK register bits (1 = masked, stops that fault driving ~FAULT) --- */
#define MAX31856_MASK_OPEN   0x01u
#define MAX31856_MASK_OVUV   0x02u
#define MAX31856_MASK_TCLOW  0x04u
#define MAX31856_MASK_TCHIGH 0x08u
#define MAX31856_MASK_CJLOW  0x10u
#define MAX31856_MASK_CJHIGH 0x20u
#define MAX31856_MASK_ALL    0x3Fu /* bits 7:6 reserved, not maskable */

/* --- SR fault-status bits (shares low 6 positions with MASK) --- */
#define MAX31856_FAULT_OPEN    0x01u
#define MAX31856_FAULT_OVUV    0x02u
#define MAX31856_FAULT_TCLOW   0x04u
#define MAX31856_FAULT_TCHIGH  0x08u
#define MAX31856_FAULT_CJLOW   0x10u
#define MAX31856_FAULT_CJHIGH  0x20u
#define MAX31856_FAULT_TCRANGE 0x40u /* never maskable */
#define MAX31856_FAULT_CJRANGE 0x80u /* never maskable */

/* --- Fixed-point scales (datasheet register bit-weight tables) --- */
#define MAX31856_TC_THRESHOLD_C_PER_LSB 0.0625f    /* LTHFTH/L, LTLFTH/L, int16 */
#define MAX31856_CJ_OFFSET_C_PER_LSB    0.0625f    /* CJTO, int8 */
#define MAX31856_CJ_TEMP_C_PER_LSB      (1.0f / 256.0f)  /* CJTH:CJTL, int16 */
#define MAX31856_TC_TEMP_C_PER_LSB      0.0078125f /* LTCBH/M/L, 19-bit code (PLAN.md 3.2) */

/* Dead-channel corruption modes (PLAN.md 3.2's corruption-knob list). All
 * three force every byte of a read transaction on this channel regardless
 * of address; HIGH_Z is functionally identical to ALL_ONE at this register
 * level (a floating bus idling high) -- the distinction only matters at the
 * electrical layer this pure module does not simulate, and is kept as its
 * own value so a test/caller can still assert on which mode was requested. */
typedef enum {
    MAX31856_DEAD_NONE = 0,
    MAX31856_DEAD_ALL_ZERO,
    MAX31856_DEAD_ALL_ONE,
    MAX31856_DEAD_HIGH_Z,
} max31856_dead_mode_t;

/* PLAN.md 3.2's per-channel corruption knobs. */
typedef struct {
    bool stuck_ltcb;              /* freeze the reported TC temp (incl. SR
                                    * comparisons derived from it) at its
                                    * last value -- "LTCB frozen while the
                                    * zone keeps moving" */
    float noise_sigma_c;           /* Gaussian jitter stddev added to the
                                     * reported TC temp before quantizing,
                                     * degC. 0 = no noise. */
    max31856_dead_mode_t dead_mode; /* all reads return this fixed pattern */
    float bit_error_rate;           /* 0..1, independent per-bit flip
                                      * probability on read bytes (flaky
                                      * MISO) */
    bool spurious_fault_pin;        /* force ~FAULT asserted regardless of
                                      * SR/MASK */

    bool shorted;                   /* PLAN.md 7.1 "Shorted TC": reported TC
                                      * temperature is forced to the reported
                                      * CJ temperature (near-ambient/CJ)
                                      * regardless of the true zone
                                      * temperature -- what a real shorted
                                      * junction (zero Seebeck voltage)
                                      * reads. Takes precedence over
                                      * drift_offset_c (a shorted junction
                                      * cannot also be drifting its true
                                      * reading -- there is no true reading
                                      * anymore); stuck_ltcb still wins over
                                      * both if also set, since "frozen at
                                      * last value" is the more severe/
                                      * definite claim (same severity
                                      * doctrine fault_sched.c documents for
                                      * its own zone-level overrides). */
    float drift_offset_c;           /* PLAN.md 7.1 "Drifting TC": calibration-
                                      * drift ramp offset, degC, added to the
                                      * true TC reading before noise/
                                      * quantization. This module has no time
                                      * source of its own (advance_conversion()
                                      * takes no dt) -- the ramp is expected
                                      * to be computed by the caller from
                                      * elapsed sim time and rewritten here
                                      * every recompute, exactly like every
                                      * other knob in this struct is a plain
                                      * instantaneous value, never an
                                      * accumulator this module integrates
                                      * itself. 0 = no drift. */
    float cj_fault_offset_c;        /* PLAN.md 7.1 "CJ fault": added to the
                                      * internally-sensed CJ temperature
                                      * (true_cj_c + CJTO) before quantizing
                                      * into CJTH:CJTL and before the
                                      * existing CJHF/CJLF threshold compare
                                      * -- so a nonzero value both reports a
                                      * wrong CJ temperature *and* naturally
                                      * asserts SR's CJHIGH/CJLOW bits once
                                      * it crosses the master-written
                                      * thresholds, with no separate SR-
                                      * forcing path needed (unlike OPEN/
                                      * OVUV, which have no threshold to
                                      * cross on their own). Has no effect
                                      * while CR0.CJ_DISABLE is set, since in
                                      * that mode the master owns CJTH:CJTL
                                      * outright and this module only reads
                                      * it back. 0 = no CJ fault. */
} max31856_corruption_t;

/* One emulated channel: the live 16-byte register image, the "true" inputs
 * fed each conversion, and the corruption knobs layered on top. */
typedef struct {
    uint8_t regs[MAX31856_REG_COUNT];

    /* Reported-value bookkeeping: what the last conversion actually put in
     * LTCB/SR, independent of the raw registers, so stuck_ltcb and
     * noise/bit-error corruption have a stable "last value" to reuse or
     * perturb from a single source of truth. */
    float last_reported_tc_c;
    bool has_reported;

    max31856_corruption_t corruption;

    uint32_t rng_state; /* xorshift32, see max31856_regs.c */

    /* --- transaction state --- */
    bool cs_low;
    bool txn_is_write;
    uint8_t txn_addr;          /* next register address to touch (auto-incrementing) */
    uint8_t read_snapshot[MAX31856_REG_COUNT]; /* coherency snapshot for an open read */

    /* Master-write bookkeeping (TC_GET_MASTER_CONFIG, PLAN.md 5.2/3.2): set
     * true the first time apply_write_rule() ever runs for this channel --
     * i.e. the first data byte of the first write transaction the master
     * ever issues, even one targeting a read-only address (an attempted
     * write is still "the master wrote something"). Write-once
     * false->true, never cleared except by max31856_regs_init() -- this is
     * what lets a caller distinguish "the DUT configured this channel
     * wrong" from "the DUT never configured this channel at all", which
     * PLAN.md 5.2 calls out as "a different and equally important
     * failure." A plain bool read/write is used deliberately (no snapshot,
     * no coherency machinery): it is monotonic and single-byte, so a reader
     * on another core can never observe a torn value the way a multi-byte
     * struct copy could -- unlike regs[], which needs the caller-side
     * getters in spi_emu_a.h/spi_emu_b.h to protect against a mid-
     * transaction sample. */
    bool master_has_written;
} max31856_channel_t;

/* Seeds regs[] to the part's documented power-on defaults (CR0=00h,
 * CR1=03h, MASK=FFh, everything else 0) and clears corruption/state.
 * rng_seed seeds the deterministic xorshift32 generator used for noise and
 * bit-error injection -- 0 is remapped to a fixed nonzero value internally
 * (xorshift32 cannot recover from an all-zero state). */
void max31856_regs_init(max31856_channel_t *ch, uint32_t rng_seed);

/* --- Byte-level transaction interface (mirrors the real SPI protocol) --- */

/* Begins a transaction: addr_byte is the raw first byte on the wire (0x0F
 * or 0x8Fh, etc). Bit 7 selects write vs read. For a read, snapshots the
 * live register image so a multi-byte burst never tears (see file header). */
void max31856_regs_cs_assert(max31856_channel_t *ch, uint8_t addr_byte);

/* Clocks one byte out for a read transaction (post-corruption). Auto-
 * increments the internal address, wrapping at MAX31856_REG_COUNT (matches
 * the part: reading past SR wraps back to CR0). Undefined byte value (0x00)
 * if called outside an open read transaction. */
uint8_t max31856_regs_clock_read_byte(max31856_channel_t *ch);

/* Clocks one byte in for a write transaction and applies write-back rules
 * (read-only registers ignored, self-clearing bits, CJ-disable-gated
 * writability) immediately. Auto-increments and wraps like the read side.
 * No-op if called outside an open write transaction. */
void max31856_regs_clock_write_byte(max31856_channel_t *ch, uint8_t data_in);

/* Ends the transaction. */
void max31856_regs_cs_deassert(max31856_channel_t *ch);

/* --- Convenience burst helpers (open/clock/close in one call) --- */
void max31856_regs_write_burst(max31856_channel_t *ch, uint8_t addr, const uint8_t *data, size_t len);
void max31856_regs_read_burst(max31856_channel_t *ch, uint8_t addr, uint8_t *out, size_t len);

/* Advances one simulated conversion: recomputes LTCB/CJTH:CJTL/SR from
 * true_tc_c/true_cj_c (the thermal model's output) plus whatever the master
 * has written into the threshold registers, honoring corruption knobs and
 * CR0's comparator/interrupt fault-clearing mode. Resolves a pending
 * CR0.ONESHOT (self-clears it). Returns true unconditionally -- the return
 * value exists so a caller can treat it as "a DRDY edge happened" without
 * a separate no-op path today, and so a future revision can report "no
 * conversion happened this call" (e.g. CMODE off and no pending one-shot)
 * without an API break. */
bool max31856_regs_advance_conversion(max31856_channel_t *ch, float true_tc_c, float true_cj_c);

/* Current ~FAULT pin level: true = asserted (low). Derived from SR & the
 * effective mask (TCRANGE/CJRANGE always unmaskable, per the datasheet and
 * both real drivers' header comments), OR corruption.spurious_fault_pin. */
bool max31856_regs_fault_pin_asserted(const max31856_channel_t *ch);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_SIM_MAX31856_REGS_H
