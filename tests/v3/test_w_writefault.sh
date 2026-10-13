#!/bin/sh
# Group W: write-side fault injection (oct-review §7 item 4).
#
# vfs.hammer2.inject_wio_disk_mask completes hammer2's writes to the
# masked disks with EIO (data columns, P/Q, zero fill, metadata mirror
# copies, volume headers); inject_delay_ms_disk_mask delays hammer2's
# reads and synchronous writes to them by inject_delay_ms, like a hung
# disk that has not detached.
#
#   W1  one disk's writes fail: no panic, the member is auto-failed, what
#       was written reads back degraded (cold), raid replace resilvers
#       it to a clean scrub
#   W2  the same with two disks (the most RAID6 survives)
#   W3  writes and reads to one disk are slow: no panic, they complete,
#       the data is intact.  There is NO I/O timeout path (R11), so the
#       slow disk is NOT failed; the test asserts that current behaviour
#       and will need to change when a timeout is added.
#
# A QMP device_del test of disk_gone is not here: DISK_GONE is compiled
# only into the Pi (arm64) kernel config, not the h2dev x86 guest.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group W: Write-side Fault Injection (NDISKS=$NDISKS) ==="

inject_off() {
    sysctl -w vfs.hammer2.inject_wio_disk_mask=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null 2>&1
}
trap inject_off EXIT

# wfail_case <label> <disk> [<disk>]: write while the disks' writes fail
wfail_case() {
    local label="$1" mask=0 d n=0 want
    shift
    for d in "$@"; do mask=$((mask | (1 << d))); done

    setup_fresh
    check_v3
    write_ref_data "before"

    sysctl -w vfs.hammer2.inject_wio_disk_mask=$mask > /dev/null 2>&1
    if guarded 300 write_ref_data "during"; then
        result PASS "$label: writes with disks '$*' failing completed"
    else
        result FAIL "$label: writes with disks '$*' failing hung or failed"
    fi
    guarded 120 sync
    sysctl -w vfs.hammer2.inject_wio_disk_mask=0 > /dev/null 2>&1

    for d in "$@"; do
        if disk_state_is "$d" FAILED; then
            n=$((n + 1))
        else
            result FAIL "$label: disk$d not auto-failed by write EIO"
        fi
    done
    [ "$n" = "$#" ] && result PASS "$label: every failing member auto-failed"

    # degraded, from the media
    if remount; then
        verify_ref "$label: pre-failure data correct (degraded, cold)" "before"
        verify_ref "$label: data written during the failure correct (degraded, cold)" "during"
    else
        result FAIL "$label: remount of the degraded array failed"
    fi

    # replace each one in turn
    for d in "$@"; do
        fresh_disk "$d"
        if hammer2 -s $MNTPT raid replace "$(disk_dev "$d")" \
                "$(disk_dev "$d")" > /dev/null 2>&1; then
            result PASS "$label: replace disk$d accepted"
        else
            result FAIL "$label: replace disk$d failed"
        fi
    done
    if remount; then
        verify_ref "$label: pre-failure data correct after resilver (cold)" "before"
        verify_ref "$label: during-failure data correct after resilver (cold)" "during"
        scrub_clean "$label"
    else
        result FAIL "$label: remount after replace failed"
    fi
    teardown "$label"
}

# W1: a data/parity mix: disk1 is data or parity depending on the slot
wfail_case W1 1
# W2: two members at once
wfail_case W2 1 $((NDISKS - 1))

# W3: slow disk
W3_DISK=1
setup_fresh
check_v3
write_ref_data "base"
sysctl -w vfs.hammer2.inject_delay_ms=2000 > /dev/null 2>&1
sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=$((1 << W3_DISK)) > /dev/null 2>&1
t0=$(date +%s)
dd if=/dev/urandom of=$MNTPT/slow_a bs=65536 count=6 2>/dev/null
sha256 $MNTPT/slow_a > /var/tmp/v3_slow.txt
if guarded 600 sync; then
    dt=$(($(date +%s) - t0))
    if [ "$dt" -ge 2 ]; then
        result PASS "W3: sync with disk${W3_DISK} 2 s late completed (${dt}s, delay engaged)"
    else
        result FAIL "W3: sync took ${dt}s: delay was not applied"
    fi
else
    result FAIL "W3: sync with a slow disk hung"
fi
sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null 2>&1
# Current behaviour: no I/O timeout, the slow member stays ONLINE.
if disk_state_is "$W3_DISK" FAILED; then
    result FAIL "W3: slow disk${W3_DISK} was failed (an I/O timeout path now exists? update this test)"
else
    result PASS "W3: slow disk${W3_DISK} not failed (no I/O timeout path, R11)"
fi
if remount; then
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=$((1 << W3_DISK)) > /dev/null 2>&1
    t0=$(date +%s)
    if guarded 600 sha256 $MNTPT/slow_a > /var/tmp/v3_slow_chk.txt; then
        dt=$(($(date +%s) - t0))
        if diff -q /var/tmp/v3_slow.txt /var/tmp/v3_slow_chk.txt > /dev/null; then
            result PASS "W3: cold read with slow disk${W3_DISK} correct (${dt}s)"
        else
            result FAIL "W3: cold read with slow disk data mismatch"
        fi
    else
        result FAIL "W3: cold read with a slow disk hung"
    fi
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null 2>&1
    verify_ref "W3: earlier data correct" "base"
    scrub_clean "W3"
else
    result FAIL "W3: remount failed"
fi
teardown "W3"

summary
