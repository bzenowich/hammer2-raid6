#!/bin/bash
# Minimal repro: single degraded write + sync
# Does this hang on its own, without any prior test cycles?

DISKDIR=/var/tmp
MNTPT=/mnt/test
LOG=/var/tmp/repro_sync.log

exec > >(tee -a $LOG) 2>&1
> $LOG

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

echo "=== Test A: degraded write + sync (NO sha256, NO prior cycles) ==="
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
sync; sync
rbs "after healthy write+sync"

hammer2 -s $MNTPT raid fail-disk /dev/vn0
rbs "after fail vn0"

dd if=/dev/urandom of=$MNTPT/newfile_c bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/newfile_d bs=65536 count=64  2>/dev/null
rbs "after degraded dd"

echo "  Syncing (60s timeout)..."
timeout 60 sync && echo "  PASS: sync OK" || echo "  FAIL: sync hung"
rbs "after sync"

teardown
rbs "after teardown A"
echo ""
echo "=== Test A done ==="
