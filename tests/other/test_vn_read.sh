truncate -s 128M /var/tmp/test_vn.img
vnconfig vn4 /var/tmp/test_vn.img
echo "hello" > /mnt/test_vn_file
# simulate failure logic without hammer2
dd if=/dev/vn4 bs=512 count=1 | hexdump -C
vnconfig -u vn4
dd if=/dev/vn4 bs=512 count=1 2>&1 || echo "expected failure"
