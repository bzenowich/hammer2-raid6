#!/bin/sh
# Group G: Auto-fail — verify that explicit fail-disk and degraded mount work.
# (True EIO injection requires physical hardware or fault-injection; tested manually.)

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group G: Auto-fail and Degraded Mount ==="

# G1: Explicit fail-disk + unmount + remount in degraded mode, verify data
setup_fresh
check_v4
write_ref_data "ref"

hammer2 -s $MNTPT raid fail-disk /dev/vn2 > /dev/null 2>&1
sync; sync
umount $MNTPT

# Remount without vn2 — should succeed in degraded mode
vnconfig -u vn2 2>/dev/null || true
DEGRADED_SPEC="/dev/vn0:/dev/vn1:/dev/vn3:/dev/vn4:/dev/vn5"
if mount -t hammer2 "${DEGRADED_SPEC}@RZ2TEST" $MNTPT 2>/dev/null; then
    sha256 $MNTPT/ref_a > /var/tmp/g1_check.txt 2>&1
    sha256 $MNTPT/ref_b >> /var/tmp/g1_check.txt 2>&1
    if diff -q /var/tmp/rz2_ref.txt /var/tmp/g1_check.txt > /dev/null 2>&1; then
        result PASS "G1: degraded remount (absent disk) — data correct"
    else
        result FAIL "G1: degraded remount — data mismatch"
    fi
    umount $MNTPT
else
    result FAIL "G1: degraded remount failed"
fi
for i in 0 1 2 3 4 5; do vnconfig -u vn$i 2>/dev/null || true; done

# G2: fail-disk state persists across unmount/remount (voldata persisted)
setup_fresh
check_v4
write_ref_data "ref"

hammer2 -s $MNTPT raid fail-disk /dev/vn4 > /dev/null 2>&1
sync; sync

# Check that disk 4 shows as failed before unmount
status_before=$(hammer2 -s $MNTPT raid status 2>/dev/null || echo "")
umount $MNTPT

# Remount with all devices present (including vn4 still configured)
mount -t hammer2 $PFSPATH $MNTPT
# After remount, disk 4 should still be FAILED
status_after=$(hammer2 -s $MNTPT raid status 2>/dev/null || echo "")
if echo "$status_after" | grep -q "FAILED"; then
    result PASS "G2: fail state persisted across unmount/remount"
else
    result FAIL "G2: fail state NOT persisted (disk appears ONLINE after remount)"
fi
teardown "G2"

# G3: Degraded write while one disk is absent, verify correctness
setup_fresh
check_v4
write_ref_data "before"

hammer2 -s $MNTPT raid fail-disk /dev/vn1 > /dev/null 2>&1
vnconfig -u vn1 2>/dev/null || true
write_ref_data "during"
sync; sync

# Bring vn1 back, resilver, then verify both datasets
vnconfig -S 1073741824 vn1
hammer2 -s $MNTPT raid replace /dev/vn1 /dev/vn1 > /dev/null 2>&1

verify_ref "G3: pre-failure data still correct" "before"
verify_ref "G3: degraded-written data correct" "during"
teardown "G3"

summary
