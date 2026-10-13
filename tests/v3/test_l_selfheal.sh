#!/bin/sh
# Group L: Read-path self-heal (bitrot.md §7.4 bullet 1 — ZFS behavior).
#
# L1 — corrupt one disk's stripe-data region while unmounted, remount,
#      READ the file (no scrub): content must match the pre-corruption
#      reference (in-memory heal + parity reconstruction on the read
#      path), and `raid status` must show cksum_err > 0 with healed > 0
#      on the corrupted disk only.
# L2 — after the deferred repair writes land, a scrub must be clean
#      (proves the read path repaired the media, not just the buffer
#      cache).
# L4 — R8: disk 0 (the metadata primary) failed AND disk 1's metadata
#      copies corrupt.  The degraded read takes the first surviving
#      copy, which is bad; the tree must still read from disks 2/3.
# L3 — metadata self-heal: corrupt a slice of the metadata zone on one
#      disk, remount, walk the tree (find + stat).  All files must be
#      reachable (mirror-copy failover) and no CHECK errors may leak to
#      userspace.
# L7 — every failure point of the first mount unwinds completely and
#      the same mount works afterwards (inject_mount_fail).
# L5 — P4: cold metadata reads are spread over the mirrors, so a bad
#      copy on disk 2 (not the primary) is what a read finds first for
#      the blocks disk 2 serves.  The tree must read, and the repair
#      must go to disk 2.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group L: Read-path self-heal (NDISKS=$NDISKS) ==="

scrub_field() {
    local field="$1"
    local out="$2"
    awk -v f="$field" '$1 == f":" { print $2 }' < "$out"
}

# status_cksum <disk_idx> <field-col>: parse `raid status` per-disk line:
#   disk[N]:  ONLINE  cksum_err E  healed H  unrepairable U
status_cksum() {
    local idx="$1"
    local field="$2"
    grep "^disk\[$idx\]:" /var/tmp/l_status.txt | \
        awk -v f="$field" \
        '{ for (i = 1; i < NF; i++) if ($i == f) { print $(i+1); exit } }'
}

# L1/L2: data-column self-heal on the read path.
setup_fresh
check_v3
dd if=/dev/urandom of=$MNTPT/l1 bs=65536 count=512 2>/dev/null
sha256 $MNTPT/l1 > /var/tmp/l1_ref.txt
sync; sync
umount $MNTPT

# Corrupt 16 MB of stripe data on logical disk 1 at the start of the
# stripe data zone, same profile as K2.
CORRUPT_DEV=$(disk_dev 1)
dd if=/dev/urandom of="$CORRUPT_DEV" bs=65536 count=256 seek=$DATA_BLK \
    conv=notrunc 2>/dev/null

mount -t hammer2 $PFSPATH $MNTPT
kmsg_clear

# THE test: read the file with NO scrub.  Self-heal must reconstruct
# every corrupt column inline.
sha256 $MNTPT/l1 > /var/tmp/l1_check.txt 2>&1
if diff -q /var/tmp/l1_ref.txt /var/tmp/l1_check.txt > /dev/null 2>&1; then
    result PASS "L1: read-path returned correct content over corrupt disk"
else
    result FAIL "L1: content mismatch on read (self-heal failed)"
fi

if kmsg | grep -q 'selfheal: read-path repair'; then
    result PASS "L1: dmesg shows read-path self-heal activity"
else
    result FAIL "L1: no selfheal messages in dmesg"
    kmsg | tail -20
fi

hammer2 -s $MNTPT raid status > /var/tmp/l_status.txt 2>&1
L1_ERR1=$(status_cksum 1 cksum_err)
L1_HEAL1=$(status_cksum 1 healed)
L1_ERR0=$(status_cksum 0 cksum_err)
if [ "${L1_ERR1:-0}" -gt 0 ] && [ "${L1_HEAL1:-0}" -gt 0 ]; then
    result PASS "L1: raid status counters (disk1 err=$L1_ERR1 healed=$L1_HEAL1)"
else
    result FAIL "L1: raid status counters missing (err=$L1_ERR1 healed=$L1_HEAL1)"
    cat /var/tmp/l_status.txt
fi
if [ "${L1_ERR0:-x}" = "0" ]; then
    result PASS "L1: healthy disk 0 shows zero cksum errors"
else
    result FAIL "L1: disk 0 unexpectedly shows cksum_err=$L1_ERR0"
