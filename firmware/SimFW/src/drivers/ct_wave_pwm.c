// ct_wave_pwm.c -- see ct_wave_pwm.h for the design summary. This is the
// only file that touches these 3 PWM slices, the pacer slice, or their 3 DMA
// channels (docs/PLAN.md section 4 single-owner doctrine); wave_owner.c is
// its only caller.
//
// --- GPIO assignment (PROVISIONAL -- see docs/PLAN.md section 3.6) --------
//
// PLAN.md 3.6's pin budget table lists "CT sine PWM x3 | 3 | + external RC
// and transformer" without pinning down which 3 GPIO -- that is this file's
// job, pending a real docs/HARDWARE.md. Chosen here:
//
//   Zone 0 -> GPIO16 (PWM slice 0, channel A)
//   Zone 1 -> GPIO18 (PWM slice 1, channel A)
//   Zone 2 -> GPIO20 (PWM slice 2, channel A)
//
// Rationale: RP2040 maps GPIO -> PWM slice/channel as
// slice = (gpio / 2) % 8, channel = gpio % 2 (channel A = even GPIO).
// GP16/18/20 land on three *distinct* slices (0/1/2), each on channel A, so
// each channel gets its own free-running carrier with no slice sharing and
// no need to pack two channels' duty values into one 32-bit CC register.
// They were picked specifically to avoid: GPIO4/GPIO5 (i2c_owner.c's I2C0
// SDA/SCL, src/tasks/i2c_owner.c's header comment) and the low GPIO numbers
// (0-15) a PIO-based SPI driver most conventionally reaches for first (6
// contiguous pins for bus A, 4 for bus B, PLAN.md 3.6) -- at the time this
// file was written, spi_emu_a.c/spi_emu_b.c (the parallel PIO-SPI-engine
// agent's files) were both still idling stubs with zero GPIO claims, so
// there is nothing yet to conflict with in the repo -- but PIO state
// machines can reach *any* GPIO, so this is not a proof of safety, only the
// best check possible today. **A full 3-driver GPIO collision audit is
// required once docs/HARDWARE.md exists and/or spi_emu_a/b land real
// bodies** -- flagged again in this pass's report.
//
// Pacer slice: PWM slice 3 (GP22/23's slice, per the same formula) is used
// *without* routing its output to any GPIO -- pwm_set_enabled() and the
// slice's DREQ work independently of whether a pin is bound to the slice.
// Slice 3 was picked simply because it is not one of 0/1/2 (the three CT
// channels above); it drives nothing externally and claims no header pin.
//
// --- Carrier frequency arithmetic (PLAN.md 3.3: "~244 kHz ... 8-bit
// resolution at 125 MHz sysclk") -----------------------------------------
//
// RP2040 PWM output frequency = sysclk / (clkdiv * (wrap + 1)).
// 8-bit resolution => wrap = 255 (256 distinct duty levels, 0..255).
// sysclk = 125 MHz (pico-sdk default system clock, unmodified by this
// driver).
//
//   clkdiv = 2.0, wrap = 255:
//     f_carrier = 125,000,000 / (2.0 * 256) = 125,000,000 / 512
//               = 244,140.625 Hz  (~244.14 kHz -- matches the ~244 kHz
//               target to within 0.06%, comfortably >> the ~1-2 kHz 2-pole
//               RC corner PLAN.md 3.3 specifies).
//
// --- Sample-rate (DMA pacer) arithmetic (PLAN.md 3.3: "256 entries per
// cycle at 60 Hz -> table stepped at 15.36 kHz") ---------------------------
//
// 256 samples/cycle * 60 Hz = 15,360 Hz exactly -- the target sample rate.
// The pacer PWM slice's own wrap event (not its duty) is what paces the
// per-sample DMA transfer, via that slice's DREQ output, so:
//
//   f_pace = sysclk / (clkdiv * (wrap + 1))  [same formula, clkdiv=1.0 here]
//
// 125,000,000 / 15,360 = 8138.0208333... -- not an integer, so no
// (clkdiv=1, integer wrap) combination hits 15.36 kHz exactly (RP2040's
// clkdiv is 8.4 fixed-point, wrap is a 16-bit integer -- the achievable
// frequency set is a countable grid, and 15,360 Hz is not on it at
// clkdiv=1). Nearest practical integer-wrap point at clkdiv=1.0:
//
//   wrap = 8137 (period = wrap+1 = 8138 cycles):
//     f_pace = 125,000,000 / 8138 = 15,361.19 Hz
//     error vs 15,360 Hz target = +1.19 Hz = +0.0077%
//
// This is negligible for a simulator whose whole point is bench-testing
// firmware logic, not serving as a calibrated reference -- and it means
// each 256-sample table (one full 60 Hz cycle) actually takes
// 256/15361.19 = 16.6653 ms instead of the nominal 16.667 ms, i.e. the
// synthesized "60 Hz" line is actually ~60.0046 Hz. Documented here per this
// pass's requirement to show the arithmetic even though it cannot be scope-
// verified against a real signal without hardware.
#include "ct_wave_pwm.h"

