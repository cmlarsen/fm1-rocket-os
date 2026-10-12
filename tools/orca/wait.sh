#!/bin/bash
cd "$(dirname "$0")/../.."
SP="$(cd "$(dirname "$0")" && pwd)"; OUT="${TMPDIR:-/tmp}/orca_wait_last.json"; RUN="${ORCA_RUN:?set ORCA_RUN to the orchestration run id}"
ACK="${1:-}"
for i in $(seq 1 200); do
  if [ -n "$ACK" ]; then A="--ack $ACK"; else A=""; fi
  orca orchestration check --run $RUN $A --wait --types "worker_done,escalation,question" --timeout-ms 600000 --json 2>&1 | grep -v _keepalive > $OUT
  res=$(python3 - "$OUT" <<'PY'
import json,sys
s=open(sys.argv[1]).read()
try: d=json.loads(s)
except Exception: print("RAW");print(s[:2000]);sys.exit()
r=d.get("result",d);ms=r.get("messages",[])
if ms and all(m["type"]=="heartbeat" for m in ms): print("HB",r.get("deliveryId"));sys.exit()
if not ms: print("EMPTY");sys.exit()
print("REAL delivery",r.get("deliveryId"))
for m in ms:
  print("---",m["id"],m["type"],"|",m["subject"],"| from",m["from_handle"])
  print("payload",(m.get("payload") or "")[:400])
  print(m["body"][:1800])
PY
)
  hs=$(python3 $SP/health.py 2>&1)
  if [ -n "$hs" ]; then echo "$hs"; exit 0; fi
  case "$res" in
    HB*) ACK=$(echo "$res" | awk '{print $2}');;
    EMPTY*) ACK="";;
    *) echo "$res"; exit 0;;
  esac
done
echo "loop limit"
