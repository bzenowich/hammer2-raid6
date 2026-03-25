#!/bin/sh
# Group D: Resilver — replace a failed disk and verify reconstruction.
# Tests: D1 (basic resilver), D2 (sequential resilvers), D3 (resilver + write during)

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group D: Resilver ==="

# D1: Basic resilver — fail one disk, resilver, verify
setup_fresh
check_v4
dd if=/dev/urandom of=$MNTPT/resilver_ref bs=65536 count=800 2>/dev/null
sha256 $MNTPT/resilver_ref > /var/tmp/d1_ref.txt
sync; sync

hammer2 -s $MNTPT raid fail-disk /dev/vn3 > /dev/null 2>&1
vnconfig -u vn3 2>/dev/null || true
# Attach fresh swap-backed replacement
vnconfig -S 1073741824 vn3
# Resilver
if hammer2 -s $MNTPT raid replace /dev/vn3 /dev/vn3 > /dev/null 2>&1; then
    # Verify post-resilver
    sha256 $MNTPT/resilver_ref > /var/tmp/d1_check.txt 2>&1
    if diff -q /var/tmp/d1_ref.txt /var/tmp/d1_check.txt > /dev/null 2>&1; then
        result PASS "D1: post-resilver data correct (degraded read)"
    else
        result FAIL "D1: post-resilver data mismatch"
    fi
    # Remount fully healthy, verify again
    umount $MNTPT
    mount -t hammer2 $PFSPATH $MNTPT
    sha256 $MNTPT/resilver_ref > /var/tmp/d1_check2.txt 2>&1
    if diff -q /var/tmp/d1_ref.txt /var/tmp/d1_check2.txt > /dev/null 2>&1; then
        result PASS "D1: post-remount data correct (healthy read)"
    else
        result FAIL "D1: post-remount data mismatch"
    fi
else
    result FAIL "D1: resilver command failed"
fi
teardown "D1"

# D2: Sequential resilvers (fail vn1, resilver; write more; fail vn4, resilver)
setup_fresh
check_v4
write_ref_data "r1"

hammer2 -s $MNTPT raid fail-disk /dev/vn1 > /dev/null 2>&1
vnconfig -u vn1 2>/dev/null || true
vnconfig -S 1073741824 vn1
hammer2 -s $MNTPT raid replace /dev/vn1 /dev/vn1 > /dev/null 2>&1
verify_ref "D2: r1 after first resilver (vn1)" "r1"

write_ref_data "r2"

hammer2 -s $MNTPT raid fail-disk /dev/vn4 > /dev/null 2>&1
vnconfig -u vn4 2>/dev/null || true
vnconfig -S 1073741824 vn4
hammer2 -s $MNTPT raid replace /dev/vn4 /dev/vn4 > /dev/null 2>&1
verify_ref "D2: r1 after second resilver (vn4)" "r1"
verify_ref "D2: r2 after second resilver (vn4)" "r2"
teardown "D2"

# D3: Write during resilver — data written concurrently must be correct after
setup_fresh
check_v4
write_ref_data "pre"
sync; sync

hammer2 -s $MNTPT raid fail-disk /dev/vn0 > /dev/null 2>&1
vnconfig -u vn0 2>/dev/null || true
vnconfig -S 1073741824 vn0

# Start resilver in background, write concurrently, then check
hammer2 -s $MNTPT raid replace /dev/vn0 /dev/vn0 > /dev/null 2>&1 &
RPID=$!

# Write new data while resilver is running
dd if=/dev/urandom of=$MNTPT/during_resilver bs=65536 count=256 2>/dev/null
sha256 $MNTPT/during_resilver > /var/tmp/d3_new.txt
sync; sync

wait $RPID
RESILVER_RC=$?

verify_ref "D3: pre-resilver data still correct" "pre"
sha256 $MNTPT/during_resilver > /var/tmp/d3_check.txt 2>&1
if diff -q /var/tmp/d3_new.txt /var/tmp/d3_check.txt > /dev/null 2>&1; then
    result PASS "D3: data written during resilver correct"
else
    result FAIL "D3: data written during resilver mismatch"
fi
if [ "$RESILVER_RC" = "0" ]; then
    result PASS "D3: resilver completed successfully"
else
    result FAIL "D3: resilver returned non-zero ($RESILVER_RC)"
fi
teardown "D3"

summary
