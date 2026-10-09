#pragma once
/* Save-section probe for host tests (docs/audits/CFG_STORE_SAVE_RACE_2026-10-09.md,
 * "Save mutex vs. flash worker").
 *
 * On the device, every cfg store save lock (cfg_save_lock.h) reserves the
 * flash worker BEFORE taking its mutex and releases the reservation AFTER
 * giving it, via pref_cfg_fs_save_section_enter()/_exit(). That ordering is
 * the whole deadlock fix: a task that holds a save mutex always holds the
 * worker reservation too, so no worker job can be running (and blocked on
 * that mutex) while the task waits on the worker for its commit.
 *
 * This probe installs recording hooks and a recording pref_cfg_fs write fn,
 * and checks the shape mechanically:
 *   - enter runs with no lock held (the stub's g_test_stub_lock_depth is at
 *     its baseline), so the reservation was taken before the save mutex;
 *   - exit runs with the lock already given;
 *   - every cfg write happens inside a reservation;
 *   - enter/exit balance.
 * on_enter, when set, runs inside the enter hook -- a test uses it to see
 * what RAM held before the save mutex was taken. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cfg_fs.h"
#include "freertos/semphr.h"
#include "pref_cfg_fs.h"

typedef struct {
    int base_lock_depth;
    int enters;
    int exits;
    int depth;
    int writes;
    int writes_outside_section;
    int enter_with_lock_held;
    int exit_with_lock_held;
    void (*on_enter)(void);
} save_section_probe_t;

static save_section_probe_t g_ssp;

static bool ssp_enter(void)
{
    g_ssp.enters++;
    if (g_test_stub_lock_depth != g_ssp.base_lock_depth) {
        g_ssp.enter_with_lock_held++;
    }
    if (g_ssp.on_enter) {
        g_ssp.on_enter();
    }
    g_ssp.depth++;
    return true;
}

static void ssp_exit(bool reserved)
{
    (void)reserved;
    g_ssp.exits++;
    if (g_test_stub_lock_depth != g_ssp.base_lock_depth) {
        g_ssp.exit_with_lock_held++;
    }
    g_ssp.depth--;
}

static esp_err_t ssp_write(const char *rel_path, const void *data, size_t len)
{
    g_ssp.writes++;
    if (g_ssp.depth <= 0) {
        g_ssp.writes_outside_section++;
    }
    return cfg_fs_write_atomic(rel_path, data, len);
}

static void ssp_install(void)
{
    memset(&g_ssp, 0, sizeof(g_ssp));
    g_ssp.base_lock_depth = g_test_stub_lock_depth;
    pref_cfg_fs_set_save_section_hooks(ssp_enter, ssp_exit);
    pref_cfg_fs_set_write_fn(ssp_write);
}

static void ssp_uninstall(void)
{
    pref_cfg_fs_set_save_section_hooks(NULL, NULL);
    pref_cfg_fs_reset_write_fn_for_test();
}

/* True when every section seen was well formed and at least `min_writes`
 * cfg writes happened inside one. */
static bool ssp_shape_ok(int min_writes)
{
    return g_ssp.enters >= 1 && g_ssp.enters == g_ssp.exits && g_ssp.depth == 0 &&
           g_ssp.enter_with_lock_held == 0 && g_ssp.exit_with_lock_held == 0 &&
           g_ssp.writes >= min_writes && g_ssp.writes_outside_section == 0;
}
