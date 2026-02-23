#!/bin/sh
# HAMMER2 RAID6 online resilver test (Test D)
# Requires the filesystem to be mounted and test data already written.

set -e

MNTPT=/mnt
DISKDIR=/var/tmp
SEL=/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@DATA

echo "=== HAMMER2 RAID6 Online Resilver Test (Test D) ==="
echo ""

# 1. Record reference hash
echo "Recording reference hash..."
sha256 $MNTPT/testfile_1 > /var/tmp/ref_resilver.txt
cat /var/tmp/ref_resilver.txt

# 2. Simulate disk 2 failure
echo ""
echo "Simulating disk 2 (vn2) failure..."
vnconfig -u vn2

# 3. Verify degraded reads still work
echo "Verifying degraded read..."
sha256 $MNTPT/testfile_1 > /var/tmp/deg_before_resilver.txt
if diff -q /var/tmp/ref_resilver.txt /var/tmp/deg_before_resilver.txt > /dev/null 2>&1; then
    echo "Degraded read: PASS"
else
    echo "Degraded read: FAIL"
fi

# 4. Prepare replacement disk
echo ""
echo "Preparing replacement disk..."
truncate -s 1073741824 $DISKDIR/disk_new.img
vnconfig vn4 $DISKDIR/disk_new.img

# 5. Online replace (filesystem stays mounted)
echo ""
echo "Starting online resilver..."
hammer2 -s $SEL raid replace /dev/vn2 /dev/vn4
echo "Resilver command returned $?"

# 6. Verify data intact after resilver
echo ""
echo "Verifying data after resilver..."
sha256 $MNTPT/testfile_1 > /var/tmp/after_resilver.txt
if diff -q /var/tmp/ref_resilver.txt /var/tmp/after_resilver.txt > /dev/null 2>&1; then
    echo "TEST D (RESILVER): PASS"
else
    echo "TEST D (RESILVER): FAIL"
    diff /var/tmp/ref_resilver.txt /var/tmp/after_resilver.txt
fi

# 7. Cleanup
echo ""
echo "Cleaning up..."
vnconfig -u vn4
rm -f $DISKDIR/disk_new.img

echo "=== Resilver test complete ==="
