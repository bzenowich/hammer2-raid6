#!/bin/sh
# Group Q: many small files on sub-column allocation (oct-review.md
# section 11.5 step 4; vfs.hammer2.raid6_subcol_max, default 32768).
#
#   Q1  4000 small files, remove half, bulkfree x2, remount: every
#       survivor verified against the source, every removed file gone,
#       scrub clean
#   Q2  the same array with one, then two disks lost (failed and wiped):
#       every survivor read back
#   Q3  corrupt one sub-block on the media: scrub finds exactly that
#       chain (bad == 1) and repairs exactly it, the data reads back
#   Q4  overwrite in the same TXG (the chain's column still pending) and
#       in the next (closed): same size, smaller, bigger
#   Q5  churn: create, remove half, bulkfree x2, repeat.  REPORTS the
#       columns allocated, live and partially live; not asserted (bulkfree
#       per column is step 5 of 11.5, 11.6 Q5)
#
# Environment: SUBCOL_MAX (default: the kernel's), NFILES (default 4000),
# CHURN_ROUNDS (default 4).

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group Q: Small files on sub-columns (NDISKS=$NDISKS) ==="

MAX_SAVED=$(sysctl -n vfs.hammer2.raid6_subcol_max 2>/dev/null)
if [ -z "$MAX_SAVED" ]; then
    echo "  SKIP: no vfs.hammer2.raid6_subcol_max in this kernel"
    exit 77
fi
restore_max() {
    sysctl -w vfs.hammer2.raid6_subcol_max="$MAX_SAVED" > /dev/null 2>&1
    rm -rf $SRC
}
SRC=/var/tmp/q_src
trap restore_max EXIT INT TERM
[ -n "$SUBCOL_MAX" ] &&
    sysctl -w vfs.hammer2.raid6_subcol_max="$SUBCOL_MAX" > /dev/null
echo "  raid6_subcol_max=$(sysctl -n vfs.hammer2.raid6_subcol_max)"

NFILES=${NFILES:-4000}
CHURN_ROUNDS=${CHURN_ROUNDS:-4}

ctr() { sysctl -n vfs.hammer2.$1; }

# gen <dir> <first> <count> <seed>: files f<i> of 1, 2, 4 or 8 KB (and a
# 32 KB one every 16th) of random hex, deterministic.
gen() {
    mkdir -p "$1"
    awk -v d="$1" -v a=$2 -v n=$3 -v s=$4 'BEGIN {
        srand(s)
        for (i = a; i < a + n; i++) {
            f = sprintf("%s/f%d", d, i)
            sz = (i % 16 == 15) ? 32768 : 1024 * (2 ^ (i % 4))
            for (j = 0; j < sz / 8; j++)
                printf "%08x", int(rand() * 4294967296) > f
            close(f)
        }
    }'
}

# survivors: the source sums minus the files whose number is odd.
even_sums() { grep -E '\(f[0-9]*[02468]\)' /var/tmp/q_src.sum; }

# verify <label> <expected sums file>
verify() {
    (cd $MNTPT && ls | sort | grep '^f' | xargs sha256 2>&1) > /var/tmp/q_check.sum
    if sort "$2" | diff -q - /var/tmp/q_check.sum > /dev/null 2>&1 ||
       diff -q "$2" /var/tmp/q_check.sum > /dev/null 2>&1; then
        result PASS "$1"
    else
        result FAIL "$1: $(diff "$2" /var/tmp/q_check.sum | grep -c '^[<>]') differing lines"
        diff "$2" /var/tmp/q_check.sum | head -5 | sed 's/^/      /'
    fi
}

bulkfree2() {
    hammer2 bulkfree $MNTPT > /dev/null 2>&1
    hammer2 bulkfree $MNTPT > /dev/null 2>&1
}

