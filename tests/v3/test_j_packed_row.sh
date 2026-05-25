#!/bin/sh
# Group J: 6C/6D packed-row tests.
# Exercises the variable-width-stripe wins specific to v3:
#   J1  Multi-file write packs N chains per row.  Freeing one chain
#       must not lose data of the other chains in the same row.
#   J2  Snapshot + free packed chains: snapshot must still read
#       intact even after the live tree drops every column.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

SNAP_MNT=/mnt/v3snap

echo "=== Group J: Packed Row (6C/6D, NDISKS=$NDISKS) ==="

snap_unmount_quiet() {
    umount "$SNAP_MNT" 2>/dev/null || umount -f "$SNAP_MNT" 2>/dev/null || true
}
trap 'snap_unmount_quiet' EXIT INT TERM

hash_of() {
    sha256 "$1" 2>/dev/null | awk '{print $NF}'
}

# ----------------------------------------------------------------
# J1: write N small files in one TXG → likely packed.  Delete some,
# remount, verify survivors.
# ----------------------------------------------------------------
setup_fresh
check_v3

# Write ndata*2 distinct small files in one batch so several should
# share rows under the 6C packer.
NDATA=$((NDISKS - 2))
NFILES=$((NDATA * 4))

i=0
while [ "$i" -lt "$NFILES" ]; do
    dd if=/dev/urandom of=$MNTPT/pack_$i bs=65536 count=4 2>/dev/null
    i=$((i + 1))
done
sync; sync

# Snapshot the hashes
i=0
> /var/tmp/j1_pre.txt
while [ "$i" -lt "$NFILES" ]; do
    echo "$i $(hash_of $MNTPT/pack_$i)" >> /var/tmp/j1_pre.txt
    i=$((i + 1))
done

# Delete every other file — the survivors and their rows must remain
# intact even though their rows had packed siblings that are now freed.
i=0
while [ "$i" -lt "$NFILES" ]; do
    if [ $((i % 2)) -eq 1 ]; then
        rm -f $MNTPT/pack_$i
    fi
    i=$((i + 1))
done
sync; sync

umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT

# Verify surviving files match their pre-deletion hashes
i=0
ok=1
while [ "$i" -lt "$NFILES" ]; do
    if [ $((i % 2)) -eq 0 ]; then
        want=$(awk -v k="$i" '$1==k{print $2}' /var/tmp/j1_pre.txt)
        got=$(hash_of $MNTPT/pack_$i)
        if [ "$want" != "$got" ]; then
            echo "    mismatch on pack_$i: want=$want got=$got"
            ok=0
        fi
    fi
    i=$((i + 1))
done
if [ "$ok" = "1" ]; then
    result PASS "J1: survivors intact after packed-row sibling deletes"
else
    result FAIL "J1: at least one survivor diverged"
fi
teardown "J1"

# ----------------------------------------------------------------
# J2: pack a row, snapshot, delete the live chains, snapshot must
# still read.  Tests refcount keeps the row from being freed while
# the snapshot still references it.
# ----------------------------------------------------------------
setup_fresh
check_v3

i=0
while [ "$i" -lt "$NDATA" ]; do
    dd if=/dev/urandom of=$MNTPT/snap_pack_$i bs=65536 count=4 2>/dev/null
    i=$((i + 1))
done
sync; sync

i=0
> /var/tmp/j2_pre.txt
while [ "$i" -lt "$NDATA" ]; do
    echo "$i $(hash_of $MNTPT/snap_pack_$i)" >> /var/tmp/j2_pre.txt
    i=$((i + 1))
done

J2_LABEL="j2snap"
if ! hammer2 -s $MNTPT snapshot $MNTPT "$J2_LABEL" \
        > /var/tmp/j2_snap.out 2>&1; then
    result FAIL "J2: snapshot create failed"
    cat /var/tmp/j2_snap.out
    teardown "J2"
else
    # Delete every live pack file
    i=0
    while [ "$i" -lt "$NDATA" ]; do
        rm -f $MNTPT/snap_pack_$i
        i=$((i + 1))
    done
    sync; sync

    # Mount snapshot and read every file
    mkdir -p "$SNAP_MNT"
    snap_unmount_quiet
    if mount -t hammer2 "${DEVSPEC}@${J2_LABEL}" "$SNAP_MNT" \
            2>/dev/null; then
        i=0
        ok=1
        while [ "$i" -lt "$NDATA" ]; do
            want=$(awk -v k="$i" '$1==k{print $2}' /var/tmp/j2_pre.txt)
            got=$(hash_of $SNAP_MNT/snap_pack_$i)
            if [ "$want" != "$got" ]; then
                echo "    snap mismatch on snap_pack_$i: want=$want got=$got"
                ok=0
            fi
            i=$((i + 1))
        done
        if [ "$ok" = "1" ]; then
            result PASS "J2: snapshot intact after live-tree deletes packed row"
        else
            result FAIL "J2: snapshot lost data after live deletes"
        fi
        snap_unmount_quiet
    else
        result FAIL "J2: failed to mount snapshot ${J2_LABEL}"
    fi
    teardown "J2"
fi

summary
