#!/bin/sh
# Group V: sub-column allocation (oct-review.md section 11.5 step 3,
# vfs.hammer2.raid6_subcol_max).
#
# With raid6_subcol_max set, data chains of 1-32 KB share a column: an
# open sub-column per generation, size-aligned placement, one pin on the
# dio, closed when full, when a chain finds no range, or at the flush.
# This group turns it on (and restores the sysctl on exit) and checks
# the layout on the media and every path that reads or rewrites it.
#
#   V1  seven chains of 32+16+8+4+2+1+1 KB in one TXG tile one column
#   V2  mixed sizes: ranges aligned to their size, never overlapping;
#       far fewer columns than chains; cold read, scrub clean
#   V3  every sync closes the open sub-column (open == close), no
#       ticket is ever released by a safety net
#   V4  rewrite and grow in the same TXG (open sub-column) and in the
#       next (closed): content, scrub
#   V5  rm of half the files before they are written
#   V6  corrupt one sub-block: read-path self-heal, then scrub repair
#   V7  failed disk: degraded read, dual reconstruction, resilver
#
# Environment: SUBCOL_MAX (default 32768).

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group V: Sub-column allocation (NDISKS=$NDISKS) ==="

MAX_SAVED=$(sysctl -n vfs.hammer2.raid6_subcol_max 2>/dev/null)
if [ -z "$MAX_SAVED" ]; then
    echo "  SKIP: no vfs.hammer2.raid6_subcol_max in this kernel"
    exit 77
fi
restore_max() {
    sysctl -w vfs.hammer2.raid6_subcol_max="$MAX_SAVED" > /dev/null 2>&1
}
trap restore_max EXIT
sysctl -w vfs.hammer2.raid6_subcol_max="${SUBCOL_MAX:-32768}" > /dev/null

ctr() { sysctl -n vfs.hammer2.$1; }

# Sizes in KB, cycled; a file's block is its size rounded up to a power
# of two, so these give chains of 1, 2, 4, 8, 16 and 32 KB.
SIZES="1 4 2 32 8 1 16 2 4 1 8 2 1 4 16 1 2 32 1 8"

# write_mixed <prefix> <count>: random (incompressible) files.
write_mixed() {
    local p="$1" count="$2" n=0 sz
    set -- $SIZES
    while [ $n -lt $count ]; do
        eval sz=\${$((n % 20 + 1))}
        dd if=/dev/urandom of=$MNTPT/${p}_$n bs=${sz}k count=1 2>/dev/null
        n=$((n + 1))
    done
}

sums() { sha256 $MNTPT/${1}_* > /var/tmp/v3_${1}.txt 2>&1; }

verify() {
    local label="$1" p="$2"
    sha256 $MNTPT/${p}_* > /var/tmp/v3_check.txt 2>&1
    if diff -q /var/tmp/v3_${p}.txt /var/tmp/v3_check.txt > /dev/null 2>&1
    then
        result PASS "$label"
    else
        result FAIL "$label"
    fi
}

