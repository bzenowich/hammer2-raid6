#!/bin/sh
# Group L: Read-path self-heal (bitrot.md §7.4 bullet 1 — ZFS behavior).
#
# L1 — corrupt one disk's stripe-data region while unmounted, remount,
#      READ the file (no scrub): content must match the pre-corruption
#      reference (in-memory heal + parity reconstruction on the read
#      path), and `raid status` must show cksum_err > 0 with healed > 0
#      on the corrupted disk only.
# L2 — after the deferred repair writes land, a scrub must be clean
#      (proves the read path repaired the media, not just the buffer
#      cache).
# L3 — metadata self-heal: corrupt a slice of the metadata zone on one
#      disk, remount, walk the tree (find + stat).  All files must be
#      reachable (mirror-copy failover) and no CHECK errors may leak to
#      userspace.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group L: Read-path self-heal (NDISKS=$NDISKS) ==="

scrub_field() {
    local field="$1"
    local out="$2"
    awk -v f="$field" '$1 == f":" { print $2 }' < "$out"
}

# status_cksum <disk_idx> <field-col>: parse `raid status` per-disk line:
#   disk[N]:  ONLINE  cksum_err E  healed H  unrepairable U
status_cksum() {
    local idx="$1"
    local field="$2"
    grep "^disk\[$idx\]:" /var/tmp/l_status.txt | \
        awk -v f="$field" \
        '{ for (i = 1; i < NF; i++) if ($i == f) { print $(i+1); exit } }'
}

# L1/L2: data-column self-heal on the read path.
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/l1 bs=65536 count=512 2>/dev/null
sha256 $MNTPT/l1 > /var/tmp/l1_ref.txt
sync; sync
umount $MNTPT

# Corrupt 16 MB of stripe data on logical disk 1 at the start of the
# stripe data zone, same profile as K2.
CORRUPT_DEV=$(disk_dev 1)
dd if=/dev/urandom of="$CORRUPT_DEV" bs=65536 count=256 seek=$DATA_BLK \
    conv=notrunc 2>/dev/null

mount -t hammer2 $PFSPATH $MNTPT
kmsg_clear

# THE test: read the file with NO scrub.  Self-heal must reconstruct
# every corrupt column inline.
sha256 $MNTPT/l1 > /var/tmp/l1_check.txt 2>&1
if diff -q /var/tmp/l1_ref.txt /var/tmp/l1_check.txt > /dev/null 2>&1; then
    result PASS "L1: read-path returned correct content over corrupt disk"
else
    result FAIL "L1: content mismatch on read (self-heal failed)"
fi

if kmsg | grep -q 'selfheal: read-path repair'; then
    result PASS "L1: dmesg shows read-path self-heal activity"
else
    result FAIL "L1: no selfheal messages in dmesg"
    kmsg | tail -20
fi

hammer2 -s $MNTPT raid status > /var/tmp/l_status.txt 2>&1
L1_ERR1=$(status_cksum 1 cksum_err)
L1_HEAL1=$(status_cksum 1 healed)
L1_ERR0=$(status_cksum 0 cksum_err)
if [ "${L1_ERR1:-0}" -gt 0 ] && [ "${L1_HEAL1:-0}" -gt 0 ]; then
    result PASS "L1: raid status counters (disk1 err=$L1_ERR1 healed=$L1_HEAL1)"
else
    result FAIL "L1: raid status counters missing (err=$L1_ERR1 healed=$L1_HEAL1)"
    cat /var/tmp/l_status.txt
fi
if [ "${L1_ERR0:-x}" = "0" ]; then
    result PASS "L1: healthy disk 0 shows zero cksum errors"
else
    result FAIL "L1: disk 0 unexpectedly shows cksum_err=$L1_ERR0"
fi

