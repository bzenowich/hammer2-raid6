DISKDIR=/var/tmp
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    echo "Zeroing disk $i..."
    # 1GB each
    dd if=/dev/zero of=$DISKDIR/disk${i}.img bs=1M count=1024 2>/dev/null
    vnconfig vn$i $DISKDIR/disk${i}.img
done
# The script will format them
sh /var/tmp/test_f_sequential_fail.sh
