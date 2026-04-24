#!/usr/bin/env bash
# test_regression.sh — regression tests for bugs fixed during development
# Covers: prune counter, name lookup, delete network cleanup,
#         inspect JSON validity, exec DRY path, metadata persistence.
# Run from project root: bash tests/test_regression.sh

set -euo pipefail
BIN="./container-sim"
ROOTFS="./rootfs/test-basic"
PASS=0; FAIL=0

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1 — $2"; ((++FAIL)); }

sim() { printf '%s\nexit\n' "$1" | "$BIN" 2>&1; }

echo "=== test_regression.sh ==="
echo ""

# ── 1. Name-based lookup ────────────────────────────────────────────────────
echo "--- name-based lookup ---"
out=$(printf "create myc myhost %s\nstart myc\nstop myc\ninspect myc\nexit\n" "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q '"Name".*"myc"'; then
    pass "inspect by name resolves correctly"
else
    fail "inspect by name" "did not find container by name"
fi

# ── 2. Prefer running container on duplicate name ───────────────────────────
echo ""
echo "--- duplicate name: prefer running ---"
out=$(printf "
runbg alpha myh1 %s /bin/sleep 9999
runbg alpha myh2 %s /bin/sleep 9999
stats alpha
exit
" "$ROOTFS" "$ROOTFS" | timeout 15 "$BIN" 2>&1)
running_count=$(echo "$out" | grep -c "RUNNING" || true)
if [ "$running_count" -ge 1 ]; then
    pass "stats alpha hits a running container"
else
    fail "stats alpha" "did not find running container by duplicate name"
fi

# ── 3. Prune resets counter past live containers ────────────────────────────
echo ""
echo "--- prune counter reset ---"
out=$(printf "
runbg prunekept myh %s /bin/sleep 9999
create pruned1 myh %s
delete pruned1
prune
list
exit
" "$ROOTFS" "$ROOTFS" | timeout 20 "$BIN" 2>&1)
reset_line=$(echo "$out" | grep "counter reset" || true)
if echo "$reset_line" | grep -qE "reset to [2-9]|reset to [0-9]{2,}"; then
    pass "prune resets counter above 1 when live containers exist"
else
    fail "prune counter" "counter not reset correctly: '$reset_line'"
fi

# ── 4. No ID collision after prune ──────────────────────────────────────────
echo ""
echo "--- no ID collision after prune ---"
out=$(printf "
runbg live myh %s /bin/sleep 9999
create dead myh %s
prune
runbg newc myh %s /bin/sleep 9999
list
exit
" "$ROOTFS" "$ROOTFS" "$ROOTFS" | timeout 20 "$BIN" 2>&1)
if echo "$out" | grep -q "already running"; then
    fail "ID collision" "duplicate ID after prune"
else
    pass "no ID collision after prune + new container"
fi

# ── 5. Delete cleans up network metadata ────────────────────────────────────
echo ""
echo "--- delete clears network metadata ---"
out=$(printf "
runbg netc myh %s /bin/sleep 9999
stop netc
delete netc
list
exit
" "$ROOTFS" | timeout 20 "$BIN" 2>&1)
# Only check the list output (lines after the last separator line)
list_section=$(echo "$out" | awk '/^ID.*NAME.*PID/{found=1} found{print}')
if echo "$list_section" | grep -q "netc"; then
    fail "delete network" "deleted container still appears in list output"
else
    pass "deleted container removed from list"
fi

# ── 6. inspect outputs valid JSON ───────────────────────────────────────────
echo ""
echo "--- inspect JSON validity ---"
out=$(printf "
create jsontest myhost %s
inspect jsontest
exit
" "$ROOTFS" | "$BIN" 2>&1)
json_block=$(echo "$out" | sed -n '/{/,/^}/p' | head -40)
if echo "$json_block" | python3 -c "import sys,json; json.loads(sys.stdin.read())" 2>/dev/null; then
    pass "inspect output is valid JSON"
else
    fail "inspect JSON" "output is not valid JSON"
fi

# ── 7. exec runs inside container namespace ──────────────────────────────────
echo ""
echo "--- exec runs in container ---"
out=$(printf "
runbg exectest myhost %s /bin/sleep 9999
exec exectest /bin/echo exec-ok
exit
" "$ROOTFS" | timeout 15 "$BIN" 2>&1)
if echo "$out" | grep -q "exec-ok"; then
    pass "exec output visible"
else
    fail "exec" "did not get expected output"
fi

# ── 8. Metadata persists across restarts ────────────────────────────────────
echo ""
echo "--- metadata persistence ---"
printf "runbg persist1 myh %s /bin/sleep 9999\nexit\n" "$ROOTFS" | timeout 10 "$BIN" > /dev/null 2>&1
out=$(printf "list\nexit\n" | timeout 5 "$BIN" 2>&1)
if echo "$out" | grep -q "persist1"; then
    pass "container persists across simulator restart"
else
    fail "metadata persistence" "container not found after restart"
fi
# cleanup
printf "prune\nexit\n" | timeout 5 "$BIN" > /dev/null 2>&1 || true

# ── 9. logs show output from runbg container ────────────────────────────────
echo ""
echo "--- logs capture ---"
out=$(printf "
runbg logtest myh %s /bin/sh -c \"echo hello-log; sleep 9999\"
logs logtest
exit
" "$ROOTFS" | timeout 15 "$BIN" 2>&1)
if echo "$out" | grep -q "hello-log"; then
    pass "logs capture container stdout"
else
    fail "logs" "expected 'hello-log' in log output"
fi

# ── Summary ──────────────────────────────────────────────────────────────────
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
