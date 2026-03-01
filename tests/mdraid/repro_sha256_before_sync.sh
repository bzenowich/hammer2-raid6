#!/bin/bash
# Minimal repro: does sha256 of degraded-written data before sync cause the hang?
#
# The combo test hangs at sync after: fail -> dd -> sha256 -> sync
# The standalone repro works fine:    fail -> dd -> sync (no sha256)
#
# This script tests both patterns to isolate the cause.

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

# ======================================================================
# Test 1: NO sha256 before sync (the working pattern)
# ======================================================================
echo "=== Test 1: fail -> dd -> sync (NO sha256) ==="
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
sync; sync
rbs "after healthy write+sync"

hammer2 -s $MNTPT raid fail-disk /dev/vn0
rbs "after fail vn0"

dd if=/dev/urandom of=$MNTPT/newfile_c bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/newfile_d bs=65536 count=64  2>/dev/null
rbs "after degraded dd (NO sha256)"

echo "  Syncing (30s timeout)..."
timeout 30 sync && echo "  PASS: sync completed" || echo "  FAIL: sync hung"
rbs "after sync"

teardown
rbs "after teardown test1"
echo ""

# ======================================================================
# Test 2: WITH sha256 before sync (the combo test pattern)
# ======================================================================
echo "=== Test 2: fail -> dd -> sha256 -> sync (WITH sha256) ==="
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

# This is what the combo test does: sha256 the degraded-written data
sha256 $MNTPT/newfile_c > /var/tmp/combo_new.txt
sha256 $MNTPT/newfile_d >> /var/tmp/combo_new.txt
rbs "after sha256 (pre-sync)"

echo "  Syncing (30s timeout)..."
timeout 30 sync && echo "  PASS: sync completed" || echo "  FAIL: sync hung"
rbs "after sync"

teardown
rbs "after teardown test2"
echo ""

# ======================================================================
# Test 3: sha256 of HEALTHY data (not degraded-written) before sync
# ======================================================================
echo "=== Test 3: fail -> dd -> sha256(healthy) -> sync ==="
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

# Only sha256 the HEALTHY data (testfile_a/b), not the degraded-written data
sha256 $MNTPT/testfile_a > /dev/null
sha256 $MNTPT/testfile_b > /dev/null
rbs "after sha256 healthy data (pre-sync)"

echo "  Syncing (30s timeout)..."
timeout 30 sync && echo "  PASS: sync completed" || echo "  FAIL: sync hung"
rbs "after sync"

teardown
rbs "after teardown test3"
echo ""

# ======================================================================
# Test 4: WITH sha256 before sync, but run AFTER 10 prior test cycles
# (to test if it's accumulated state, not sha256 per se)
# ======================================================================
echo "=== Test 4: 10 warmup cycles, then fail -> dd -> sha256 -> sync ==="

# Run 10 warmup cycles (matching the 10 read tests in combo)
for cycle in $(seq 1 10); do
    echo "  warmup cycle $cycle..."
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn0
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null
    vnconfig -u vn0 2>/dev/null || true
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null
    teardown
done
rbs "after 10 warmup cycles"

# Now the write test with sha256
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
sync; sync
rbs "after healthy write+sync"

hammer2 -s $MNTPT raid fail-disk /dev/vn0
rbs "after fail vn0"

dd if=/dev/urandom of=$MNTPT/newfile_c bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/newfile_d bs=65536 count=64  2>/dev/null
sha256 $MNTPT/newfile_c > /var/tmp/combo_new.txt
sha256 $MNTPT/newfile_d >> /var/tmp/combo_new.txt
rbs "after degraded dd+sha256"

echo "  Syncing (30s timeout)..."
timeout 30 sync && echo "  PASS: sync completed" || echo "  FAIL: sync hung"
rbs "after sync"

teardown
rbs "after teardown test4"

echo ""
echo "=== Done ==="
