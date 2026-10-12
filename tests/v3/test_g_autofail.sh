#!/bin/sh
# Group G: Auto-fail — verify that explicit fail-disk and degraded mount work.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group G: Auto-fail and Degraded Mount (NDISKS=$NDISKS) ==="

# G1: Explicit fail-disk + unmount + remount in degraded mode, verify data
G1_DISK=2
setup_fresh
check_v3
write_ref_data "ref"

fail_disk "$G1_DISK" "G1"
sync; sync
umount $MNTPT

# Remount without disk G1_DISK — should succeed in degraded mode
DEGRADED="$(degraded_spec $G1_DISK)@V3TEST"
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

# G2: fail-disk state persists across unmount/remount (voldata persisted)
# Fail second-to-last disk so the index is always valid
G2_DISK=$((NDISKS - 2))
setup_fresh
check_v3
write_ref_data "ref"

fail_disk "$G2_DISK" "G2"
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

# G3: Degraded write while one disk is failed, resilver onto a wiped
# disk, then verify both datasets cold and scrub.
G3_DISK=1
setup_fresh
check_v3
write_ref_data "before"

if fail_disk "$G3_DISK" "G3"; then
    write_ref_data "during"
    fresh_disk "$G3_DISK"
    if hammer2 -s $MNTPT raid replace \
            "$(disk_dev $G3_DISK)" "$(disk_dev $G3_DISK)" > /dev/null 2>&1 &&
       remount; then
        verify_ref "G3: pre-failure data correct after resilver (cold)" "before"
        verify_ref "G3: degraded-written data correct after resilver (cold)" "during"
        scrub_clean "G3"
    else
        result FAIL "G3: replace or remount failed"
    fi
fi
teardown "G3"

# G4: a disk failed by a read error (here injected, with autofail on)
# is recorded as FAILED in the volume header, so it is not trusted
# again, stale, after a remount.
G4_DISK=$((NDISKS - 2))
setup_fresh
check_v3
write_ref_data "g4"
remount
sysctl -w vfs.hammer2.inject_eio_autofail=1 > /dev/null 2>&1
sysctl -w vfs.hammer2.inject_eio_disk_mask=$((1 << G4_DISK)) > /dev/null 2>&1
verify_ref "G4: data correct while disk${G4_DISK} returns EIO" "g4"
sysctl -w vfs.hammer2.inject_eio_disk_mask=0 > /dev/null 2>&1
sysctl -w vfs.hammer2.inject_eio_autofail=0 > /dev/null 2>&1
if disk_state_is "$G4_DISK" FAILED; then
    result PASS "G4: read EIO failed disk${G4_DISK}"
else
    result FAIL "G4: read EIO did not fail disk${G4_DISK}"
fi
sleep 2		# the deferred record is written by a task
sync; sync
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT
if disk_state_is "$G4_DISK" FAILED; then
    result PASS "G4: read-EIO failure persisted across remount"
else
    result FAIL "G4: disk${G4_DISK} ONLINE after remount (failure not persisted)"
fi
teardown "G4"

# G5: a disk absent at mount is recorded as FAILED, so a later mount
# with all disks does not trust its stale columns.
G5_DISK=$((NDISKS - 1))
setup_fresh
check_v3
write_ref_data "g5"
sync; sync
umount $MNTPT
if mount -t hammer2 "$(degraded_spec $G5_DISK)@V3TEST" $MNTPT 2>/dev/null; then
    write_ref_data "g5b"
    sleep 2
    sync; sync
    umount $MNTPT
    mount -t hammer2 $PFSPATH $MNTPT
    if disk_state_is "$G5_DISK" FAILED; then
        result PASS "G5: absent disk${G5_DISK} persisted as FAILED"
    else
        result FAIL "G5: absent disk${G5_DISK} ONLINE after full remount"
    fi
    verify_ref "G5: data written while degraded is intact" "g5b"
else
    result FAIL "G5: degraded mount failed"
fi
teardown "G5"

summary
