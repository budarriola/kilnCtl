// ct_wave_i2s.c -- see ct_wave_i2s.h for the design summary and the
// MUTUAL EXCLUSION / PIO PROGRAM MEMORY conflict notes. This is the only
// file that touches PIO1's 2 free state machines, the 2 DMA channels it
// claims, or GPIO27/28/21/22 (docs/DESIGN_NOTES.md section 4 single-owner
// doctrine). Nothing calls ct_wave_i2s_init() yet -- see ct_wave_i2s.h's
// header comment.
//
// --- GPIO assignment (PROVISIONAL, and a KNOWN, DOCUMENTED CONFLICT with
// docs/HARDWARE.md section 1 -- not an oversight) -------------------------
//
// docs/HARDWARE.md section 1 has 25 of the Pico's 26 header GPIOs already
// assigned, with exactly ONE true spare: GPIO28. This driver needs FOUR
// signals (BCLK, WS, DIN_A, DIN_B). There is no way to find 4 genuinely
// free GPIOs on this fixture today -- so three of the four claims below
// reuse GPIOs docs/HARDWARE.md section 1 already reserves for something
// else that has no OWNER FILE yet (FAULT_SAFETY = 27, FAULT_MAIN_1 = 21,
// FAULT_MAIN_2 = 22 -- all three rows say "owner file: (none yet)"), which
// is exactly the kind of "sixth independent guess" docs/HARDWARE.md section
// 0 item 2 warns against picking without updating that table in the same
// commit. This file does NOT update docs/HARDWARE.md (out of this pass's
// file scope) -- flagged loudly here and in this task's report instead, for
// whoever reconciles it. The pins:
//
//   GPIO27 -> BCLK   (collides with reserved FAULT_SAFETY)
//   GPIO28 -> WS     (the one true spare -- no conflict)
//   GPIO21 -> DIN_A  (collides with reserved FAULT_MAIN_1)
//   GPIO22 -> DIN_B  (collides with reserved FAULT_MAIN_2)
//
// GPIO19 (FAULT_MAIN_0) and GPIO0/1 (debug UART) were deliberately left
// untouched, so at least one FAULT line and the whole debug-UART pair stay
// available for their documented purposes if this driver is ever the one
// that ships. GPIO16/18/20 (ct_wave_pwm.c's 3 CT PWM carriers) and GPIO17
// (DRDY_MAIN_2) were left alone per this task's explicit instruction not to
// reuse ct_wave_pwm.c's pins -- that driver is "still live" even though
// this one exists to replace it eventually. If/when ct_wave_pwm.c is
// actually retired, GPIO16/18/20 becomes the obvious real fix for this
// section's conflict (3 GPIOs, exactly the number this driver borrows from
// the FAULT_* rows) -- noted here for whoever does that later, not acted on
// now.
//
// BCLK/WS MUST be two ADJACENT ascending GPIOs (27, 28): PIO side-set pins
// are always a contiguous block starting at a configured base. Checked
// below.
//
// --- The "two SMs, one program, no double-driven pins" approach ----------
//
// Both PIO1 state machines run the SAME program (ct_wave_i2s.pio) at the
// SAME clkdiv, started together via pio_enable_sm_mask_in_sync() so BCLK/WS
// (module A's SM) and module B's own internal bit/frame counter never
// drift relative to each other while both are running. Only module A's SM
// (sm_a) is ever given the side-set pins' OUTPUT ENABLE (via
// pio_sm_set_consecutive_pindirs(..., true) on GPIO27/28) -- module B's SM
// (sm_b) is configured with the SAME side-set base (27) purely because the
// SDK's config struct requires SOME base pin number even for a side-set the
// caller never intends to observe; sm_b's pindir for 27/28 is left at its
// reset default (input/Hi-Z) and gpio_set_function() for PIO1 on those pins
// is only ever effectively "claimed" by sm_a's own pindir call, so sm_b's
// side-set instructions have ZERO physical effect on any real pin -- there
// is no double-driving, only one SM's OE is ever asserted on GPIO27/28.
// This is the "map its clock pins somewhere harmless" option this task's
// brief asked for, done by leaving sm_b's copy of those pins un-enabled
// rather than by finding yet another physical GPIO neither SM actually
// needs (which this fixture's pin budget cannot afford anyway -- see
// above).
//
// The alternative the brief flagged as "acceptable and preferable on
// resource grounds" -- one SM, `out pins, 2` bit-interleaving both DIN_A
// and DIN_B into a single combined bitstream -- was considered and
// rejected: it would require the CPU (or the DMA source buffer) to
// pre-interleave module A's and module B's bits together AT THE BIT LEVEL
// before every transfer, which breaks the seam's own contract that this
// driver plays PLAIN per-module interleaved-int16 buffers straight off DMA
// with no CPU repacking step. The two-SM approach costs one extra
// (already-idle) PIO1 state machine and pays for it with buffers the caller
// can fill with an ordinary array-write access pattern; that trade favors
// legibility and matches the seam better here.
//
// --- BCLK / sample-rate arithmetic (EXACT -- unlike ct_wave_pwm.c's
// ~0.008% carrier error, worth showing why) --------------------------------
//
// UDA1334ATS datasheet p3: 16-bit samples, 16 kHz minimum supported rate --
// the fixture uses exactly that minimum, do not go lower. I2S needs
// BCLK = sample_rate * channels_per_frame(2) * bits_per_channel(16):
//
//   BCLK = 16,000 * 2 * 16 = 512,000 Hz
//
// This driver's PIO program spends exactly 2 SM instruction-cycles per
// BCLK half-period (one "out"/falling-edge instruction, one
// "jmp"-or-"set"/rising-edge instruction -- see ct_wave_i2s.pio's header),
// so one full BCLK period = 2 SM cycles, and the required SM instruction
// rate = 2 * BCLK = 1,024,000 Hz. sysclk stays at pico-sdk's stock 125 MHz
// (deliberate, documented decision -- not reconsidered here, no
// overclocking proposed):
//
//   clkdiv = sysclk / SM_cycle_rate = 125,000,000 / 1,024,000 = 122.0703125
//
// PIO clkdiv is a 16-bit-integer + 8-bit-fraction fixed-point value
// (granularity 1/256). 122.0703125 = 122 + 18/256 EXACTLY (18/256 =
// 0.0703125 with no rounding) -- one of the rare cases where the
// achievable-frequency grid actually contains the target exactly (compare
// ct_wave_pwm.c's own pacer, which could not land on an integer wrap at
// clkdiv=1 and accepted a +0.0077% error instead):
//
//   sysclk / (122 + 18/256) = 125,000,000 / 122.0703125 = 1,024,000 Hz exactly
//   BCLK = 1,024,000 / 2 = 512,000 Hz exactly
//   sample rate = BCLK / (2 channels * 16 bits) = 512,000 / 32 = 16,000 Hz exactly
//
// Zero frequency error, to the precision of the arithmetic above.
// sm_config_set_clkdiv_int_frac() is used below (not the float-taking
// clkdiv setter) specifically so this exact fixed-point value is what gets
// programmed, with no float -> fixed-point rounding step in between.
//
// --- DMA IRQ vector: none claimed --------------------------------------
// See ct_wave_i2s.h's SEAM CHOICE section -- both DMA IRQ vectors are
// already fully spoken for (DMA_IRQ_0: max31856_pio_engine.c, DMA_IRQ_1:
// ct_wave_pwm.c) and a third irq_set_exclusive_handler() call on either one
// panics at boot. This driver polls dma_channel_is_busy() from task context
// instead (ct_wave_i2s_poll()) and never touches hardware/irq.h.
#include "ct_wave_i2s.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "pico/time.h"

