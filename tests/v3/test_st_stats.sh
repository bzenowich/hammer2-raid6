#!/bin/sh
# Group ST: per-mount I/O statistics (HAMMER2IOC_RAID_STATS, `hammer2 raid
# stats`, oct-review.md S6).  The counters are what would have shown the
# S1 read-before-write bug on day one; this group checks that they add up.
#
#   ST1  bad usage: the tool refuses a path that is not a mount
#   ST2  write 64 MB + sync on a fresh mount: data write bytes >= file
#        bytes, P and Q writes equal in ops and bytes, new_preread == 0,
#        no degraded reads, data read bytes ~ 0, row seals counted
#   ST3  cold read after remount: data reads > 0, degraded_read_* == 0
#   ST4  scrub: SCRUB-class reads > 0, still no degraded reads, new_preread 0
#   ST5  stats -z: counters are zero right after zeroing
#   ST6  one disk failed, cold read: degraded_read_1 > 0, data correct,
#        DEGRADED-class reads on the survivors, nothing read from the
#        failed disk
#   ST7  replace the failed disk (resilver): resilver_slots_copied > 0 and
#        RESILVER writes on that disk; degraded reads stop afterwards
#
# Counters are per mount, so each phase that remounts starts from zero.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group ST: RAID statistics (NDISKS=$NDISKS) ==="

ST=/var/tmp/v3_st_stats.txt
FILE_MB=64
FILE_BYTES=$((FILE_MB * 1048576))
FAILED=1

# st_get: fetch the counters into $ST
st_get() {
    hammer2 -s $MNTPT raid stats -p > $ST 2>&1
}

# st_val <name>: a mount-wide counter
st_val() {
    awk -v n="$1" '$1 == n { print $2 }' $ST
}

# st_sum <rd_ops|rd_bytes|wr_ops|wr_bytes> <class> [disk]: sum over the
# disks (or one disk)
st_sum() {
    awk -v k="$1" -v c="$2" -v d="${3:--1}" '
        { split($1, a, ".")
          if (a[1] == k && a[3] == c && (d < 0 || a[2] == d)) s += $2 }
        END { print s + 0 }' $ST
}

# st_alldisk <rd_ops|...> <disk>: all classes of one disk
st_alldisk() {
    awk -v k="$1" -v d="$2" '
        { split($1, a, ".")
          if (a[1] == k && a[2] == d) s += $2 }
        END { print s + 0 }' $ST
}

# st_check <label> <condition result 0/1> <detail>
st_check() {
    if [ "$2" = 1 ]; then
        result PASS "$1 ($3)"
    else
        result FAIL "$1 ($3)"
    fi
}

setup_fresh
check_v3

# ST1: a path that is not a hammer2 mount must be refused
if hammer2 raid stats /var/tmp > /dev/null 2>&1; then
    result FAIL "ST1: raid stats accepted a non-hammer2 path"
else
    result PASS "ST1: raid stats refuses a path that is not a mount"
fi

# ST2: write phase
dd if=/dev/urandom of=$MNTPT/st_a bs=1048576 count=$FILE_MB 2>/dev/null
sha256 $MNTPT/st_a > /var/tmp/v3_st_ref.txt
sync; sync
st_get
if ! grep -q '^new_preread ' $ST; then
    result FAIL "ST2: raid stats -p printed no counters"
    sed 's/^/      /' $ST
    summary
    exit $?
fi
DW=$(st_sum wr_bytes data)
PW=$(st_sum wr_bytes P); QW=$(st_sum wr_bytes Q)
PO=$(st_sum wr_ops P); QO=$(st_sum wr_ops Q)
DR=$(st_sum rd_bytes data)
st_check "ST2a: data write bytes >= file bytes" \
    $([ "$DW" -ge $FILE_BYTES ] && echo 1 || echo 0) "data=$DW file=$FILE_BYTES"
st_check "ST2b: P and Q writes equal (ops and bytes)" \
    $([ "$PO" -gt 0 ] && [ "$PO" = "$QO" ] && [ "$PW" = "$QW" ] && echo 1 || echo 0) \
    "P ops=$PO bytes=$PW, Q ops=$QO bytes=$QW"
st_check "ST2c: new_preread == 0 (no read before a new write)" \
    $([ "$(st_val new_preread)" = 0 ] && echo 1 || echo 0) \
    "new_preread=$(st_val new_preread) meta=$(st_val new_preread_meta)"
st_check "ST2d: no degraded reads on a healthy array" \
    $([ "$(st_val degraded_read_1)" = 0 ] && [ "$(st_val degraded_read_2)" = 0 ] && echo 1 || echo 0) \
    "1=$(st_val degraded_read_1) 2=$(st_val degraded_read_2)"
st_check "ST2e: data read bytes during the write <= 1 MB" \
    $([ "$DR" -le 1048576 ] && echo 1 || echo 0) "data reads=$DR"
SEALS=$(( $(st_val seal_full) + $(st_val seal_flush) ))
st_check "ST2f: row seals counted, parity not above data" \
    $([ "$SEALS" -gt 0 ] && [ "$PW" -le "$DW" ] && echo 1 || echo 0) \
    "full=$(st_val seal_full) flush=$(st_val seal_flush) partial=$(st_val seal_partial) P=$PW data=$DW"
