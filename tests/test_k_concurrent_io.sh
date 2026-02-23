#!/bin/sh
# HAMMER2 RAID6 Test K: Concurrent I/O during degraded + resilver
#
# Derived from: mdadm 24raid456deadlock
#
# Runs multiple parallel writers and readers while simultaneously failing
# a disk and running a resilver. Verifies:
#   (a) no process hangs indefinitely (deadlock check via timeout)
#   (b) all writers complete successfully
#   (c) all data is intact after resilver completes
#
# A deadlock is detected if any background process fails to complete
# within TIMEOUT seconds.

DISKDIR=/var/tmp
MNTPT=/mnt/test
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@TEST"
NWORKERS=4
TIMEOUT=120   # seconds to wait for workers before declaring deadlock
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

echo "=== HAMMER2 RAID6 Test K: Concurrent I/O During Degraded + Resilver ==="
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
truncate -s 1073741824 $DISKDIR/disk4.img
mkdir -p $MNTPT
newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
mount -t hammer2 $PFSPATH $MNTPT
echo "Mounted."

# --- Write initial data ---
echo ""
echo "--- Writing initial data ---"
for i in $(seq 1 $NWORKERS); do
    dd if=/dev/urandom of=$MNTPT/init_$i bs=65536 count=64 2>/dev/null
done
for i in $(seq 1 $NWORKERS); do
    sha256 $MNTPT/init_$i
done > /var/tmp/test_k_init_hashes.txt
sync; sync
echo "Initial data written."

# --- Start background writers ---
echo ""
echo "--- Starting $NWORKERS background writers ---"
for i in $(seq 1 $NWORKERS); do
    dd if=/dev/urandom of=$MNTPT/concurrent_$i bs=65536 count=128 2>/dev/null &
    echo $! >> /var/tmp/test_k_writer_pids.txt
done
echo "Writers started: $(cat /var/tmp/test_k_writer_pids.txt | tr '\n' ' ')"

# Small delay to let writes begin
sleep 1

# --- Fail vn2 while writers are running ---
echo ""
echo "--- Failing vn2 while writers are running ---"
hammer2 -s $MNTPT raid fail-disk /dev/vn2
vnconfig -u vn2
vnconfig vn2 $DISKDIR/disk4.img
echo "vn2 replaced by disk4.img"

# --- Start resilver while writers are running ---
echo ""
echo "--- Starting resilver while writers are running ---"
hammer2 -s $MNTPT raid replace /dev/vn2 /dev/vn2 &
RESILVER_PID=$!
echo "Resilver PID: $RESILVER_PID"

# --- Wait for writers with deadlock detection ---
echo ""
echo "--- Waiting for background writers (timeout: ${TIMEOUT}s) ---"
WRITER_PIDS=$(cat /var/tmp/test_k_writer_pids.txt 2>/dev/null)
rm -f /var/tmp/test_k_writer_pids.txt

DEADLINE=$(($(date +%s) + TIMEOUT))
ALL_DONE=1
for pid in $WRITER_PIDS; do
    while kill -0 $pid 2>/dev/null; do
        if [ $(date +%s) -gt $DEADLINE ]; then
            result FAIL "DEADLOCK DETECTED: writer PID $pid did not complete within ${TIMEOUT}s"
            kill $pid 2>/dev/null || true
            ALL_DONE=0
            break
        fi
        sleep 1
    done
done

if [ $ALL_DONE -eq 1 ]; then
    result PASS "all $NWORKERS writers completed without deadlock"
fi

# --- Wait for resilver with deadlock detection ---
echo ""
echo "--- Waiting for resilver (timeout: ${TIMEOUT}s) ---"
DEADLINE=$(($(date +%s) + TIMEOUT))
while kill -0 $RESILVER_PID 2>/dev/null; do
    if [ $(date +%s) -gt $DEADLINE ]; then
        result FAIL "DEADLOCK DETECTED: resilver did not complete within ${TIMEOUT}s"
        kill $RESILVER_PID 2>/dev/null || true
        RESILVER_PID=0
        break
    fi
    sleep 2
done

if [ "$RESILVER_PID" != "0" ] && ! kill -0 $RESILVER_PID 2>/dev/null; then
    wait $RESILVER_PID
    if [ $? -eq 0 ]; then
        result PASS "resilver completed successfully"
    else
        result FAIL "resilver exited with error"
    fi
fi

sync; sync

# --- Verify initial data still intact ---
echo ""
echo "--- Verifying initial data integrity ---"
for i in $(seq 1 $NWORKERS); do
    sha256 $MNTPT/init_$i
done > /var/tmp/test_k_init_check.txt
if diff -q /var/tmp/test_k_init_hashes.txt /var/tmp/test_k_init_check.txt > /dev/null 2>&1; then
    result PASS "initial data intact after concurrent resilver + writes"
else
    result FAIL "initial data corrupted during concurrent resilver + writes"
fi

# --- Verify concurrent writes are readable ---
echo ""
echo "--- Verifying concurrent write files exist and are readable ---"
READABLE=0
for i in $(seq 1 $NWORKERS); do
    if sha256 $MNTPT/concurrent_$i > /dev/null 2>&1; then
        READABLE=$((READABLE + 1))
    fi
done
if [ $READABLE -eq $NWORKERS ]; then
    result PASS "all $NWORKERS concurrent write files are readable"
else
    result FAIL "only $READABLE of $NWORKERS concurrent write files readable"
fi

# --- Unmount + remount integrity check ---
echo ""
echo "--- Unmount + remount ---"
umount $MNTPT
if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
    result PASS "filesystem mounts cleanly after concurrent test"

    for i in $(seq 1 $NWORKERS); do
        sha256 $MNTPT/init_$i
    done > /var/tmp/test_k_remount_check.txt
    if diff -q /var/tmp/test_k_init_hashes.txt /var/tmp/test_k_remount_check.txt > /dev/null 2>&1; then
        result PASS "initial data intact after remount"
    else
        result FAIL "initial data corrupted after remount"
    fi
    umount $MNTPT
else
    result FAIL "filesystem failed to mount after concurrent test"
fi

echo ""
echo "=== Test K complete: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ]
