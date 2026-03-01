#!/bin/sh
# HAMMER2 RAID6 Test: All failure combinations on 4-disk array
#
# Tests all 4 single-failure and all 6 dual-failure combinations,
# plus 6 dual-failure write tests (write between first and second failure).
# Each sub-test gets fresh disk images to ensure isolation.
#
# Left-symmetric RAID6 rotates P and Q across all disks, so failing
# different disks exercises different reconstruction paths.

DISKDIR=/var/tmp
MNTPT=/mnt/test
kldstat -q -m hammer2 || kldload hammer2
PASS=0
FAIL=0
TOTAL=0
ERRORS=""
SUBTEST_TIMEOUT=120  # seconds per sub-test

result() {
    TOTAL=$((TOTAL + 1))
    if [ "$1" = "PASS" ]; then
        echo "  $1: $2"
        PASS=$((PASS + 1))
    else
        echo "  $1: $2"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}  FAIL: $2
"
    fi
}

setup_fresh() {
    # Unmount if mounted
    umount $MNTPT 2>/dev/null || true
    # Detach all vn devices
    for i in 0 1 2 3; do
        vnconfig -u vn$i 2>/dev/null || true
    done
    # Fresh disk images (rm + truncate = guaranteed zero sparse files)
    for i in 0 1 2 3; do
        rm -f $DISKDIR/disk${i}.img
        truncate -s 1073741824 $DISKDIR/disk${i}.img
    done
    # Attach
    for i in 0 1 2 3; do
        vnconfig vn$i $DISKDIR/disk${i}.img
    done
    # Format and mount
    newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
    mkdir -p $MNTPT
    if ! mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST $MNTPT; then
        echo "  FATAL: mount failed in setup_fresh"
        exit 1
    fi
    # Clear dmesg
    dmesg -c > /dev/null 2>&1
}

write_ref_data() {
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/testfile_a > /var/tmp/combo_ref.txt
    sha256 $MNTPT/testfile_b >> /var/tmp/combo_ref.txt
    sync; sync
}

# Verify with timeout — runs sha256 in background and waits up to SUBTEST_TIMEOUT
verify_ref() {
    local label="$1"
    (sha256 $MNTPT/testfile_a; sha256 $MNTPT/testfile_b) > /var/tmp/combo_check.txt 2>&1 &
    local pid=$!
    local deadline=$(($(date +%s) + SUBTEST_TIMEOUT))
    while kill -0 $pid 2>/dev/null; do
        if [ $(date +%s) -gt $deadline ]; then
            echo "  TIMEOUT: verify_ref hung ($label)"
            kill -9 $pid 2>/dev/null || true
            result FAIL "$label (TIMEOUT)"
            return 1
        fi
        sleep 1
    done
    wait $pid 2>/dev/null
    if diff -q /var/tmp/combo_ref.txt /var/tmp/combo_check.txt > /dev/null 2>&1; then
        result PASS "$label"
    else
        result FAIL "$label"
    fi
}

# Verify new data with timeout
verify_new() {
    local label="$1"
    (sha256 $MNTPT/newfile_c; sha256 $MNTPT/newfile_d) > /var/tmp/combo_newchk.txt 2>&1 &
    local pid=$!
    local deadline=$(($(date +%s) + SUBTEST_TIMEOUT))
    while kill -0 $pid 2>/dev/null; do
        if [ $(date +%s) -gt $deadline ]; then
            echo "  TIMEOUT: verify_new hung ($label)"
            kill -9 $pid 2>/dev/null || true
            result FAIL "$label (TIMEOUT)"
            return 1
        fi
        sleep 1
    done
    wait $pid 2>/dev/null
    if diff -q /var/tmp/combo_new.txt /var/tmp/combo_newchk.txt > /dev/null 2>&1; then
        result PASS "$label"
    else
        result FAIL "$label"
    fi
}

teardown() {
    local label="$1"
    # Check dmesg for CHECK FAILs
    local cfails=$(dmesg | grep -c "CHECK FAIL" || true)
    if [ "$cfails" != "0" ]; then
        result FAIL "$label: $cfails CHECK FAIL(s) in dmesg"
    fi
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do
        vnconfig -u vn$i 2>/dev/null || true
    done
}

echo "=== HAMMER2 RAID6: All Failure Combinations (4-disk) ==="
echo ""

# =====================================================================
# SINGLE-FAILURE READ TESTS (4 tests)
# =====================================================================
echo "=== Single-Failure Read Tests ==="
echo ""

for disk in 0 1 2 3; do
    echo "--- Single fail: vn${disk} ---"
    setup_fresh
    write_ref_data

    # Fail the disk
    hammer2 -s $MNTPT raid fail-disk /dev/vn${disk}

    # Verify degraded read (disk still attached)
    verify_ref "single-fail vn${disk} read (attached)"

    # Detach the failed disk
    vnconfig -u vn${disk} 2>/dev/null || true

    # Verify degraded read (disk detached)
    verify_ref "single-fail vn${disk} read (detached)"

    teardown "single-fail vn${disk}"
    echo ""
done

# =====================================================================
# DUAL-FAILURE READ TESTS (6 tests)
# =====================================================================
echo "=== Dual-Failure Read Tests ==="
echo ""

for pair in "0 1" "0 2" "0 3" "1 2" "1 3" "2 3"; do
    set -- $pair
    a=$1; b=$2
    echo "--- Dual fail: vn${a}+vn${b} ---"
    setup_fresh
    write_ref_data

    # Fail both disks
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}

    # Verify dual-degraded read (disks still attached)
    verify_ref "dual-fail vn${a}+vn${b} read (attached)"

    # Detach both
    vnconfig -u vn${a} 2>/dev/null || true
    vnconfig -u vn${b} 2>/dev/null || true

    # Verify dual-degraded read (disks detached)
    verify_ref "dual-fail vn${a}+vn${b} read (detached)"

    teardown "dual-fail vn${a}+vn${b}"
    echo ""
done

# =====================================================================
# DUAL-FAILURE WRITE TESTS (6 tests)
# Write new data between first and second failure, verify both datasets.
# =====================================================================
echo "=== Dual-Failure Write Tests ==="
echo ""

for pair in "0 1" "0 2" "0 3" "1 2" "1 3" "2 3"; do
    set -- $pair
    a=$1; b=$2
    echo "--- Dual fail write: vn${a}+vn${b} ---"
    setup_fresh
    write_ref_data

    # Fail first disk
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}

    # Write new data while single-degraded
    dd if=/dev/urandom of=$MNTPT/newfile_c bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/newfile_d bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/newfile_c > /var/tmp/combo_new.txt
    sha256 $MNTPT/newfile_d >> /var/tmp/combo_new.txt
    sync; sync

    # Fail second disk
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}

    # Detach both
    vnconfig -u vn${a} 2>/dev/null || true
    vnconfig -u vn${b} 2>/dev/null || true

    # Verify reference data in dual-degraded (detached)
    verify_ref "dual-write vn${a}+vn${b}: ref data (detached)"

    # Verify new data in dual-degraded (detached)
    verify_new "dual-write vn${a}+vn${b}: new data (detached)"

    teardown "dual-write vn${a}+vn${b}"
    echo ""
done

# =====================================================================
# SUMMARY
# =====================================================================
echo "========================================="
echo "=== All Failure Combos: $PASS/$TOTAL passed, $FAIL failed ==="
echo "========================================="
if [ -n "$ERRORS" ]; then
    echo ""
    echo "Failures:"
    echo "$ERRORS"
fi
[ $FAIL -eq 0 ]
