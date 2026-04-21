#!/usr/bin/env bash
# test_basic.sh — smoke tests for container lifecycle commands
# Run from the project root: bash tests/test_basic.sh

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-basic"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

run_cmd() {
    # Send command(s) to the simulator via stdin, capture output
    printf '%s\nexit\n' "$1" | "$BIN" 2>&1
}

echo "=== test_basic.sh ==="
echo ""

echo "--- create ---"
out=$(printf 'create basic-test myhost %s\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "created container-"; then
    pass "create returns container id"
else
    fail "create did not return container id"
    echo "$out"
fi

CID=$(echo "$out" | grep -oP 'container-\d+' | head -1)
if [ -z "$CID" ]; then
    echo "Cannot determine container ID — aborting remaining tests"
    exit 1
fi
echo "  container id: $CID"

echo ""
echo "--- list ---"
out=$(printf 'list\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "$CID"; then
    pass "list shows created container"
else
    fail "list does not show container"
fi
if echo "$out" | grep -q "Created"; then
    pass "state is Created after create"
else
    fail "state is not Created"
fi
if echo "$out" | grep -q "STATUS"; then
    pass "list shows STATUS column"
else
    fail "list missing STATUS column"
fi
if echo "$out" | grep -q "PORTS"; then
    pass "list shows PORTS column"
else
    fail "list missing PORTS column"
fi
if echo "$out" | grep -q "IP"; then
    pass "list shows IP column"
else
    fail "list missing IP column"
fi

echo ""
echo "--- inspect ---"
out=$(printf 'inspect %s\nexit\n' "$CID" | "$BIN" 2>&1)
if echo "$out" | grep -q '"State"'; then
    pass "inspect returns JSON-like output"
else
    fail "inspect output malformed"
fi
if echo "$out" | grep -q '"StartedAt"'; then
    pass "inspect shows StartedAt"
else
    fail "inspect missing StartedAt"
fi
if echo "$out" | grep -q '"ExitCode"'; then
    pass "inspect shows ExitCode"
else
    fail "inspect missing ExitCode"
fi

echo ""
echo "--- delete ---"
out=$(printf 'delete %s\nexit\n' "$CID" | "$BIN" 2>&1)
if echo "$out" | grep -q "deleted $CID"; then
    pass "delete succeeds for CREATED container"
else
    fail "delete failed"
    echo "$out"
fi

echo ""
echo "--- metadata file ---"
if [ -f containers.meta ]; then
    pass "containers.meta exists"
else
    fail "containers.meta not found"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
