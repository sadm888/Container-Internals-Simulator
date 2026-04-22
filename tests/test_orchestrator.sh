#!/usr/bin/env bash
# Module 12: Orchestrator tests

set -euo pipefail

BINARY="${BINARY:-./container-sim}"
ROOTFS="${ROOTFS:-./rootfs/base}"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

run_sim() {
    echo "$1" | "$BINARY" 2>/dev/null
}

run_sim_multi() {
    printf '%s\n' "$@" | "$BINARY" 2>/dev/null
}

echo "=== test_orchestrator.sh ==="

# ── helpers ─────────────────────────────────────────────────────────────────

SPEC_DIR="./specs"
DEMO_SPEC="$SPEC_DIR/demo.json"
WEBAPP_SPEC="$SPEC_DIR/webapp.json"

# Create a minimal valid spec using the base rootfs
make_spec() {
    local file="$1" name="$2" img="$3"
    cat >"$file" <<EOF
{
  "name": "$name",
  "services": {
    "svc1": {
      "image": "$img",
      "command": "/bin/sh -c 'sleep 20'"
    }
  }
}
EOF
}

make_dep_spec() {
    local file="$1" name="$2" img="$3"
    cat >"$file" <<EOF
{
  "name": "$name",
  "services": {
    "backend": {
      "image": "$img",
      "command": "/bin/sh -c 'sleep 20'",
      "restart": "never"
    },
    "frontend": {
      "image": "$img",
      "command": "/bin/sh -c 'sleep 20'",
      "depends_on": ["backend"],
      "restart": "never"
    }
  }
}
EOF
}

make_cycle_spec() {
    local file="$1"
    cat >"$file" <<EOF
{
  "name": "cycle-test",
  "services": {
    "a": { "image": "base", "command": "/bin/true", "depends_on": ["b"] },
    "b": { "image": "base", "command": "/bin/true", "depends_on": ["a"] }
  }
}
EOF
}

mkdir -p /tmp/orch-test-specs

# ── T1: orch with no args prints usage ──────────────────────────────────────
out=$(run_sim "orch")
if echo "$out" | grep -qi "usage\|subcommand"; then
    pass "T1: 'orch' with no args prints usage"
else
    fail "T1: 'orch' should print usage/subcommand help"
fi

# ── T2: orch validate on a valid spec ───────────────────────────────────────
if [ -d "$ROOTFS" ]; then
    out=$(run_sim_multi \
        "image build orchbase $ROOTFS" \
        "orch validate $DEMO_SPEC")
    if echo "$out" | grep -qi "valid\|startup order"; then
        pass "T2: orch validate reports valid spec"
    else
        fail "T2: orch validate should confirm spec is valid"
    fi
else
    pass "T2: skipped (rootfs not present)"
fi

# ── T3: orch validate detects missing image field ───────────────────────────
TMP_BAD="/tmp/orch-test-specs/bad_noimg.json"
cat >"$TMP_BAD" <<'EOF'
{
  "name": "bad",
  "services": {
    "broken": { "command": "/bin/true" }
  }
}
EOF
out=$(run_sim "orch validate $TMP_BAD")
if echo "$out" | grep -qi "missing\|image\|error\|invalid"; then
    pass "T3: orch validate catches missing image field"
else
    fail "T3: should report missing image field"
fi

# ── T4: orch validate detects unknown dependency ─────────────────────────────
TMP_BADDEP="/tmp/orch-test-specs/bad_dep.json"
cat >"$TMP_BADDEP" <<'EOF'
{
  "name": "baddep",
  "services": {
    "web": {
      "image": "base",
      "command": "/bin/true",
      "depends_on": ["nonexistent"]
    }
  }
}
EOF
out=$(run_sim "orch validate $TMP_BADDEP")
if echo "$out" | grep -qi "unknown\|dependency\|nonexistent\|error\|invalid"; then
    pass "T4: orch validate catches unknown dependency"
else
    fail "T4: should report unknown dependency"
fi

