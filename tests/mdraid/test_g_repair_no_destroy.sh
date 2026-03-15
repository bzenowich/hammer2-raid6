#!/bin/sh
# HAMMER2 RAID6 Test G: Repair does not destroy clean data
#
# Derived from: mdadm 19repair-does-not-destroy and 19raid6repair
#
# Verifies that h2parity_fix reports 0 mismatches on a clean array,
# that running repair mode on clean data leaves it intact, and that
# data is still readable after repair.

DISKDIR=/var/tmp
MNTPT=/mnt/test
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@TEST"
H2FIX=/var/tmp/h2parity_fix
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

check_parity() {
    local label="$1"
    local out
    out=$($H2FIX -n $DISKDIR/disk0.img $DISKDIR/disk1.img \
                    $DISKDIR/disk2.img $DISKDIR/disk3.img 2>&1 | tail -4)
    echo "$out"
    # h2parity_fix outputs "P stripes fixed: N" / "Q stripes fixed: N" /
    # "I/O errors: N" — all three must be 0 for a clean array.
    if echo "$out" | grep -q "P stripes fixed:  0" && \
       echo "$out" | grep -q "Q stripes fixed:  0" && \
       echo "$out" | grep -q "I/O errors:       0"; then
        result PASS "$label: 0 parity mismatches"
    else
        result FAIL "$label: parity mismatches found"
    fi
}

echo "=== HAMMER2 RAID6 Test G: Repair Does Not Destroy ==="
echo ""

if [ ! -x "$H2FIX" ]; then
    echo "ERROR: $H2FIX not found or not executable"
    exit 1
fi

# --- Setup: fresh disks ---
echo "--- Setup: formatting fresh disk images ---"
if df | grep -q "$MNTPT"; then
    umount $MNTPT
fi
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
done
dd if=/dev/zero of=$DISKDIR/disk0.img bs=1048576 count=1024 2>/dev/null
dd if=/dev/zero of=$DISKDIR/disk1.img bs=1048576 count=1024 2>/dev/null
dd if=/dev/zero of=$DISKDIR/disk2.img bs=1048576 count=1024 2>/dev/null
dd if=/dev/zero of=$DISKDIR/disk3.img bs=1048576 count=1024 2>/dev/null
for i in 0 1 2 3; do
    vnconfig vn$i $DISKDIR/disk${i}.img
done
mkdir -p $MNTPT
newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
echo "Formatted."

# --- Check 1: parity correct right after newfs ---
echo ""
echo "--- Check 1: parity after fresh newfs ---"
check_parity "after newfs"

# --- Mount, write data, unmount ---
echo ""
echo "--- Mounting and writing data ---"
mount -t hammer2 $PFSPATH $MNTPT
dd if=/dev/urandom of=$MNTPT/testfile bs=65536 count=128 2>/dev/null
sha256 $MNTPT/testfile > /var/tmp/test_g_sha.txt
echo "Wrote testfile, hash: $(cat /var/tmp/test_g_sha.txt)"
sync; sync
umount $MNTPT
echo "Unmounted."

# --- Check 2: parity correct after mount+write+unmount ---
echo ""
echo "--- Check 2: parity after mount+write+unmount ---"
check_parity "after mount+write+unmount"

# --- Run repair mode (should be a no-op on clean data) ---
echo ""
echo "--- Running h2parity_fix in repair mode (write pass) ---"
$H2FIX $DISKDIR/disk0.img $DISKDIR/disk1.img \
       $DISKDIR/disk2.img $DISKDIR/disk3.img 2>&1 | tail -3

# --- Check 3: parity still correct after repair pass ---
echo ""
echo "--- Check 3: parity after repair pass ---"
check_parity "after repair pass"

# --- Remount and verify data is intact ---
echo ""
echo "--- Remounting and verifying data integrity ---"
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    vnconfig vn$i $DISKDIR/disk${i}.img
done
mount -t hammer2 $PFSPATH $MNTPT
sha256 $MNTPT/testfile > /var/tmp/test_g_sha_after.txt
if diff -q /var/tmp/test_g_sha.txt /var/tmp/test_g_sha_after.txt > /dev/null 2>&1; then
    result PASS "data intact after repair pass"
else
    result FAIL "data corrupted by repair pass"
fi

# --- Run repair a second time (idempotency) ---
umount $MNTPT
echo ""
echo "--- Running h2parity_fix again (idempotency check) ---"
$H2FIX $DISKDIR/disk0.img $DISKDIR/disk1.img \
       $DISKDIR/disk2.img $DISKDIR/disk3.img 2>&1 | tail -3
check_parity "after second repair pass (idempotency)"

# --- Final data check ---
echo ""
echo "--- Final data check after second repair pass ---"
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    vnconfig vn$i $DISKDIR/disk${i}.img
done
mount -t hammer2 $PFSPATH $MNTPT
sha256 $MNTPT/testfile > /var/tmp/test_g_sha_final.txt
if diff -q /var/tmp/test_g_sha.txt /var/tmp/test_g_sha_final.txt > /dev/null 2>&1; then
    result PASS "data intact after second repair pass"
else
    result FAIL "data corrupted by second repair pass"
fi

umount $MNTPT 2>/dev/null || true

echo ""
echo "=== Test G complete: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ]
