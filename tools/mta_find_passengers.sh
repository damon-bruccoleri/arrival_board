#!/usr/bin/env bash
set -euo pipefail

STOP_ID="${1:-}"
if [ -z "$STOP_ID" ]; then
  echo "Usage: $0 <STOP_ID>   (example: $0 501627)" >&2
  exit 2
fi

# Load env if present
if [ -f /home/damon/arrival_board/arrival_board.env ]; then
  set -a
  . /home/damon/arrival_board/arrival_board.env
  set +a
fi

: "${MTA_KEY:?ERROR: MTA_KEY is not set (check /home/damon/arrival_board/arrival_board.env)}"

URL="https://bustime.mta.info/api/siri/stop-monitoring.json?key=${MTA_KEY}&version=2&OperatorRef=MTA&MonitoringRef=${STOP_ID}&MaximumStopVisits=60&StopMonitoringDetailLevel=normal"

OUT="/tmp/mta_stop_${STOP_ID}.json"
echo "Fetching:"
echo "$URL"
echo
curl -fsS "$URL" -o "$OUT"
echo "Saved: $OUT"
echo

python3 - "$OUT" <<'PY'
import json,sys,re
path=sys.argv[1]
j=json.load(open(path,"r",encoding="utf-8",errors="replace"))

want=re.compile(r"(passenger|occup|capacity|loadfactor|crowd|rider|ridership)", re.I)

def walk(x,p="$"):
    if isinstance(x,dict):
        for k,v in x.items():
            np=f"{p}.{k}"
            if want.search(k):
                print(np, "=>", repr(v)[:200])
            walk(v,np)
    elif isinstance(x,list):
        for i,v in enumerate(x):
            walk(v,f"{p}[{i}]")
walk(j)
PY
