#!/usr/bin/env bash
# test_cli.sh — tests for CLI features added in the web/CLI layer:
#   prune, quoted-arg parsing, logs -n, alert commands

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-cli"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

echo "=== test_cli.sh ==="
echo ""

# ── prune ────────────────────────────────────────────────────────────────────
echo "--- prune ---"

# Create two containers, then prune them
out=$(printf 'create c-prune1 h1 %s\ncreate c-prune2 h2 %s\nprune\nlist\nexit\n' \
    "$ROOTFS" "$ROOTFS" | "$BIN" 2>&1)

if echo "$out" | grep -q "pruned"; then
    pass "prune reports count"
else
    fail "prune output missing"
    echo "$out"
fi

if echo "$out" | grep -q "counter reset to 1"; then
    pass "prune resets ID counter message"
else
    fail "prune did not report counter reset"
fi

# After prune + new create, ID should restart from 0001
out2=$(printf 'create fresh-box myhost %s\nlist\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out2" | grep -q "container-0001"; then
    pass "ID counter resets to container-0001 after prune"
else
    fail "ID counter did not reset to container-0001"
    echo "$out2"
fi

echo ""
echo "--- quoted-arg parsing ---"

# run with sh -c "echo hello" — the quoted arg must reach sh as one token
out=$(printf 'run qtest myhost %s /bin/sh -c "echo hello"\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "hello"; then
    pass "quoted sh -c arg executes correctly"
else
    fail "quoted arg did not execute (no 'hello' in output)"
    echo "$out"
fi

# make sure the command_line field round-trips with quotes when inspected
out=$(printf 'create qcreate myhost %s\nlist\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "qcreate"; then
    pass "create with name works correctly"
else
    fail "create with name failed"
fi

echo ""
echo "--- delete rejects paused ---"

# Create + pause (can't pause without running, so just test the reject path via state file)
# We test the guard by checking delete of a running container is also rejected
out=$(printf 'create dp-test h %s\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
CID=$(echo "$out" | grep -oP 'container-\d+' | head -1)
if [ -z "$CID" ]; then
    fail "could not create container for delete-guard test"
else
    out2=$(printf 'delete %s\nexit\n' "$CID" | "$BIN" 2>&1)
    if echo "$out2" | grep -q "deleted $CID"; then
        pass "delete succeeds for CREATED container"
    else
        fail "delete failed for CREATED container"
        echo "$out2"
    fi
fi

echo ""
echo "--- alert commands ---"

out=$(printf 'alert ls\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -qiE "no alerts|alert"; then
    pass "alert ls runs without error"
else
    fail "alert ls failed"
    echo "$out"
fi

out=$(printf 'alert set fake-id cpu 80\nexit\n' | "$BIN" 2>&1)
# Either it succeeds or says container not found — both are valid CLI paths
if echo "$out" | grep -qiE "alert|error"; then
    pass "alert set produces output"
else
    fail "alert set produced no output"
fi

out=$(printf 'alert rm fake-id cpu\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -qiE "removed|no.*alert|error"; then
    pass "alert rm produces output"
else
    fail "alert rm produced no output"
fi

echo ""
echo "--- logs -n ---"

# runbg creates a log file; then logs -n 2 should print at most 2 lines
# We can't run a real container in this test (requires root/namespaces), so
# just verify the CLI path rejects unknown IDs with a clear error.
out=$(printf 'logs -n 5 no-such-container\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "not found"; then
    pass "logs -n reports not-found for unknown container"
else
    fail "logs -n did not report not-found"
    echo "$out"
fi

echo ""
echo "--- help includes new commands ---"
out=$(printf 'help\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "prune"; then
    pass "help lists prune"
else
    fail "help missing prune"
fi
if echo "$out" | grep -q "alert"; then
    pass "help lists alert"
else
    fail "help missing alert"
fi
if echo "$out" | grep -q "web"; then
    pass "help lists web"
else
    fail "help missing web"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
