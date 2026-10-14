#!/bin/sh
# Group O: async sealing (oct-review.md P3 phase 2, vfs.hammer2.async_seal)
#
# A sealed row's P/Q and zero-column writes are started by the writer and
# waited for by a per-mount reaper kthread.  vfs.hammer2.inject_pq_defer_ms
# makes the reaper start them that many ms after the row was sealed (0
# releases them at once), so the media holds the old parity while the row
# is in flight; vfs.hammer2.pq_inflight counts such rows and
# vfs.hammer2.pq_test probes and reads their columns.
#
#   O1  on, plain: write, sync, remount, data intact, scrub clean, nothing
#       left in flight, no header written with parity in flight
#   O2  a degraded-style read (parity reconstruction of a data column)
#       and read_sibling of a row whose parity is held back wait for it and
#       return the right bytes; the media really held old parity meanwhile
#   O3  parity scrub (scrub -p) during that window repairs nothing
#   O4  flush ordering: with the parity held, sync does not finish and no
#       volume header reaches a disk; when released it finishes, the data
#       reads back, and with a disk failed it reconstructs from the parity
#   O5  raid replace while parity is in flight: the rebuilt disk is right
#   O6  P/Q write error: the reaper auto-fails the member, data intact
#   O7  umount -f while parity is in flight: returns, nothing left in
#       flight, the array mounts and the data is intact
#   O8  back-pressure: async_seal_max 2 holds writers, data intact
#   O9  async_seal turned off with rows in flight: they still complete

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group O: Async seal (NDISKS=$NDISKS) ==="

SMAX_DEF=$(sysctl -n vfs.hammer2.async_seal_max 2>/dev/null)
ASYNC_DEF=$(sysctl -n vfs.hammer2.async_seal 2>/dev/null)
o_off() {
    sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.inject_wio_disk_mask=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.async_seal_max=${SMAX_DEF:-16} > /dev/null 2>&1
    sysctl -w vfs.hammer2.async_seal=${ASYNC_DEF:-1} > /dev/null 2>&1
}
trap o_off EXIT

FRESH_SIZE=2g
sysctl -w vfs.hammer2.async_seal=1 > /dev/null
sysctl -w vfs.hammer2.async_seal_max=256 > /dev/null

inflight() { sysctl -n vfs.hammer2.pq_inflight; }
ctr() { sysctl -n vfs.hammer2.$1; }

# wait_inflight <secs>: until a row has parity in flight
wait_inflight() {
    local n=0
    while [ "$(inflight)" = "0" ]; do
        n=$((n + 1))
        [ "$n" -gt $(($1 * 20)) ] && return 1
        sleep 0.05
    done
    return 0
}

# wait_idle <secs>: until nothing is in flight
wait_idle() {
    local n=0
    while [ "$(inflight)" != "0" ]; do
        n=$((n + 1))
        [ "$n" -gt $(($1 * 10)) ] && return 1
        sleep 0.1
    done
    return 0
}

# hdr_sum: checksum of the first volume header copy of every disk
hdr_sum() {
    local i=0
    while [ "$i" -lt "$NDISKS" ]; do
        dd if=$(disk_dev $i) bs=65536 count=1 2>/dev/null
        i=$((i + 1))
    done | sha256
}

mkdata() {
    dd if=/dev/urandom of=$MNTPT/$1 bs=65536 count=$2 2>/dev/null
}

# --------------------------------------------------------------------
# O1
# --------------------------------------------------------------------
setup_fresh
check_v3
A0=$(ctr rz_pq_async); V0=$(ctr rz_pq_hdr_viol)
mkdata o1_a 256
mkdata o1_b 64
sha256 $MNTPT/o1_a > /var/tmp/v3_o1.txt
sha256 $MNTPT/o1_b >> /var/tmp/v3_o1.txt
sync; sync
if [ "$(ctr rz_pq_async)" -gt "$A0" ]; then
    result PASS "O1: rows were sealed asynchronously ($(( $(ctr rz_pq_async) - A0 )) rows)"
else
    result FAIL "O1: no row was sealed asynchronously"
