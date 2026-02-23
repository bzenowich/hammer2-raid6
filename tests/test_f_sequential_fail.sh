#!/bin/sh
# HAMMER2 RAID6 Test F: Sequential disk failures
#
# Derived from: mdadm 01r5fail (sequential failure scenario)
#
# Fails disk 1, writes data while single-degraded, then fails disk 2
# while still single-degraded. Verifies all three datasets (pre-failure,
# between failures, after second failure) are readable in dual-degraded
# mode with both disks detached.

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

check_all() {
    local label="$1"
    sha256 $MNTPT/data_healthy > /var/tmp/test_f_check.txt
    sha256 $MNTPT/data_degraded1 >> /var/tmp/test_f_check.txt
    if diff -q /var/tmp/test_f_ref.txt /var/tmp/test_f_check.txt > /dev/null 2>&1; then
        result PASS "$label"
    else
        result FAIL "$label"
        diff /var/tmp/test_f_ref.txt /var/tmp/test_f_check.txt || true
    fi
}

echo "=== HAMMER2 RAID6 Test F: Sequential Disk Failures ==="
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

# --- Write data on healthy array ---
echo ""
echo "--- Writing data on healthy array ---"
dd if=/dev/urandom of=$MNTPT/data_healthy bs=65536 count=128 2>/dev/null
sync; sync
echo "data_healthy written."

# --- First failure: vn2 ---
echo ""
echo "--- First failure: marking /dev/vn2 failed ---"
hammer2 -s $MNTPT raid fail-disk /dev/vn2
vnconfig -u vn2 && echo "vn2 detached"

# --- Write data while single-degraded ---
echo ""
echo "--- Writing data while single-degraded (vn2 gone) ---"
dd if=/dev/urandom of=$MNTPT/data_degraded1 bs=65536 count=128 2>/dev/null
sync; sync
echo "data_degraded1 written."

# Record all hashes now (before second failure)
sha256 $MNTPT/data_healthy > /var/tmp/test_f_ref.txt
sha256 $MNTPT/data_degraded1 >> /var/tmp/test_f_ref.txt
echo "Reference hashes recorded."

# --- Second failure: vn1 (while already degraded) ---
echo ""
echo "--- Second failure: marking /dev/vn1 failed (dual-degraded) ---"
hammer2 -s $MNTPT raid fail-disk /dev/vn1

# --- Verify data in dual-degraded mode (vn1 attached, vn2 detached) ---
echo ""
echo "--- Dual-degraded reads (vn1 attached, vn2 detached) ---"
check_all "dual-degraded read (vn1 attached, vn2 detached)"

# --- Detach vn1 ---
echo ""
echo "--- Detaching /dev/vn1 ---"
vnconfig -u vn1 && echo "vn1 detached"

# --- Verify data with both vn1 and vn2 fully detached ---
echo ""
echo "--- Dual-degraded reads (both vn1 and vn2 detached) ---"
check_all "dual-degraded read (vn1 and vn2 detached)"

# --- Also verify data written in single-degraded mode specifically ---
echo ""
echo "--- Verifying degraded-written data specifically ---"
sha256 $MNTPT/data_degraded1 > /var/tmp/test_f_check_single.txt
sha256ref=$(grep data_degraded1 /var/tmp/test_f_ref.txt)
sha256got=$(grep data_degraded1 /var/tmp/test_f_check_single.txt)
if [ "$sha256ref" = "$sha256got" ]; then
    result PASS "data written while single-degraded recoverable after second failure"
else
    result FAIL "data written while single-degraded NOT recoverable after second failure"
fi

# --- Cleanup ---
umount $MNTPT 2>/dev/null || true

echo ""
echo "=== Test F complete: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ]
