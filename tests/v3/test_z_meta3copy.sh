#!/bin/sh
# Group Z: metadata on 3 copies (oct-review 12).
#
# A v3 metadata block has its primary on disk 0 and two mirrors on
# disks picked from the block number, 3 copies instead of one per disk.
# RAID6 survives any 2 failures, and so must the metadata.
#
#   Z1  placement: every metadata block that disk 0 holds is on exactly
#       3 disks (raw media compared), and the mirrors are spread over
#       disks 1..N-1
#   Z2  every pair of failed disks (their metadata zones wiped too):
#       mount degraded, every file reads cold, scrub clean
#   Z3  the primary and one mirror of every metadata block corrupt:
#       everything reads and scrubs, healed from the third copy
#   Z4  resilver each disk position, then fail two others: the copies
#       the resilver wrote are the ones that carry the data
#   Z5  a mirror disk fails, files are written (2 copies each), then a
#       second disk fails (the primary, then another mirror)
#   Z6  an array made with all-disk mirrors (layout 2, as before the
#       change) mounts, keeps all-disk mirrors, reads, and survives two
#       failures; converted with vfs.hammer2.md_3copy_upgrade it writes
#       3 copies and old and new files survive two failures
#
# Z6 needs newfs_hammer2 to honour HAMMER2_MKFS_LAYOUT2.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

NSCAN=256		# 64 KB blocks of the metadata extent examined
Z_FILES=/var/tmp/z_files.txt

echo "=== Group Z: Metadata on 3 copies (NDISKS=$NDISKS) ==="

# mk_tree <prefix>: directories of small files (inode, directory and
# freemap metadata) plus a few large ones.
mk_tree() {
    local p="$1" d f
    d=0
    while [ "$d" -lt 12 ]; do
        mkdir -p $MNTPT/${p}_d$d
        f=0
        while [ "$f" -lt 25 ]; do
            dd if=/dev/urandom of=$MNTPT/${p}_d$d/f$f bs=4096 count=1 \
                2>/dev/null
            f=$((f + 1))
        done
        d=$((d + 1))
    done
    dd if=/dev/urandom of=$MNTPT/${p}_big1 bs=65536 count=64 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/${p}_big2 bs=65536 count=40 2>/dev/null
}

# tree_sums <out>: checksum of every file in the tree, sorted by name.
tree_sums() {
    (cd $MNTPT && find . -type f | sort | xargs sha256) > "$1" 2>&1
}

# ref_tree <prefix>: build a tree and record its sums in $Z_FILES.
ref_tree() {
    mk_tree "$1"
    sync; sync
    tree_sums $Z_FILES
}

# check_tree <label>: guarded cold read of the whole tree against $Z_FILES
check_tree() {
    guarded 300 sh -c "cd $MNTPT && find . -type f | sort | xargs sha256 > /var/tmp/z_now.txt 2>&1"
    local rc=$?
    if [ "$rc" = 124 ]; then
        result FAIL "$1: read hung; reboot the guest"
        summary
        exit 1
    fi
    if [ -s $Z_FILES ] && ! grep -q "No such\|rror" /var/tmp/z_now.txt && \
       diff -q $Z_FILES /var/tmp/z_now.txt > /dev/null 2>&1; then
        result PASS "$1: every file reads correctly ($(wc -l < $Z_FILES | tr -d ' ') files)"
    else
        result FAIL "$1: tree differs or unreadable"
        diff $Z_FILES /var/tmp/z_now.txt | head -5 | sed 's/^/      /'
    fi
}

# unrepairable <label>: no disk counts an unrepairable block
no_unrepairable() {
    local d u tot=0
    d=0
    while [ "$d" -lt "$NDISKS" ]; do
        u=$(disk_counter $d unrepairable)
        tot=$((tot + ${u:-0}))
        d=$((d + 1))
    done
    if [ "$tot" = 0 ]; then
        result PASS "$1: no unrepairable blocks"
    else
        result FAIL "$1: $tot unrepairable block(s)"
    fi
}

# wipe_meta <idx>: destroy a failed disk's metadata and data zones
wipe_meta() {
    dd if=/dev/zero of=$(disk_dev "$1") bs=65536 seek=$MD_BLK \
        count=$((DATA_BLK - MD_BLK)) 2>/dev/null || true
    corrupt_data_zone "$1"
}

# holders: scan the first NSCAN blocks of the metadata extent on the
# unmounted array.  Writes "blk d d d" (the disks whose copy equals
# disk 0's) for each block disk 0 holds, to /var/tmp/z_holders.txt.
holders() {
    local b d zc c0 c h
    zc=$(dd if=/dev/zero bs=65536 count=1 2>/dev/null | cksum | awk '{ print $1 }')
    : > /var/tmp/z_holders.txt
    b=0
    while [ "$b" -lt "$NSCAN" ]; do
        c0=$(dd if=$(disk_dev 0) bs=65536 skip=$((MD_BLK + b)) count=1 2>/dev/null | cksum | awk '{ print $1 }')
        if [ "$c0" != "$zc" ]; then
            h=0
            d=1
            while [ "$d" -lt "$NDISKS" ]; do
                c=$(dd if=$(disk_dev $d) bs=65536 skip=$((MD_BLK + b)) count=1 2>/dev/null | cksum | awk '{ print $1 }')
                [ "$c" = "$c0" ] && h="$h $d"
                d=$((d + 1))
            done
            echo "$b $h" >> /var/tmp/z_holders.txt
        fi
        b=$((b + 1))
    done
}

