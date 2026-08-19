// tx_watermark.h -- pure decision logic for the TX-reserve watermark that
// protects telemetry from log-frame displacement (docs/ARCHITECTURE.md
// section 1: "a log frame must never be able to displace a telemetry
// frame"; CommonFW/docs/LINK_PROTOCOL.md section 6/7's telemetry-required
// framing; ROADMAP.md M5: "TX ring reserves capacity for telemetry; log
// frames dropped above the watermark and the drops counted").
//
// Deliberately free of pico-sdk/FreeRTOS and of uart_owner.h: `fill` is the
// TX ring's used/capacity fraction the caller already has
// (link_task_get_tx_ring_fill_fraction() -- log_task.c never talks to
// uart_owner directly, only through link_task's wrapper, same boundary
// uart_owner.c's own header comment describes), so this function can be
// exercised from a plain host build
// (firmware/SaftyFW/test/build_host_tests.ps1) the same way
// safety_guards.c's pure predicates already are. log_task.c is the only
// production caller; it still owns the drop counter (log_task_get_dropped())
// and the reserve-fraction constant (LOG_TX_RESERVE_FRACTION) -- this file
// only answers "admit or drop", never touches state.
#ifndef SAFTYFW_TASKS_TX_WATERMARK_H
#define SAFTYFW_TASKS_TX_WATERMARK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// True if a log frame should be DROPPED (never sent) given the TX ring's
// current fill fraction (0.0 = empty, 1.0 = full). `reserve_fraction` is the
// fraction of the ring's capacity reserved exclusively for telemetry (Frame
// A/B/D/E) -- once `fill` reaches or exceeds it, log frames are refused so
// the remaining headroom stays available for the next telemetry send, which
// never consults this function at all (link_task.c's Frame A/B/D/E sends go
// straight to uart_owner_send(), unconditionally).
bool tx_watermark_should_drop_log(float fill, float reserve_fraction);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_TX_WATERMARK_H