# layout: "disk column offset size" (bytes) for every data chain.
layout() {
    hammer2 -q show $DEVSPEC 2>/dev/null |
        awk '$1 ~ /^data\./ && $4 ~ /^vol=/ { sub(/^vol=/, "", $4); print $4, $2 }' |
        while read d off; do
            v=$((0x${off#0x}))
            echo "$d $((v & ~0xffff)) $((v & 0xffc0)) $((1 << (v & 63)))"
        done | sort -u
}

scrub_field() { awk -v f="$1" '$1 == f":" { print $2 }' < "$2"; }

# --- Q1
setup_fresh
check_v3
rm -rf $SRC
gen $SRC 0 $NFILES 7
(cd $SRC && ls | sort | xargs sha256) > /var/tmp/q_src.sum 2>&1
C0=$(ctr rz_cols_alloc)
cp -R $SRC/. $MNTPT/
sync; sync
C1=$(ctr rz_cols_alloc)
echo "  INFO: Q1: $NFILES files took $((C1 - C0)) columns"
i=1
while [ $i -lt $NFILES ]; do
    echo $MNTPT/f$i
    i=$((i + 2))
done | xargs rm -f
sync
bulkfree2
remount
even_sums > /var/tmp/q_even.sum
verify "Q1: survivors after rm of half, bulkfree x2, remount" /var/tmp/q_even.sum
GONE=$(ls $MNTPT | grep -c '^f[0-9]*[13579]$')
if [ "$GONE" = 0 ]; then
    result PASS "Q1: removed files are gone"
else
    result FAIL "Q1: $GONE removed files still present"
fi
scrub_clean "Q1"
if [ "$(ctr rz_subcol_stale)" = 0 ] && [ "$(ctr rz_put_anomaly)" = 0 ]; then
    result PASS "Q1: no safety-net release, no double put"
else
    result FAIL "Q1: stale=$(ctr rz_subcol_stale) put_anomaly=$(ctr rz_put_anomaly)"
fi

# --- Q2: reads with two disks lost (same array)
for DISK in 1 2; do
    if fail_disk "$DISK" "Q2"; then
        sync
        umount $MNTPT
        corrupt_data_zone "$DISK"
        if mount -t hammer2 $PFSPATH $MNTPT; then
            if [ "$DISK" = 1 ]; then
                verify "Q2: one disk lost, every survivor read" /var/tmp/q_even.sum
            else
                verify "Q2: two disks lost, every survivor read" /var/tmp/q_even.sum
            fi
        else
            result FAIL "Q2: mount refused with disk $DISK lost"
            break
        fi
    else
        break
    fi
done
kmsg_clear
teardown "Q2"

# --- Q3: one corrupt sub-block, scrub finds and repairs exactly it
setup_fresh
check_v3
cp -R $SRC/. $MNTPT/
sync; sync
remount
# A chain at a non-zero offset that is not the first in its column; pick
# one of 4 KB or more so the 1 KB hit stays inside it.
layout | awk '$3 != 0 && $4 >= 4096 { print $1, $2 + $3 }' > /var/tmp/q_sub.txt
NSUB=$(wc -l < /var/tmp/q_sub.txt | tr -d ' ')
if [ "${NSUB:-0}" -gt 10 ]; then
    result PASS "Q3: $NSUB chains of >= 4 KB at a non-zero offset"
else
    result FAIL "Q3: only ${NSUB:-0} sub-block chains"
fi
set -- $(sed -n 7p /var/tmp/q_sub.txt)
D3=$1; O3=$2
umount $MNTPT
dd if=/dev/urandom of=$(disk_dev "$D3") bs=1024 count=1 seek=$((O3 / 1024)) \
    conv=notrunc 2>/dev/null
mount -t hammer2 $PFSPATH $MNTPT
kmsg_clear
hammer2 -s $MNTPT raid scrub > /var/tmp/q3_scrub.txt 2>&1
BAD=$(scrub_field brefs_bad /var/tmp/q3_scrub.txt)
REP=$(scrub_field brefs_repaired /var/tmp/q3_scrub.txt)
UNREP=$(scrub_field brefs_unrepairable /var/tmp/q3_scrub.txt)
if [ "$BAD" = 1 ] && [ "$REP" = 1 ] && [ "$UNREP" = 0 ]; then
    result PASS "Q3: scrub found and repaired exactly one chain (disk $D3 off $O3)"
else
    result FAIL "Q3: scrub bad=$BAD repaired=$REP unrepairable=$UNREP (want 1/1/0)"
    sed 's/^/      /' /var/tmp/q3_scrub.txt
fi
scrub_clean "Q3 second pass"
remount
verify "Q3: every file reads back after the repair" /var/tmp/q_src.sum
kmsg_clear
teardown "Q3"

# --- Q4: overwrite in the same TXG and in the next
setup_fresh
check_v3
rm -rf $SRC
gen $SRC 0 200 11
mkdir -p $SRC.b $SRC.c
gen $SRC.b 0 200 12
gen $SRC.c 0 200 13
rm -f $SRC.b/f*[02468]; rm -f $SRC.c/f*[13579]
# step 1: write, then overwrite with new content in the same TXG
cp $SRC/f* $MNTPT/
cp $SRC.b/f* $MNTPT/
# the files whose number is odd were not in $SRC.b: they hold $SRC content
# and the even ones hold $SRC.b content
cp $SRC.c/f* $MNTPT/ 2>/dev/null
# expected content: odd files from $SRC.c, even files from $SRC.b
(cd $MNTPT && ls | sort | xargs sha256) > /var/tmp/q4_pending.sum
(
  for f in $(cd $SRC.b && ls); do (cd $SRC.b && sha256 $f); done
  for f in $(cd $SRC.c && ls); do (cd $SRC.c && sha256 $f); done
) | sort > /var/tmp/q4_exp1.sum
sort /var/tmp/q4_pending.sum | diff -q - /var/tmp/q4_exp1.sum > /dev/null &&
    result PASS "Q4: same-TXG overwrite reads back (pending column)" ||
    { result FAIL "Q4: same-TXG overwrite content"
      sort /var/tmp/q4_pending.sum | diff - /var/tmp/q4_exp1.sum | head -6 | sed 's/^/      /'
      wc -l /var/tmp/q4_pending.sum /var/tmp/q4_exp1.sum | sed 's/^/      /'; }
sync
# step 2: after the sync the columns are closed: overwrite in place with
# a smaller, same-size and a bigger file
i=0
while [ $i -lt 200 ]; do
    case $((i % 3)) in
    0) dd if=/dev/urandom of=$MNTPT/f$i bs=512 count=1 2>/dev/null ;;
    1) dd if=/dev/urandom of=$MNTPT/f$i bs=1k count=$((1 << (i % 4))) 2>/dev/null ;;
    2) dd if=/dev/urandom of=$MNTPT/f$i bs=1k count=20 2>/dev/null ;;
    esac
    i=$((i + 1))
