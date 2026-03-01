#!/bin/bash
# Narrow down which aspect of verify_ref's background subshell causes the hang.
# Three variants tested independently. Each runs 10 warmup cycles then degraded sync.
#
# Variant A: foreground sha256, output to file  (no & no wait)
# Variant B: background sha256, output to /dev/null
# Variant C: background sha256, output to file  (the combo pattern)
#
# Only run ONE variant per invocation (pass A, B, or C as argument).
# This avoids one variant's hang preventing the others from running.

VARIANT=${1:-A}
DISKDIR=/var/tmp
MNTPT=/mnt/test
LOG=/var/tmp/repro_variant_${VARIANT}.log
SUBTEST_TIMEOUT=120

> $LOG
exec > >(tee -a $LOG) 2>&1

kldstat -q -m hammer2 || kldload hammer2

rbs() {
    local val=$(sysctl -n vfs.runningbufspace)
    local cnt=$(sysctl -n vfs.runningbufcount)
    echo "  RBS [$1]: $val ($cnt bufs)"
}

verify_A() {
    # Foreground subshell, output to file
    (sha256 $MNTPT/testfile_a; sha256 $MNTPT/testfile_b) > /var/tmp/combo_check.txt 2>&1
    echo "  verify done: $1"
}

verify_B() {
    # Background subshell, output to /dev/null
    (sha256 $MNTPT/testfile_a; sha256 $MNTPT/testfile_b) > /dev/null 2>&1 &
    local pid=$!
    local deadline=$(($(date +%s) + SUBTEST_TIMEOUT))
    while kill -0 $pid 2>/dev/null; do
        if [ $(date +%s) -gt $deadline ]; then
            echo "  TIMEOUT ($1)"; kill -9 $pid 2>/dev/null || true; return 1
        fi
        sleep 1
    done
    wait $pid 2>/dev/null
    echo "  verify done: $1"
}

verify_C() {
    # Background subshell, output to file (EXACT combo test pattern)
    (sha256 $MNTPT/testfile_a; sha256 $MNTPT/testfile_b) > /var/tmp/combo_check.txt 2>&1 &
    local pid=$!
    local deadline=$(($(date +%s) + SUBTEST_TIMEOUT))
    while kill -0 $pid 2>/dev/null; do
        if [ $(date +%s) -gt $deadline ]; then
            echo "  TIMEOUT ($1)"; kill -9 $pid 2>/dev/null || true; return 1
        fi
        sleep 1
    done
    wait $pid 2>/dev/null
    echo "  verify done: $1"
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

echo "=== Variant $VARIANT ==="
rbs "initial"

# 10 warmup cycles: 4 single + 6 dual
echo "--- 4 single-fail warmup cycles ---"
for disk in 0 1 2 3; do
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/testfile_a > /var/tmp/combo_ref.txt
    sha256 $MNTPT/testfile_b >> /var/tmp/combo_ref.txt
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${disk}
    verify_${VARIANT} "single vn${disk} attached"
    vnconfig -u vn${disk} 2>/dev/null || true
    verify_${VARIANT} "single vn${disk} detached"
    teardown
done
rbs "after 4 single-fail"

echo "--- 6 dual-fail warmup cycles ---"
for pair in "0 1" "0 2" "0 3" "1 2" "1 3" "2 3"; do
    set -- $pair; a=$1; b=$2
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/testfile_a > /var/tmp/combo_ref.txt
    sha256 $MNTPT/testfile_b >> /var/tmp/combo_ref.txt
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}
    verify_${VARIANT} "dual ${a}+${b} attached"
    vnconfig -u vn${a} 2>/dev/null || true
    vnconfig -u vn${b} 2>/dev/null || true
    verify_${VARIANT} "dual ${a}+${b} detached"
    teardown
done
rbs "after 6 dual-fail"

echo "--- Degraded write + sync ---"
setup_fresh
dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
sync; sync
hammer2 -s $MNTPT raid fail-disk /dev/vn0
dd if=/dev/urandom of=$MNTPT/newfile_c bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/newfile_d bs=65536 count=64  2>/dev/null
rbs "before sync"
echo "  Syncing (45s timeout)..."
timeout 45 sync && echo "  PASS" || echo "  FAIL: hung"
rbs "after sync"
teardown
rbs "final"
echo "=== Variant $VARIANT done ==="
