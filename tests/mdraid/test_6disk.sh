#!/bin/sh
# HAMMER2 RAID6 Test: 6-disk array (4 data + P + Q)
#
# Verifies RAID6 works correctly with ndisks = 6.
# Tests healthy read, single failure, dual failure, degraded write, and resilver.
# Each sub-test uses fresh zeroed disks.

DISKDIR=/var/tmp
MNTPT=/mnt/test
kldstat -q -m hammer2 || kldload hammer2
# Create extra vn devices via clone handler (only vn0-vn3 exist by default)
[ -e /dev/vn4 ] || true > /dev/vn
[ -e /dev/vn5 ] || true > /dev/vn
NDISKS=6
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3:/dev/vn4:/dev/vn5"
PFSPATH="${DEVSPEC}@TEST"
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
    # Fresh disk images (rm + truncate = guaranteed zero sparse files)
    for i in 0 1 2 3 4 5; do
        rm -f $DISKDIR/disk${i}.img
        truncate -s 1073741824 $DISKDIR/disk${i}.img
    done
    for i in 0 1 2 3 4 5; do
        vnconfig vn$i $DISKDIR/disk${i}.img
    done
    newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 /dev/vn4 /dev/vn5 > /dev/null 2>&1
    mkdir -p $MNTPT
    if ! mount -t hammer2 $PFSPATH $MNTPT; then
        echo "  FATAL: mount failed in setup_fresh"
        exit 1
    fi
    dmesg -c > /dev/null 2>&1
}

write_ref_data() {
    dd if=/dev/urandom of=$MNTPT/testfile_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/testfile_b bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/testfile_a > /var/tmp/d6_ref.txt
    sha256 $MNTPT/testfile_b >> /var/tmp/d6_ref.txt
    sync; sync
}

verify_ref() {
    local label="$1"
    sha256 $MNTPT/testfile_a > /var/tmp/d6_check.txt 2>&1
    sha256 $MNTPT/testfile_b >> /var/tmp/d6_check.txt 2>&1
    if diff -q /var/tmp/d6_ref.txt /var/tmp/d6_check.txt > /dev/null 2>&1; then
        result PASS "$label"
    else
        result FAIL "$label"
    fi
}

teardown() {
    local label="$1"
    local cfails=$(dmesg | grep -c "CHECK FAIL" || true)
    if [ "$cfails" != "0" ]; then
        result FAIL "$label: $cfails CHECK FAIL(s) in dmesg"
    fi
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3 4 5; do
        vnconfig -u vn$i 2>/dev/null || true
    done
}

echo "=== HAMMER2 RAID6: 6-Disk Array Tests ==="
echo ""

# =====================================================================
# 1. Healthy read (verify 6-disk striping works)
# =====================================================================
echo "--- 1. Healthy read ---"
setup_fresh
write_ref_data
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT
verify_ref "6-disk healthy read after remount"
teardown "6-disk healthy"
echo ""

# =====================================================================
# 2. Single-failure tests (3 representative disks)
# =====================================================================
echo "--- 2. Single-failure tests ---"

for disk in 0 3 5; do
    echo "  Single fail: vn${disk}"
    setup_fresh
    write_ref_data

    hammer2 -s $MNTPT raid fail-disk /dev/vn${disk}
    vnconfig -u vn${disk} 2>/dev/null || true

    verify_ref "6-disk single-fail vn${disk} read (detached)"
    teardown "6-disk single-fail vn${disk}"
done
echo ""

# =====================================================================
# 3. Dual-failure tests (3 representative pairs)
# =====================================================================
echo "--- 3. Dual-failure tests ---"

for pair in "0 5" "2 3" "1 4"; do
    set -- $pair
    a=$1; b=$2
    echo "  Dual fail: vn${a}+vn${b}"
    setup_fresh
    write_ref_data

    hammer2 -s $MNTPT raid fail-disk /dev/vn${a}
    hammer2 -s $MNTPT raid fail-disk /dev/vn${b}
    vnconfig -u vn${a} 2>/dev/null || true
    vnconfig -u vn${b} 2>/dev/null || true

    verify_ref "6-disk dual-fail vn${a}+vn${b} read (detached)"
    teardown "6-disk dual-fail vn${a}+vn${b}"
done
echo ""

# =====================================================================
# 4. Degraded write test
# =====================================================================
echo "--- 4. Degraded write test ---"
setup_fresh
write_ref_data

# Fail vn4, write new data
hammer2 -s $MNTPT raid fail-disk /dev/vn4
dd if=/dev/urandom of=$MNTPT/newfile_c bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/newfile_d bs=65536 count=64  2>/dev/null
sha256 $MNTPT/newfile_c > /var/tmp/d6_new.txt
sha256 $MNTPT/newfile_d >> /var/tmp/d6_new.txt
sync; sync

# Fail vn1 (dual-degraded)
hammer2 -s $MNTPT raid fail-disk /dev/vn1
vnconfig -u vn4 2>/dev/null || true
vnconfig -u vn1 2>/dev/null || true

# Verify both datasets
verify_ref "6-disk degraded-write: ref data"
sha256 $MNTPT/newfile_c > /var/tmp/d6_newchk.txt 2>&1
sha256 $MNTPT/newfile_d >> /var/tmp/d6_newchk.txt 2>&1
if diff -q /var/tmp/d6_new.txt /var/tmp/d6_newchk.txt > /dev/null 2>&1; then
    result PASS "6-disk degraded-write: new data"
else
    result FAIL "6-disk degraded-write: new data"
fi
teardown "6-disk degraded-write"
echo ""

# =====================================================================
# 5. Resilver test
# =====================================================================
echo "--- 5. Resilver test ---"
setup_fresh
write_ref_data

# Fail vn3, replace with spare
hammer2 -s $MNTPT raid fail-disk /dev/vn3
vnconfig -u vn3
rm -f $DISKDIR/disk6.img
truncate -s 1073741824 $DISKDIR/disk6.img
vnconfig vn3 $DISKDIR/disk6.img
hammer2 -s $MNTPT raid replace /dev/vn3 /dev/vn3
echo "  Resilvered vn3"

verify_ref "6-disk resilver: data after resilver"

# Remount check
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT
verify_ref "6-disk resilver: data after remount"
teardown "6-disk resilver"
echo ""

# =====================================================================
# SUMMARY
# =====================================================================
echo "========================================="
echo "=== 6-Disk Tests: $PASS/$TOTAL passed, $FAIL failed ==="
echo "========================================="
if [ -n "$ERRORS" ]; then
    echo ""
    echo "Failures:"
    echo "$ERRORS"
fi
[ $FAIL -eq 0 ]
