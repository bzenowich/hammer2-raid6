#!/bin/bash
# Find the data volume threshold that causes degraded sync to hang.
# repro_bufspace_dual writes 8MB degraded (1 file) and passes.
# test_all_fail_combos writes 12MB degraded (2 files) and hangs.
# This script tests increasing sizes to find the threshold.

DISKDIR=/var/tmp
MNTPT=/mnt/test
LOG=/var/tmp/repro_volume.log

> $LOG
exec > >(tee -a $LOG) 2>&1

kldstat -q -m hammer2 || kldload hammer2

rbs() {
    local val=$(sysctl -n vfs.runningbufspace)
    local cnt=$(sysctl -n vfs.runningbufcount)
    echo "  RBS [$1]: $val ($cnt bufs)"
}

setup_fresh() {
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do vnconfig -u vn$i 2>/dev/null || true; done
    for i in 0 1 2 3; do rm -f $DISKDIR/disk${i}.img; truncate -s 1073741824 $DISKDIR/disk${i}.img; done
    for i in 0 1 2 3; do vnconfig vn$i $DISKDIR/disk${i}.img; done
    newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
    mkdir -p $MNTPT
    if ! mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST $MNTPT; then
        echo "  FATAL: mount failed"; exit 1
    fi
    dmesg -c > /dev/null 2>&1
}

teardown() {
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do vnconfig -u vn$i 2>/dev/null || true; done
}

rbs "initial"

# Test increasing degraded write sizes: 4MB, 8MB, 10MB, 12MB
for mb in 4 8 10 12; do
    count=$((mb * 16))  # 64KB blocks
    echo "=== Degraded write: ${mb}MB ($count x 64KB) ==="
    setup_fresh

    # Write healthy data first
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    sync; sync
    rbs "after healthy write"

    # Fail disk 0
    hammer2 -s $MNTPT raid fail-disk /dev/vn0
    rbs "after fail vn0"

    # Write degraded data
    dd if=/dev/urandom of=$MNTPT/newfile bs=65536 count=$count 2>/dev/null
    rbs "after degraded dd ${mb}MB"

    # Sync with timeout
    echo "  Syncing (45s timeout)..."
    timeout 45 sync && echo "  PASS: sync OK (${mb}MB)" || echo "  FAIL: sync hung (${mb}MB)"
    rbs "after sync ${mb}MB"

    teardown
    rbs "after teardown ${mb}MB"
    echo ""
done

echo "=== Done ==="