#include "drivers/simfw_fatal.h"

#include "ct_wave_i2s.pio.h"

// --- GPIO assignment (see file header) ------------------------------------
#define CT_WAVE_I2S_GPIO_BCLK  27u
#define CT_WAVE_I2S_GPIO_WS    28u
#define CT_WAVE_I2S_GPIO_DIN_A 21u
#define CT_WAVE_I2S_GPIO_DIN_B 22u

_Static_assert(CT_WAVE_I2S_GPIO_WS == CT_WAVE_I2S_GPIO_BCLK + 1u,
               "ct_wave_i2s.c: WS must be exactly one GPIO above BCLK -- "
               "PIO side-set pins are a contiguous block starting at the "
               "configured base (BCLK). See ct_wave_i2s.pio's .side_set 2 "
               "directive and this file's header.");

// --- PIO / clkdiv config (see file header arithmetic) ---------------------
#define CT_WAVE_I2S_CLKDIV_INT  122u
#define CT_WAVE_I2S_CLKDIV_FRAC 18u // 122 + 18/256 = 122.0703125 exactly -> 512.000 kHz BCLK, 16.000 kHz sample rate, zero error (see file header)

// The only PIO block with any free state machines -- PIO0 is completely
// full (bus A: 4 of 4 SMs, docs/HARDWARE.md section 1b.6 note 7). Note this
// block's PROGRAM MEMORY is nearly full too, unlike its SM count -- see
// ct_wave_i2s.h's header for the conflict this creates.
#define CT_WAVE_I2S_PIO pio1

