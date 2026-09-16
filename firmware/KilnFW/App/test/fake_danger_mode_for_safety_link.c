// Minimal stand-ins for danger_mode_active() and heat_enable_is_held(),
// used only by kilnctl_host_tests_safety_link.exe (test_safety_link_
// compile.c pulls in safety_link_frames.c, which since 2026-09-15 (Opus
// re-review N1) calls both from safety_build_and_send_context()). The real
// danger_mode.c pulls in kiln_io_owner.h/profile_executor_state.h, and the
// real heat_enable.c defines heat_enable_service_pending_release() -- which
// this test file already fakes itself (see its own header comment) -- so
// linking either real .c would either widen this executable's established
// stub surface or collide (LNK2005) with that existing fake. Both are out
// of scope for the two static wire-decode decisions this executable
// targets; a fixed-false/fixed-not-held fake is enough to satisfy the
// linker. Real coverage of context-flag production lives in test_heat_
// enable.c (heat_enable.c, real) and any future dedicated danger-mode host
// test.
#include <stdbool.h>
#include "heat_enable.h"

bool danger_mode_active(void)
{
    return false;
}

bool heat_enable_is_held(heat_enable_claimant_t who)
{
    (void)who;
    return false;
}