#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "pico/platform.h"

// --- GPIO / slice assignment (see file header) ------------------------------
// Named as individual macros, not just array literals, so the compile-time
// guard block below can _Static_assert each one against the forbidden-pin
// list -- an array initializer alone cannot be a _Static_assert operand.
#define CT_WAVE_GPIO_0 16u
#define CT_WAVE_GPIO_1 18u
#define CT_WAVE_GPIO_2 20u
static const uint8_t CT_WAVE_GPIO[CT_WAVE_PWM_NUM_CHANNELS] = { CT_WAVE_GPIO_0, CT_WAVE_GPIO_1, CT_WAVE_GPIO_2 };
#define CT_WAVE_PACER_SLICE 3u

// --- Compile-time guard: the "PWM pacer / CT channel-B" latent trap --------
// (docs/HARDWARE.md section 0 item 9, found by the pin-map reconciliation in
// commit 1d32e84). RP2040's fixed GPIO->PWM mux table gives slice N exactly
// four candidate output pins: channel A on GPIO(2N) and GPIO(2N+16), channel
// B on GPIO(2N+1) and GPIO(2N+17). That mapping is silicon, not a choice this
// file makes -- so it applies just as much to the pacer slice (which binds no
// pin today, on purpose) and to each CT channel's own slice's unused channel-B
// pin as it does to the three channel-A pins this file actually drives.
//
// Two of those never-bound-today pin groups collide with docs/HARDWARE.md
// section 1's non-PWM owners:
//   - Pacer slice 3 (CT_WAVE_PACER_SLICE)'s own candidates: GPIO6 (SPI bus A
//     SCLK), GPIO7 (SPI bus A MOSI), GPIO22 (FAULT_MAIN_2). GPIO23 is not a
//     header pin on a stock Pico.
//   - Each CT channel's slice's channel-B pin (channel-A GPIO + 1): GPIO17
//     (DRDY_MAIN_2), GPIO19 (FAULT_MAIN_0), GPIO21 (FAULT_MAIN_1).
//
// Nothing calls gpio_set_function(..., GPIO_FUNC_PWM) on any of those six
// pins today (arm_dma() only ever writes the CC register's channel-A
// halfword, never touches channel B's function select, and the pacer slice
// is pwm_init()'d with no gpio_set_function() call at all) -- but the trap is
// that *nothing stops a future edit from adding one*, and every one of the
// six is either a PIO-function signal (SCLK/MOSI) or a SIO open-drain
// ~FAULT/~DRDY line owned elsewhere, so a stray PWM carrier there is a real
// hardware fault, not a style nit.
//
// tools/check_single_owner.ps1's single-owner rule already makes this file
// the ONLY place in the tree that may include hardware/pwm.h, so every
// gpio_set_function(pin, GPIO_FUNC_PWM) call in the whole codebase lives
// here. The _Static_assert block below therefore fully covers the one path
// this driver actually uses to pick a pin (the CT_WAVE_GPIO_* macros feeding
// the array above) -- reassigning any of them to a forbidden pin is a build
// error, not a silent hardware fault. It does NOT by itself catch a brand
// new hardcoded gpio_set_function(6, GPIO_FUNC_PWM)-style call added
// elsewhere in this file bypassing the macros entirely; that residual case
// is covered by check_single_owner.ps1's PWM SAFETY RULES section, which
// also fails loudly if this whole guard block is ever deleted.
#define CT_WAVE_PWM_FORBIDDEN_GPIO_6  6u  // SPI bus A SCLK   (spi_emu_a.c)     -- pacer slice 3 channel-A candidate
#define CT_WAVE_PWM_FORBIDDEN_GPIO_7  7u  // SPI bus A MOSI   (spi_emu_a.c)     -- pacer slice 3 channel-B candidate
#define CT_WAVE_PWM_FORBIDDEN_GPIO_17 17u // DRDY_MAIN_2      (spi_emu_a.c)     -- CT ch0 slice's own channel-B pin
#define CT_WAVE_PWM_FORBIDDEN_GPIO_19 19u // FAULT_MAIN_0     (HARDWARE.md sec1)-- CT ch1 slice's own channel-B pin
#define CT_WAVE_PWM_FORBIDDEN_GPIO_21 21u // FAULT_MAIN_1     (HARDWARE.md sec1)-- CT ch2 slice's own channel-B pin
#define CT_WAVE_PWM_FORBIDDEN_GPIO_22 22u // FAULT_MAIN_2     (HARDWARE.md sec1)-- pacer slice 3 channel-A candidate

