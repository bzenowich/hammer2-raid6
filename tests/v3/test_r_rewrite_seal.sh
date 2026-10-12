#!/bin/sh
# Group R: R5 repro (oct-review.md R5).  OPT-IN: not in run_all.sh's
# default list, because on an unfixed kernel it is expected to PANIC the
# guest ("v3 putblk for a row not open") or to leave bad parity.
# Run it with:  sh run_all.sh R
#
#   R1  Within one TXG (no sync between), write file A (4 KB, O_SYNC), fill
#       A's stripe row with other small O_SYNC files so it seals, then
#       rewrite A (O_SYNC).  A's chain is still MODIFIED, v3 never sets
#       DEDUPABLE (dedup_record returns early), so chain_modify keeps A's
#       slot and the second putblk finds its row sealed.  Expected on a
#       correct kernel: A reads back as the new data, cold, and
#       h2stripe_check finds P/Q correct.
#   R2  Same, rewriting A twice after the seal.
#   R4a/R4b  put-stall hook: sync waits for a stalled column / times out.
#   R3  Control, runs first: as R1 but A is fsync'd after its first write,
#       which clears MODIFIED, so the rewrite takes a new slot.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group R: rewrite after row seal (R5, NDISKS=$NDISKS) ==="

# h2wr <file> <byte> <nbytes> [fsync]: overwrite from offset 0 with
# <nbytes> of <byte>, through an O_SYNC descriptor.
H2WR=/var/tmp/h2wr
cat > /var/tmp/h2wr.c <<'CEOF'
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int
main(int argc, char **argv)
{
	size_t n = (size_t)atol(argv[3]);
	char *buf = malloc(n);
	int fd = open(argv[1], O_WRONLY | O_CREAT | O_SYNC, 0644);

	if (fd < 0 || buf == NULL)
		return 1;
	memset(buf, argv[2][0], n);
	if (pwrite(fd, buf, n, 0) != (ssize_t)n)
		return 1;
	if (argc > 4 && fsync(fd) != 0)
		return 1;
	return close(fd) != 0;
}
CEOF
cc -O -o $H2WR /var/tmp/h2wr.c || { echo "  FATAL: cc failed"; exit 1; }

if ! command -v h2stripe_check > /dev/null 2>&1; then
    H2CHECK=$(find /usr/local/bin /usr/bin /root /var/tmp -name h2stripe_check 2>/dev/null | head -1)
else
    H2CHECK=h2stripe_check
fi

# fill_row <n>: n other 4 KB O_SYNC files, so A's row gets all its columns.
fill_row() {
    local k=0
    while [ "$k" -lt "$1" ]; do
        $H2WR $MNTPT/fill_$k F 4096
        k=$((k + 1))
    done
}

# check_a <label> <byte>: A must read back as 4096 x <byte>, cold.
check_a() {
    local want got
    want=$(head -c 4096 /dev/zero | tr '\0' "$2" | sha256 -q)
    got=$(sha256 -q $MNTPT/A 2>/dev/null)
    if [ "$got" = "$want" ]; then
        result PASS "$1: A reads back as the rewritten data"
    else
        result FAIL "$1: A has wrong contents after rewrite"
    fi
}

parity_ok() {
    local label="$1"
    umount $MNTPT || return 1
    if [ -z "$H2CHECK" ]; then
        result FAIL "$label: h2stripe_check not installed"
    # shellcheck disable=SC2086
    elif $H2CHECK $DEVS > /var/tmp/r_check.txt 2>&1; then
        result PASS "$label: h2stripe_check: P/Q parity correct"
    else
        result FAIL "$label: h2stripe_check: parity errors"
        sed 's/^/      /' /var/tmp/r_check.txt
    fi
    mount -t hammer2 $PFSPATH $MNTPT
}

# R3: as R1, but A is fsync'd after its first write.  fsync flushes the
# file's chains, clears MODIFIED, so the rewrite takes a new slot (newmod=1
# on v3 for an unmodified chain): no panic expected.  Runs first so a
# panic in R1 does not hide it.
setup_fresh
check_v3
$H2WR $MNTPT/A a 4096 fsync
fill_row $((NDISKS * 2))
$H2WR $MNTPT/A Z 4096
sync
remount
check_a R3 Z
parity_ok R3
teardown R3

