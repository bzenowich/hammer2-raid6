#!/bin/sh
# Group N: capacity and freeing (docs/capacity.md).
#   N1  Fill past the old limit (2 GB per disk x ndata): every file must
#       verify cold and df must count it.
#   N2  Delete most of it.  One bulkfree pass only stages (df unchanged),
#       the second frees (df shows the space back).  Then write more
#       than would fit without reuse: the new files and the survivors
#       must verify cold and the array scrub clean.
#   N3  Snapshot a file, overwrite it, then churn a scratch file with
#       bulkfree between rounds until the allocator has wrapped the disk
#       several times.  The snapshot must read back intact cold: its
#       rows are referenced and must never be freed.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

SNAP_MNT=/mnt/v3snap
CHUNK=/var/tmp/n_chunk

echo "=== Group N: Capacity and freeing (NDISKS=$NDISKS) ==="

snap_unmount_quiet() {
    umount "$SNAP_MNT" 2>/dev/null || umount -f "$SNAP_MNT" 2>/dev/null || true
}
trap 'snap_unmount_quiet; rm -f $CHUNK' EXIT INT TERM

used_mb() {
    df -m $MNTPT | awk 'NR == 2 { print $3 }'
}

# bulkfree_n <count> <label>: run <count> bulkfree passes.
bulkfree_n() {
    local k=0
    while [ "$k" -lt "$1" ]; do
        if ! hammer2 bulkfree $MNTPT > /var/tmp/n_bulkfree.txt 2>&1; then
            result FAIL "$2: bulkfree failed"
            sed 's/^/      /' /var/tmp/n_bulkfree.txt
            return 1
        fi
        k=$((k + 1))
    done
    sleep 2     # statfs recounts used slots at most once a second
}

# The old limit is 2 GB per disk of data, ndata disks.
NDATA=$((NDISKS - 2))
OLD_LIMIT_MB=$((2048 * NDATA))
NFILL=$((OLD_LIMIT_MB / 64 + 8))

dd if=/dev/urandom of=$CHUNK bs=1m count=64 2>/dev/null
CH=$(sha256 -q $CHUNK)

# ----------------------------------------------------------------
# N1: fill past the old limit
# ----------------------------------------------------------------
setup_fresh
check_v3

i=0
while [ "$i" -lt "$NFILL" ]; do
    cp $CHUNK $MNTPT/f$i || break
    i=$((i + 1))
done
sync
if [ "$i" = "$NFILL" ]; then
    result PASS "N1: wrote $NFILL x 64 MB (old limit ${OLD_LIMIT_MB} MB)"
else
    result FAIL "N1: write failed at file $i of $NFILL"
fi
u=$(used_mb)
if [ "$u" -ge $((NFILL * 64)) ]; then
    result PASS "N1: df counts the data (${u} MB used)"
else
    result FAIL "N1: df shows ${u} MB used after $((NFILL * 64)) MB written"