#define CT_WAVE_PWM_GPIO_IS_FORBIDDEN(gpio)                                   \
    ((gpio) == CT_WAVE_PWM_FORBIDDEN_GPIO_6  ||                               \
     (gpio) == CT_WAVE_PWM_FORBIDDEN_GPIO_7  ||                               \
     (gpio) == CT_WAVE_PWM_FORBIDDEN_GPIO_17 ||                               \
     (gpio) == CT_WAVE_PWM_FORBIDDEN_GPIO_19 ||                               \
     (gpio) == CT_WAVE_PWM_FORBIDDEN_GPIO_21 ||                               \
     (gpio) == CT_WAVE_PWM_FORBIDDEN_GPIO_22)

_Static_assert(!CT_WAVE_PWM_GPIO_IS_FORBIDDEN(CT_WAVE_GPIO_0),
               "ct_wave_pwm.c: CT_WAVE_GPIO_0 is assigned to a GPIO that "
               "docs/HARDWARE.md section 1 gives to a non-PWM owner (SPI bus "
               "A SCLK/MOSI, or a FAULT_MAIN_*/DRDY_MAIN_2 open-drain line) -- "
               "gpio_set_function(..., GPIO_FUNC_PWM) on that pin would put a "
               "free-running PWM carrier onto a claimed signal line. See "
               "docs/HARDWARE.md section 0 item 9.");
_Static_assert(!CT_WAVE_PWM_GPIO_IS_FORBIDDEN(CT_WAVE_GPIO_1),
               "ct_wave_pwm.c: CT_WAVE_GPIO_1 is assigned to a GPIO that "
               "docs/HARDWARE.md section 1 gives to a non-PWM owner (SPI bus "
               "A SCLK/MOSI, or a FAULT_MAIN_*/DRDY_MAIN_2 open-drain line) -- "
               "gpio_set_function(..., GPIO_FUNC_PWM) on that pin would put a "
               "free-running PWM carrier onto a claimed signal line. See "
               "docs/HARDWARE.md section 0 item 9.");
