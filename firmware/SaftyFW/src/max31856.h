// max31856.h -- single-channel MAX31856 driver for the safety thermocouple
// (J7, SaftyThermocoupleBoard). Ported from
// firmware/KilnFW/App/drivers/MAX31856.{c,h} per docs/ARCHITECTURE.md
// section 3 ("port it, do not rewrite it") -- same register map, same four
// fixed-point conversions, same comparator-fault-mode logic, same "every
// read fills the output struct even on failure, NaN not a cached value"
// discipline. firmware/KilnFW/docs/MAX31856.md is the reference for the part
// itself; firmware/SaftyFW/docs/THERMOCOUPLE.md section 1 is authoritative
// for what differs on THIS board and is cited inline below.
//
// What changed in the port, and why (THERMOCOUPLE.md section 1):
//   - ONE channel, not three: no MAX31856Class/MAX31856BusClass split, no
//     per-channel registry, no spi_device_handle_t indexing. This file's
//     static state IS the one channel. THERMOCOUPLE.md section 1: "the bus
//     is not shared" -- so the multi-channel/bus-sharing machinery in the
//     original is complexity with nothing to serve here, not a missing
//     feature.
//   - SPI via spi_owner.h's pico-sdk hardware/spi.h wrapper instead of
//     ESP-IDF's spi_master.h / esp_spi_owner.h. Same SPI mode 1 / 4 MHz /
//     burst-read concept, different call shapes.
//   - No MAX31856_drdy_provider_t hook and no elapsed-time "stale" estimate
//     inside this driver. On this board ~DRDY (GPIO12) is a real Pico GPIO
//     interrupt, owned by thermo_task, not a value this driver has to poll
//     or infer -- thermo_task blocks on the DRDY notification and detects
//     DRDY silence itself (THERMOCOUPLE.md section 1's "Two of these are
//     outright improvements"). This driver's max31856_read() is a plain
//     "do the burst read now" call; whether now is the right time to call it
//     is thermo_task's job, exactly the division ARCHITECTURE.md section 3
//     gives spi_owner/drivers vs. the task that owns the interface.
//   - tc_type is still a runtime parameter to max31856_configure(), per
//     THERMOCOUPLE.md section 2 ("a real decision, not a default") -- but
//     everything else CR0/CR1/MASK holds is now fixed by
//     THERMOCOUPLE.md section 5's table rather than caller-configurable,
//     because this board has exactly one commissioning axis (the
//     thermocouple type) where KilnFW's zone channels had none of the
//     safety-specific ones (comparator mode, this exact MASK value) hard
//     decided yet at the driver-API level.
#ifndef SAFTYFW_MAX31856_H
#define SAFTYFW_MAX31856_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Register addresses (datasheet Table 6, "Register Memory Map") -----
 * Reads use the 0Xh address, writes use 8Xh (read address + 0x80). The
 * address auto-increments while ~CS stays low, which is what makes the
 * 6-register burst in max31856_read() one transaction. Ported verbatim from
 * MAX31856_REG_* in firmware/KilnFW/App/drivers/MAX31856.h. */
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

#define MAX31856_WRITE_ADDR(reg) ((uint8_t)((reg) | 0x80u))

/* --- CR0 bits (datasheet "Register 00h/80h: Configuration 0 Register") -- */
#define MAX31856_CR0_CMODE       0x80u /* 1 = automatic conversion (~100ms), 0 = normally off */
#define MAX31856_CR0_ONESHOT     0x40u /* self-clearing */
#define MAX31856_CR0_OCFAULT1    0x20u
#define MAX31856_CR0_OCFAULT0    0x10u
#define MAX31856_CR0_CJ_DISABLE  0x08u /* 1 = internal cold-junction sensor off */
#define MAX31856_CR0_FAULT_INT   0x04u /* 0 = comparator mode, 1 = interrupt mode */
#define MAX31856_CR0_FAULTCLR    0x02u /* self-clearing; no effect in comparator mode */
#define MAX31856_CR0_FILTER_50HZ 0x01u /* 0 = reject 60Hz, 1 = reject 50Hz */

/* CR0.OCFAULT[1:0] open-circuit detection timing (datasheet Table 4). MODE1
 * is right for thermocouple loops well under 5k, same choice KilnFW makes. */
