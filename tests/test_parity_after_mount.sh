#!/bin/sh
# Zero disks, newfs, mount, write data, unmount, check parity
dd if=/dev/zero of=/var/tmp/disk0.img bs=65536 count=16384 2>/dev/null
dd if=/dev/zero of=/var/tmp/disk1.img bs=65536 count=16384 2>/dev/null
dd if=/dev/zero of=/var/tmp/disk2.img bs=65536 count=16384 2>/dev/null
dd if=/dev/zero of=/var/tmp/disk3.img bs=65536 count=16384 2>/dev/null
vnconfig vn0 /var/tmp/disk0.img
vnconfig vn1 /var/tmp/disk1.img
vnconfig vn2 /var/tmp/disk2.img
vnconfig vn3 /var/tmp/disk3.img
newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 >/dev/null 2>&1
echo "=== after newfs (fresh disks) ==="
/var/tmp/h2parity_fix -n /var/tmp/disk0.img /var/tmp/disk1.img /var/tmp/disk2.img /var/tmp/disk3.img 2>&1 | tail -5

mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST /mnt/test
dd if=/dev/urandom of=/mnt/test/testfile1 bs=65536 count=64 2>/dev/null
sync
umount /mnt/test
echo "=== after mount+write+unmount ==="
/var/tmp/h2parity_fix -n /var/tmp/disk0.img /var/tmp/disk1.img /var/tmp/disk2.img /var/tmp/disk3.img 2>&1 | tail -5

# Now run newfs AGAIN on the same disks (stale data case)
newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 >/dev/null 2>&1
echo "=== after second newfs (reused disks) ==="
/var/tmp/h2parity_fix -n /var/tmp/disk0.img /var/tmp/disk1.img /var/tmp/disk2.img /var/tmp/disk3.img 2>&1 | tail -5
