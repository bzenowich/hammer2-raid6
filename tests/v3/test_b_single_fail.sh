#!/bin/sh
# Group B: Single disk failure — degraded reads for each disk position
# and degraded writes.
#
# Every read here happens after a remount.  Reading right after
# fail-disk is served from the buffer cache and never exercises
# reconstruction (verified: such a read still "passes" with three of
# four columns destroyed).
#
#   B1..BN  fail disk N, unmount, overwrite its stripe data zone with
#           garbage, remount with the full spec.  The data must come
#           back correct and the failed disk must not be read (its
#           cksum_err stays 0) — reconstruction, not the stale column.
#   B(N+1)  remount with disk 2 ABSENT from the spec, write while
#           degraded, remount again, verify old and new data cold.
#   B(N+2)  same array, also corrupt disk N-1's data zone: every read
#           of a disk N-1 column is now one absent + one bad column,
#           so it needs a P+Q dual reconstruction.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group B: Single Disk Failure (NDISKS=$NDISKS) ==="

DISK=0
while [ "$DISK" -lt "$NDISKS" ]; do
    LABEL="B$((DISK+1))"
    setup_fresh
    check_v3
    write_ref_data "ref"

    if fail_disk "$DISK" "$LABEL"; then
        sync
        umount $MNTPT
        corrupt_data_zone "$DISK"
        if mount -t hammer2 $PFSPATH $MNTPT; then
            verify_ref "$LABEL: cold degraded read, disk${DISK} failed" "ref"
            ERR=$(disk_counter "$DISK" cksum_err)
            if [ "${ERR:-x}" = "0" ]; then
                result PASS "$LABEL: failed disk${DISK} not read"
            else
                result FAIL "$LABEL: failed disk${DISK} was read (cksum_err=$ERR)"
            fi
        else
            result FAIL "$LABEL: remount with disk${DISK} failed refused"
        fi
    fi
    teardown "$LABEL"
    DISK=$((DISK + 1))
done

# Degraded write with the disk absent from the mount spec.
ABSENT=2
SECOND=$((NDISKS - 1))
BW="B$((NDISKS+1))"
BD="B$((NDISKS+2))"
setup_fresh
check_v3
write_ref_data "ref"
umount $MNTPT
DEGRADED="$(degraded_spec $ABSENT)@V3TEST"
if mount -t hammer2 "$DEGRADED" $MNTPT; then
    write_ref_data "dw"
    if remount "$DEGRADED"; then
        verify_ref "$BW: pre-failure data, cold, disk${ABSENT} absent" "ref"
        verify_ref "$BW: degraded-written data, cold, disk${ABSENT} absent" "dw"
        umount $MNTPT

        corrupt_data_zone "$SECOND"
        if mount -t hammer2 "$DEGRADED" $MNTPT; then
            verify_ref "$BD: pre-failure data, disk${ABSENT} absent + disk${SECOND} corrupt" "ref"
            verify_ref "$BD: degraded-written data, disk${ABSENT} absent + disk${SECOND} corrupt" "dw"
            HEAL=$(disk_counter "$SECOND" healed)
            if [ "${HEAL:-0}" -gt 0 ]; then
                result PASS "$BD: dual reconstruction healed disk${SECOND} ($HEAL)"
            else
                result FAIL "$BD: no heals recorded on disk${SECOND} (corruption not read?)"
            fi
        else
            result FAIL "$BD: degraded mount refused"
        fi
    else
        result FAIL "$BW: degraded remount refused"
    fi
else
    result FAIL "$BW: degraded mount without disk${ABSENT} refused"
fi
# The healed CHECK FAILs on disk SECOND are expected; the data
# assertions above are what decide this test.
kmsg_clear
teardown "$BD"

summary
