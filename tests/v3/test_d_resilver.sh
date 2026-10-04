#!/bin/sh
# Group D: Resilver — replace a failed disk and verify reconstruction.
#
# fresh_disk zeroes the replacement through the data and metadata
# zones, and every check after a resilver reads cold (remount) and then
# requires:
#   - the data to match,
#   - cksum_err == 0 on the rebuilt disk (a column the resilver failed
#     to write reads as zeros, fails its CHECK and gets parity-healed —
#     the data would still match, so the counter is what proves the
#     resilver wrote it),
#   - a clean scrub (mdadm's post-recovery "check, mismatch_cnt == 0").
#
#   D1  basic resilver
#   D2  two sequential resilvers with writes in between
#   D3  writes concurrent with a resilver (overlap is asserted)
#   D4  resilver_skip_unalloc=1 is correct and faster than =0
#   D5  stale re-add: the failed disk comes back with its OLD contents
#       (missed writes) instead of zeros; resilver must overwrite it
#   D6  dual-degraded rebuild: two disks failed, resilver the first
#       from the two survivors (P+Q), then the second.  Disk 0 is kept
#       out of the pair: an array with disk 0 failed does not remount.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group D: Resilver (NDISKS=$NDISKS) ==="

# replace_disk <idx> <label>: resilver disk idx onto itself; FAIL and
# return 1 unless the ioctl succeeds and the disk is ONLINE afterwards.
replace_disk() {
    local idx="$1"
    local label="$2"
    if ! hammer2 -s $MNTPT raid replace "$(disk_dev "$idx")" \
            "$(disk_dev "$idx")" > /var/tmp/d_replace.txt 2>&1; then
        result FAIL "$label: raid replace disk${idx} failed"
        sed 's/^/      /' /var/tmp/d_replace.txt
        return 1
    fi
    if ! disk_state_is "$idx" ONLINE; then
        result FAIL "$label: disk${idx} not ONLINE after replace"
        return 1
    fi
    return 0
}

# check_rebuilt <idx> <label> <prefix...>: cold-verify after a resilver.
check_rebuilt() {
    local idx="$1"
    local label="$2"
    shift 2
    if ! remount; then
        result FAIL "$label: remount after resilver failed"
        return 1
    fi
    for p in "$@"; do
        verify_ref "$label: '$p' correct after resilver of disk${idx} (cold)" "$p"
    done
    local err
    err=$(disk_counter "$idx" cksum_err)
    if [ "${err:-x}" = "0" ]; then
        result PASS "$label: rebuilt disk${idx} read clean (no parity heals)"
    else
        result FAIL "$label: rebuilt disk${idx} had $err bad columns (resilver did not write them)"
    fi
    scrub_clean "$label"
}

# D1: basic resilver.
D1_DISK=3
setup_fresh
check_v3
write_ref_data "d1"
dd if=/dev/urandom of=$MNTPT/d1_big bs=65536 count=800 2>/dev/null
sync
if fail_disk "$D1_DISK" "D1"; then
    fresh_disk "$D1_DISK"
    replace_disk "$D1_DISK" "D1" && check_rebuilt "$D1_DISK" "D1" "d1"
fi
teardown "D1"

# D2: sequential resilvers.
D2_DISK1=1
D2_DISK2=$((NDISKS - 2))
setup_fresh
check_v3
write_ref_data "r1"
if fail_disk "$D2_DISK1" "D2"; then
    fresh_disk "$D2_DISK1"
    replace_disk "$D2_DISK1" "D2" && check_rebuilt "$D2_DISK1" "D2a" "r1"
fi
write_ref_data "r2"
if fail_disk "$D2_DISK2" "D2"; then
    fresh_disk "$D2_DISK2"
    replace_disk "$D2_DISK2" "D2" && check_rebuilt "$D2_DISK2" "D2b" "r1" "r2"
fi
teardown "D2"

