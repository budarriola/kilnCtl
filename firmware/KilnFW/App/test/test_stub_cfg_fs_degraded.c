/* Link stub for executables that compile cfg_fs_status.c / cfg_fs_refusal_http.h callers without the real
 * cfg_fs.c (persfx MED-1 degraded-store registry). Reports "nothing degraded". */
#include "cfg_fs.h"

int cfg_fs_degraded_count(void) { return 0; }
bool cfg_fs_degraded_name(int idx, char *out, size_t cap)
{
    (void)idx;
    if (out && cap) {
        out[0] = '\0';
    }
    return false;
}
