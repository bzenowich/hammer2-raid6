#!/bin/sh
# Group H: Snapshot semantics across degraded states.
# Closes the Phase 2 exit gate item "Snapshot create + modify + restore
# passes across degraded states" (newplan.md §7 Phase 2 Exit).
#
# Tests:
#   H1  snapshot under healthy -> fail a disk -> modify live tree ->
#       read snapshot via independent mount -> snapshot content
#       must match the pre-failure state.  Live tree must reflect
#       the modification.
#   H2  snapshot under degraded -> remount -> read snapshot ->
#       content matches what was on disk at snapshot time.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

SNAP_MNT=/mnt/v3snap

echo "=== Group H: Snapshot across degraded states (NDISKS=$NDISKS) ==="

snap_unmount_quiet() {
    umount "$SNAP_MNT" 2>/dev/null || umount -f "$SNAP_MNT" 2>/dev/null || true
}

# Always release the snapshot mount on exit.
trap 'snap_unmount_quiet' EXIT INT TERM

# ----------------------------------------------------------------
# H1: snapshot healthy, degrade, modify live, verify snapshot intact.
# ----------------------------------------------------------------
setup_fresh
check_v3

# sha256 prints "SHA256 (path) = hex"; the path column differs between
# the live mount and the snapshot mount even when the data is identical,
# so reduce every capture to just the hex digest before diffing.
hash_of() {
    sha256 "$1" 2>/dev/null | awk '{print $NF}'
}

# Pre-snapshot content
dd if=/dev/urandom of=$MNTPT/payload bs=65536 count=64 2>/dev/null
hash_of $MNTPT/payload > /var/tmp/h1_pre.txt
sync; sync

# Take snapshot under healthy
SNAP_LABEL="h1snap"
if ! hammer2 -s $MNTPT snapshot $MNTPT "$SNAP_LABEL" > /var/tmp/h1_snap.out 2>&1; then
    result FAIL "H1: snapshot creation failed (see /var/tmp/h1_snap.out)"
    teardown "H1"
else
    # Fail a disk (disk 2: stable data column for NDISKS>=4)
    H1_FAIL=2
    [ "$H1_FAIL" -ge "$NDISKS" ] && H1_FAIL=$((NDISKS - 1))
    hammer2 -s $MNTPT raid fail-disk "$(disk_dev $H1_FAIL)" > /dev/null 2>&1
    sync; sync

    # Modify live tree under degraded
    dd if=/dev/urandom of=$MNTPT/payload bs=65536 count=64 2>/dev/null
    hash_of $MNTPT/payload > /var/tmp/h1_post.txt
    sync; sync

    # Live tree must reflect the modification
    if diff -q /var/tmp/h1_pre.txt /var/tmp/h1_post.txt > /dev/null 2>&1; then
        result FAIL "H1: live tree unchanged after rewrite — COW did not fire"
    else
        result PASS "H1: live tree reflects degraded-mode rewrite"
    fi

    # Mount the snapshot independently and verify pre-failure content.
    mkdir -p "$SNAP_MNT"
    snap_unmount_quiet
    if mount -t hammer2 "${DEVSPEC}@${SNAP_LABEL}" "$SNAP_MNT" 2>/dev/null; then
        if [ -f "$SNAP_MNT/payload" ]; then
            hash_of "$SNAP_MNT/payload" > /var/tmp/h1_snap_read.txt
            if diff -q /var/tmp/h1_pre.txt /var/tmp/h1_snap_read.txt \
                    > /dev/null 2>&1; then
                result PASS "H1: snapshot preserves pre-failure content"
            else
                result FAIL "H1: snapshot content diverged from pre-failure state"
            fi
        else
            result FAIL "H1: snapshot mounted but payload file missing"
        fi
        snap_unmount_quiet
    else
        result FAIL "H1: failed to mount snapshot ${SNAP_LABEL} under degraded"
    fi

    teardown "H1"
fi

# ----------------------------------------------------------------
# H2: snapshot taken WHILE degraded, then remount and verify.
# ----------------------------------------------------------------
setup_fresh
check_v3

dd if=/dev/urandom of=$MNTPT/dpayload bs=65536 count=64 2>/dev/null
hash_of $MNTPT/dpayload > /var/tmp/h2_pre.txt
sync; sync

# Degrade BEFORE the snapshot
H2_FAIL=1
hammer2 -s $MNTPT raid fail-disk "$(disk_dev $H2_FAIL)" > /dev/null 2>&1
sync; sync

H2_LABEL="h2snap"
if ! hammer2 -s $MNTPT snapshot $MNTPT "$H2_LABEL" > /var/tmp/h2_snap.out 2>&1; then
    result FAIL "H2: snapshot under degraded failed"
    teardown "H2"
else
    result PASS "H2: snapshot creation succeeded under degraded"

    # Modify after the snapshot so we can distinguish snapshot vs live.
    dd if=/dev/urandom of=$MNTPT/dpayload bs=65536 count=64 2>/dev/null
    sync; sync

    # Cycle the live mount so caches don't fake a pass.
    umount $MNTPT
    DEGRADED_SPEC="$(degraded_spec $H2_FAIL)@V3TEST"
    if mount -t hammer2 "$DEGRADED_SPEC" $MNTPT 2>/dev/null; then
        # Mount the degraded-time snapshot
        snap_unmount_quiet
        DEGRADED_SNAP_SPEC="$(degraded_spec $H2_FAIL)@${H2_LABEL}"
        if mount -t hammer2 "$DEGRADED_SNAP_SPEC" "$SNAP_MNT" 2>/dev/null; then
            hash_of "$SNAP_MNT/dpayload" > /var/tmp/h2_snap_read.txt
            if diff -q /var/tmp/h2_pre.txt /var/tmp/h2_snap_read.txt \
                    > /dev/null 2>&1; then
                result PASS "H2: degraded-time snapshot content correct"
            else
                result FAIL "H2: degraded-time snapshot content diverged"
            fi
            snap_unmount_quiet
        else
            result FAIL "H2: failed to mount degraded-time snapshot"
        fi
    else
        result FAIL "H2: failed to remount live tree in degraded mode"
    fi
    teardown "H2"
fi

summary
