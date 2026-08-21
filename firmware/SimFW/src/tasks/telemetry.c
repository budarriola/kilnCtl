// telemetry.c -- see telemetry.h. Real body: the periodic TELEMETRY frame
// (docs/DESIGN_NOTES.md section 5.3, default 2 Hz, rate settable) and the EVT-ring
// drain/forward (immediate, DESIGN_NOTES.md section 4.5/5.3). Byte layouts are
// docs/PROTOCOL.md section 6's authoritative copy; keep the two in sync in
// the same commit, same discipline this codebase asks everywhere else.
//
// Owns no peripheral: reads sim_engine's published snapshot/event ring
// (src/sim/sim_snapshot.h), spi_emu_a/b's instrumentation counters, fault_sched's
// slot list, and i2c_owner's relay/fault-line snapshot, then hands built
// frames to usb_owner_send_broadcast() (usb_owner.h) -- never touches USB
// CDC directly (single-owner-per-peripheral doctrine, DESIGN_NOTES.md section 4's
// opening paragraph).
#include "telemetry.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "benchproto/benchproto_frame.h"

#include "cmd_ids.h"
#include "fault_sched.h"
#include "i2c_owner.h"
#include "log_task.h"
#include "sim/sim_snapshot.h"
#include "sim_engine.h"
#include "spi_emu_a.h"
#include "spi_emu_b.h"
#include "task_priorities.h"
#include "usb_owner.h"

#define TELEMETRY_STACK_WORDS configMINIMAL_STACK_SIZE

// The event ring is drained on every pass of this loop, independent of the
// (possibly much slower) TELEMETRY frame rate, so an EVT frame is never
// held behind a slow telemetry period -- DESIGN_NOTES.md 5.3: EVT is "unsolicited,
// immediate". 20 ms (50 Hz) is comfortably faster than sim_engine's own
// 10 Hz tick (the fastest thing that can produce events), so a burst of a
// few events in one sim_engine tick still drains within one or two polls.
#define TELEMETRY_EVT_POLL_MS 20u

// Events drained per poll. SIM_EVENT_RING_SIZE is 256 (sim_snapshot.h); a
// generous per-poll cap bounds this loop's worst-case stack/time per
// iteration while still being far more than one sim_engine tick could ever
// produce in practice (a handful of relay edges/fault fires at once, not
// dozens).
#define TELEMETRY_EVT_DRAIN_MAX 16u

static TaskHandle_t s_task_handle = NULL;

static volatile uint32_t s_rate_hz = TELEMETRY_DEFAULT_RATE_HZ;

// Instrumentation -- single-word, cross-task readable like i2c_owner.h's
// own getters (telemetry.h's doc comment). Only ever written from this
// task's own loop, so no lock is needed for the writer side; a reader
// racing a write sees either the old or new value, never a torn one
// (uint32_t reads/writes are atomic on this target).
static volatile uint32_t s_evt_seq_gap_count = 0;
static volatile uint32_t s_send_drop_count = 0;
static volatile uint32_t s_evt_ring_hwm = 0;

// This task's own drain cursor into sim_engine's event ring -- sole reader,
// sole writer, no lock needed (sim_event_ring_drain() itself is documented
// safe for concurrent callers, but telemetry is the only one that exists).
static uint32_t s_evt_next_seq = 0;

// --- Little-endian payload packing ------------------------------------------
// benchproto's own header fields are big-endian (BENCHPROTO.md sec 3), but
// multi-byte fields INSIDE a payload are the project's little-endian
// convention (same section: "the natural layout for a memcpy into a
// float/double... or a Python struct.pack('<...')"). These helpers are
// telemetry-local rather than a shared utility: no other task in this pass
// builds multi-field binary payloads yet (cmd_task.c's handlers are small
// enough to hand-pack directly), and duplicating five one-line helpers is
// cheaper than introducing a new shared header this pass doesn't own.
static size_t put_u8(uint8_t *buf, size_t i, uint8_t v)
{
    buf[i] = v;
    return i + 1u;
}

