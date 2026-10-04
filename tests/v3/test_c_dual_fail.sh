#!/bin/sh
# Group C: Dual disk failure — all unique pairs from an NDISKS-disk array
# (the mdadm 01raid6integ shape: every pair, data compared each time).
#
# For each pair: fail both disks, unmount, overwrite both disks' stripe
# data zones with garbage, remount with the full spec, read everything.
# Correct data can only come from P+Q dual reconstruction.
#
# This used to read right after fail-disk without remounting.  Those
# reads were served from the buffer cache and passed even with three of
# four columns destroyed, so the group never exercised dual
# reconstruction.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

NPAIRS=$(( NDISKS * (NDISKS - 1) / 2 ))
echo "=== Group C: Dual Disk Failure ($NPAIRS pairs, NDISKS=$NDISKS) ==="

N=0
A=0
while [ "$A" -lt "$NDISKS" ]; do
    B=$((A + 1))
    while [ "$B" -lt "$NDISKS" ]; do
        N=$((N + 1))
        LABEL=$(printf "C%02d" $N)

        setup_fresh
        check_v3
        write_ref_data "ref"

        if fail_disk "$A" "$LABEL" && fail_disk "$B" "$LABEL"; then
            sync
            umount $MNTPT
            corrupt_data_zone "$A"
            corrupt_data_zone "$B"
            if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
                verify_ref "${LABEL}: cold read, disk${A}+disk${B} failed" "ref"
            else
                result FAIL "${LABEL}: array with disk${A}+disk${B} failed will not mount"
                kmsg | grep "v3 quorum" | tail -1 | sed 's/^/      /'
            fi
        fi
        teardown "${LABEL}"
        B=$((B + 1))
    done
    A=$((A + 1))
done

summary
