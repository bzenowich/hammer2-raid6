#!/bin/sh
# common.sh — shared helpers for tests/perf.  Runs on the VM.

NDISKS="${NDISKS:-4}"
MNTPT="${MNTPT:-/mnt/perf}"
LABEL="${LABEL:-PERF}"
RESULTS="${RESULTS:-/var/tmp/perf_results}"

DEVS=""
DEVSPEC=""
i=0
while [ "$i" -lt "$NDISKS" ]; do
    dev="/dev/vbd${i}"
    DEVS="${DEVS} ${dev}"
    DEVSPEC="${DEVSPEC:+${DEVSPEC}:}${dev}"
    i=$((i + 1))
done
PFSPATH_V4="${DEVSPEC}@${LABEL}"
PFSPATH_1D="/dev/vbd0@${LABEL}"

die() { echo "FATAL: $*" >&2; exit 1; }

require_fio() {
    command -v fio >/dev/null 2>&1 || die "fio not installed (see README.md)"
}

unmount_quiet() {
    umount "$MNTPT" 2>/dev/null || umount -f "$MNTPT" 2>/dev/null || true
}

zap_disks() {
    # Wipe HAMMER2 reserved zone on every disk so newfs is clean.
    local j=0
    while [ "$j" -lt "$NDISKS" ]; do
        dd if=/dev/zero of=/dev/vbd${j} bs=65536 count=1024 \
            >/dev/null 2>&1 || true
        j=$((j + 1))
    done
}

setup_h2_1disk() {
    unmount_quiet
    zap_disks
    newfs_hammer2 -L "$LABEL" /dev/vbd0 >/dev/null || die "newfs 1disk failed"
    mkdir -p "$MNTPT"
    mount -t hammer2 "$PFSPATH_1D" "$MNTPT" || die "mount 1disk failed"
}

setup_v4_healthy() {
    unmount_quiet
    zap_disks
    # shellcheck disable=SC2086
    newfs_hammer2 -R 6 -L "$LABEL" $DEVS >/dev/null || die "newfs v4 failed"
    mkdir -p "$MNTPT"
    mount -t hammer2 "$PFSPATH_V4" "$MNTPT" || die "mount v4 failed"
}

setup_v4_degraded() {
    setup_v4_healthy
    # Fail one data column (index 2).  Index 0 and N-1 are typically
    # P/Q for stripe 0; idx 2 is a stable data column for NDISKS>=4.
    hammer2 -s "$MNTPT" raid fail-disk /dev/vbd2 >/dev/null 2>&1 \
        || die "raid fail-disk failed"
}

teardown_config() {
    sync; sync
    unmount_quiet
}

# Drop caches between runs by remounting (DragonFly has no
# vfs.vmiodirenable knob equivalent we can rely on for hammer2).
remount_cycle() {
    local pfs="$1"
    sync; sync
    umount "$MNTPT" || return 1
    mount -t hammer2 "$pfs" "$MNTPT" || return 1
}

snapshot_sysctl() {
    local tag="$1"
    sysctl vfs.hammer2 > "${RESULTS}/sysctl_${tag}.txt" 2>/dev/null || true
}

snapshot_iostat() {
    local tag="$1"
    iostat -x 1 2 > "${RESULTS}/iostat_${tag}.txt" 2>/dev/null || true
}

dmesg_checkfails() {
    dmesg | grep -c "CHECK FAIL" 2>/dev/null || echo 0
}
