#!/bin/sh
# Group B: Single disk failure — verify degraded reads for each disk position
# and degraded writes.
# Tests: B1-B6 (fail each disk), B7 (degraded write + dual reconstruction verify)

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group B: Single Disk Failure ==="

# B1-B6: Fail each disk in turn
for DISK in 0 1 2 3 4 5; do
    setup_fresh
    check_v4
    write_ref_data "ref"

    hammer2 -s $MNTPT raid fail-disk /dev/vn${DISK} > /dev/null 2>&1
    vnconfig -u vn${DISK} 2>/dev/null || true

    verify_ref "B$((DISK+1)): single-fail vn${DISK}" "ref"
    teardown "B$((DISK+1))"
done

# B7: Degraded write after single failure, then verify through dual reconstruction
setup_fresh
check_v4
write_ref_data "ref"

hammer2 -s $MNTPT raid fail-disk /dev/vn2 > /dev/null 2>&1
vnconfig -u vn2 2>/dev/null || true

# Write new data while degraded (1 disk down)
dd if=/dev/urandom of=$MNTPT/dw_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/dw_b bs=65536 count=64 2>/dev/null
sha256 $MNTPT/dw_a > /var/tmp/b7_new.txt
sha256 $MNTPT/dw_b >> /var/tmp/b7_new.txt
sync; sync

# Fail a second disk to force dual-degraded reconstruction on reads
hammer2 -s $MNTPT raid fail-disk /dev/vn5 > /dev/null 2>&1
vnconfig -u vn5 2>/dev/null || true

verify_ref "B7: pre-fail data through dual reconstruction" "ref"
sha256 $MNTPT/dw_a > /var/tmp/b7_check.txt 2>&1
sha256 $MNTPT/dw_b >> /var/tmp/b7_check.txt 2>&1
if diff -q /var/tmp/b7_new.txt /var/tmp/b7_check.txt > /dev/null 2>&1; then
    result PASS "B7: degraded-written data correct through dual reconstruction"
else
    result FAIL "B7: degraded-written data mismatch"
fi
teardown "B7"

summary