fi
[ "$(inflight)" = "0" ] && result PASS "O1: nothing in flight after sync" ||
    result FAIL "O1: $(inflight) rows in flight after sync"
[ "$(ctr rz_pq_hdr_viol)" = "$V0" ] && result PASS "O1: no header with parity in flight" ||
    result FAIL "O1: header written with parity in flight"
remount
sha256 $MNTPT/o1_a > /var/tmp/v3_o1c.txt
sha256 $MNTPT/o1_b >> /var/tmp/v3_o1c.txt
diff -q /var/tmp/v3_o1.txt /var/tmp/v3_o1c.txt > /dev/null &&
    result PASS "O1: data intact after remount" || result FAIL "O1: data differs after remount"
scrub_clean "O1"
teardown "O1"

# --------------------------------------------------------------------
# O2 + O3: one window: the parity of 32 rows is held back for 5 s
# --------------------------------------------------------------------
setup_fresh
check_v3
sysctl -w vfs.hammer2.inject_pq_defer_ms=5000 > /dev/null
mkdata o2_a 64
sha256 $MNTPT/o2_a > /var/tmp/v3_o2.txt
RD0=$(ctr rz_pq_rd_wait)
sync &
SPID=$!
if wait_inflight 10; then
    result PASS "O2: rows with parity in flight appeared"
    sysctl -w vfs.hammer2.pq_test=probe > /dev/null 2>&1
    PR=$(sysctl -n vfs.hammer2.pq_test)
    set -- $(echo "$PR" | tr '=' ' ')
    # off=<n> p=<d> q=<d> d=<d>
    OFF=$2; PD=$4; QD=$6; DD=$8
    echo "  info: O2: probe '$PR'"
    sysctl -w vfs.hammer2.pq_test="raw $PD $OFF" > /dev/null 2>&1
    RAW0=$(sysctl -n vfs.hammer2.pq_test)
    sysctl -w vfs.hammer2.pq_test="deg $DD $OFF" > /dev/null 2>&1
    DEG=$(sysctl -n vfs.hammer2.pq_test)
    sysctl -w vfs.hammer2.pq_test="col $DD $OFF" > /dev/null 2>&1
    COL=$(sysctl -n vfs.hammer2.pq_test)
    sysctl -w vfs.hammer2.pq_test="raw $PD $OFF" > /dev/null 2>&1
    RAW1=$(sysctl -n vfs.hammer2.pq_test)
    echo "  info: O2: raw P before: $RAW0; deg: $DEG; col: $COL; raw P after: $RAW1"
    DCRC=$(echo "$DEG" | sed -n 's/^crc=\([0-9a-f]*\).*/\1/p')
    CCRC=$(echo "$COL" | sed -n 's/^crc=\([0-9a-f]*\).*/\1/p')
    DWAIT=$(echo "$DEG" | sed -n 's/.*waited_ms=\([0-9]*\).*/\1/p')
    R0=$(echo "$RAW0" | sed -n 's/^crc=\([0-9a-f]*\).*/\1/p')
    R1=$(echo "$RAW1" | sed -n 's/^crc=\([0-9a-f]*\).*/\1/p')
    if [ -n "$R0" ] && [ -n "$R1" ] && [ "$R0" != "$R1" ]; then
        result PASS "O2: the media held old parity while the row was in flight"
    else
        result FAIL "O2: parity on the media did not change across the window (raw $R0 / $R1): hold not engaged"
    fi
    if [ -n "$DCRC" ] && [ "$DCRC" = "$CCRC" ]; then
        result PASS "O2: reconstruction from parity of a row in flight is right"
    else
        result FAIL "O2: reconstruction differs from the column (deg '$DEG' col '$COL')"
    fi
    if [ "${DWAIT:-0}" -ge 500 ] && [ "$(ctr rz_pq_rd_wait)" -gt "$RD0" ]; then
        result PASS "O2: the reader waited for the parity (${DWAIT} ms)"
    else
        result FAIL "O2: the reader did not wait (waited_ms=$DWAIT)"
    fi
else
    result FAIL "O2: no row with parity in flight appeared"
    sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null
fi
wait $SPID
sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null
[ "$(inflight)" = "0" ] && result PASS "O2: nothing in flight after sync" ||
    result FAIL "O2: rows still in flight"

