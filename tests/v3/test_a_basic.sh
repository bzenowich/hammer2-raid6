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

# A5: newfs onto a smaller size must not leave the old array's volume
# headers in the zones beyond the new size.  Volume headers sit every 2 GB;
# newfs -s 2g writes only zone 0, and a header left at 2 GB carries a
# higher txg_seq than the new one, so mount adopts it (and then refuses
# the mount; the refusal also leaves the disks open).  Known bug: this
# FAILs until newfs clears them.  Checked on the media, nothing is mounted.
setup_fresh full
check_v3
sync
umount $MNTPT 2>/dev/null
d0=$(disk_dev 0)
zone1_before=$(dd if=$d0 bs=65536 skip=32768 count=1 2>/dev/null | sha256 -q)
zero64=$(dd if=/dev/zero bs=65536 count=1 2>/dev/null | sha256 -q)
if [ "$zone1_before" = "$zero64" ]; then
    result PASS "A5: (no second header zone on this disk, nothing to check)"
else
    newfs_hammer2 -R 6 -L V3TEST $(fresh_size_opt 2g) $DEVS > /var/tmp/a5_newfs.txt 2>&1
    zone1_after=$(dd if=$d0 bs=65536 skip=32768 count=1 2>/dev/null | sha256 -q)
    if [ "$zone1_after" = "$zone1_before" ]; then
        result FAIL "A5: newfs -s 2g left the old array's header at 2 GB (mount would adopt it)"
    else
        result PASS "A5: newfs -s 2g cleared the old header at 2 GB"
    fi
fi
setup_fresh full
check_v3
teardown "A5"

summary
