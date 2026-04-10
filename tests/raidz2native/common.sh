#!/bin/sh
# common.sh — shared helpers for raidz2native (v4) integration tests
#
# DISK_MODE=vtbd  (default): use physical /dev/vtbd* QEMU block devices
# DISK_MODE=vn             : use swap-backed vnconfig -S devices (in-memory)
#
# NDISKS: number of disks to use (default 4, supports 4-6)

DISK_MODE="${DISK_MODE:-vtbd}"
NDISKS="${NDISKS:-4}"
MNTPT=/mnt/rz2test
PASS=0; FAIL=0; TOTAL=0; ERRORS=""

# Build device list and colon-separated DEVSPEC
DEVS=""
DEVSPEC=""
i=0
while [ "$i" -lt "$NDISKS" ]; do
    if [ "$DISK_MODE" = "vtbd" ]; then
        dev="/dev/vbd${i}"
    else
        dev="/dev/vn${i}"
    fi
    DEVS="${DEVS} ${dev}"
    if [ -z "$DEVSPEC" ]; then
        DEVSPEC="${dev}"
    else
        DEVSPEC="${DEVSPEC}:${dev}"
    fi
    i=$((i + 1))
done
PFSPATH="${DEVSPEC}@RZ2TEST"

# Return the device path for disk index $1
disk_dev() {
    if [ "$DISK_MODE" = "vtbd" ]; then
        echo "/dev/vbd${1}"
    else
        echo "/dev/vn${1}"
    fi
}

# Build a DEVSPEC with disk index $1 excluded (for degraded mount)
degraded_spec() {
    local skip="$1"
    local spec=""
    local j=0
    while [ "$j" -lt "$NDISKS" ]; do
        if [ "$j" != "$skip" ]; then
            dev=$(disk_dev "$j")
            spec="${spec:+${spec}:}${dev}"
        fi
        j=$((j + 1))
    done
    echo "$spec"
}

# Detach disk $1 (index).  vn mode: unconfigure; vtbd mode: disk stays present.
detach_disk() {
    local idx="$1"
    if [ "$DISK_MODE" = "vn" ]; then
        vnconfig -u vn${idx} 2>/dev/null || true
    fi
}

# Prepare disk $1 as a fresh replacement for resilver.
# vn mode: unconfigure then re-configure with fresh swap backing.
# vtbd mode: zero the first 64 MB (one HAMMER2 zone) to clear metadata.
fresh_disk() {
    local idx="$1"
    if [ "$DISK_MODE" = "vn" ]; then
        vnconfig -u vn${idx} 2>/dev/null || true
        vnconfig -S 1073741824 vn${idx}
    else
        dd if=/dev/zero of=/dev/vbd${idx} bs=65536 count=1024 2>/dev/null || true
    fi
}

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
    # grep -c exits 1 (no matches) or 0 (matches); always outputs a count.
    # Do NOT use "|| echo 0" — that appends a second zero when grep exits 1,
    # corrupting cfails into "0\n0" which makes result() print two lines.
    cfails=$(dmesg | grep -c "CHECK FAIL" 2>/dev/null)
    cfails="${cfails:-0}"
    if [ "$cfails" != "0" ]; then
        result FAIL "$label: $cfails CHECK FAIL(s) in dmesg"
        return 1
    fi
    return 0
}

setup_fresh() {
    umount $MNTPT 2>/dev/null || true
    if [ "$DISK_MODE" = "vn" ]; then
        i=0
        while [ "$i" -lt "$NDISKS" ]; do
            vnconfig -u vn${i} 2>/dev/null || true
            vnconfig -S 1073741824 vn${i}
            i=$((i + 1))
        done
    fi
    # Retry newfs up to 5 times: devices may be briefly busy after umount
    local newfs_ok=0
    local attempt=0
    while [ "$attempt" -lt 5 ]; do
        # shellcheck disable=SC2086
        if newfs_hammer2 -R 6 -L RZ2TEST $DEVS > /dev/null 2>&1; then
            newfs_ok=1
            break
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    if [ "$newfs_ok" = "0" ]; then
        echo "  FATAL: newfs_hammer2 failed after 5 attempts in setup_fresh"
        # shellcheck disable=SC2086
        newfs_hammer2 -R 6 -L RZ2TEST $DEVS 2>&1 | head -5
        exit 1
    fi
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
    sync
    umount $MNTPT 2>/dev/null || umount -f $MNTPT 2>/dev/null || true
    if [ "$DISK_MODE" = "vn" ]; then
        i=0
        while [ "$i" -lt "$NDISKS" ]; do
            vnconfig -u vn${i} 2>/dev/null || true
            i=$((i + 1))
        done
    fi
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
    if hammer2 -s $MNTPT volume-list 2>/dev/null | grep -q "^version 4"; then
        return 0
    fi
    echo "SKIP: v4 (RAIDZ2-native) format not detected; skipping test suite"
    exit 77
}