# zero_heads: stale metadata of earlier arrays would look like copies
zero_heads() {
    local d
    for d in $DEVS; do
        dd if=/dev/zero of=$d bs=1m count=300 2>/dev/null || true
    done
}

# --- Z1 ---------------------------------------------------------------
zero_heads
setup_fresh
check_v3
ref_tree z1
umount $MNTPT
holders
NB=$(wc -l < /var/tmp/z_holders.txt | tr -d ' ')
# copies per block = words after the block number
# block 0 of the extent is written by newfs to every disk
Z1_BAD=$(awk 'NF != 4 && $1 != 0 { n++ } END { print n + 0 }' /var/tmp/z_holders.txt)
if [ "$NDISKS" -le 3 ]; then
    Z1_BAD=0
fi
if [ "$NB" -ge 8 ] && [ "$Z1_BAD" = 0 ]; then
    result PASS "Z1: $NB metadata blocks, each on exactly 3 disks"
else
    result FAIL "Z1: $NB blocks, $Z1_BAD not on exactly 3 disks"
    head -8 /var/tmp/z_holders.txt | sed 's/^/      /'
fi
# every disk 1..N-1 holds some mirrors
Z1_SPREAD=1
d=1
while [ "$d" -lt "$NDISKS" ]; do
    n=$(awk -v d=$d '{ for (i = 3; i <= NF; i++) if ($i == d) c++; if ($2 == d) c++ } END { print c + 0 }' /var/tmp/z_holders.txt)
    echo "    disk $d holds a copy of $n of $NB blocks"
    [ "$n" -gt 0 ] || Z1_SPREAD=0
    d=$((d + 1))
done
if [ "$Z1_SPREAD" = 1 ]; then
    result PASS "Z1: mirrors spread over disks 1..$((NDISKS - 1))"
else
    result FAIL "Z1: some disk holds no mirror"
fi
mount -t hammer2 $PFSPATH $MNTPT
check_tree "Z1"
teardown "Z1"

# --- Z2: every pair ---------------------------------------------------
N=0
A=0
while [ "$A" -lt "$NDISKS" ]; do
    B=$((A + 1))
    while [ "$B" -lt "$NDISKS" ]; do
        N=$((N + 1))
        LABEL=$(printf "Z2.%02d" $N)
        setup_fresh
        check_v3
        ref_tree z2
        if fail_disk "$A" "$LABEL" && fail_disk "$B" "$LABEL"; then
            sync
            umount $MNTPT
            wipe_meta "$A"
            wipe_meta "$B"
            if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
                check_tree "$LABEL disk${A}+disk${B} failed"
                scrub_clean "$LABEL disk${A}+disk${B} failed"
            else
                result FAIL "$LABEL: array with disk${A}+disk${B} failed will not mount"
            fi
        fi
        teardown "$LABEL"
        B=$((B + 1))
    done
    A=$((A + 1))
done

# --- Z3: primary + one mirror corrupt ---------------------------------
zero_heads
setup_fresh
check_v3
ref_tree z3
umount $MNTPT
holders
NB=$(wc -l < /var/tmp/z_holders.txt | tr -d ' ')
# disk 0 and, alternately, the first or the last mirror of each block
while read -r blk h0 m1 m2 rest; do
    corrupt_disk 0 $((MD_BLK + blk)) 1
    if [ $((blk % 2)) = 0 ]; then
        corrupt_disk "$m1" $((MD_BLK + blk)) 1
    else
        corrupt_disk "${m2:-$m1}" $((MD_BLK + blk)) 1
    fi
done < /var/tmp/z_holders.txt
kmsg_clear
if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
    check_tree "Z3: primary and one mirror of $NB blocks corrupt"
    scrub_clean "Z3"
    no_unrepairable "Z3"
    echo "    healed: $(for d in 0 1 2 3; do printf '%s ' $(disk_counter $d healed); done)"
    kmsg | grep "selfheal" | wc -l | sed 's/^/    selfheal log lines: /'
else
    result FAIL "Z3: mount refused with primary and one mirror corrupt"
fi
# the log carries the CHECK FAILs of the corrupt copies, expected here
kmsg_clear
teardown "Z3"

