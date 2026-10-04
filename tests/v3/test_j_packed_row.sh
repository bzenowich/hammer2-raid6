#!/bin/sh
# Group J: 6C/6D packed-row tests.
# Exercises the variable-width-stripe wins specific to v3:
#   J1  Multi-file write packs N chains per row.  Delete every other
#       file, bulkfree (rm alone frees nothing — space is only returned
#       by bulkfree), then write new files so the freed slots are
#       reallocated next to the surviving siblings.  Survivors and new
#       files must verify cold and the array must scrub clean.
#   J2  Snapshot + delete the live copies + bulkfree + new writes: the
#       snapshot still references the rows, so bulkfree must not hand
#       them out again; the snapshot must read intact cold.

# Fill freed space with fresh data and record hashes: fill_new <prefix> <n>
fill_new() {
    local k=0
    > /var/tmp/j_new_$1.txt
    while [ "$k" -lt "$2" ]; do
        dd if=/dev/urandom of=$MNTPT/$1_$k bs=65536 count=4 2>/dev/null
        echo "$k $(sha256 -q $MNTPT/$1_$k)" >> /var/tmp/j_new_$1.txt
        k=$((k + 1))
    done
    sync
}

# check_new <prefix> <dir> <label>
check_new() {
    local bad=0 k want
    while read k want; do
        [ "$(sha256 -q $2/$1_$k 2>/dev/null)" = "$want" ] || bad=$((bad + 1))
    done < /var/tmp/j_new_$1.txt
    if [ "$bad" = "0" ]; then
        result PASS "$3"
    else
        result FAIL "$3 ($bad files wrong)"
    fi
}

bulkfree_ok() {
    if hammer2 bulkfree $MNTPT > /var/tmp/j_bulkfree.txt 2>&1; then
        return 0
    fi
    result FAIL "$1: bulkfree failed"
    sed 's/^/      /' /var/tmp/j_bulkfree.txt
    return 1
}

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
bulkfree_ok "J1"
fill_new j1new $((NFILES / 2))

remount

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
check_new j1new $MNTPT "J1: files written into freed slots intact (cold)"
scrub_clean "J1"
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
    bulkfree_ok "J2"
    fill_new j2new $((NDATA * 4))
    remount

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
    check_new j2new $MNTPT "J2: post-bulkfree writes intact (cold)"
    scrub_clean "J2"
    teardown "J2"
fi

summary