# D3: writes during resilver.  With resilver_skip_unalloc=0 the
# resilver walks every slot of the disk, which takes long enough for
# the concurrent write to land inside it; the overlap is checked rather
# than assumed.
D3_DISK=0
setup_fresh
check_v3
write_ref_data "pre"
if fail_disk "$D3_DISK" "D3"; then
    fresh_disk "$D3_DISK"
    sysctl -w vfs.hammer2.resilver_skip_unalloc=0 > /dev/null
    hammer2 -s $MNTPT raid replace \
        "$(disk_dev $D3_DISK)" "$(disk_dev $D3_DISK)" > /dev/null 2>&1 &
    RPID=$!
    sleep 1
    write_ref_data "during"
    if kill -0 $RPID 2>/dev/null; then
        result PASS "D3: writes completed while resilver still running"
    else
        result FAIL "D3: resilver finished before the writes (no overlap tested)"
    fi
    wait $RPID
    RESILVER_RC=$?
    sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
    if [ "$RESILVER_RC" = "0" ] && disk_state_is "$D3_DISK" ONLINE; then
        result PASS "D3: resilver completed"
        check_rebuilt "$D3_DISK" "D3" "pre" "during"
    else
        result FAIL "D3: resilver rc=$RESILVER_RC"
    fi
fi
teardown "D3"

# D4: bitmap-skip resilver.  Sparse array (5 MB of data), resilver
# with skip=0 (walks every slot) and skip=1 (allocated slots only).
# Timed with time -p (10 ms resolution; date +%s made "0s <= 0s" pass).
d4_resilver() {
    local skip="$1"
    sysctl -w vfs.hammer2.resilver_skip_unalloc=$skip > /dev/null
    fail_disk "$D4_DISK" "D4" || return 1
    fresh_disk "$D4_DISK"
    /usr/bin/time -p hammer2 -s $MNTPT raid replace \
        "$(disk_dev $D4_DISK)" "$(disk_dev $D4_DISK)" \
        > /dev/null 2> /var/tmp/d4_time.txt
    local rc=$?
    D4_T=$(awk '$1 == "real" { printf "%d\n", $2 * 100 }' /var/tmp/d4_time.txt)
    if [ "$rc" = "0" ]; then
        check_rebuilt "$D4_DISK" "D4 skip=$skip (${D4_T}0ms)" "d4"
    else
        result FAIL "D4: skip=$skip resilver rc=$rc"
    fi
}
D4_DISK=2
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/d4_a bs=65536 count=80 2>/dev/null
cp $MNTPT/d4_a $MNTPT/d4_b
sha256 $MNTPT/d4_a $MNTPT/d4_b > /var/tmp/v4_d4.txt
sync
d4_resilver 0
D4_T0=${D4_T:-0}
d4_resilver 1
D4_T1=${D4_T:-0}
sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
if [ "$D4_T0" -gt 0 ] && [ "$D4_T1" -lt "$D4_T0" ]; then
    result PASS "D4: skip=1 faster than skip=0 (${D4_T1}0ms vs ${D4_T0}0ms)"
else
    result FAIL "D4: skip=1 not faster (${D4_T1}0ms vs ${D4_T0}0ms)"
fi
teardown "D4"

# D5: stale re-add.  Fail a disk, keep writing (it misses those
# writes), then resilver it WITHOUT wiping it.  The stale columns for
# old data happen to be correct, the ones for new data are stale or
# empty; afterwards every column must be current.
D5_DISK=1
setup_fresh
check_v3
write_ref_data "old"
if fail_disk "$D5_DISK" "D5"; then
    write_ref_data "new"
    rm $MNTPT/old_b
    dd if=/dev/urandom of=$MNTPT/old_b bs=65536 count=64 2>/dev/null
    sha256 $MNTPT/old_a $MNTPT/old_b > /var/tmp/v4_old.txt
    sync
    replace_disk "$D5_DISK" "D5" && check_rebuilt "$D5_DISK" "D5" "old" "new"
fi
teardown "D5"

# D6: resilver with a second disk still failed.
D6_A=1
D6_B=$((NDISKS - 1))
setup_fresh
check_v3
write_ref_data "d6"
dd if=/dev/urandom of=$MNTPT/d6_big bs=65536 count=400 2>/dev/null
sync
if fail_disk "$D6_A" "D6" && fail_disk "$D6_B" "D6"; then
    fresh_disk "$D6_A"
    fresh_disk "$D6_B"
    if replace_disk "$D6_A" "D6"; then
        # disk B is still failed: this remount reads A's rebuilt
        # columns with only one redundant column left.
        check_rebuilt "$D6_A" "D6a" "d6"
        replace_disk "$D6_B" "D6" && check_rebuilt "$D6_B" "D6b" "d6"
    fi
fi
teardown "D6"

summary
