set -e
DISKDIR=/var/tmp
MNTPT=/mnt/repro
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@REPRO"

umount $MNTPT 2>/dev/null || true
for i in 0 1 2 3 4; do
    vnconfig -u vn$i 2>/dev/null || true
    truncate -s 2G $DISKDIR/repro_disk${i}.img
    if [ $i -lt 4 ]; then
        vnconfig vn$i $DISKDIR/repro_disk${i}.img
    fi
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
vnconfig -u vn2 2>/dev/null || true

echo "Reading data in degraded mode..."
sha256 $MNTPT/data_healthy || echo "READ FAILED"

echo "Replacing vn2 with vn4 (resilver)..."
vnconfig vn4 $DISKDIR/repro_disk4.img
hammer2 -s $MNTPT raid replace /dev/vn2 /dev/vn4

echo "Waiting for resilver..."
while true; do
    STATUS=$(hammer2 -s $MNTPT raid status | grep "Resilver" || true)
    echo "$STATUS"
    if echo "$STATUS" | grep -q "100%"; then break; fi
    if [ -z "$STATUS" ]; then break; fi
    sleep 2
done

echo "Verifying data after resilver..."
sha256 $MNTPT/data_healthy
GOT_HASH=$(sha256 -q $MNTPT/data_healthy)
if [ "$REF_HASH" = "$GOT_HASH" ]; then
    echo "RESILVER SUCCESS"
else
    echo "RESILVER FAILED"
fi

umount $MNTPT
