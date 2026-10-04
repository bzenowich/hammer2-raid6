#!/bin/sh
# Group I: Forced unmount.
#
# NOT a crash test.  `umount -f` on hammer2 still runs the full unmount
# flush (hammer2_vfs_unmount syncs even with MNT_FORCE), so these cases
# remount a cleanly-flushed filesystem.  They check that the forced
# path loses nothing and that bulkfree reclaims space afterwards.  The
# power-loss case needs the guest reset under writes, which only the
# host can do: see crash_host.sh.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group I: Forced Unmount (NDISKS=$NDISKS) ==="

# I1: Write data, forced unmount, remount, verify
setup_fresh
check_v3
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
            result PASS "I1: synced file intact after forced unmount"
        else
            result FAIL "I1: synced file corrupted after forced unmount"
        fi
    else
        result FAIL "I1: synced file missing after forced unmount"
    fi
    # No CHECK FAIL should appear from the recovery
    check_no_checkfail "I1"
else
    result FAIL "I1: remount failed after forced unmount"
fi
teardown "I1"

# I2: Unclean unmount while degraded
# Fail disk 3 (valid for NDISKS >= 4)
I2_DISK=3
setup_fresh
check_v3
write_ref_data "ref"

fail_disk "$I2_DISK" "I2"

dd if=/dev/urandom of=$MNTPT/degraded_write bs=65536 count=64 2>/dev/null
sync; sync

# Forced unmount
umount -f $MNTPT 2>/dev/null || umount $MNTPT 2>/dev/null || true

# Remount degraded (without I2_DISK)
DEGRADED="$(degraded_spec $I2_DISK)@V3TEST"
if mount -t hammer2 "$DEGRADED" $MNTPT 2>/dev/null; then
    verify_ref "I2: reference intact after degraded forced unmount" "ref"
    check_no_checkfail "I2"
    umount $MNTPT
else
    result FAIL "I2: degraded remount failed after forced unmount"
fi

# I3: forced unmount, remount, delete, bulkfree twice (the second pass
# frees what both found unreferenced): df must show the orphan's space
# back, the kept file must verify cold and the array scrub clean.
# J1 and N prove freed space is reused safely.
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/keep bs=65536 count=128 2>/dev/null
sha256 $MNTPT/keep > /var/tmp/i3_keep.txt
sync; sync
dd if=/dev/urandom of=$MNTPT/orphan bs=65536 count=128 2>/dev/null
umount -f $MNTPT 2>/dev/null || umount $MNTPT 2>/dev/null || true

if mount -t hammer2 $PFSPATH $MNTPT; then
    rm -f $MNTPT/orphan
    sync; sync
    kmsg_clear
    used0=$(df -k $MNTPT | awk 'NR == 2 { print $3 }')
    if hammer2 bulkfree $MNTPT > /var/tmp/i3_bulkfree.out 2>&1 &&
       hammer2 bulkfree $MNTPT >> /var/tmp/i3_bulkfree.out 2>&1; then
        check_no_checkfail "I3-bulkfree" &&
            result PASS "I3: bulkfree completed after forced unmount"
    else
        result FAIL "I3: bulkfree failed (see /var/tmp/i3_bulkfree.out)"
    fi
    sleep 2     # statfs recounts used slots at most once a second
    used1=$(df -k $MNTPT | awk 'NR == 2 { print $3 }')
    if [ "$((used0 - used1))" -ge 4096 ]; then
        result PASS "I3: bulkfree returned the orphan's space (used ${used0}K -> ${used1}K)"
    else
        result FAIL "I3: bulkfree did not return the orphan's space (used ${used0}K -> ${used1}K)"
    fi
    if remount; then
        sha256 $MNTPT/keep > /var/tmp/i3_keep_check.txt 2>&1
        if diff -q /var/tmp/i3_keep.txt /var/tmp/i3_keep_check.txt > /dev/null 2>&1; then
            result PASS "I3: kept file content intact after bulkfree (cold)"
        else
            result FAIL "I3: kept file content changed after bulkfree"
        fi
        scrub_clean "I3"
    else
        result FAIL "I3: remount after bulkfree failed"
    fi
else
    result FAIL "I3: remount failed for bulkfree pass"
fi
teardown "I3"

summary
