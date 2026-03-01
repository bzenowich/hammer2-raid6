#!/bin/bash
# Track runningbufspace across dual-fail test cycles.
# Mirrors the combo test's dual-fail read pattern.

DISKDIR=/var/tmp
MNTPT=/mnt/test

kldstat -q -m hammer2 || kldload hammer2

rbs() {
    local label="$1"
    local val=$(sysctl -n vfs.runningbufspace)
    local cnt=$(sysctl -n vfs.runningbufcount)
    echo "  RBS [$label]: $val ($cnt bufs)"
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

# Phase 1: Single-fail read tests (4 tests, matching combo)
for disk in 0 1 2 3; do
    echo "--- Single fail: vn${disk} ---"
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${disk}
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  PASS (attached)" || echo "  FAIL"
    vnconfig -u vn${disk} 2>/dev/null || true
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  PASS (detached)" || echo "  FAIL"
    teardown
    rbs "after single-fail vn${disk}"
done

echo ""

# Phase 2: Dual-fail read tests (6 tests, matching combo)
for pair in "0 1" "0 2" "0 3" "1 2" "1 3" "2 3"; do
    set -- $pair
    a=$1; b=$2
    echo "--- Dual fail: vn${a}+vn${b} ---"
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  PASS (attached)" || echo "  FAIL"
    vnconfig -u vn${a} 2>/dev/null || true
    vnconfig -u vn${b} 2>/dev/null || true
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  PASS (detached)" || echo "  FAIL"
    teardown
    rbs "after dual-fail vn${a}+vn${b}"
done

echo ""
echo "=== All read tests done ==="
rbs "pre-write"
echo ""

# Phase 3: One write test (the one that hangs)
echo "--- Dual fail WRITE: vn0+vn1 ---"
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
sync; sync
rbs "after initial write"
echo "Failing vn0..."
hammer2 -s $MNTPT raid fail-disk /dev/vn0
rbs "after fail vn0"
echo "Writing degraded..."
dd if=/dev/urandom of=$MNTPT/newfile bs=65536 count=128 2>/dev/null
rbs "after degraded write (pre-sync)"
echo "Syncing (30s timeout)..."
timeout 30 sync && echo "  sync OK" || echo "  FAIL: sync timeout"
rbs "after sync"

echo "=== Done ==="
