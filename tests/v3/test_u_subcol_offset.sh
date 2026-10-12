#!/bin/sh
# Group U: chains at a non-zero offset inside their column
# (oct-review.md section 11.5 step 2, the test hook
# vfs.hammer2.raid6_subcol_test).
#
# With the hook on, a data chain smaller than 64 KB is placed at an
# aligned offset inside its (otherwise private) column, so data_off is
# column + offset + radix.  This group writes small files and checks
# that every path which maps a blockref to media bytes copes with that.
# The hook is restored on exit.
#
#   U1  the media really holds chains at an offset (hammer2 show)
#   U2  cold read, scrub clean
#   U3  corrupt one sub-block while unmounted: read-path self-heal fixes
#       it (counters, content), the follow-up scrub is clean
#   U4  corrupt another: a scrub finds and repairs it
#   U5  failed disk: degraded read, plus one more bad column (dual)
#   U6  resilver of a failed disk, cold read and scrub afterwards
#   U7  rewrite and grow files in the same TXG, cold read
#
# Environment: SUBCOL_MODE=1|2 selects the hook mode (default 2).

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group U: Sub-column offsets (NDISKS=$NDISKS) ==="

HOOK_SAVED=$(sysctl -n vfs.hammer2.raid6_subcol_test 2>/dev/null)
if [ -z "$HOOK_SAVED" ]; then
    echo "  SKIP: no vfs.hammer2.raid6_subcol_test in this kernel"
    exit 77
fi
restore_hook() {
    sysctl -w vfs.hammer2.raid6_subcol_test="$HOOK_SAVED" > /dev/null 2>&1
}
trap restore_hook EXIT
sysctl -w vfs.hammer2.raid6_subcol_test="${SUBCOL_MODE:-2}" > /dev/null

scrub_field() {
    awk -v f="$1" '$1 == f":" { print $2 }' < "$2"
}

# Sizes straddle every chain size from 1 KB to 64 KB (a file's block is
# its size rounded up to a power of two); text is compressible, random
# is not.
write_small() {
    local p="$1" n=0 sz
    : > /var/tmp/v3_${p}.txt
    for sz in 600 1000 1500 2100 3000 4096 5000 7000 8192 9000 12000 \
              16000 17000 24000 31000 32768 40000; do
        n=$((n + 1))
        dd if=/dev/urandom of=$MNTPT/${p}_r$n bs=$sz count=1 2>/dev/null
        yes "${p} text line $n abcdefghijklmnop" | head -c $sz \
            > $MNTPT/${p}_t$n
    done
    sync
    sha256 $MNTPT/${p}_* > /var/tmp/v3_${p}.txt
}

verify_small() {
    local label="$1" p="$2"
    sha256 $MNTPT/${p}_* > /var/tmp/v3_check.txt 2>&1
    if diff -q /var/tmp/v3_${p}.txt /var/tmp/v3_check.txt > /dev/null 2>&1
    then
        result PASS "$label"
    else
        result FAIL "$label"
    fi
}

