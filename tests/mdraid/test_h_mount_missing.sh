#!/bin/sh
# HAMMER2 RAID6 Test H: Mount behavior with missing/failed devices
#
# No direct mdadm equivalent — tests HAMMER2 mount semantics.
#
# Three scenarios:
#   1. Mount when a device is simply not configured (vnconfig missing)
#      — should fail cleanly.
#   2. Mark disk failed via raid fail-disk, unmount, remount with disk
#      still attached but state=FAILED in on-disk header
#      — should mount in degraded mode.
#   3. Mark disk failed, unmount, unconfigure it (vnconfig -u), remount
#      — should mount degraded using on-disk FAILED state to skip device.

DISKDIR=/var/tmp
MNTPT=/mnt/test
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@TEST"
PASS=0
FAIL=0

result() {
    if [ "$1" = "PASS" ]; then
        echo "$1: $2"
        PASS=$((PASS + 1))
    else
        echo "$1: $2"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== HAMMER2 RAID6 Test H: Mount With Missing/Failed Devices ==="
echo ""

# --- Base setup ---
echo "--- Base setup: format clean array ---"
if df | grep -q "$MNTPT"; then
    umount $MNTPT
fi
for i in 0 1 2 3; do
    vnconfig -u vn$i 2>/dev/null || true
    vnconfig vn$i $DISKDIR/disk${i}.img
done
mkdir -p $MNTPT
newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
mount -t hammer2 $PFSPATH $MNTPT
dd if=/dev/urandom of=$MNTPT/testfile bs=65536 count=64 2>/dev/null
sha256 $MNTPT/testfile > /var/tmp/test_h_sha.txt
sync; sync
umount $MNTPT
echo "Base setup done."

# =============================================================
# Scenario 1: Mount with device not configured (degraded mount)
# =============================================================
echo ""
echo "--- Scenario 1: mount with vn2 not configured (degraded) ---"

# Unconfigure vn2 entirely (no disk image behind it)
vnconfig -u vn2 2>/dev/null || true

# Attempt mount — should succeed in degraded mode
if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
    result PASS "mount succeeded in degraded mode (vn2 not configured)"

    # Verify data is readable
    sha256 $MNTPT/testfile > /var/tmp/test_h_check1.txt
    if diff -q /var/tmp/test_h_sha.txt /var/tmp/test_h_check1.txt > /dev/null 2>&1; then
        result PASS "data readable in degraded mode (scenario 1)"
    else
        result FAIL "data not readable in degraded mode (scenario 1)"
    fi
    umount $MNTPT
else
    result FAIL "mount failed with vn2 not configured (degraded mount)"
fi

# Restore vn2
vnconfig vn2 $DISKDIR/disk2.img

# =============================================================
# Scenario 2: Disk marked FAILED in header, device still present
# =============================================================
echo ""
echo "--- Scenario 2: remount after fail-disk (device still present) ---"

# Mount, mark failed, unmount
mount -t hammer2 $PFSPATH $MNTPT
hammer2 -s $MNTPT raid fail-disk /dev/vn2
umount $MNTPT
echo "vn2 marked FAILED in on-disk header. Device still configured."

# Attempt remount — should succeed in degraded mode
if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
    result PASS "mount succeeded in degraded mode (vn2 FAILED, still present)"

    # Verify data is readable
    sha256 $MNTPT/testfile > /var/tmp/test_h_check2.txt
    if diff -q /var/tmp/test_h_sha.txt /var/tmp/test_h_check2.txt > /dev/null 2>&1; then
        result PASS "data readable in degraded mode (scenario 2)"
    else
        result FAIL "data not readable in degraded mode (scenario 2)"
    fi
    umount $MNTPT
else
    result FAIL "mount failed in degraded mode (vn2 FAILED but present) — not yet implemented?"
fi

# =============================================================
# Scenario 3: Disk marked FAILED in header, device unconfigured
# =============================================================
echo ""
echo "--- Scenario 3: remount after fail-disk + vnconfig -u ---"

# Unconfigure vn2 (simulates physically removed disk)
vnconfig -u vn2 2>/dev/null || true
echo "vn2 unconfigured. On-disk state still FAILED."

# Attempt remount — should succeed, using on-disk FAILED state to skip vn2
if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
    result PASS "mount succeeded with vn2 absent (FAILED in header)"

    # Verify data is readable
    sha256 $MNTPT/testfile > /var/tmp/test_h_check3.txt
    if diff -q /var/tmp/test_h_sha.txt /var/tmp/test_h_check3.txt > /dev/null 2>&1; then
        result PASS "data readable with vn2 absent (scenario 3)"
    else
        result FAIL "data not readable with vn2 absent (scenario 3)"
    fi
    umount $MNTPT
else
    result FAIL "mount failed with vn2 absent (FAILED in header) — not yet implemented?"
fi

# --- Restore vn2 ---
vnconfig vn2 $DISKDIR/disk2.img 2>/dev/null || true

echo ""
echo "=== Test H complete: $PASS passed, $FAIL failed ==="
# Note: scenarios 2 and 3 may fail if mount-time degraded state
# restoration is not yet implemented. This is expected and documented.
[ $FAIL -eq 0 ]
