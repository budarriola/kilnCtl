"""Tests for tools/check_no_handler_direct_driver_calls.py.

Pins `FW_WIFI_ALLOWLIST`'s exact contents (a reviewer noted nothing else
guards that allowlist from silently growing) and exercises the check's core
behaviors: a direct call is caught, a bare reference (function pointer /
`#define` alias) is caught, a comment or string mention is not, and an
allowlisted (file, function) pair is exempt while a different function in
the same file is not.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO_ROOT / "tools" / "check_no_handler_direct_driver_calls.py"

spec = importlib.util.spec_from_file_location("check_no_handler_direct_driver_calls", CHECK_PATH)
check_mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(check_mod)


def test_wifi_allowlist_pinned() -> None:
    """A new exemption must be a deliberate edit to this test, not a silent
    addition to the checker's allowlist. If this fails because someone added
    a genuinely justified entry, update the expected set here in the same
    change -- do not just delete this assertion."""
    assert check_mod.FW_WIFI_ALLOWLIST == {
        ("factory_reset.c", "execute_scope_job"),
        ("factory_reset.c", "log_net80211_key_count"),
    }, (
        "FW_WIFI_ALLOWLIST changed. If a new esp_wifi_*() direct-call exemption "
        "is genuinely justified (documented the way the existing two are in "
        "this checker's module docstring), update the expected set in this "
        "test deliberately. Do not let the allowlist grow silently."
    )


def _write(tmp_path: Path, filename: str, content: str) -> Path:
    target = tmp_path.joinpath(filename)
    target.write_text(content, encoding="utf-8")
    return target


def test_direct_call_is_caught(tmp_path: Path) -> None:
    src = """
void handler(void) {
    MAX31856_read_all(&buf);
}
"""
    target = _write(tmp_path, "fake_dashboard_http.c", src)
    violations = check_mod.check_file(target)
    assert violations, "expected a direct MAX31856_read_all() call to be flagged"
    assert "MAX31856_read_all" in violations[0]


def test_function_pointer_reference_is_caught(tmp_path: Path) -> None:
    src = """
static read_all_fn_t f = MAX31856_read_all;
"""
    target = _write(tmp_path, "fake_ota_http.c", src)
    violations = check_mod.check_file(target)
    assert violations, "expected a bare function-pointer reference to be flagged"
    assert "reference to" in violations[0]


def test_define_alias_is_caught(tmp_path: Path) -> None:
    src = """
#define RAW_READ_ALL MAX31856_read_all
"""
    target = _write(tmp_path, "fake_diagnostics_http.c", src)
    violations = check_mod.check_file(target)
    assert violations, "expected a #define alias to be flagged"
    assert "reference to" in violations[0]


def test_comment_and_string_mentions_are_not_flagged(tmp_path: Path) -> None:
    src = """
/* This handler no longer calls MAX31856_read_all() directly -- see the
 * owner accessor instead. */
void handler(void) {
    // kiln_io_read() used to be called here.
    ESP_LOGI(TAG, "calling MAX31856_read_all() is not allowed here");
    do_the_real_thing();
}
"""
    target = _write(tmp_path, "fake_clean_http.c", src)
    violations = check_mod.check_file(target)
    assert violations == []


def test_allowlisted_function_exempt_other_function_in_same_file_flagged() -> None:
    """factory_reset.c's execute_scope_job()/log_net80211_key_count() are
    allowlisted; a third, unrelated function in the same file calling
    esp_wifi_restore() directly must still be flagged."""
    src = """
void execute_scope_job(void) {
    esp_wifi_restore();
}

void log_net80211_key_count(void) {
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
}

void some_other_handler(void) {
    esp_wifi_restore();
}
"""
    import tempfile

    with tempfile.TemporaryDirectory() as d:
        target = Path(d) / "factory_reset.c"
        target.write_text(src, encoding="utf-8")
        violations = check_mod.check_file(target)

    assert len(violations) == 1, violations
    assert "some_other_handler" in violations[0]


def test_real_tree_has_no_violations() -> None:
    """End-to-end: the actual firmware/KilnFW/App/drivers/http/*.c tree, as
    fixed by HTTP_HANDLER_OWNERSHIP_PLAN Batch A."""
    violations: list[str] = []
    for path in check_mod.find_files(REPO_ROOT):
        violations.extend(check_mod.check_file(path))
    assert violations == [], violations