fi
remount
bad=0; j=0
while [ "$j" -lt "$NFILL" ]; do
    [ "$(sha256 -q $MNTPT/f$j 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
    j=$((j + 1))
done
if [ "$bad" = "0" ]; then
    result PASS "N1: all $NFILL files intact (cold)"
else
    result FAIL "N1: $bad of $NFILL files wrong (cold)"
fi

# ----------------------------------------------------------------
# N2: delete, two-stage bulkfree, reuse
# ----------------------------------------------------------------
j=0
while [ "$j" -lt "$NFILL" ]; do
    [ $((j % 4)) -eq 0 ] || rm -f $MNTPT/f$j
    j=$((j + 1))
done
sync; sync
ndel=$((NFILL - (NFILL + 3) / 4))
u0=$(used_mb)
bulkfree_n 1 "N2"
u1=$(used_mb)
if [ "$u1" -ge $((u0 - 64)) ]; then
    result PASS "N2: first bulkfree pass only stages (${u0} -> ${u1} MB)"
else
    result FAIL "N2: first bulkfree pass freed space (${u0} -> ${u1} MB)"
fi
bulkfree_n 1 "N2"
u2=$(used_mb)
if [ $((u1 - u2)) -ge $((ndel * 64 * 9 / 10)) ]; then
    result PASS "N2: second pass frees the deleted files (${u1} -> ${u2} MB)"
else
    result FAIL "N2: second pass freed too little (${u1} -> ${u2} MB, $ndel x 64 MB deleted)"
fi

# Write more than the space left before the frees: needs the freed rows.
avail0=$(df -m $MNTPT | awk 'NR == 2 { print $4 }')
nnew=$(( (ndel * 64 + 512) / 64 ))
k=0
while [ "$k" -lt "$nnew" ]; do
    cp $CHUNK $MNTPT/g$k || break
    k=$((k + 1))
done
sync
if [ "$k" = "$nnew" ]; then
    result PASS "N2: wrote $nnew x 64 MB into freed space (${avail0} MB avail)"
else
    result FAIL "N2: write failed at file $k of $nnew (${avail0} MB avail)"
fi
remount
bad=0; j=0
while [ "$j" -lt "$NFILL" ]; do
    if [ $((j % 4)) -eq 0 ]; then
        [ "$(sha256 -q $MNTPT/f$j 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
    fi
    j=$((j + 1))
done
j=0
while [ "$j" -lt "$k" ]; do
    [ "$(sha256 -q $MNTPT/g$j 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
    j=$((j + 1))
done
if [ "$bad" = "0" ]; then
    result PASS "N2: survivors and new files intact (cold)"
else
    result FAIL "N2: $bad files wrong after reuse (cold)"
fi
scrub_clean "N2"
teardown "N2"

# ----------------------------------------------------------------
# N3: snapshot survives allocator wrap with bulkfree running
# ----------------------------------------------------------------
setup_fresh
check_v3

dd if=/dev/urandom of=$MNTPT/keep bs=1m count=128 2>/dev/null
SH=$(sha256 -q $MNTPT/keep)
sync
if ! hammer2 -s $MNTPT snapshot $MNTPT N3SNAP > /var/tmp/n3_snap.out 2>&1; then
    result FAIL "N3: snapshot create failed"
    cat /var/tmp/n3_snap.out
    teardown "N3"
    summary
    exit
fi
sync
dd if=/dev/urandom of=$MNTPT/keep bs=1m count=128 conv=notrunc 2>/dev/null
LH=$(sha256 -q $MNTPT/keep)
sync

# Total churn: three times the array's data capacity.
cap=$(df -m $MNTPT | awk 'NR == 2 { print $2 }')
rounds=$(( (cap * 3 + 511) / 512 ))
dd if=/dev/urandom of=$CHUNK bs=1m count=512 2>/dev/null
r=0; werr=0
while [ "$r" -lt "$rounds" ]; do
    dd if=$CHUNK of=$MNTPT/scratch bs=1m conv=notrunc 2>/dev/null || werr=1
    sync
    [ "$werr" = "0" ] || break
    bulkfree_n 1 "N3" || break
    r=$((r + 1))
done
if [ "$werr" = "0" ] && [ "$r" = "$rounds" ]; then
    result PASS "N3: churned $rounds x 512 MB through a ${cap} MB array"
else
    result FAIL "N3: churn write failed in round $r of $rounds"
fi
remount
[ "$(sha256 -q $MNTPT/keep 2>/dev/null)" = "$LH" ] &&
    result PASS "N3: live file intact (cold)" ||
    result FAIL "N3: live file wrong (cold)"
mkdir -p "$SNAP_MNT"
snap_unmount_quiet
if mount -t hammer2 "${DEVSPEC}@N3SNAP" "$SNAP_MNT"; then
    if [ "$(sha256 -q $SNAP_MNT/keep 2>/dev/null)" = "$SH" ]; then
        result PASS "N3: snapshot intact after allocator wrap (cold)"
    else
        result FAIL "N3: snapshot data lost after allocator wrap"
    fi
    snap_unmount_quiet
else
    result FAIL "N3: snapshot mount failed"
fi
scrub_clean "N3"
teardown "N3"

summary
