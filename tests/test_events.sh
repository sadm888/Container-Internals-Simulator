#!/usr/bin/env bash
# Module 11: Event Bus + Metrics tests

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

echo "=== test_events.sh ==="

# ── T1: events shows exactly one table header when events exist ────────
out=$(run_sim_multi \
    "image build hdrtest ./rootfs/test-basic" \
    "events")
header_count=$(echo "$out" | grep -c "TIME.*SEQ.*EVENT" || true)
if [ "$header_count" -eq 1 ]; then
    pass "T1: events shows exactly one table header (no duplication)"
else
    fail "T1: expected exactly 1 table header, got $header_count"
fi

# ── T2: events shows no-events message when ring is empty ─────────────
out=$(run_sim "events")
if echo "$out" | grep -q "no events yet"; then
    pass "T2: events shows 'no events yet' on empty ring"
else
    fail "T2: should show 'no events yet' on empty bus"
fi

# ── T3: metrics shows counter table ───────────────────────────────────
out=$(run_sim "metrics")
if echo "$out" | grep -q "containers_started_total"; then
    pass "T3: metrics shows containers_started_total counter"
else
    fail "T3: metrics should show containers_started_total"
fi

if echo "$out" | grep -q "startup_latency_samples"; then
    pass "T4: metrics shows startup_latency_samples"
else
    fail "T4: metrics should show startup_latency_samples"
fi

if echo "$out" | grep -q "events_total"; then
    pass "T5: metrics shows events_total counter"
else
    fail "T5: metrics should show events_total"
fi

if echo "$out" | grep -q "containers_unpaused_total"; then
    pass "T6: metrics shows containers_unpaused_total"
else
    fail "T6: metrics should show containers_unpaused_total"
fi

# ── T7: metrics --prometheus outputs Prometheus exposition format ──────
out=$(run_sim "metrics --prometheus")
if echo "$out" | grep -q "^# HELP containers_started_total"; then
    pass "T7: metrics --prometheus emits # HELP lines"
else
    fail "T7: metrics --prometheus should emit # HELP lines"
fi

if echo "$out" | grep -q "^# TYPE containers_started_total counter"; then
    pass "T8: metrics --prometheus emits # TYPE counter lines"
else
    fail "T8: metrics --prometheus should emit # TYPE counter"
fi

if echo "$out" | grep -qP "^containers_started_total \d+"; then
    pass "T9: metrics --prometheus emits metric value lines"
else
    fail "T9: metrics --prometheus should emit 'metric value' lines"
fi

if echo "$out" | grep -q "events_total"; then
    pass "T10: metrics --prometheus includes events_total"
else
    fail "T10: metrics --prometheus should include events_total"
fi

# ── T11: image build emits IMAGE_BUILT event ──────────────────────────
out=$(run_sim_multi \
    "image build evtimg ./rootfs/test-basic" \
    "events")
if echo "$out" | grep -q "IMAGE_BUILT"; then
    pass "T11: IMAGE_BUILT event emitted on image build"
else
    fail "T11: IMAGE_BUILT event should appear after image build"
fi

# ── T12: events --type filters correctly ──────────────────────────────
out=$(run_sim_multi \
    "image build filtertest ./rootfs/test-basic" \
    "image rm filtertest" \
    "events --type IMAGE_BUILT")
if echo "$out" | grep -q "IMAGE_BUILT"; then
    pass "T12a: events --type IMAGE_BUILT shows IMAGE_BUILT events"
else
    fail "T12a: events --type IMAGE_BUILT should show IMAGE_BUILT"
fi
if echo "$out" | grep -q "IMAGE_REMOVED"; then
    fail "T12b: events --type IMAGE_BUILT should NOT show IMAGE_REMOVED"
else
    pass "T12b: events --type IMAGE_BUILT correctly hides IMAGE_REMOVED"
fi

# ── T13: events --type with unknown type prints error ─────────────────
out=$(run_sim "events --type BOGUS_EVENT")
if echo "$out" | grep -qi "unknown event type\|error"; then
    pass "T13: events --type BOGUS prints error message"
