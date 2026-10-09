#pragma once
/* Per-store save lock for cfg stores whose saver can run on more than one task
 * (docs/audits/CFG_STORE_SAVE_RACE_2026-10-09.md). Same shape as
 * profiles_http.c's profiles_save_lock(): static storage, handle created lazily
 * under a claim flag (create runs outside the critical section), published with
 * release/acquire, the loser waits for the winner. Hold it across ONLY the
 * "read rev, commit, publish rev" section -- never across a producer call. It
 * is a leaf lock: nothing may be taken while holding it except pref_cfg_fs. */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
    portMUX_TYPE mux;
    bool claimed;
} cfg_save_lock_t;

#define CFG_SAVE_LOCK_INIT { .handle = NULL, .mux = portMUX_INITIALIZER_UNLOCKED, .claimed = false }

#if defined(__GNUC__)
#define CFG_SAVE_LOCK_LOAD_(l) __atomic_load_n(&(l)->handle, __ATOMIC_ACQUIRE)
#define CFG_SAVE_LOCK_STORE_(l, v) __atomic_store_n(&(l)->handle, (v), __ATOMIC_RELEASE)
#else
#define CFG_SAVE_LOCK_LOAD_(l) (*(SemaphoreHandle_t volatile *)&(l)->handle)
#define CFG_SAVE_LOCK_STORE_(l, v) (*(SemaphoreHandle_t volatile *)&(l)->handle = (v))
#endif

static inline void cfg_save_lock_take(cfg_save_lock_t *l)
{
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
}

static inline void cfg_save_lock_give(cfg_save_lock_t *l)
{
    (void)xSemaphoreGive(CFG_SAVE_LOCK_LOAD_(l));
}
