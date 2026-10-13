#!/bin/sh
# Group Y: row-ordered parity scrub (oct-review.md S6)
#
# `hammer2 raid scrub` walks the tree (checksums) and then reads every
# allocated row on all members, recomputes P and Q and compares.  A
# mismatch is localised with the syndrome and repaired.  The options -t
# (skip the parity pass) and -p (skip the tree walk) split the two.
#
#   Y1  filled array: parity_bad=0, rows_checked>0, clean second scrub
#   Y2  raw corruption of a P column: detected, repaired
#   Y3  raw corruption of a Q column: detected, repaired
#   Y4  a zero-filled column of a sealed partial row: detected, repaired
#   Y5  a data column, found by `scrub -p` only, confirmed by chain csum
#   Y6  a data column and its P in the same row, full scrub
#   Y7  scrub concurrent with writes and deletes: no false repairs
#   Y8  degraded array: the parity pass is skipped

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group Y: Parity scrub (NDISKS=$NDISKS) ==="

OUT=/var/tmp/v3_y_scrub.txt

# yfield <name> <file>
yfield() {
    awk -v f="$1" '$1 == f":" { print $2 }' < "$2"
}

# yscrub <label> <args...>: run a scrub into $OUT, return its status
yscrub() {
    hammer2 -s $MNTPT raid scrub "$@" > $OUT 2>&1
}

# ycheck <label> <rc> <rows> <bad> <repaired> <unloc>: assert counters;
# "any" skips a field, "pos" requires > 0.
ycheck() {
    local label="$1" want_rc="$2" w_rows="$3" w_bad="$4" w_rep="$5" w_unl="$6"
    local rc=$7 rows bad rep unl bb ub ok=1
    rows=$(yfield rows_checked $OUT)
    bad=$(yfield parity_bad $OUT)
    rep=$(yfield parity_repaired $OUT)
    unl=$(yfield parity_unlocatable $OUT)
    bb=$(yfield brefs_bad $OUT)
    ub=$(yfield brefs_unrepairable $OUT)
    [ "$rc" = "$want_rc" ] || ok=0
    case "$w_rows" in any) ;; pos) [ "${rows:-0}" -gt 0 ] || ok=0 ;;
        *) [ "${rows:-x}" = "$w_rows" ] || ok=0 ;; esac
    case "$w_bad" in any) ;; *) [ "${bad:-x}" = "$w_bad" ] || ok=0 ;; esac
    case "$w_rep" in any) ;; *) [ "${rep:-x}" = "$w_rep" ] || ok=0 ;; esac
    case "$w_unl" in any) ;; *) [ "${unl:-x}" = "$w_unl" ] || ok=0 ;; esac
    [ "${ub:-0}" = "0" ] || ok=0
    if [ $ok = 1 ]; then
        result PASS "$label (rows=$rows bad=$bad repaired=$rep unloc=$unl brefs_bad=$bb)"
    else
        result FAIL "$label (rc=$rc rows=$rows bad=$bad repaired=$rep unloc=$unl brefs_bad=$bb unrep=$ub)"
        sed 's/^/      /' $OUT
    fi
}

