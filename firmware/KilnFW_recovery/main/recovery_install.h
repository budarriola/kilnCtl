// recovery_install.h -- unattended install of the staged update at recovery boot.
// Logic lives in update_install_record.c (update_install_boot_run, host-tested);
// this file is the raw-NVS adapter, the apply poll task and the status snapshot.
#ifndef RECOVERY_INSTALL_H
#define RECOVERY_INSTALL_H

#include <stdbool.h>

#include "update_install_record.h"

#ifdef __cplusplus
extern "C" {
#endif

// Run once per recovery boot after NVS init and after the LCD/HTTP are up.
// Starts the apply when a valid install request is present; on any refusal the
// failure is persisted, shown on the LCD, and recovery stays up.
void recovery_install_boot(void);

// Snapshot of the persisted result taken at boot and refreshed when this boot
// records one. Returns true when a result exists.
// Clears a leftover install request and result (operator exit / manual apply). 0 = request gone.
int recovery_install_supersede(void);

bool recovery_install_last_result(update_install_res_t *out);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_INSTALL_H
