#!/bin/sh
# HAMMER2 RAID6 Test: Sequential dual-disk resilver
#
# Scenario A: Fail 2 disks, resilver first, resilver second (sequential dual resilver)
# Scenario B: Fail-resilver-fail-resilver (serial single failures)
#
# Tests multiple disk pairs to exercise different reconstruction paths.

DISKDIR=/var/tmp
MNTPT=/mnt/test
kldstat -q -m hammer2 || kldload hammer2
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
    for i in 0 1 2 3; do
        vnconfig -u vn$i 2>/dev/null || true
    done
    # Fresh disk images (rm + truncate = guaranteed zero sparse files)
    for i in 0 1 2 3; do
        rm -f $DISKDIR/disk${i}.img
        truncate -s 1073741824 $DISKDIR/disk${i}.img
    done
    # Create spare images
    rm -f $DISKDIR/disk4.img $DISKDIR/disk5.img
    truncate -s 1073741824 $DISKDIR/disk4.img
    truncate -s 1073741824 $DISKDIR/disk5.img
    for i in 0 1 2 3; do
        vnconfig vn$i $DISKDIR/disk${i}.img
    done
    newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
    mkdir -p $MNTPT
    if ! mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST $MNTPT; then
        echo "  FATAL: mount failed in setup_fresh"
        exit 1
    fi
    dmesg -c > /dev/null 2>&1
}

teardown() {
    local label="$1"
    local cfails=$(dmesg | grep -c "CHECK FAIL" || true)
    if [ "$cfails" != "0" ]; then
        result FAIL "$label: $cfails CHECK FAIL(s) in dmesg"
    fi
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do
        vnconfig -u vn$i 2>/dev/null || true
    done
}

echo "=== HAMMER2 RAID6: Sequential Dual-Disk Resilver ==="
echo ""

# =====================================================================
# SCENARIO A: Fail 2, resilver both sequentially
# =====================================================================
echo "=== Scenario A: Fail 2, resilver both sequentially ==="
echo ""

for pair in "2 1" "0 3" "1 2"; do
    set -- $pair
    a=$1; b=$2
    echo "--- Scenario A: fail vn${a}+vn${b}, resilver sequentially ---"
    setup_fresh

    # Write healthy data
    dd if=/dev/urandom of=$MNTPT/data_healthy bs=65536 count=128 2>/dev/null
    sha256 $MNTPT/data_healthy > /var/tmp/seqr_ref.txt
    sync; sync

    # Fail first disk, write degraded data
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}
    dd if=/dev/urandom of=$MNTPT/data_degraded1 bs=65536 count=128 2>/dev/null
    sha256 $MNTPT/data_degraded1 >> /var/tmp/seqr_ref.txt
    sync; sync

    # Fail second disk
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}

    # Verify all data readable in dual-degraded
    sha256 $MNTPT/data_healthy > /var/tmp/seqr_check.txt 2>&1
    sha256 $MNTPT/data_degraded1 >> /var/tmp/seqr_check.txt 2>&1
    if diff -q /var/tmp/seqr_ref.txt /var/tmp/seqr_check.txt > /dev/null 2>&1; then
        result PASS "A vn${a}+vn${b}: dual-degraded read"
    else
        result FAIL "A vn${a}+vn${b}: dual-degraded read"
    fi

    # Resilver first disk (vnA) with spare
    vnconfig -u vn${a}
    vnconfig vn${a} $DISKDIR/disk4.img
    hammer2 -s $MNTPT raid replace /dev/vn${a} /dev/vn${a}
    echo "  Resilvered vn${a} (vnB still failed)"

    # Verify data (now single-degraded on vnB only)
    sha256 $MNTPT/data_healthy > /var/tmp/seqr_check2.txt 2>&1
    sha256 $MNTPT/data_degraded1 >> /var/tmp/seqr_check2.txt 2>&1
    if diff -q /var/tmp/seqr_ref.txt /var/tmp/seqr_check2.txt > /dev/null 2>&1; then
        result PASS "A vn${a}+vn${b}: read after 1st resilver (single-degraded on vn${b})"
    else
        result FAIL "A vn${a}+vn${b}: read after 1st resilver (single-degraded on vn${b})"
    fi

    # Write more data after first resilver
    dd if=/dev/urandom of=$MNTPT/data_after_resilver1 bs=65536 count=128 2>/dev/null
    sha256 $MNTPT/data_after_resilver1 >> /var/tmp/seqr_ref.txt
    sync; sync

    # Resilver second disk (vnB) with spare
    vnconfig -u vn${b}
    vnconfig vn${b} $DISKDIR/disk5.img
    hammer2 -s $MNTPT raid replace /dev/vn${b} /dev/vn${b}
    echo "  Resilvered vn${b} (array fully restored)"

    # Verify ALL 3 datasets (healthy array)
    sha256 $MNTPT/data_healthy > /var/tmp/seqr_check3.txt 2>&1
    sha256 $MNTPT/data_degraded1 >> /var/tmp/seqr_check3.txt 2>&1
    sha256 $MNTPT/data_after_resilver1 >> /var/tmp/seqr_check3.txt 2>&1
    if diff -q /var/tmp/seqr_ref.txt /var/tmp/seqr_check3.txt > /dev/null 2>&1; then
        result PASS "A vn${a}+vn${b}: all 3 datasets after full restore"
    else
        result FAIL "A vn${a}+vn${b}: all 3 datasets after full restore"
    fi

    # Unmount + remount to verify persistence
    umount $MNTPT
    mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST $MNTPT
    sha256 $MNTPT/data_healthy > /var/tmp/seqr_check4.txt 2>&1
    sha256 $MNTPT/data_degraded1 >> /var/tmp/seqr_check4.txt 2>&1
    sha256 $MNTPT/data_after_resilver1 >> /var/tmp/seqr_check4.txt 2>&1
    if diff -q /var/tmp/seqr_ref.txt /var/tmp/seqr_check4.txt > /dev/null 2>&1; then
        result PASS "A vn${a}+vn${b}: all data intact after remount"
    else
        result FAIL "A vn${a}+vn${b}: all data intact after remount"
    fi

    teardown "A vn${a}+vn${b}"
    echo ""
