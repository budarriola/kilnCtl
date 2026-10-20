"""KG-01 browser check against the bench board (read-only apart from login).
Run: <python with playwright> kg01_browser_check.py <outdir>
Credentials come from User-scope env vars at runtime; never printed or screenshotted."""
import os, sys, json, subprocess, glob
from playwright.sync_api import sync_playwright

BASE = "http://192.168.1.156"
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

def uenv(name):
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
        "[Environment]::GetEnvironmentVariable('%s','User')" % name],
        capture_output=True, text=True)
    return r.stdout.strip()
USER = uenv("KILNCTL_WEB_USERNAME"); PW = uenv("KILNCTL_WEB_PASSWORD")
assert USER and PW, "credentials missing"
exe = glob.glob(os.path.expandvars(r"%LOCALAPPDATA%\ms-playwright\chromium_headless_shell-1234\**\chrome-headless-shell.exe"), recursive=True)[0]
res = {}
def shot(page, name): page.screenshot(path=os.path.join(OUT, name + ".png"))

def login_dialog(page):
    page.wait_for_selector("#kc-login-username", state="visible", timeout=15000)

def fill_and_submit(page):
    page.fill("#kc-login-username", USER); page.fill("#kc-login-password", PW)
    page.click(".kc-login-panel button[type=submit]")

def panel_colors(page):
    return page.evaluate("""()=>{const p=document.querySelector('.kc-login-panel');const o=document.querySelector('.kc-login-overlay');
      const a=getComputedStyle(p),b=getComputedStyle(o);return {panel_bg:a.backgroundColor,panel_fg:a.color,overlay_bg:b.backgroundColor,body_bg:getComputedStyle(document.body).backgroundColor}}""")

with sync_playwright() as p:
    br = p.chromium.launch(executable_path=exe)
    # --- 1 + 2: unauth, per scheme
    for scheme in ("light", "dark"):
        ctx = br.new_context(color_scheme=scheme, viewport={"width": 1000, "height": 800})
        page = ctx.new_page()
        main_resps = []
        page.on("response", lambda r: main_resps.append((r.url, r.status, r.headers.get("cache-control"))) if r.request.resource_type == "document" else None)
        page.goto(BASE + "/", wait_until="load"); page.wait_for_timeout(2500)
        title = page.title(); body = len(page.inner_text("body"))
        shot(page, "01_dashboard_" + scheme)
        if scheme == "light":
            res["1_dashboard"] = {"title": title, "body_chars": body, "dialog_open": page.is_visible("#kc-login-username")}
        # direct protected
        main_resps.clear()
        page.goto(BASE + "/diagnostics", wait_until="load"); 
        login_dialog(page); page.wait_for_timeout(500)
        diag_leak = page.evaluate("()=>!!document.getElementById('nvsCard')||!!document.getElementById('diagBundleBtn')")
        shot(page, "02_diagnostics_gate_" + scheme)
        res["2_" + scheme] = {"chain": main_resps[:3], "final_url": page.url, "protected_dom": diag_leak,
                              "colors": panel_colors(page), "dialog": True}
        ctx.close()
    # --- 3..6: light context, full flow
    ctx = br.new_context(color_scheme="dark", viewport={"width": 1000, "height": 900}, accept_downloads=True)
    page = ctx.new_page()
    shells = {}
    page.on("response", lambda r: shells.__setitem__(r.url, (r.status, r.headers.get("cache-control"))) if r.request.resource_type == "document" else None)
    page.goto(BASE + "/", wait_until="load"); page.wait_for_timeout(3000)
    link = page.query_selector("a[href='/profiles']") or page.query_selector("a[href='/settings/zones']")
    href = link.get_attribute("href")
    requests_before = []
    page.on("request", lambda r: requests_before.append(r.url) if r.resource_type == "document" else None)
    link.evaluate("e=>e.click()")
    login_dialog(page)
    res["3_prompt_first"] = {"href": href, "url_while_dialog": page.url, "navigated_before_login": any(href in u for u in requests_before)}
    fill_and_submit(page)
    page.wait_for_url("**" + href, timeout=20000); page.wait_for_load_state("load"); page.wait_for_timeout(1500)
    res["3_prompt_first"]["landed"] = page.url
    shot(page, "03_landed")
    # 4
    for path in ("/diagnostics", "/settings/zones", "/profiles", "/readiness"):
        r = page.goto(BASE + path, wait_until="load")
        res.setdefault("4_shells", {})[path] = {"status": r.status, "cache_control": r.headers.get("cache-control"), "url": page.url}
    # 6 on diagnostics
    page.goto(BASE + "/diagnostics", wait_until="load"); page.wait_for_timeout(6000)
    heads = page.evaluate("()=>[...document.querySelectorAll('th')].map(t=>t.textContent.trim())")
    txt = page.inner_text("body")
    import re
    nvs = [l for l in txt.splitlines() if "kiln_nvs" in l.lower()]
    cb = page.query_selector("#diagCoredump") 
    res["6_diag"] = {"has_Repeats": "Repeats" in heads, "has_Count": "Count" in heads, "kiln_nvs_lines": nvs[:6],
                     "bundle_href": page.get_attribute("#diagBundleBtn", "href")}
    shot(page, "06_diagnostics")
    dlres = {}
    try:
        with page.expect_download(timeout=60000) as d:
            page.click("#diagBundleBtn")
        dl = d.value; path = os.path.join(OUT, "support_bundle.kdiag"); dl.save_as(path)
        dlres["click_download"] = "ok"
    except Exception as e:
        dlres["click_download"] = "FAILED: " + str(e)[:120]
    rr = page.request.get(BASE + "/api/diag/bundle", timeout=90000)
    data = rr.body()
    open(os.path.join(OUT, "support_bundle_fetch.kdiag"), "wb").write(data)
    dlres.update({"fetch_status": rr.status, "ctype": rr.headers.get("content-type"), "cdisp": rr.headers.get("content-disposition"),
                  "bytes": len(data), "head": data[:8].decode("latin1"), "coredump_in_bytes": b"coredump" in data.lower()})
    res["6_diag"]["bundle"] = dlres
    # 5 logout then back
    page.goto(BASE + "/", wait_until="load"); page.wait_for_timeout(3000)
    page.goto(BASE + "/diagnostics", wait_until="load"); page.wait_for_timeout(1500)
    btn = page.query_selector(".kc-logout-btn")
    res["5_logout"] = {"logout_btn_visible": bool(btn and btn.is_visible())}
    if btn and btn.is_visible():
        btn.click(); page.wait_for_timeout(3000)
    res["5_logout"]["url_after_logout"] = page.url
    page.go_back(); page.wait_for_timeout(3000)
    leak = page.evaluate("()=>!!document.getElementById('nvsCard')||!!document.getElementById('diagBundleBtn')")
    res["5_logout"].update({"url_after_back": page.url, "protected_dom_after_back": leak,
                            "dialog_visible": page.is_visible("#kc-login-username")})
    shot(page, "05_after_back")
    # confirm session really dead
    st = page.evaluate("fetch('/api/auth/session').then(r=>r.text())")
    res["5_logout"]["session_after"] = st[:200]
    json.dump(res, open(os.path.join(OUT, "results.json"), "w"), indent=1)
    print(json.dumps(res, indent=1), flush=True)
    try:
        ctx.close(); br.close()
    except Exception: pass
