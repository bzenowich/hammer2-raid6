#!/bin/sh
# common.sh — shared helpers for raidz2native (v4) integration tests
#
# Uses swap-backed vn devices (vnconfig -S): no image files, no UFS layer.

MNTPT=/mnt/rz2test
NDISKS=6
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3:/dev/vn4:/dev/vn5"
PFSPATH="${DEVSPEC}@RZ2TEST"
PASS=0; FAIL=0; TOTAL=0; ERRORS=""

result() {
    TOTAL=$((TOTAL + 1))
    if [ "$1" = "PASS" ]; then
        echo "  PASS: $2"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $2"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}  FAIL: $2
"
    fi
}

check_no_checkfail() {
    local label="$1"
    local cfails
    cfails=$(dmesg | grep -c "CHECK FAIL" 2>/dev/null || echo 0)
    if [ "$cfails" != "0" ]; then
        result FAIL "$label: $cfails CHECK FAIL(s) in dmesg"
        return 1
    fi
    return 0
}

setup_fresh() {
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3 4 5; do
        vnconfig -u vn$i 2>/dev/null || true
        vnconfig -S 1073741824 vn$i
    done
    newfs_hammer2 -R 6 -L RZ2TEST \
        /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 /dev/vn4 /dev/vn5 \
        > /dev/null 2>&1
    mkdir -p $MNTPT
    if ! mount -t hammer2 $PFSPATH $MNTPT; then
        echo "  FATAL: mount failed in setup_fresh"
        exit 1
    fi
    dmesg -c > /dev/null 2>&1
}

teardown() {
    local label="$1"
    check_no_checkfail "$label"
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3 4 5; do
        vnconfig -u vn$i 2>/dev/null || true
    done
}

write_ref_data() {
    local prefix="${1:-ref}"
    dd if=/dev/urandom of=$MNTPT/${prefix}_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/${prefix}_b bs=65536 count=64 2>/dev/null
    sha256 $MNTPT/${prefix}_a > /var/tmp/rz2_${prefix}.txt
    sha256 $MNTPT/${prefix}_b >> /var/tmp/rz2_${prefix}.txt
    sync; sync
}

verify_ref() {
    local label="$1"
    local prefix="${2:-ref}"
    sha256 $MNTPT/${prefix}_a > /var/tmp/rz2_check.txt 2>&1
    sha256 $MNTPT/${prefix}_b >> /var/tmp/rz2_check.txt 2>&1
    if diff -q /var/tmp/rz2_${prefix}.txt /var/tmp/rz2_check.txt \
            > /dev/null 2>&1; then
        result PASS "$label"
    else
        result FAIL "$label"
    fi
}

summary() {
    echo "========================================="
    echo "=== $PASS/$TOTAL PASS, $FAIL FAIL ==="
    echo "========================================="
    if [ -n "$ERRORS" ]; then
        printf "Failures:\n%s" "$ERRORS"
    fi
    [ $FAIL -eq 0 ]
}

# Version guard: check that the mounted filesystem is v4 (RAIDZ2-native)
check_v4() {
    if hammer2 -s $MNTPT volconf 2>/dev/null | grep -q "version.*4"; then
        return 0
    fi
    echo "SKIP: v4 (RAIDZ2-native) format not detected; skipping test suite"
    exit 77
}