#define MAX31856_OC_MODE1 0x01u

/* --- MASK register bits (datasheet "Register 02h/82h") -----------------
 * 1 = masked (stops the fault driving ~FAULT; the bit still shows up in SR
 * either way). Bits 6:7 (TC-range/CJ-range) do not exist in this register --
 * the datasheet's own behaviour is that those two SR bits never drive
 * ~FAULT regardless of MASK, which is why THERMOCOUPLE.md section 5's
 * "unmask OPEN, OVUV, TCRANGE, CJRANGE" is trivially satisfied for the
 * latter two: there is nothing to unmask. */
#define MAX31856_MASK_OPEN   0x01u
#define MAX31856_MASK_OVUV   0x02u
#define MAX31856_MASK_TCLOW  0x04u
#define MAX31856_MASK_TCHIGH 0x08u
#define MAX31856_MASK_CJLOW  0x10u
#define MAX31856_MASK_CJHIGH 0x20u
#define MAX31856_MASK_ALL    0x3Fu

/* THERMOCOUPLE.md section 5: unmask OPEN + OVUV, leave the four threshold
 * faults masked (S1 owns the ceiling in software; TCRANGE/CJRANGE are
 * unmaskable per the note above). Reserved bits 7:6 are written as 1s, the
 * part's own factory-default state for them. Identical value and reasoning
 * to KilnFW's MAX31856_DEFAULT_FAULT_MASK (0xFC). */
#define MAX31856_DEFAULT_FAULT_MASK \
    ((uint8_t)((MAX31856_MASK_ALL & ~(MAX31856_MASK_OPEN | MAX31856_MASK_OVUV)) | 0xC0u))

/* SR / fault_bits layout -- identical numbering to safety_guards.h's
 * SAFETY_THERMO_FAULT_* (both mirror the same datasheet register; do not let
 * the two drift). Kept here too so this driver is self-contained and does
 * not need to #include safety_guards.h. */
#define MAX31856_FAULT_OPEN    0x01u
#define MAX31856_FAULT_OVUV    0x02u
#define MAX31856_FAULT_TCLOW   0x04u
#define MAX31856_FAULT_TCHIGH  0x08u
#define MAX31856_FAULT_CJLOW   0x10u
#define MAX31856_FAULT_CJHIGH  0x20u
#define MAX31856_FAULT_TCRANGE 0x40u
#define MAX31856_FAULT_CJRANGE 0x80u

/* --- CR1.TC TYPE[3:0] (datasheet Table 2) ------------------------------- */
#define MAX31856_TC_TYPE_B 0x00u
#define MAX31856_TC_TYPE_E 0x01u
#define MAX31856_TC_TYPE_J 0x02u
#define MAX31856_TC_TYPE_K 0x03u
#define MAX31856_TC_TYPE_N 0x04u
#define MAX31856_TC_TYPE_R 0x05u
#define MAX31856_TC_TYPE_S 0x06u
#define MAX31856_TC_TYPE_T 0x07u

/* CR1.AVGSEL[2:0]: THERMOCOUPLE.md section 5 fixes this at 4 samples
 * (~230ms/conversion) for every installation, so unlike tc_type it is not a
 * max31856_configure() parameter. 010b = 4 samples (000=1,001=2,010=4,
 * 011=8,1xx=16). */
#define MAX31856_AVGSEL_4_SAMPLES 0x02u

// THERMOCOUPLE.md section 2: "The type is configuration, and a mismatch is a
// silent hazard" -- tc_type must be a real, per-installation commissioning
// decision against tc_placement_mode and the kiln's peak temperature. There
// is no config_store yet (Phase 9) to source that decision from, so this is
// a placeholder main.c passes at boot purely so the part has SOME type
// configured rather than being left at an ambiguous power-on default. Type K
// is chosen because it is the safe "does not crash, does not claim precision
// it cannot have" placeholder, matching KilnFW's own zone default -- it is
// explicitly NOT a claim that K is correct for this installation. Whoever
// wires config_store in Phase 9 must replace the call site that uses this,
// not just change the constant here.
#define MAX31856_TC_TYPE_PLACEHOLDER MAX31856_TC_TYPE_K

/* --- Fixed-point scales (datasheet register bit-weight tables), ported
 * verbatim from MAX31856.h. --------------------------------------------- */
