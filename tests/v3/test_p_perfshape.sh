#!/bin/sh
# Group P: I/O shape.  Functional tests cannot see a write path that
# reads before it writes or leaves one disk doing all the work; the data
# stays correct.  This group counts bytes at the member devices with
# devstat (src/diag/h2devstat.c) over a measured window:
#   P1  256 MB of incompressible data, then sync
#   P2  4000 files of 4 KB (compressed sub-64K blocks), then sync
#       Each asserts: array reads <= 1 MB (new blocks are never read
#       first), per-disk bytes written within 10% of the mean (P1/P1c),
#       total written <= 2.15x user bytes (P1/P1c; 4+2 parity overhead
#       is 1.5x for the data columns, the rest is metadata).  P2/P2c
#       judge balance on data columns per disk instead (see below).
#   P1c 256 MB of text-like data LZ4 halves (about 2x), then sync
#   P2c 4000 files of 4 KB that LZ4 shrinks to about 1 KB, then sync
#   P3  cold sequential read of the P1 file; informational only.
#
# P1/P1c/P2/P2c also print the columns reserved (vfs.hammer2.rz_cols_alloc),
# the df space used and, for the big writes, the throughput.  The sub-column
# targets of oct-review.md 11.1 / 11.5 are asserted (the default since
# raid6_subcol_max = 32768, step 4):
#   P2  write amp <= 8x, df space <= 2x user bytes
#   P2c columns reserved <= 1/3 of P2
#   P1c columns reserved <= 0.6x P1, throughput >= 90% of P1
# P5_ASSERT=0 only prints them (for a kernel with raid6_subcol_max=0, or
# when the throughput ratio is too noisy on a loaded host).
#
# Balance of the small-file runs is judged on DATA COLUMNS per disk (distinct
# data columns in `hammer2 show`), not bytes: with packing, metadata (mirrors
# on every member, primaries on disk 0) dominates the bytes written, while
# data columns are spread exactly evenly.  The byte deviation is printed.
#
# Baseline before sub-column allocation (h2dev, 4 disks = 2+2, 2026-10-12;
# the PFS shows comp lz4:default, setcomp lz4 is applied anyway):
#          written  amp     columns  df used   throughput
#   P1     518 MB   2.03x   4096     256 MB    36.3 MB/s
#   P1c    518 MB   2.03x   4096     256 MB    29.2 MB/s  (LZ4 saves nothing)
#   P2     551 MB   35.3x   4000     256 MB (16.4x user bytes)
#   P2c    534 MB   34.2x   4000     256 MB (same as P2)
# Source data is prepared outside the measured window.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

BIGSRC=/var/tmp/p_rand256
SMALLSRC=/var/tmp/p_small
CMPSRC=/var/tmp/p_cmp256
SMALLCSRC=/var/tmp/p_smallc
BIG_MB=256
P5_ASSERT=${P5_ASSERT:-1}
NFILES=4000
SMALL_AMP=8
[ "$P5_ASSERT" = 0 ] && SMALL_AMP=45
trap 'rm -rf $SMALLSRC $SMALLCSRC $CMPSRC' EXIT INT TERM

echo "=== Group P: I/O shape (NDISKS=$NDISKS) ==="

# Find or build h2devstat (needs libdevstat; same lookup as h2stripe_check).
H2DS=$(command -v h2devstat 2>/dev/null)
[ -n "$H2DS" ] || H2DS=$(find /usr/local/bin /usr/bin /root /var/tmp -name h2devstat -type f 2>/dev/null | head -1)
if [ -z "$H2DS" ]; then
    SRC=$(find /root /var/tmp "$SCRIPTDIR/../../src/diag" -name h2devstat.c 2>/dev/null | head -1)
    if [ -n "$SRC" ] && cc -o /var/tmp/h2devstat "$SRC" -ldevstat 2>/dev/null; then
        H2DS=/var/tmp/h2devstat
    fi