# sub_blocks: "disk byte_offset_of_block" for every data chain at a non-zero
# offset in its column, from hammer2 show (data_off, vol= is the disk).
sub_blocks() {
    hammer2 -q show $DEVSPEC 2>/dev/null |
        awk '$1 ~ /^data\./ && $4 ~ /^vol=/ { sub(/^vol=/, "", $4); print $4, $2 }' |
        while read d off; do
            off=${off#0x}
            v=$((0x$off & ~0x3f))
            if [ $((v & 0xfc00)) -ne 0 ]; then
                echo "$d $v"
            fi
        done
}

# corrupt_sub <disk> <byte_offset>: 1 KB of random data at the start of the
# chain's range, on the unmounted disk.
corrupt_sub() {
    dd if=/dev/urandom of=$(disk_dev "$1") bs=1024 count=1 \
        seek=$(($2 / 1024)) conv=notrunc 2>/dev/null
}

setup_fresh
check_v3
write_small u1
sub_blocks > /var/tmp/u_sub.txt
NSUB=$(wc -l < /var/tmp/u_sub.txt | tr -d ' ')
if [ "${NSUB:-0}" -gt 4 ]; then
    result PASS "U1: $NSUB chains sit at a non-zero column offset"
else
    result FAIL "U1: only ${NSUB:-0} chains at a non-zero offset (hook not applied?)"
fi
remount
verify_small "U2: cold read of the small files" u1
scrub_clean "U2"

# U3: one corrupt sub-block, healed on the read path.
set -- $(sed -n 1p /var/tmp/u_sub.txt)
S3_DISK=$1; S3_OFF=$2
umount $MNTPT
corrupt_sub "$S3_DISK" "$S3_OFF"
mount -t hammer2 $PFSPATH $MNTPT
kmsg_clear
verify_small "U3: read over a corrupt sub-block (disk $S3_DISK off $S3_OFF)" u1
H3=$(disk_counter "$S3_DISK" healed)
if [ "${H3:-0}" -gt 0 ]; then
    result PASS "U3: sub-block healed on the read path (healed=$H3)"
else
    result FAIL "U3: no heal counted (healed=$H3)"
fi
sleep 3
sync
scrub_clean "U3 follow-up"
remount
verify_small "U3: cold read after the repair landed" u1

# U4: another one, found and repaired by scrub.
set -- $(sed -n 2p /var/tmp/u_sub.txt)
S4_DISK=$1; S4_OFF=$2
umount $MNTPT
corrupt_sub "$S4_DISK" "$S4_OFF"
mount -t hammer2 $PFSPATH $MNTPT
hammer2 -s $MNTPT raid scrub > /var/tmp/u4_scrub.txt 2>&1
REP=$(scrub_field brefs_repaired /var/tmp/u4_scrub.txt)
UNREP=$(scrub_field brefs_unrepairable /var/tmp/u4_scrub.txt)
if [ "${REP:-0}" -ge 1 ] && [ "${UNREP:-x}" = "0" ]; then
    result PASS "U4: scrub repaired the corrupt sub-block (repaired=$REP)"
else
    result FAIL "U4: scrub repaired=$REP unrepairable=$UNREP"
    sed 's/^/      /' /var/tmp/u4_scrub.txt
fi
remount
scrub_clean "U4 follow-up"
verify_small "U4: cold read after scrub repair" u1
# The corruption above is deliberate: its checksum failures are expected.
kmsg_clear
teardown "U4"

# U5: degraded read of sub-blocks, then a dual reconstruction.
setup_fresh
check_v3
write_small u5
DISK=1
if fail_disk "$DISK" "U5"; then
    sync
    umount $MNTPT
    corrupt_data_zone "$DISK"
    if mount -t hammer2 $PFSPATH $MNTPT; then
        verify_small "U5: cold degraded read, disk${DISK} failed" u5
        umount $MNTPT
        corrupt_data_zone $((DISK + 1))
        if mount -t hammer2 $PFSPATH $MNTPT; then
            verify_small "U5: dual reconstruction, disk${DISK} failed + disk$((DISK + 1)) corrupt" u5
        else
            result FAIL "U5: mount after second corruption refused"
        fi
    else
        result FAIL "U5: degraded mount refused"
    fi
fi
kmsg_clear
teardown "U5"

# U6: resilver.
setup_fresh
check_v3
write_small u6
D6=2
if fail_disk "$D6" "U6"; then
    fresh_disk "$D6"
    if hammer2 -s $MNTPT raid replace "$(disk_dev $D6)" "$(disk_dev $D6)" \
            > /var/tmp/u6_replace.txt 2>&1; then
        remount
        verify_small "U6: cold read after resilver of disk${D6}" u6
        E6=$(disk_counter "$D6" cksum_err)
        if [ "${E6:-x}" = "0" ]; then
            result PASS "U6: rebuilt disk${D6} read clean"
        else
            result FAIL "U6: rebuilt disk${D6} had $E6 bad columns"
        fi
        scrub_clean "U6"
    else
        result FAIL "U6: raid replace failed"
        sed 's/^/      /' /var/tmp/u6_replace.txt
    fi
fi
teardown "U6"

# U7: rewrite and grow in the same TXG (a pending column is re-placed), and
# in the next one (a fresh slot).
setup_fresh
check_v3
i=0
while [ $i -lt 12 ]; do
    dd if=/dev/urandom of=$MNTPT/u7_$i bs=1k count=1 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/u7_$i bs=1k count=$((2 << (i % 5))) 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/u7_$i bs=1k count=$((1 << (i % 6))) \
        conv=notrunc 2>/dev/null
    i=$((i + 1))
done
sync
i=0
while [ $i -lt 12 ]; do
    dd if=/dev/urandom of=$MNTPT/u7_$i bs=1k count=3 seek=$((i % 4)) \
        conv=notrunc 2>/dev/null
    i=$((i + 1))
done
sha256 $MNTPT/u7_* > /var/tmp/v3_u7.txt
sync
remount
sha256 $MNTPT/u7_* > /var/tmp/v3_check.txt
if diff -q /var/tmp/v3_u7.txt /var/tmp/v3_check.txt > /dev/null 2>&1; then
    result PASS "U7: rewritten and grown files read back after remount"
else
    result FAIL "U7: content mismatch after rewrites"
fi
scrub_clean "U7"
teardown "U7"

summary
