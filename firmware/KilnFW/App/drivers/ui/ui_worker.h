// RF-01: one persistent LCD worker task shared by the PIN-verify and Wi-Fi UI jobs.
//
// Those jobs used to xTaskCreate() a 4-6 KB internal-RAM stack per tap and delete it
// afterwards; the churn fragmented the internal heap's largest free block (the update fetch
// needs a 6144 B block). This worker owns ONE statically allocated internal stack (no
// heap use for stack/TCB/queue, internal so NVS/flash writes stay legal) and runs submitted
// jobs one at a time. Jobs must return (never vTaskDelete(NULL)). Callers keep their own
// busy flags; the queue only serialises distinct jobs.
#ifndef UI_WORKER_H
#define UI_WORKER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ui_worker_fn_t)(void *arg);

/* Queue fn(arg) for the worker, starting the worker on first use. False when fn is NULL, the
 * worker cannot start, or the queue is full (caller shows its "could not start" status). */
bool ui_worker_submit(ui_worker_fn_t fn, void *arg);

#ifdef __cplusplus
}
#endif

#endif // UI_WORKER_H
