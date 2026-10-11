#!/bin/sh
# Group T: Disks reused from an earlier filesystem, and a refused mount.
#
# HAMMER2 keeps up to 4 volume header copies, one per 2 GB zone, and the
# mount takes the newest valid one.  Two ways a previous array's header
# used to win over the fresh filesystem's:
#
#   T1  newfs -R 6 with -s smaller than 2 GB per disk wrote only zone 0;
#       the old array's zone 1 header (newer txg_seq) survived and the
#       mount chose it: "no valid space map header", then a refused RW
#       mount.  newfs must invalidate every header zone that fits on the
#       device.
#   T2  The same stale zone, put back by hand after a full newfs (a disk
#       reused from a larger array, or a smaller slice).  The mount must
#       ignore a header copy whose fsid differs from the disk's zone 0.
#
# And a mount that fails after the devices are open must release them:
#
#   T3  Corrupt the space map and the super-root of every disk, so the RW
#       mount is refused ("space map rebuild failed ... refusing RW
#       mount").  newfs on the same devices must then work without a
#       reboot (the devices were left open: "Device busy"), and
#       kldunload must not report 'HAMMER2-mount' still allocated.
#
# Each of T1/T2 requires: the mount succeeds, the filesystem is the new
# one (empty, new fsid) and a scrub is clean.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group T: Disk Reuse and Refused Mount (NDISKS=$NDISKS) ==="

ZONE_BLKS=32768		# HAMMER2_ZONE_BYTES64 / 64 KB

# sizes_2g: "2g:2g:..." for -s, one 2 GB zone per disk.
sizes_2g() {
    local i=0 s=""
    while [ "$i" -lt "$NDISKS" ]; do
        s="${s:+$s:}2g"
        i=$((i + 1))
    done
    echo "$s"
}

# zone_nonzero <idx> <zone>: true if the 64 KB header block of that zone
# holds anything.
zone_nonzero() {
    [ "$(dd if="$(disk_dev "$1")" bs=65536 count=1 skip=$(($2 * ZONE_BLKS)) \
        2>/dev/null | tr -d '\000' | wc -c)" -gt 0 ]
}

# age_array: write and remount until every disk has a header in zone 1,
# so the array holds a stale zone 1 with a txg_seq above a fresh one's.
age_array() {
    local n=0 i ok
    while [ "$n" -lt 6 ]; do
        ok=1
        i=0
        while [ "$i" -lt "$NDISKS" ]; do
            zone_nonzero $i 1 || ok=0
            i=$((i + 1))
        done
        [ "$ok" = 1 ] && return 0
        dd if=/dev/urandom of=$MNTPT/age$n bs=65536 count=16 2>/dev/null
        remount || return 1
        n=$((n + 1))
    done
    return 1
}

# vol_fsid: the fsid in the newest header of disk 0, from the dump.
vol_fsid() {
    hammer2 volhdr $DEVSPEC 2>/dev/null | awk '$1 == "fsid" { print $2; exit }'
}

# newfs_fsid <file>: the fsid newfs printed.
newfs_fsid() {
    awk '$1 == "vol-fsid:" { print $2; exit }' "$1"
}

# new_fs_checks <label> <fsid-from-newfs>: the mount is the new
# filesystem, usable and clean.
new_fs_checks() {
    local label="$1" want="$2" n got
    if ! mount -t hammer2 $PFSPATH $MNTPT 2>/var/tmp/t_mount.txt; then
        result FAIL "$label: mount failed on the freshly made filesystem"
        kmsg | grep -i "hammer2" | tail -6 | sed 's/^/      /'
        return 1
    fi
    result PASS "$label: mount succeeded"
    n=$(ls $MNTPT | wc -l)
    if [ "$n" -eq 0 ]; then
        result PASS "$label: filesystem is empty (the new one)"
    else
        result FAIL "$label: $n entries left from the old filesystem"
    fi
    got=$(vol_fsid)
    if [ -n "$want" ] && [ "$got" = "$want" ]; then
        result PASS "$label: volume header fsid is the new one"
    else
        result FAIL "$label: volume header fsid '$got' != newfs '$want'"
    fi
    dd if=/dev/urandom of=$MNTPT/new bs=65536 count=32 2>/dev/null
    sha256 $MNTPT/new > /var/tmp/t_new.txt
    if remount; then
        sha256 $MNTPT/new > /var/tmp/t_chk.txt 2>&1
        if diff -q /var/tmp/t_new.txt /var/tmp/t_chk.txt > /dev/null 2>&1; then
            result PASS "$label: write survives a cold remount"
        else
            result FAIL "$label: write lost after remount"
        fi
        scrub_clean "$label"
    else
        result FAIL "$label: cold remount failed"
    fi
    return 0
}

