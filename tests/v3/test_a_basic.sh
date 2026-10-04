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

# A3: Blockref encoding — every DATA blockref names a disk in
# [0..NDISKS-1] (bref.copyid), sits at a 64 KB-aligned per-disk offset,
# and no two blockrefs share a (disk, offset) slot.
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/probe bs=65536 count=4 2>/dev/null
sync; sync
hammer2 -q show $DEVSPEC > /var/tmp/a3_show.txt 2>&1
awk '$1 ~ /^data\./ && $4 ~ /^vol=/ { sub(/^vol=/, "", $4); print $4, $2 }' \
    /var/tmp/a3_show.txt > /var/tmp/a3_data.txt
ndata=$(wc -l < /var/tmp/a3_data.txt | tr -d ' ')
bad=""
while read disk off; do
    if [ "$disk" -lt 0 ] || [ "$disk" -ge "$NDISKS" ]; then
        bad="$bad disk=$disk"
    fi
    # low 16 bits of data_off: offset bits must be 0, only the radix set
    low=$((0x$(echo "$off" | cut -c13-16)))
    if [ $((low & 0xffc0)) -ne 0 ]; then
        bad="$bad off=$off"
    fi
done < /var/tmp/a3_data.txt
dups=$(sort /var/tmp/a3_data.txt | uniq -d | wc -l | tr -d ' ')
if [ "$ndata" -lt 4 ]; then
    result FAIL "A3: hammer2 show found $ndata DATA blockrefs, expected 4"
    head -5 /var/tmp/a3_show.txt
elif [ -n "$bad" ]; then
    result FAIL "A3: bad DATA blockref encoding:$bad"
elif [ "$dups" != "0" ]; then
    result FAIL "A3: $dups DATA slot(s) referenced twice"
else
    result PASS "A3: $ndata DATA blockrefs: disk in [0...$((NDISKS-1))], 64 KB-aligned, distinct slots"
fi
teardown "A3"

# A4: COW — overwrite allocates new stripe slot (no address reuse)
setup_fresh
check_v3
# Write a file large enough to span multiple DIOs (8 x 64KB = 512 KB)
dd if=/dev/urandom of=$MNTPT/cow_test bs=65536 count=8 2>/dev/null
sync; sync
data_slots > /var/tmp/a4_before.txt
# Overwrite
dd if=/dev/urandom of=$MNTPT/cow_test bs=65536 count=8 2>/dev/null
sync; sync
data_slots > /var/tmp/a4_after.txt
nbefore=$(wc -l < /var/tmp/a4_before.txt | tr -d ' ')
nafter=$(wc -l < /var/tmp/a4_after.txt | tr -d ' ')
overlap=$(comm -12 /var/tmp/a4_before.txt /var/tmp/a4_after.txt | wc -l | tr -d ' ')
if [ "$nbefore" -lt 8 ] || [ "$nafter" -lt 8 ]; then
    result FAIL "A4: hammer2 show found $nbefore/$nafter DATA blocks, expected 8 each"
elif [ "$overlap" = "0" ]; then
    result PASS "A4: COW — 8 blocks rewritten into fresh stripe slots"
else
    result FAIL "A4: COW — $overlap stripe slot(s) reused (not fresh COW)"
fi
teardown "A4"

summary
