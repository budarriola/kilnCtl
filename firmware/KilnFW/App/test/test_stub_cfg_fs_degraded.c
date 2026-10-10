/* Link stub for executables that compile cfg_fs_status.c / cfg_fs_refusal_http.h callers without the real
 * cfg_fs.c (persfx MED-1 degraded-store registry). Reports "nothing degraded" unless a test registers a name with
 * test_stub_cfg_fs_degraded_set() (persfx3 MED-2: reaches the 409 branch of cfg_fs_http_persist_failed_for()). */
#include <string.h>
#include "cfg_fs.h"

static char s_name[48];

void test_stub_cfg_fs_degraded_set(const char *name)
{
    if (name) {
        strncpy(s_name, name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    } else {
        s_name[0] = '\0';
    }
}

int cfg_fs_degraded_count(void) { return s_name[0] ? 1 : 0; }
bool cfg_fs_degraded_is(const char *name) { return name && s_name[0] && strcmp(name, s_name) == 0; }
bool cfg_fs_degraded_name(int idx, char *out, size_t cap)
{
    if (out && cap) {
        out[0] = '\0';
    }
    if (idx == 0 && s_name[0] && out && cap) {
        strncpy(out, s_name, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }
    return false;
}