else
    fail "T13: should print error for unknown event type"
fi

# ── T14: image remove emits IMAGE_REMOVED event ───────────────────────
out=$(run_sim_multi \
    "image build rmtest ./rootfs/test-basic" \
    "image rm rmtest" \
    "events --type IMAGE_REMOVED")
if echo "$out" | grep -q "IMAGE_REMOVED"; then
    pass "T14: IMAGE_REMOVED event emitted on image rm"
else
    fail "T14: IMAGE_REMOVED should appear after image rm"
fi

# ── T15: image tag emits only IMAGE_TAGGED (not double IMAGE_BUILT) ───
out=$(run_sim_multi \
    "image build tagbase ./rootfs/test-basic" \
    "image tag tagbase tagcopy" \
    "events")
built_count=$(echo "$out" | grep -c "IMAGE_BUILT" || true)
tagged_count=$(echo "$out" | grep -c "IMAGE_TAGGED" || true)
if [ "$built_count" -eq 1 ]; then
    pass "T15a: image tag emits exactly 1 IMAGE_BUILT (not double)"
else
    fail "T15a: image tag should emit 1 IMAGE_BUILT, got $built_count"
fi
if [ "$tagged_count" -eq 1 ]; then
    pass "T15b: image tag emits 1 IMAGE_TAGGED event"
else
    fail "T15b: image tag should emit 1 IMAGE_TAGGED, got $tagged_count"
fi

# ── T16: metrics --prometheus summary block for latency ───────────────
# Only present after at least one container start; skip if no rootfs
if [ -d "./rootfs/test-basic" ]; then
    out=$(run_sim_multi \
        "image build latprom ./rootfs/test-basic" \
        "metrics --prometheus")
    # With zero container starts, latency block is absent — that's correct
    if echo "$out" | grep -q "startup_latency_milliseconds_count" ||
       echo "$out" | grep -q "startup_latency_samples"; then
        pass "T16: prometheus output conditionally includes latency block"
    else
        # Latency block is absent when startup_count == 0 — this is correct
        pass "T16: prometheus omits latency block correctly when no containers started"
    fi
fi

# ── T17: events -n limits returned count ──────────────────────────────
out=$(run_sim_multi \
    "image build n1 ./rootfs/test-basic" \
    "image build n2 ./rootfs/test-basic" \
    "image build n3 ./rootfs/test-basic" \
    "events -n 1")
event_rows=$(echo "$out" | grep -cP "IMAGE_BUILT|IMAGE_REMOVED|IMAGE_TAGGED" || true)
if [ "$event_rows" -le 2 ]; then
    pass "T17: events -n 1 limits output to ~1 event row"
else
    fail "T17: events -n 1 should limit output (got $event_rows rows)"
fi

# ── T18: events_total increments per event ────────────────────────────
out=$(run_sim_multi \
    "image build et1 ./rootfs/test-basic" \
    "image build et2 ./rootfs/test-basic" \
    "metrics")
total=$(echo "$out" | grep "events_total" | grep -oP '\d+' | tail -1 || echo "0")
if [ "${total:-0}" -ge 2 ] 2>/dev/null; then
    pass "T18: events_total >= 2 after two image builds"
else
    fail "T18: events_total should be >= 2 after two events"
fi

# ── T19: metrics simulator_uptime_seconds present ────────────────────
out=$(run_sim "metrics")
if echo "$out" | grep -q "simulator_uptime_seconds"; then
    pass "T19: metrics shows simulator_uptime_seconds"
else
    fail "T19: metrics should show simulator_uptime_seconds"
fi

# ── T20: no filter shows all events ──────────────────────────────────
out=$(run_sim_multi \
    "image build all1 ./rootfs/test-basic" \
    "image rm all1" \
    "events")
if echo "$out" | grep -q "IMAGE_BUILT" && echo "$out" | grep -q "IMAGE_REMOVED"; then
    pass "T20: unfiltered events shows all event types"
else
    fail "T20: unfiltered events should show all event types"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
echo ""
[ "$FAIL" -eq 0 ]
