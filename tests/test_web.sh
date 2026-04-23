#!/usr/bin/env bash
# test_web.sh — REST API smoke tests for the embedded web dashboard
# Requires curl; skips gracefully if not available.

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
PORT=18080   # use a non-default port to avoid collisions

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

echo "=== test_web.sh ==="
echo ""

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not found — skipping web API tests"
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed (skipped) ==="
    exit 0
fi

# Start the simulator in background with a web server on PORT
SIM_PID=""
cleanup() {
    [ -n "$SIM_PID" ] && kill "$SIM_PID" 2>/dev/null || true
    wait "$SIM_PID" 2>/dev/null || true
}
trap cleanup EXIT

(printf "web %d\n" "$PORT"; sleep 6; printf "exit\n") | "$BIN" >/dev/null 2>&1 &
SIM_PID=$!

# Wait for server to be ready (up to 3 s)
for i in $(seq 1 15); do
    curl -sf "http://localhost:${PORT}/" >/dev/null 2>&1 && break
    sleep 0.2
done

echo "--- static files ---"
code=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${PORT}/")
if [ "$code" = "200" ]; then
    pass "GET / returns 200"
else
    fail "GET / returned $code"
fi

code=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${PORT}/style.css")
if [ "$code" = "200" ]; then
    pass "GET /style.css returns 200"
else
    fail "GET /style.css returned $code"
fi

code=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${PORT}/app.js")
if [ "$code" = "200" ]; then
    pass "GET /app.js returns 200"
else
    fail "GET /app.js returned $code"
fi

echo ""
echo "--- API endpoints ---"

body=$(curl -sf "http://localhost:${PORT}/api/containers" 2>/dev/null || echo "ERROR")
if echo "$body" | grep -qE '^\['; then
    pass "GET /api/containers returns JSON array"
else
    fail "GET /api/containers did not return JSON array: $body"
fi

body=$(curl -sf "http://localhost:${PORT}/api/events?n=5" 2>/dev/null || echo "ERROR")
if echo "$body" | grep -qE '^\['; then
    pass "GET /api/events returns JSON array"
else
    fail "GET /api/events did not return JSON array: $body"
fi

body=$(curl -sf "http://localhost:${PORT}/api/metrics" 2>/dev/null || echo "ERROR")
if echo "$body" | grep -q '"uptime_seconds"'; then
    pass "GET /api/metrics returns uptime_seconds"
else
    fail "GET /api/metrics missing uptime_seconds: $body"
fi

body=$(curl -sf "http://localhost:${PORT}/api/alerts" 2>/dev/null || echo "ERROR")
if echo "$body" | grep -qE '^\['; then
    pass "GET /api/alerts returns JSON array"
else
    fail "GET /api/alerts did not return JSON array: $body"
fi

body=$(curl -sf "http://localhost:${PORT}/api/stats" 2>/dev/null || echo "ERROR")
if echo "$body" | grep -qE '^\{'; then
    pass "GET /api/stats returns JSON object"
else
    fail "GET /api/stats did not return JSON object: $body"
fi

echo ""
echo "--- realtime state refresh ---"
cleanup

(printf "web %d\n" "$PORT"; printf "runbg webtest webhost ./rootfs/test-basic /bin/sleep 1\n"; sleep 8; printf "exit\n") | "$BIN" >/dev/null 2>&1 &
SIM_PID=$!

for i in $(seq 1 25); do
    body=$(curl -sf "http://localhost:${PORT}/api/containers" 2>/dev/null || true)
    if echo "$body" | grep -q '"name":"webtest"'; then
        break
    fi
    sleep 0.2
done

sleep 2
body=$(curl -sf "http://localhost:${PORT}/api/containers" 2>/dev/null || echo "ERROR")
if echo "$body" | grep -q '"name":"webtest".*"state":"STOPPED"'; then
    pass "API refreshes container state after background exit"
else
    fail "container state stayed stale after exit: $body"
fi

echo ""
echo "--- CORS headers ---"
header=$(curl -sI "http://localhost:${PORT}/api/containers" | grep -i "Access-Control" || true)
if echo "$header" | grep -qi "Access-Control-Allow-Origin"; then
    pass "CORS header present on API response"
else
    fail "CORS header missing"
fi

echo ""
echo "--- 404 for unknown routes ---"
code=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${PORT}/api/does-not-exist")
if [ "$code" = "404" ]; then
    pass "Unknown route returns 404"
else
    fail "Unknown route returned $code (expected 404)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