_Static_assert(!CT_WAVE_PWM_GPIO_IS_FORBIDDEN(CT_WAVE_GPIO_2),
               "ct_wave_pwm.c: CT_WAVE_GPIO_2 is assigned to a GPIO that "
               "docs/HARDWARE.md section 1 gives to a non-PWM owner (SPI bus "
               "A SCLK/MOSI, or a FAULT_MAIN_*/DRDY_MAIN_2 open-drain line) -- "
               "gpio_set_function(..., GPIO_FUNC_PWM) on that pin would put a "
               "free-running PWM carrier onto a claimed signal line. See "
               "docs/HARDWARE.md section 0 item 9.");

// --- Carrier config (see file header arithmetic) ----------------------------
#define CT_WAVE_CARRIER_WRAP   255u   // 8-bit resolution: 256 duty levels
#define CT_WAVE_CARRIER_CLKDIV 2.0f   // -> ~244.14 kHz carrier @ 125 MHz sysclk

// --- Pacer config (see file header arithmetic) ------------------------------
#define CT_WAVE_PACER_WRAP   8137u    // period = 8138 cycles
#define CT_WAVE_PACER_CLKDIV 1.0f     // -> ~15,361.19 Hz sample rate @ 125 MHz sysclk

// Chosen instead of DMA_IRQ_0 specifically to stay clear of whatever the
// parallel PIO-SPI-engine agent's spi_emu_a/b picks -- DMA_IRQ_0 is the
// pico-sdk examples' conventional first choice, so DMA_IRQ_1 is the lower-
// collision-risk pick for a second, independent DMA-using driver landing at
// the same time. Still needs the same final collision audit noted in the
// file header.
#define CT_WAVE_DMA_IRQ DMA_IRQ_1

static int s_dma_chan[CT_WAVE_PWM_NUM_CHANNELS] = { -1, -1, -1 };
static uint8_t s_slice[CT_WAVE_PWM_NUM_CHANNELS];

// Two 256-entry uint16_t tables per channel: the one DMA is actively
// streaming from (`active`) and a staging area (`pending`) wave_owner writes
// new parameter tables into. Both arrays are read/written only from: (a)
// wave_owner's own task calling ct_wave_pwm_load_table(), and (b) this
// file's DMA_IRQ_1 handler -- never from any other task, so a short
// save_and_disable_interrupts() critical section around the handoff is
// sufficient (no FreeRTOS primitives, since one side is an ISR).
static uint16_t s_active_table[CT_WAVE_PWM_NUM_CHANNELS][SINE_SYNTH_TABLE_LEN];
static uint16_t s_pending_table[CT_WAVE_PWM_NUM_CHANNELS][SINE_SYNTH_TABLE_LEN];
static volatile bool s_pending_valid[CT_WAVE_PWM_NUM_CHANNELS];

static dma_channel_config s_dma_cfg[CT_WAVE_PWM_NUM_CHANNELS];

// Re-arms channel `ch`'s DMA transfer to stream s_active_table[ch] into its
// PWM slice's channel-A compare register, one 256-entry pass (one 60 Hz
// cycle). Writing only a 16-bit halfword at the CC register's base address
// hits channel A's bits [15:0] only (RP2040 datasheet: CC register packs
// channel A in bits 15:0, channel B in bits 31:16) -- channel B is never
// touched, which is fine since none of CT_WAVE_GPIO's slices route a pin to
// channel B.
static void arm_dma(uint8_t ch)
{
    volatile uint16_t *cc_low16 = (volatile uint16_t *)&pwm_hw->slice[s_slice[ch]].cc;
    dma_channel_configure(s_dma_chan[ch], &s_dma_cfg[ch],
                           cc_low16,               // write address: PWM CC reg, channel-A halfword
                           s_active_table[ch],      // read address: this channel's active 256-entry table
                           SINE_SYNTH_TABLE_LEN,    // one full cycle per arm
                           true);                   // start immediately
}