# L2: give the deferred repair writes a moment to land, then scrub.
# The scrub must find nothing left to repair on the blocks the read
# path already healed — and it heals the rest of the corrupted region
# (blocks of l1 we read cover everything, so expect clean-or-repairs,
# but a SECOND scrub right after must be fully clean).
sleep 3
sync
hammer2 -s $MNTPT raid scrub > /var/tmp/l2_scrub1.txt 2>&1
hammer2 -s $MNTPT raid scrub > /var/tmp/l2_scrub2.txt 2>&1
L2_RC=$?
L2_BAD=$(scrub_field brefs_bad /var/tmp/l2_scrub2.txt)
L2_UNREP=$(scrub_field brefs_unrepairable /var/tmp/l2_scrub2.txt)
if [ "$L2_RC" = "0" ] && [ "${L2_BAD:-x}" = "0" ] && \
   [ "${L2_UNREP:-x}" = "0" ]; then
    result PASS "L2: follow-up scrub clean (on-disk repair landed)"
else
    result FAIL "L2: follow-up scrub bad=$L2_BAD unrep=$L2_UNREP rc=$L2_RC"
    cat /var/tmp/l2_scrub2.txt
fi
kmsg_clear
teardown "L1"

# L3: metadata self-heal via mirror copies.  Create a wide directory
# tree (lots of INODE/INDIRECT/DIRENT metadata), corrupt part of the
# metadata zone on disk 2, remount, and walk everything.
setup_fresh
check_v3
mkdir -p $MNTPT/l3
i=0
while [ $i -lt 200 ]; do
    mkdir -p $MNTPT/l3/dir$i
    echo "content-$i" > $MNTPT/l3/dir$i/file
    i=$((i + 1))
done
sync; sync
FCOUNT_REF=$(find $MNTPT/l3 | wc -l | tr -d ' ')
umount $MNTPT

# Metadata brefs carry copyid 0: the primary copy is always read from
# disk 0, and disks 1..N-1 hold the mirror copies.  To exercise the
# mirror-failover heal the corruption must land on DISK 0's metadata
# zone (extent 0, MD_BLK..DATA_BLK from common.sh; 20..220 MB on the
# 4 GB test disks).  Skip the first 1 MB of the extent, where mkfs put
# the super-root; corrupt the rest of it.
MD_DEV=$(disk_dev 0)
dd if=/dev/urandom of="$MD_DEV" bs=65536 count=$((DATA_BLK - MD_BLK - 16)) \
    seek=$((MD_BLK + 16)) \
    conv=notrunc 2>/dev/null

mount -t hammer2 $PFSPATH $MNTPT
kmsg_clear

FCOUNT=$(find $MNTPT/l3 2>/var/tmp/l3_find_err.txt | wc -l | tr -d ' ')
CATFAIL=0
i=0
while [ $i -lt 200 ]; do
    C=$(cat $MNTPT/l3/dir$i/file 2>/dev/null)
    [ "$C" = "content-$i" ] || CATFAIL=$((CATFAIL + 1))
    i=$((i + 20))
done

if [ "$FCOUNT" = "$FCOUNT_REF" ] && [ "$CATFAIL" = "0" ] && \
   [ ! -s /var/tmp/l3_find_err.txt ]; then
    result PASS "L3: full tree reachable over corrupt metadata disk ($FCOUNT nodes)"
else
    result FAIL "L3: tree walk degraded (count=$FCOUNT/$FCOUNT_REF catfail=$CATFAIL)"
    cat /var/tmp/l3_find_err.txt
fi

# Accept either the (rate-limited) read-path heal line or the repair
# kthread's write line — in L3 only disk 0 metadata was corrupted, so
# any repair write to disk 0 proves the mirror heal fired.
if kmsg | grep -q 'selfheal: read-path repair.*metadata\|selfheal: repair write disk 0'; then
    result PASS "L3: dmesg shows metadata mirror self-heal"
else
    # The walk must have hit at least some of the 4 MB corrupt window
    # on the primary metadata disk; silence here means the heal path
    # did not fire.
    result FAIL "L3: no metadata selfheal lines in dmesg"
    kmsg | tail -15
fi
kmsg_clear
teardown "L3"

summary
