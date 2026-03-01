#!/bin/sh
set -e

umount /mnt/test 2>/dev/null || true
vnconfig -u vn0 2>/dev/null || true
vnconfig -u vn1 2>/dev/null || true
vnconfig -u vn2 2>/dev/null || true
vnconfig -u vn3 2>/dev/null || true

# Fresh disk images
dd if=/dev/zero of=/var/tmp/disk0.img bs=1048576 count=1024 2>/dev/null
dd if=/dev/zero of=/var/tmp/disk1.img bs=1048576 count=1024 2>/dev/null
dd if=/dev/zero of=/var/tmp/disk2.img bs=1048576 count=1024 2>/dev/null
dd if=/dev/zero of=/var/tmp/disk3.img bs=1048576 count=1024 2>/dev/null

vnconfig vn0 /var/tmp/disk0.img
vnconfig vn1 /var/tmp/disk1.img
vnconfig vn2 /var/tmp/disk2.img
vnconfig vn3 /var/tmp/disk3.img

echo "=== Step 1: fresh newfs ==="
newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3

echo "=== Step 2: h2parity_fix -n right after newfs ==="
/var/tmp/h2parity_fix -n /var/tmp/disk0.img /var/tmp/disk1.img /var/tmp/disk2.img /var/tmp/disk3.img | tail -5

echo "=== Step 3: mount, write 4MB file, unmount ==="
mkdir -p /mnt/test
mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST /mnt/test
dd if=/dev/urandom of=/mnt/test/testfile bs=65536 count=64 2>/dev/null
sha256 /mnt/test/testfile > /var/tmp/sha256_before.txt
cat /var/tmp/sha256_before.txt
umount /mnt/test

echo "=== Step 4: h2parity_fix -n after mount+write+umount ==="
/var/tmp/h2parity_fix -n /var/tmp/disk0.img /var/tmp/disk1.img /var/tmp/disk2.img /var/tmp/disk3.img | tail -5

echo "=== Step 5: remount and verify data ==="
mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST /mnt/test
sha256 /mnt/test/testfile > /var/tmp/sha256_after.txt
cat /var/tmp/sha256_after.txt
if diff /var/tmp/sha256_before.txt /var/tmp/sha256_after.txt > /dev/null; then
    echo "DATA INTEGRITY: PASS"
else
    echo "DATA INTEGRITY: FAIL"
fi
umount /mnt/test
echo "DONE"
