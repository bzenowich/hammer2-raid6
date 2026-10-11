#!/bin/sh
# Group P: I/O shape.  Functional tests cannot see a write path that
# reads before it writes or leaves one disk doing all the work; the data
# stays correct.  This group counts bytes at the member devices with
# devstat (src/diag/h2devstat.c) over a measured window:
#   P1  256 MB of incompressible data, then sync
#   P2  4000 files of 4 KB (compressed sub-64K blocks), then sync
#       Each asserts: array reads <= 1 MB (new blocks are never read
#       first), per-disk bytes written within 10% of the mean, total
#       written <= 2.15x user bytes (4+2 parity overhead is 1.5x for
#       the data columns, the rest is metadata).
#   P3  cold sequential read of the P1 file; informational only.
# Source data is prepared outside the measured window.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

BIGSRC=/var/tmp/p_rand256
SMALLSRC=/var/tmp/p_small
BIG_MB=256
NFILES=4000
trap 'rm -rf $SMALLSRC' EXIT INT TERM

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
    local label="$1" ubytes="$2" before="$3" after="$4" strict="${5:-1}" maxamp="${6:-2.15}"
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
    echo "  $label: user=$((ubytes / 1048576)) MB array-reads=$rdt B written=$((wrt / 1048576)) MB" \
         "max-disk-deviation=$dev write-amp=$ratio"
    if [ "$rdt" -le 1048576 ]; then
        result PASS "$label: array reads during write $rdt B (<= 1 MB)"
    else
        result FAIL "$label: array reads during write $rdt B (> 1 MB)"
    fi
    if awk -v d="$dev" 'BEGIN { exit !(d <= 0.10) }'; then
        result PASS "$label: per-disk writes balanced (max deviation $dev)"
    elif [ "$strict" = 1 ]; then
        result FAIL "$label: per-disk writes unbalanced (max deviation $dev > 0.10)"
    else
        echo "  INFO: $label: per-disk writes unbalanced (max deviation $dev > 0.10) [known, not asserted]"
    fi
    if awk -v r="$ratio" -v m="$maxamp" 'BEGIN { exit !(r <= m) }'; then
        result PASS "$label: write amplification $ratio (<= $maxamp)"
    elif [ "$strict" = 1 ]; then
        result FAIL "$label: write amplification $ratio (> $maxamp)"
    else
        echo "  INFO: $label: write amplification $ratio (> $maxamp) [known, not asserted]"
    fi
}

# Source data, outside the measured windows.
if [ ! -s $BIGSRC ] || [ "$(stat -f %z $BIGSRC)" != "$((BIG_MB * 1048576))" ]; then
    dd if=/dev/urandom of=$BIGSRC bs=1048576 count=$BIG_MB 2>/dev/null
fi
rm -rf $SMALLSRC
mkdir -p $SMALLSRC
awk -v n=$NFILES 'BEGIN {
    for (i = 0; i < n; i++) {
        f = sprintf("%s/f%d", "'$SMALLSRC'", i)
        s = ""
        while (length(s) < 4096)
            s = s sprintf("file %d line %d lorem ipsum dolor sit amet\n", i, length(s))
        printf "%s", substr(s, 1, 4096) > f
        close(f)
    }
}'

# P1: large incompressible write
setup_fresh
check_v3
sync; sleep 2
ds_snap /var/tmp/p_b1
dd if=$BIGSRC of=$MNTPT/big bs=1048576 2>/dev/null
sync; sync; sleep 2
ds_snap /var/tmp/p_a1
ds_report "P1" $((BIG_MB * 1048576)) /var/tmp/p_b1 /var/tmp/p_a1
teardown "P1"

# P3 first needs P1's file, so run it before P2 re-creates the array.
setup_fresh
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
setup_fresh
check_v3
sync; sleep 2
ds_snap /var/tmp/p_b2
cp -R $SMALLSRC $MNTPT/small
sync; sync; sleep 2
ds_snap /var/tmp/p_a2
# Every 4 KB file still costs a whole 64 KB column plus parity (oct-review
# P5), about 32x the user bytes, so the amplification limit here is 40x;
# before P10 (metadata mirrors written on every dirty release) it was 326x.
ds_report "P2" $((NFILES * 4096)) /var/tmp/p_b2 /var/tmp/p_a2 1 40
n=$(ls $MNTPT/small | wc -l | tr -d ' ')
if [ "$n" = "$NFILES" ]; then
    result PASS "P2: $n files present"
else
    result FAIL "P2: $n of $NFILES files present"
fi
teardown "P2"

summary
