#!/usr/bin/env bash
# test_monitoring.sh — exec, logs, inspect lifecycle, pause/unpause, image registry
# Run from project root: bash tests/test_monitoring.sh

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-mon"

pass() { echo "  [PASS] $1"; ((PASS++)); }
fail() { echo "  [FAIL] $1"; ((FAIL++)); }

cleanup() {
    printf 'list\nexit\n' | "$BIN" 2>&1 | grep -oP 'container-\d+' | while read -r id; do
        printf 'stop %s\ndelete %s\n' "$id" "$id"
    done | "$BIN" >/dev/null 2>&1 || true
    rm -f images.meta images.meta.tmp
}
trap cleanup EXIT

echo "=== test_monitoring.sh ==="
echo ""

# ─── inspect lifecycle fields ────────────────────────────────────────────────
echo "--- inspect: lifecycle fields after run --rm ---"
out=$(printf 'run --rm lc-test host-lc %s /bin/echo hi\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
CID=$(echo "$out" | grep -oP 'container-\d+' | head -1)

if [ -n "$CID" ]; then
    out2=$(printf 'inspect %s\nexit\n' "$CID" | "$BIN" 2>&1)
    echo "$out2" | grep -q '"StartedAt"'   && pass "inspect shows StartedAt"   || fail "inspect missing StartedAt"
    echo "$out2" | grep -qP '"ExitCode"\s*:\s*"0"' && pass "inspect ExitCode 0 after clean exit" || fail "inspect ExitCode not 0"
    echo "$out2" | grep -q '"Image"'       && pass "inspect shows Image field"  || fail "inspect missing Image field"
else
    fail "could not create container for inspect test"
fi

echo ""

# ─── logs: runbg captures stdout ─────────────────────────────────────────────
echo "--- logs: runbg captures stdout ---"
out=$(printf 'runbg log-test host-log %s /bin/echo captured-output\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
CID=$(echo "$out" | grep -oP 'container-\d+' | head -1)

if [ -n "$CID" ]; then
    sleep 1
    out2=$(printf 'logs %s\nexit\n' "$CID" | "$BIN" 2>&1)
    echo "$out2" | grep -q "captured-output" && pass "logs shows captured stdout" || { fail "logs did not show expected output"; echo "    Got: $out2"; }
    printf 'stop %s\ndelete %s\nexit\n' "$CID" "$CID" | "$BIN" >/dev/null 2>&1 || true
else
    fail "could not create container for logs test"
fi

echo ""

# ─── exec: command runs in container namespace ───────────────────────────────
echo "--- exec: hostname matches container ---"
out=$(printf 'runbg exec-test host-exec %s /bin/sleep 20\nlist\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
CID=$(echo "$out" | grep -oP 'container-\d+' | head -1)

if [ -n "$CID" ] && echo "$out" | grep -q "RUNNING"; then
    out2=$(printf 'exec %s /bin/hostname\nexit\n' "$CID" | "$BIN" 2>&1)
    echo "$out2" | grep -q "host-exec" && pass "exec /bin/hostname returns container hostname" || { fail "exec hostname wrong"; echo "    Got: $out2"; }
    printf 'stop %s\ndelete %s\nexit\n' "$CID" "$CID" | "$BIN" >/dev/null 2>&1 || true
else
    fail "container not RUNNING for exec test"
fi

echo ""

# ─── pause / unpause ─────────────────────────────────────────────────────────
echo "--- pause/unpause state transitions ---"
out=$(printf 'runbg pause-test host-pause %s /bin/sleep 30\nlist\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
CID=$(echo "$out" | grep -oP 'container-\d+' | head -1)

if [ -n "$CID" ] && echo "$out" | grep -q "RUNNING"; then
    out2=$(printf 'pause %s\nlist\nexit\n' "$CID" | "$BIN" 2>&1)
    echo "$out2" | grep -q "PAUSED"  && pass "pause → PAUSED state"   || fail "pause did not show PAUSED"

    out3=$(printf 'unpause %s\nlist\nexit\n' "$CID" | "$BIN" 2>&1)
    echo "$out3" | grep -q "RUNNING" && pass "unpause → RUNNING state" || fail "unpause did not show RUNNING"

    # stats should work on a paused container
    out4=$(printf 'pause %s\nstats %s\nunpause %s\nexit\n' "$CID" "$CID" "$CID" | "$BIN" 2>&1)
    echo "$out4" | grep -q "RSS" && pass "stats works on paused container" || fail "stats failed on paused container"

    printf 'stop %s\ndelete %s\nexit\n' "$CID" "$CID" | "$BIN" >/dev/null 2>&1 || true
else
    fail "container not RUNNING for pause test"
fi

echo ""

# ─── image registry: build / ls / inspect / tag / rm ────────────────────────
echo "--- image registry ---"

# build
out=$(printf 'image build myapp:v1 %s\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
echo "$out" | grep -q "registered\|updated" && pass "image build registers an image" || fail "image build failed"

# ls
out2=$(printf 'image ls\nexit\n' | "$BIN" 2>&1)
echo "$out2" | grep -q "myapp" && pass "image ls shows registered image" || fail "image ls missing myapp"

# inspect
out3=$(printf 'image inspect myapp:v1\nexit\n' | "$BIN" 2>&1)
echo "$out3" | grep -q '"Tag"'    && pass "image inspect shows Tag"    || fail "image inspect missing Tag"
echo "$out3" | grep -q '"Rootfs"' && pass "image inspect shows Rootfs" || fail "image inspect missing Rootfs"

# tag
out4=$(printf 'image tag myapp:v1 myapp:stable\nimage ls\nexit\n' | "$BIN" 2>&1)
echo "$out4" | grep -q "myapp:stable" && pass "image tag creates alias" || fail "image tag did not create alias"

# create container from image name
out5=$(printf 'create img-test host-img myapp:v1\nlist\nexit\n' | "$BIN" 2>&1)
echo "$out5" | grep -q "container-" && pass "container create resolves image name" || fail "container create with image name failed"

# inspect container shows image ref
CID5=$(echo "$out5" | grep -oP 'container-\d+' | head -1)
if [ -n "$CID5" ]; then
    out6=$(printf 'inspect %s\nexit\n' "$CID5" | "$BIN" 2>&1)
    echo "$out6" | grep -q "myapp:v1" && pass "container inspect shows image ref" || fail "container inspect missing image ref"
    printf 'delete %s\nexit\n' "$CID5" | "$BIN" >/dev/null 2>&1 || true
fi

# rm
out7=$(printf 'image rm myapp:v1\nimage rm myapp:stable\nimage ls\nexit\n' | "$BIN" 2>&1)
echo "$out7" | grep -q "removed" && pass "image rm removes image" || fail "image rm failed"

# edge case: empty tag treated as latest
out8=$(printf 'image build edgecase: %s\nimage ls\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
# "edgecase:" has a colon with empty tag — should default to "latest"
# The shell will split "edgecase:" as one token since there's no space
out8=$(printf 'image build edgecase %s\nimage tag edgecase: edgecase:copy\nimage ls\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
echo "$out8" | grep -q "edgecase:copy" && pass "image tag with empty src tag defaults to :latest" || fail "empty tag handling broken"
printf 'image rm edgecase\nimage rm edgecase:copy\nexit\n' | "$BIN" >/dev/null 2>&1 || true

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