fi
if [ -z "$H2DS" ]; then
    result FAIL "P: h2devstat not available (build src/diag/h2devstat.c)"
    summary
    exit $?
fi

# Member names as devstat knows them (no /dev/, no slice suffix).
DSNAMES=""
i=0
while [ "$i" -lt "$NDISKS" ]; do
    DSNAMES="$DSNAMES $(basename ${DISK_PREFIX})$((DISK_BASE + i))"
    i=$((i + 1))
done
DSNAMES=$(echo $DSNAMES)

# ds_snap <file>: record device counters
ds_snap() {
    $H2DS $DSNAMES | sort > "$1"
}

# ds_report <label> <user_bytes> <before> <after>: print and assert.
ds_report() {
    local label="$1" ubytes="$2" before="$3" after="$4" strict="${5:-1}" maxamp="${6:-2.15}" maxdev="${7:-0.10}"
    local out=/var/tmp/p_ds.txt
    join "$before" "$after" | awk -v ub="$ubytes" '
        { rd = $7 - $3; wr = $9 - $5
          rdt += rd; wrt += wr; n++; w[n] = wr; r[n] = rd; nm[n] = $1 }
        END {
            mean = wrt / n; maxdev = 0
            for (i = 1; i <= n; i++) {
                printf "  %s: read %.2f MB, wrote %.2f MB\n", nm[i], r[i] / 1048576, w[i] / 1048576 > "/dev/stderr"
                d = (w[i] > mean ? w[i] - mean : mean - w[i]) / (mean ? mean : 1)
                if (d > maxdev) maxdev = d
            }
            printf "%d %d %.4f %.3f\n", rdt, wrt, maxdev, wrt / ub
        }' > $out
    set -- $(cat $out)
    local rdt="$1" wrt="$2" dev="$3" ratio="$4"
    LAST_WRT=$wrt
    LAST_RATIO=$ratio
    echo "  $label: user=$((ubytes / 1048576)) MB array-reads=$rdt B written=$((wrt / 1048576)) MB" \
         "max-disk-deviation=$dev write-amp=$ratio"
    if [ "$rdt" -le 1048576 ]; then
        result PASS "$label: array reads during write $rdt B (<= 1 MB)"
    else
        result FAIL "$label: array reads during write $rdt B (> 1 MB)"
    fi
    if [ "$maxdev" = info ]; then
        echo "  INFO: $label: per-disk byte deviation $dev (metadata dominates; see data columns)"
    elif awk -v d="$dev" -v m="$maxdev" 'BEGIN { exit !(d <= m) }'; then
        result PASS "$label: per-disk writes balanced (max deviation $dev)"
    elif [ "$strict" = 1 ] && [ "$maxdev" != info ]; then
        result FAIL "$label: per-disk writes unbalanced (max deviation $dev > $maxdev)"
    else
        echo "  INFO: $label: per-disk writes unbalanced (max deviation $dev > $maxdev) [known, not asserted]"
    fi
    if awk -v r="$ratio" -v m="$maxamp" 'BEGIN { exit !(r <= m) }'; then
        result PASS "$label: write amplification $ratio (<= $maxamp)"
    elif [ "$strict" = 1 ]; then
        result FAIL "$label: write amplification $ratio (> $maxamp)"
    else
        echo "  INFO: $label: write amplification $ratio (> $maxamp) [known, not asserted]"
    fi
}


# p5_target <label> <ok:0|1> <text>: a 11.1 target.  Printed always;
# asserted unless P5_ASSERT=0.
p5_target() {
    local label="$1" ok="$2" text="$3"
    if [ "$P5_ASSERT" != 0 ]; then
        if [ "$ok" = 1 ]; then result PASS "$label: $text"
        else result FAIL "$label: $text"; fi
    else
        result PASS "$label: $text [target not asserted, P5_ASSERT=0]"
    fi
}

