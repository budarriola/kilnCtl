/* Companion to test_stub_cfg_fs_status_deps.c for executables that do not
 * define cfg_fs_is_available() themselves. */
#include "cfg_fs.h"
bool cfg_fs_is_available(void) { return false; }
