#!/bin/bash
# Track runningbufspace across test cycles to find the leak source.
# Each step prints runningbufspace so we can see exactly when it grows.

DISKDIR=/var/tmp
MNTPT=/mnt/test

kldstat -q -m hammer2 || kldload hammer2

rbs() {
    local label="$1"
    local val=$(sysctl -n vfs.runningbufspace)
    local cnt=$(sysctl -n vfs.runningbufcount)
    echo "  RBS [$label]: $val ($cnt bufs)"
}

rbs "initial"

# Run 10 cycles of: create disks, format, mount, write, fail, read, unmount, destroy
for cycle in $(seq 1 10); do
    echo "=== Cycle $cycle ==="

    # Clean up from previous
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do vnconfig -u vn$i 2>/dev/null || true; done
    rbs "after cleanup"

    # Create fresh disks
    for i in 0 1 2 3; do rm -f $DISKDIR/disk${i}.img; truncate -s 1073741824 $DISKDIR/disk${i}.img; done
    for i in 0 1 2 3; do vnconfig vn$i $DISKDIR/disk${i}.img; done
    rbs "after vnconfig"

    # Format
    newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
    rbs "after newfs"

    # Mount
    mkdir -p $MNTPT
    if ! mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST $MNTPT; then
        echo "  FATAL: mount failed"
        exit 1
    fi
    rbs "after mount"
    dmesg -c > /dev/null 2>&1

    # Write data
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sync; sync
    rbs "after write+sync"

    # Fail disk 0
    hammer2 -s $MNTPT raid fail-disk /dev/vn0
    rbs "after fail vn0"

    # Read degraded
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  read: PASS" || echo "  read: FAIL"
    rbs "after degraded read"

    # Detach failed disk
    vnconfig -u vn0 2>/dev/null || true
    rbs "after detach vn0"

    # Read with disk detached
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  read2: PASS" || echo "  read2: FAIL"
    rbs "after detached read"

    # Unmount
    umount $MNTPT 2>/dev/null || true
    rbs "after umount"

    # Detach remaining
    for i in 1 2 3; do vnconfig -u vn$i 2>/dev/null || true; done
    rbs "after vnconfig -u all"

    echo ""
done

echo "=== Done ==="
rbs "final"
