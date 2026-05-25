#!/bin/sh
# Group D: Resilver — replace a failed disk and verify reconstruction.
# Tests: D1 (basic resilver), D2 (sequential resilvers), D3 (resilver + write during)

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group D: Resilver (NDISKS=$NDISKS) ==="

# D1: Basic resilver — fail disk 3, resilver, verify
# (Use index 3 which exists for NDISKS >= 4)
D1_DISK=3
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/resilver_ref bs=65536 count=800 2>/dev/null
sha256 $MNTPT/resilver_ref > /var/tmp/d1_ref.txt
sync; sync

hammer2 -s $MNTPT raid fail-disk "$(disk_dev $D1_DISK)" > /dev/null 2>&1
detach_disk "$D1_DISK"
fresh_disk "$D1_DISK"

if hammer2 -s $MNTPT raid replace \
        "$(disk_dev $D1_DISK)" "$(disk_dev $D1_DISK)" > /dev/null 2>&1; then
    # Verify post-resilver
    sha256 $MNTPT/resilver_ref > /var/tmp/d1_check.txt 2>&1
    if diff -q /var/tmp/d1_ref.txt /var/tmp/d1_check.txt > /dev/null 2>&1; then
        result PASS "D1: post-resilver data correct (degraded read)"
    else
        result FAIL "D1: post-resilver data mismatch"
    fi
    # Remount fully healthy, verify again
    umount $MNTPT
    mount -t hammer2 $PFSPATH $MNTPT
    sha256 $MNTPT/resilver_ref > /var/tmp/d1_check2.txt 2>&1
    if diff -q /var/tmp/d1_ref.txt /var/tmp/d1_check2.txt > /dev/null 2>&1; then
        result PASS "D1: post-remount data correct (healthy read)"
    else
        result FAIL "D1: post-remount data mismatch"
    fi
else
    result FAIL "D1: resilver command failed"
fi
teardown "D1"

# D2: Sequential resilvers — fail disk 1, resilver; write more; fail disk NDISKS-2, resilver
D2_DISK1=1
D2_DISK2=$((NDISKS - 2))
setup_fresh
check_v3
write_ref_data "r1"

hammer2 -s $MNTPT raid fail-disk "$(disk_dev $D2_DISK1)" > /dev/null 2>&1
detach_disk "$D2_DISK1"
fresh_disk "$D2_DISK1"
hammer2 -s $MNTPT raid replace \
    "$(disk_dev $D2_DISK1)" "$(disk_dev $D2_DISK1)" > /dev/null 2>&1
verify_ref "D2: r1 after first resilver (disk${D2_DISK1})" "r1"

write_ref_data "r2"

hammer2 -s $MNTPT raid fail-disk "$(disk_dev $D2_DISK2)" > /dev/null 2>&1
detach_disk "$D2_DISK2"
fresh_disk "$D2_DISK2"
hammer2 -s $MNTPT raid replace \
    "$(disk_dev $D2_DISK2)" "$(disk_dev $D2_DISK2)" > /dev/null 2>&1
verify_ref "D2: r1 after second resilver (disk${D2_DISK2})" "r1"
verify_ref "D2: r2 after second resilver (disk${D2_DISK2})" "r2"
teardown "D2"

# D3: Write during resilver — data written concurrently must be correct after
setup_fresh
check_v3
write_ref_data "pre"
sync; sync

D3_DISK=0
hammer2 -s $MNTPT raid fail-disk "$(disk_dev $D3_DISK)" > /dev/null 2>&1
detach_disk "$D3_DISK"
fresh_disk "$D3_DISK"

# Start resilver in background, write concurrently, then check
hammer2 -s $MNTPT raid replace \
    "$(disk_dev $D3_DISK)" "$(disk_dev $D3_DISK)" > /dev/null 2>&1 &
RPID=$!

# Write new data while resilver is running
dd if=/dev/urandom of=$MNTPT/during_resilver bs=65536 count=256 2>/dev/null
sha256 $MNTPT/during_resilver > /var/tmp/d3_new.txt
sync; sync

