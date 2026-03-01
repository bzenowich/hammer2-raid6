#!/bin/bash
# Instrumented test: tracks runningbufspace between sub-tests
# to find if buffer space leaks across test cycles.

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

check_bufspace() {
    local label="$1"
    local rbs=$(sysctl -n vfs.runningbufspace 2>/dev/null || echo "N/A")
    local dbs=$(sysctl -n vfs.dirtybufspace 2>/dev/null || echo "N/A")
    echo "  BUFCHECK [$label]: runningbufspace=$rbs dirtybufspace=$dbs"
}

teardown() {
    local label="$1"
    local cfails=$(dmesg | grep -c "CHECK FAIL" || true)
    if [ "$cfails" != "0" ]; then
        echo "  CHECK FAIL: $label ($cfails)"
    fi
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do vnconfig -u vn$i 2>/dev/null || true; done
}

echo "=== Leak Check: runningbufspace tracking ==="
check_bufspace "initial"

# Single-failure read tests (same as combo test)
for disk in 0 1 2 3; do
    echo "--- Single fail: vn${disk} ---"
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${disk}
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  PASS (attached)" || echo "  FAIL (attached)"
    vnconfig -u vn${disk} 2>/dev/null || true
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  PASS (detached)" || echo "  FAIL (detached)"
    teardown "single-fail vn${disk}"
    check_bufspace "after single-fail vn${disk}"
done

# Dual-failure read tests (same as combo test)
for pair in "0 1" "0 2" "0 3" "1 2" "1 3" "2 3"; do
    set -- $pair
    a=$1; b=$2
    echo "--- Dual fail: vn${a}+vn${b} ---"
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  PASS (attached)" || echo "  FAIL (attached)"
    vnconfig -u vn${a} 2>/dev/null || true
    vnconfig -u vn${b} 2>/dev/null || true
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  PASS (detached)" || echo "  FAIL (detached)"
    teardown "dual-fail vn${a}+vn${b}"
    check_bufspace "after dual-fail vn${a}+vn${b}"
done

echo ""
echo "=== All read tests done. Buffer state before write test: ==="
check_bufspace "pre-write"
echo ""

# Now the write test that hangs
echo "--- Dual fail WRITE: vn0+vn1 ---"
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
sync; sync
check_bufspace "after initial data write"

echo "Failing vn0..."
hammer2 -s $MNTPT raid fail-disk /dev/vn0
check_bufspace "after fail vn0"

echo "Writing new data while degraded..."
dd if=/dev/urandom of=$MNTPT/newfile_c bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/newfile_d bs=65536 count=64  2>/dev/null
check_bufspace "after degraded write (pre-sync)"

echo "Syncing (30s timeout)..."
timeout 30 sync && echo "  sync OK" || echo "  FAIL: sync timeout"
check_bufspace "after sync"

echo "Failing vn1..."
timeout 15 hammer2 -s $MNTPT raid fail-disk /dev/vn1 && echo "  vn1 failed" || echo "  FAIL: fail-disk timeout"

echo "Detaching..."
vnconfig -u vn0 2>/dev/null || true
vnconfig -u vn1 2>/dev/null || true

echo "Reading..."
timeout 10 sha256 $MNTPT/testfile_a > /dev/null && echo "  PASS: ref" || echo "  FAIL: ref"
timeout 10 sha256 $MNTPT/newfile_c > /dev/null && echo "  PASS: new" || echo "  FAIL: new"

teardown "dual-write vn0+vn1"
check_bufspace "final"

echo "=== Done ==="
