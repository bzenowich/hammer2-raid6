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
#   W3  writes and reads to one disk are slow, but within
#       vfs.hammer2.io_timeout_ms: no panic, they complete, the data is
#       intact, and the slow disk stays ONLINE
#   W4  writes to a disk that hangs for far longer than io_timeout_ms
#       (R11): the member is auto-failed within about the timeout, writes
#       continue, the data reads back degraded (cold), raid replace
#       resilvers it to a clean scrub
#   W5  the same for reads of cold data
#   W6  a driver that returns its bios 8 s late (inject_hold_ms): the
#       timed waits give up after 2 s and abandon the buffers, the member
#       is auto-failed, and the late completions release the abandoned
#       buffers from another thread (no panic); the array replaces and
#       scrubs clean and unmounts
#
# A QMP device_del test of disk_gone is not here: DISK_GONE is compiled
# only into the Pi (arm64) kernel config, not the h2dev x86 guest.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group W: Write-side Fault Injection (NDISKS=$NDISKS) ==="

IO_TIMEOUT_DEF=$(sysctl -n vfs.hammer2.io_timeout_ms 2>/dev/null)
inject_off() {
    sysctl -w vfs.hammer2.inject_wio_disk_mask=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.inject_hold_ms_disk_mask=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.io_timeout_ms=${IO_TIMEOUT_DEF:-30000} \
        > /dev/null 2>&1
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
sysctl -w vfs.hammer2.io_timeout_ms=${IO_TIMEOUT_DEF:-30000} > /dev/null 2>&1
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
# 2 s late is well inside io_timeout_ms: the slow member stays ONLINE.
if disk_state_is "$W3_DISK" FAILED; then
    result FAIL "W3: slow disk${W3_DISK} was failed (2 s delay, timeout ${IO_TIMEOUT_DEF} ms)"
else
    result PASS "W3: slow disk${W3_DISK} not failed (2 s delay, timeout ${IO_TIMEOUT_DEF} ms)"
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

# whang_watch <disk>: in the background, note how many seconds until the
# disk shows FAILED (read it with whang_took)
whang_watch() {
    rm -f /var/tmp/v3_whang_t
    (
        t=0
        while ! disk_state_is "$1" FAILED; do
            sleep 1
            t=$((t + 1))
            [ "$t" -ge 40 ] && break
        done
        echo $t > /var/tmp/v3_whang_t
    ) &
}

# whang_wait <disk> <max secs>: wait for the disk to be auto-failed; echo
# how long it took
whang_wait() {
    local d="$1" max="$2" n=0
    while [ "$n" -lt "$max" ]; do
        disk_state_is "$d" FAILED && break
        sleep 1
        n=$((n + 1))
    done
    echo $n
}

# whang_finish <label> <disk>: injection off, let the sleeping injected
# I/O drain, then the degraded, replace and clean-scrub checks
whang_finish() {
    local label="$1" d="$2"
    sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.inject_hold_ms_disk_mask=0 > /dev/null 2>&1
    sysctl -w vfs.hammer2.io_timeout_ms=${IO_TIMEOUT_DEF:-30000} \
        > /dev/null 2>&1
    sleep 12
    if remount; then
        verify_ref "$label: pre-hang data correct (degraded, cold)" "before"
        [ -s /var/tmp/v3_during.txt ] &&
            verify_ref "$label: data written during the hang correct (degraded, cold)" "during"
    else
        result FAIL "$label: remount of the degraded array failed"
    fi
    fresh_disk "$d"
    if hammer2 -s $MNTPT raid replace "$(disk_dev "$d")" \
            "$(disk_dev "$d")" > /dev/null 2>&1; then
        result PASS "$label: replace disk$d accepted"
    else
        result FAIL "$label: replace disk$d failed"
    fi
    if remount; then
        verify_ref "$label: pre-hang data correct after resilver (cold)" "before"
        [ -s /var/tmp/v3_during.txt ] &&
            verify_ref "$label: during-hang data correct after resilver (cold)" "during"
        scrub_clean "$label"
    else
        result FAIL "$label: remount after replace failed"
    fi
    teardown "$label"
}

# W4: writes while a disk hangs for 10 s per I/O with a 2 s timeout
W4_DISK=1
setup_fresh
check_v3
write_ref_data "before"
rm -f /var/tmp/v3_during.txt
sysctl -w vfs.hammer2.io_timeout_ms=2000 > /dev/null 2>&1
sysctl -w vfs.hammer2.inject_delay_ms=10000 > /dev/null 2>&1
sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=$((1 << W4_DISK)) > /dev/null 2>&1
whang_watch $W4_DISK
t0=$(date +%s)
if guarded 300 write_ref_data "during"; then
    dt=$(($(date +%s) - t0))
    result PASS "W4: writes with disk${W4_DISK} hanging completed (${dt}s)"
else
    result FAIL "W4: writes with a hanging disk hung"
fi
dt=$(whang_wait "$W4_DISK" 20)
sleep 1
tf=$(cat /var/tmp/v3_whang_t 2>/dev/null)
if disk_state_is "$W4_DISK" FAILED && [ "${tf:-99}" -le 8 ]; then
    result PASS "W4: hanging disk${W4_DISK} auto-failed after ${tf}s (timeout 2 s, delay 10 s)"
else
    result FAIL "W4: hanging disk${W4_DISK} not failed after ${dt}s"
fi
whang_finish "W4" "$W4_DISK"

# W5: cold reads while a disk hangs
W5_DISK=2
setup_fresh
check_v3
write_ref_data "before"
rm -f /var/tmp/v3_during.txt
remount
sysctl -w vfs.hammer2.io_timeout_ms=2000 > /dev/null 2>&1
sysctl -w vfs.hammer2.inject_delay_ms=10000 > /dev/null 2>&1
sysctl -w vfs.hammer2.inject_delay_ms_disk_mask=$((1 << W5_DISK)) > /dev/null 2>&1
whang_watch $W5_DISK
t0=$(date +%s)
if guarded 300 sha256 $MNTPT/before_a $MNTPT/before_b > /var/tmp/v3_check.txt 2>&1 &&
   diff -q /var/tmp/v3_before.txt /var/tmp/v3_check.txt > /dev/null; then
    dt=$(($(date +%s) - t0))
    result PASS "W5: cold reads with disk${W5_DISK} hanging correct (${dt}s)"
else
    result FAIL "W5: cold reads with a hanging disk hung or wrong"
fi
dt=$(whang_wait "$W5_DISK" 20)
sleep 1
tf=$(cat /var/tmp/v3_whang_t 2>/dev/null)
if disk_state_is "$W5_DISK" FAILED && [ "${tf:-99}" -le 8 ]; then
    result PASS "W5: hanging disk${W5_DISK} auto-failed after ${tf}s (timeout 2 s, delay 10 s)"
else
    result FAIL "W5: hanging disk${W5_DISK} not failed after ${dt}s"
fi
whang_finish "W5" "$W5_DISK"

# W6: the member's timed bios (P/Q, headers, flushes) complete 8 s late
W6_DISK=1
setup_fresh
check_v3
write_ref_data "before"
rm -f /var/tmp/v3_during.txt
sysctl -w vfs.hammer2.io_timeout_ms=2000 > /dev/null 2>&1
sysctl -w vfs.hammer2.inject_hold_ms=8000 > /dev/null 2>&1
sysctl -w vfs.hammer2.inject_hold_ms_disk_mask=$((1 << W6_DISK)) > /dev/null 2>&1
t0=$(date +%s)
if guarded 300 write_ref_data "during"; then
    dt=$(($(date +%s) - t0))
    result PASS "W6: writes, sync and headers with disk${W6_DISK} returning late completed (${dt}s)"
else
    result FAIL "W6: writes with a late-returning disk hung"
fi
dt=$(whang_wait "$W6_DISK" 20)
if disk_state_is "$W6_DISK" FAILED; then
    result PASS "W6: late-returning disk${W6_DISK} auto-failed"
else
    result FAIL "W6: late-returning disk${W6_DISK} not failed"
fi
whang_finish "W6" "$W6_DISK"

summary
