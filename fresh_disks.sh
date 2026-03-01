#!/bin/sh
for i in 0 1 2 3 4; do
    vnconfig -u vn${i} 2>/dev/null || true
done
rm -f /var/tmp/disk0.img /var/tmp/disk1.img /var/tmp/disk2.img \
      /var/tmp/disk3.img /var/tmp/disk4.img
for i in 0 1 2 3 4; do
    truncate -s 1073741824 /var/tmp/disk${i}.img
done
ls -lh /var/tmp/disk*.img