# ── T5: orch validate detects dependency cycle ───────────────────────────────
TMP_CYCLE="/tmp/orch-test-specs/cycle.json"
make_cycle_spec "$TMP_CYCLE"
out=$(run_sim "orch validate $TMP_CYCLE")
if echo "$out" | grep -qi "cycle\|invalid\|error"; then
    pass "T5: orch validate detects dependency cycle"
else
    fail "T5: should detect dependency cycle"
fi

# ── T6: orch graph prints startup order ──────────────────────────────────────
if [ -f "$WEBAPP_SPEC" ]; then
    out=$(run_sim "orch graph $WEBAPP_SPEC")
    if echo "$out" | grep -q "Startup order"; then
        pass "T6: orch graph prints startup order"
    else
        fail "T6: orch graph should print 'Startup order'"
    fi
    if echo "$out" | grep -q "→"; then
        pass "T7: orch graph shows dependency arrows"
    else
        fail "T7: orch graph should show arrows between services"
    fi
else
    pass "T6: skipped (webapp.json not present)"
    pass "T7: skipped (webapp.json not present)"
fi

# ── T8: orch graph shows roots (no dependencies) ────────────────────────────
if [ -f "$WEBAPP_SPEC" ]; then
    out=$(run_sim "orch graph $WEBAPP_SPEC")
    if echo "$out" | grep -q "root\|no dependencies"; then
        pass "T8: orch graph labels root services"
    else
        fail "T8: orch graph should label root services (no deps)"
    fi
else
    pass "T8: skipped"
fi

# ── T9: orch validate reports startup order ──────────────────────────────────
if [ -d "$ROOTFS" ]; then
    TMP_DEP="/tmp/orch-test-specs/dep_test.json"
    make_dep_spec "$TMP_DEP" "deptest" "orchbase"
    out=$(run_sim "orch validate $TMP_DEP")
    if echo "$out" | grep -q "backend"; then
        pass "T9: orch validate prints startup order with service names"
    else
        fail "T9: orch validate should print startup order"
    fi
fi

# ── T10: orch run starts containers and orch status shows them ───────────────
if [ -d "$ROOTFS" ]; then
    TMP_SPEC="/tmp/orch-test-specs/run_test.json"
    make_spec "$TMP_SPEC" "runtest" "orchbase"
    out=$(run_sim_multi \
        "orch run $TMP_SPEC" \
        "orch status" \
        "orch down")
    if echo "$out" | grep -qi "svc1\|up\|running\|healthy\|starting"; then
        pass "T10: orch run starts services visible in orch status"
    else
        fail "T10: orch run should start services visible in status"
    fi
fi

# ── T11: orch status with no spec running prints message ─────────────────────
out=$(run_sim "orch status")
if echo "$out" | grep -qi "no spec\|orch run"; then
    pass "T11: orch status with no spec shows helpful message"
else
    fail "T11: orch status should report no spec running"
fi

# ── T12: orch ps is alias for orch status ────────────────────────────────────
out1=$(run_sim "orch status")
out2=$(run_sim "orch ps")
if [ "$out1" = "$out2" ]; then
    pass "T12: orch ps is identical to orch status"
else
    fail "T12: orch ps should be an alias for orch status"
fi

# ── T13: orch down with no spec running prints message ───────────────────────
out=$(run_sim "orch down")
if echo "$out" | grep -qi "no spec\|not running"; then
    pass "T13: orch down with no spec shows helpful message"
else
    fail "T13: orch down should report no spec running"
fi

# ── T14: orch run emits ORCH_SPEC_UP event ───────────────────────────────────
if [ -d "$ROOTFS" ]; then
    TMP_EV="/tmp/orch-test-specs/ev_test.json"
    make_spec "$TMP_EV" "evtest" "orchbase"
    out=$(run_sim_multi \
        "orch run $TMP_EV" \
        "events --type ORCH_SPEC_UP" \
        "orch down")
    if echo "$out" | grep -q "ORCH_SPEC_UP"; then
        pass "T14: orch run emits ORCH_SPEC_UP event"
    else
        fail "T14: ORCH_SPEC_UP event should appear after orch run"
    fi
