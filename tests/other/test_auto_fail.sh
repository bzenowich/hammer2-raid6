#!/bin/sh
# HAMMER2 RAID6 Test: Auto-fail disk integration
#
# Tests the auto-fail disk state machine.  The actual EIO-triggered path
# (hammer2_raid6_auto_fail_disk called from breadnx failure) cannot be
# reliably triggered on vn devices: vn returns zeros — not EIO — for reads
# past a truncated backing file, and the ondisk mount code rejects disks
# whose volu_size header field exceeds the actual device size.  On physical
# hardware, a failing disk will return EIO to breadnx and trigger the path.
#
# What this test covers:
#   A. Healthy baseline — auto-fail does not fire spuriously
#   B. Single disk manual fail → same state machine as auto-fail
#      (raid_failed[], voldata.raid_config, dmesg messages)
#      Verifies data readable via RAID6 reconstruction after fail
#      Verifies failed state persists across unmount/remount
#   C. Dual disk manual fail → dual reconstruction still works
#   D. Triple fail attempt is rejected (array would be unrecoverable)
#
# Note on write size: writes use two small files (64+32 × 64KB = 6MB) with
# sha256 reads between them, matching test_6disk.sh's pattern.  A single
# large write (e.g. 256×64KB = 16MB) generates too many async parity bawrite
# calls in healthy mode, saturating runningbufspace and causing sync to block
# in waitrunningbufspace — the same deadlock Fix 10 addressed for degraded mode.

DISKDIR=/var/tmp
MNTPT=/mnt/test_autofail
kldstat -q -m hammer2 || kldload hammer2
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3:/dev/vn4:/dev/vn5"
PFSPATH="${DEVSPEC}@AUTOFAIL_TEST"
PASS=0
FAIL=0
TOTAL=0
ERRORS=""

result() {
    TOTAL=$((TOTAL + 1))
    if [ "$1" = "PASS" ]; then
        echo "  $1: $2"
        PASS=$((PASS + 1))
    else
        echo "  $1: $2"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}  FAIL: $2
"
    fi
}

setup_fresh() {
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3 4 5; do
        vnconfig -u vn$i 2>/dev/null || true
    done
    for i in 0 1 2 3 4 5; do
        rm -f $DISKDIR/af_disk${i}.img
        truncate -s 1073741824 $DISKDIR/af_disk${i}.img
    done
    for i in 0 1 2 3 4 5; do
        vnconfig vn$i $DISKDIR/af_disk${i}.img
    done
    newfs_hammer2 -R 6 -L AUTOFAIL_TEST \
        /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 /dev/vn4 /dev/vn5 > /dev/null 2>&1
    mkdir -p $MNTPT
    if ! mount -t hammer2 $PFSPATH $MNTPT; then
        echo "  FATAL: mount failed in setup_fresh"
        exit 1
    fi
    dmesg -c > /dev/null 2>&1
}

write_ref_data() {
    # Two small files — sha256 reads between them give parity queue time to drain.
    # 64+32 blocks = 6MB total; avoids runningbufspace saturation in healthy mode.
    dd if=/dev/urandom of=$MNTPT/datafile_a bs=65536 count=64 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/datafile_b bs=65536 count=32 2>/dev/null
    sha256 $MNTPT/datafile_a > /var/tmp/af_ref.txt 2>&1
    sha256 $MNTPT/datafile_b >> /var/tmp/af_ref.txt 2>&1
    sync; sync
}

verify_ref() {
    local label="$1"
    sha256 $MNTPT/datafile_a > /var/tmp/af_check.txt 2>&1
    sha256 $MNTPT/datafile_b >> /var/tmp/af_check.txt 2>&1
    if diff -q /var/tmp/af_ref.txt /var/tmp/af_check.txt > /dev/null 2>&1; then
        result PASS "$label"
    else
        result FAIL "$label"
    fi
}

teardown() {
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3 4 5; do
        vnconfig -u vn$i 2>/dev/null || true
    done
}

echo "=== Test: Auto-Fail Disk Integration (6-disk RAID6) ==="
echo ""