done
(cd $MNTPT && ls | sort | xargs sha256) > /var/tmp/q4_exp2.sum
sync
remount
(cd $MNTPT && ls | sort | xargs sha256) > /var/tmp/q4_got2.sum
if diff -q /var/tmp/q4_exp2.sum /var/tmp/q4_got2.sum > /dev/null; then
    result PASS "Q4: closed-column overwrites read back after remount"
else
    result FAIL "Q4: closed-column overwrites differ"
fi
scrub_clean "Q4"
if [ "$(ctr rz_subcol_stale)" = 0 ] && [ "$(ctr rz_put_anomaly)" = 0 ]; then
    result PASS "Q4: no safety-net release, no double put"
else
    result FAIL "Q4: stale=$(ctr rz_subcol_stale) put_anomaly=$(ctr rz_put_anomaly)"
fi
rm -rf $SRC.b $SRC.c
kmsg_clear
teardown "Q4"

# --- Q5: churn.  Reported, not asserted.
setup_fresh
check_v3
CA0=$(ctr rz_cols_alloc)
round=0
while [ $round -lt $CHURN_ROUNDS ]; do
    rm -rf $SRC
    gen $SRC $((round * 1000)) 1000 $((round + 20))
    cp -R $SRC/. $MNTPT/
    sync; sync
    # remove half of everything now present
    ls $MNTPT | grep '^f' | awk 'NR % 2 == 0 { print "'$MNTPT'/" $1 }' | xargs rm -f
    sync
    bulkfree2
    layout > /var/tmp/q5_layout.txt
    awk -v r=$round -v alloc=$(( $(ctr rz_cols_alloc) - CA0 )) '
        { k = $1 ":" $2; used[k] += $4 }
        END { for (k in used) { live++; b += used[k]; if (used[k] < 65536) part++ }
              printf "  INFO: Q5 round %d: columns allocated so far %d, live %d, dead-or-reused %d, partially live %d (mean occupancy %.0f%% of live columns)\n",
                     r + 1, alloc, live, alloc - live, part, live ? 100 * b / (live * 65536) : 0 }' /var/tmp/q5_layout.txt
    round=$((round + 1))
done
remount
(cd $MNTPT && ls | sort | xargs sha256 2>&1 | grep -c 'No such\|rror') > /var/tmp/q5_err.txt
if [ "$(cat /var/tmp/q5_err.txt)" = 0 ]; then
    result PASS "Q5: churned tree reads back without errors"
else
    result FAIL "Q5: read errors after churn"
fi
scrub_clean "Q5"
teardown "Q5"

summary
