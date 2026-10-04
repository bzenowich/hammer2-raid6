#!/bin/sh
# Group M: silent corruption in more than one column (RAID6 = two
# erasures; ZFS raidz2 recovers these by trying column combinations
# against the block checksum).
#
#   M1  disk A failed (fail-disk) + disk B silently corrupt.  Blocks on
#       A are rebuilt by the degraded path, whose rebuild reads B's
#       corrupt column as good unless it retries with B as a second
#       erasure.  Blocks on B must be self-healed with A as the second
#       erasure.
#   M2  two ONLINE disks silently corrupt.  Each bad block's own column
#       is known bad; its row-mate on the other disk is not, so the
#       read must try a second erasure.
#
# M2 deadlocked the kernel when found (two self-heals holding one
# column buffer each and waiting in getblk for the other's), so its
# reads run under a watchdog.  A hang is reported as FAIL and the
# group stops without unmounting: the stuck reader cannot be killed,
# and the guest needs a reboot before the next run.  Keep this group
# last in run_all.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

# m_verify <prefix> <label>: verify_ref with the reads under a watchdog.
# Returns 124 on a hang.  (verify_ref itself cannot run under guarded:
# it would record its result in the background subshell.)
m_verify() {
    guarded 120 sh -c "sha256 $MNTPT/$1_a $MNTPT/$1_b > /var/tmp/m_check.txt 2>&1"
    local rc=$?
    [ "$rc" = 124 ] && return 124
    if diff -q /var/tmp/v4_$1.txt /var/tmp/m_check.txt > /dev/null 2>&1; then
        result PASS "$2"
    else
        result FAIL "$2"
    fi
    return 0
}

echo "=== Group M: Multi-column silent corruption (NDISKS=$NDISKS) ==="

# M1
M1_FAIL=1
M1_BAD=2
setup_fresh
check_v3
write_ref_data "m1"
if fail_disk "$M1_FAIL" "M1"; then
    sync
    umount $MNTPT
    corrupt_data_zone "$M1_BAD"
    if mount -t hammer2 $PFSPATH $MNTPT; then
        m_verify m1 "M1: disk${M1_FAIL} failed + disk${M1_BAD} corrupt: data correct"
        if [ $? = 124 ]; then
            result FAIL "M1: read hung (kernel deadlock); reboot the guest"
            summary
            exit 1
        fi
    else
        result FAIL "M1: mount with one failed disk refused"
    fi
fi
kmsg_clear
teardown "M1"

# M2
M2_A=0
M2_B=3
setup_fresh
check_v3
write_ref_data "m2"
umount $MNTPT
corrupt_data_zone "$M2_A"
corrupt_data_zone "$M2_B"
if mount -t hammer2 $PFSPATH $MNTPT; then
    m_verify m2 "M2: disk${M2_A}+disk${M2_B} silently corrupt: data correct"
    if [ $? = 124 ]; then
        result FAIL "M2: read hung (kernel deadlock); reboot the guest"
        summary
        exit 1
    fi
else
    result FAIL "M2: mount refused"
fi
kmsg_clear
teardown "M2"

summary
