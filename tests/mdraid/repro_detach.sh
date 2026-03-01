#!/bin/bash
# Minimal repro: single test + dual-fail write + vnconfig -u detach
# Tests whether detaching failed disks while hammer2 is mounted causes hangs.

DISKDIR=/var/tmp
MNTPT=/mnt/test

kldstat -q -m hammer2 || kldload hammer2

setup_fresh() {
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do vnconfig -u vn$i 2>/dev/null || true; done
    for i in 0 1 2 3; do rm -f $DISKDIR/disk${i}.img; truncate -s 1073741824 $DISKDIR/disk${i}.img; done
    for i in 0 1 2 3; do vnconfig vn$i $DISKDIR/disk${i}.img; done
    newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
    mkdir -p $MNTPT
    if ! mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST $MNTPT; then
        echo "FATAL: mount failed"; exit 1
    fi
    dmesg -c > /dev/null 2>&1
}

echo "=== Test 1: single-fail vn0 + detach ==="
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile bs=65536 count=64 2>/dev/null
sync
echo "Failing vn0..."
hammer2 -s $MNTPT raid fail-disk /dev/vn0
echo "Reading degraded..."
timeout 10 sha256 $MNTPT/testfile > /dev/null && echo "  PASS: degraded read" || echo "  FAIL: degraded read"
echo "Detaching vn0..."
vnconfig -u vn0 2>/dev/null || true
echo "Reading after detach..."
timeout 10 sha256 $MNTPT/testfile > /dev/null && echo "  PASS: detached read" || echo "  FAIL: detached read"
echo ""

echo "=== Test 2: dual-fail vn0+vn1 + detach ==="
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile bs=65536 count=64 2>/dev/null
sync
echo "Failing vn0..."
hammer2 -s $MNTPT raid fail-disk /dev/vn0
echo "Failing vn1..."
hammer2 -s $MNTPT raid fail-disk /dev/vn1
echo "Reading dual-degraded..."
timeout 10 sha256 $MNTPT/testfile > /dev/null && echo "  PASS: dual-degraded read" || echo "  FAIL: dual-degraded read"
echo "Detaching vn0+vn1..."
vnconfig -u vn0 2>/dev/null || true
vnconfig -u vn1 2>/dev/null || true
echo "Reading after detach..."
timeout 10 sha256 $MNTPT/testfile > /dev/null && echo "  PASS: detached read" || echo "  FAIL: detached read"
echo ""

echo "=== Test 3: dual-fail WRITE vn0+vn1 + detach ==="
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile bs=65536 count=64 2>/dev/null
sync
echo "Failing vn0..."
hammer2 -s $MNTPT raid fail-disk /dev/vn0
echo "Writing while degraded..."
dd if=/dev/urandom of=$MNTPT/newfile bs=65536 count=128 2>/dev/null
echo "Syncing (15s timeout)..."
timeout 15 sync && echo "  sync OK" || echo "  FAIL: sync timeout"
echo "Failing vn1..."
timeout 15 hammer2 -s $MNTPT raid fail-disk /dev/vn1 && echo "  vn1 failed" || echo "  FAIL: fail-disk timeout"
echo "Detaching vn0+vn1..."
vnconfig -u vn0 2>/dev/null || true
vnconfig -u vn1 2>/dev/null || true
echo "Reading after detach..."
timeout 10 sha256 $MNTPT/testfile > /dev/null && echo "  PASS: ref read" || echo "  FAIL: ref read"
timeout 10 sha256 $MNTPT/newfile > /dev/null && echo "  PASS: new read" || echo "  FAIL: new read"
echo ""

echo "=== Test 4: repeat test 3 (accumulated state check) ==="
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile bs=65536 count=64 2>/dev/null
sync
echo "Failing vn0..."
hammer2 -s $MNTPT raid fail-disk /dev/vn0
echo "Writing while degraded..."
dd if=/dev/urandom of=$MNTPT/newfile bs=65536 count=128 2>/dev/null
echo "Syncing (15s timeout)..."
timeout 15 sync && echo "  sync OK" || echo "  FAIL: sync timeout"
echo "Failing vn1..."
timeout 15 hammer2 -s $MNTPT raid fail-disk /dev/vn1 && echo "  vn1 failed" || echo "  FAIL: fail-disk timeout"
echo "Detaching vn0+vn1..."
vnconfig -u vn0 2>/dev/null || true
vnconfig -u vn1 2>/dev/null || true
echo "Reading after detach..."
timeout 10 sha256 $MNTPT/testfile > /dev/null && echo "  PASS: ref read" || echo "  FAIL: ref read"
timeout 10 sha256 $MNTPT/newfile > /dev/null && echo "  PASS: new read" || echo "  FAIL: new read"
echo ""

echo "=== Done ==="
