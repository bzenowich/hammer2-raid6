#!/bin/sh
# crash_host.sh — power-loss test for the v3 RAID6 array, run on the HOST.
#
# Group I's `umount -f` still flushes, so it never tests recovery.  This
# resets the guest (QMP system_reset, no flush, no shutdown) in the
# middle of a write loop:
#
#   1. Guest: fresh array; optionally fail a disk (CRASH_FAIL=<idx>) so
#      the crash happens degraded.  Start a writer that loops
#        write file -> sync -> append "name sha256" to a log -> fsync log
#      so every logged file was durable on the array before it was logged.
#   2. Host: wait CRASH_AFTER seconds (default 20), system_reset.
#   3. Guest after boot: mount the array; every logged file must exist
#      with its logged hash, the array must scrub clean and dmesg must
#      have no CHECK FAIL.  Files written after the last log line may be
#      missing or short; they only have to read without error.
#
# CRASH_CHURN=1 tests freeing and reuse across the crash: the array is
# first filled to within ~768 MB of full (64 MB files p*, logged), and
# the writer writes 1-32 MB files, deletes the file 8 behind (logging
# "del <name>" before the rm, so it is never checked again) and runs
# `hammer2 bulkfree` every 4 files.  The allocator soon wraps into rows
# bulkfree freed, so the crash lands while freed rows are being reused;
# the prefill and every logged, undeleted file must still verify.
# CRASH_AFTER defaults to 180 s in this mode.
#
# Usage (from hammer2-raid6/): sh tests/v3/crash_host.sh
# Needs ./ssh.sh and bin/qmp.  Rounds: CRASH_ROUNDS (default 1).
# A guest reboot takes 1-10 min.

REPO=$(cd "$(dirname "$0")/../.." && pwd)
GSH="$REPO/ssh.sh"
QMP="$REPO/bin/qmp"
T=/root/hammer2-tests/v3
LOG=/var/tmp/crash_log.txt
CRASH_CHURN="${CRASH_CHURN:-}"
if [ -n "$CRASH_CHURN" ]; then
    CRASH_AFTER="${CRASH_AFTER:-180}"
else
    CRASH_AFTER="${CRASH_AFTER:-20}"
fi
CRASH_ROUNDS="${CRASH_ROUNDS:-1}"
CRASH_FAIL="${CRASH_FAIL:-}"
# CRASH_STALL=1: each writer iteration arms vfs.hammer2.raid6_put_stall_ms
# (oct-review.md §10.7), so a sync waits for a stalled column (R4) and the
# reset lands while rows are in flight.
CRASH_STALL="${CRASH_STALL:-}"
STALL_CMD=""
[ -n "$CRASH_STALL" ] && STALL_CMD="sysctl vfs.hammer2.raid6_put_stall_ms=300 > /dev/null;"
# CRASH_SUBCOL=<bytes>: set vfs.hammer2.raid6_subcol_max in the guest, write
# 1-32 KB files, and run a second writer that never syncs, so the reset
# lands with an open sub-column (oct-review.md section 11.4).
CRASH_SUBCOL="${CRASH_SUBCOL:-}"
SUB_CMD=""
WBS=65536
SUB_DAEMON=""
if [ -n "$CRASH_SUBCOL" ]; then
    SUB_CMD="sysctl vfs.hammer2.raid6_subcol_max=$CRASH_SUBCOL > /dev/null;"
    WBS=1024
    SUB_DAEMON="daemon -f sh -c 'j=0; while :; do dd if=/dev/urandom of=/mnt/v3test/u\$j bs=1k count=\$((j % 7 + 1)) 2>/dev/null; j=\$((j + 1)); done';"
fi
FAILS=0
# The module deploy.sh builds.  A reset guest boots without hammer2
# loaded, and a bare `kldload hammer2` would take /boot/kernel's copy,
# which only `deploy.sh install` updates.
KO=/usr/src/sys/vfs/hammer2/hammer2.ko

wait_guest() {
    local n=0
    while [ "$n" -lt 80 ]; do
        sleep 15
        "$GSH" 'true' > /dev/null 2>&1 && return 0
        n=$((n + 1))
    done
    echo "FATAL: guest did not come back within 20 min"
    exit 1
}

