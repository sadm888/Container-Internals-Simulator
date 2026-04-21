#!/usr/bin/env bash
# run_all.sh — runs all test suites
# Usage: bash tests/run_all.sh

set -euo pipefail

cd "$(dirname "$0")/.."

TOTAL_PASS=0
TOTAL_FAIL=0

run_suite() {
    local script=$1
    local exit_code=0
    echo ""
    echo "======================================"
    bash "$script" || exit_code=$?
    if [ $exit_code -ne 0 ]; then
        ((++TOTAL_FAIL))
    else
        ((++TOTAL_PASS))
    fi
}

echo "Building..."
make -s

run_suite tests/test_basic.sh
run_suite tests/test_isolation.sh
run_suite tests/test_resources.sh
run_suite tests/test_scheduler.sh
run_suite tests/test_monitoring.sh
run_suite tests/test_network.sh
run_suite tests/test_security.sh

echo ""
echo "======================================"
echo "SUITES: $TOTAL_PASS passed, $TOTAL_FAIL failed"
[ "$TOTAL_FAIL" -eq 0 ] && exit 0 || exit 1
