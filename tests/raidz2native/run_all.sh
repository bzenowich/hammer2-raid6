#!/bin/sh
# run_all.sh — run the full RAIDZ2-native (v4) integration test suite.
#
# Usage: sh run_all.sh [group ...]
#   If no groups are specified, runs all groups (A B C D F G I).
#   Specify group letters to run only those tests, e.g.: sh run_all.sh A B

SCRIPTDIR=$(dirname "$0")

TOTAL_PASS=0
TOTAL_FAIL=0
SUITE_ERRORS=""

run_group() {
    local name="$1"
    local script="$2"
    echo ""
    echo "########## $name ##########"
    sh "$script"
    RC=$?
    # Parse PASS/FAIL counts from summary line (e.g. "=== 4/5 PASS, 1 FAIL ===")
    local pass fail
    pass=$(sh "$script" 2>/dev/null | grep "^=== " | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f1)
    fail=$(sh "$script" 2>/dev/null | grep "^=== " | grep -oE '[0-9]+ FAIL' | awk '{print $1}')
    TOTAL_PASS=$((TOTAL_PASS + ${pass:-0}))
    TOTAL_FAIL=$((TOTAL_FAIL + ${fail:-0}))
    if [ "${fail:-0}" != "0" ]; then
        SUITE_ERRORS="${SUITE_ERRORS}  $name: ${fail} failure(s)\n"
    fi
    return $RC
}

# Determine which groups to run
GROUPS="${@:-A B C D F G I}"

for GROUP in $GROUPS; do
    case $GROUP in
    A) sh "$SCRIPTDIR/test_a_basic.sh"       ;;
    B) sh "$SCRIPTDIR/test_b_single_fail.sh"  ;;
    C) sh "$SCRIPTDIR/test_c_dual_fail.sh"    ;;
    D) sh "$SCRIPTDIR/test_d_resilver.sh"     ;;
    F) sh "$SCRIPTDIR/test_f_cow_invariant.sh" ;;
    G) sh "$SCRIPTDIR/test_g_autofail.sh"     ;;
    I) sh "$SCRIPTDIR/test_i_unclean.sh"      ;;
    *) echo "Unknown group: $GROUP (valid: A B C D F G I)" ;;
    esac
done

echo ""
echo "========================================="
echo "  RAIDZ2-native Integration Test Suite"
echo "  Total: $TOTAL_PASS pass, $TOTAL_FAIL fail"
echo "========================================="
if [ -n "$SUITE_ERRORS" ]; then
    printf "Groups with failures:\n%b" "$SUITE_ERRORS"
fi
[ $TOTAL_FAIL -eq 0 ]