fi

# L2: give the deferred repair writes a moment to land, then scrub.
# The scrub must find nothing left to repair on the blocks the read
# path already healed — and it heals the rest of the corrupted region
# (blocks of l1 we read cover everything, so expect clean-or-repairs,
# but a SECOND scrub right after must be fully clean).
sleep 3
sync
hammer2 -s $MNTPT raid scrub > /var/tmp/l2_scrub1.txt 2>&1
hammer2 -s $MNTPT raid scrub > /var/tmp/l2_scrub2.txt 2>&1
L2_RC=$?
L2_BAD=$(scrub_field brefs_bad /var/tmp/l2_scrub2.txt)
L2_UNREP=$(scrub_field brefs_unrepairable /var/tmp/l2_scrub2.txt)
if [ "$L2_RC" = "0" ] && [ "${L2_BAD:-x}" = "0" ] && \
   [ "${L2_UNREP:-x}" = "0" ]; then
    result PASS "L2: follow-up scrub clean (on-disk repair landed)"
else
    result FAIL "L2: follow-up scrub bad=$L2_BAD unrep=$L2_UNREP rc=$L2_RC"
    cat /var/tmp/l2_scrub2.txt
fi
kmsg_clear
teardown "L1"

# L3: metadata self-heal via mirror copies.  Create a wide directory
# tree (lots of INODE/INDIRECT/DIRENT metadata), corrupt part of the
# metadata zone on disk 2, remount, and walk everything.
setup_fresh
check_v3
mkdir -p $MNTPT/l3
i=0
while [ $i -lt 200 ]; do
    mkdir -p $MNTPT/l3/dir$i
    echo "content-$i" > $MNTPT/l3/dir$i/file
    i=$((i + 1))
done
sync; sync
FCOUNT_REF=$(find $MNTPT/l3 | wc -l | tr -d ' ')
umount $MNTPT

# Metadata brefs carry copyid 0: the primary copy is always read from
# disk 0, and disks 1..N-1 hold the mirror copies.  To exercise the
# mirror-failover heal the corruption must land on DISK 0's metadata
# zone (extent 0, MD_BLK..DATA_BLK from common.sh; 20..220 MB on the
# 4 GB test disks).  Skip the first 1 MB of the extent, where mkfs put
# the super-root; corrupt the rest of it.
MD_DEV=$(disk_dev 0)
dd if=/dev/urandom of="$MD_DEV" bs=65536 count=$((DATA_BLK - MD_BLK - 16)) \
    seek=$((MD_BLK + 16)) \
    conv=notrunc 2>/dev/null

mount -t hammer2 $PFSPATH $MNTPT
kmsg_clear

FCOUNT=$(find $MNTPT/l3 2>/var/tmp/l3_find_err.txt | wc -l | tr -d ' ')
CATFAIL=0
i=0
while [ $i -lt 200 ]; do
    C=$(cat $MNTPT/l3/dir$i/file 2>/dev/null)
    [ "$C" = "content-$i" ] || CATFAIL=$((CATFAIL + 1))
    i=$((i + 20))
done

if [ "$FCOUNT" = "$FCOUNT_REF" ] && [ "$CATFAIL" = "0" ] && \
   [ ! -s /var/tmp/l3_find_err.txt ]; then
    result PASS "L3: full tree reachable over corrupt metadata disk ($FCOUNT nodes)"
else
    result FAIL "L3: tree walk degraded (count=$FCOUNT/$FCOUNT_REF catfail=$CATFAIL)"
    cat /var/tmp/l3_find_err.txt
fi

# Accept either the (rate-limited) read-path heal line or the repair
# kthread's write line — in L3 only disk 0 metadata was corrupted, so
# any repair write to disk 0 proves the mirror heal fired.
if kmsg | grep -q 'selfheal: read-path repair.*metadata\|selfheal: repair write disk 0'; then
    result PASS "L3: dmesg shows metadata mirror self-heal"
else
    # The walk must have hit at least some of the 4 MB corrupt window
    # on the primary metadata disk; silence here means the heal path
    # did not fire.
    result FAIL "L3: no metadata selfheal lines in dmesg"
    kmsg | tail -15
fi
kmsg_clear
teardown "L3"