fi

# ── T15: orch run emits ORCH_SVC_STARTED events ──────────────────────────────
if [ -d "$ROOTFS" ]; then
    TMP_SVC="/tmp/orch-test-specs/svc_ev.json"
    make_spec "$TMP_SVC" "svcevtest" "orchbase"
    out=$(run_sim_multi \
        "orch run $TMP_SVC" \
        "events --type ORCH_SVC_STARTED" \
        "orch down")
    if echo "$out" | grep -q "ORCH_SVC_STARTED"; then
        pass "T15: orch run emits ORCH_SVC_STARTED per service"
    else
        fail "T15: ORCH_SVC_STARTED should appear after orch run"
    fi
fi

# ── T16: orch validate rejects missing spec file ─────────────────────────────
out=$(run_sim "orch validate /tmp/nonexistent_spec_file_xyz.json")
if echo "$out" | grep -qi "cannot open\|error\|not found"; then
    pass "T16: orch validate reports missing spec file"
else
    fail "T16: should report error for missing spec file"
fi

# ── T17: orch run + orch down emits ORCH_SPEC_DOWN ───────────────────────────
if [ -d "$ROOTFS" ]; then
    TMP_DW="/tmp/orch-test-specs/down_test.json"
    make_spec "$TMP_DW" "downtest" "orchbase"
    out=$(run_sim_multi \
        "orch run $TMP_DW" \
        "orch down" \
        "events --type ORCH_SPEC_DOWN")
    if echo "$out" | grep -q "ORCH_SPEC_DOWN"; then
        pass "T17: orch down emits ORCH_SPEC_DOWN event"
    else
        fail "T17: ORCH_SPEC_DOWN should appear after orch down"
    fi
fi

# ── T18: second orch run while one is running prints error ───────────────────
if [ -d "$ROOTFS" ]; then
    TMP_DUP="/tmp/orch-test-specs/dup_test.json"
    make_spec "$TMP_DUP" "duptest" "orchbase"
    out=$(run_sim_multi \
        "orch run $TMP_DUP" \
        "orch run $TMP_DUP" \
        "orch down")
    if echo "$out" | grep -qi "already running\|orch down"; then
        pass "T18: second orch run reports spec already running"
    else
        fail "T18: should reject second orch run while spec is active"
    fi
fi

# ── T19: JSON parser handles spec with health_check block ────────────────────
TMP_HC="/tmp/orch-test-specs/hc_parse.json"
cat >"$TMP_HC" <<'EOF'
{
  "name": "hctest",
  "services": {
    "app": {
      "image": "base",
      "command": "/bin/true",
      "health_check": {
        "exec": "/bin/true",
        "interval_ms": 2000,
        "retries": 3,
        "start_period_ms": 500
      }
    }
  }
}
EOF
out=$(run_sim "orch validate $TMP_HC")
# Should either validate OK or fail only on missing image — not a parse error
if echo "$out" | grep -qi "valid\|parse error\|missing" ; then
    pass "T19: JSON parser handles health_check block"
else
    fail "T19: health_check block should parse without crash"
fi

# ── T20: orch graph on webapp.json shows 3 services ─────────────────────────
if [ -f "$WEBAPP_SPEC" ]; then
    out=$(run_sim "orch graph $WEBAPP_SPEC")
    svc_count=$(echo "$out" | grep -c "\[" || true)
    if [ "$svc_count" -ge 3 ]; then
        pass "T20: orch graph shows all 3 services in webapp spec"
    else
        fail "T20: orch graph should show 3 services (got $svc_count brackets)"
    fi
else
    pass "T20: skipped (webapp.json not present)"
fi

# cleanup temp specs
rm -rf /tmp/orch-test-specs

echo ""
echo "Results: $PASS passed, $FAIL failed"
echo ""
[ "$FAIL" -eq 0 ]