# R1
setup_fresh
check_v3
$H2WR $MNTPT/A a 4096
fill_row $((NDISKS * 2))
echo "  R1: rewriting A after its row sealed (a panic here is the bug)"
$H2WR $MNTPT/A Z 4096
kmsg | grep -i "row not open"
sync
remount
check_a R1 Z
parity_ok R1
teardown R1

# R2
setup_fresh
check_v3
$H2WR $MNTPT/A a 4096
fill_row $((NDISKS * 2))
$H2WR $MNTPT/A Y 4096
$H2WR $MNTPT/A Z 4096
sync
remount
check_a R2 Z
parity_ok R2
teardown R2

# R4: a strategy write holds its row reservation (raid6_put_stall_ms)
# while sync runs.  The flush must wait for the column (oct-review.md
# §10), seal the row once, and leave exact parity.  The free slots are
# first filled with garbage by a write/rm/bulkfree cycle, so a zero-fill
# seal that races the arriving column would show as a parity error.
r4_prep() {
    setup_fresh
    check_v3
    local k=0
    while [ "$k" -lt 40 ]; do
        dd if=/dev/urandom of=$MNTPT/g_$k bs=65536 count=2 2>/dev/null
        k=$((k + 1))
    done
    sync
    rm -f $MNTPT/g_*
    sync
    hammer2 bulkfree $MNTPT > /dev/null 2>&1
    hammer2 bulkfree $MNTPT > /dev/null 2>&1
}

# R4a: stall shorter than the wait; sync waits, then everything is exact.
r4_prep
sysctl vfs.hammer2.raid6_gen_wait_ms=5000 > /dev/null
sysctl vfs.hammer2.raid6_put_stall_ms=3000 > /dev/null
( $H2WR $MNTPT/S 4 4096 ) &
WPID=$!
sleep 1
T0=$(date +%s)
sync
T1=$(date +%s)
wait $WPID
if [ $((T1 - T0)) -ge 1 ]; then
    result PASS "R4a: sync waited for the stalled column ($((T1 - T0 + 1))s after arming)"
else
    result FAIL "R4a: sync did not wait ($((T1 - T0))s)"
fi
[ "$(sysctl -n vfs.hammer2.rz_gen_wait_fail)" = 0 ] &&
    result PASS "R4a: no failed gen wait" || result FAIL "R4a: gen wait failed"
H=$(head -c 4096 /dev/zero | tr '\0' 4 | sha256 -q)
[ "$(sha256 -q $MNTPT/S)" = "$H" ] &&
    result PASS "R4a: stalled write intact" || result FAIL "R4a: stalled write wrong"
parity_ok R4a
teardown R4a

# R4b: stall longer than raid6_gen_wait_ms.
r4_prep
sysctl vfs.hammer2.raid6_gen_wait_ms=1000 > /dev/null
sysctl vfs.hammer2.raid6_put_stall_ms=4000 > /dev/null
( $H2WR $MNTPT/S 5 4096 ) &
WPID=$!
sleep 1
sync
sleep 5
wait $WPID
sync
# The stalled write holds its chain lock, so the flush blocks on that lock
# before it reaches the column wait: the gen-wait timeout itself is not
# reachable with this hook (rz_gen_wait_fail stays 0).  Check that a stall
# longer than the wait neither hangs nor corrupts.
result PASS "R4b: sync completed with stall > raid6_gen_wait_ms"
sysctl vfs.hammer2.raid6_gen_wait_ms=5000 > /dev/null
H=$(head -c 4096 /dev/zero | tr '\0' 5 | sha256 -q)
[ "$(sha256 -q $MNTPT/S)" = "$H" ] &&
    result PASS "R4b: write intact after the retry" || result FAIL "R4b: write wrong"
parity_ok R4b
teardown R4b

umount $MNTPT 2>/dev/null
summary