# col_balance <label> <maxdev>: DATA columns per disk from `hammer2 show`
# (distinct disk:column pairs; vol= is the disk).  Parity and metadata are
# not counted.  Each disk must be within maxdev of the mean.
col_balance() {
    local label="$1" maxdev="$2" i out
    hammer2 -q show $DEVSPEC 2>/dev/null |
        awk '$1 ~ /^data\./ && $4 ~ /^vol=/ { sub(/^vol=/, "", $4); print $4, substr($2, 1, length($2) - 4) }' |
        sort -u |
        awk -v nd=$NDISKS '{ c[$1]++; t++ }
            END { m = t / nd; mx = 0
                  for (i = 0; i < nd; i++) {
                      printf "  disk %d: %d data columns\n", i, c[i] > "/dev/stderr"
                      d = (c[i] > m ? c[i] - m : m - c[i]) / (m ? m : 1)
                      if (d > mx) mx = d }
                  printf "%d %.4f\n", t, mx }' > /var/tmp/p_cb.txt
    set -- $(cat /var/tmp/p_cb.txt)
    if [ "${1:-0}" -gt 0 ] && awk -v d="$2" -v m="$maxdev" 'BEGIN { exit !(d <= m) }'; then
        result PASS "$label: data columns balanced ($1 columns, max deviation $2)"
    else
        result FAIL "$label: data columns unbalanced or none ($1 columns, max deviation $2 > $maxdev)"
    fi
}

# used_kb: df "Used" of the test mount, after a sync.
used_kb() {
    sync; sync; sleep 1
    df -k $MNTPT | awk 'NR == 2 { print $3 }'
}

# Set LZ4 explicitly on the PFS root (new files inherit it) and show it.
set_lz4() {
    hammer2 setcomp lz4 $MNTPT > /var/tmp/p_comp.txt 2>&1 ||
        echo "  INFO: setcomp lz4 failed: $(cat /var/tmp/p_comp.txt)"
    echo "  comp: $(hammer2 stat $MNTPT 2>&1 | sed -n 2p)"
}

# big_write <label> <src>: P1-style write of a 256 MB file into a fresh
# array; sets BW_COLS BW_SECS BW_MBS BW_USED_KB BW_WRT.
big_write() {
    local label="$1" src="$2" c0 u0 u1
    setup_fresh full
    check_v3
    set_lz4
    sync; sleep 2
    u0=$(used_kb)
    ds_snap /var/tmp/p_b1
    c0=$(sysctl -n vfs.hammer2.rz_cols_alloc 2>/dev/null)
    # /usr/bin/time -p gives the elapsed time to 10 ms (date has no %N).
    /usr/bin/time -p sh -c "dd if=$src of=$MNTPT/big bs=1048576 2>/dev/null; sync; sync" \
        2> /var/tmp/p_time.txt
    sleep 2
    ds_snap /var/tmp/p_a1
    BW_COLS=$(( $(sysctl -n vfs.hammer2.rz_cols_alloc 2>/dev/null) - c0 ))
    u1=$(used_kb)
    BW_USED_KB=$((u1 - u0))
    BW_SECS=$(awk '$1 == "real" { print $2 }' /var/tmp/p_time.txt)
    BW_MBS=$(awk -v s="$BW_SECS" -v m=$BIG_MB 'BEGIN { printf "%.1f", (s > 0) ? m / s : 0 }')
    ds_report "$label" $((BIG_MB * 1048576)) /var/tmp/p_b1 /var/tmp/p_a1
    BW_WRT=$LAST_WRT
    echo "    $label: columns reserved +$BW_COLS, df used +$((BW_USED_KB / 1024)) MB," \
         "$BW_SECS s (write+sync) = $BW_MBS MB/s"
    teardown "$label"
}