# O3: the parity pass while rows are in flight repairs nothing
sysctl -w vfs.hammer2.inject_pq_defer_ms=4000 > /dev/null
mkdata o3_a 64
sync &
SPID=$!
if wait_inflight 10; then
    hammer2 -s $MNTPT raid scrub -p > /var/tmp/v3_o3.txt 2>&1
    RC=$?
    BAD=$(awk '$1 == "parity_bad:" { print $2 }' /var/tmp/v3_o3.txt)
    REP=$(awk '$1 == "parity_repaired:" { print $2 }' /var/tmp/v3_o3.txt)
    UNL=$(awk '$1 == "parity_unlocatable:" { print $2 }' /var/tmp/v3_o3.txt)
    SK=$(awk '$1 == "parity_skipped:" { print $2 }' /var/tmp/v3_o3.txt)
    if [ "$RC" = 0 ] && [ "${BAD:-x}" = 0 ] && [ "${REP:-x}" = 0 ] && [ "${UNL:-x}" = 0 ]; then
        result PASS "O3: parity scrub during the window found and repaired nothing (skipped=$SK)"
    else
        result FAIL "O3: rc=$RC bad=$BAD repaired=$REP unloc=$UNL"
        sed 's/^/      /' /var/tmp/v3_o3.txt
    fi
else
    result FAIL "O3: no row with parity in flight appeared"
fi
sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null
wait $SPID
hammer2 -s $MNTPT raid scrub > /var/tmp/v3_o3b.txt 2>&1
BAD=$(awk '$1 == "parity_bad:" { print $2 }' /var/tmp/v3_o3b.txt)
ROWS=$(awk '$1 == "rows_checked:" { print $2 }' /var/tmp/v3_o3b.txt)
if [ "${BAD:-x}" = 0 ] && [ "${ROWS:-0}" -gt 0 ]; then
    result PASS "O3: full scrub afterwards clean (rows=$ROWS)"
else
    result FAIL "O3: full scrub afterwards bad=$BAD rows=$ROWS"
fi
sha256 $MNTPT/o2_a | diff -q - /var/tmp/v3_o2.txt > /dev/null &&
    result PASS "O3: earlier file intact" || result FAIL "O3: earlier file changed"
teardown "O2/O3"

# --------------------------------------------------------------------
# O4: flush ordering.  Parity held for 60 s (or until released): sync
# must not finish and no volume header may reach a disk.
# --------------------------------------------------------------------
setup_fresh
check_v3
mkdata o4_a 128
sha256 $MNTPT/o4_a > /var/tmp/v3_o4.txt
sync; sync
H0=$(hdr_sum)
V0=$(ctr rz_pq_hdr_viol)
sysctl -w vfs.hammer2.inject_pq_defer_ms=60000 > /dev/null
mkdata o4_b 128
sha256 $MNTPT/o4_b >> /var/tmp/v3_o4.txt
sync &
SPID=$!
if wait_inflight 10; then
    sleep 4
    if kill -0 $SPID 2>/dev/null; then
        result PASS "O4: sync waits while the parity is held back"
    else
        result FAIL "O4: sync finished with the parity held back"
    fi
    H1=$(hdr_sum)
    if [ "$H0" = "$H1" ]; then
        result PASS "O4: no volume header written while the parity is held back"
    else
        result FAIL "O4: a volume header reached the media with parity in flight"
    fi
else
    result FAIL "O4: no row with parity in flight appeared"
fi
sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null
n=0
while kill -0 $SPID 2>/dev/null && [ $n -lt 300 ]; do sleep 0.1; n=$((n + 1)); done
if kill -0 $SPID 2>/dev/null; then
    result FAIL "O4: sync did not finish after the release"
else
    result PASS "O4: sync finished after the release"
fi
wait $SPID
H2=$(hdr_sum)
[ "$H0" != "$H2" ] && result PASS "O4: the header was written after the release" ||
    result FAIL "O4: no header written by the sync"
[ "$(ctr rz_pq_hdr_viol)" = "$V0" ] && result PASS "O4: header-with-parity-in-flight counter 0" ||
    result FAIL "O4: counter says a header was written with parity in flight"
