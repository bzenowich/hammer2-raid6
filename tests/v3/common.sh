#!/bin/sh
# common.sh — shared helpers for v3 (RAIDZ2-native) integration tests.
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
# DISK_PREFIX: device name prefix (default /dev/vbd).  On real hardware,
# e.g. the Pi's USB disks: DISK_PREFIX=/dev/da DISK_BASE=0.

NDISKS="${NDISKS:-4}"
DISK_BASE="${DISK_BASE:-1}"
DISK_PREFIX="${DISK_PREFIX:-/dev/vbd}"
MNTPT=/mnt/v3test
PASS=0; FAIL=0; TOTAL=0; ERRORS=""

# Build device list and colon-separated DEVSPEC
DEVS=""
DEVSPEC=""
i=0
while [ "$i" -lt "$NDISKS" ]; do
    dev="${DISK_PREFIX}$((DISK_BASE + i))"
    # Never newfs a disk something has mounted (the system disk, say).
    if mount | grep -v "@V3TEST " | grep -q "^${dev}[^0-9]"; then
        echo "common.sh: ${dev} is mounted; refusing to use it" >&2
        exit 1
    fi
    DEVS="${DEVS} ${dev}"
    if [ -z "$DEVSPEC" ]; then
        DEVSPEC="${dev}"
    else
        DEVSPEC="${DEVSPEC}:${dev}"
    fi
    i=$((i + 1))
done
PFSPATH="${DEVSPEC}@V3TEST"