# small_write <label> <srcdir>: P2-style copy of 4000 files into a fresh
# array; sets SW_COLS SW_USED_KB SW_RATIO SW_WRT.  Leaves the array mounted
# (P4/P5 use the tree) and does not tear down.
small_write() {
    local label="$1" src="$2" c0 r0 u0 u1
    setup_fresh full
    check_v3
    set_lz4
    sync; sleep 2
    u0=$(used_kb)
    ds_snap /var/tmp/p_b2
    c0=$(sysctl -n vfs.hammer2.rz_cols_alloc 2>/dev/null)
    r0=$(sysctl -n vfs.hammer2.rz_rewrite_slots 2>/dev/null)
    cp -R $src $MNTPT/small
    sync; sync; sleep 2
    ds_snap /var/tmp/p_a2
    SW_COLS=$(( $(sysctl -n vfs.hammer2.rz_cols_alloc) - c0 ))
    echo "    $label: columns reserved +$SW_COLS, rewrite slots +$(( $(sysctl -n vfs.hammer2.rz_rewrite_slots) - r0 ))"
    u1=$(used_kb)
    SW_USED_KB=$((u1 - u0))
}

# Source data, outside the measured windows.
if [ ! -s $BIGSRC ] || [ "$(stat -f %z $BIGSRC)" != "$((BIG_MB * 1048576))" ]; then
    dd if=/dev/urandom of=$BIGSRC bs=1048576 count=$BIG_MB 2>/dev/null
fi
rm -rf $SMALLSRC $SMALLCSRC
mkdir -p $SMALLSRC $SMALLCSRC
# P2: incompressible files (random hex, LZ4 cannot halve it); P2c: the
# same count of text-like files that LZ4 shrinks to about 1 KB.  Both
# deterministic (fixed srand seed).
awk -v n=$NFILES -v d=$SMALLSRC 'BEGIN {
    srand(2)
    for (i = 0; i < n; i++) {
        f = sprintf("%s/f%d", d, i)
        for (j = 0; j < 512; j++)
            printf "%08x", int(rand() * 4294967296) > f
        close(f)
    }
}'
awk -v n=$NFILES -v d=$SMALLCSRC 'BEGIN {
    for (i = 0; i < n; i++) {
        f = sprintf("%s/f%d", d, i)
        s = ""
        while (length(s) < 4096)
            s = s sprintf("file %d line %d lorem ipsum dolor sit amet\n", i, length(s))
        printf "%s", substr(s, 1, 4096) > f
        close(f)
    }
}'
# P1c source: 1 MB of pseudo-random words (a 256-word vocabulary of
# 4-7 letter words, one random pick per word), repeated BIG_MB times.
# Each 64 KB block compresses on its own, so the repeat does not help LZ4.
# Tuned to about 2x.  Never zero blocks (those take the zero detection).
if [ ! -s $CMPSRC ] || [ "$(stat -f %z $CMPSRC)" != "$((BIG_MB * 1048576))" ]; then
    awk 'BEGIN {
        srand(1)
        for (w = 0; w < 256; w++) {
            len = 4 + int(rand() * 4); v = ""
            for (k = 0; k < len; k++) v = v sprintf("%c", 97 + int(rand() * 26))
            voc[w] = v
        }
        n = 0
        while (n < 1048576) {
            l = ""
            for (k = 0; k < 12; k++) l = l (k ? " " : "") voc[int(rand() * 256)]
            l = l "\n"
            if (n + length(l) > 1048576) l = substr(l, 1, 1048576 - n)
            printf "%s", l; n += length(l)
        }
    }' > /var/tmp/p_cmp1m
    : > $CMPSRC
    i=0
    while [ $i -lt $BIG_MB ]; do cat /var/tmp/p_cmp1m >> $CMPSRC; i=$((i + 1)); done
    rm -f /var/tmp/p_cmp1m
fi

