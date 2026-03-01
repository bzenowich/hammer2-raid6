#!/bin/bash
# Test if the combo test's verify_ref() background subshell pattern
# causes the degraded sync hang.
#
# The combo test uses: (sha256 ...; sha256 ...) > file 2>&1 &
# with a polling loop + wait. All repros use inline timeout+sha256.
# This is the ONLY remaining difference.

DISKDIR=/var/tmp
MNTPT=/mnt/test
LOG=/var/tmp/repro_verify.log
SUBTEST_TIMEOUT=120

> $LOG
exec > >(tee -a $LOG) 2>&1

kldstat -q -m hammer2 || kldload hammer2

rbs() {
    local val=$(sysctl -n vfs.runningbufspace)
    local cnt=$(sysctl -n vfs.runningbufcount)
    echo "  RBS [$1]: $val ($cnt bufs)"
}

# EXACT copy of combo test's verify_ref
verify_ref() {
    local label="$1"
    (sha256 $MNTPT/testfile_a; sha256 $MNTPT/testfile_b) > /var/tmp/combo_check.txt 2>&1 &
    local pid=$!
    local deadline=$(($(date +%s) + SUBTEST_TIMEOUT))
    while kill -0 $pid 2>/dev/null; do
        if [ $(date +%s) -gt $deadline ]; then
            echo "  TIMEOUT: verify_ref hung ($label)"
            kill -9 $pid 2>/dev/null || true
            return 1
        fi
        sleep 1
    done
    wait $pid 2>/dev/null
    echo "  verify_ref done: $label"
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
# Phase 1: 10 warmup cycles using verify_ref (background subshell pattern)
# Matches combo test's single + dual fail read tests exactly
# ======================================================================
echo "=== 4 single-fail warmup cycles (with verify_ref) ==="
for disk in 0 1 2 3; do
    echo "--- single-fail vn${disk} ---"
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/testfile_a > /var/tmp/combo_ref.txt
    sha256 $MNTPT/testfile_b >> /var/tmp/combo_ref.txt
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${disk}
    verify_ref "single-fail vn${disk} (attached)"
    rbs "after verify attached (single vn${disk})"
    vnconfig -u vn${disk} 2>/dev/null || true
    verify_ref "single-fail vn${disk} (detached)"
    rbs "after verify detached (single vn${disk})"
    teardown
    rbs "after teardown (single vn${disk})"
done

echo ""
echo "=== 6 dual-fail warmup cycles (with verify_ref) ==="
for pair in "0 1" "0 2" "0 3" "1 2" "1 3" "2 3"; do
    set -- $pair
    a=$1; b=$2
    echo "--- dual-fail vn${a}+vn${b} ---"
    setup_fresh
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/testfile_a > /var/tmp/combo_ref.txt
    sha256 $MNTPT/testfile_b >> /var/tmp/combo_ref.txt
    sync; sync
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}
    verify_ref "dual-fail vn${a}+vn${b} (attached)"
    rbs "after verify attached (dual ${a}+${b})"
    vnconfig -u vn${a} 2>/dev/null || true
    vnconfig -u vn${b} 2>/dev/null || true
    verify_ref "dual-fail vn${a}+vn${b} (detached)"
    rbs "after verify detached (dual ${a}+${b})"
    teardown
    rbs "after teardown (dual ${a}+${b})"
done

echo ""
echo "=== Now: degraded write + sync ==="
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
