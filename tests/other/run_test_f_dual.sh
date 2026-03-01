DISKDIR=/var/tmp
MNTPT=/mnt/test
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@TEST"

# Cleanup
umount $MNTPT 2>/dev/null || true
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    echo "Zeroing disk $i..."
    dd if=/dev/zero of=$DISKDIR/disk${i}.img bs=1M count=1024 2>/dev/null
    vnconfig vn$i $DISKDIR/disk${i}.img
done

echo "Formatting RAID6..."
newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null

echo "Mounting..."
mkdir -p $MNTPT
mount -t hammer2 $PFSPATH $MNTPT

echo "Writing initial data..."
dd if=/dev/urandom of=$MNTPT/data_healthy bs=64k count=128 2>/dev/null
sync; sleep 2
REF_HASH=$(sha256 -q $MNTPT/data_healthy)

echo "Failing vn2 and vn1..."
hammer2 -s $MNTPT raid fail-disk /dev/vn2
hammer2 -s $MNTPT raid fail-disk /dev/vn1
vnconfig -u vn2 2>/dev/null || true
vnconfig -u vn1 2>/dev/null || true

echo "Reading data in dual-degraded mode..."
GOT_HASH=$(sha256 -q $MNTPT/data_healthy || echo "READ_ERROR")
echo "REF: $REF_HASH"
echo "GOT: $GOT_HASH"

if [ "$REF_HASH" = "$GOT_HASH" ]; then
    echo "DUAL DEGRADED DATA SUCCESS"
else
    echo "DUAL DEGRADED DATA FAILED"
fi

umount $MNTPT 2>/dev/null || true