# the parity that was held is on the media and right: lose a disk, read cold
if fail_disk 1 "O4" && remount "$(degraded_spec 1)@V3TEST"; then
    sha256 $MNTPT/o4_a > /var/tmp/v3_o4c.txt
    sha256 $MNTPT/o4_b >> /var/tmp/v3_o4c.txt
    diff -q /var/tmp/v3_o4.txt /var/tmp/v3_o4c.txt > /dev/null &&
        result PASS "O4: data reconstructs from the parity (disk 1 out, cold)" ||
        result FAIL "O4: reconstructed data differs"
else
    result FAIL "O4: degraded remount failed"
fi
teardown "O4"

# --------------------------------------------------------------------
# O5: raid replace while parity is in flight
# --------------------------------------------------------------------
setup_fresh
check_v3
mkdata o5_a 128
sha256 $MNTPT/o5_a > /var/tmp/v3_o5.txt
sync; sync
fail_disk 2 "O5"
sysctl -w vfs.hammer2.inject_pq_defer_ms=4000 > /dev/null
mkdata o5_b 128
sha256 $MNTPT/o5_b >> /var/tmp/v3_o5.txt
sync &
SPID=$!
if wait_inflight 10; then
    fresh_disk 2
    if guarded 300 hammer2 -s $MNTPT raid replace "$(disk_dev 2)" "$(disk_dev 2)" > /var/tmp/v3_o5r.txt 2>&1; then
        result PASS "O5: raid replace with parity in flight completed"
    else
        result FAIL "O5: raid replace failed: $(head -3 /var/tmp/v3_o5r.txt | tr '\n' ' ')"
    fi
else
    result FAIL "O5: no row with parity in flight appeared"
fi
sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null
wait $SPID
sync
remount
sha256 $MNTPT/o5_a > /var/tmp/v3_o5c.txt
sha256 $MNTPT/o5_b >> /var/tmp/v3_o5c.txt
diff -q /var/tmp/v3_o5.txt /var/tmp/v3_o5c.txt > /dev/null &&
    result PASS "O5: data intact after the replace" || result FAIL "O5: data differs after the replace"
scrub_clean "O5"
teardown "O5"

# --------------------------------------------------------------------
# O6: P/Q write error with async on: the reaper auto-fails the member
# --------------------------------------------------------------------
setup_fresh
check_v3
mkdata o6_a 64
sha256 $MNTPT/o6_a > /var/tmp/v3_o6.txt
sync; sync
E0=$(ctr rz_pq_err)
sysctl -w vfs.hammer2.inject_wio_disk_mask=$((1 << 1)) > /dev/null
mkdata o6_b 128
sha256 $MNTPT/o6_b >> /var/tmp/v3_o6.txt
guarded 300 sync
sysctl -w vfs.hammer2.inject_wio_disk_mask=0 > /dev/null
if [ "$(ctr rz_pq_err)" -gt "$E0" ]; then
    result PASS "O6: the reaper saw failed parity writes ($(( $(ctr rz_pq_err) - E0 )))"
else
    result FAIL "O6: no failed parity write was counted"
fi
disk_state_is 1 FAILED && result PASS "O6: disk 1 auto-failed" || result FAIL "O6: disk 1 not failed"
[ "$(inflight)" = "0" ] && result PASS "O6: nothing left in flight" || result FAIL "O6: rows left in flight"
if remount; then
    sha256 $MNTPT/o6_a > /var/tmp/v3_o6c.txt
    sha256 $MNTPT/o6_b >> /var/tmp/v3_o6c.txt
    diff -q /var/tmp/v3_o6.txt /var/tmp/v3_o6c.txt > /dev/null &&
        result PASS "O6: data intact (degraded, cold)" || result FAIL "O6: data differs"
else
    result FAIL "O6: remount failed"
fi
fresh_disk 1
if hammer2 -s $MNTPT raid replace "$(disk_dev 1)" "$(disk_dev 1)" > /dev/null 2>&1; then
    remount
    scrub_clean "O6 after replace"
else
    result FAIL "O6: replace failed"
fi
teardown "O6"

