#!/bin/sh
# common.sh — shared helpers for v4 (RAIDZ2-native) integration tests.
# Authoritative test substrate is virtio-blk (/dev/vbd*); per
# newplan.md §8, vn-backed runs are no longer supported.
#
# The harness VM (launch-dfly.sh) places the SYSTEM disk at /dev/vbd0
# and the RAID test disks at /dev/vbd1..vbd${NDISKS}.  Test disks must
# never include vbd0 or the next setup_fresh will newfs the root
# filesystem and panic the box.  $DISK_BASE is the offset into vbd*
# (default 1 = skip system); override with DISK_BASE=0 only if your
# harness puts test disks at vbd0 (no current launch-dfly config does).
#
# NDISKS: number of disks to use (default 4, supports 4-6).

NDISKS="${NDISKS:-4}"
DISK_BASE="${DISK_BASE:-1}"
MNTPT=/mnt/v4test
PASS=0; FAIL=0; TOTAL=0; ERRORS=""

# Build device list and colon-separated DEVSPEC
DEVS=""
DEVSPEC=""
i=0
while [ "$i" -lt "$NDISKS" ]; do
    dev="/dev/vbd$((DISK_BASE + i))"
    DEVS="${DEVS} ${dev}"
    if [ -z "$DEVSPEC" ]; then
        DEVSPEC="${dev}"
    else
        DEVSPEC="${DEVSPEC}:${dev}"
    fi
    i=$((i + 1))
done
PFSPATH="${DEVSPEC}@V4TEST"

# Return the device path for disk index $1 (logical 0..NDISKS-1)
disk_dev() {
    echo "/dev/vbd$((DISK_BASE + $1))"
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

# Detach disk $1 (index) — vbd disks stay present on the bus,
# so the FS just stops talking to them via the failed flag.
detach_disk() {
    :
}

# Prepare disk $1 as a fresh replacement for resilver — zero the
# first 64 MB (one HAMMER2 reserved-zone segment) to wipe the
# header copies the kernel scans on attach.
fresh_disk() {
    local idx="$1"
    dd if=/dev/zero of=/dev/vbd$((DISK_BASE + idx)) bs=65536 count=1024 \
        2>/dev/null || true
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
    local newfs_ok=0
    local attempt=0
    while [ "$attempt" -lt 5 ]; do
        # shellcheck disable=SC2086
        if newfs_hammer2 -R 6 -L V4TEST $DEVS > /dev/null 2>&1; then
            newfs_ok=1
            break
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    if [ "$newfs_ok" = "0" ]; then
        echo "  FATAL: newfs_hammer2 failed after 5 attempts in setup_fresh"
        # shellcheck disable=SC2086
        newfs_hammer2 -R 6 -L V4TEST $DEVS 2>&1 | head -5
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
}

write_ref_data() {
    local prefix="${1:-ref}"
    dd if=/dev/urandom of=$MNTPT/${prefix}_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/${prefix}_b bs=65536 count=64 2>/dev/null
    sha256 $MNTPT/${prefix}_a > /var/tmp/v4_${prefix}.txt
    sha256 $MNTPT/${prefix}_b >> /var/tmp/v4_${prefix}.txt
    sync; sync
}

verify_ref() {
    local label="$1"
    local prefix="${2:-ref}"
    sha256 $MNTPT/${prefix}_a > /var/tmp/v4_check.txt 2>&1
    sha256 $MNTPT/${prefix}_b >> /var/tmp/v4_check.txt 2>&1
    if diff -q /var/tmp/v4_${prefix}.txt /var/tmp/v4_check.txt \
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

# Version guard: check that the mounted filesystem is RAIDZ2-native (v3).
check_v4() {
    if hammer2 -s $MNTPT volume-list 2>/dev/null | grep -q "^version 3"; then
        return 0
    fi
    echo "SKIP: RAIDZ2-native (v3) format not detected; skipping test suite"
    exit 77
}
