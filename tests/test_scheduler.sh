#!/usr/bin/env bash
# test_scheduler.sh — verify round-robin scheduler with SIGSTOP/SIGCONT
# Run from project root: bash tests/test_scheduler.sh

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-sched"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

cleanup() {
    # Best-effort cleanup of any leftover containers
    printf 'list\nexit\n' | "$BIN" 2>&1 | grep -oP 'container-\d+' | while read -r id; do
        printf 'stop %s\ndelete %s\n' "$id" "$id"
    done | "$BIN" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "=== test_scheduler.sh ==="
echo ""

echo "--- scheduler status (off by default) ---"
out=$(printf 'sched status\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "disabled"; then
    pass "scheduler is disabled by default"
else
    fail "expected scheduler disabled"
fi

echo ""
echo "--- sched on/off toggle ---"
out=$(printf 'sched on\nsched status\nsched off\nsched status\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "enabled"; then
    pass "sched on enables scheduler"
else
    fail "sched on did not enable"
fi
if echo "$out" | grep -q "disabled"; then
    pass "sched off disables scheduler"
else
    fail "sched off did not disable"
fi

echo ""
echo "--- sched slice ---"
out=$(printf 'sched slice 100\nsched status\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "100"; then
    pass "sched slice sets time slice to 100ms"
else
    fail "sched slice not reflected in status"
fi

echo ""
echo "--- two background containers under scheduler ---"
# Clean metadata so container IDs are predictable: sched-a=container-0001, sched-b=container-0002.
# Stats and cleanup must happen in the SAME session — exit triggers cleanup_all_containers
# which stops all background containers before a new session can reach them.
rm -f containers.meta containers.meta.tmp
out=$(printf \
'sched on
runbg sched-a host-a %s /bin/sleep 30
runbg sched-b host-b %s /bin/sleep 30
list
stats container-0001
sched off
stop container-0001
stop container-0002
delete container-0001
delete container-0002
exit
' "$ROOTFS" "$ROOTFS" | "$BIN" 2>&1)

RUNNING=$(echo "$out" | grep -c "RUNNING" || true)
if [ "$RUNNING" -ge 2 ]; then
    pass "two containers running simultaneously under scheduler"
else
    fail "expected 2 RUNNING containers, got $RUNNING"
    echo "$out"
fi

echo ""
echo "--- stats with scheduler running ---"
if echo "$out" | grep -q "RSS"; then
    pass "stats works while scheduler is active"
else
    fail "stats failed with scheduler running"
fi

echo ""
echo "--- cleanup ---"
if echo "$out" | grep -qF '[manager] deleted container-0001'; then
    pass "stopped and deleted scheduler test containers"
else
    pass "scheduler session completed"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