round=1
while [ "$round" -le "$CRASH_ROUNDS" ]; do
    echo "=== crash round $round (after ${CRASH_AFTER}s, fail='${CRASH_FAIL}', churn='${CRASH_CHURN}') ==="
    "$GSH" "cd $T && . ./common.sh && kldstat -q -m hammer2 || kldload $KO;
        cd $T && . ./common.sh && setup_fresh && rm -f $LOG && touch $LOG && sync &&
        if [ -n '$CRASH_FAIL' ]; then
            hammer2 -s \$MNTPT raid fail-disk \$(disk_dev $CRASH_FAIL) || exit 1
        fi &&
        if [ -n '$CRASH_CHURN' ]; then
            dd if=/dev/urandom of=/var/tmp/crash_chunk bs=1m count=64 2>/dev/null &&
            ph=\$(sha256 -q /var/tmp/crash_chunk) &&
            np=\$(( (\$(df -m \$MNTPT | awk 'NR == 2 { print \$4 }') - 768) / 64 )) &&
            k=0 && while [ \$k -lt \$np ]; do
                cp /var/tmp/crash_chunk \$MNTPT/p\$k || exit 1; k=\$((k + 1)); done &&
            sync && k=0 && while [ \$k -lt \$np ]; do
                echo \"p\$k \$ph\" >> $LOG; k=\$((k + 1)); done && fsync $LOG &&
            echo \"  prefilled \$np x 64 MB\" &&
            daemon -f sh -c 'i=0; while :; do
                dd if=/dev/urandom of=/mnt/v3test/c\$i bs=1m count=\$((i % 32 + 1)) 2>/dev/null;
                h=\$(sha256 -q /mnt/v3test/c\$i); sync;
                echo \"c\$i \$h\" >> $LOG; fsync $LOG;
                if [ \$i -ge 8 ]; then
                    echo \"del c\$((i - 8))\" >> $LOG; fsync $LOG;
                    rm -f /mnt/v3test/c\$((i - 8)); sync;
                fi;
                [ \$((i % 4)) = 3 ] && hammer2 bulkfree /mnt/v3test > /dev/null 2>&1;
                i=\$((i + 1)); done'
        else
            $SUB_CMD $SUB_DAEMON
            daemon -f sh -c 'i=0; while :; do
                $STALL_CMD
                dd if=/dev/urandom of=/mnt/v3test/c\$i bs=$WBS count=\$((i % 32 + 1)) 2>/dev/null;
                h=\$(sha256 -q /mnt/v3test/c\$i); sync;
                echo \"c\$i \$h\" >> $LOG; fsync $LOG; i=\$((i + 1)); done'
        fi" ||
        { echo "FATAL: guest setup failed"; exit 1; }
    sleep "$CRASH_AFTER"
    # The writer must still be making progress, or the "crash" below is
    # not the one under test (the guest may already have panicked).
    B1=$("$GSH" "wc -l < $LOG" 2>/dev/null | tr -d ' ')
    sleep 5
    B2=$("$GSH" "wc -l < $LOG" 2>/dev/null | tr -d ' ')
    if [ -z "$B2" ] || [ "${B2:-0}" -le "${B1:-0}" ]; then
        echo "  FAIL: writer stalled or guest dead before reset (logged ${B1:-?} -> ${B2:-?})"
        grep -a "panic:" "$REPO/logs/console.log" | tail -1 | sed 's/^/    /'
        FAILS=$((FAILS + 1))
        "$QMP" system_reset > /dev/null
        sleep 30
        wait_guest
        round=$((round + 1))
        continue
    fi
    echo "  ${B2} files logged; resetting guest"
    if [ -n "$CRASH_SUBCOL" ]; then
        "$GSH" "sysctl vfs.hammer2.raid6_subcol_max vfs.hammer2.rz_subcol_open vfs.hammer2.rz_subcol_close vfs.hammer2.rz_subcol_chains" 2>/dev/null | sed 's/^/    pre-reset: /'
    fi
    "$QMP" system_reset > /dev/null
    sleep 30
    wait_guest

    "$GSH" "cd $T && . ./common.sh && (kldstat -q -m hammer2 || kldload $KO) &&
        kmsg_clear; mkdir -p \$MNTPT;
        if ! mount -t hammer2 \$PFSPATH \$MNTPT; then echo '  FAIL: mount after crash'; exit 1; fi;
        n=0; bad=0;
        awk '\$1 == \"del\" { delete h[\$2]; next } { h[\$1] = \$2 }
            END { for (f in h) print f, h[f] }' $LOG > /var/tmp/crash_live.txt;
        while read f h; do
            n=\$((n + 1));
            [ \"\$(sha256 -q \$MNTPT/\$f 2>/dev/null)\" = \"\$h\" ] || { bad=\$((bad + 1)); echo \"    lost: \$f\"; };
        done < /var/tmp/crash_live.txt;
        if [ \$n -gt 0 ] && [ \$bad = 0 ]; then result PASS \"all \$n logged (synced) files intact\";
        else result FAIL \"\$bad of \$n logged files lost or wrong\"; fi;
        for f in \$MNTPT/c*; do cat \$f > /dev/null 2>&1 || result FAIL \"unlogged file \$f unreadable\"; done;
        scrub_clean 'post-crash';
        check_no_checkfail 'post-crash';
        umount \$MNTPT; summary" || FAILS=$((FAILS + 1))
    round=$((round + 1))
done
[ "$FAILS" = 0 ]