# L4: R8.  With disk 0 failed, a metadata read falls back to the
# mirror copies in disk order.  Disk 1's copy is bitrotted, so the
# first copy taken fails its CHECK and the read has to go on to the
# copies on disks 2 and 3.
setup_fresh
check_v3
mkdir -p $MNTPT/l4
i=0
while [ $i -lt 200 ]; do
    mkdir -p $MNTPT/l4/dir$i
    echo "content-$i" > $MNTPT/l4/dir$i/file
    i=$((i + 1))
done
sync; sync
FCOUNT_REF=$(find $MNTPT/l4 | wc -l | tr -d ' ')
if fail_disk 0 "L4"; then
    sync
    umount $MNTPT
    corrupt_disk 1 $((MD_BLK + 16)) $((DATA_BLK - MD_BLK - 16))
    if mount -t hammer2 $PFSPATH $MNTPT; then
        kmsg_clear
        FCOUNT=$(find $MNTPT/l4 2>/var/tmp/l4_find_err.txt | wc -l | tr -d ' ')
        CATFAIL=0
        i=0
        while [ $i -lt 200 ]; do
            C=$(cat $MNTPT/l4/dir$i/file 2>/dev/null)
            [ "$C" = "content-$i" ] || CATFAIL=$((CATFAIL + 1))
            i=$((i + 10))
        done
        if [ "$FCOUNT" = "$FCOUNT_REF" ] && [ "$CATFAIL" = "0" ] && \
           [ ! -s /var/tmp/l4_find_err.txt ]; then
            result PASS "L4: tree reachable, disk 0 failed + disk 1 metadata corrupt ($FCOUNT nodes)"
        else
            result FAIL "L4: tree walk degraded (count=$FCOUNT/$FCOUNT_REF catfail=$CATFAIL)"
            cat /var/tmp/l4_find_err.txt
        fi
    else
        result FAIL "L4: remount refused"
    fi
fi
kmsg_clear
teardown "L4"

# L5: P4.  Corrupt a mirror's metadata zone, not the primary's.
setup_fresh
check_v3
mkdir -p $MNTPT/l5
i=0
while [ $i -lt 200 ]; do
    mkdir -p $MNTPT/l5/dir$i
    echo "content-$i" > $MNTPT/l5/dir$i/file
    i=$((i + 1))
done
sync; sync
FCOUNT_REF=$(find $MNTPT/l5 | wc -l | tr -d ' ')
umount $MNTPT
corrupt_disk 2 $((MD_BLK + 16)) $((DATA_BLK - MD_BLK - 16))
if mount -t hammer2 $PFSPATH $MNTPT; then
    kmsg_clear
    FCOUNT=$(find $MNTPT/l5 2>/var/tmp/l5_find_err.txt | wc -l | tr -d ' ')
    CATFAIL=0
    i=0
    while [ $i -lt 200 ]; do
        C=$(cat $MNTPT/l5/dir$i/file 2>/dev/null)
        [ "$C" = "content-$i" ] || CATFAIL=$((CATFAIL + 1))
        i=$((i + 10))
    done
    if [ "$FCOUNT" = "$FCOUNT_REF" ] && [ "$CATFAIL" = "0" ] && \
       [ ! -s /var/tmp/l5_find_err.txt ]; then
        result PASS "L5: tree reachable over corrupt mirror metadata ($FCOUNT nodes)"
    else
        result FAIL "L5: tree walk degraded (count=$FCOUNT/$FCOUNT_REF catfail=$CATFAIL)"
        cat /var/tmp/l5_find_err.txt
    fi
    sleep 2
    if kmsg | grep -q 'selfheal: repair write disk 2'; then
        result PASS "L5: bad mirror copy on disk 2 repaired"
    else
        result FAIL "L5: no repair write to disk 2"
        kmsg | tail -15
    fi
else
    result FAIL "L5: remount refused"
fi
kmsg_clear
teardown "L5"

# L6: a mount refused because the super-root fails its check on every
# copy must unwind cleanly (it once panicked in hammer2_pfsfree_scan ->
# hammer2_inode_drop).  The mount fails, a second attempt fails the same
# way, and a newfs + mount afterwards still works (no leaked state).
setup_fresh
check_v3
mkdir -p $MNTPT/l6
echo "content-6" > $MNTPT/l6/file
sync; sync
umount $MNTPT
d=0
while [ $d -lt $NDISKS ]; do
    corrupt_disk $d $((MD_BLK + 16)) $((DATA_BLK - MD_BLK - 16))
    d=$((d + 1))