# --------------------------------------------------------------------
# O7: umount -f with parity in flight
# --------------------------------------------------------------------
setup_fresh
check_v3
mkdata o7_a 128
sha256 $MNTPT/o7_a > /var/tmp/v3_o7.txt
sysctl -w vfs.hammer2.inject_pq_defer_ms=6000 > /dev/null
sync &
SPID=$!
if wait_inflight 10; then
    t0=$(date +%s)
    if guarded 120 umount -f $MNTPT; then
        result PASS "O7: umount -f with parity in flight returned ($(( $(date +%s) - t0 )) s)"
    else
        result FAIL "O7: umount -f failed or hung"
    fi
else
    result FAIL "O7: no row with parity in flight appeared"
    umount $MNTPT 2>/dev/null
fi
sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null
wait $SPID
mount | grep -q " $MNTPT " && { result FAIL "O7: still mounted"; umount -f $MNTPT; }
if mount -t hammer2 $PFSPATH $MNTPT; then
    sha256 $MNTPT/o7_a | diff -q - /var/tmp/v3_o7.txt > /dev/null &&
        result PASS "O7: remounts, data intact" || result FAIL "O7: data differs"
    scrub_clean "O7"
else
    result FAIL "O7: mount after umount -f failed"
fi
teardown "O7"

# --------------------------------------------------------------------
# O8: back-pressure
# --------------------------------------------------------------------
setup_fresh
check_v3
sysctl -w vfs.hammer2.async_seal_max=2 > /dev/null
B0=$(ctr rz_pq_bp_wait)
# Hold each row's parity 100 ms so rows pile up whatever the disks' speed
# (without it, fast media can drain every row before a third is sealed).
sysctl -w vfs.hammer2.inject_pq_defer_ms=100 > /dev/null
mkdata o8_a 64
sha256 $MNTPT/o8_a > /var/tmp/v3_o8.txt
sync; sync
sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null
if [ "$(ctr rz_pq_bp_wait)" -gt "$B0" ]; then
    result PASS "O8: writers were held by the bound ($(( $(ctr rz_pq_bp_wait) - B0 )) waits)"
else
    result FAIL "O8: bound of 2 never held a writer"
fi
sysctl -w vfs.hammer2.async_seal_max=256 > /dev/null
remount
sha256 $MNTPT/o8_a | diff -q - /var/tmp/v3_o8.txt > /dev/null &&
    result PASS "O8: data intact" || result FAIL "O8: data differs"
scrub_clean "O8"
teardown "O8"

# --------------------------------------------------------------------
# O9: async_seal switched off with rows in flight
# --------------------------------------------------------------------
setup_fresh
check_v3
sysctl -w vfs.hammer2.inject_pq_defer_ms=3000 > /dev/null
mkdata o9_a 64
sha256 $MNTPT/o9_a > /var/tmp/v3_o9.txt
sync &
SPID=$!
if wait_inflight 10; then
    sysctl -w vfs.hammer2.async_seal=0 > /dev/null
    mkdata o9_b 64
    sha256 $MNTPT/o9_b >> /var/tmp/v3_o9.txt
    sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null
    guarded 120 sync
    result PASS "O9: sync with async off and rows in flight finished"
else
    result FAIL "O9: no row with parity in flight appeared"
    sysctl -w vfs.hammer2.inject_pq_defer_ms=0 > /dev/null
fi
wait $SPID
sync
sysctl -w vfs.hammer2.async_seal=1 > /dev/null
[ "$(inflight)" = "0" ] && result PASS "O9: nothing in flight" || result FAIL "O9: rows in flight"
remount
sha256 $MNTPT/o9_a > /var/tmp/v3_o9c.txt
[ -f $MNTPT/o9_b ] && sha256 $MNTPT/o9_b >> /var/tmp/v3_o9c.txt
diff -q /var/tmp/v3_o9.txt /var/tmp/v3_o9c.txt > /dev/null &&
    result PASS "O9: data intact" || result FAIL "O9: data differs"
scrub_clean "O9"
teardown "O9"

sysctl -w vfs.hammer2.async_seal=${ASYNC_DEF:-1} > /dev/null
summary
