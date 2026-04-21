#!/usr/bin/env bash
# test_network.sh — Module 9: bridge networking, veth, port publishing
# Run from the project root: bash tests/test_network.sh
# Requires root (bridge init needs CAP_NET_ADMIN).

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-isolation"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

echo "=== test_network.sh ==="
echo ""

# ── root check ────────────────────────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    echo "  [SKIP] network tests require root (bridge init needs CAP_NET_ADMIN)"
    echo ""
    echo "--- summary ---"
    echo "  PASSED: $PASS  FAILED: $FAIL  SKIPPED: all"
    exit 0
fi

# ── helper: send commands to simulator ───────────────────────────────────────
run_cmd() {
    printf '%s\nexit\n' "$1" | "$BIN" 2>&1
}

# ── ensure bridge is down before we start ────────────────────────────────────
ip link delete csbr0 2>/dev/null || true
iptables -t nat -D POSTROUTING -s 172.17.0.0/16 ! -o csbr0 -j MASQUERADE 2>/dev/null || true

# ── net init ─────────────────────────────────────────────────────────────────
echo "--- net init ---"
out=$(run_cmd "net init")
if echo "$out" | grep -q "bridge csbr0 up"; then
    pass "net init creates bridge"
else
    fail "net init did not report bridge up"
    echo "$out"
fi

# Confirm bridge actually exists at kernel level
if ip link show csbr0 2>/dev/null | grep -q "UP"; then
    pass "bridge csbr0 is UP in kernel"
else
    fail "bridge csbr0 not found in kernel after init"
fi

# ── runbg without port publish — should get an IP ────────────────────────────
echo ""
echo "--- runbg with bridge active ---"
out=$(printf 'net init\nrunbg nettest myhost %s /bin/sleep 30\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
CTNR_ID=$(echo "$out" | grep -o 'container-[0-9]*' | tail -1)

if [ -n "$CTNR_ID" ]; then
    pass "runbg returned container id ($CTNR_ID)"
else
    fail "runbg did not return a container id"
    echo "$out"
fi

# Check container got a 172.17 IP in the banner
if echo "$out" | grep -qE "ip\s+:\s+172\.17\."; then
    pass "container banner shows bridge IP"
else
    fail "container banner missing bridge IP"
    echo "$out"
fi

# ── net <id> shows networking info ───────────────────────────────────────────
echo ""
echo "--- net <id> ---"
if [ -n "$CTNR_ID" ]; then
    out2=$(run_cmd "net $CTNR_ID")
    if echo "$out2" | grep -qE "172\.17\."; then
        pass "net <id> shows container IP"
    else
        fail "net <id> missing IP"
        echo "$out2"
    fi
    if echo "$out2" | grep -q "veth-host"; then
        pass "net <id> shows veth-host"
    else
        fail "net <id> missing veth-host field"
        echo "$out2"
    fi
    if echo "$out2" | grep -q "csbr0"; then
        pass "net <id> shows bridge name"
    else
        fail "net <id> missing bridge name"
        echo "$out2"
    fi
fi

# ── stop cleans up veth ──────────────────────────────────────────────────────
echo ""
echo "--- stop tears down veth ---"
if [ -n "$CTNR_ID" ]; then
    # get the veth name before stopping
    VETH=$(run_cmd "net $CTNR_ID" | grep "veth-host" | awk '{print $NF}')
    run_cmd "stop $CTNR_ID" >/dev/null
    sleep 0.5
    if [ -n "$VETH" ] && ! ip link show "$VETH" 2>/dev/null | grep -q "$VETH"; then
        pass "veth $VETH removed after stop"
    elif [ -z "$VETH" ]; then
        pass "no veth to check (bridge may not have been up during start)"
    else
        fail "veth $VETH still present after stop"
    fi
    run_cmd "delete $CTNR_ID" >/dev/null
fi

# ── port publishing ───────────────────────────────────────────────────────────
echo ""
echo "--- port publishing ---"
out=$(printf 'net init\nrunbg porttest myhost %s -p 18080:80/tcp /bin/sleep 30\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
PRT_ID=$(echo "$out" | grep -o 'container-[0-9]*' | tail -1)

if [ -n "$PRT_ID" ]; then
    pass "runbg -p returned container id"
else
    fail "runbg -p did not return container id"
    echo "$out"
fi

if echo "$out" | grep -q "18080:80"; then
    pass "banner shows port mapping"
else
    fail "banner missing port mapping"
    echo "$out"
fi

# Check iptables rule was installed
if iptables -t nat -L PREROUTING 2>/dev/null | grep -q "18080"; then
    pass "iptables DNAT rule for port 18080 installed"
else
    fail "iptables DNAT rule for port 18080 not found"
fi

# Stop removes iptables rule
if [ -n "$PRT_ID" ]; then
    run_cmd "stop $PRT_ID" >/dev/null
    sleep 0.5
    if ! iptables -t nat -L PREROUTING 2>/dev/null | grep -q "18080"; then
        pass "iptables DNAT rule removed after stop"
    else
        fail "iptables DNAT rule still present after stop"
    fi
    run_cmd "delete $PRT_ID" >/dev/null
fi

# ── net teardown ──────────────────────────────────────────────────────────────
echo ""
echo "--- net teardown ---"
out=$(run_cmd "net teardown")
if echo "$out" | grep -q "removed"; then
    pass "net teardown reports bridge removed"
else
    fail "net teardown did not report removal"
    echo "$out"
fi

if ! ip link show csbr0 2>/dev/null | grep -q "csbr0"; then
    pass "bridge csbr0 gone from kernel after teardown"
else
    fail "bridge csbr0 still present after teardown"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "--- summary ---"
echo "  PASSED: $PASS  FAILED: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