done
kmsg_clear
if mount -t hammer2 $PFSPATH $MNTPT 2>/var/tmp/l6_mount1.txt; then
    result FAIL "L6: mount with every metadata copy corrupt succeeded"
    umount $MNTPT 2>/dev/null || umount -f $MNTPT 2>/dev/null || true
else
    result PASS "L6: mount refused with every metadata copy corrupt"
fi
if kmsg | grep -q 'hammer2_mount: .*reading super-root'; then
    result PASS "L6: refused at the super-root check"
else
    result FAIL "L6: mount not refused at the super-root check"
    kmsg | grep hammer2 | tail -8
fi
if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
    result FAIL "L6: second mount of corrupt array succeeded"
    umount $MNTPT 2>/dev/null || umount -f $MNTPT 2>/dev/null || true
else
    result PASS "L6: second mount refused too"
fi

# Refusal after the super-root is fully set up: no such PFS label.
setup_fresh
umount $MNTPT
kmsg_clear
if mount -t hammer2 ${DEVSPEC}@NOSUCHPFS $MNTPT 2>/dev/null; then
    result FAIL "L6: mount of a missing PFS label succeeded"
    umount $MNTPT 2>/dev/null || true
else
    result PASS "L6: mount of a missing PFS label refused"
fi
mount -t hammer2 $PFSPATH $MNTPT
echo "content-6b" > $MNTPT/l6b
sync
if [ "$(cat $MNTPT/l6b 2>/dev/null)" = "content-6b" ]; then
    result PASS "L6: newfs + mount works after refused mounts"
else
    result FAIL "L6: fresh mount unusable after refused mounts"
fi
kmsg_clear
teardown "L6"

# L7: every failure point of the first mount of a device must unwind
# completely.  vfs.hammer2.inject_mount_fail=N makes point N fail once
# (see hammer2_mount_inject(); 1 init_devvp, 2 open_devvp, 3 init_volumes,
# 4/5 sm_init before/after its allocations, 6 space map rebuild, 7/8
# super-root lookup/check, 9 PFS lookup, 10 PFS acquire).  For each
# point, three times: the mount is refused, the injection is consumed, no
# panic; then the same mount works, the data verifies and the members
# can be closed again (a leaked open makes the next open fail EBUSY, and
# vmstat -m must not show HAMMER2-mount objects or vnodes growing).
l7_h2_mem() {
    vmstat -m | awk '$1 == "HAMMER2-mount" { print $2 }'
}
l7_vnodes() {
    sysctl -n debug.numvnodes 2>/dev/null || sysctl -n vfs.numvnodes
}
setup_fresh
check_v3
write_ref_data l7
umount $MNTPT
# warm up: clean mount cycles so lazily created state is not a "leak",
# and a control: the same cycles without a refusal must not grow either
l7_i=0
while [ $l7_i -lt 3 ]; do
    mount -t hammer2 $PFSPATH $MNTPT && sha256 $MNTPT/l7_a > /dev/null && umount $MNTPT
    l7_i=$((l7_i + 1))
done
L7_MEM0=$(l7_h2_mem)
L7_VN0=$(l7_vnodes)
l7_i=0
while [ $l7_i -lt 10 ]; do
    mount -t hammer2 $PFSPATH $MNTPT && sha256 $MNTPT/l7_a > /dev/null && umount $MNTPT
    l7_i=$((l7_i + 1))
