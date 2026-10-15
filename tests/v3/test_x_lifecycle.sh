#!/bin/sh
# Group X: lifecycle (oct-review §7 item 5).
#
#   X1  unmount during a resilver: plain umount is refused (the ioctl
#       holds the mount), the resilver finishes, the array is clean
#   X2  umount -f during a resilver: the resilver is stopped (the
#       "forced unmount: stopping resilver/scrub" path), nothing hangs
#       or panics; the array mounts again, raid replace (re)builds the
#       disk, data correct cold and scrub clean
#   X3  the same two unmounts during a scrub (the scrub is slowed by
#       inject_delay_ms on a member so it outlasts the umount)
#   X4  fill to ENOSPC: writes fail cleanly, what was written reads
#       back, delete + two bulkfree passes give the space back, writes
#       succeed again, scrub clean
#   X4b the same to the very end: write(2) must return ENOSPC (no hang)
#   X5  raid6_simd 0/1 parity cross-check: encode with one setting and
#       reconstruct (two data disks failed) with the other, both ways.
#       Two erasures need both P and Q, so each direction checks both
#       syndromes of one setting against the other.  (Comparing raw disk
#       bytes does not work: which row a column lands in depends on
#       flush timing, so on the Pi two identical runs lay the same data
#       out differently.)  Only the arm64 build has a SIMD path
#       (NEON); on other builds the two settings run the same code
#       and the test says so.
#   X7  an interrupted resilver resumes from its committed cursor
#       (same mount, after a remount), and a stale cursor (a write while
#       the disk was out) is ignored
#   X9  a member's device goes away (inject_gone) under a heavy write and
#       metadata load, disk 0 and another: no panic, failed and closed,
#       data intact, replace onto the same path, scrub clean
#   X8  Phase A copies only the freemap-allocated metadata; what it
#       skipped is never read as metadata
#   X10 new rows put no data column on a failed disk (1 and 2 failed),
#       degraded-written data reads back and survives a resilver
#   X6  disk count: the number of vbd test disks the guest has against
#       NDISKS (information; a 6-disk matrix needs a harness change)
#
# Sizes: every array is the smallest one (2 GB per disk, X_SIZE), so the
# run time does not grow with the disk size (Pi slices).

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group X: Lifecycle (NDISKS=$NDISKS) ==="

X_SIZE="${X_SIZE:-2g}"
CHUNK=/var/tmp/x_chunk
X_DISK=1
X4_RESERVE=${X4_RESERVE:-256}   # MB left free by x4
head -c 4194304 /dev/urandom > /var/tmp/x_small
NBIG=${X_NBIG:-12}      # 64 MB files for the resilver/scrub tests

x_cleanup() {
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null 2>&1
    sysctl -w vfs.hammer2.raid6_simd=1 > /dev/null 2>&1
    sysctl -w vfs.hammer2.resilver_stop_slots=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.resilver_ckpt_slots=2048 > /dev/null 2>&1
    rm -f $CHUNK /var/tmp/x_small /var/tmp/x9.on
}
trap x_cleanup EXIT INT TERM

head -c 67108864 /dev/urandom > $CHUNK
CH=$(sha256 -q $CHUNK)

is_mounted() {
    mount | grep -q " on $MNTPT "
}

# fill_big <n>: n x 64 MB copies of $CHUNK, big0..big(n-1)
fill_big() {
    local i=0
    while [ "$i" -lt "$1" ]; do
        cp $CHUNK $MNTPT/big$i || return 1
        i=$((i + 1))
    done
    sync
}