typedef struct {
    int dma_chan;                                   // -1 until claimed
    uint sm;
    int16_t buf[2][CT_WAVE_I2S_SAMPLES_PER_BUFFER];  // ping/pong, interleaved L,R int16
    uint8_t playing;                                 // which of buf[0]/buf[1] the DMA channel is currently reading
    absolute_time_t armed_at;                        // when `playing` was last (re)armed -- underrun margin check
    uint32_t underrun_count;
} ct_wave_i2s_module_t;

static ct_wave_i2s_module_t s_module[CT_WAVE_I2S_NUM_MODULES];
static ct_wave_i2s_refill_fn s_refill;
static void *s_refill_ctx;
static uint s_pio_offset;

// Expected wall-clock duration of one buffer's playback. Used only for the
// underrun-margin check in ct_wave_i2s_poll() -- see
// ct_wave_i2s_get_underrun_count()'s header comment.
#define CT_WAVE_I2S_BUFFER_US \
    ((int64_t)CT_WAVE_I2S_FRAMES_PER_BUFFER * 1000000 / CT_WAVE_I2S_SAMPLE_RATE_HZ)

// Re-arms `module`'s DMA channel to stream buf[which_buf] into its PIO SM's
// TX FIFO, one int16 sample per DMA beat (see ct_wave_i2s.pio's header for
// why a 16-bit-wide transfer needs no repacking).
static void arm_dma(uint8_t module, uint8_t which_buf)
{
    ct_wave_i2s_module_t *m = &s_module[module];

    dma_channel_config c = dma_channel_get_default_config((uint)m->dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(CT_WAVE_I2S_PIO, m->sm, true /* is_tx */));

    dma_channel_configure((uint)m->dma_chan, &c,
                           &CT_WAVE_I2S_PIO->txf[m->sm], // write address: this SM's TX FIFO
                           m->buf[which_buf],             // read address: the buffer being handed to hardware
                           CT_WAVE_I2S_SAMPLES_PER_BUFFER, // one int16 per DMA beat, interleaved L,R, consumed in that order
                           true);                          // start now

    m->playing = which_buf;
    m->armed_at = get_absolute_time();
}