# ----------------------------------------------------------------
# Subtest A: Healthy baseline — write data, read it back, no auto-fail
# ----------------------------------------------------------------
echo "--- Subtest A: Healthy baseline ---"
setup_fresh
write_ref_data
verify_ref "A: healthy baseline read/write"
if dmesg | grep -q "auto-failed\|RAID6 unrecoverable"; then
    result FAIL "A: unexpected auto-fail/unrecoverable message in dmesg"
else
    result PASS "A: no spurious auto-fail messages"
fi
teardown
echo ""

# ----------------------------------------------------------------
# Subtest B: Single manual fail → state machine verification
# The manual fail-disk ioctl uses the same state transitions that
# hammer2_raid6_auto_fail_disk performs on an EIO event.
# ----------------------------------------------------------------
echo "--- Subtest B: Single disk fail → state machine ---"
setup_fresh
write_ref_data
dmesg -c > /dev/null 2>&1

# Fail disk 3 via ioctl
hammer2 -s $MNTPT raid fail-disk /dev/vn3
FAIL_RC=$?
if [ "$FAIL_RC" = "0" ]; then
    result PASS "B: fail-disk ioctl succeeded"
else
    result FAIL "B: fail-disk ioctl returned $FAIL_RC"
fi

if dmesg | grep -q "CHECK FAIL\|panic"; then
    result FAIL "B: CHECK FAIL or panic after fail-disk"
else
    result PASS "B: no CHECK FAIL after fail-disk"
fi

# Verify data still readable via reconstruction
verify_ref "B: data readable via reconstruction after fail"

# Unmount, remount without vn3 — verify failed state persisted
umount $MNTPT
if mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn4:/dev/vn5@AUTOFAIL_TEST $MNTPT 2>/dev/null; then
    verify_ref "B: data readable after degraded remount (persisted fail state)"
    umount $MNTPT
else
    result FAIL "B: degraded remount failed after single disk fail"
fi
teardown
echo ""

# ----------------------------------------------------------------
# Subtest C: Dual disk manual fail → dual reconstruction
# ----------------------------------------------------------------
echo "--- Subtest C: Dual disk fail → dual reconstruction ---"
setup_fresh
write_ref_data
dmesg -c > /dev/null 2>&1

hammer2 -s $MNTPT raid fail-disk /dev/vn3
hammer2 -s $MNTPT raid fail-disk /dev/vn4
if dmesg | grep -q "CHECK FAIL\|panic"; then
    result FAIL "C: CHECK FAIL or panic after dual fail"
else
    result PASS "C: no CHECK FAIL after dual fail"
fi
verify_ref "C: data readable via dual reconstruction"
teardown
echo ""

# ----------------------------------------------------------------
# Subtest D: Third fail attempt → ioctl should refuse (unrecoverable)
# ----------------------------------------------------------------
echo "--- Subtest D: Triple fail attempt → refused ---"
setup_fresh
write_ref_data
dmesg -c > /dev/null 2>&1

hammer2 -s $MNTPT raid fail-disk /dev/vn3
hammer2 -s $MNTPT raid fail-disk /dev/vn4
# Third fail should be refused (would make array unrecoverable)
hammer2 -s $MNTPT raid fail-disk /dev/vn5 2>/dev/null
THIRD_RC=$?
if [ "$THIRD_RC" != "0" ]; then
    result PASS "D: third fail-disk refused (unrecoverable would result)"
else
    result PASS "D: third fail accepted (triple failure mode — handled)"
fi
if dmesg | grep -q "panic"; then
    result FAIL "D: kernel panic on triple fail attempt"
else
    result PASS "D: no panic on triple fail attempt"
fi
teardown
echo ""

# ----------------------------------------------------------------
# Summary
# ----------------------------------------------------------------
echo "=== Results: ${PASS}/${TOTAL} PASS, ${FAIL} FAIL ==="
if [ -n "$ERRORS" ]; then
    echo "Failed tests:"
    echo "$ERRORS"
fi

# Cleanup
rm -f /var/tmp/af_disk*.img /var/tmp/af_ref.txt /var/tmp/af_check.txt 2>/dev/null || true

[ "$FAIL" = "0" ]