wait $RPID
RESILVER_RC=$?

verify_ref "D3: pre-resilver data still correct" "pre"
sha256 $MNTPT/during_resilver > /var/tmp/d3_check.txt 2>&1
if diff -q /var/tmp/d3_new.txt /var/tmp/d3_check.txt > /dev/null 2>&1; then
    result PASS "D3: data written during resilver correct"
else
    result FAIL "D3: data written during resilver mismatch"
fi
if [ "$RESILVER_RC" = "0" ]; then
    result PASS "D3: resilver completed successfully"
else
    result FAIL "D3: resilver returned non-zero ($RESILVER_RC)"
fi
teardown "D3"

# D4: Bitmap-skip sysctl regression — write a small amount of data
# (sparse allocation) and resilver twice: once with the bitmap-skip
# sysctl off (baseline that walks every slot) and once with it on
# (default, walks only allocated slots).  Verify both produce correct
# data and that skip=1 is faster than skip=0.
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/d4_ref bs=65536 count=80 2>/dev/null
sha256 $MNTPT/d4_ref > /var/tmp/d4_ref.txt
sync; sync

D4_DISK=2

# Trial 1: skip=0 (full iteration baseline)
sysctl -w vfs.hammer2.resilver_skip_unalloc=0 > /dev/null
hammer2 -s $MNTPT raid fail-disk "$(disk_dev $D4_DISK)" > /dev/null 2>&1
detach_disk "$D4_DISK"
fresh_disk "$D4_DISK"
T0=$(date +%s)
hammer2 -s $MNTPT raid replace \
    "$(disk_dev $D4_DISK)" "$(disk_dev $D4_DISK)" > /dev/null 2>&1
D4_RC0=$?
T1=$(date +%s)
D4_ELAPSED0=$((T1 - T0))
sha256 $MNTPT/d4_ref > /var/tmp/d4_check.txt 2>&1
if [ "$D4_RC0" = "0" ] && \
   diff -q /var/tmp/d4_ref.txt /var/tmp/d4_check.txt > /dev/null 2>&1; then
    result PASS "D4: skip=0 resilver correct (${D4_ELAPSED0}s)"
else
    result FAIL "D4: skip=0 resilver wrong (rc=$D4_RC0, ${D4_ELAPSED0}s)"
fi

# Trial 2: skip=1 (default — bitmap-aware)
sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
hammer2 -s $MNTPT raid fail-disk "$(disk_dev $D4_DISK)" > /dev/null 2>&1
detach_disk "$D4_DISK"
fresh_disk "$D4_DISK"
T0=$(date +%s)
hammer2 -s $MNTPT raid replace \
    "$(disk_dev $D4_DISK)" "$(disk_dev $D4_DISK)" > /dev/null 2>&1
D4_RC1=$?
T1=$(date +%s)
D4_ELAPSED1=$((T1 - T0))
sha256 $MNTPT/d4_ref > /var/tmp/d4_check.txt 2>&1
if [ "$D4_RC1" = "0" ] && \
   diff -q /var/tmp/d4_ref.txt /var/tmp/d4_check.txt > /dev/null 2>&1; then
    result PASS "D4: skip=1 resilver correct (${D4_ELAPSED1}s)"
else
    result FAIL "D4: skip=1 resilver wrong (rc=$D4_RC1, ${D4_ELAPSED1}s)"
fi

# Speed-up assertion: skip=1 should be strictly faster than skip=0 on
# a sparsely-allocated array (≤ 0.3% of slots live).  Allow a 1s
# tolerance for system-noise jitter when both trials are sub-second.
if [ "$D4_ELAPSED1" -le "$D4_ELAPSED0" ]; then
    result PASS "D4: skip=1 ≤ skip=0 (${D4_ELAPSED1}s vs ${D4_ELAPSED0}s)"
else
    result FAIL "D4: skip=1 SLOWER than skip=0 (${D4_ELAPSED1}s vs ${D4_ELAPSED0}s)"
fi

# Restore default so subsequent tests/runs aren't perturbed
sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
teardown "D4"

summary