#define MAX31856_TC_TEMP_C_PER_LSB (1.0f / 4096.0f) /* LTCBH/M/L, 19 sig. bits */
#define MAX31856_CJ_TEMP_C_PER_LSB (1.0f / 256.0f)  /* CJTH:CJTL */

/* One read's worth of measurement. Same failure contract as KilnFW's
 * MAX31856Reading: if the SPI transfer failed, both temperatures are NaN and
 * spi_failed is true -- never 0, never the previous reading. A transfer that
 * succeeded but whose result the part itself says is meaningless (OPEN,
 * OVUV, TCRANGE for tc_temperature_c; CJRANGE for cj_temperature_c) also
 * reports NaN for that half, with spi_failed staying false (the bus worked,
 * the sensor did not) -- see max31856.c's max31856_read(). */
typedef struct {
    float   tc_temperature_c;  /* linearized, CJ-compensated; NaN if invalid */
    float   cj_temperature_c;  /* NaN only when the read failed or CJRANGE */
    uint8_t fault_status;      /* SR register, MAX31856_FAULT_* bits */
    bool    fault_pin_asserted; /* ~FAULT GPIO low right now */
    bool    spi_failed;        /* the transfer failed; both temperatures NaN */
} max31856_reading_t;

/* --- Bring-up ------------------------------------------------------------
 * Configures cs_gpio as a plain output (idling high) and fault_gpio as an
 * input with the internal pull-up (the daughterboard's ~FAULT is open-drain
 * with nothing else pulling it up, same reasoning as KilnFW's MAX31856_init
 * -- R1 on THIS board is an *external* pull-up per THERMOCOUPLE.md section 1,
 * so the internal one is redundant-but-harmless belt-and-braces, not load
 * bearing). Seeds the register shadows to the part's documented power-on
 * defaults (CR0=00h, CR1=03h, MASK=FFh) without writing anything -- call
 * max31856_configure() for that. spi_owner_init() must already have run.
 * Does not touch SPI. */
bool max31856_init(uint8_t cs_gpio, uint8_t fault_gpio);

/* Full CR0/CR1/MASK (re)configuration per THERMOCOUPLE.md section 5's table:
 * auto-convert, OCFAULT mode 1, CJ enabled, comparator fault mode, 60Hz
 * notch, AVGSEL = 4 samples, MASK = MAX31856_DEFAULT_FAULT_MASK. tc_type is
 * the one runtime-commissioned field (THERMOCOUPLE.md section 2) -- see
 * MAX31856_TC_TYPE_PLACEHOLDER in max31856.c for why K is passed today and
 * why that is explicitly not a decision about what belongs on this board.
 *
 * Handles the same two datasheet ordering constraints KilnFW's
 * MAX31856_configure() does: "change the notch frequency only in Normally
 * Off mode" and "averaging should not be changed while converting" -- so
 * this always stops conversions, writes CR1, then CR0 again with CMODE=1,
 * regardless of whether this is first bring-up or a re-assert after a
 * detected reset (THERMOCOUPLE.md's completion checklist: "config
 * re-asserted if the part is ever seen to have reset"). Returns false on any
 * SPI failure; the caller (main.c) logs and continues per
 * docs/ARCHITECTURE.md section 5 step 6 -- a missing part must not abort
 * boot. */
bool max31856_configure(uint8_t tc_type);

/* One burst read of CJTH..SR (0x0A..0x0F, six registers, one transaction) --
 * cold-junction temperature, linearized thermocouple temperature and fault
 * status, all from the same conversion. Always fills *out, including on
 * failure. Call this only after a DRDY edge (or a detected DRDY-silence
 * timeout, in which case the caller -- thermo_task -- should not trust
 * freshness even if this call itself succeeds); this driver does not gate on
 * DRDY itself, see the file header. */
bool max31856_read(max31856_reading_t *out);

/* How long the currently configured conversion takes, in ms -- tCONV plus
 * the AVGSEL=4 averaging adder, steady-state automatic-mode timing (not the
 * longer first-conversion-after-CMODE-goes-high figure, since thermo_task
 * uses this only to size its steady-state DRDY-wait timeout). 0 if the part
 * has not been configured yet. */
uint32_t max31856_conversion_time_ms(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_MAX31856_H