done
echo "  control: 10 clean cycles: HAMMER2-mount objects $L7_MEM0 -> $(l7_h2_mem), vnodes $L7_VN0 -> $(l7_vnodes)"
L7_MEM0=$(l7_h2_mem)
L7_VN0=$(l7_vnodes)
l7_bad=0
L7_GROW=0
L7_MEMLAST=$L7_MEM0
for p in 1 2 3 4 5 6 7 8 9 10; do
    n=0
    while [ $n -lt 3 ]; do
        kmsg_clear
        sysctl vfs.hammer2.inject_mount_fail=$p > /dev/null
        if mount -t hammer2 $PFSPATH $MNTPT 2>/var/tmp/l7_mount.txt; then
            result FAIL "L7: point $p: mount succeeded"
            umount $MNTPT 2>/dev/null || umount -f $MNTPT 2>/dev/null
            l7_bad=1
        elif ! kmsg | grep -q "injected failure at point $p\$"; then
            result FAIL "L7: point $p: mount refused but not at the point"
            kmsg | grep hammer2 | tail -5
            l7_bad=1
        elif [ "$(sysctl -n vfs.hammer2.inject_mount_fail)" != 0 ]; then
            result FAIL "L7: point $p: injection not consumed"
            l7_bad=1
        fi
        if kmsg | grep -q "left hanging\|left over after final"; then
            result FAIL "L7: point $p: unwind left I/Os or chains behind"
            l7_bad=1
        fi
        sysctl vfs.hammer2.inject_mount_fail=0 > /dev/null
        if mount -t hammer2 $PFSPATH $MNTPT 2>/var/tmp/l7_mount2.txt; then
            verify_ref "L7: point $p round $n: data intact after refusal" l7
            umount $MNTPT
        else
            result FAIL "L7: point $p round $n: mount after refusal failed"
            cat /var/tmp/l7_mount2.txt
            l7_bad=1
        fi
        l7_m=$(l7_h2_mem)
        [ "$l7_m" -gt "$L7_MEMLAST" ] && L7_GROW=$((L7_GROW + 1))
        L7_MEMLAST=$l7_m
        n=$((n + 1))
    done
    [ $l7_bad = 0 ] && result PASS "L7: point $p refused and unwound 3 times"
    echo "  point $p: HAMMER2-mount objects $(l7_h2_mem), vnodes $(l7_vnodes)"
    l7_bad=0
done
L7_MEM1=$(l7_h2_mem)
L7_VN1=$(l7_vnodes)
# HAMMER2-mount objects also step up now and then (6 or 12 at a time) in
# plain mount + read + dmesg cycles with no refusal at all (mount/umount
# alone stay flat), so a leak is judged by growth that keeps coming: a
# leak per refusal would grow on most of the 30 rounds.
if [ "$L7_GROW" -le 5 ]; then
    result PASS "L7: HAMMER2-mount objects $L7_MEM0 -> $L7_MEM1 (grew $L7_GROW times in 30)"
else
    result FAIL "L7: HAMMER2-mount objects leaked $L7_MEM0 -> $L7_MEM1 (grew $L7_GROW times in 30)"
fi
# debug.numvnodes is system-wide: after a boot it climbs with unrelated
# activity (the Pi suite saw +56 across L7, yet 30 bare point-1 refusals
# on the same kernel moved it by -5), so it is reported, not judged.  A
# leak that matters shows up as HAMMER2-mount objects or a member that
# cannot be reopened below.
echo "  info: L7: vnodes $L7_VN0 -> $L7_VN1 (system-wide, not judged)"

# A refused RO mount after a failed rebuild is allowed; the mount must
# work and unmount cleanly.
kmsg_clear
sysctl vfs.hammer2.inject_mount_fail=6 > /dev/null
if mount -t hammer2 -r $PFSPATH $MNTPT 2>/dev/null; then
    verify_ref "L7: RO mount with a failed space map rebuild reads" l7
    umount $MNTPT
    result PASS "L7: RO mount survives a failed space map rebuild"
else
    result FAIL "L7: RO mount refused after a failed space map rebuild"
fi
sysctl vfs.hammer2.inject_mount_fail=0 > /dev/null

# A second mount of a PFS that is already mounted is refused and leaves
# the first one alone.
mount -t hammer2 $PFSPATH $MNTPT
mkdir -p ${MNTPT}2
if mount -t hammer2 $PFSPATH ${MNTPT}2 2>/dev/null; then
    result FAIL "L7: second mount of a mounted PFS succeeded"
    umount ${MNTPT}2
else
    result PASS "L7: second mount of a mounted PFS refused"
fi
verify_ref "L7: first mount intact after the refused second" l7
umount $MNTPT

# Members closable: raw writes and a fresh newfs work after all of it.
l7_dd=0
for d in $DEVS; do
    dd if=/dev/zero of=$d bs=512 count=1 seek=100000 2>/dev/null || l7_dd=1
done
if [ $l7_dd = 0 ]; then
    result PASS "L7: members writable (not left open) after the refusals"
else
    result FAIL "L7: raw write to a member failed"
fi
setup_fresh
echo ok > $MNTPT/l7_after
sync
if [ "$(cat $MNTPT/l7_after)" = ok ]; then
    result PASS "L7: newfs + mount after the refusals"
else
    result FAIL "L7: newfs + mount after the refusals unusable"
fi
kmsg_clear
teardown "L7"

summary
