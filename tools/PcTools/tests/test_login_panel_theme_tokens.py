"""KT-01: the sign-in dialog panel must use theme-aware tokens, not the fixed dark --ui-* ones."""
import re
from pathlib import Path

CSS = Path(__file__).resolve().parents[3] / "firmware/KilnFW/App/drivers/http/theme.css"


def _rule(selector: str) -> str:
    m = re.search(re.escape(selector) + r"\s*\{([^}]*)\}", CSS.read_text(encoding="utf-8"))
    assert m, selector
    return m.group(1)


def test_panel_uses_theme_aware_tokens():
    body = _rule(".kc-login-panel")
    assert "var(--card-bg)" in body and "color: var(--fg)" in body
    assert "--ui-card" not in body and "--ui-text-primary" not in body


def test_panel_label_uses_theme_aware_color():
    body = _rule(".kc-login-panel label")
    assert "var(--fg)" in body and "--ui-text-secondary" not in body