static size_t put_u16le(uint8_t *buf, size_t i, uint16_t v)
{
    buf[i] = (uint8_t)(v & 0xFFu);
    buf[i + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    return i + 2u;
}

static size_t put_u32le(uint8_t *buf, size_t i, uint32_t v)
{
    buf[i] = (uint8_t)(v & 0xFFu);
    buf[i + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    buf[i + 2u] = (uint8_t)((v >> 16) & 0xFFu);
    buf[i + 3u] = (uint8_t)((v >> 24) & 0xFFu);
    return i + 4u;
}

static size_t put_u64le(uint8_t *buf, size_t i, uint64_t v)
{
    for (int b = 0; b < 8; b++) {
        buf[i + (size_t)b] = (uint8_t)((v >> (8 * b)) & 0xFFu);
    }
    return i + 8u;
}

static size_t put_f32le(uint8_t *buf, size_t i, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return put_u32le(buf, i, bits);
}

// --- Frame builders ----------------------------------------------------------

// docs/PROTOCOL.md section 6's TELEMETRY layout, verbatim.
static size_t build_telemetry_frame(uint8_t *buf, const sim_snapshot_t *snap, uint32_t seed,
                                     uint16_t active_fault_count, bool fault_line_asserted,
                                     uint32_t spi_transactions_total, uint32_t spi_underruns_total,
                                     uint32_t evt_ring_hwm, uint32_t evt_seq_gap_count,
                                     uint32_t evt_send_drop_count)
{
    size_t i = 0;
    i = put_u8(buf, i, SIMFW_EVT_FRAME_KIND_TELEMETRY);
    i = put_u64le(buf, i, snap->sim_time_us);
    i = put_u32le(buf, i, snap->timescale_x100);
    i = put_u32le(buf, i, seed);
    i = put_u8(buf, i, snap->zone_count);

    for (uint8_t z = 0; z < snap->zone_count && z < SIM_SNAPSHOT_MAX_ZONES; z++) {
        i = put_f32le(buf, i, snap->zones[z].T_true_c);
        i = put_f32le(buf, i, snap->zones[z].T_tc_reported_c);
        i = put_f32le(buf, i, snap->zones[z].T_safety_reported_c);
        i = put_f32le(buf, i, snap->zones[z].current_a);
    }

    i = put_u16le(buf, i, snap->relay_mask);
    i = put_u8(buf, i, snap->estop_open ? 1u : 0u);
    i = put_u8(buf, i, fault_line_asserted ? 1u : 0u);
    i = put_u16le(buf, i, active_fault_count);
    i = put_u32le(buf, i, spi_transactions_total);
    i = put_u32le(buf, i, spi_underruns_total);
    i = put_u32le(buf, i, evt_ring_hwm);
    i = put_u32le(buf, i, evt_seq_gap_count);
    i = put_u32le(buf, i, evt_send_drop_count);
    return i;
}

// docs/PROTOCOL.md section 6's EVT layout, verbatim.
static size_t build_evt_frame(uint8_t *buf, const sim_event_t *ev)
{
    size_t i = 0;
    i = put_u8(buf, i, SIMFW_EVT_FRAME_KIND_EVENT);
    i = put_u32le(buf, i, ev->seq);
    i = put_u64le(buf, i, ev->sim_time_us);
    i = put_u8(buf, i, (uint8_t)ev->type);
    i = put_u8(buf, i, ev->a);
    i = put_u8(buf, i, ev->b);
    i = put_f32le(buf, i, ev->f0);
    return i;
}

// --- Task --------------------------------------------------------------------

static void telemetry_drain_and_forward_events(void)
{
    sim_event_t events[TELEMETRY_EVT_DRAIN_MAX];
    uint32_t expected_seq = s_evt_next_seq;
    uint32_t n = sim_event_ring_drain(events, TELEMETRY_EVT_DRAIN_MAX, &s_evt_next_seq);

    if (n > s_evt_ring_hwm) {
        s_evt_ring_hwm = n;
    }

    if (n > 0 && events[0].seq != expected_seq) {
        // sim_event_ring_drain()'s own documented loss signal: the ring
        // wrapped past what we last drained. Count it (PROTOCOL.md section
        // 6's loss-visibility contract) and log it -- this is exactly the
        // "a run with a sequence gap must never be silently certified"
        // condition DESIGN_NOTES.md 5.3 calls out, so it is worth a line even
        // though the TELEMETRY frame's own counter already carries it.
        s_evt_seq_gap_count++;
        log_task_log(LOG_LEVEL_WARN, "telemetry", "event ring seq gap detected");
    }

    for (uint32_t k = 0; k < n; k++) {
        uint8_t buf[BENCHPROTO_FRAME_MAX_PAYLOAD];
        size_t len = build_evt_frame(buf, &events[k]);
        if (!usb_owner_send_broadcast(SIMFW_TASK_ID_EVT, buf, (uint8_t)len)) {
            s_send_drop_count++;
        }
    }
}

static void telemetry_build_and_send_state_frame(void)
{
    sim_snapshot_t snap;
    if (!sim_snapshot_read(&snap)) {
        // sim_engine has not published a snapshot yet (called before its
        // first tick) -- nothing to report this period, try again next.
        return;
    }

    fault_slot_t slots[FAULT_ENGINE_MAX_SLOTS];
    size_t slot_count = fault_sched_list(slots, FAULT_ENGINE_MAX_SLOTS);
    uint16_t active_fault_count = 0;
    for (size_t s = 0; s < slot_count; s++) {
        if (slots[s].state == FAULT_STATE_ACTIVE) {
            active_fault_count++;
        }
    }

    max31856_pio_stats_t a0 = spi_emu_a_get_stats(0);
    max31856_pio_stats_t a1 = spi_emu_a_get_stats(1);
    max31856_pio_stats_t a2 = spi_emu_a_get_stats(2);
    max31856_pio_stats_t b0 = spi_emu_b_get_stats(0);
    uint32_t spi_transactions_total = a0.transactions + a1.transactions + a2.transactions + b0.transactions;
    uint32_t spi_underruns_total = a0.first_byte_late + a1.first_byte_late + a2.first_byte_late + b0.first_byte_late;

    i2c_owner_relay_states_t relays = i2c_owner_get_relay_states();

    // seed: gap-closure pass -- sim_engine_get_seed() now exists and
    // cmd_task.c's SYS_SET_SEED handler is wired up, so this reports
    // whatever the PC side last set (0 if never set), closing the "seed is
    // always 0" gap docs/PROTOCOL.md section 6 previously documented.
    uint32_t seed = sim_engine_get_seed();

    uint8_t buf[BENCHPROTO_FRAME_MAX_PAYLOAD];
    size_t len = build_telemetry_frame(buf, &snap, seed, active_fault_count, relays.fault_line_asserted,
                                        spi_transactions_total, spi_underruns_total, s_evt_ring_hwm,
                                        s_evt_seq_gap_count, s_send_drop_count);

    if (!usb_owner_send_broadcast(SIMFW_TASK_ID_EVT, buf, (uint8_t)len)) {
        s_send_drop_count++;
    }
}

static void telemetry_task_fn(void *arg)
{
    (void)arg;

    TickType_t next_state_frame_tick = xTaskGetTickCount();

    for (;;) {
        // EVT frames are immediate -- drained/forwarded every poll,
        // independent of the (possibly much slower) TELEMETRY rate below.
        telemetry_drain_and_forward_events();

        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_state_frame_tick) >= 0) {
            telemetry_build_and_send_state_frame();

            uint32_t hz = s_rate_hz;
            if (hz == 0 || hz > TELEMETRY_MAX_RATE_HZ) {
                hz = TELEMETRY_DEFAULT_RATE_HZ;
            }
            TickType_t period = pdMS_TO_TICKS(1000u / hz);
            if (period == 0) {
                period = 1;
            }
            next_state_frame_tick = now + period;
        }

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_EVT_POLL_MS));
    }
}

bool telemetry_start(void)
{
    s_rate_hz = TELEMETRY_DEFAULT_RATE_HZ;
    s_evt_seq_gap_count = 0;
    s_send_drop_count = 0;
    s_evt_ring_hwm = 0;
    s_evt_next_seq = 0;

    BaseType_t ok = xTaskCreate(telemetry_task_fn, "telemetry", TELEMETRY_STACK_WORDS, NULL,
                                 SIMFW_PRIO_TELEMETRY, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_ELASTIC_PATH);
    return true;
}

bool telemetry_set_rate_hz(uint32_t hz)
{
    if (hz == 0 || hz > TELEMETRY_MAX_RATE_HZ) {
        return false;
    }
    s_rate_hz = hz;
    return true;
}

uint32_t telemetry_get_rate_hz(void)
{
    return s_rate_hz;
}

uint32_t telemetry_get_evt_seq_gap_count(void)
{
    return s_evt_seq_gap_count;
}

uint32_t telemetry_get_send_drop_count(void)
{
    return s_send_drop_count;
}

uint32_t telemetry_get_evt_ring_high_water_mark(void)
{
    return s_evt_ring_hwm;
}
