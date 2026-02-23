#!/bin/sh
# HAMMER2 RAID6 Test I: Writes concurrent with online resilver
#
# Derived from: mdadm 25raid456-recovery-while-reshape
#
# Writes reference data, fails a disk, attaches a fresh replacement,
# then starts the resilver in the background while concurrently writing
# new data. Verifies both datasets after resilver completes, and again
# after unmount+remount.

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

echo "=== HAMMER2 RAID6 Test I: Writes During Online Resilver ==="
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
# Ensure fresh spare disk
truncate -s 1073741824 $DISKDIR/disk4.img
mkdir -p $MNTPT
newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
mount -t hammer2 $PFSPATH $MNTPT
echo "Mounted."

# --- Write reference data ---
echo ""
echo "--- Writing reference data (healthy array) ---"
dd if=/dev/urandom of=$MNTPT/ref_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/ref_b bs=65536 count=128 2>/dev/null
sha256 $MNTPT/ref_a > /var/tmp/test_i_ref.txt
sha256 $MNTPT/ref_b >> /var/tmp/test_i_ref.txt
sync; sync
echo "Reference data written."

# --- Fail vn2 and attach fresh replacement ---
echo ""
echo "--- Failing vn2, attaching disk4 as replacement ---"
hammer2 -s $MNTPT raid fail-disk /dev/vn2
vnconfig -u vn2
vnconfig vn2 $DISKDIR/disk4.img
echo "vn2 now backed by disk4.img"

# --- Start resilver in background ---
echo ""
echo "--- Starting resilver in background ---"
hammer2 -s $MNTPT raid replace /dev/vn2 /dev/vn2 &
RESILVER_PID=$!
echo "Resilver PID: $RESILVER_PID"

# Small delay to let resilver start
sleep 1

# --- Write new data concurrently with resilver ---
echo ""
echo "--- Writing new data while resilver is running ---"
dd if=/dev/urandom of=$MNTPT/concurrent_c bs=65536 count=64 2>/dev/null
dd if=/dev/urandom of=$MNTPT/concurrent_d bs=65536 count=64 2>/dev/null
sha256 $MNTPT/concurrent_c >> /var/tmp/test_i_ref.txt
sha256 $MNTPT/concurrent_d >> /var/tmp/test_i_ref.txt
sync; sync
echo "Concurrent data written."

# --- Wait for resilver to complete ---
echo ""
echo "--- Waiting for resilver to complete ---"
wait $RESILVER_PID
RESILVER_EXIT=$?
if [ $RESILVER_EXIT -eq 0 ]; then
    result PASS "resilver completed successfully"
else
    result FAIL "resilver exited with code $RESILVER_EXIT"
fi

# --- Verify all data (mounted, post-resilver) ---
echo ""
echo "--- Verifying all data (mounted, post-resilver) ---"
sha256 $MNTPT/ref_a > /var/tmp/test_i_check.txt
sha256 $MNTPT/ref_b >> /var/tmp/test_i_check.txt
sha256 $MNTPT/concurrent_c >> /var/tmp/test_i_check.txt
sha256 $MNTPT/concurrent_d >> /var/tmp/test_i_check.txt
if diff -q /var/tmp/test_i_ref.txt /var/tmp/test_i_check.txt > /dev/null 2>&1; then
    result PASS "all data intact after resilver (mounted)"
else
    result FAIL "data mismatch after resilver (mounted)"
    diff /var/tmp/test_i_ref.txt /var/tmp/test_i_check.txt || true
fi

# --- Unmount and remount ---
echo ""
echo "--- Unmount + remount ---"
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT && echo "Remount: SUCCESS" || {
    result FAIL "remount failed after resilver"
    exit 1
}

# --- Verify all data after remount ---
echo ""
echo "--- Verifying all data (after remount) ---"
sha256 $MNTPT/ref_a > /var/tmp/test_i_check2.txt
sha256 $MNTPT/ref_b >> /var/tmp/test_i_check2.txt
sha256 $MNTPT/concurrent_c >> /var/tmp/test_i_check2.txt
sha256 $MNTPT/concurrent_d >> /var/tmp/test_i_check2.txt
if diff -q /var/tmp/test_i_ref.txt /var/tmp/test_i_check2.txt > /dev/null 2>&1; then
    result PASS "all data intact after remount"
else
    result FAIL "data mismatch after remount"
    diff /var/tmp/test_i_ref.txt /var/tmp/test_i_check2.txt || true
fi

umount $MNTPT 2>/dev/null || true

echo ""
echo "=== Test I complete: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ]
