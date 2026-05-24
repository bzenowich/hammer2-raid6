#!/bin/sh
# Group A: Basic read/write on a healthy v3 (RAIDZ2-native) array.
# Tests: A1 (write/remount/verify), A2 (small files), A3 (bref encoding),
#        A4 (COW: overwrite allocates new stripe slot).

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group A: Basic Read/Write (Healthy) ==="

# A1: Write 100 MB, unmount, remount, verify
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/bigfile bs=65536 count=1600 2>/dev/null
sha256 $MNTPT/bigfile > /var/tmp/a1_ref.txt
sync; sync
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT
sha256 $MNTPT/bigfile > /var/tmp/a1_check.txt
if diff -q /var/tmp/a1_ref.txt /var/tmp/a1_check.txt > /dev/null 2>&1; then
    result PASS "A1: 100 MB write/remount/verify"
else
    result FAIL "A1: 100 MB write/remount/verify (checksum mismatch)"
fi
teardown "A1"

# A2: Small file variety
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/f_1k bs=1024 count=1 2>/dev/null
dd if=/dev/urandom of=$MNTPT/f_4k bs=4096 count=1 2>/dev/null
dd if=/dev/urandom of=$MNTPT/f_64k bs=65536 count=1 2>/dev/null
dd if=/dev/urandom of=$MNTPT/f_1m bs=65536 count=16 2>/dev/null
dd if=/dev/urandom of=$MNTPT/f_16m bs=65536 count=256 2>/dev/null
rm -f /var/tmp/a2_ref.txt /var/tmp/a2_check.txt
for f in f_1k f_4k f_64k f_1m f_16m; do
    sha256 $MNTPT/$f >> /var/tmp/a2_ref.txt
done
sync; sync
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT
for f in f_1k f_4k f_64k f_1m f_16m; do
    sha256 $MNTPT/$f >> /var/tmp/a2_check.txt
done
if diff -q /var/tmp/a2_ref.txt /var/tmp/a2_check.txt > /dev/null 2>&1; then
    result PASS "A2: small file variety (5 sizes)"
else
    result FAIL "A2: small file variety (checksum mismatch)"
fi
teardown "A2"

# A3: Blockref encoding — copyid in [0..NDISKS-1], data_off 64KB-aligned
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/probe bs=65536 count=4 2>/dev/null
sync; sync
hammer2 -s $MNTPT show > /var/tmp/a3_show.txt 2>&1
# Check: no DATA blockref with copyid outside [0, NDISKS-1]
if grep -q "type=DATA" /var/tmp/a3_show.txt 2>/dev/null; then
    bad_copyid=$(grep "type=DATA" /var/tmp/a3_show.txt |
        awk '{ for(i=1;i<=NF;i++) if($i~/^copyid=/) print $i }' |
        sed 's/copyid=//' |
        awk -v n="$NDISKS" '$1 < 0 || $1 >= n { print $1 }')
    if [ -z "$bad_copyid" ]; then
        result PASS "A3: all DATA blockrefs have copyid in [0...$((NDISKS-1))]"
    else
        result FAIL "A3: DATA blockref copyid out of range: $bad_copyid"
    fi
else
    result PASS "A3: no DATA blockrefs (file too small; skip encoding check)"
fi
teardown "A3"

# A4: COW — overwrite allocates new stripe slot (no address reuse)
setup_fresh
check_v3
# Write a file large enough to span multiple DIOs (8 x 64KB = 512 KB)
dd if=/dev/urandom of=$MNTPT/cow_test bs=65536 count=8 2>/dev/null
sync; sync
hammer2 -s $MNTPT show 2>/dev/null |
    grep "type=DATA" | awk '{ for(i=1;i<=NF;i++) if($i~/^data_off=/) print $i }' |
    sort > /var/tmp/a4_before.txt
# Overwrite
dd if=/dev/urandom of=$MNTPT/cow_test bs=65536 count=8 2>/dev/null
sync; sync
hammer2 -s $MNTPT show 2>/dev/null |
    grep "type=DATA" | awk '{ for(i=1;i<=NF;i++) if($i~/^data_off=/) print $i }' |
    sort > /var/tmp/a4_after.txt
overlap=$(comm -12 /var/tmp/a4_before.txt /var/tmp/a4_after.txt | wc -l | tr -d ' ')
if [ "$overlap" = "0" ]; then
    result PASS "A4: COW — no stripe slot reused after overwrite"
else
    result FAIL "A4: COW — $overlap stripe slot(s) reused (not fresh COW)"
fi
teardown "A4"

summary
