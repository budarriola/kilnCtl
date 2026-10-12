// recovery_http_policy.h -- pure decisions of recovery_http.c's boot_guard and
// Wi-Fi reset routes, free of ESP-IDF types so a host test can exercise them
// (check_recovery_wifi_policy.ps1, which also source-scans recovery_http.c).
#ifndef RECOVERY_HTTP_POLICY_H
#define RECOVERY_HTTP_POLICY_H

#include <stdbool.h>
#include <stddef.h>

// The Wi-Fi reset may report "cleared" only when every wifi_nvs key erase
// succeeded, the commit succeeded, AND the app's legacy default-partition copy
// (which wifi_prov_migrate_from_default_partition() would re-adopt after the
// reset erased saved_nets) is gone or was never reachable (rc 0).
static inline bool rhp_wifi_reset_ok(size_t erase_failed, int commit_rc, int legacy_rc)
{
    return erase_failed == 0 && commit_rc == 0 && legacy_rc == 0;
}

#endif // RECOVERY_HTTP_POLICY_H