st_check "ST2g: metadata and mirror writes counted" \
    $([ "$(st_sum wr_bytes meta)" -gt 0 ] && [ "$(st_sum wr_bytes mmirror)" -gt 0 ] && echo 1 || echo 0) \
    "meta=$(st_sum wr_bytes meta) mmirror=$(st_sum wr_bytes mmirror) volhdr=$(st_sum wr_bytes volhdr)"
st_check "ST2h: open_rows_high recorded" \
    $([ "$(st_val open_rows_high)" -gt 0 ] && echo 1 || echo 0) \
    "cur=$(st_val open_rows_cur) high=$(st_val open_rows_high)"

# ST3: cold read after remount (counters restart at mount)
remount
cat $MNTPT/st_a > /dev/null
st_get
DR=$(st_sum rd_bytes data)
st_check "ST3a: cold read counted as data reads" \
    $([ "$DR" -ge $((FILE_BYTES / 2)) ] && echo 1 || echo 0) "data reads=$DR"
st_check "ST3b: healthy read: no degraded reads, no preread, no writes of data" \
    $([ "$(st_val degraded_read_1)" = 0 ] && [ "$(st_val new_preread)" = 0 ] && [ "$(st_sum wr_bytes data)" = 0 ] && echo 1 || echo 0) \
    "deg1=$(st_val degraded_read_1) preread=$(st_val new_preread) datawr=$(st_sum wr_bytes data)"

# ST4: scrub
hammer2 -s $MNTPT raid scrub > /var/tmp/v3_st_scrub.txt 2>&1
SRC=$?
st_get
st_check "ST4: scrub reads counted, nothing degraded" \
    $([ "$SRC" = 0 ] && [ "$(st_sum rd_bytes scrub)" -gt 0 ] && [ "$(st_val degraded_read_1)" = 0 ] && [ "$(st_val new_preread)" = 0 ] && echo 1 || echo 0) \
    "rc=$SRC scrub reads=$(st_sum rd_bytes scrub) deg1=$(st_val degraded_read_1)"

# ST5: zeroing
hammer2 -s $MNTPT raid stats -z > /dev/null 2>&1
st_get
ZT=$(( $(st_sum rd_bytes data) + $(st_sum rd_bytes scrub) + $(st_sum wr_bytes data) + $(st_sum wr_bytes P) ))
st_check "ST5: counters are zero after stats -z" \
    $([ "$ZT" = 0 ] && [ "$(st_val seal_full)" = 0 ] && echo 1 || echo 0) "sum=$ZT"

# ST6: one disk failed, cold read
if fail_disk $FAILED "ST6"; then
    sync
    umount $MNTPT
    corrupt_data_zone $FAILED
    if mount -t hammer2 $PFSPATH $MNTPT; then
        sha256 $MNTPT/st_a > /var/tmp/v3_st_chk.txt 2>&1
        st_get
        if diff -q /var/tmp/v3_st_ref.txt /var/tmp/v3_st_chk.txt > /dev/null 2>&1; then
            result PASS "ST6a: degraded read returns the data"
        else
            result FAIL "ST6a: degraded read returned wrong data"
        fi
        D1=$(st_val degraded_read_1)
        st_check "ST6b: degraded_read_1 > 0, degraded_read_2 == 0" \
            $([ "$D1" -gt 0 ] && [ "$(st_val degraded_read_2)" = 0 ] && echo 1 || echo 0) \
            "deg1=$D1 deg2=$(st_val degraded_read_2)"
        st_check "ST6c: sibling reads classed degraded; failed disk not read" \
            $([ "$(st_sum rd_bytes degraded)" -gt 0 ] && [ "$(st_alldisk rd_ops $FAILED)" = 0 ] && echo 1 || echo 0) \
            "degraded reads=$(st_sum rd_bytes degraded) failed disk read ops=$(st_alldisk rd_ops $FAILED)"
        st_check "ST6d: new_preread still 0" \
            $([ "$(st_val new_preread)" = 0 ] && echo 1 || echo 0) "new_preread=$(st_val new_preread)"
    else
        result FAIL "ST6: degraded remount refused"
    fi

    # ST7: replace the failed disk (rebuild onto the same device)
    fresh_disk $FAILED
    if hammer2 -s $MNTPT raid replace "$(disk_dev $FAILED)" "$(disk_dev $FAILED)" \
            > /var/tmp/v3_st_repl.txt 2>&1; then
        st_get
        st_check "ST7a: resilver counted" \
            $([ "$(st_val resilver_slots_copied)" -gt 0 ] && [ "$(st_sum wr_bytes resilver $FAILED)" -gt 0 ] && echo 1 || echo 0) \
            "copied=$(st_val resilver_slots_copied) skipped=$(st_val resilver_slots_skipped) wr=$(st_sum wr_bytes resilver $FAILED) rd=$(st_sum rd_bytes resilver)"
        hammer2 -s $MNTPT raid stats -z > /dev/null 2>&1
        remount
        sha256 $MNTPT/st_a > /var/tmp/v3_st_chk.txt 2>&1
        st_get
        if diff -q /var/tmp/v3_st_ref.txt /var/tmp/v3_st_chk.txt > /dev/null 2>&1 &&
           [ "$(st_val degraded_read_1)" = 0 ]; then
            result PASS "ST7b: after the rebuild the read is healthy (degraded_read_1=0)"
        else
            result FAIL "ST7b: after the rebuild deg1=$(st_val degraded_read_1) or data wrong"
        fi
    else
        result FAIL "ST7: raid replace failed"
        cat /var/tmp/v3_st_repl.txt
    fi
fi

kmsg_clear
teardown "ST"
summary
