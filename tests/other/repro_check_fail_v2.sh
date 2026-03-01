set -e
DISKDIR=/var/tmp
MNTPT=/mnt/repro
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@REPRO"

umount $MNTPT 2>/dev/null || true
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    truncate -s 2G $DISKDIR/repro_disk${i}.img
    vnconfig vn$i $DISKDIR/repro_disk${i}.img
done

echo "Formatting RAID6..."
newfs_hammer2 -R 6 -L REPRO /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null

echo "Mounting..."
mkdir -p $MNTPT
mount -t hammer2 $PFSPATH $MNTPT

echo "Writing healthy data..."
dd if=/dev/urandom of=$MNTPT/data_healthy bs=64k count=100 2>/dev/null
sync; sleep 2

echo "Failing vn2..."
hammer2 -s $MNTPT raid fail-disk /dev/vn2
vnconfig -u vn2 2>/dev/null || true

echo "Writing data while degraded..."
dd if=/dev/urandom of=$MNTPT/data_degraded bs=64k count=100 2>/dev/null
sync; sleep 2

echo "Failing vn1 (dual-degraded)..."
hammer2 -s $MNTPT raid fail-disk /dev/vn1
vnconfig -u vn1 2>/dev/null || true

echo "Reading data..."
sha256 $MNTPT/data_healthy || echo "READ FAILED for data_healthy"
sha256 $MNTPT/data_degraded || echo "READ FAILED for data_degraded"

echo "Unmounting..."
umount $MNTPT
