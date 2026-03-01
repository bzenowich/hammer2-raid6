#!/bin/sh
# HAMMER2 RAID6 Test E: Write during degraded mode
#
# Derived from: mdadm 01raid6integ (extended write scenario)
#
# Writes reference data, marks a disk failed, then writes NEW data while
# the array is degraded. Verifies both datasets are readable degraded and
# after the failed disk is detached.

set -e

DISKDIR=/var/tmp
MNTPT=/mnt/test
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@TEST"
PASS=0
FAIL=0

result() {
    if [ "$1" = "PASS" ]; then
        echo "$1: $2"
        PASS=$((PASS + 1))
    else
        echo "$1: $2"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== HAMMER2 RAID6 Test E: Write During Degraded Mode ==="
echo ""

# --- Setup ---
echo "--- Setup ---"
if df | grep -q "$MNTPT"; then
    umount $MNTPT
fi
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    vnconfig vn$i $DISKDIR/disk${i}.img
done
mkdir -p $MNTPT
newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
mount -t hammer2 $PFSPATH $MNTPT
echo "Mounted at $MNTPT"

# --- Write reference data (healthy array) ---
echo ""
echo "--- Writing reference data (healthy array) ---"
dd if=/dev/urandom of=$MNTPT/ref_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/ref_b bs=65536 count=64  2>/dev/null
sha256 $MNTPT/ref_a > /var/tmp/test_e_ref.txt
sha256 $MNTPT/ref_b >> /var/tmp/test_e_ref.txt
sync; sync
echo "Reference data written and synced."

# --- Fail vn2 ---
echo ""
echo "--- Marking /dev/vn2 as failed ---"
hammer2 -s $MNTPT raid fail-disk /dev/vn2
echo "Array is now single-degraded."

# --- Write new data WHILE degraded ---
echo ""
echo "--- Writing new data while degraded ---"
dd if=/dev/urandom of=$MNTPT/new_c bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/new_d bs=65536 count=64  2>/dev/null
sha256 $MNTPT/new_c > /var/tmp/test_e_new.txt
sha256 $MNTPT/new_d >> /var/tmp/test_e_new.txt
sync; sync
echo "New data written and synced."

# --- Verify all data (vn2 still attached, degraded) ---
echo ""
echo "--- Verifying all data (vn2 failed but attached) ---"

sha256 $MNTPT/ref_a > /var/tmp/test_e_check_ref.txt
sha256 $MNTPT/ref_b >> /var/tmp/test_e_check_ref.txt
if diff -q /var/tmp/test_e_ref.txt /var/tmp/test_e_check_ref.txt > /dev/null 2>&1; then
    result PASS "reference data readable while degraded (vn2 attached)"
else
    result FAIL "reference data mismatch while degraded (vn2 attached)"
fi

sha256 $MNTPT/new_c > /var/tmp/test_e_check_new.txt
sha256 $MNTPT/new_d >> /var/tmp/test_e_check_new.txt
if diff -q /var/tmp/test_e_new.txt /var/tmp/test_e_check_new.txt > /dev/null 2>&1; then
    result PASS "new (degraded-written) data readable while degraded (vn2 attached)"
else
    result FAIL "new (degraded-written) data mismatch while degraded (vn2 attached)"
fi

# --- Detach vn2 ---
echo ""
echo "--- Detaching /dev/vn2 ---"
vnconfig -u vn2 && echo "vnconfig -u vn2: SUCCESS" || echo "vnconfig -u vn2: FAILED"

# --- Verify all data (vn2 fully detached) ---
echo ""
echo "--- Verifying all data (vn2 detached) ---"

sha256 $MNTPT/ref_a > /var/tmp/test_e_check_ref2.txt
sha256 $MNTPT/ref_b >> /var/tmp/test_e_check_ref2.txt
if diff -q /var/tmp/test_e_ref.txt /var/tmp/test_e_check_ref2.txt > /dev/null 2>&1; then
    result PASS "reference data readable (vn2 detached)"
else
    result FAIL "reference data mismatch (vn2 detached)"
fi

sha256 $MNTPT/new_c > /var/tmp/test_e_check_new2.txt
sha256 $MNTPT/new_d >> /var/tmp/test_e_check_new2.txt
if diff -q /var/tmp/test_e_new.txt /var/tmp/test_e_check_new2.txt > /dev/null 2>&1; then
    result PASS "new (degraded-written) data readable (vn2 detached)"
else
    result FAIL "new (degraded-written) data mismatch (vn2 detached)"
fi

# --- Cleanup ---
umount $MNTPT 2>/dev/null || true

echo ""
echo "=== Test E complete: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ]