# layout: "disk column offset size" for every data chain, in bytes, from
# hammer2 show (data_off, vol= is the disk).
layout() {
    hammer2 -q show $DEVSPEC 2>/dev/null |
        awk '$1 ~ /^data\./ && $4 ~ /^vol=/ { sub(/^vol=/, "", $4); print $4, $2 }' |
        while read d off; do
            v=$((0x${off#0x}))
            echo "$d $((v & ~0xffff)) $((v & 0xffc0)) $((1 << (v & 63)))"
        done
}

# layout_bad: lines naming a chain not aligned to its size, past the end
# of its column, or overlapping another in the same column.
layout_bad() {
    layout | sort -k1,1n -k2,2n -k3,3n | awk '
        { if ($3 % $4 != 0) print "misaligned:", $0;
          if ($3 + $4 > 65536) print "past end:", $0;
          if ($1 == pd && $2 == pc && $3 < pe) print "overlap:", $0;
          pd = $1; pc = $2; pe = $3 + $4 }'
}

# ncols: distinct (disk, column) pairs holding data; nchains.
ncols() { layout | awk '{ print $1, $2 }' | sort -u | wc -l | tr -d ' '; }
nchains() { layout | wc -l | tr -d ' '; }

# corrupt_sub <disk> <byte_offset>: 1 KB of random data at the start of the
# chain's range, on the unmounted disk.
corrupt_sub() {
    dd if=/dev/urandom of=$(disk_dev "$1") bs=1024 count=1 \
        seek=$(($2 / 1024)) conv=notrunc 2>/dev/null
}

scrub_field() {
    awk -v f="$1" '$1 == f":" { print $2 }' < "$2"
}

# sub_blocks: "disk byte_offset" of every chain at a non-zero offset.
sub_blocks() {
    layout | while read d col off sz; do
        [ "$off" -ne 0 ] && echo "$d $((col + off))"
    done
}

# balanced <label>: after a sync every sub-column is closed.
balanced() {
    local o c
    o=$(ctr rz_subcol_open); c=$(ctr rz_subcol_close)
    if [ "$o" = "$c" ]; then
        result PASS "$1: every sub-column closed (open=$o close=$c)"
    else
        result FAIL "$1: open=$o close=$c after sync"
    fi
}

# V1: a perfect tiling lands in one column, in any order.
setup_fresh
check_v3
C0=$(ctr rz_cols_alloc); H0=$(ctr rz_subcol_chains)
n=0
for sz in 1 32 1 16 2 4 8; do
    dd if=/dev/urandom of=$MNTPT/v1_$n bs=${sz}k count=1 2>/dev/null
    n=$((n + 1))
done
sums v1
sync
C1=$(ctr rz_cols_alloc); H1=$(ctr rz_subcol_chains)
DC=$((C1 - C0)); DH=$((H1 - H0))
if [ "$DH" -ge 7 ] && [ "$DC" -le 2 ]; then
    result PASS "V1: 7 chains (64 KB) took $DC column(s)"
else
    result FAIL "V1: $DH chains took $DC columns"
fi
BAD=$(layout_bad)
if [ -z "$BAD" ]; then
    result PASS "V1: aligned, no overlap"
else
    result FAIL "V1: layout"
    echo "$BAD" | sed 's/^/      /'
fi
remount
verify "V1: cold read" v1
scrub_clean "V1"
balanced "V1"

# V2: mixed sizes.
write_mixed v2 100
sums v2
sync
BAD=$(layout_bad)
if [ -z "$BAD" ]; then
    result PASS "V2: aligned, no overlap"
else
    result FAIL "V2: layout"
    echo "$BAD" | sed 's/^/      /'
fi
NC=$(nchains); NL=$(ncols)
if [ "$NC" -gt 100 ] && [ "$NL" -lt $((NC / 2)) ]; then
    result PASS "V2: $NC chains in $NL columns"
else
    result FAIL "V2: $NC chains in $NL columns (no sharing?)"
fi
remount
verify "V2: cold read" v2
verify "V2: cold read of V1 files" v1
scrub_clean "V2"
balanced "V2"

# V3: a sync after each file closes its sub-column; nothing leaks.
O0=$(ctr rz_subcol_open)
i=0
while [ $i -lt 10 ]; do
    dd if=/dev/urandom of=$MNTPT/v3_$i bs=4k count=1 2>/dev/null
    sync
    i=$((i + 1))
done
O1=$(ctr rz_subcol_open)
if [ $((O1 - O0)) -ge 10 ]; then
    result PASS "V3: each synced file opened its own sub-column ($((O1 - O0)))"
else
    result FAIL "V3: only $((O1 - O0)) sub-columns for 10 synced files"
fi
balanced "V3"
STALE=$(ctr rz_subcol_stale)
if [ "$STALE" = "0" ]; then
    result PASS "V3: no ticket needed a safety net"
else
    result FAIL "V3: $STALE tickets released by a safety net"
fi
sums v3
remount
verify "V3: cold read" v3
teardown "V3"

# V4: rewrite and grow, in the open sub-column and after it closed.
setup_fresh
check_v3
i=0
while [ $i -lt 24 ]; do
    dd if=/dev/urandom of=$MNTPT/vr_$i bs=1k count=$((1 << (i % 4))) 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/vr_$i bs=1k count=$((1 << ((i + 2) % 6))) 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/vr_$i bs=1k count=1 conv=notrunc 2>/dev/null
    i=$((i + 1))
done
sync
i=0
while [ $i -lt 24 ]; do
    dd if=/dev/urandom of=$MNTPT/vr_$i bs=1k count=$((1 << ((i + 1) % 5))) \
        seek=$((i % 3)) conv=notrunc 2>/dev/null
    i=$((i + 1))
done
sums vr
sync
BAD=$(layout_bad)
if [ -z "$BAD" ]; then
    result PASS "V4: aligned, no overlap after rewrites"
else
    result FAIL "V4: layout"
    echo "$BAD" | sed 's/^/      /'
fi
remount
verify "V4: rewritten files read back" vr
scrub_clean "V4"
balanced "V4"
P4=$(ctr rz_put_anomaly)
if [ "$P4" = "0" ]; then
    result PASS "V4: no column was put twice"
else
    result FAIL "V4: rz_put_anomaly=$P4"
fi
teardown "V4"

# V5: files removed before their data is written.
setup_fresh
check_v3
write_mixed v5 60
i=0
while [ $i -lt 60 ]; do
    rm -f $MNTPT/v5_$i
    i=$((i + 2))
done
sums v5
sync
remount
verify "V5: survivors after rm of half" v5
scrub_clean "V5"
balanced "V5"
teardown "V5"

# V6: corrupt sub-blocks.
setup_fresh
check_v3
write_mixed v6 60
sums v6
sync
remount
sub_blocks > /var/tmp/v_sub.txt
NSUB=$(wc -l < /var/tmp/v_sub.txt | tr -d ' ')
if [ "${NSUB:-0}" -gt 10 ]; then
    result PASS "V6: $NSUB chains at a non-zero offset"
else
    result FAIL "V6: only ${NSUB:-0} chains at a non-zero offset"
fi
set -- $(sed -n 1p /var/tmp/v_sub.txt)
D6=$1; O6=$2
umount $MNTPT
corrupt_sub "$D6" "$O6"
mount -t hammer2 $PFSPATH $MNTPT
kmsg_clear
verify "V6: read over a corrupt sub-block (disk $D6 off $O6)" v6
H6=$(disk_counter "$D6" healed)
if [ "${H6:-0}" -gt 0 ]; then
    result PASS "V6: healed on the read path (healed=$H6)"
else
    result FAIL "V6: no heal counted (healed=$H6)"
fi
sleep 3
sync
scrub_clean "V6 follow-up"
set -- $(sed -n 2p /var/tmp/v_sub.txt)
umount $MNTPT
corrupt_sub "$1" "$2"
mount -t hammer2 $PFSPATH $MNTPT
hammer2 -s $MNTPT raid scrub > /var/tmp/v6_scrub.txt 2>&1
REP=$(scrub_field brefs_repaired /var/tmp/v6_scrub.txt)
UNREP=$(scrub_field brefs_unrepairable /var/tmp/v6_scrub.txt)
if [ "${REP:-0}" -ge 1 ] && [ "${UNREP:-x}" = "0" ]; then
    result PASS "V6: scrub repaired the corrupt sub-block (repaired=$REP)"
else
    result FAIL "V6: scrub repaired=$REP unrepairable=$UNREP"
fi
remount
verify "V6: cold read after repair" v6
scrub_clean "V6 final"
kmsg_clear
teardown "V6"

# V7: failed disk, dual reconstruction, resilver.
setup_fresh
check_v3
write_mixed v7 60
sums v7
sync
DISK=1
if fail_disk "$DISK" "V7"; then
    sync
    umount $MNTPT
    corrupt_data_zone "$DISK"
    if mount -t hammer2 $PFSPATH $MNTPT; then
        verify "V7: cold degraded read, disk${DISK} failed" v7
        umount $MNTPT
        corrupt_data_zone $((DISK + 1))
        if mount -t hammer2 $PFSPATH $MNTPT; then
            verify "V7: dual reconstruction" v7
        else
            result FAIL "V7: mount after second corruption refused"
        fi
    else
        result FAIL "V7: degraded mount refused"
    fi
fi
# V7 overwrote a second member's data zone, parity columns included; reads
# heal the data they use, but an unread P/Q column stays garbage until a
# parity scrub (group Y), so the parity check would flag that, not a bug.
parity_skip "V7 leaves unread P/Q columns of a second member corrupt on purpose"
kmsg_clear
teardown "V7"
setup_fresh
check_v3
write_mixed v7 60
sums v7
sync
D7=2
if fail_disk "$D7" "V7 resilver"; then
    fresh_disk "$D7"
    if hammer2 -s $MNTPT raid replace "$(disk_dev $D7)" "$(disk_dev $D7)" \
            > /var/tmp/v7_replace.txt 2>&1; then
        remount
        verify "V7: cold read after resilver of disk${D7}" v7
        scrub_clean "V7 resilver"
    else
        result FAIL "V7: raid replace failed"
        sed 's/^/      /' /var/tmp/v7_replace.txt
    fi
fi
kmsg_clear
teardown "V7 resilver"

STALE=$(ctr rz_subcol_stale)
if [ "$STALE" = "0" ]; then
    result PASS "V: no ticket needed a safety net"
else
    result FAIL "V: $STALE tickets released by a safety net"
fi

summary
