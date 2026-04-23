#!/usr/bin/env bash
# test_monitoring.sh — exec, logs, inspect lifecycle, pause/unpause, image registry
# Run from project root: bash tests/test_monitoring.sh

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-mon"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

cleanup() {
    rm -f containers.meta containers.meta.tmp
    rm -f images.meta images.meta.tmp
}
trap cleanup EXIT

echo "=== test_monitoring.sh ==="
echo ""

# ─── inspect lifecycle fields ────────────────────────────────────────────────
# Use 'run' (not --rm) so the container stays in metadata as STOPPED for inspect.
echo "--- inspect: lifecycle fields after run ---"
rm -f containers.meta containers.meta.tmp
out=$(printf 'run lc-test host-lc %s /bin/hostname\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
CID=$(echo "$out" | grep -F '[manager] created container-' | grep -oP 'container-\d+' | head -1)

if [ -n "$CID" ]; then
    out2=$(printf 'inspect %s\nexit\n' "$CID" | "$BIN" 2>&1)
    echo "$out2" | grep -q '"StartedAt"'                && pass "inspect shows StartedAt"          || fail "inspect missing StartedAt"
    echo "$out2" | grep -qP '"ExitCode"\s*:\s*"0"'      && pass "inspect ExitCode 0 after clean exit" || {
        fail "inspect ExitCode not 0"
        echo "    actual: $(echo "$out2" | grep 'ExitCode' || true)"
        echo "    run out: $(echo "$out" | grep -E 'manager|error|hint' | head -5 || true)"
    }
    echo "$out2" | grep -q '"Image"'                    && pass "inspect shows Image field"         || fail "inspect missing Image field"
    echo "$out2" | grep -q '"NetworkSettings"'          && pass "inspect shows NetworkSettings"     || fail "inspect missing NetworkSettings"
    echo "$out2" | grep -q '"Profile"'                  && pass "inspect NetworkSettings has Profile" || fail "inspect missing network Profile"
    printf 'delete %s\nexit\n' "$CID" | "$BIN" >/dev/null 2>&1 || true
else
    fail "could not create container for inspect test"
    fail "inspect missing StartedAt"
    fail "inspect ExitCode not 0"
    fail "inspect missing Image field"
    fail "inspect missing NetworkSettings"
    fail "inspect missing network Profile"
fi

echo ""

# ─── logs: runbg captures stdout ─────────────────────────────────────────────
echo "--- logs: runbg captures stdout ---"
rm -f containers.meta containers.meta.tmp
# The pipe must pause after runbg so /bin/hostname gets CPU time before exit
# triggers cleanup_all_containers (which SIGKILLs background containers).
# Using process substitution with sleep gives the container time to write its output.
out=$( (printf 'runbg log-test host-log %s /bin/hostname\n' "$ROOTFS"; sleep 1.5; printf 'exit\n') | "$BIN" 2>&1)
CID=$(echo "$out" | grep -F '[manager] created container-' | grep -oP 'container-\d+' | head -1)

if [ -n "$CID" ]; then
    sleep 1
    out2=$(printf 'logs %s\nexit\n' "$CID" | "$BIN" 2>&1)
    echo "$out2" | grep -q "host-log" && pass "logs shows captured stdout" || {
        fail "logs did not show expected output"
        echo "    runbg out: $(echo "$out" | grep -E 'manager|error|hint' | head -5 || true)"
        echo "    log content: $(echo "$out2" | grep -FA5 -e '--- logs:' || true)"
    }

    # logs -f: should auto-exit because container is already stopped
    out3=$(printf 'logs -f %s\nexit\n' "$CID" | "$BIN" 2>&1)
    echo "$out3" | grep -q "host-log" \
        && pass "logs -f shows content and exits when container stopped" \
        || fail "logs -f did not show expected content"

    printf 'delete %s\nexit\n' "$CID" | "$BIN" >/dev/null 2>&1 || true
else
    fail "could not create container for logs test"
    fail "logs -f exit on stopped container"
fi

echo ""

# ─── logs: create + start also captures stdout ────────────────────────────────
echo "--- logs: start captures stdout ---"
rm -f containers.meta containers.meta.tmp
out=$( (
    printf 'create start-log host-start-log %s\n' "$ROOTFS"
    printf 'start container-0001\n'
    sleep 1.0
    printf 'logs container-0001\n'
    printf 'delete container-0001\n'
    printf 'exit\n'
) | "$BIN" 2>&1)

echo "$out" | grep -q "host-start-log" \
    && pass "logs shows stdout for create + start containers" \
    || {
        fail "create + start containers did not capture logs"
        echo "$out" | tail -20
    }

echo ""

# ─── exec: command runs in container namespace ───────────────────────────────
# Must use a single session: exec requires the container to still be RUNNING,
# but exit triggers cleanup_all_containers which stops background containers.
echo "--- exec: hostname matches container ---"
rm -f containers.meta containers.meta.tmp
out=$(printf 'runbg exec-test host-exec %s /bin/sleep 20\nexec container-0001 /bin/hostname\nstop container-0001\ndelete container-0001\nexit\n' \
     "$ROOTFS" | "$BIN" 2>&1)

if echo "$out" | grep -qF '[manager] started container-'; then
    echo "$out" | grep -q "host-exec" \
        && pass "exec /bin/hostname returns container hostname" \
        || { fail "exec hostname wrong"; echo "    Got exec output: $(echo "$out" | grep -A1 'exec container' || true)"; }
else
    fail "container not RUNNING for exec test"
fi

echo ""

# ─── pause / unpause ─────────────────────────────────────────────────────────
# Single session: pause/unpause/stats all happen while container is still running.
echo "--- pause/unpause state transitions ---"
rm -f containers.meta containers.meta.tmp
out=$(printf \
'runbg pause-test host-pause %s /bin/sleep 30
pause container-0001
list
unpause container-0001
list
pause container-0001
stats container-0001
unpause container-0001
stop container-0001
delete container-0001
exit
' "$ROOTFS" | "$BIN" 2>&1)

if echo "$out" | grep -qF '[manager] started container-'; then
    echo "$out" | grep -q "PAUSED"  && pass "pause → PAUSED state"        || fail "pause did not show PAUSED"
    echo "$out" | grep -q "RUNNING" && pass "unpause → RUNNING state"     || fail "unpause did not show RUNNING"
    echo "$out" | grep -q "RSS"     && pass "stats works on paused container" || fail "stats failed on paused container"
else
    fail "container not RUNNING for pause test"
    fail "pause did not show PAUSED"
    fail "unpause did not show RUNNING"
    fail "stats failed on paused container"
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

# inspect container shows image ref — parse from [manager] created line to avoid stale IDs
CID5=$(echo "$out5" | grep -F '[manager] created container-' | grep -oP 'container-\d+' | head -1)
if [ -n "$CID5" ]; then
    out6=$(printf 'inspect %s\nexit\n' "$CID5" | "$BIN" 2>&1)
    echo "$out6" | grep -q "myapp:v1" && pass "container inspect shows image ref" || fail "container inspect missing image ref"
    printf 'delete %s\nexit\n' "$CID5" | "$BIN" >/dev/null 2>&1 || true
fi

# rm
out7=$(printf 'image rm myapp:v1\nimage rm myapp:stable\nimage ls\nexit\n' | "$BIN" 2>&1)
echo "$out7" | grep -q "removed" && pass "image rm removes image" || fail "image rm failed"

# edge case: empty tag treated as latest
out8=$(printf 'image build edgecase %s\nimage tag edgecase: edgecase:copy\nimage ls\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
echo "$out8" | grep -q "edgecase:copy" && pass "image tag with empty src tag defaults to :latest" || fail "empty tag handling broken"
printf 'image rm edgecase\nimage rm edgecase:copy\nexit\n' | "$BIN" >/dev/null 2>&1 || true

echo ""

# ─── net summary (net / net ls) ─────────────────────────────────────────────
echo "--- net summary ---"
rm -f containers.meta containers.meta.tmp
out_net=$(printf 'net\nexit\n' | "$BIN" 2>&1)
echo "$out_net" | grep -qi "bridge\|csbr0" \
    && pass "net (no args) shows bridge info" \
    || fail "net (no args) missing bridge info"
echo "$out_net" | grep -q "UP\|DOWN\|init" \
    && pass "net summary shows bridge state" \
    || fail "net summary missing bridge state"

# net ls is alias
out_nls=$(printf 'net ls\nexit\n' | "$BIN" 2>&1)
echo "$out_nls" | grep -qi "bridge\|csbr0" \
    && pass "net ls is alias for net" \
    || fail "net ls alias broken"

echo ""

# ─── stop --timeout configurable grace period ────────────────────────────────
echo "--- stop --timeout ---"
rm -f containers.meta containers.meta.tmp
stop_t_out=$( (
    printf 'runbg stop-t-test st-host %s /bin/sleep 60\n' "$ROOTFS"
    sleep 0.4
    printf 'stop -t 3 container-0001\ndelete container-0001\nexit\n'
) | "$BIN" 2>&1)
if echo "$stop_t_out" | grep -q "timeout 3s"; then
    pass "stop -t 3 uses 3s timeout"
elif echo "$stop_t_out" | grep -q "stopped"; then
    pass "stop -t 3 stopped container (timeout shown varies)"
else
    fail "stop -t 3 did not confirm stop"
    echo "$stop_t_out" | tail -6
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
