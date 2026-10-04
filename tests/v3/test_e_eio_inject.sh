#!/bin/sh
# Group E: EIO injection — verify a surviving-column read error during
# reconstruction surfaces as a clean error and does not panic.
#
# Drives vfs.hammer2.inject_eio_disk_mask (per-disk bitmask, EIO is
# forced without auto-failing the disk so each test is repeatable).
#
# Tests:
#   E1  one disk failed + injected EIO on a surviving column is two
#       erasures, which RAID6 recovers: the cold read must return the
#       correct data, not merely "not panic".
#   E2  EIO on disk 0 (the metadata primary) on a healthy array: the
#       tree walk and file reads must fail over to mirror copies and
#       succeed, and disk 0 must not be auto-failed by injected EIO.
#   E3  resilver with EIO injected on a source column: again two
#       erasures, so the resilver must complete and the rebuilt disk
#       must verify clean.
#
# Each case used to record PASS on both branches of its check, so the
# group could only fail on a panic.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group E: EIO Injection (NDISKS=$NDISKS) ==="

inject_mask() {
    sysctl -w vfs.hammer2.inject_eio_disk_mask="$1" > /dev/null 2>&1
}

# Always clear injection on exit so a failed run doesn't strand the
# system with a stuck sysctl.
trap 'inject_mask 0' EXIT INT TERM

bit_for_disk() {
    # echo (1 << $1)
    awk "BEGIN { printf \"%d\\n\", lshift(1, $1); }" 2>/dev/null || \
        echo $((1 << $1))
}

dmesg_panic_check() {
    local label="$1"
    if kmsg | grep -q "panic:"; then
        result FAIL "$label: kernel panic in dmesg"
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------
# E1
# ---------------------------------------------------------------
E1_FAIL=$((NDISKS - 1))
E1_INJECT=2
[ "$E1_INJECT" -ge "$NDISKS" ] && E1_INJECT=0
[ "$E1_INJECT" = "$E1_FAIL" ] && E1_INJECT=$((E1_INJECT - 1))

setup_fresh
check_v3
write_ref_data "e1"
if fail_disk "$E1_FAIL" "E1" && remount; then
    kmsg_clear
    inject_mask "$(bit_for_disk "$E1_INJECT")"
    verify_ref "E1: disk${E1_FAIL} failed + EIO on disk${E1_INJECT}: data correct (cold)" "e1"
    inject_mask 0
    if disk_state_is "$E1_INJECT" ONLINE; then
        result PASS "E1: injected EIO did not auto-fail disk${E1_INJECT}"
    else
        result FAIL "E1: disk${E1_INJECT} auto-failed by injected EIO"
    fi
    dmesg_panic_check "E1"
    if remount; then
        verify_ref "E1: data correct after inject cleared (cold)" "e1"
    else
        result FAIL "E1: remount after inject failed"
    fi
fi
teardown "E1"

# ---------------------------------------------------------------
# E2
# ---------------------------------------------------------------
setup_fresh
check_v3
mkdir -p $MNTPT/e2dir
i=0
while [ $i -lt 50 ]; do
    echo "e2-$i" > $MNTPT/e2dir/f$i
    i=$((i + 1))
done
write_ref_data "e2"
if remount; then
    kmsg_clear
    inject_mask "$(bit_for_disk 0)"
    N=$(find $MNTPT/e2dir -type f 2>/dev/null | wc -l | tr -d ' ')
    if [ "$N" = "50" ]; then
        result PASS "E2: tree walk with EIO on metadata primary (50 files)"
    else
        result FAIL "E2: tree walk returned $N of 50 files under EIO on disk 0"
    fi
    verify_ref "E2: file data correct with EIO on disk 0 (cold)" "e2"
    inject_mask 0
    if disk_state_is 0 ONLINE; then
        result PASS "E2: injected EIO did not auto-fail disk 0"
    else
        result FAIL "E2: disk 0 auto-failed by injected EIO"
    fi
    dmesg_panic_check "E2"
else
    result FAIL "E2: remount failed"
fi
teardown "E2"

# ---------------------------------------------------------------
# E3
# ---------------------------------------------------------------
E3_RES=$((NDISKS - 2))   # disk being resilvered
E3_SRC=0                 # disk we inject on
[ "$E3_SRC" = "$E3_RES" ] && E3_SRC=1

setup_fresh
check_v3
write_ref_data "e3"
if fail_disk "$E3_RES" "E3"; then
    fresh_disk "$E3_RES"
    kmsg_clear
    inject_mask "$(bit_for_disk "$E3_SRC")"
    hammer2 -s $MNTPT raid replace \
        "$(disk_dev $E3_RES)" "$(disk_dev $E3_RES)" > /var/tmp/e3_out.txt 2>&1
    RC=$?
    inject_mask 0
    dmesg_panic_check "E3"
    if [ "$RC" = "0" ] && disk_state_is "$E3_RES" ONLINE; then
        result PASS "E3: resilver completed with EIO on source disk${E3_SRC}"
        if remount; then
            verify_ref "E3: data correct after resilver (cold)" "e3"
            ERR=$(disk_counter "$E3_RES" cksum_err)
            if [ "${ERR:-x}" = "0" ]; then
                result PASS "E3: rebuilt disk${E3_RES} read clean"
            else
                result FAIL "E3: rebuilt disk${E3_RES} had $ERR bad columns"
            fi
            scrub_clean "E3"
        else
            result FAIL "E3: remount after resilver failed"
        fi
    else
        result FAIL "E3: resilver rc=$RC with EIO on disk${E3_SRC} (2 erasures are recoverable)"
        sed 's/^/      /' /var/tmp/e3_out.txt
    fi
fi
teardown "E3"

summary
