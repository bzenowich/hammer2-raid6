#!/bin/sh
# Group C: Dual disk failure — all unique pairs from an NDISKS-disk array.

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

        hammer2 -s $MNTPT raid fail-disk "$(disk_dev $A)" > /dev/null 2>&1
        hammer2 -s $MNTPT raid fail-disk "$(disk_dev $B)" > /dev/null 2>&1
        detach_disk "$A"
        detach_disk "$B"

        verify_ref "${LABEL}: dual-fail disk${A}+disk${B}" "ref"
        teardown "${LABEL}"
        B=$((B + 1))
    done
    A=$((A + 1))
done

summary