# --- T1: newfs with a smaller -s must clear the old zone 1 ---
t1() {
    local label=T1 fsid
    setup_fresh
    check_v3
    write_ref_data "old"
    if ! age_array; then
        result FAIL "$label: could not get a zone 1 header onto every disk"
        teardown $label
        return
    fi
    umount $MNTPT || { result FAIL "$label: umount"; return; }
    kmsg_clear
    # shellcheck disable=SC2086
    if ! newfs_hammer2 -R 6 -L V3TEST -s $(sizes_2g) $DEVS \
            > /var/tmp/t_newfs.txt 2>&1; then
        result FAIL "$label: newfs -s $(sizes_2g) failed"
        sed 's/^/      /' /var/tmp/t_newfs.txt | tail -5
        return
    fi
    fsid=$(newfs_fsid /var/tmp/t_newfs.txt)
    i=0
    ok=1
    while [ "$i" -lt "$NDISKS" ]; do
        zone_nonzero $i 1 && ok=0
        i=$((i + 1))
    done
    if [ "$ok" = 1 ]; then
        result PASS "$label: newfs cleared the header zone beyond the filesystem"
    else
        result FAIL "$label: newfs left a stale header in zone 1"
    fi
    new_fs_checks $label "$fsid" && teardown $label
}

# --- T2: a stale zone with another fsid, planted after a full newfs ---
t2() {
    local label=T2 fsid i
    setup_fresh
    check_v3
    write_ref_data "old"
    if ! age_array; then
        result FAIL "$label: could not get a zone 1 header onto every disk"
        teardown $label
        return
    fi
    umount $MNTPT || { result FAIL "$label: umount"; return; }
    i=0
    while [ "$i" -lt "$NDISKS" ]; do
        dd if=$(disk_dev $i) of=/var/tmp/t_zone1_$i bs=65536 count=1 \
            skip=$ZONE_BLKS 2>/dev/null
        i=$((i + 1))
    done
    # shellcheck disable=SC2086
    if ! newfs_hammer2 -R 6 -L V3TEST $DEVS > /var/tmp/t_newfs.txt 2>&1; then
        result FAIL "$label: newfs failed"
        return
    fi
    fsid=$(newfs_fsid /var/tmp/t_newfs.txt)
    i=0
    while [ "$i" -lt "$NDISKS" ]; do
        dd if=/var/tmp/t_zone1_$i of=$(disk_dev $i) bs=65536 count=1 \
            seek=$ZONE_BLKS conv=notrunc 2>/dev/null
        i=$((i + 1))
    done
    kmsg_clear
    new_fs_checks $label "$fsid" || return
    if kmsg | grep -q "belongs to another filesystem"; then
        result PASS "$label: stale header zones were logged as ignored"
    else
        result FAIL "$label: no 'belongs to another filesystem' message"
    fi
    teardown $label
}

# --- T3: a refused RW mount releases the devices and memory ---
t3() {
    local label=T3 i attempt
    setup_fresh
    check_v3
    write_ref_data "ref"
    umount $MNTPT || { result FAIL "$label: umount"; return; }
    # Space map and the start of the metadata (super-root) on every disk.
    i=0
    while [ "$i" -lt "$NDISKS" ]; do
        corrupt_disk $i 64 $((MD_BLK - 64))
        corrupt_disk $i $MD_BLK 64
        i=$((i + 1))
    done
    kmsg_clear
    if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
        result FAIL "$label: mount succeeded on a destroyed space map"
        umount -f $MNTPT 2>/dev/null
        return
    fi
    if kmsg | grep -q "refusing RW mount"; then
        result PASS "$label: RW mount refused"
    else
        result PASS "$label: mount failed (not by the space map refusal)"
    fi
    # The devices must be closed again.
    attempt=0
    while [ "$attempt" -lt 3 ]; do
        # shellcheck disable=SC2086
        newfs_hammer2 -R 6 -L V3TEST $DEVS > /var/tmp/t_newfs.txt 2>&1 && break
        attempt=$((attempt + 1))
        sleep 1
    done
    if [ "$attempt" -lt 3 ]; then
        result PASS "$label: newfs works on the devices after the failed mount"
    else
        result FAIL "$label: newfs after the failed mount: $(tail -1 /var/tmp/t_newfs.txt)"
        return
    fi
    if mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
        result PASS "$label: new filesystem mounts"
        umount $MNTPT
    else
        result FAIL "$label: new filesystem does not mount"
    fi
    kmsg_clear
    if kldunload hammer2 2>/dev/null; then
        if kmsg | grep -q "malloc_uninit.*HAMMER2"; then
            result FAIL "$label: kldunload: $(kmsg | grep 'malloc_uninit.*HAMMER2' | head -2 | tr '\n' ' ')"
        else
            result PASS "$label: kldunload leaves no HAMMER2 memory allocated"
        fi
        kldload hammer2
    else
        result FAIL "$label: kldunload hammer2 failed"
    fi
}

t1
t2
t3

summary
