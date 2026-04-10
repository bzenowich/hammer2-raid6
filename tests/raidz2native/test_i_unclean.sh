#!/bin/sh
# Group I: Unclean unmount — power-loss simulation.
# Verifies HAMMER2 journal replay (MEDIA recovery) restores consistent state.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group I: Unclean Unmount (NDISKS=$NDISKS) ==="

# I1: Write data, simulate crash (forced unmount without sync), remount, verify
setup_fresh
check_v4
dd if=/dev/urandom of=$MNTPT/clean bs=65536 count=256 2>/dev/null
sha256 $MNTPT/clean > /var/tmp/i1_ref.txt
sync; sync

# Write more data that may not be flushed — we'll check for consistency, not
# exact content (crash may have dropped in-flight writes)
dd if=/dev/urandom of=$MNTPT/dirty bs=65536 count=64 2>/dev/null
# Force unmount without sync (simulates power loss; hammer2 has no O_SYNC
# requirement — the on-disk state should be consistent at the last checkpoint)
umount -f $MNTPT 2>/dev/null || umount $MNTPT 2>/dev/null || true

# Remount — HAMMER2 should recover to last consistent checkpoint
if mount -t hammer2 $PFSPATH $MNTPT; then
    # The 'clean' file (synced before crash) must be intact
    if [ -f $MNTPT/clean ]; then
        sha256 $MNTPT/clean > /var/tmp/i1_check.txt 2>&1
        if diff -q /var/tmp/i1_ref.txt /var/tmp/i1_check.txt > /dev/null 2>&1; then
            result PASS "I1: fsynced file intact after unclean unmount"
        else
            result FAIL "I1: fsynced file corrupted after unclean unmount"
        fi
    else
        result FAIL "I1: fsynced file missing after unclean unmount"
    fi
    # No CHECK FAIL should appear from the recovery
    check_no_checkfail "I1"
else
    result FAIL "I1: remount failed after unclean unmount"
fi
teardown "I1"

# I2: Unclean unmount while degraded
# Fail disk 3 (valid for NDISKS >= 4)
I2_DISK=3
setup_fresh
check_v4
write_ref_data "ref"

hammer2 -s $MNTPT raid fail-disk "$(disk_dev $I2_DISK)" > /dev/null 2>&1
detach_disk "$I2_DISK"

dd if=/dev/urandom of=$MNTPT/degraded_write bs=65536 count=64 2>/dev/null
sync; sync

# Simulate crash
umount -f $MNTPT 2>/dev/null || umount $MNTPT 2>/dev/null || true

# Remount degraded (without I2_DISK)
DEGRADED="$(degraded_spec $I2_DISK)@RZ2TEST"
if mount -t hammer2 "$DEGRADED" $MNTPT 2>/dev/null; then
    verify_ref "I2: pre-crash reference intact after degraded unclean unmount" "ref"
    check_no_checkfail "I2"
    umount $MNTPT
else
    result FAIL "I2: degraded remount failed after unclean unmount"
fi
# Cleanup all disks
i=0
while [ "$i" -lt "$NDISKS" ]; do
    detach_disk "$i"
    i=$((i + 1))
done

summary
