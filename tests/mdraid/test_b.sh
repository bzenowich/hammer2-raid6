#!/bin/sh
# HAMMER2 RAID6 Test B: Single disk failure + degraded mode
set -e

DISKDIR=/var/tmp
MNTPT=/mnt/test
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@TEST"

echo "=== HAMMER2 RAID6 Test B: Single Disk Failure ==="

# --- Setup ---
echo ""
echo "--- Setup ---"

# Unmount if mounted
if df | grep -q "$MNTPT"; then
    echo "Unmounting $MNTPT..."
    umount $MNTPT
fi

# Reconfigure vn devices (clear and re-assign)
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    vnconfig vn$i $DISKDIR/disk${i}.img
done
echo "vn devices configured."

# Format RAID6 filesystem
echo "Formatting RAID6..."
newfs_hammer2 -R 6 -L TEST $DEVSPEC 2>&1 | tail -5

# Create mount point
mkdir -p $MNTPT

# Mount
echo "Mounting..."
mount -t hammer2 $PFSPATH $MNTPT
echo "Mounted at $MNTPT"

df $MNTPT

# --- Write test data ---
echo ""
echo "--- Writing test data ---"

dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
echo "Wrote 8MB + 4MB test files."

sha256 $MNTPT/testfile_a > /var/tmp/test_b_hashes.txt
sha256 $MNTPT/testfile_b >> /var/tmp/test_b_hashes.txt
cat /var/tmp/test_b_hashes.txt
echo "Checksums recorded."

# Sync to disk before simulating failure
sync; sync
echo "Synced."

# --- Test B: Mark vn2 as failed ---
echo ""
echo "--- Marking /dev/vn2 as failed ---"

hammer2 -s $MNTPT raid fail-disk /dev/vn2
echo ""

echo "RAID status after fail-disk:"
hammer2 raid status /dev/vn0

# --- Verify reads work in degraded mode (disk still attached) ---
echo ""
echo "--- Degraded read verification (vn2 still attached) ---"

sha256 $MNTPT/testfile_a > /var/tmp/test_b_degraded1.txt
sha256 $MNTPT/testfile_b >> /var/tmp/test_b_degraded1.txt

if diff -q /var/tmp/test_b_hashes.txt /var/tmp/test_b_degraded1.txt > /dev/null 2>&1; then
    echo "DEGRADED READ (vn2 attached): PASS"
else
    echo "DEGRADED READ (vn2 attached): FAIL"
    diff /var/tmp/test_b_hashes.txt /var/tmp/test_b_degraded1.txt
fi

# --- Detach vn2 ---
echo ""
echo "--- Detaching /dev/vn2 ---"

vnconfig -u vn2 && echo "vnconfig -u vn2: SUCCESS" || echo "vnconfig -u vn2: FAILED"

# --- Verify reads still work with vn2 detached ---
echo ""
echo "--- Degraded read verification (vn2 detached) ---"

sha256 $MNTPT/testfile_a > /var/tmp/test_b_degraded2.txt
sha256 $MNTPT/testfile_b >> /var/tmp/test_b_degraded2.txt

if diff -q /var/tmp/test_b_hashes.txt /var/tmp/test_b_degraded2.txt > /dev/null 2>&1; then
    echo "DEGRADED READ (vn2 detached): PASS"
else
    echo "DEGRADED READ (vn2 detached): FAIL"
    diff /var/tmp/test_b_hashes.txt /var/tmp/test_b_degraded2.txt
fi

echo ""
echo "=== Test B complete ==="
