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
data_slots > /var/tmp/f1_v1.txt

dd if=/dev/urandom of=$MNTPT/cow1 bs=65536 count=16 2>/dev/null
sync; sync
data_slots > /var/tmp/f1_v2.txt

dd if=/dev/urandom of=$MNTPT/cow1 bs=65536 count=16 2>/dev/null
sync; sync
data_slots > /var/tmp/f1_v3.txt

# v1 and v2 must not share any addresses
overlap12=$(comm -12 /var/tmp/f1_v1.txt /var/tmp/f1_v2.txt | wc -l | tr -d ' ')
# v2 and v3 must not share any addresses
overlap23=$(comm -12 /var/tmp/f1_v2.txt /var/tmp/f1_v3.txt | wc -l | tr -d ' ')

n1=$(wc -l < /var/tmp/f1_v1.txt | tr -d ' ')
n2=$(wc -l < /var/tmp/f1_v2.txt | tr -d ' ')
n3=$(wc -l < /var/tmp/f1_v3.txt | tr -d ' ')

if [ "$n1" -lt 16 ] || [ "$n2" -lt 16 ] || [ "$n3" -lt 16 ]; then
    result FAIL "F1: hammer2 show found $n1/$n2/$n3 DATA blocks, expected 16 each"
elif [ "$overlap12" = "0" ]; then
    result PASS "F1: write 1→2: no stripe slot reuse"
else
    result FAIL "F1: write 1→2: $overlap12 slot(s) reused"
fi
if [ "$n1" -lt 16 ] || [ "$n2" -lt 16 ] || [ "$n3" -lt 16 ]; then
    : # reported above
elif [ "$overlap23" = "0" ]; then
    result PASS "F1: write 2→3: no stripe slot reuse"
else
    result FAIL "F1: write 2→3: $overlap23 slot(s) reused"
fi
teardown "F1"

# F2: h2stripe_check parity verification
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
        checked=$(awk -F: '/Allocated slots checked/ { print $2 + 0 }' \
            /var/tmp/f2_check.txt)
        if [ "${checked:-0}" -gt 0 ]; then
            result PASS "F2: h2stripe_check: P/Q parity correct in $checked stripe slots"
        else
            result FAIL "F2: h2stripe_check checked no stripe slots"
            cat /var/tmp/f2_check.txt
        fi
    else
        result FAIL "F2: h2stripe_check: parity errors detected"
        cat /var/tmp/f2_check.txt
    fi

    # Remount for teardown
    mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null || true
    teardown "F2"
else
    # A missing checker must not read as verified parity.
    result FAIL "F2: h2stripe_check not installed (build src/diag/h2stripe_check.c)"
fi

# F3: Snapshot COW — after a snapshot, overwriting the live file must
# leave the snapshot's copy intact (read back through its own mount).
F3_SNAP=F3SNAP
F3_MNT=/mnt/v3snap
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/snap_test bs=65536 count=32 2>/dev/null
sha256 -q $MNTPT/snap_test > /var/tmp/f3_snap.txt
sync; sync

if ! hammer2 -s $MNTPT snapshot $MNTPT $F3_SNAP > /var/tmp/f3_snap.out 2>&1; then
    result FAIL "F3: snapshot creation failed"
    cat /var/tmp/f3_snap.out
else
    # Overwrite the original
    dd if=/dev/urandom of=$MNTPT/snap_test bs=65536 count=32 2>/dev/null
    sha256 -q $MNTPT/snap_test > /var/tmp/f3_new.txt
    sync; sync

    mkdir -p $F3_MNT
    if ! mount -t hammer2 "${DEVSPEC}@${F3_SNAP}" $F3_MNT; then
        result FAIL "F3: failed to mount snapshot $F3_SNAP"
    else
        sha256 -q $F3_MNT/snap_test > /var/tmp/f3_old.txt 2>/dev/null
        umount $F3_MNT
        if ! diff -q /var/tmp/f3_snap.txt /var/tmp/f3_old.txt > /dev/null 2>&1; then
            result FAIL "F3: snapshot content changed by live overwrite"
        elif diff -q /var/tmp/f3_snap.txt /var/tmp/f3_new.txt > /dev/null 2>&1; then
            result FAIL "F3: live file unchanged after overwrite"
        else
            result PASS "F3: snapshot keeps pre-overwrite data, live file has new data"
        fi
    fi
fi
teardown "F3"

summary
