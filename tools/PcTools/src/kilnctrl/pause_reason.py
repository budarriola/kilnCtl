"""Human-readable text for /api/profile_exec's ``pause_reason``.

The field is emitted only by the HTTP JSON (dashboard_exec_http.c); the serial PROFILES status frame does not
carry it. Wording mirrors ui_page_home_pause_reason_text() (LCD) and main_page.html's pauseReasonText().
"""
from __future__ import annotations

_KNOWN = {
    "pico_reboot_undecided": "Heat withheld: the safety processor rebooted and the cause is not yet known",
    "pico_fatal_reboot": "Paused: the safety processor rebooted after a fatal fault",
    "heat_grant_unconfirmed": "Paused: heat grant not confirmed by the safety processor",
}


def pause_reason_text(reason) -> str:
    """'' for no reason; known codes get a sentence; unknown codes are shown verbatim, never hidden."""
    if not reason:
        return ""
    reason = str(reason)
    return _KNOWN.get(reason, f"Paused: {reason}")