bool ct_wave_i2s_init(ct_wave_i2s_refill_fn refill, void *user_ctx)
{
    if (refill == NULL) {
        return false;
    }
    s_refill = refill;
    s_refill_ctx = user_ctx;

    // --- PIO program: check capacity BEFORE loading, per this project's
    // "non-panicking claim, explicit simfw_fatal() on failure" doctrine
    // (docs/HARDWARE.md section 1b.5). pio_add_program() alone would
    // hard-assert (panic) with a generic SDK message instead of naming the
    // actual word counts. THIS CHECK IS EXPECTED TO FAIL on the current
    // build -- see this file's header: bus B already occupies 29 of PIO1's
    // 32 instruction words, and this program needs 8.
    if (!pio_can_add_program(CT_WAVE_I2S_PIO, &ct_wave_i2s_out_program)) {
        simfw_fatal("ct_wave_i2s",
                     "PIO1 program memory exhausted: ct_wave_i2s_out needs "
                     "%u instruction word(s) but bus B "
                     "(max31856_pio_engine.c RX+TX_b) already occupies most "
                     "of PIO1's 32-word program memory -- docs/HARDWARE.md "
                     "section 1b.6 note 7 only accounts for state-machine "
                     "COUNT, not program memory (see ct_wave_i2s.h header)",
                     (unsigned)ct_wave_i2s_out_program.length);
    }
    s_pio_offset = pio_add_program(CT_WAVE_I2S_PIO, &ct_wave_i2s_out_program);

    // --- PIO state machines: 2 required = both of PIO1's free SMs --------
    int sm_a = pio_claim_unused_sm(CT_WAVE_I2S_PIO, false);
    if (sm_a < 0) {
        simfw_fatal("ct_wave_i2s",
                     "PIO1 state machine exhausted claiming module A's SM "
                     "(docs/HARDWARE.md section 1b.6 note 7: PIO1 should "
                     "have 2 free, bus B holds the other 2)");
    }
    int sm_b = pio_claim_unused_sm(CT_WAVE_I2S_PIO, false);
    if (sm_b < 0) {
        simfw_fatal("ct_wave_i2s",
                     "PIO1 state machine exhausted claiming module B's SM "
                     "(module A's SM %d already claimed this boot)", sm_a);
    }
    s_module[0].sm = (uint)sm_a;
    s_module[1].sm = (uint)sm_b;
    s_module[0].dma_chan = -1;
    s_module[1].dma_chan = -1;

    // --- GPIOs: only module A's SM (sm_a) is ever given ownership of
    // BCLK/WS's output-enable -- see file header "two SMs, one program"
    // note for why sm_b's identical side-set config never reaches a pin.
    pio_gpio_init(CT_WAVE_I2S_PIO, CT_WAVE_I2S_GPIO_BCLK);
    pio_gpio_init(CT_WAVE_I2S_PIO, CT_WAVE_I2S_GPIO_WS);
    pio_gpio_init(CT_WAVE_I2S_PIO, CT_WAVE_I2S_GPIO_DIN_A);
    pio_gpio_init(CT_WAVE_I2S_PIO, CT_WAVE_I2S_GPIO_DIN_B);
    pio_sm_set_consecutive_pindirs(CT_WAVE_I2S_PIO, (uint)sm_a, CT_WAVE_I2S_GPIO_BCLK, 2, true);
    pio_sm_set_consecutive_pindirs(CT_WAVE_I2S_PIO, (uint)sm_a, CT_WAVE_I2S_GPIO_DIN_A, 1, true);
    pio_sm_set_consecutive_pindirs(CT_WAVE_I2S_PIO, (uint)sm_b, CT_WAVE_I2S_GPIO_DIN_B, 1, true);
    // Deliberately NOT calling pio_sm_set_consecutive_pindirs() for sm_b on
    // GPIO27/28 -- that omission is what keeps sm_b's side-set instructions
    // from ever actually driving those pins (they stay input/Hi-Z, sm_a's
    // own drive is the only one that reaches the pad). See file header.

    for (uint8_t module = 0; module < CT_WAVE_I2S_NUM_MODULES; module++) {
        uint sm = s_module[module].sm;
        pio_sm_config cfg = ct_wave_i2s_out_program_get_default_config(s_pio_offset);

        sm_config_set_sideset_pins(&cfg, CT_WAVE_I2S_GPIO_BCLK); // same base for both SMs -- see file header; only sm_a's OE makes this physically real
        sm_config_set_out_pins(&cfg,
                                module == 0 ? CT_WAVE_I2S_GPIO_DIN_A : CT_WAVE_I2S_GPIO_DIN_B,
                                1);
        sm_config_set_out_shift(&cfg,
                                 false, // shift_left: OUT emits the OSR's current MSB first
                                 true,  // autopull
                                 16);   // one int16 sample per refill -- see ct_wave_i2s.pio's header
        sm_config_set_clkdiv_int_frac(&cfg, CT_WAVE_I2S_CLKDIV_INT, CT_WAVE_I2S_CLKDIV_FRAC);

        pio_sm_init(CT_WAVE_I2S_PIO, sm, s_pio_offset, &cfg);
        // Preload X=14 so the very first word out of reset already has the
        // correct 15-iteration bit count (see ct_wave_i2s.pio's header) --
        // without this the program's own "set x, 14" instructions would not
        // run until AFTER the first word, leaving X at whatever garbage
        // value pio_sm_init() left behind for that one word.
        pio_sm_exec(CT_WAVE_I2S_PIO, sm, pio_encode_set(pio_x, 14));
    }

    // --- DMA channels: one per module -------------------------------------
    for (uint8_t module = 0; module < CT_WAVE_I2S_NUM_MODULES; module++) {
        // required = false, checked explicitly, simfw_fatal() on exhaustion
        // -- same posture as ct_wave_pwm.c and max31856_pio_engine.c
        // (docs/HARDWARE.md section 1b.5). THIS IS EXPECTED TO FAIL on
        // module B's claim if ct_wave_pwm_init() already ran this boot:
        // only 1 of the RP2040's 12 DMA channels is free once ct_wave_pwm.c
        // (3) + bus A (5) + bus B (3) = 11 are budgeted, and this driver
        // needs 2. See ct_wave_i2s.h's MUTUAL EXCLUSION note.
        int chan = dma_claim_unused_channel(false);
        if (chan < 0) {
            simfw_fatal("ct_wave_i2s",
                         "DMA channel exhausted claiming module %u of %u "
                         "(docs/HARDWARE.md section 1b: only 1 of 12 "
                         "channels is free once ct_wave_pwm.c/bus A/bus B "
                         "are budgeted; ct_wave_i2s needs 2 -- the two CT "
                         "output backends cannot both be initialised in the "
                         "same boot, see ct_wave_i2s.h)",
                         (unsigned)module, (unsigned)CT_WAVE_I2S_NUM_MODULES);
        }
        s_module[module].dma_chan = chan;
        s_module[module].underrun_count = 0;
    }

    // --- Pre-fill both buffers of both modules before starting playback ---
    for (uint8_t module = 0; module < CT_WAVE_I2S_NUM_MODULES; module++) {
        s_refill(module, s_module[module].buf[0], CT_WAVE_I2S_FRAMES_PER_BUFFER, s_refill_ctx);
        s_refill(module, s_module[module].buf[1], CT_WAVE_I2S_FRAMES_PER_BUFFER, s_refill_ctx);
    }

    // --- Start both SMs together, then arm both DMA channels on buf[0] ----
    // pio_enable_sm_mask_in_sync() is what keeps sm_a/sm_b's bit/frame
    // counters from ever drifting apart -- see file header.
    uint32_t mask = (1u << s_module[0].sm) | (1u << s_module[1].sm);
    pio_enable_sm_mask_in_sync(CT_WAVE_I2S_PIO, mask);
    for (uint8_t module = 0; module < CT_WAVE_I2S_NUM_MODULES; module++) {
        arm_dma(module, 0);
    }

    return true;
}

