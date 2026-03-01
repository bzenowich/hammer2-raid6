#!/bin/bash
# Test if accumulating mount/fail/unmount cycles causes the degraded sync hang.
# Run N warmup cycles (matching combo test pattern), then do a degraded write+sync.

DISKDIR=/var/tmp
MNTPT=/mnt/test
LOG=/var/tmp/repro_cycles.log

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

# Warmup cycle: matches the combo test's single-fail pattern exactly
warmup_single_fail() {
    local disk=$1
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/testfile_a > /var/tmp/combo_ref.txt
    sha256 $MNTPT/testfile_b >> /var/tmp/combo_ref.txt
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${disk}
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null
    timeout 10 sha256 $MNTPT/testfile_b > /dev/null
    vnconfig -u vn${disk} 2>/dev/null || true
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null
    timeout 10 sha256 $MNTPT/testfile_b > /dev/null
    teardown
}

# Warmup cycle: matches combo test's dual-fail pattern
warmup_dual_fail() {
    local a=$1 b=$2
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/testfile_a > /var/tmp/combo_ref.txt
    sha256 $MNTPT/testfile_b >> /var/tmp/combo_ref.txt
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null
    timeout 10 sha256 $MNTPT/testfile_b > /dev/null
    vnconfig -u vn${a} 2>/dev/null || true
    vnconfig -u vn${b} 2>/dev/null || true
    timeout 10 sha256 $MNTPT/testfile_a > /dev/null
    timeout 10 sha256 $MNTPT/testfile_b > /dev/null
    teardown
}

rbs "initial"

# Run the EXACT same warmup as combo test: 4 single + 6 dual = 10 cycles
echo "=== Running 4 single-fail warmup cycles ==="
for disk in 0 1 2 3; do
    echo "  warmup single-fail vn${disk}..."
    warmup_single_fail $disk
    rbs "after single-fail vn${disk}"
done

echo ""
echo "=== Running 6 dual-fail warmup cycles ==="
for pair in "0 1" "0 2" "0 3" "1 2" "1 3" "2 3"; do
    set -- $pair
    echo "  warmup dual-fail vn${1}+vn${2}..."
    warmup_dual_fail $1 $2
    rbs "after dual-fail vn${1}+vn${2}"
done

echo ""
echo "=== Now: degraded write + sync (the operation that hangs in combo) ==="
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
sha256 $MNTPT/testfile_a > /var/tmp/combo_ref.txt
sha256 $MNTPT/testfile_b >> /var/tmp/combo_ref.txt
sync; sync
rbs "after healthy write+sync"

hammer2 -s $MNTPT raid fail-disk /dev/vn0
rbs "after fail vn0"

dd if=/dev/urandom of=$MNTPT/newfile_c bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/newfile_d bs=65536 count=64  2>/dev/null
sha256 $MNTPT/newfile_c > /var/tmp/combo_new.txt
sha256 $MNTPT/newfile_d >> /var/tmp/combo_new.txt
rbs "after degraded dd+sha256"

echo "  Syncing (45s timeout)..."
timeout 45 sync && echo "  PASS: sync OK" || echo "  FAIL: sync hung"
rbs "after sync"

teardown
rbs "final"
echo "=== Done ==="