# Return the device path for disk index $1 (logical 0..NDISKS-1)
disk_dev() {
    echo "${DISK_PREFIX}$((DISK_BASE + $1))"
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

# Layout of the 4 GB test disks (docs/capacity.md): reserved segment
# 0..4 MB, space map 4..12 MB, aux 12..20 MB, metadata extent 0
# 20..220 MB, stripe data from 220 MB (64 KB block 3520).
DATA_BLK=3520

# Prepare disk $1 as a fresh replacement for resilver.  Zero the first
# 512 MB: the reserved/header segment, the space map, metadata extent 0
# and the start of the stripe data zone.  Zeroing
# only the header segment left every old column in place, so a resilver
# that wrote nothing still read back correct data.
fresh_disk() {
    local idx="$1"
    dd if=/dev/zero of=$(disk_dev "$idx") bs=65536 count=8192 \
        2>/dev/null || true
}

# Overwrite $3 x 64 KB of disk $1 starting at 64 KB block $2 with
# random data (unmounted-media corruption).
corrupt_disk() {
    dd if=/dev/urandom of=$(disk_dev "$1") bs=65536 count="$3" seek="$2" \
        conv=notrunc 2>/dev/null
}

# Corrupt the first 100 MB of the stripe data zone of disk $1.
corrupt_data_zone() {
    corrupt_disk "$1" $DATA_BLK 1600
}

# fail_disk <idx> <label>: mark disk failed and verify the ioctl took
# effect.  Records a FAIL and returns 1 otherwise, so a test never runs
# its "degraded" checks against a healthy array.
fail_disk() {
    local idx="$1"
    local label="$2"
    if ! hammer2 -s $MNTPT raid fail-disk "$(disk_dev "$idx")" \
            > /dev/null 2>&1; then
        result FAIL "$label: fail-disk $idx returned error"
        return 1
    fi
    if ! disk_state_is "$idx" FAILED; then
        result FAIL "$label: disk $idx not FAILED after fail-disk"
        return 1
    fi
    return 0
}

# disk_state_is <idx> <STATE>: per-disk state from `raid status`.
disk_state_is() {
    hammer2 -s $MNTPT raid status 2>/dev/null |
        grep -q "^disk\[$1\]:[[:space:]]*$2"
}

# disk_counter <idx> <field>: cksum_err / healed / unrepairable.
disk_counter() {
    hammer2 -s $MNTPT raid status 2>/dev/null |
        grep "^disk\[$1\]:" |
        awk -v f="$2" '{ for (i = 1; i < NF; i++) if ($i == f) { print $(i+1); exit } }'
}

# remount [spec]: unmount and mount again so later reads come from the
# media, not the buffer cache.  Reads made while the FS stays mounted
# after fail-disk or a corrupting dd are served from cache and prove
# nothing about reconstruction.  Default spec is the full array.
remount() {
    local spec="${1:-$PFSPATH}"
    sync
    umount $MNTPT || return 1
    mount -t hammer2 "$spec" $MNTPT
}

# scrub_clean <label>: run `raid scrub` and require zero bad and zero
# unrepairable brefs (the mdadm "check, mismatch_cnt == 0" analogue).
scrub_clean() {
    local label="$1"
    local out=/var/tmp/v4_scrub.txt
    hammer2 -s $MNTPT raid scrub > $out 2>&1
    local rc=$?
    local bad unrep done_
    bad=$(awk '$1 == "brefs_bad:" { print $2 }' $out)
    unrep=$(awk '$1 == "brefs_unrepairable:" { print $2 }' $out)
    done_=$(awk '$1 == "brefs_done:" { print $2 }' $out)
    if [ "$rc" = "0" ] && [ "${bad:-x}" = "0" ] && \
       [ "${unrep:-x}" = "0" ] && [ "${done_:-0}" -gt 0 ]; then
        result PASS "$label: scrub clean ($done_ brefs)"
    else
        result FAIL "$label: scrub rc=$rc done=$done_ bad=$bad unrep=$unrep"
        sed 's/^/      /' $out
    fi
}

# guarded <secs> <cmd...>: run cmd in the background and wait at most
# <secs>.  Returns cmd's status, or 124 if it is still running (a
# kernel hang leaves it in D state; it cannot be killed).  timeout(1)
# is unusable on DragonFly master (sigaction(32) failure).
guarded() {
    local secs="$1"
    shift
    "$@" &
    local pid=$! n=0
    while kill -0 $pid 2>/dev/null; do
        if [ "$n" -ge "$secs" ]; then
            echo "  HANG: '$*' still running after ${secs}s" >&2
            return 124
        fi
        sleep 1
        n=$((n + 1))
    done
    wait $pid
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

# kmsg: dmesg(8) that survives a busy kernel log.  dmesg sizes the
# buffer with one sysctl and reads it with a second; anything logged in
# between (the self-heal repair writes, say) makes the read fail with
# ENOMEM and print nothing, which turns every "dmesg | grep" into a
# silent miss.  Retry until a read succeeds.
kmsg() {
    local i=0
    while [ "$i" -lt 10 ]; do
        dmesg 2>/dev/null && return 0
        i=$((i + 1))
        sleep 1
    done
    echo "kmsg: dmesg failed 10 times" >&2
    return 1
}

# kmsg_clear: dmesg -c with the same retry.  A clear that loses the
# ENOMEM race leaves the previous test's CHECK FAIL lines in the buffer
# and the next teardown blames them on the wrong test.
kmsg_clear() {
    local i=0
    while [ "$i" -lt 10 ]; do
        dmesg -c > /dev/null 2>&1 && return 0
        i=$((i + 1))
        sleep 1
    done
    echo "kmsg_clear: dmesg -c failed 10 times" >&2
    return 1
}

check_no_checkfail() {
    local label="$1"
    local cfails
    cfails=$(kmsg | grep -c "CHECK FAIL" 2>/dev/null)
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
        if newfs_hammer2 -R 6 -L V3TEST $DEVS > /dev/null 2>&1; then
            newfs_ok=1
            break
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    if [ "$newfs_ok" = "0" ]; then
        echo "  FATAL: newfs_hammer2 failed after 5 attempts in setup_fresh"
        # shellcheck disable=SC2086
        newfs_hammer2 -R 6 -L V3TEST $DEVS 2>&1 | head -5
        exit 1
    fi
    mkdir -p $MNTPT
    if ! mount -t hammer2 $PFSPATH $MNTPT; then
        echo "  FATAL: mount failed in setup_fresh"
        exit 1
    fi
    kmsg_clear
}

teardown() {
    local label="$1"
    check_no_checkfail "$label"
    sync
    umount $MNTPT 2>/dev/null || umount -f $MNTPT 2>/dev/null || true
}

# data_slots — print "disk:data_off" for every DATA blockref on the array,
# sorted and unique.  Reads the media with `hammer2 -q show`, whose quiet
# lines are "data.N <data_off> <key>/<keybits> vol=<disk> ...": on a v3
# RAID6 array vol= is bref.copyid, the disk holding the block, and
# data_off is the per-disk offset with the size radix in its low 6 bits.
# Run after sync; an empty result means the walk found no data at all.
data_slots() {
    hammer2 -q show $DEVSPEC 2>/dev/null |
        awk '$1 ~ /^data\./ && $4 ~ /^vol=/ { sub(/^vol=/, "", $4); print $4 ":" $2 }' |
        sort -u
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
    if [ -s /var/tmp/v4_${prefix}.txt ] && \
       ! grep -q "No such\|rror" /var/tmp/v4_check.txt && \
       diff -q /var/tmp/v4_${prefix}.txt /var/tmp/v4_check.txt \
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
check_v3() {
    if hammer2 -s $MNTPT volume-list 2>/dev/null | grep -q "^version 3"; then
        return 0
    fi
    echo "SKIP: RAIDZ2-native (v3) format not detected; skipping test suite"
    exit 77
}