void ct_wave_i2s_poll(void)
{
    for (uint8_t module = 0; module < CT_WAVE_I2S_NUM_MODULES; module++) {
        ct_wave_i2s_module_t *m = &s_module[module];

        if (dma_channel_is_busy((uint)m->dma_chan)) {
            continue; // still playing -- nothing to refill yet
        }

        // Idle-for-too-long check -- see ct_wave_i2s_get_underrun_count()'s
        // header comment. A margin of 2x the nominal buffer period (rather
        // than 1x) is used because poll() being called slightly late, well
        // under a full extra buffer's worth, is normal scheduling jitter,
        // not a stall.
        if (absolute_time_diff_us(m->armed_at, get_absolute_time()) >
            2 * CT_WAVE_I2S_BUFFER_US) {
            m->underrun_count++;
        }

        uint8_t idle_buf = (uint8_t)(1u - m->playing);
        s_refill(module, m->buf[idle_buf], CT_WAVE_I2S_FRAMES_PER_BUFFER, s_refill_ctx);
        arm_dma(module, idle_buf);
    }
}

uint32_t ct_wave_i2s_get_underrun_count(uint8_t module)
{
    if (module >= CT_WAVE_I2S_NUM_MODULES) {
        return 0;
    }
    return s_module[module].underrun_count;
}