# first_chain: "disk phys" of the first data chain on a whole column
# (no sub-column offset).
first_chain() {
    hammer2 -q show $DEVSPEC 2>/dev/null |
        awk '$1 ~ /^data\./ && $4 ~ /^vol=/ { sub(/^vol=/, "", $4); print $4, $2 }' |
        while read d off; do
            off=${off#0x}
            v=$((0x$off & ~0x3f))
            if [ $((v & 0xfc00)) -eq 0 ] && [ $v -ge 4194304 ]; then
                echo "$d $v"
                break
            fi
        done
}

# row geometry for a column at phys $1: sets Y_BASE Y_SLOT Y_P Y_Q
row_of() {
    Y_BASE=$(($1 & ~0xffff))
    Y_SLOT=$(((Y_BASE - 4194304) / 65536))
    Y_P=$((Y_SLOT % NDISKS))
    Y_Q=$(((Y_P + 1) % NDISKS))
}

# ycorrupt <disk> <base>: 64 KB of random data over the column
ycorrupt() {
    dd if=/dev/urandom of=$(disk_dev "$1") bs=65536 count=1 \
        seek=$(($2 / 65536)) conv=notrunc 2>/dev/null
}

yfile() {
    dd if=/dev/urandom of=$MNTPT/$1 bs=65536 count=$2 2>/dev/null
}

# yp_case <label> <which> : corrupt P or Q of the row of the first chain
yp_case() {
    local label="$1" which="$2" tgt
    setup_fresh
    check_v3
    yfile y_a 64
    sync; sync
    sha256 $MNTPT/y_a > /var/tmp/v3_y.txt
    set -- $(first_chain)
    row_of $2
    if [ $which = P ]; then tgt=$Y_P; else tgt=$Y_Q; fi
    umount $MNTPT
    ycorrupt $tgt $Y_BASE
    mount -t hammer2 $PFSPATH $MNTPT
    yscrub; ycheck "$label: scrub finds and repairs $which (slot $Y_SLOT disk $tgt)" 0 pos 1 1 0 $?
    yscrub; ycheck "$label: second scrub clean" 0 pos 0 0 0 $?
    sha256 $MNTPT/y_a | diff -q - /var/tmp/v3_y.txt > /dev/null 2>&1 &&
        result PASS "$label: file intact" || result FAIL "$label: file changed"
    remount
    scrub_clean "$label"
    teardown
}

# Y1
setup_fresh
check_v3
yfile y_a 128; yfile y_b 64
dd if=/dev/zero of=$MNTPT/y_z bs=65536 count=32 2>/dev/null
sync; sync
yscrub; ycheck "Y1: filled array, parity pass clean" 0 pos 0 0 0 $?
yscrub; ycheck "Y1: second scrub clean" 0 pos 0 0 0 $?
yscrub -t; R=$?
RW=$(yfield rows_checked $OUT)
[ $R = 0 ] && [ "${RW:-x}" = 0 ] && result PASS "Y1: scrub -t skips the parity pass" ||
    result FAIL "Y1: scrub -t rc=$R rows_checked=$RW"
SYS=$(sysctl -n vfs.hammer2.scrub_pq_rows 2>/dev/null)
[ "${SYS:-0}" -gt 0 ] && result PASS "Y1: sysctl scrub_pq_rows=$SYS" ||
    result FAIL "Y1: sysctl scrub_pq_rows=$SYS"
teardown

yp_case Y2 P
yp_case Y3 Q

# Y4: an unreserved column of a sealed partial row is zero on media and
# covered by P and Q; corrupt it.
setup_fresh
check_v3
yfile y_a 1
sync; sync
sha256 $MNTPT/y_a > /var/tmp/v3_y.txt
set -- $(first_chain)
CHAIN_DISK=$1
row_of $2
Y4D=-1
i=0
while [ $i -lt $NDISKS ]; do
    if [ $i != $Y_P ] && [ $i != $Y_Q ] && [ $i != $CHAIN_DISK ]; then
        Y4D=$i
        break
    fi
    i=$((i + 1))
done
if [ $Y4D -lt 0 ]; then
    result FAIL "Y4: no spare data column in row $Y_SLOT"
else
    umount $MNTPT
    ycorrupt $Y4D $Y_BASE
    mount -t hammer2 $PFSPATH $MNTPT
    yscrub; ycheck "Y4: zero column of a partial row (slot $Y_SLOT disk $Y4D)" 0 pos 1 1 0 $?
    yscrub; ycheck "Y4: second scrub clean" 0 pos 0 0 0 $?
    sha256 $MNTPT/y_a | diff -q - /var/tmp/v3_y.txt > /dev/null 2>&1 &&
        result PASS "Y4: file intact" || result FAIL "Y4: file changed"
fi
teardown

# Y5: a live data column, tree walk skipped.
setup_fresh
check_v3
yfile y_a 64
sync; sync
sha256 $MNTPT/y_a > /var/tmp/v3_y.txt
set -- $(first_chain)
Y5D=$1
row_of $2
umount $MNTPT
ycorrupt $Y5D $Y_BASE
mount -t hammer2 $PFSPATH $MNTPT
yscrub -p; ycheck "Y5: data column found by the parity pass alone (slot $Y_SLOT disk $Y5D)" 0 pos 1 1 0 $?
yscrub; ycheck "Y5: full scrub clean" 0 pos 0 0 0 $?
remount
sha256 $MNTPT/y_a | diff -q - /var/tmp/v3_y.txt > /dev/null 2>&1 &&
    result PASS "Y5: file intact after cold remount" || result FAIL "Y5: file changed"
teardown

# Y6: a data column and the P column of the same row.
setup_fresh
check_v3
yfile y_a 64
sync; sync
sha256 $MNTPT/y_a > /var/tmp/v3_y.txt
set -- $(first_chain)
Y6D=$1
row_of $2
umount $MNTPT
ycorrupt $Y6D $Y_BASE
ycorrupt $Y_P $Y_BASE
mount -t hammer2 $PFSPATH $MNTPT
yscrub; Y6RC=$?
Y6B=$(yfield brefs_bad $OUT)
if [ $Y6RC = 0 ] && [ "${Y6B:-0}" -gt 0 ]; then
    result PASS "Y6: data + P damage healed (brefs_bad=$Y6B, rc 0)"
else
    result FAIL "Y6: rc=$Y6RC brefs_bad=$Y6B"
    sed 's/^/      /' $OUT
fi
yscrub; ycheck "Y6: second scrub clean" 0 pos 0 0 0 $?
remount
sha256 $MNTPT/y_a | diff -q - /var/tmp/v3_y.txt > /dev/null 2>&1 &&
    result PASS "Y6: file intact after cold remount" || result FAIL "Y6: file changed"
teardown

# Y7: scrub while the filesystem is being written and truncated; the
# in-flight rows must not be "repaired".
setup_fresh
check_v3
i=0
while [ $i -lt 20 ]; do
    yfile y_w$i 16
    i=$((i + 1))
done
sync
(
    j=0
    while [ $j -lt 60 ]; do
        dd if=/dev/urandom of=$MNTPT/y_c$((j % 8)) bs=65536 count=8 2>/dev/null
        rm -f $MNTPT/y_w$((j % 20))
        dd if=/dev/urandom of=$MNTPT/y_w$((j % 20)) bs=65536 count=16 2>/dev/null
        j=$((j + 1))
    done
) &
WPID=$!
yscrub; Y7RC=$?
yscrub; Y7RC2=$?
wait $WPID
Y7R=$(yfield parity_repaired $OUT)
Y7U=$(yfield parity_unlocatable $OUT)
if [ $Y7RC = 0 ] && [ $Y7RC2 = 0 ] && [ "${Y7R:-x}" = 0 ] && [ "${Y7U:-x}" = 0 ]; then
    result PASS "Y7: scrubs under write load, no false repairs"
else
    result FAIL "Y7: rc=$Y7RC/$Y7RC2 repaired=$Y7R unloc=$Y7U"
    sed 's/^/      /' $OUT
fi
sync; sync
yscrub; ycheck "Y7: quiesced scrub clean" 0 pos 0 0 0 $?
teardown

# Y8: degraded arrays skip the pass.
setup_fresh
check_v3
yfile y_a 32
sync; sync
if fail_disk 1 "Y8"; then
    yscrub; Y8RC=$?
    Y8R=$(yfield rows_checked $OUT)
    [ "${Y8R:-x}" = 0 ] && result PASS "Y8: degraded, parity pass skipped (rc=$Y8RC)" ||
        result FAIL "Y8: degraded rows_checked=$Y8R"
fi
teardown

summary
