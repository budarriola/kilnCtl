#pragma once
/* Per-store save lock for cfg stores whose saver can run on more than one task
 * (docs/audits/CFG_STORE_SAVE_RACE_2026-10-09.md). Same shape as
 * profiles_http.c's profiles_save_lock(): static storage, handle created lazily
 * under a claim flag (create runs outside the critical section), published with
 * release/acquire, the loser waits for the winner. Hold it across ONLY the
 * "read rev, commit, publish rev" section -- never across a producer call.
 * Inside it take only short RAM locks whose holders never wait on the flash
 * worker (a portMUX, a getter's own mutex), another cfg_save_lock_t (the
 * reservation below is recursive), and pref_cfg_fs / cfg_fs writes. Never a
 * lock whose holder can dispatch to the worker -- e.g. profile_executor's
 * s_exec.lock: read executor state BEFORE taking the save lock.
 *
 * FLASH-WORKER RESERVATION (2026-10-09): a saver can run both on httpd/backup
 * tasks AND as a job on bx_flash_worker (CONTROL SET_UNIT_PREF, PROFILES
 * SAVE/DELETE, SET_ZONE_PID/MODEL, AUTOTUNE ACCEPT), while the commit inside
 * the section dispatches onto that same worker. Holding the mutex off the
 * worker while the worker runs a job that blocks on it deadlocks both. So
 * take reserves the worker first (pref_cfg_fs_save_section_enter(), a no-op
 * on the worker itself) and give releases it last. The reservation works
 * before the worker task exists too: app_main creates the reservation lock
 * and installs the hooks first thing (uart_bridge_ext_save_reservation_init()).
 * Order: reservation outer, mutex inner. See pref_cfg_fs.h.
 *
 * FACTORY-RESET FENCE (2026-10-09): every take registers the lock once in pref_cfg_fs's registry so
 * cfg_save_barrier.c can take/give each one after the reset mark is set (barrier), and a writer calls
 * cfg_save_lock_reset_refused() INSIDE the section, immediately before its persist (refusal). Both are
 * documented in pref_cfg_fs.h. The refusal check adds no lock, so the nesting rule above is unchanged. */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pref_cfg_fs.h"

typedef struct {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
    portMUX_TYPE mux;
    bool claimed;
    bool registered; /* in pref_cfg_fs's lock registry (factory-reset barrier); set once under mux */
    bool reserved; /* written only by the holder, after the take; read by the holder before the give */
} cfg_save_lock_t;

#define CFG_SAVE_LOCK_INIT { .handle = NULL, .mux = portMUX_INITIALIZER_UNLOCKED, .claimed = false, .registered = false, .reserved = false }

#if defined(__GNUC__)
#define CFG_SAVE_LOCK_LOAD_(l) __atomic_load_n(&(l)->handle, __ATOMIC_ACQUIRE)
#define CFG_SAVE_LOCK_STORE_(l, v) __atomic_store_n(&(l)->handle, (v), __ATOMIC_RELEASE)
#else
#define CFG_SAVE_LOCK_LOAD_(l) (*(SemaphoreHandle_t volatile *)&(l)->handle)
#define CFG_SAVE_LOCK_STORE_(l, v) (*(SemaphoreHandle_t volatile *)&(l)->handle = (v))
#endif

static inline void cfg_save_lock_register_(cfg_save_lock_t *l)
{
    bool mine = false;
    portENTER_CRITICAL(&l->mux);
    if (!l->registered) {
        l->registered = true;
        mine = true;
    }
    portEXIT_CRITICAL(&l->mux);
    if (mine) {
        pref_cfg_fs_lock_registry_add(l);
    }
}

/* True while a factory reset is in flight and the caller is not the reset job itself. Call inside the
 * save section, right before the persist; on true give the lock and return ESP_ERR_INVALID_STATE. */
static inline bool cfg_save_lock_reset_refused(void)
{
    return pref_cfg_fs_reset_refuses_write();
}

static inline void cfg_save_lock_take(cfg_save_lock_t *l)
{
    cfg_save_lock_register_(l); /* BEFORE the take: see the fence note above */
    bool reserved = pref_cfg_fs_save_section_enter();
    if (CFG_SAVE_LOCK_LOAD_(l) == NULL) {
        bool mine = false;
        portENTER_CRITICAL(&l->mux);
        if (!l->claimed) {
            l->claimed = true;
            mine = true;
        }
        portEXIT_CRITICAL(&l->mux);
        if (mine) {
            CFG_SAVE_LOCK_STORE_(l, xSemaphoreCreateMutexStatic(&l->storage));
        } else {
            while (CFG_SAVE_LOCK_LOAD_(l) == NULL) {
                vTaskDelay(1);
            }
        }
    }
    (void)xSemaphoreTake(CFG_SAVE_LOCK_LOAD_(l), portMAX_DELAY);
    l->reserved = reserved;
}

static inline void cfg_save_lock_give(cfg_save_lock_t *l)
{
    bool reserved = l->reserved;
    l->reserved = false;
    (void)xSemaphoreGive(CFG_SAVE_LOCK_LOAD_(l));
    pref_cfg_fs_save_section_exit(reserved);
}
