#!/bin/sh
# run_all.sh — run the v4 (RAIDZ2-native) integration test suite
# against virtio-blk (/dev/vbd*).  vn-backed runs are no longer
# supported per newplan.md §8.
#
# Usage: sh run_all.sh [group ...]
#   If no groups are specified, runs all groups (A B C D F G I).
#   Specify group letters to run only those tests, e.g.: sh run_all.sh A B
#
# Environment:
#   NDISKS=4         (default) number of disks; supports 4-6

NDISKS="${NDISKS:-4}"
export NDISKS

SCRIPTDIR=$(dirname "$0")

echo "v4 RAIDZ2-native test suite: NDISKS=$NDISKS (vbd substrate)"

TOTAL_PASS=0
TOTAL_FAIL=0
SUITE_ERRORS=""

run_group() {
    local name="$1"
    local script="$2"
    local tmpout="/var/tmp/v4_group_out.txt"
    echo ""
    echo "########## $name ##########"
    # Run script once, capture output, then display and parse from the file.
    sh "$script" > "$tmpout" 2>&1
    RC=$?
    cat "$tmpout"
    # Parse PASS/FAIL counts from summary line (e.g. "=== 4/5 PASS, 1 FAIL ===")
    local pass fail
    pass=$(grep "^=== " "$tmpout" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f1)
    fail=$(grep "^=== " "$tmpout" | grep -oE '[0-9]+ FAIL' | awk '{print $1}')
    TOTAL_PASS=$((TOTAL_PASS + ${pass:-0}))
    TOTAL_FAIL=$((TOTAL_FAIL + ${fail:-0}))
    if [ "${fail:-0}" != "0" ]; then
        SUITE_ERRORS="${SUITE_ERRORS}  $name: ${fail} failure(s)\n"
    fi
    return $RC
}

# Determine which groups to run
GROUPS="${*:-A B C D E F G H I}"

for GROUP in $GROUPS; do
    case $GROUP in
    A) run_group "Group A" "$SCRIPTDIR/test_a_basic.sh"        ;;
    B) run_group "Group B" "$SCRIPTDIR/test_b_single_fail.sh"  ;;
    C) run_group "Group C" "$SCRIPTDIR/test_c_dual_fail.sh"    ;;
    D) run_group "Group D" "$SCRIPTDIR/test_d_resilver.sh"     ;;
    E) run_group "Group E" "$SCRIPTDIR/test_e_eio_inject.sh"   ;;
    F) run_group "Group F" "$SCRIPTDIR/test_f_cow_invariant.sh" ;;
    G) run_group "Group G" "$SCRIPTDIR/test_g_autofail.sh"     ;;
    H) run_group "Group H" "$SCRIPTDIR/test_h_snap_degraded.sh" ;;
    I) run_group "Group I" "$SCRIPTDIR/test_i_unclean.sh"      ;;
    *) echo "Unknown group: $GROUP (valid: A B C D E F G H I)" ;;
    esac
done

echo ""
echo "========================================="
echo "  v4 RAIDZ2-native Integration Test Suite"
echo "  NDISKS=$NDISKS (vbd substrate)"
echo "  Total: $TOTAL_PASS pass, $TOTAL_FAIL fail"
echo "========================================="
if [ -n "$SUITE_ERRORS" ]; then
    printf "Groups with failures:\n%b" "$SUITE_ERRORS"
fi
[ $TOTAL_FAIL -eq 0 ]
