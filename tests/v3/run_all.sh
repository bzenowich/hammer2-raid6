#!/bin/sh
# run_all.sh — run the v3 (RAIDZ2-native) integration test suite
# against virtio-blk (/dev/vbd*).  vn-backed runs are no longer
# supported per newplan.md §8.
#
# Usage: sh run_all.sh [group ...]
#   If no groups are specified, runs A B C D E F G H I J K L N P Q S T U V W X Y Z,
#   then M (last: it can hang the guest).  R is not in the default list.
#   Specify group letters to run only those tests, e.g.: sh run_all.sh A B
#   Groups: A basic, B single failure, C dual failure, D resilver,
#   E EIO injection, F COW invariant, G auto-fail, H snapshots degraded,
#   I unclean unmount, J packed rows, K scrub, L self-heal,
#   M multi-corrupt, N capacity, P performance shape (devstat),
#   R rewrite/seal (opt-in, not in the default list), S split header,
#   W write-side fault injection (vfs.hammer2.inject_wio/delay),
#   X lifecycle (umount during resilver/scrub, ENOSPC, simd parity),
#   Y parity scrub (row-ordered P/Q check, localise, repair),
#   Z metadata on 3 copies (placement, any 2 failures, heal, resilver, layout 2),
#   T slot reuse, U sub-column offsets (test hook raid6_subcol_test),
#   V sub-column allocation (raid6_subcol_max), Q small files on sub-columns.
#   O async sealing (opt-in; forces vfs.hammer2.async_seal=1),
#   Run R with: sh run_all.sh R
#   Run any group with async sealing on: ASYNC_SEAL=1 sh run_all.sh A B
#
# Environment:
#   NDISKS=4         (default) number of disks; supports 4-6

NDISKS="${NDISKS:-4}"
export NDISKS

SCRIPTDIR=$(dirname "$0")

echo "v3 RAIDZ2-native test suite: NDISKS=$NDISKS (${DISK_PREFIX:-/dev/vbd}N${DISK_SUFFIX} substrate)"

TOTAL_PASS=0
TOTAL_FAIL=0
SUITE_ERRORS=""

run_group() {
    local name="$1"
    local script="$2"
    local tmpout="/var/tmp/v3_group_out.txt"
    echo ""
    echo "########## $name ##########"
    # Run script once, capture output, then display and parse from the file.
    local t0=$(date +%s)
    sh "$script" > "$tmpout" 2>&1
    RC=$?
    cat "$tmpout"
    echo "=== $name time: $(( $(date +%s) - t0 ))s ==="
    GROUP_TIMES="${GROUP_TIMES} ${name#Group }=$(( $(date +%s) - t0 ))s"
    # Parse PASS/FAIL counts from summary line (e.g. "=== 4/5 PASS, 1 FAIL ===")
    local pass fail
    pass=$(grep "^=== " "$tmpout" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f1)
    fail=$(grep "^=== " "$tmpout" | grep -oE '[0-9]+ FAIL' | awk '{print $1}')
    TOTAL_PASS=$((TOTAL_PASS + ${pass:-0}))
    TOTAL_FAIL=$((TOTAL_FAIL + ${fail:-0}))
    if [ "${fail:-0}" != "0" ]; then
        SUITE_ERRORS="${SUITE_ERRORS}  $name: ${fail} failure(s)\n"
    fi
    # A group that dies early (FATAL in setup_fresh, check_v3 skip with
    # exit 77, a shell error) prints no summary line, and its earlier
    # results never reach the totals.  Count that as a failure rather
    # than letting the suite report success over a group that never ran.
    if [ -z "$pass" ] || { [ "$RC" != "0" ] && [ "${fail:-0}" = "0" ]; }; then
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        SUITE_ERRORS="${SUITE_ERRORS}  $name: aborted (exit $RC, no clean summary)\n"
    fi
    return $RC
}

# Determine which groups to run
GROUPS="${*:-A B C D E F G H I J K L N P Q S T U V W X Y Z M}"

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
    J) run_group "Group J" "$SCRIPTDIR/test_j_packed_row.sh"   ;;
    K) run_group "Group K" "$SCRIPTDIR/test_k_scrub.sh"        ;;
    L) run_group "Group L" "$SCRIPTDIR/test_l_selfheal.sh"     ;;
    M) run_group "Group M" "$SCRIPTDIR/test_m_multi_corrupt.sh" ;;
    N) run_group "Group N" "$SCRIPTDIR/test_n_capacity.sh"     ;;
    P) run_group "Group P" "$SCRIPTDIR/test_p_perfshape.sh"    ;;
    Q) run_group "Group Q" "$SCRIPTDIR/test_q_small_files.sh" ;;
    R) run_group "Group R" "$SCRIPTDIR/test_r_rewrite_seal.sh" ;;
    S) run_group "Group S" "$SCRIPTDIR/test_s_split_header.sh" ;;
    T) run_group "Group T" "$SCRIPTDIR/test_t_reuse.sh"        ;;
    U) run_group "Group U" "$SCRIPTDIR/test_u_subcol_offset.sh" ;;
    V) run_group "Group V" "$SCRIPTDIR/test_v_subcol_alloc.sh" ;;
    W) run_group "Group W" "$SCRIPTDIR/test_w_writefault.sh"   ;;
    X) run_group "Group X" "$SCRIPTDIR/test_x_lifecycle.sh"    ;;
    O) run_group "Group O" "$SCRIPTDIR/test_o_async_seal.sh" ;;
    Y) run_group "Group Y" "$SCRIPTDIR/test_y_parity_scrub.sh" ;;
    Z) run_group "Group Z" "$SCRIPTDIR/test_z_meta3copy.sh" ;;
    *) echo "Unknown group: $GROUP (valid: A B C D E F G H I J K L M N O P Q R S T U V W X Y Z)" ;;
    esac
done

echo ""
echo "========================================="
echo "  v3 RAIDZ2-native Integration Test Suite"
echo "  NDISKS=$NDISKS (${DISK_PREFIX:-/dev/vbd}N${DISK_SUFFIX} substrate)"
echo "  Total: $TOTAL_PASS pass, $TOTAL_FAIL fail"
echo "  Group times:$GROUP_TIMES"
echo "========================================="
if [ -n "$SUITE_ERRORS" ]; then
    printf "Groups with failures:\n%b" "$SUITE_ERRORS"
fi
[ $TOTAL_FAIL -eq 0 ]
