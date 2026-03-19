#!/bin/sh
# Minimal 5-disk degraded write repro
# Tests each step individually to find where it stalls

DISKDIR=/var/tmp
MNTPT=/mnt/test
kldstat -q -m hammer2 || kldload hammer2

echo "=== 5-disk degraded write repro ==="
for i in 0 1 2 3; do vnconfig -u vn$i 2>/dev/null || true; done

for i in 0 1 2 3; do
    rm -f $DISKDIR/disk${i}.img
    truncate -s 1073741824 $DISKDIR/disk${i}.img
    vnconfig vn$i $DISKDIR/disk${i}.img
done
rm -f $DISKDIR/disk4.img
truncate -s 1073741824 $DISKDIR/disk4.img
vnconfig vn $DISKDIR/disk4.img   # clone: creates vn4

echo "vn devices configured"
ls -la /dev/vn[0-9]

newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 /dev/vn4 > /dev/null 2>&1
mkdir -p $MNTPT
mount -t hammer2 "/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3:/dev/vn4@TEST" $MNTPT
echo "Mounted 5-disk array"

echo "--- Failing vn3 (no prior writes, no sync needed) ---"
hammer2 -s $MNTPT raid fail-disk /dev/vn3
echo "vn3 failed"

echo "--- Writing 1 block in degraded mode ---"
dd if=/dev/urandom of=$MNTPT/test1 bs=65536 count=1 2>/dev/null
echo "1-block write done"

echo "--- sync after 1 block ---"
sync
echo "sync 1 done"

echo "--- Writing 16 blocks ---"
dd if=/dev/urandom of=$MNTPT/test2 bs=65536 count=16 2>/dev/null
echo "16-block write done"
sync
echo "sync 2 done"

echo "--- Writing 128 blocks ---"
dd if=/dev/urandom of=$MNTPT/test3 bs=65536 count=128 2>/dev/null
echo "128-block write done"
sync
echo "sync 3 done (if we get here, no deadlock!)"

echo "--- Checking runningbufspace ---"
sysctl vfs.runningbufspace

umount $MNTPT
for i in 0 1 2 3 4; do vnconfig -u vn$i 2>/dev/null || true; done
sync

echo "=== DONE ==="
