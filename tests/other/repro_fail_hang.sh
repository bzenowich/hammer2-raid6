#!/bin/sh
# Minimal repro: dual-fail write hang for specific disk pairs
# Tests each pair individually with a 60-second timeout per test.
# Identifies which disk pair causes the hang.

DISKDIR=/var/tmp
MNTPT=/mnt/test
TIMEOUT=60

run_pair() {
    local a=$1 b=$2
    echo ""
    echo "=== Testing dual-fail write: vn${a} then vn${b} ==="

    # Clean setup
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do
        vnconfig -u vn$i 2>/dev/null || true
    done
    for i in 0 1 2 3; do
        rm -f $DISKDIR/disk${i}.img
        truncate -s 1073741824 $DISKDIR/disk${i}.img
    done
    for i in 0 1 2 3; do
        vnconfig vn$i $DISKDIR/disk${i}.img
    done
    newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
    mkdir -p $MNTPT
    mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST $MNTPT
    dmesg -c > /dev/null 2>&1

    # Write reference data
    dd if=/dev/urandom of=$MNTPT/testfile bs=65536 count=64 2>/dev/null
    sha256 $MNTPT/testfile > /var/tmp/repro_ref.txt
    sync; sync

    # Fail first disk
    echo "  Failing vn${a}..."
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}

    # Write new data while single-degraded
    echo "  Writing while degraded..."
    dd if=/dev/urandom of=$MNTPT/newfile bs=65536 count=64 2>/dev/null
    sha256 $MNTPT/newfile > /var/tmp/repro_new.txt
    sync; sync

    # Fail second disk
    echo "  Failing vn${b}..."
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}

    # Detach both
    vnconfig -u vn${a} 2>/dev/null || true
    vnconfig -u vn${b} 2>/dev/null || true

    # Try reading reference data with timeout
    echo "  Reading reference data (timeout ${TIMEOUT}s)..."
    sha256 $MNTPT/testfile > /var/tmp/repro_chk1.txt 2>&1 &
    PID=$!
    DEADLINE=$(($(date +%s) + TIMEOUT))
    while kill -0 $PID 2>/dev/null; do
        if [ $(date +%s) -gt $DEADLINE ]; then
            echo "  TIMEOUT: reference data read HUNG (pid $PID)"
            kill -9 $PID 2>/dev/null || true
            dmesg | grep -i "check fail" | head -5
            return 1
        fi
        sleep 1
    done
    wait $PID 2>/dev/null
    if diff -q /var/tmp/repro_ref.txt /var/tmp/repro_chk1.txt > /dev/null 2>&1; then
        echo "  Reference data: PASS"
    else
        echo "  Reference data: FAIL (mismatch)"
    fi

    # Try reading new (degraded-written) data with timeout
    echo "  Reading degraded-written data (timeout ${TIMEOUT}s)..."
    sha256 $MNTPT/newfile > /var/tmp/repro_chk2.txt 2>&1 &
    PID=$!
    DEADLINE=$(($(date +%s) + TIMEOUT))
    while kill -0 $PID 2>/dev/null; do
        if [ $(date +%s) -gt $DEADLINE ]; then
            echo "  TIMEOUT: degraded-written data read HUNG (pid $PID)"
            echo "  Process state:"
            ps -l -p $PID 2>/dev/null || true
            dmesg | tail -10
            kill -9 $PID 2>/dev/null || true
            return 1
        fi
        sleep 1
    done
    wait $PID 2>/dev/null
    if diff -q /var/tmp/repro_new.txt /var/tmp/repro_chk2.txt > /dev/null 2>&1; then
        echo "  Degraded-written data: PASS"
    else
        echo "  Degraded-written data: FAIL (mismatch)"
    fi

    local cfails=$(dmesg | grep -c "CHECK FAIL" || true)
    if [ "$cfails" != "0" ]; then
        echo "  WARNING: $cfails CHECK FAIL(s)"
    fi

    echo "  DONE: vn${a}+vn${b} OK"
    umount $MNTPT 2>/dev/null || true
    return 0
}

echo "=== Dual-fail write hang reproduction ==="
echo "Testing all 6 disk pairs with ${TIMEOUT}s timeout each"

# Test pairs in order - the (0,1) pair is expected to hang
PAIRS="2 1
0 2
0 3
1 2
1 3
0 1"

PASSED=0
FAILED=0
HUNG=0

echo "$PAIRS" | while read a b; do
    if run_pair $a $b; then
        PASSED=$((PASSED + 1))
    else
        echo "  *** PAIR vn${a}+vn${b} FAILED/HUNG ***"
        FAILED=$((FAILED + 1))
        # Don't continue if hung - need reboot
        echo "  Attempting cleanup..."
        umount -f $MNTPT 2>/dev/null || true
        for i in 0 1 2 3; do
            vnconfig -u vn$i 2>/dev/null || true
        done
        # Check if cleanup worked
        sleep 2
        if ps aux | grep -v grep | grep -q "sha256.*mnt/test"; then
            echo "  Processes still stuck - remaining tests may fail"
            echo "  Consider rebooting"
        fi
    fi
done

echo ""
echo "=== Complete ==="