# P1: large incompressible write; P1c: compressible (about 2x)
big_write P1 $BIGSRC
P1_COLS=$BW_COLS; P1_MBS=$BW_MBS; P1_WRT=$BW_WRT
big_write P1c $CMPSRC
P1C_COLS=$BW_COLS; P1C_MBS=$BW_MBS; P1C_WRT=$BW_WRT
echo "  P1c vs P1: columns $P1C_COLS vs $P1_COLS, throughput $P1C_MBS vs $P1_MBS MB/s," \
     "written $((P1C_WRT / 1048576)) MB vs $((P1_WRT / 1048576)) MB"
p5_target "P1c" $(awk -v c=$P1C_COLS -v p=$P1_COLS 'BEGIN { print (c <= 0.6 * p) }') \
    "columns reserved $P1C_COLS vs P1 $P1_COLS (target <= 0.6x)"
p5_target "P1c" $(awk -v c=$P1C_MBS -v p=$P1_MBS 'BEGIN { print (c >= 0.9 * p) }') \
    "throughput $P1C_MBS vs P1 $P1_MBS MB/s (target >= 90%)"

# P3 first needs P1's file, so run it before P2 re-creates the array.
setup_fresh full
dd if=$BIGSRC of=$MNTPT/big bs=1048576 2>/dev/null
sync; sync
remount
sleep 2
ds_snap /var/tmp/p_b3
dd if=$MNTPT/big of=/dev/null bs=1048576 2>/dev/null
ds_snap /var/tmp/p_a3
rd=$(join /var/tmp/p_b3 /var/tmp/p_a3 | awk '{ t += $7 - $3 } END { print t + 0 }')
echo "  INFO: P3 cold read: $((rd / 1048576)) MB read from members for $BIG_MB MB file," \
     "ratio $(awk -v r=$rd -v b=$((BIG_MB * 1048576)) 'BEGIN { printf "%.2f", r / b }')"
teardown "P3"

# P2: many small files
small_write P2 $SMALLSRC
# With sub-column allocation (the default) 4000 x 4 KB files write about
# 4x the user bytes; without it every file costs a whole column plus
# parity, 35-41x (P5_ASSERT=0, raid6_subcol_max=0: limit 45x, which still
# catches the 326x before P10).  Balance is judged on data columns (the
# byte deviation is metadata: primaries on disk 0, mirrors everywhere,
# syncer passes vary).
ds_report "P2" $((NFILES * 4096)) /var/tmp/p_b2 /var/tmp/p_a2 1 $SMALL_AMP info
col_balance P2 0.10
P2_COLS=$SW_COLS; P2_RATIO=$LAST_RATIO
echo "    P2: df used +$((SW_USED_KB / 1024)) MB for $((NFILES * 4 / 1024)) MB of files," \
     "space ratio $(awk -v u=$SW_USED_KB -v b=$((NFILES * 4)) 'BEGIN { printf "%.2f", u / b }')"
p5_target "P2" $(awk -v r=$P2_RATIO 'BEGIN { print (r <= 8) }') "write amplification $P2_RATIO (target <= 8)"
p5_target "P2" $(awk -v u=$SW_USED_KB -v b=$((NFILES * 4)) 'BEGIN { print (u <= 2 * b) }') \
    "df space $SW_USED_KB KB for $((NFILES * 4)) KB of files (target <= 2x)"
n=$(ls $MNTPT/small | wc -l | tr -d ' ')
if [ "$n" = "$NFILES" ]; then
    result PASS "P2: $n files present"
else
    result FAIL "P2: $n of $NFILES files present"
fi

# P4: metadata reads spread over the mirrors (oct-review P4).  A cold
# walk of the P2 tree touches inode and directory blocks only; their
# copies sit on every member, so no member should serve most of them.
remount
sleep 2
ds_snap /var/tmp/p_b4
ls -lR $MNTPT/small > /dev/null
ds_snap /var/tmp/p_a4
join /var/tmp/p_b4 /var/tmp/p_a4 | awk '
    { rd = $7 - $3; t += rd; n++; r[n] = rd; nm[n] = $1; if (rd > mx) mx = rd
      if (rd > 0) nz++ }
    END {
        for (i = 1; i <= n; i++)
            printf "  %s: read %.2f MB\n", nm[i], r[i] / 1048576 > "/dev/stderr"
        printf "%d %d %d %.3f\n", t, nz, n, (t ? mx / t : 1)
    }' > /var/tmp/p_ds.txt
