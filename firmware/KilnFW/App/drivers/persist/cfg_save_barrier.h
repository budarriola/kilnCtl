#pragma once
/* Factory-reset barrier (see pref_cfg_fs.h "FACTORY-RESET WRITER FENCE"). Call after
 * relay_authority_reset_in_flight_begin() and before the erase, from a task holding no save lock and
 * not on the flash worker: takes and gives every registered cfg_save_lock_t once, in registration
 * order, never nested. Returns after every writer that was inside a save section has left it. */
void persist_reset_barrier(void);
