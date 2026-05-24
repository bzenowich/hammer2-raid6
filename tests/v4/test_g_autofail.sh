#!/bin/sh
# Group G: Auto-fail — verify that explicit fail-disk and degraded mount work.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group G: Auto-fail and Degraded Mount (NDISKS=$NDISKS) ==="

# G1: Explicit fail-disk + unmount + remount in degraded mode, verify data
G1_DISK=2
setup_fresh
check_v4
write_ref_data "ref"

hammer2 -s $MNTPT raid fail-disk "$(disk_dev $G1_DISK)" > /dev/null 2>&1
sync; sync
umount $MNTPT

# Remount without disk G1_DISK — should succeed in degraded mode
detach_disk "$G1_DISK"
DEGRADED="$(degraded_spec $G1_DISK)@V4TEST"
if mount -t hammer2 "$DEGRADED" $MNTPT 2>/dev/null; then
    sha256 $MNTPT/ref_a > /var/tmp/g1_check.txt 2>&1
    sha256 $MNTPT/ref_b >> /var/tmp/g1_check.txt 2>&1
    if diff -q /var/tmp/v4_ref.txt /var/tmp/g1_check.txt > /dev/null 2>&1; then
        result PASS "G1: degraded remount (absent disk${G1_DISK}) — data correct"
    else
        result FAIL "G1: degraded remount — data mismatch"
    fi
    umount $MNTPT
else
    result FAIL "G1: degraded remount failed"
fi
# Cleanup all disks
i=0
while [ "$i" -lt "$NDISKS" ]; do
    detach_disk "$i"
    i=$((i + 1))
done

# G2: fail-disk state persists across unmount/remount (voldata persisted)
# Fail second-to-last disk so the index is always valid
G2_DISK=$((NDISKS - 2))
setup_fresh
check_v4
write_ref_data "ref"

hammer2 -s $MNTPT raid fail-disk "$(disk_dev $G2_DISK)" > /dev/null 2>&1
sync; sync

umount $MNTPT

# Remount with all devices present (including G2_DISK still configured)
mount -t hammer2 $PFSPATH $MNTPT
# After remount, disk G2_DISK should still be FAILED.
# `hammer2 raid status <mntpath>` issues HAMMER2IOC_RAID_RESILVER_STATUS
# which now returns the per-disk state populated by init_volumes from
# voldata, so a FAILED line proves both flush and load worked.
status_after=$(hammer2 raid status "$MNTPT" 2>/dev/null || echo "")
if echo "$status_after" | grep -q "^disk\[${G2_DISK}\]:.*FAILED"; then
    result PASS "G2: fail state persisted across unmount/remount"
else
    result FAIL "G2: fail state NOT persisted (disk appears ONLINE after remount)"
    echo "$status_after" | sed 's/^/      /'
fi
teardown "G2"

# G3: Degraded write while one disk is absent, verify correctness
G3_DISK=1
setup_fresh
check_v4
write_ref_data "before"

hammer2 -s $MNTPT raid fail-disk "$(disk_dev $G3_DISK)" > /dev/null 2>&1
detach_disk "$G3_DISK"
write_ref_data "during"
sync; sync

# Bring disk back (fresh), resilver, then verify both datasets
fresh_disk "$G3_DISK"
hammer2 -s $MNTPT raid replace \
    "$(disk_dev $G3_DISK)" "$(disk_dev $G3_DISK)" > /dev/null 2>&1

verify_ref "G3: pre-failure data still correct" "before"
verify_ref "G3: degraded-written data correct" "during"
teardown "G3"

summary
