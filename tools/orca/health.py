import json, os, re, subprocess, sys, time
STALE_MIN = int(os.environ.get("STALE_MIN", "40"))
ERR = re.compile(r"Insufficient (account )?(funds|balance)|Upstream request failed|hit your usage limit|usage limit reached|"
                 r"rate[ _-]?limit(ed| exceeded)|quota exceeded|429 Too Many|401 Unauthorized|Invalid API key|"
                 r"credit balance is too low|We need title only, summarize|ECONNREFUSED|Authentication failed", re.I)
def orca(*a):
    out = subprocess.run(["orca", *a, "--json"], capture_output=True, text=True).stdout
    d = json.loads(out); return d.get("result", d)
def newest_mtime(path):
    best = 0
    for root, dirs, files in os.walk(path):
        dirs[:] = [x for x in dirs if x not in (".git", "node_modules")]
        for f in files:
            try: best = max(best, os.lstat(os.path.join(root, f)).st_mtime)
            except OSError: pass
    try: best = max(best, os.stat(os.path.join(path, ".git")).st_mtime)
    except OSError: pass
    return best
now = time.time()
for w in orca("orchestration", "worker-list").get("workers", []):
    if w.get("dispatchStatus") != "dispatched": continue
    pr = w.get("projection") or {}
    h = w.get("agentTerminalHandle"); ws = ((pr.get("workspace") or {}).get("id") or "").split("::", 1)[-1]
    agent = (pr.get("provider") or {}).get("id", "?"); did = w["dispatchId"]
    if not h or not ws: continue
    tail = ""
    try:
        r = orca("terminal", "read", "--terminal", h)
        t = r.get("terminal", r) if isinstance(r, dict) else {}
        tail = "\n".join(t.get("tail") or t.get("lines") or [])[-3000:]
        if not tail: tail = json.dumps(r)[-3000:]
    except Exception as e:
        print(f"STALL {did} {agent} {ws}: terminal unreadable ({e})"); continue
    m = ERR.search(tail[-1500:])
    if m: print(f"STALL {did} {agent} {ws}: agent error '{m.group(0)}'"); continue
    if w.get("terminalState") != "active":
        print(f"STALL {did} {agent} {ws}: terminal {w.get('terminalState')}"); continue
    idle = (now - newest_mtime(ws)) / 60
    if idle > STALE_MIN: print(f"STALL {did} {agent} {ws}: no file changes for {idle:.0f} min")
