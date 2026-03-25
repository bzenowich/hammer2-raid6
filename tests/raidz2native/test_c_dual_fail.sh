#!/bin/sh
# Group C: Dual disk failure — all 15 unique pairs from a 6-disk array.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group C: Dual Disk Failure (15 pairs) ==="

# All 15 unique pairs of disks from {0,1,2,3,4,5}
PAIRS="0,1 0,2 0,3 0,4 0,5 1,2 1,3 1,4 1,5 2,3 2,4 2,5 3,4 3,5 4,5"

N=0
for PAIR in $PAIRS; do
    N=$((N + 1))
    A=$(echo $PAIR | cut -d, -f1)
    B=$(echo $PAIR | cut -d, -f2)
    LABEL=$(printf "C%02d" $N)

    setup_fresh
    check_v4
    write_ref_data "ref"

    hammer2 -s $MNTPT raid fail-disk /dev/vn${A} > /dev/null 2>&1
    hammer2 -s $MNTPT raid fail-disk /dev/vn${B} > /dev/null 2>&1
    vnconfig -u vn${A} 2>/dev/null || true
    vnconfig -u vn${B} 2>/dev/null || true

    verify_ref "${LABEL}: dual-fail vn${A}+vn${B}" "ref"
    teardown "${LABEL}"
done

summary
