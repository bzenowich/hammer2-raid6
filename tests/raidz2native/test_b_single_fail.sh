#!/bin/sh
# Group B: Single disk failure — verify degraded reads for each disk position
# and degraded writes.
# Tests: B1-BN (fail each disk), B_last (degraded write + dual reconstruction)

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group B: Single Disk Failure (NDISKS=$NDISKS) ==="

# B1..BN: Fail each disk in turn
DISK=0
while [ "$DISK" -lt "$NDISKS" ]; do
    setup_fresh
    check_v4
    write_ref_data "ref"

    hammer2 -s $MNTPT raid fail-disk "$(disk_dev $DISK)" > /dev/null 2>&1
    detach_disk "$DISK"

    verify_ref "B$((DISK+1)): single-fail disk${DISK}" "ref"
    teardown "B$((DISK+1))"
    DISK=$((DISK + 1))
done

# B_last: Degraded write after single failure, then verify through dual reconstruction
setup_fresh
check_v4
write_ref_data "ref"

FAIL1=2
FAIL2=$((NDISKS - 1))
hammer2 -s $MNTPT raid fail-disk "$(disk_dev $FAIL1)" > /dev/null 2>&1
detach_disk "$FAIL1"

# Write new data while degraded (1 disk down)
dd if=/dev/urandom of=$MNTPT/dw_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/dw_b bs=65536 count=64 2>/dev/null
sha256 $MNTPT/dw_a > /var/tmp/blast_new.txt
sha256 $MNTPT/dw_b >> /var/tmp/blast_new.txt
sync; sync

# Fail a second disk to force dual-degraded reconstruction on reads
hammer2 -s $MNTPT raid fail-disk "$(disk_dev $FAIL2)" > /dev/null 2>&1
detach_disk "$FAIL2"

BLABEL="B$((NDISKS+1))"
verify_ref "$BLABEL: pre-fail data through dual reconstruction" "ref"
sha256 $MNTPT/dw_a > /var/tmp/blast_check.txt 2>&1
sha256 $MNTPT/dw_b >> /var/tmp/blast_check.txt 2>&1
if diff -q /var/tmp/blast_new.txt /var/tmp/blast_check.txt > /dev/null 2>&1; then
    result PASS "$BLABEL: degraded-written data correct through dual reconstruction"
else
    result FAIL "$BLABEL: degraded-written data mismatch"
fi
teardown "$BLABEL"

summary