// Shared DMA_IRQ_1 handler for all 3 CT channels. Fires once per channel per
// completed 256-sample pass -- i.e. once per synthesized ~60 Hz cycle per
// channel, at table index 0, which sine_synth_init_table()'s contract
// guarantees is an exact zero (a rising zero crossing). Swapping in a
// pending table right here is therefore inherently zero-crossing-gated.
static void __not_in_flash_func(ct_wave_dma_irq_handler)(void)
{
    for (uint8_t ch = 0; ch < CT_WAVE_PWM_NUM_CHANNELS; ch++) {
        if (dma_channel_get_irq1_status(s_dma_chan[ch])) {
            dma_channel_acknowledge_irq1(s_dma_chan[ch]);

            if (s_pending_valid[ch]) {
                memcpy(s_active_table[ch], s_pending_table[ch], sizeof(s_active_table[ch]));
                s_pending_valid[ch] = false;
            }

            arm_dma(ch);
        }
    }
}

bool ct_wave_pwm_init(void)
{
    // Pacer slice: free-running, no GPIO bound, purely for its DREQ.
    pwm_config pacer_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&pacer_cfg, CT_WAVE_PACER_CLKDIV);
    pwm_config_set_wrap(&pacer_cfg, CT_WAVE_PACER_WRAP);
    pwm_init(CT_WAVE_PACER_SLICE, &pacer_cfg, true); // start counting now

    for (uint8_t ch = 0; ch < CT_WAVE_PWM_NUM_CHANNELS; ch++) {
        uint8_t gpio = CT_WAVE_GPIO[ch];
        uint8_t slice = pwm_gpio_to_slice_num(gpio);
        s_slice[ch] = slice;

        gpio_set_function(gpio, GPIO_FUNC_PWM);

        pwm_config cfg = pwm_get_default_config();
        pwm_config_set_clkdiv(&cfg, CT_WAVE_CARRIER_CLKDIV);
        pwm_config_set_wrap(&cfg, CT_WAVE_CARRIER_WRAP);
        pwm_init(slice, &cfg, true); // start the carrier now, at mid-scale (0 duty) until first table lands

        // Silence (mid-scale, i.e. no AC component after the RC filter/
        // transformer) until wave_owner loads a real table.
        for (uint32_t i = 0; i < SINE_SYNTH_TABLE_LEN; i++) {
            s_active_table[ch][i] = (CT_WAVE_CARRIER_WRAP + 1u) / 2u;
        }
        s_pending_valid[ch] = false;

        int chan = dma_claim_unused_channel(false);
        if (chan < 0) {
            return false;
        }
        s_dma_chan[ch] = chan;

        dma_channel_config c = dma_channel_get_default_config((uint)chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, DREQ_PWM_WRAP0 + CT_WAVE_PACER_SLICE); // pacer slice's wrap DREQ
        s_dma_cfg[ch] = c;

        dma_channel_set_irq1_enabled((uint)chan, true);
        arm_dma(ch);
    }

    irq_set_exclusive_handler(CT_WAVE_DMA_IRQ, ct_wave_dma_irq_handler);
    irq_set_enabled(CT_WAVE_DMA_IRQ, true);

    return true;
}

void ct_wave_pwm_load_table(uint8_t channel, const uint16_t levels[SINE_SYNTH_TABLE_LEN],
                             bool apply_immediately)
{
    if (channel >= CT_WAVE_PWM_NUM_CHANNELS) {
        return;
    }

    if (!apply_immediately) {
        uint32_t save = save_and_disable_interrupts();
        memcpy(s_pending_table[channel], levels, sizeof(s_pending_table[channel]));
        s_pending_valid[channel] = true;
        restore_interrupts(save);
        return;
    }

    // Mid-cycle step distortion path (PLAN.md 3.3): apply right now,
    // wherever the carrier currently is in its cycle, instead of waiting for
    // the natural zero-crossing boundary the IRQ path gates on.
    uint32_t save = save_and_disable_interrupts();
    memcpy(s_active_table[channel], levels, sizeof(s_active_table[channel]));
    s_pending_valid[channel] = false; // any older pending table is superseded
    arm_dma(channel);
    restore_interrupts(save);
}
