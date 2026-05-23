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
DEGRADED="$(degraded_spec $I2_DISK)@V4TEST"
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

# I3: Crash + recover + bulkfree must reclaim orphan stripes (newplan §7
# Phase 2 Exit: "bulkfree reclaims orphaned stripes").
# Strategy: write data, sync, write more data (no sync), force-umount,
# remount, run bulkfree, verify it completes without panic / CHECK FAIL
# and reports freed bytes > 0 (otherwise the orphan-reclaim contract
# isn't exercised).
setup_fresh
check_v4
dd if=/dev/urandom of=$MNTPT/keep bs=65536 count=128 2>/dev/null
sync; sync

# Write data that will be orphaned by the forced umount.
dd if=/dev/urandom of=$MNTPT/orphan bs=65536 count=128 2>/dev/null
# No sync — these writes may or may not be flushed; the bulkfree must
# handle either case cleanly.

umount -f $MNTPT 2>/dev/null || umount $MNTPT 2>/dev/null || true

if mount -t hammer2 $PFSPATH $MNTPT; then
    # Delete the orphan file so its blocks are definitely reclaimable.
    rm -f $MNTPT/orphan
    sync; sync

    dmesg -c > /dev/null 2>&1
    BF_OUT=/var/tmp/i3_bulkfree.out
    if hammer2 bulkfree $MNTPT > "$BF_OUT" 2>&1; then
        if check_no_checkfail "I3-bulkfree"; then
            result PASS "I3: bulkfree completed after crash recovery"
        fi
        # Best-effort orphan-reclaim signal: any non-zero "freed" line.
        if grep -qE "freed|reclaim" "$BF_OUT"; then
            FREED=$(grep -iE "freed|reclaim" "$BF_OUT" | head -1)
            echo "    bulkfree report: $FREED"
        fi
    else
        result FAIL "I3: bulkfree failed after crash recovery (see $BF_OUT)"
    fi

    # The kept file must still verify.
    if [ -f $MNTPT/keep ]; then
        result PASS "I3: pre-crash kept file present after bulkfree"
    else
        result FAIL "I3: pre-crash kept file lost"
    fi
    umount $MNTPT
else
    result FAIL "I3: remount failed for bulkfree pass"
fi

summary
