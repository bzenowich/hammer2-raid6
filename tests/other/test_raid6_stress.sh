#!/bin/sh
# HAMMER2 RAID6 stress test
# Tests: A (parallel I/O), B (single disk failure), C (dual disk failure)

set -e

MNTPT=/mnt
DISKDIR=/var/tmp
N=4  # parallel workers (reduced from 8 to fit on 1GB disks)

echo "=== HAMMER2 RAID6 Stress Test ==="
echo ""

# Verify the filesystem is mounted
if ! df | grep -q "$MNTPT"; then
    echo "ERROR: $MNTPT is not mounted"
    exit 1
fi

# --- Test A: Parallel read/write stress (healthy array) ---
echo "=== Test A: Parallel read/write stress ==="

echo "Writing $N files in parallel (32MB each)..."
for i in $(seq 1 $N); do
    dd if=/dev/urandom of=$MNTPT/testfile_$i bs=65536 count=512 2>/dev/null &
done
wait
echo "Write complete."

echo "Hashing files..."
for i in $(seq 1 $N); do sha256 $MNTPT/testfile_$i; done > /var/tmp/hashes_before.txt

echo "Re-reading and hashing..."
for i in $(seq 1 $N); do sha256 $MNTPT/testfile_$i; done > /var/tmp/hashes_after.txt

if diff -q /var/tmp/hashes_before.txt /var/tmp/hashes_after.txt > /dev/null 2>&1; then
    echo "TEST A: PASS"
else
    echo "TEST A: FAIL"
    diff /var/tmp/hashes_before.txt /var/tmp/hashes_after.txt
fi

echo ""

# --- Test B: Single disk failure ---
echo "=== Test B: Single disk failure (degrade vn2) ==="

sha256 $MNTPT/testfile_1 > /var/tmp/ref1.txt
echo "Reference hash recorded."

echo "Disconnecting vn2..."
vnconfig -u vn2

echo "Reading back with degraded array..."
sha256 $MNTPT/testfile_1 > /var/tmp/deg1.txt

if diff -q /var/tmp/ref1.txt /var/tmp/deg1.txt > /dev/null 2>&1; then
    echo "TEST B (DEGRADED-1): PASS"
else
    echo "TEST B (DEGRADED-1): FAIL"
    diff /var/tmp/ref1.txt /var/tmp/deg1.txt
fi

echo "Reconnecting vn2..."
vnconfig vn2 $DISKDIR/disk2.img

echo ""

# --- Test C: Dual disk failure ---
echo "=== Test C: Dual disk failure (degrade vn1 and vn3) ==="

echo "Disconnecting vn1 and vn3..."
vnconfig -u vn1
vnconfig -u vn3

echo "Reading back with dual-degraded array..."
sha256 $MNTPT/testfile_1 > /var/tmp/deg2.txt 2>/dev/null || true

if diff -q /var/tmp/ref1.txt /var/tmp/deg2.txt > /dev/null 2>&1; then
    echo "TEST C (DEGRADED-2): PASS"
else
    echo "TEST C (DEGRADED-2): FAIL (may need parity data written first)"
    diff /var/tmp/ref1.txt /var/tmp/deg2.txt 2>/dev/null || true
fi

echo "Reconnecting vn1 and vn3..."
vnconfig vn1 $DISKDIR/disk1.img
vnconfig vn3 $DISKDIR/disk3.img

echo ""
echo "=== Stress test complete ==="
