#!/bin/sh
# HAMMER2 RAID6 Test D: Online Resilver (self-contained)

DISKDIR=/var/tmp
MNTPT=/mnt/test
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@TEST"

echo "=== HAMMER2 RAID6 Test D: Online Resilver ==="
echo ""

# --- Clean setup ---
echo "--- Setup ---"
if df | grep -q "$MNTPT"; then
    umount $MNTPT
fi
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    vnconfig vn$i $DISKDIR/disk${i}.img
done
mkdir -p $MNTPT
newfs_hammer2 -R 6 -L TEST $DEVSPEC > /dev/null 2>&1 || true
mount -t hammer2 $PFSPATH $MNTPT
echo "Mounted. Writing test data..."

dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
sha256 $MNTPT/testfile_a > /var/tmp/test_d_ref.txt
sha256 $MNTPT/testfile_b >> /var/tmp/test_d_ref.txt
sync; sync
echo "Reference checksums:"
cat /var/tmp/test_d_ref.txt

# --- Fail and detach vn2 ---
echo ""
echo "--- Failing /dev/vn2 ---"
hammer2 -s $MNTPT raid fail-disk /dev/vn2
vnconfig -u vn2 && echo "vnconfig -u vn2: SUCCESS"

# --- Attach fresh image to vn2 and resilver ---
echo ""
echo "--- Attaching fresh image to vn2 for resilver ---"
vnconfig vn2 $DISKDIR/disk4.img
echo "vn2 now backed by disk4.img (fresh)"

echo ""
echo "--- Resilvering /dev/vn2 ---"
time hammer2 -s $MNTPT raid replace /dev/vn2 /dev/vn2 && \
    echo "Resilver: SUCCESS" || { echo "Resilver: FAILED"; exit 1; }

# --- RAID status ---
echo ""
echo "--- RAID status after resilver ---"
hammer2 raid status /dev/vn0

# --- Verify data integrity (still mounted) ---
echo ""
echo "--- Data integrity (array mounted post-resilver) ---"
sha256 $MNTPT/testfile_a > /var/tmp/test_d_post.txt
sha256 $MNTPT/testfile_b >> /var/tmp/test_d_post.txt
if diff -q /var/tmp/test_d_ref.txt /var/tmp/test_d_post.txt > /dev/null 2>&1; then
    echo "DATA INTEGRITY (post-resilver, mounted): PASS"
else
    echo "DATA INTEGRITY (post-resilver, mounted): FAIL"
    diff /var/tmp/test_d_ref.txt /var/tmp/test_d_post.txt
fi

# --- Unmount and remount to confirm persistence ---
echo ""
echo "--- Unmount + remount ---"
umount $MNTPT
# After resilver, vn2 is replaced by vn4's image but still at /dev/vn2
mount -t hammer2 $PFSPATH $MNTPT && echo "Remount: SUCCESS" || { echo "Remount: FAILED"; exit 1; }

echo ""
echo "--- Data integrity after remount ---"
sha256 $MNTPT/testfile_a > /var/tmp/test_d_remount.txt
sha256 $MNTPT/testfile_b >> /var/tmp/test_d_remount.txt
if diff -q /var/tmp/test_d_ref.txt /var/tmp/test_d_remount.txt > /dev/null 2>&1; then
    echo "DATA INTEGRITY (after remount): PASS"
else
    echo "DATA INTEGRITY (after remount): FAIL"
    diff /var/tmp/test_d_ref.txt /var/tmp/test_d_remount.txt
fi

umount $MNTPT
echo ""
echo "=== Test D complete ==="
