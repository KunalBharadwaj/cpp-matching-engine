#!/usr/bin/env bash
# End-to-end smoke test for the order-entry gateway: start the server, run the
# scripted client against it, and check the exchange produced a risk rejection
# and routed fills. Assumes `cmake --build build --target gateway_server
# gateway_client` has been run.
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${1:-7799}"
LOG="$(mktemp)"

./build/gateway_server "$PORT" >"$LOG" 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null || true; rm -f "$LOG"' EXIT

for _ in $(seq 1 50); do grep -q listening "$LOG" && break; sleep 0.1; done

OUT="$(./build/gateway_client "$PORT")"
echo "$OUT"

if echo "$OUT" | grep -q "RJCT" && echo "$OUT" | grep -q "FILL" && echo "$OUT" | grep -q "CXLD"; then
    echo "gateway smoke: OK"
else
    echo "gateway smoke: FAILED"; exit 1
fi
