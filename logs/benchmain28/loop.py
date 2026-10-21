import json,time,sys
from kilnctrl import dashboard_http_client as d, ota_http_client as o
H="192.168.1.156"; OUT="logs/benchmain28/resets.jsonl"
def snap():
    cr=d.get_crash_report(H); st=d.get_heap_status(H); bg=o.get_boot_guard_status(H)
    bh=cr.get("boot_history",[])
    return dict(t=time.strftime("%H:%M:%S"),uptime=st.get("uptime_s"),reset=st.get("reset_reason"),
      nvs=cr.get("kiln_nvs"),bh=[(e["seq"],e["repeat"]) for e in bh[:2]],nbh=len(bh),
      bg=bg,heap_min=st["heap_internal"]["min_free"],trips=[(t["seq"],t["count"]) for t in cr.get("trip_history",[])])
def wait_ready(limit=240):
    t0=time.time()
    while time.time()-t0<limit:
        try:
            s=snap()
            if (s["uptime"] or 0)>=45 and s["bg"].get("persisted_count",s["bg"].get("boot_count"))==0 and not s["bg"].get("recovery_mode"): return s
            if s["bg"].get("persisted_count",0)>=2 or s["bg"].get("recovery_mode"): return s
        except Exception as e: pass
        time.sleep(4)
    return None
def log(r):
    open(OUT,"a").write(json.dumps(r)+"\n"); print(json.dumps(r),flush=True)
s=wait_ready(); log(dict(i=0,**s))
for i in range(1,26):
    if s is None or s["bg"].get("persisted_count",0)>=2 or s["bg"].get("recovery_mode") or s["uptime"]<45:
        log(dict(i=i,STOP="guard",s=s)); sys.exit(1)
    try: r=o.sw_reset(H)
    except Exception as e: r=str(e)
    time.sleep(8)
    t0=time.time(); s=wait_ready()
    if s is None: log(dict(i=i,STOP="no board")); sys.exit(1)
    log(dict(i=i,sw=str(r)[:120],**s))
log(dict(done=True))
