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
# Usage (from hammer2-raid6/): sh tests/v3/crash_host.sh
# Needs ./ssh.sh and bin/qmp.  Rounds: CRASH_ROUNDS (default 1).
# A guest reboot takes 1-10 min.

REPO=$(cd "$(dirname "$0")/../.." && pwd)
GSH="$REPO/ssh.sh"
QMP="$REPO/bin/qmp"
T=/root/hammer2-tests/v3
LOG=/var/tmp/crash_log.txt
CRASH_AFTER="${CRASH_AFTER:-20}"
CRASH_ROUNDS="${CRASH_ROUNDS:-1}"
CRASH_FAIL="${CRASH_FAIL:-}"
FAILS=0

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
    echo "=== crash round $round (after ${CRASH_AFTER}s, fail='${CRASH_FAIL}') ==="
    "$GSH" "cd $T && . ./common.sh && kldstat -q -m hammer2 || kldload hammer2;
        cd $T && . ./common.sh && setup_fresh && rm -f $LOG && touch $LOG && sync &&
        if [ -n '$CRASH_FAIL' ]; then
            hammer2 -s \$MNTPT raid fail-disk \$(disk_dev $CRASH_FAIL) || exit 1
        fi &&
        daemon -f sh -c 'i=0; while :; do
            dd if=/dev/urandom of=/mnt/v3test/c\$i bs=65536 count=\$((i % 32 + 1)) 2>/dev/null;
            h=\$(sha256 -q /mnt/v3test/c\$i); sync;
            echo \"c\$i \$h\" >> $LOG; fsync $LOG; i=\$((i + 1)); done'" ||
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
    "$QMP" system_reset > /dev/null
    sleep 30
    wait_guest

    "$GSH" "cd $T && . ./common.sh && (kldstat -q -m hammer2 || kldload hammer2) &&
        kmsg_clear; mkdir -p \$MNTPT;
        if ! mount -t hammer2 \$PFSPATH \$MNTPT; then echo '  FAIL: mount after crash'; exit 1; fi;
        n=0; bad=0;
        while read f h; do
            n=\$((n + 1));
            [ \"\$(sha256 -q \$MNTPT/\$f 2>/dev/null)\" = \"\$h\" ] || { bad=\$((bad + 1)); echo \"    lost: \$f\"; };
        done < $LOG;
        if [ \$n -gt 0 ] && [ \$bad = 0 ]; then result PASS \"all \$n logged (synced) files intact\";
        else result FAIL \"\$bad of \$n logged files lost or wrong\"; fi;
        for f in \$MNTPT/c*; do cat \$f > /dev/null 2>&1 || result FAIL \"unlogged file \$f unreadable\"; done;
        scrub_clean 'post-crash';
        check_no_checkfail 'post-crash';
        umount \$MNTPT; summary" || FAILS=$((FAILS + 1))
    round=$((round + 1))
done
[ "$FAILS" = 0 ]
