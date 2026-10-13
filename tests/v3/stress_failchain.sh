#!/bin/sh
# Stress (not in run_all): CHECK FAIL reporting vs read-path self-heal.
#
# oct-review §7 item 6: hammer2_characterize_failed_chain faulted on
# chain->dio->bp during group B (degraded mount, concurrent self-heal
# repair write).  This loops the B(N+2) profile: one disk absent from
# the mount spec, a second disk's data columns corrupt, several
# parallel readers so CHECK FAIL reports (chain.c) overlap with
# reconstruction, in-memory heals and the deferred repair writes
# (io.c).  Each round re-corrupts the media, because a heal repairs it.
#
#   STRESS_SECS   total run time (default 120)
#   STRESS_READERS parallel readers (default 4)
#
# Usage: sh stress_failchain.sh   (on the guest, under bin/h2dev-lock)

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

STRESS_SECS="${STRESS_SECS:-120}"
STRESS_READERS="${STRESS_READERS:-4}"
ABSENT=2
BAD=1

echo "=== Stress: failchain vs self-heal (NDISKS=$NDISKS, ${STRESS_SECS}s) ==="

setup_fresh
check_v3
i=0
while [ $i -lt 8 ]; do
    dd if=/dev/urandom of=$MNTPT/f$i bs=65536 count=96 2>/dev/null
    sha256 $MNTPT/f$i | awk '{print $NF}' > /var/tmp/sf_ref$i.txt
    i=$((i + 1))
done
sync; sync
umount $MNTPT

SPEC="$(degraded_spec $ABSENT)@V3TEST"
END=$(( $(date +%s) + STRESS_SECS ))
ROUND=0
BADROUNDS=0
while [ "$(date +%s)" -lt "$END" ]; do
    ROUND=$((ROUND + 1))
    # Corrupt 32 MB of data columns on disk BAD
    dd if=/dev/urandom of=$(disk_dev $BAD) bs=65536 count=512 \
        seek=$DATA_BLK conv=notrunc 2>/dev/null
    if ! mount -t hammer2 $SPEC $MNTPT; then
        result FAIL "round $ROUND: degraded mount refused"
        break
    fi
    r=0
    while [ $r -lt $STRESS_READERS ]; do
        (
            i=0
            while [ $i -lt 8 ]; do
                f=$(( (i + r) % 8 ))
                got=$(sha256 $MNTPT/f$f 2>/dev/null | awk '{print $NF}')
                [ "$got" = "$(cat /var/tmp/sf_ref$f.txt)" ] || exit 1
                i=$((i + 1))
            done
        ) &
        eval "RP$r=$!"
        r=$((r + 1))
    done
    r=0; ok=1
    while [ $r -lt $STRESS_READERS ]; do
        eval "p=\$RP$r"
        wait $p || ok=0
        r=$((r + 1))
    done
    [ $ok = 1 ] || BADROUNDS=$((BADROUNDS + 1))
    umount $MNTPT || { result FAIL "round $ROUND: umount failed"; break; }
done

if [ "$BADROUNDS" = 0 ] && [ "$FAIL" = 0 ]; then
    result PASS "stress: $ROUND rounds, all reads correct"
else
    result FAIL "stress: $BADROUNDS of $ROUND rounds returned wrong data"
fi
NCF=$(kmsg | grep -c "CHECK FAIL")
echo "  info: $NCF CHECK FAIL lines logged (rate limited)"
kmsg | grep -c "Fatal trap\|panic" | grep -qv '^0' && result FAIL "stress: panic text in dmesg"

mount -t hammer2 $PFSPATH $MNTPT 2>/dev/null && umount $MNTPT
echo "=== $PASS/$TOTAL PASS, $FAIL FAIL ==="
[ "$FAIL" = 0 ]