# --- Z4: resilver each position, then fail two others -----------------
D=0
while [ "$D" -lt "$NDISKS" ]; do
    LABEL="Z4.$D"
    setup_fresh
    check_v3
    ref_tree z4
    if fail_disk "$D" "$LABEL"; then
        # new files while degraded: their metadata lacks disk D's copy
        mk_tree z4w
        sync; sync
        tree_sums $Z_FILES
        fresh_disk "$D"
        if hammer2 -s $MNTPT raid replace "$(disk_dev "$D")" "$(disk_dev "$D")" \
                > /var/tmp/z_replace.txt 2>&1 && disk_state_is "$D" ONLINE; then
            result PASS "$LABEL: disk $D resilvered"
            E=$(( (D + 1) % NDISKS ))
            F=$(( (D + 2) % NDISKS ))
            # leave disk D and the disk after the other two
            if [ "$NDISKS" -gt 4 ]; then
                F2=$(( (D + 3) % NDISKS ))
            fi
            if fail_disk "$E" "$LABEL" && fail_disk "$F" "$LABEL"; then
                sync
                umount $MNTPT
                wipe_meta "$E"
                wipe_meta "$F"
                if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
                    check_tree "$LABEL resilvered disk $D + survivors, disk${E}+disk${F} failed"
                    scrub_clean "$LABEL"
                else
                    result FAIL "$LABEL: mount with disk${E}+disk${F} failed refused"
                fi
            fi
        else
            result FAIL "$LABEL: replace of disk $D failed"
            sed 's/^/      /' /var/tmp/z_replace.txt
        fi
    fi
    teardown "$LABEL"
    D=$((D + 1))
done

# --- Z5: degraded writes ----------------------------------------------
for SECOND in 0 2; do
    LABEL="Z5.$SECOND"
    setup_fresh
    check_v3
    mk_tree z5a
    sync; sync
    if fail_disk 1 "$LABEL"; then
        mk_tree z5b
        sync; sync
        tree_sums $Z_FILES
        if fail_disk "$SECOND" "$LABEL"; then
            sync
            umount $MNTPT
            wipe_meta 1
            wipe_meta "$SECOND"
            if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
                check_tree "$LABEL disk 1 failed during writes, then disk $SECOND"
                scrub_clean "$LABEL"
            else
                result FAIL "$LABEL: mount refused"
            fi
        fi
    fi
    teardown "$LABEL"
done

# --- Z6: an array made before the change ------------------------------
HAMMER2_MKFS_LAYOUT2=1
export HAMMER2_MKFS_LAYOUT2
zero_heads
setup_fresh
unset HAMMER2_MKFS_LAYOUT2
check_v3
ref_tree z6
umount $MNTPT
holders
NB=$(wc -l < /var/tmp/z_holders.txt | tr -d ' ')
Z6_BAD=$(awk -v n=$NDISKS 'NF != n + 1 { c++ } END { print c + 0 }' /var/tmp/z_holders.txt)
if [ "$NB" -ge 8 ] && [ "$Z6_BAD" = 0 ]; then
    result PASS "Z6: layout 2 array keeps all-disk mirrors ($NB blocks on $NDISKS disks)"
else
    result FAIL "Z6: layout 2 array: $NB blocks, $Z6_BAD not on every disk"
fi
mount -t hammer2 $PFSPATH $MNTPT
check_tree "Z6: layout 2 array mounts and reads"
sync
umount $MNTPT
# two failures on the unconverted array
A=1; B=2
mount -t hammer2 $PFSPATH $MNTPT
if fail_disk $A Z6 && fail_disk $B Z6; then
    sync
    umount $MNTPT
    wipe_meta $A
    wipe_meta $B
    mount -t hammer2 $PFSPATH $MNTPT
    check_tree "Z6: layout 2 array, disk${A}+disk${B} failed"
fi
teardown "Z6a"

# convert: layout 2 array, healthy, mount with the upgrade sysctl
HAMMER2_MKFS_LAYOUT2=1
export HAMMER2_MKFS_LAYOUT2
zero_heads
setup_fresh
unset HAMMER2_MKFS_LAYOUT2
ref_tree z6o
umount $MNTPT
sysctl vfs.hammer2.md_3copy_upgrade=1 > /dev/null
kmsg_clear
mount -t hammer2 $PFSPATH $MNTPT
sysctl vfs.hammer2.md_3copy_upgrade=0 > /dev/null
if kmsg | grep -q "metadata converted to 3 copies"; then
    result PASS "Z6: layout 2 array converted at mount"
else
    result FAIL "Z6: no conversion message"
fi
mk_tree z6n
sync; sync
tree_sums $Z_FILES
umount $MNTPT
# blocks written now are on 3 disks; older ones stay on every disk
holders
Z6_N3=$(awk 'NF == 4 { c++ } END { print c + 0 }' /var/tmp/z_holders.txt)
if [ "$Z6_N3" -gt 0 ]; then
    result PASS "Z6: converted array writes 3 copies ($Z6_N3 blocks on 3 disks)"
else
    result FAIL "Z6: converted array wrote no 3-copy block"
fi
A=0; B=3
mount -t hammer2 $PFSPATH $MNTPT
check_tree "Z6: converted array, cold remount"
if fail_disk $A Z6o && fail_disk $B Z6o; then
    sync
    umount $MNTPT
    wipe_meta $A
    wipe_meta $B
    mount -t hammer2 $PFSPATH $MNTPT
    check_tree "Z6: converted array, disk${A}+disk${B} failed"
    scrub_clean "Z6: converted array"
fi
teardown "Z6b"

summary