set -- $(cat /var/tmp/p_ds.txt)
echo "  P4: cold metadata walk read $1 B from $2 of $3 members, busiest share $4"
if [ "$1" -ge 1048576 ] && [ "$2" -ge 2 ] && \
   awk -v s="$4" 'BEGIN { exit !(s <= 0.60) }'; then
    result PASS "P4: metadata reads spread (busiest member $4 of $1 B)"
else
    result FAIL "P4: metadata reads not spread (busiest member $4 of $1 B, $2 members)"
fi

# P5: cold metadata reads against mirror copies being written.  A writer
# churns metadata and syncs (the sync issues the mirror copies without
# waiting) while the tree is walked again and again, remounting between
# rounds so the reads miss the cache.  A read that took a mirror's old
# media contents would fail its CHECK: no member may count an error.
(
    j=0
    while [ -f /var/tmp/p5_run ]; do
        mkdir -p $MNTPT/churn$((j % 8))
        k=0
        while [ $k -lt 200 ]; do
            echo $j.$k > $MNTPT/churn$((j % 8))/f$k
            k=$((k + 1))
        done
        sync
        j=$((j + 1))
    done
) &
P5PID=$!
: > /var/tmp/p5_run
P5ERR=0
r=0
while [ $r -lt 6 ]; do
    ls -lR $MNTPT/small > /dev/null 2>>/var/tmp/p5_err.txt || P5ERR=1
    ls -lR $MNTPT/churn0 > /dev/null 2>>/var/tmp/p5_err.txt
    r=$((r + 1))
done
rm -f /var/tmp/p5_run
wait $P5PID
hammer2 -s $MNTPT raid status > /var/tmp/p5_status.txt 2>&1
remount
ls -lR $MNTPT/small > /dev/null 2>>/var/tmp/p5_err.txt || P5ERR=1
hammer2 -s $MNTPT raid status >> /var/tmp/p5_status.txt 2>&1
CK=$(grep '^disk\[' /var/tmp/p5_status.txt | awk '{ for (i = 1; i < NF; i++) if ($i == "cksum_err") t += $(i+1) } END { print t + 0 }')
if [ "$P5ERR" = 0 ] && [ "$CK" = 0 ]; then
    result PASS "P5: metadata walks during mirror writes, cksum errors $CK"
else
    result FAIL "P5: walk error $P5ERR, cksum errors $CK"
    cat /var/tmp/p5_err.txt
fi
teardown "P2"

# P2c: the same count of compressible files (LZ4: 4 KB -> about 1 KB).
# Compression saves nothing on v3 today (a column per chain either way).
small_write P2c $SMALLCSRC
ds_report "P2c" $((NFILES * 4096)) /var/tmp/p_b2 /var/tmp/p_a2 1 $SMALL_AMP info
col_balance P2c 0.10
echo "    P2c vs P2: columns $SW_COLS vs $P2_COLS, write-amp $LAST_RATIO vs $P2_RATIO," \
     "df used +$((SW_USED_KB / 1024)) MB"
p5_target "P2c" $(awk -v c=$SW_COLS -v p=$P2_COLS 'BEGIN { print (c <= p / 3) }') \
    "columns reserved $SW_COLS vs P2 $P2_COLS (target <= 1/3)"
n=$(ls $MNTPT/small | wc -l | tr -d ' ')
if [ "$n" = "$NFILES" ]; then
    result PASS "P2c: $n files present"
else
    result FAIL "P2c: $n of $NFILES files present"
fi
teardown "P2c"

summary
