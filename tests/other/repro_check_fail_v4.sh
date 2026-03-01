set -e
DISKDIR=/var/tmp
MNTPT=/mnt/repro
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@REPRO"

umount $MNTPT 2>/dev/null || true
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    echo "Zeroing disk $i..."
    # Just zero the first 256MB to be safe and fast
    dd if=/dev/zero of=$DISKDIR/repro_disk${i}.img bs=1M count=256 conv=notrunc 2>/dev/null || \
    dd if=/dev/zero of=$DISKDIR/repro_disk${i}.img bs=1M count=256 2>/dev/null
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
REF_HASH=$(sha256 -q $MNTPT/data_healthy)

echo "Failing vn2..."
hammer2 -s $MNTPT raid fail-disk /dev/vn2
# Do NOT detach yet, test if breadnx works on failed-but-attached disk

echo "Reading data in degraded mode (attached)..."
sha256 $MNTPT/data_healthy

echo "Detaching vn2..."
vnconfig -u vn2

echo "Reading data in degraded mode (detached)..."
sha256 $MNTPT/data_healthy

echo "Unmounting..."
umount $MNTPT
