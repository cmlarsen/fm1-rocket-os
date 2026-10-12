import sys, time, json, subprocess
sys.argv += []
did, ws = sys.argv[1], sys.argv[2]
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 420
ns = {}; exec(open(__file__.rsplit("/", 1)[0] + "/health.py").read().split("now = time.time()")[0], ns)
t0 = time.time(); base = ns["newest_mtime"](ws)
while time.time() - t0 < limit:
    time.sleep(30)
    w = next((x for x in ns["orca"]("orchestration", "worker-list").get("workers", []) if x["dispatchId"] == did), None)
    if not w: print(f"START-FAIL {did}: dispatch not listed"); sys.exit(1)
    r = ns["orca"]("terminal", "read", "--terminal", w["agentTerminalHandle"])
    tail = "\n".join((r.get("terminal", r) or {}).get("tail") or [])
    m = ns["ERR"].search(tail[-1500:])
    if m: print(f"START-FAIL {did}: agent error '{m.group(0)}'"); sys.exit(1)
    if ns["newest_mtime"](ws) > base: print(f"START-OK {did}: worktree files changing after {time.time()-t0:.0f}s"); sys.exit(0)
print(f"START-FAIL {did}: no file changes in {limit}s"); sys.exit(1)
