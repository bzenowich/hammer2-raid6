#!/bin/sh
# HAMMER2 RAID6 Test J: Unclean unmount / power-loss simulation
#
# Derived from: HAMMER2 COW consistency property
#
# Writes data and syncs (checkpoint 1). Writes more data without syncing.
# Forces unmount with umount -f to simulate an abrupt shutdown. Remounts
# and verifies the filesystem is consistent and synced data is intact.
#
# HAMMER2's COW design guarantees that the on-disk state is always a
# consistent snapshot — either a write committed or it didn't; partial
# states are not visible after remount.

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

echo "=== HAMMER2 RAID6 Test J: Unclean Unmount (Power-Loss Simulation) ==="
echo ""

# --- Setup ---
echo "--- Setup ---"
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
echo "Mounted."

# --- Write and sync checkpoint 1 ---
echo ""
echo "--- Writing and syncing checkpoint 1 ---"
dd if=/dev/urandom of=$MNTPT/synced_file bs=65536 count=128 2>/dev/null
sync; sync
sha256 $MNTPT/synced_file > /var/tmp/test_j_synced.txt
echo "Checkpoint 1 synced. Hash: $(cat /var/tmp/test_j_synced.txt)"

# --- Write more data WITHOUT explicit sync ---
echo ""
echo "--- Writing data WITHOUT sync (simulate in-flight writes) ---"
dd if=/dev/urandom of=$MNTPT/unsynced_file bs=65536 count=64 2>/dev/null
# Deliberately no sync here

# --- Force unmount (simulate power loss / abrupt shutdown) ---
echo ""
echo "--- Forcing unmount (simulating abrupt shutdown) ---"
# umount -f flushes buffers on DragonFlyBSD but skips the graceful
# filesystem-level close. This is the closest we can get to power-off
# without actually killing the VM.
if umount -f $MNTPT 2>/dev/null; then
    echo "umount -f: SUCCESS"
else
    # Fallback: normal umount (kernel will flush dirty pages)
    echo "umount -f failed, falling back to normal umount"
    umount $MNTPT
fi

# --- Remount ---
echo ""
echo "--- Remounting ---"
if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
    result PASS "filesystem mounts cleanly after forced unmount"
else
    result FAIL "filesystem failed to mount after forced unmount"
    echo ""
    echo "dmesg tail:"
    dmesg | tail -20
    exit 1
fi

# --- Verify synced data is intact ---
echo ""
echo "--- Verifying synced data ---"
if [ -f $MNTPT/synced_file ]; then
    sha256 $MNTPT/synced_file > /var/tmp/test_j_check.txt
    if diff -q /var/tmp/test_j_synced.txt /var/tmp/test_j_check.txt > /dev/null 2>&1; then
        result PASS "synced_file intact after forced unmount"
    else
        result FAIL "synced_file corrupted after forced unmount"
    fi
else
    result FAIL "synced_file missing after forced unmount"
fi

# --- Check for any bad-magic or CHECK FAIL in dmesg ---
echo ""
echo "--- Checking dmesg for errors ---"
if dmesg | tail -50 | grep -q "bad magic\|CHECK FAIL\|bad crc"; then
    result FAIL "bad-magic or CHECK FAIL errors in dmesg after remount"
    dmesg | tail -30 | grep -i "bad magic\|check fail\|bad crc" || true
else
    result PASS "no bad-magic or CHECK FAIL errors in dmesg"
fi

# --- Second round: force unmount in degraded mode ---
echo ""
echo "--- Scenario 2: forced unmount while degraded ---"
dd if=/dev/urandom of=$MNTPT/degraded_pre bs=65536 count=64 2>/dev/null
sync; sync
sha256 $MNTPT/degraded_pre > /var/tmp/test_j_degraded.txt
hammer2 -s $MNTPT raid fail-disk /dev/vn2
dd if=/dev/urandom of=$MNTPT/degraded_unsynced bs=65536 count=32 2>/dev/null
umount -f $MNTPT 2>/dev/null || umount $MNTPT

# Remount
if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
    result PASS "degraded filesystem mounts after forced unmount"
    sha256 $MNTPT/degraded_pre > /var/tmp/test_j_degraded_check.txt
    if diff -q /var/tmp/test_j_degraded.txt /var/tmp/test_j_degraded_check.txt > /dev/null 2>&1; then
        result PASS "synced degraded data intact after forced unmount"
    else
        result FAIL "synced degraded data corrupted after forced unmount"
    fi
else
    result FAIL "degraded filesystem failed to mount after forced unmount"
fi

umount $MNTPT 2>/dev/null || true

echo ""
echo "=== Test J complete: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ]
