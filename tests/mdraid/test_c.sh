#!/bin/sh
# HAMMER2 RAID6 Test C: Dual disk failure
# Assumes FS is mounted at /mnt/test, vn2 already failed+detached from Test B

MNTPT=/mnt/test
REF=/var/tmp/test_b_hashes.txt

echo "=== HAMMER2 RAID6 Test C: Dual Disk Failure ==="
echo "(vn2 already failed from Test B)"
echo ""

if ! df | grep -q "$MNTPT"; then
    echo "ERROR: $MNTPT is not mounted"
    exit 1
fi

# --- Mark vn1 as failed ---
echo "--- Marking /dev/vn1 as failed ---"
hammer2 -s $MNTPT raid fail-disk /dev/vn1
echo ""

# --- Verify reads in dual-degraded mode (vn1+vn2 failed, still attached) ---
echo "--- Dual-degraded read (both disks still attached) ---"

sha256 $MNTPT/testfile_a > /var/tmp/test_c_check.txt
sha256 $MNTPT/testfile_b >> /var/tmp/test_c_check.txt

if diff -q $REF /var/tmp/test_c_check.txt > /dev/null 2>&1; then
    echo "DUAL-DEGRADED READ (vn1+vn2 attached): PASS"
else
    echo "DUAL-DEGRADED READ (vn1+vn2 attached): FAIL"
    diff $REF /var/tmp/test_c_check.txt
fi

# --- Detach vn1 ---
echo ""
echo "--- Detaching /dev/vn1 ---"
vnconfig -u vn1 && echo "vnconfig -u vn1: SUCCESS" || echo "vnconfig -u vn1: FAILED"

# --- Verify reads with both vn1+vn2 fully detached ---
echo ""
echo "--- Dual-degraded read (both disks detached) ---"

sha256 $MNTPT/testfile_a > /var/tmp/test_c_check2.txt
sha256 $MNTPT/testfile_b >> /var/tmp/test_c_check2.txt

if diff -q $REF /var/tmp/test_c_check2.txt > /dev/null 2>&1; then
    echo "DUAL-DEGRADED READ (vn1+vn2 detached): PASS"
else
    echo "DUAL-DEGRADED READ (vn1+vn2 detached): FAIL"
    diff $REF /var/tmp/test_c_check2.txt
fi

echo ""
echo "=== Test C complete ==="