done

# =====================================================================
# SCENARIO B: Fail-resilver-fail-resilver (serial single failures)
# =====================================================================
echo "=== Scenario B: Fail-resilver-fail-resilver (serial singles) ==="
echo ""

for pair in "2 1" "0 3"; do
    set -- $pair
    a=$1; b=$2
    echo "--- Scenario B: vn${a} then vn${b} (serial single failures) ---"
    setup_fresh

    # Write healthy data
    dd if=/dev/urandom of=$MNTPT/data_healthy bs=65536 count=128 2>/dev/null
    sha256 $MNTPT/data_healthy > /var/tmp/seqr_b_ref.txt
    sync; sync

    # Fail vnA, resilver vnA
    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}
    vnconfig -u vn${a}
    vnconfig vn${a} $DISKDIR/disk4.img
    hammer2 -s $MNTPT raid replace /dev/vn${a} /dev/vn${a}
    echo "  Resilvered vn${a} (first cycle)"

    # Verify data after first resilver
    sha256 $MNTPT/data_healthy > /var/tmp/seqr_b_check1.txt 2>&1
    if diff -q /var/tmp/seqr_b_ref.txt /var/tmp/seqr_b_check1.txt > /dev/null 2>&1; then
        result PASS "B vn${a}->vn${b}: data intact after 1st resilver (vn${a})"
    else
        result FAIL "B vn${a}->vn${b}: data intact after 1st resilver (vn${a})"
    fi

    # Fail vnB, resilver vnB
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}
    vnconfig -u vn${b}
    vnconfig vn${b} $DISKDIR/disk5.img
    hammer2 -s $MNTPT raid replace /dev/vn${b} /dev/vn${b}
    echo "  Resilvered vn${b} (second cycle)"

    # Verify data after second resilver
    sha256 $MNTPT/data_healthy > /var/tmp/seqr_b_check2.txt 2>&1
    if diff -q /var/tmp/seqr_b_ref.txt /var/tmp/seqr_b_check2.txt > /dev/null 2>&1; then
        result PASS "B vn${a}->vn${b}: data intact after 2nd resilver (vn${b})"
    else
        result FAIL "B vn${a}->vn${b}: data intact after 2nd resilver (vn${b})"
    fi

    # Unmount + remount
    umount $MNTPT
    mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST $MNTPT
    sha256 $MNTPT/data_healthy > /var/tmp/seqr_b_check3.txt 2>&1
    if diff -q /var/tmp/seqr_b_ref.txt /var/tmp/seqr_b_check3.txt > /dev/null 2>&1; then
        result PASS "B vn${a}->vn${b}: data intact after remount"
    else
        result FAIL "B vn${a}->vn${b}: data intact after remount"
    fi

    teardown "B vn${a}->vn${b}"
    echo ""
done

# =====================================================================
# SUMMARY
# =====================================================================
echo "========================================="
echo "=== Sequential Resilver: $PASS/$TOTAL passed, $FAIL failed ==="
echo "========================================="
if [ -n "$ERRORS" ]; then
    echo ""
    echo "Failures:"
    echo "$ERRORS"
fi
[ $FAIL -eq 0 ]
