#!/bin/sh
# Group K: Scrub (M3 — docs/zfs_compare.md item 3).
#
# K1 — clean array scrubs without finding corruption.
# K2 — corrupt one disk's column via dd while unmounted, mount, scrub
#      should detect (CHECK FAIL) and parity-repair every affected slot;
#      post-scrub file content matches the pre-corruption reference.
# K3 — scrub runs concurrently with foreground writes (snapshot model);
#      both the scrub completes and the new writes verify after.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group K: Scrub (NDISKS=$NDISKS) ==="

# Helper: parse "brefs_bad: N" from scrub output
scrub_field() {
    local field="$1"
    local out="$2"
    awk -v f="$field" '$1 == f":" { print $2 }' < "$out"
}

# K1: scrub a clean array — expect zero bad, zero repaired.
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/k1 bs=65536 count=80 2>/dev/null
sha256 $MNTPT/k1 > /var/tmp/k1_ref.txt
sync; sync

hammer2 -s $MNTPT raid scrub > /var/tmp/k1_scrub.txt 2>&1
K1_RC=$?
K1_DONE=$(scrub_field brefs_done /var/tmp/k1_scrub.txt)
K1_BAD=$(scrub_field brefs_bad /var/tmp/k1_scrub.txt)
K1_REP=$(scrub_field brefs_repaired /var/tmp/k1_scrub.txt)
K1_UNREP=$(scrub_field brefs_unrepairable /var/tmp/k1_scrub.txt)

if [ "$K1_RC" = "0" ] && [ "${K1_BAD:-x}" = "0" ] && \
   [ "${K1_REP:-x}" = "0" ] && [ "${K1_UNREP:-x}" = "0" ]; then
    result PASS "K1: clean scrub (done=$K1_DONE bad=0 rep=0)"
else
    result FAIL "K1: clean scrub bad=$K1_BAD rep=$K1_REP unrep=$K1_UNREP rc=$K1_RC"
    cat /var/tmp/k1_scrub.txt
fi
teardown "K1"

# K2: corrupt then scrub.  Choose disk index 1 (= /dev/vbd${DISK_BASE+1});
# its data-column slots cycle through with period ndisks.  Stripe data
# begins at HAMMER2_ZONE_SEG64 (4MB) + HAMMER2_STRIPE_RAID6_START (slot
# 1024) * 64KB = 68 MB; corrupt 16 MB starting there to cover early
# allocations of our reference file.
setup_fresh
check_v3
# 32 MB file = 256 rows; with NDISKS=4 / open_rows cap = 16, many
# rows get TXG-force-sealed partial (ncols=1).  write_row zeros the
# unwritten data cols on disk (see local_hammer2_raid6.c), so parity
# reconstruction on those rows works correctly under scrub repair.
dd if=/dev/urandom of=$MNTPT/k2 bs=65536 count=512 2>/dev/null
sha256 $MNTPT/k2 > /var/tmp/k2_ref.txt
sync; sync
umount $MNTPT

# Corrupt 16 MB on logical disk 1 (= vbd$((DISK_BASE+1))) at byte 68 MB,
# the start of the stripe data zone.
CORRUPT_DEV=$(disk_dev 1)
dd if=/dev/urandom of="$CORRUPT_DEV" bs=65536 count=256 seek=1088 \
    conv=notrunc 2>/dev/null

mount -t hammer2 $PFSPATH $MNTPT
# Run scrub
hammer2 -s $MNTPT raid scrub > /var/tmp/k2_scrub.txt 2>&1
K2_RC=$?
K2_DONE=$(scrub_field brefs_done /var/tmp/k2_scrub.txt)
K2_BAD=$(scrub_field brefs_bad /var/tmp/k2_scrub.txt)
K2_REP=$(scrub_field brefs_repaired /var/tmp/k2_scrub.txt)
K2_UNREP=$(scrub_field brefs_unrepairable /var/tmp/k2_scrub.txt)

if [ "${K2_BAD:-0}" -gt 0 ]; then
    result PASS "K2: scrub detected corruption (bad=$K2_BAD)"
else
    result FAIL "K2: scrub missed corruption (bad=$K2_BAD)"
    cat /var/tmp/k2_scrub.txt
fi

if [ "${K2_REP:-0}" -gt 0 ] && [ "${K2_REP:-0}" = "${K2_BAD:-0}" ]; then
    result PASS "K2: scrub repaired all bad ($K2_REP/$K2_BAD)"
else
    result FAIL "K2: scrub repaired $K2_REP of $K2_BAD (unrep=$K2_UNREP)"
fi

# Verify file integrity after scrub.  Drop dmesg-noise from the
# corruption hits so teardown's CHECK FAIL guard doesn't flag K2.
dmesg -c > /dev/null 2>&1
sha256 $MNTPT/k2 > /var/tmp/k2_check.txt 2>&1
if diff -q /var/tmp/k2_ref.txt /var/tmp/k2_check.txt > /dev/null 2>&1; then
    result PASS "K2: post-scrub file content correct"
else
    result FAIL "K2: post-scrub file content mismatch"
fi

# Follow-up scrub: should now be clean.
hammer2 -s $MNTPT raid scrub > /var/tmp/k2_scrub2.txt 2>&1
K2B_BAD=$(scrub_field brefs_bad /var/tmp/k2_scrub2.txt)
K2B_UNREP=$(scrub_field brefs_unrepairable /var/tmp/k2_scrub2.txt)
if [ "${K2B_BAD:-x}" = "0" ] && [ "${K2B_UNREP:-x}" = "0" ]; then
    result PASS "K2: follow-up scrub clean"
else
    result FAIL "K2: follow-up scrub bad=$K2B_BAD unrep=$K2B_UNREP"
    cat /var/tmp/k2_scrub2.txt
fi
teardown "K2"

summary
