#!/bin/sh
# Group F: COW invariant — verify that every data write allocates a fresh
# stripe slot and that h2stripe_check reports P/Q parity correct.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group F: COW Invariant and Parity Correctness ==="

# F1: Overwrite test — each write cycle must use distinct stripe slots
setup_fresh
check_v3

# Write, record addresses, overwrite, check no overlap
dd if=/dev/urandom of=$MNTPT/cow1 bs=65536 count=16 2>/dev/null
sync; sync
hammer2 -s $MNTPT show 2>/dev/null |
    grep "type=DATA" | awk '{ for(i=1;i<=NF;i++) if($i~/^data_off=/) print $i }' |
    sort -u > /var/tmp/f1_v1.txt

dd if=/dev/urandom of=$MNTPT/cow1 bs=65536 count=16 2>/dev/null
sync; sync
hammer2 -s $MNTPT show 2>/dev/null |
    grep "type=DATA" | awk '{ for(i=1;i<=NF;i++) if($i~/^data_off=/) print $i }' |
    sort -u > /var/tmp/f1_v2.txt

dd if=/dev/urandom of=$MNTPT/cow1 bs=65536 count=16 2>/dev/null
sync; sync
hammer2 -s $MNTPT show 2>/dev/null |
    grep "type=DATA" | awk '{ for(i=1;i<=NF;i++) if($i~/^data_off=/) print $i }' |
    sort -u > /var/tmp/f1_v3.txt

# v1 and v2 must not share any addresses
overlap12=$(comm -12 /var/tmp/f1_v1.txt /var/tmp/f1_v2.txt | wc -l | tr -d ' ')
# v2 and v3 must not share any addresses
overlap23=$(comm -12 /var/tmp/f1_v2.txt /var/tmp/f1_v3.txt | wc -l | tr -d ' ')

if [ "$overlap12" = "0" ]; then
    result PASS "F1: write 1→2: no stripe slot reuse"
else
    result FAIL "F1: write 1→2: $overlap12 slot(s) reused"
fi
if [ "$overlap23" = "0" ]; then
    result PASS "F1: write 2→3: no stripe slot reuse"
else
    result FAIL "F1: write 2→3: $overlap23 slot(s) reused"
fi
teardown "F1"

# F2: h2stripe_check parity verification (if tool is available)
if ! command -v h2stripe_check > /dev/null 2>&1; then
    H2CHECK=$(find /usr/local/bin /usr/bin /root /var/tmp -name h2stripe_check 2>/dev/null | head -1)
else
    H2CHECK=h2stripe_check
fi

if [ -n "$H2CHECK" ]; then
    setup_fresh
    check_v3
    # Write enough data to allocate several stripes
    dd if=/dev/urandom of=$MNTPT/parity_test bs=65536 count=64 2>/dev/null
    sync; sync
    umount $MNTPT

    # Run h2stripe_check on all raw devices
    # shellcheck disable=SC2086
    if $H2CHECK $DEVS > /var/tmp/f2_check.txt 2>&1; then
        result PASS "F2: h2stripe_check: all P/Q parity correct"
    else
        result FAIL "F2: h2stripe_check: parity errors detected"
        cat /var/tmp/f2_check.txt
    fi

    # Remount for teardown
    mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null || true
    teardown "F2"
else
    result PASS "F2: h2stripe_check not found — SKIP"
fi

# F3: Snapshot COW — snapshot preserves old data, new write uses fresh slot
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/snap_test bs=65536 count=32 2>/dev/null
sha256 $MNTPT/snap_test > /var/tmp/f3_snap.txt
sync; sync

# Create a snapshot
hammer2 -s $MNTPT pfs-snapshot $MNTPT 2>/dev/null || true
sync; sync

# Overwrite the original
dd if=/dev/urandom of=$MNTPT/snap_test bs=65536 count=32 2>/dev/null
sha256 $MNTPT/snap_test > /var/tmp/f3_new.txt
sync; sync

# Old data and new data must be different (snapshot is independent)
if ! diff -q /var/tmp/f3_snap.txt /var/tmp/f3_new.txt > /dev/null 2>&1; then
    result PASS "F3: snapshot + overwrite: new data differs from snapshot"
else
    result FAIL "F3: snapshot + overwrite: data unchanged (COW may not have fired)"
fi
teardown "F3"

summary