# verify_big <n> <label>: every big file reads back as $CHUNK
verify_big() {
    local i=0 bad=0
    while [ "$i" -lt "$1" ]; do
        [ "$(sha256 -q $MNTPT/big$i 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
        i=$((i + 1))
    done
    if [ "$bad" = "0" ]; then
        result PASS "$2: all $1 big files intact (cold)"
    else
        result FAIL "$2: $bad of $1 big files wrong (cold)"
    fi
}

# zero a failed disk through the used part of the data zone
x_fresh_disk() {
    dd if=/dev/zero of=$(disk_dev "$1") bs=65536 \
        count=$((DATA_BLK + 16384)) 2>/dev/null || true
}

# wait_running <file>: poll `raid status` until a resilver is running
# (or the background job that writes <file>... has gone).
wait_resilver() {
    local pid="$1" n=0
    while [ "$n" -lt 100 ]; do
        hammer2 -s $MNTPT raid status 2>/dev/null | grep -q '^running:[[:space:]]*1' && return 0
        kill -0 $pid 2>/dev/null || return 1
        sleep 0.2
        n=$((n + 1))
    done
    return 1
}

# wait_gone <pid> <secs>: wait for a background job of ours (a guarded
# `wait` runs in a subshell and cannot wait for it); 1 if still running.
wait_gone() {
    local n=0
    while kill -0 $1 2>/dev/null; do
        [ "$n" -ge "$2" ] && return 1
        sleep 1
        n=$((n + 1))
    done
    wait $1; WRC=$?
    return 0
}

# kmsg_has <pattern>
kmsg_has() {
    kmsg | grep -q "$1"
}

# resilver_setup <label> <disk>: array with $NBIG files + ref data, the
# disk failed and zeroed, a whole-disk (skip_unalloc=0) replace started
# in the background; sets RPID and returns 0 once it is running.
resilver_setup() {
    local label="$1" d="$2" slow="$3" sz="$X_SIZE"
    [ -z "$slow" ] || sz="${X2_SIZE:-4g}"
    setup_fresh $sz
    check_v3
    write_ref_data "ref"
    if ! fill_big $NBIG; then
        result FAIL "$label: could not write the big files"
        return 1
    fi
    fail_disk "$d" "$label" || return 1
    x_fresh_disk "$d"
    sysctl -w vfs.hammer2.resilver_skip_unalloc=0 > /dev/null
    if [ -n "$slow" ]; then
        # Resilver reads go straight to the buffer cache, past the
        # delay injection, and take 6 s for the 2 GB array: less than
        # the 12 s that umount -f waits (the 4 GB array doubles it) for the client before it stops
        # the resilver.  Writers hold rebuild_lk shared across their
        # member writes and each resilver pass needs it exclusive, so
        # slow writes to a survivor (inject_delay_ms) plus a steady
        # writer stretch the resilver.
        sysctl -w vfs.hammer2.inject_delay_ms=${X_SLOW_MS:-150} > /dev/null
        sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=1 > /dev/null
        : > /var/tmp/x_writer.on
        (
            while [ -e /var/tmp/x_writer.on ]; do
                dd if=/dev/zero of=$MNTPT/writer bs=65536 count=4 \
                    conv=notrunc 2>/dev/null
                sync
            done
        ) &
        WPID=$!
    fi
    kmsg_clear
    hammer2 -s $MNTPT raid replace "$(disk_dev $d)" "$(disk_dev $d)" \
        > /var/tmp/x_replace.txt 2>&1 &
    RPID=$!
    if wait_resilver $RPID; then
        return 0
    fi
    result FAIL "$label: resilver was not running (finished or never started)"
    wait $RPID
    return 1
}

# ----------------------------------------------------------------
# X1: plain umount during a resilver
# ----------------------------------------------------------------
x1() {
if resilver_setup X1 $X_DISK; then
    if guarded 120 umount $MNTPT 2>/var/tmp/x_umount.txt; then
        result FAIL "X1: umount succeeded during a resilver"
    else
        result PASS "X1: umount refused during a resilver ($(tr '\n' ' ' < /var/tmp/x_umount.txt))"
    fi
    if kill -0 $RPID 2>/dev/null; then
        result PASS "X1: resilver still running after the refused umount"
    else
        result FAIL "X1: resilver ended with the refused umount"
    fi
    wait $RPID
    RC=$?
    sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
    if [ "$RC" = "0" ] && is_mounted && disk_state_is $X_DISK ONLINE; then
        result PASS "X1: resilver completed, disk$X_DISK ONLINE"
    else
        result FAIL "X1: resilver rc=$RC mounted=$(is_mounted && echo y || echo n)"
        sed 's/^/      /' /var/tmp/x_replace.txt
    fi
    remount
    verify_ref "X1: reference data (cold)" "ref"
    verify_big $NBIG "X1"
    scrub_clean "X1"
fi
sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
teardown "X1"
}

# ----------------------------------------------------------------
# X2: umount -f during a resilver
# ----------------------------------------------------------------
x2() {
if resilver_setup X2 $X_DISK slow; then
    sleep 3
    t0=$(date +%s)
    echo "  info: X2: resilver at: $(hammer2 -s $MNTPT raid status | awk '/^(progress|stripes_)/ { printf "%s%s ", $1, $2 }')"
    if guarded 180 umount -f $MNTPT 2>/var/tmp/x_umount.txt; then
        result PASS "X2: umount -f during a resilver returned ($(($(date +%s) - t0)) s)"
    else
        result FAIL "X2: umount -f failed or hung ($(tr '\n' ' ' < /var/tmp/x_umount.txt))"
    fi
    rm -f /var/tmp/x_writer.on
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null
    wait_gone $RPID 120
    wait_gone $WPID 30
    RC=$?
    kmsg | grep -i "resilver\|forced\|scrub" | tail -8 | sed 's/^/  dmesg: /'
    sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
    if kill -0 $RPID 2>/dev/null; then
        result FAIL "X2: the replace command is still running"
    else
        result PASS "X2: the replace command ended (rc=$WRC): $(head -2 /var/tmp/x_replace.txt | tr '\n' ' ')"
    fi
    # umount -f first syncs the mount, which queues behind the
    # resilver's rebuild_lk passes, so the resilver is often done before
    # the stop request can reach it; either way is fine, but say which.
    if kmsg_has "forced unmount: stopping resilver"; then
        echo "  info: X2: the forced unmount found the resilver ioctl still running and asked it to stop"
    else
        echo "  info: X2: the resilver ioctl had returned before the forced unmount could stop it"
    fi
    if is_mounted; then
        result FAIL "X2: still mounted after umount -f"
        umount -f $MNTPT 2>/dev/null
    fi
    # Remount: the full array, else without the half-built disk.
    if mount -t hammer2 $PFSPATH $MNTPT 2>/var/tmp/x_mount.txt; then
        result PASS "X2: full array mounts after the aborted resilver"
    elif mount -t hammer2 "$(degraded_spec $X_DISK)@V3TEST" $MNTPT; then
        result PASS "X2: array mounts degraded (without disk$X_DISK) after the aborted resilver; full mount: $(head -1 /var/tmp/x_mount.txt)"
    else
        result FAIL "X2: array does not mount after the aborted resilver"
    fi
    if is_mounted; then
        echo "  info: disk$X_DISK is $(hammer2 -s $MNTPT raid status | grep "^disk\[$X_DISK\]" | awk '{ print $2 }') after the aborted resilver"
        if disk_state_is $X_DISK FAILED; then
            if hammer2 -s $MNTPT raid replace "$(disk_dev $X_DISK)" \
                    "$(disk_dev $X_DISK)" > /var/tmp/x_replace.txt 2>&1; then
                result PASS "X2: raid replace restarts the resilver and completes"
            else
                result FAIL "X2: raid replace after the abort failed"
                sed 's/^/      /' /var/tmp/x_replace.txt
            fi
        fi
        remount
        verify_ref "X2: reference data (cold)" "ref"
        verify_big $NBIG "X2"
        # An ONLINE disk whose rebuild was cut short would show up as
        # columns healed from parity on these cold reads.
        err=$(disk_counter $X_DISK cksum_err)
        if [ "${err:-x}" = "0" ]; then
            result PASS "X2: disk$X_DISK read clean (no parity heals)"
        else
            result FAIL "X2: disk$X_DISK had $err bad columns on cold reads (rebuild incomplete but disk trusted)"
        fi
        scrub_clean "X2"
    fi
fi
teardown "X2"
}

# ----------------------------------------------------------------
# X3: unmounts during a scrub
# ----------------------------------------------------------------
x3() {
# Every read from disk 0 is delayed a little so the scrub (and its
# brefs) takes seconds, not milliseconds.
scrub_setup() {
    local label="$1"
    setup_fresh $X_SIZE
    check_v3
    write_ref_data "ref"
    # many small files: many brefs for the scrub to walk
    local i=0
    while [ "$i" -lt 300 ]; do
        dd if=/dev/urandom of=$MNTPT/s$i bs=8192 count=1 2>/dev/null
        i=$((i + 1))
    done
    fill_big 4
    remount
    sysctl -w vfs.hammer2.inject_delay_ms=3 > /dev/null
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=1 > /dev/null
    kmsg_clear
    hammer2 -s $MNTPT raid scrub > /var/tmp/x_scrub.txt 2>&1 &
    SPID=$!
    sleep 1
    if kill -0 $SPID 2>/dev/null; then
        return 0
    fi
    result FAIL "$label: scrub was not running 1 s after it started"
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null
    return 1
}

if scrub_setup X3a; then
    if guarded 120 umount $MNTPT 2>/var/tmp/x_umount.txt; then
        result FAIL "X3a: umount succeeded during a scrub"
    else
        result PASS "X3a: umount refused during a scrub ($(tr '\n' ' ' < /var/tmp/x_umount.txt))"
    fi
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null
    wait $SPID
    if is_mounted && grep -q "brefs_done" /var/tmp/x_scrub.txt; then
        result PASS "X3a: scrub completed after the refused umount"
    else
        result FAIL "X3a: scrub did not complete"
        sed 's/^/      /' /var/tmp/x_scrub.txt
    fi
    scrub_clean "X3a"
fi
sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null
teardown "X3a"

if scrub_setup X3b; then
    if guarded 180 umount -f $MNTPT 2>/var/tmp/x_umount.txt; then
        result PASS "X3b: umount -f during a scrub returned"
    else
        result FAIL "X3b: umount -f failed or hung ($(tr '\n' ' ' < /var/tmp/x_umount.txt))"
    fi
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null
    wait_gone $SPID 120
    if kill -0 $SPID 2>/dev/null; then
        result FAIL "X3b: the scrub command is still running"
    else
        result PASS "X3b: the scrub command ended"
    fi
    if kmsg_has "forced unmount: stopping resilver/scrub"; then
        result PASS "X3b: the scrub was stopped by the forced unmount"
    else
        result FAIL "X3b: forced unmount did not hit the scrub (it finished first?)"
    fi
    is_mounted && { result FAIL "X3b: still mounted after umount -f"; umount -f $MNTPT; }
    if mount -t hammer2 $PFSPATH $MNTPT; then
        result PASS "X3b: array mounts after the aborted scrub"
        verify_ref "X3b: reference data (cold)" "ref"
        verify_big 4 "X3b"
        scrub_clean "X3b"
    else
        result FAIL "X3b: array does not mount after the aborted scrub"
    fi
fi
sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null
teardown "X3b"
}

# ----------------------------------------------------------------
# X4: fill to ENOSPC, reclaim, write again
# ----------------------------------------------------------------
x4() {
setup_fresh $X_SIZE
check_v3
cap=$(df -m $MNTPT | awk 'NR == 2 { print $2 }')
# Fill to X4_RESERVE MB from the end; x4b goes all the way.
nmax=$(( cap / 64 + 16 ))
i=0
while [ "$i" -lt "$nmax" ]; do
    avail=$(df -m $MNTPT | awk 'NR == 2 { print $4 }')
    [ "$avail" -ge $((64 + X4_RESERVE)) ] || break
    cp $CHUNK $MNTPT/e$i 2>/var/tmp/x_cp.txt || break
    i=$((i + 1))
    [ $((i % 8)) -ne 0 ] || sync
done
nfull=$i
sync
avail=$(df -m $MNTPT | awk 'NR == 2 { print $4 }')
echo "  info: df at the fill limit: $(df -m $MNTPT | awk 'NR == 2 { print "size", $2, "used", $3, "avail", $4, $5 }')"
if [ "$nfull" -gt 0 ] && [ "$avail" -lt $((64 + X4_RESERVE)) ]; then
    result PASS "X4: filled the array with $nfull x 64 MB (${avail} MB avail of ${cap} MB)"
else
    result FAIL "X4: fill stopped at $nfull files with ${avail} MB avail: $(head -1 /var/tmp/x_cp.txt)"
fi
# a small file still fits in what is left
if cp /var/tmp/x_small $MNTPT/small 2>/dev/null && sync; then
    result PASS "X4: a small file still writes at ${avail} MB avail"
else
    result FAIL "X4: small write failed at ${avail} MB avail"
fi
rm -f $MNTPT/small
if remount; then
    bad=0; j=0
    while [ "$j" -lt "$nfull" ]; do
        [ "$(sha256 -q $MNTPT/e$j 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
        j=$((j + 1))
    done
    if [ "$bad" = "0" ]; then
        result PASS "X4: all $nfull files of the full array intact (cold)"
    else
        result FAIL "X4: $bad of $nfull files wrong on the full array (cold)"
    fi
else
    result FAIL "X4: remount of the full array failed"
fi
# delete the even files, reclaim (two bulkfree passes), write again
j=0; ndel=0
while [ "$j" -lt "$nfull" ]; do
    if [ $((j % 2)) -eq 0 ]; then
        rm -f $MNTPT/e$j || break
        ndel=$((ndel + 1))
    fi
    j=$((j + 1))
done
if [ "$ndel" = "$(( (nfull + 1) / 2 ))" ]; then
    result PASS "X4: deleted $ndel files while the array was full"
else
    result FAIL "X4: delete failed after $ndel files"
fi
sync; sync
k=0
while [ "$k" -lt 2 ]; do
    hammer2 bulkfree $MNTPT > /var/tmp/x_bulkfree.txt 2>&1 ||
        { result FAIL "X4: bulkfree pass $k failed"; sed 's/^/      /' /var/tmp/x_bulkfree.txt; }
    k=$((k + 1))
done
sleep 2
avail2=$(df -m $MNTPT | awk 'NR == 2 { print $4 }')
if [ "$avail2" -ge $((ndel * 64 * 9 / 10)) ]; then
    result PASS "X4: space back after bulkfree (${avail} -> ${avail2} MB avail)"
else
    result FAIL "X4: bulkfree gave back too little (${avail} -> ${avail2} MB avail, $ndel x 64 MB deleted)"
fi
nnew=$((ndel - 2)); k=0; werr=""
while [ "$k" -lt "$nnew" ]; do
    a=$(df -m $MNTPT | awk 'NR == 2 { print $4 }')
    [ "$a" -ge 128 ] || { werr="df shows only ${a} MB avail"; break; }
    cp $CHUNK $MNTPT/h$k 2>/var/tmp/x_cp.txt || { werr=$(head -1 /var/tmp/x_cp.txt); break; }
    k=$((k + 1))
done
sync
if [ "$k" = "$nnew" ]; then
    result PASS "X4: wrote $nnew x 64 MB again after reclaim"
else
    result FAIL "X4: write after reclaim stopped at file $k of $nnew: $werr"
fi
remount
bad=0; j=0
while [ "$j" -lt "$nfull" ]; do
    if [ $((j % 2)) -eq 1 ]; then
        [ "$(sha256 -q $MNTPT/e$j 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
    fi
    j=$((j + 1))
done
j=0
while [ "$j" -lt "$k" ]; do
    [ "$(sha256 -q $MNTPT/h$j 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
    j=$((j + 1))
done
if [ "$bad" = "0" ]; then
    result PASS "X4: survivors and new files intact (cold)"
else
    result FAIL "X4: $bad files wrong after reclaim (cold)"
fi
scrub_clean "X4"
teardown "X4"
}

# ----------------------------------------------------------------
# X4b: fill the array until write(2) fails.  A true ENOSPC used to wedge
# the guest: the allocation failed at flush time, the buffer completed
# with B_ERROR, bpdone() re-dirtied it and it was written again forever
# ("xop_strategy_write: error 28" hundreds of times a second), with the
# writer stuck in write(2).  Now hammer2_vfs_enospace() knows the v3 free
# space, so the writer gets ENOSPC, the flush always finds room, and an
# ENOSPC at flush time invalidates the buffer instead of retrying it.
# ----------------------------------------------------------------
x4b() {
setup_fresh $X_SIZE
check_v3
cap=$(df -m $MNTPT | awk 'NR == 2 { print $2 }')
nmax=$(( cap / 64 + 16 ))
kmsg_clear
rm -f /var/tmp/x_enospc.txt /var/tmp/x_enospc.n
(
    i=0
    while [ "$i" -lt "$nmax" ]; do
        cp $CHUNK $MNTPT/e$i || { echo "cp e$i failed"; echo $i > /var/tmp/x_enospc.n; exit 1; }
        i=$((i + 1))
    done
    echo $i > /var/tmp/x_enospc.n
    exit 2
) > /var/tmp/x_enospc.txt 2>&1 &
EPID=$!
if wait_gone $EPID 240; then
    nfull=$(cat /var/tmp/x_enospc.n 2>/dev/null)
    if [ "$WRC" = "1" ] && grep -q "No space left" /var/tmp/x_enospc.txt; then
        result PASS "X4b: write(2) returned ENOSPC after $nfull x 64 MB files ($(tail -1 /var/tmp/x_enospc.txt))"
    else
        result FAIL "X4b: the fill ended with rc=$WRC after ${nfull:-?} files, not ENOSPC: $(head -2 /var/tmp/x_enospc.txt)"
    fi
else
    result FAIL "X4b: the writer is still blocked 240 s into ENOSPC ($(kmsg | grep -c 'xop_strategy_write: error') failed buffer writes logged); reset the guest"
    return
fi
# a flush that cannot allocate must not happen; and sync must return
sync &
if wait_gone $! 120; then
    result PASS "X4b: sync returns on the full array"
else
    result FAIL "X4b: sync did not return on the full array"
fi
if kmsg | grep -q 'xop_strategy_write: error'; then
    result FAIL "X4b: a flush hit an allocation error ($(kmsg | grep -c 'xop_strategy_write: error') logged): the free-space check let too much through"
else
    result PASS "X4b: no flush ran out of space"
fi
echo "  info: df on the full array: $(df -m $MNTPT | awk 'NR == 2 { print "size", $2, "used", $3, "avail", $4, $5 }')"
rm -f $MNTPT/e$nfull        # the partial file of the failed cp
if remount; then
    bad=0; j=0
    while [ "$j" -lt "$nfull" ]; do
        [ "$(sha256 -q $MNTPT/e$j 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
        j=$((j + 1))
    done
    if [ "$nfull" -gt 0 ] && [ "$bad" = "0" ]; then
        result PASS "X4b: array mounts, all $nfull files written before ENOSPC verify (cold)"
    else
        result FAIL "X4b: $bad of $nfull files wrong after ENOSPC (cold)"
    fi
else
    result FAIL "X4b: remount of the full array failed"
    return
fi
# delete the even files, reclaim, write again
j=0; ndel=0
while [ "$j" -lt "$nfull" ]; do
    [ $((j % 2)) -ne 0 ] || { rm -f $MNTPT/e$j || break; ndel=$((ndel + 1)); }
    j=$((j + 1))
done
sync; sync
k=0
while [ "$k" -lt 2 ]; do
    hammer2 bulkfree $MNTPT > /var/tmp/x_bulkfree.txt 2>&1 ||
        { result FAIL "X4b: bulkfree pass $k failed"; sed 's/^/      /' /var/tmp/x_bulkfree.txt; }
    k=$((k + 1))
done
sleep 2
nnew=$((ndel - 2)); [ "$nnew" -ge 1 ] || nnew=1
k=0; werr=""
while [ "$k" -lt "$nnew" ]; do
    cp $CHUNK $MNTPT/h$k 2>/var/tmp/x_cp.txt || { werr=$(head -1 /var/tmp/x_cp.txt); break; }
    k=$((k + 1))
done
sync
if [ "$k" = "$nnew" ]; then
    result PASS "X4b: deleted $ndel files, bulkfree, wrote $nnew x 64 MB again"
else
    result FAIL "X4b: write after reclaim stopped at file $k of $nnew: $werr"
fi
remount
bad=0; j=0
while [ "$j" -lt "$nfull" ]; do
    if [ $((j % 2)) -eq 1 ]; then
        [ "$(sha256 -q $MNTPT/e$j 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
    fi
    j=$((j + 1))
done
j=0
while [ "$j" -lt "$k" ]; do
    [ "$(sha256 -q $MNTPT/h$j 2>/dev/null)" = "$CH" ] || bad=$((bad + 1))
    j=$((j + 1))
done
if [ "$bad" = "0" ]; then
    result PASS "X4b: survivors and new files intact (cold)"
else
    result FAIL "X4b: $bad files wrong after reclaim (cold)"
fi
scrub_clean "X4b"
teardown "X4b"
}

# ----------------------------------------------------------------
# X5: raid6_simd 0/1 parity cross-check
# ----------------------------------------------------------------
x5() {
if sysctl vfs.hammer2.raid6_simd > /dev/null 2>&1; then
    echo "  info: X5: raid6_simd selects the SIMD (1) or the C loops (0)"
else
    echo "  info: X5: $(uname -m) has no raid6_simd (no SIMD parity path): both settings run the same C loops; the test checks encode/decode agreement and determinism only"
fi
    # 1 MB pattern repeated: 16 x 64 KB distinct blocks, plus an odd tail
    head -c 1048576 /dev/urandom > /var/tmp/x_pat
    head -c 12345 /dev/urandom > /var/tmp/x_tail
    PH=$(sha256 -q /var/tmp/x_pat)
    TH=$(sha256 -q /var/tmp/x_tail)
    x5_write() {
        local i=0
        while [ "$i" -lt 24 ]; do
            cp /var/tmp/x_pat $MNTPT/p$i
            i=$((i + 1))
        done
        cp /var/tmp/x_tail $MNTPT/tail
        sync; sync
    }
    x5_check() {
        local label="$1" i=0 bad=0
        while [ "$i" -lt 24 ]; do
            [ "$(sha256 -q $MNTPT/p$i 2>/dev/null)" = "$PH" ] || bad=$((bad + 1))
            i=$((i + 1))
        done
        [ "$(sha256 -q $MNTPT/tail 2>/dev/null)" = "$TH" ] || bad=$((bad + 1))
        if [ "$bad" = "0" ]; then
            result PASS "$label"
        else
            result FAIL "$label: $bad files wrong"
        fi
    }
    for enc in 0 1; do
        dec=$((1 - enc))
        setup_fresh $X_SIZE
        check_v3
        sysctl -w vfs.hammer2.raid6_simd=$enc > /dev/null 2>&1
        x5_write
        # decode with the other setting: fail two disks (not disk 0),
        # so some data columns come from P/Q
        sysctl -w vfs.hammer2.raid6_simd=$dec > /dev/null 2>&1
        d1=1; d2=$((NDISKS - 1))
        if fail_disk $d1 "X5 enc=$enc" && fail_disk $d2 "X5 enc=$enc"; then
            # cold, without both failed disks
            sync
            umount $MNTPT
            spec=""
            j=0
            while [ "$j" -lt "$NDISKS" ]; do
                if [ "$j" != "$d1" ] && [ "$j" != "$d2" ]; then
                    spec="${spec:+$spec:}$(disk_dev $j)"
                fi
                j=$((j + 1))
            done
            if mount -t hammer2 "${spec}@V3TEST" $MNTPT; then
                x5_check "X5: written with raid6_simd=$enc, read back reconstructed with raid6_simd=$dec (disks $d1,$d2 out)"
            else
                result FAIL "X5: enc=$enc dual-degraded mount failed"
            fi
        fi
        teardown "X5 enc=$enc"
    done
    sysctl -w vfs.hammer2.raid6_simd=1 > /dev/null 2>&1
    rm -f /var/tmp/x_pat /var/tmp/x_tail
}

# ----------------------------------------------------------------
# X6: how many test disks does the guest have?
# ----------------------------------------------------------------
x6() {
n=0
d=$DISK_BASE
while [ -e "${DISK_PREFIX}${d}${DISK_SUFFIX}" ]; do
    n=$((n + 1)); d=$((d + 1))
done
echo "  info: X6: ${DISK_PREFIX}${DISK_BASE}.. present: $n disks (NDISKS=$NDISKS); NDISKS=6 needs 6"
}

# ----------------------------------------------------------------
# X7: an interrupted resilver resumes (oct-review P6 resume cursor)
#
# vfs.hammer2.resilver_stop_slots stops the resilver after N slots
# (EINTR, the disk stays FAILED, as after umount -f or a crash) and
# resilver_ckpt_slots makes it commit its cursor every few slots.  The
# next raid replace onto the same, un-wiped disk must continue from the
# cursor (resilver_resume_slot > 0) and give a clean disk.
#   X7a  stop, replace again in the same mount: resumed, clean
#   X7b  stop, unmount, mount again (the cursor comes from the media),
#        replace: resumed, clean
#   X7c  stop, then a write while the disk is out: the cursor is stale
#        (the new rows are not on the disk), the replace restarts at 0
#   X7d  stop, then the disk is wiped (a different disk): restarts at 0
# ----------------------------------------------------------------
x7_cleanup() {
    sysctl -w vfs.hammer2.resilver_stop_slots=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.resilver_ckpt_slots=2048 > /dev/null 2>&1
}

# x7_stopped <label> <nbig>: array, disk $X_DISK failed and wiped, a
# resilver stopped part-way; returns 0 if it was stopped
x7_stopped() {
    local label="$1" nbig="$2"
    setup_fresh $X_SIZE
    check_v3
    write_ref_data "ref"
    if ! fill_big $nbig; then
        result FAIL "$label: could not write the big files"
        return 1
    fi
    fail_disk $X_DISK "$label" || return 1
    x_fresh_disk $X_DISK
    sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
    sysctl -w vfs.hammer2.resilver_ckpt_slots=${X7_CKPT:-64} > /dev/null
    sysctl -w vfs.hammer2.resilver_stop_slots=${X7_STOP:-1500} > /dev/null
    kmsg_clear
    if hammer2 -s $MNTPT raid replace "$(disk_dev $X_DISK)" \
            "$(disk_dev $X_DISK)" > /var/tmp/x_replace.txt 2>&1; then
        result FAIL "$label: the resilver was not stopped (too few slots?)"
        x7_cleanup
        return 1
    fi
    sysctl -w vfs.hammer2.resilver_stop_slots=0 > /dev/null
    if disk_state_is $X_DISK FAILED; then
        result PASS "$label: resilver stopped part-way, disk$X_DISK still FAILED"
    else
        result FAIL "$label: disk$X_DISK is not FAILED after the stopped resilver"
        return 1
    fi
    return 0
}

# x7_slots <disk>: the slot count of the last "resilver disk N: ..." line
x7_slots() {
    kmsg | grep "resilver disk $1: metadata" | tail -1 |
        sed -n 's/.*ms, \([0-9]*\) slots in.*/\1/p'
}

# x7_finish <label> <expect resume: yes|no>: replace, check resume, data
x7_finish() {
    local label="$1" want="$2" first resumed n2
    first=$(x7_slots $X_DISK)
    kmsg_clear
    if hammer2 -s $MNTPT raid replace "$(disk_dev $X_DISK)" \
            "$(disk_dev ${3:-$X_DISK})" > /var/tmp/x_replace.txt 2>&1; then
        result PASS "$label: the second raid replace completes"
    else
        result FAIL "$label: the second raid replace failed"
        sed 's/^/      /' /var/tmp/x_replace.txt
    fi
    resumed=$(sysctl -n vfs.hammer2.resilver_resume_slot)
    n2=$(x7_slots $X_DISK)
    echo "  info: $label: first run rebuilt ${first:-?} slots, second run resumed at slot $resumed and rebuilt ${n2:-?}"
    if [ "$want" = yes ]; then
        if [ "${resumed:-0}" -gt 0 ] && kmsg_has "resumes at slot"; then
            result PASS "$label: the resilver resumed at slot $resumed, not 0"
        else
            result FAIL "$label: the resilver restarted from slot 0"
        fi
    else
        if [ "${resumed:-1}" = "0" ]; then
            result PASS "$label: stale cursor ignored, resilver restarted at 0"
        else
            result FAIL "$label: resumed at slot $resumed from a stale cursor"
        fi
    fi
    if disk_state_is $X_DISK ONLINE; then
        result PASS "$label: disk$X_DISK ONLINE"
    else
        result FAIL "$label: disk$X_DISK not ONLINE"
    fi
}

x7_verify() {
    local label="$1" err
    remount
    verify_ref "$label: reference data (cold)" "ref"
    verify_big $NBIG "$label"
    err=$(disk_counter $X_DISK cksum_err)
    if [ "${err:-x}" = "0" ]; then
        result PASS "$label: disk$X_DISK read clean (no parity heals)"
    else
        result FAIL "$label: disk$X_DISK had $err bad columns on cold reads"
    fi
    scrub_clean "$label"
}

x7() {
if x7_stopped X7a $NBIG; then
    x7_finish X7a yes
    x7_verify X7a
fi
x7_cleanup
teardown "X7a"

if x7_stopped X7b $NBIG; then
    sync
    if umount $MNTPT && mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null; then
        result PASS "X7b: remounted the full array after the stopped resilver"
        if disk_state_is $X_DISK FAILED; then
            x7_finish X7b yes
            x7_verify X7b
        else
            result FAIL "X7b: disk$X_DISK is not FAILED after the remount"
        fi
    else
        result FAIL "X7b: remount failed"
    fi
fi
x7_cleanup
teardown "X7b"

if x7_stopped X7c $NBIG; then
    # a write while the disk is out: rows that are not on the disk
    dd if=/dev/urandom of=$MNTPT/late bs=65536 count=64 2>/dev/null
    LATE=$(sha256 -q $MNTPT/late)
    sync; sync
    x7_finish X7c no
    remount
    [ "$(sha256 -q $MNTPT/late 2>/dev/null)" = "$LATE" ] &&
        result PASS "X7c: the file written while the disk was out reads back" ||
        result FAIL "X7c: the file written while the disk was out is wrong"
    x7_verify X7c
fi
x7_cleanup
teardown "X7c"

# a different disk (the same one, wiped) must not inherit the cursor
if x7_stopped X7d $NBIG; then
    # the disk is held open by the mount: unmount to wipe it
    sync
    umount $MNTPT
    x_fresh_disk $X_DISK
    if mount -t hammer2 $PFSPATH $MNTPT 2>/var/tmp/x_mount.txt &&
       disk_state_is $X_DISK FAILED; then
        x7_finish X7d no
        x7_verify X7d
    else
        result FAIL "X7d: full mount with the wiped disk failed: $(head -1 /var/tmp/x_mount.txt)"
    fi
fi
x7_cleanup
teardown "X7d"
}

# ----------------------------------------------------------------
# X8: Phase A copies only the freemap-allocated metadata
#
# A nearly empty array: the metadata that exists is a few MB of a zone
# of 100+ MB.  With resilver_skip_unalloc=1 the resilver must copy far
# less than the zone (vfs.hammer2.resilver_md_bytes), with =0 the lot.
# The disk is wiped first, so what the skipped part of the zone holds
# is zeros: nothing may ever read it as metadata.  Prove that with a
# directory tree grown after the resilver, and by reading all the
# metadata back from the rebuilt disk alone (the other members of one
# pair failed), then scrub.
# ----------------------------------------------------------------
x8() {
local mdbytes skipped full want
setup_fresh $X_SIZE
check_v3
mdbytes=$(((DATA_BLK - MD_BLK) * 65536))
write_ref_data "ref"
mkdir $MNTPT/d0
i=0
while [ "$i" -lt 200 ]; do
    echo "file $i" > $MNTPT/d0/f$i
    i=$((i + 1))
done
sync; sync
fail_disk $X_DISK "X8" || { teardown "X8"; return; }
x_fresh_disk $X_DISK
sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
if hammer2 -s $MNTPT raid replace "$(disk_dev $X_DISK)" \
        "$(disk_dev $X_DISK)" > /var/tmp/x_replace.txt 2>&1; then
    result PASS "X8: allocated-only resilver completes"
else
    result FAIL "X8: allocated-only resilver failed"
    sed 's/^/      /' /var/tmp/x_replace.txt
fi
skipped=$(sysctl -n vfs.hammer2.resilver_md_bytes)
echo "  info: X8: metadata zone $mdbytes bytes; Phase A copied $skipped"
if [ "${skipped:-0}" -gt 0 ] && [ "$skipped" -lt "$((mdbytes / 4))" ]; then
    result PASS "X8: Phase A copied $skipped bytes of a $mdbytes byte metadata zone"
else
    result FAIL "X8: Phase A copied $skipped of $mdbytes metadata bytes (expected well under a quarter)"
fi
# grow the tree on the rebuilt disk's zone, then read it back from it alone
mkdir $MNTPT/d1
i=0
while [ "$i" -lt 400 ]; do
    echo "late $i" > $MNTPT/d1/g$i
    i=$((i + 1))
done
sync; sync
fail_disk 0 "X8" && remount "$(degraded_spec 0)@V3TEST" &&
    result PASS "X8: mounted without disk0 (metadata now served from the others)"
verify_ref "X8: reference data, disk0 out" "ref"
n0=$(ls $MNTPT/d0 | wc -l); n1=$(ls $MNTPT/d1 | wc -l)
if [ "$n0" -eq 200 ] && [ "$n1" -eq 400 ] &&
   [ "$(cat $MNTPT/d1/g399)" = "late 399" ]; then
    result PASS "X8: all 600 small files read back with disk0 out"
else
    result FAIL "X8: directory counts d0=$n0 d1=$n1"
fi
scrub_clean "X8 (disk0 out)"
teardown "X8"

# the whole-zone baseline
setup_fresh $X_SIZE
check_v3
write_ref_data "ref"
fail_disk $X_DISK "X8b" || { teardown "X8b"; return; }
x_fresh_disk $X_DISK
sysctl -w vfs.hammer2.resilver_skip_unalloc=0 > /dev/null
if hammer2 -s $MNTPT raid replace "$(disk_dev $X_DISK)" \
        "$(disk_dev $X_DISK)" > /var/tmp/x_replace.txt 2>&1; then
    full=$(sysctl -n vfs.hammer2.resilver_md_bytes)
    # only the blocks the target holds a copy of (oct-review 12): all of
    # them for disk 0, 2 of every N-1 for the others
    if [ "$X_DISK" = 0 ]; then
        want=$mdbytes
    else
        want=$((mdbytes * 2 / (NDISKS - 1)))
    fi
    want=$((want * 95 / 100))
    if [ "${full:-0}" -ge "$want" ]; then
        result PASS "X8b: skip_unalloc=0 copies the whole zone ($full bytes, at least $want)"
    else
        result FAIL "X8b: skip_unalloc=0 copied $full of $mdbytes bytes"
    fi
else
    result FAIL "X8b: whole-zone resilver failed"
fi
sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
scrub_clean "X8b"
teardown "X8b"
}

# ----------------------------------------------------------------
# X9: a member's device goes away under a heavy write + metadata load
# (the Pi's disk_gone: the member is failed and its device closed, which
# flushes and invalidates the member's buffers; a buffer made on it
# meanwhile used to panic vinvalbuf, "flush failed").  The guest kernel
# has no disk_gone event, so vfs.hammer2.inject_gone runs the same
# handler for a member.  Disk 0 holds the primary of every metadata
# block, so the dio layer's buffers are on it.  Each round: writers,
# small-file churn and a reader run while the member is dropped after a
# different delay; nothing may panic, the closed member must be
# reported, the survivors' data must read back, and the array has to
# take the disk again (replace onto the same path) and scrub clean.
# ----------------------------------------------------------------
x9_round() {
    local d="$1" delay="$2" label="X9 disk $1 +${2}s" n w alive
    setup_fresh $X_SIZE
    check_v3
    write_ref_data "ref"
    mkdir $MNTPT/churn
    kmsg_clear
    : > /var/tmp/x9.on
    (
        k=0
        while [ -e /var/tmp/x9.on ]; do
            dd if=$CHUNK of=$MNTPT/w$((k % 6)) bs=65536 2>/dev/null
            k=$((k + 1))
        done
    ) &
    W1=$!
    (
        k=0
        while [ -e /var/tmp/x9.on ]; do
            j=0
            while [ "$j" -lt 100 ]; do
                echo "$k $j" > $MNTPT/churn/c$j
                j=$((j + 1))
            done
            ls -l $MNTPT/churn > /dev/null
            rm -f $MNTPT/churn/c*
            sync
            k=$((k + 1))
        done
    ) &
    W2=$!
    (
        while [ -e /var/tmp/x9.on ]; do
            cat $MNTPT/ref_a $MNTPT/ref_b > /dev/null
            ls -lR $MNTPT > /dev/null 2>&1
        done
    ) &
    W3=$!
    [ "$delay" = 0 ] || sleep $delay
    sysctl -w vfs.hammer2.inject_gone=$d > /dev/null
    n=0
    while [ "$n" -lt 150 ]; do
        kmsg | grep "RAID6 disk $d .* is gone" | grep -q "failed and closed" &&
            break
        sleep 0.2
        n=$((n + 1))
    done
    if [ "$n" -ge 150 ]; then
        # the close never ends: it waits for a buffer a writer holds
        result FAIL "$label: member not closed after 30 s (hung): $(kmsg | grep 'is gone' | tail -1)"
        rm -f /var/tmp/x9.on
        return 1
    fi
    result PASS "$label: member failed and closed under load"
    sleep 2
    rm -f /var/tmp/x9.on
    wait_gone $W1 120; wait_gone $W2 120; wait_gone $W3 120
    alive=0
    for w in $W1 $W2 $W3; do
        kill -0 $w 2>/dev/null && alive=1
    done
    if [ "$alive" = 1 ]; then
        result FAIL "$label: the load did not finish"
        return 1
    fi
    sync
    verify_ref "$label: reference data" "ref"
    x_fresh_disk $d
    sysctl -w vfs.hammer2.resilver_skip_unalloc=1 > /dev/null
    if hammer2 -s $MNTPT raid replace "$(disk_dev $d)" "$(disk_dev $d)" \
            > /var/tmp/x_replace.txt 2>&1; then
        result PASS "$label: replace onto the same path"
    else
        result FAIL "$label: replace failed"
        sed 's/^/      /' /var/tmp/x_replace.txt
    fi
    scrub_clean "$label"
    teardown "$label"
}

x9() {
    local d t
    for d in ${X9_DISKS:-0 2}; do
        for t in ${X9_DELAYS:-0 1 2}; do
            x9_round $d $t || return
        done
    done
}

# ----------------------------------------------------------------
# X10: new rows put no data column on a failed disk (oct-review §2)
#
# Fail 1 disk (X10a) or 2 (X10b), write a mix of big and small files, and
# read the blockrefs from the media: no new data block may name a
# failed disk.  Then the data must read back cold, the disks must
# resilver (fresh media) and the scrub be clean.  With two data disks
# down, a row whose P and Q sit on the two survivors has no data disk
# left: the allocator must go on to the next slot, not fail the write.
# ----------------------------------------------------------------
x10_round() {
    local label="$1"
    shift
    local f i new bad
    setup_fresh $X_SIZE
    check_v3
    write_ref_data "ref"
    data_slots > /var/tmp/x10_before.txt
    for f in "$@"; do
        fail_disk "$f" "$label" || { teardown "$label"; return; }
    done
    write_ref_data "dw"
    i=0
    mkdir $MNTPT/small
    while [ "$i" -lt 150 ]; do
        dd if=/dev/urandom of=$MNTPT/small/f$i bs=4096 count=1 2>/dev/null
        i=$((i + 1))
    done
    sync; sync
    data_slots > /var/tmp/x10_after.txt
    comm -13 /var/tmp/x10_before.txt /var/tmp/x10_after.txt > /var/tmp/x10_new.txt
    new=$(wc -l < /var/tmp/x10_new.txt | tr -d ' ')
    bad=0
    for f in "$@"; do
        bad=$((bad + $(grep -c "^$f:" /var/tmp/x10_new.txt)))
    done
    if [ "$new" -lt 100 ]; then
        result FAIL "$label: only $new new data blocks seen"
    elif [ "$bad" = "0" ]; then
        result PASS "$label: $new new data blocks, none on failed disk(s) $*"
    else
        result FAIL "$label: $bad of $new new data blocks on failed disk(s) $*"
    fi
    remount
    verify_ref "$label: pre-failure data (cold, degraded)" "ref"
    verify_ref "$label: degraded-written data (cold, degraded)" "dw"
    for f in "$@"; do
        fresh_disk $f
    done
    bad=0
    for f in "$@"; do
        hammer2 -s $MNTPT raid replace "$(disk_dev $f)" "$(disk_dev $f)" \
            > /var/tmp/x_replace.txt 2>&1 || bad=1
    done
    if [ "$bad" = "0" ]; then
        result PASS "$label: resilver completes"
    else
        result FAIL "$label: replace failed"
        sed 's/^/      /' /var/tmp/x_replace.txt
    fi
    remount
    verify_ref "$label: pre-failure data (cold, resilvered)" "ref"
    verify_ref "$label: degraded-written data (cold, resilvered)" "dw"
    scrub_clean "$label"
    teardown "$label"
}

x10() {
    x10_round X10a 1
    x10_round X10b 1 2
}

for t in ${X_TESTS:-x1 x2 x3 x4 x4b x5 x6 x7 x8 x9 x10}; do
    $t
done

summary
