#!/bin/sh
# Group S: Split volume headers (a crash in the middle of the header loop).
#
# The commit point of a TXG is the volume header, written to every disk
# in turn.  A power cut part-way through leaves the members at two
# different rz_txg_seq values: some at T+1, the rest at T.  A disk that
# reached T+1 still holds its T header in another zone (the zones are
# written in rotation), so every disk can vote for T.  Expected mount
# behaviour (docs/volhdr_quorum.md, oct-review.md R2):
#
#   - the mount picks the highest seq that a majority of the electorate
#     supports, majority = max(N/2 + 1, N - 2), where a disk supports a
#     seq if ANY of its valid header zones carries it;
#   - stepping back one TXG (T+1 -> T) needs no operator action;
#   - a disk whose newest header is below the chosen seq is marked
#     FAILED (it needs a resilver or replace, its columns for the
#     chosen TXG are missing);
#   - a disk ABOVE the chosen seq (an orphan T+1 header after a rollback)
#     stays ONLINE; its orphan zone is ignored and is overwritten by the
#     next commit;
#   - data is that of the chosen TXG: the synced base file is intact,
#     the later file is either whole or absent, never torn;
#   - after the replace of the FAILED disks (or at once when none are)
#     a scrub is clean and a further remount works.
#
# The crash is simulated on unmounted media: after a clean mount cycle
# that advances the headers, the NEWEST header zone of k disks is zeroed.
# That leaves those disks exactly as a crash before their T+1 header
# write would (their T header sits in the previous zone), while the other
# disks keep the complete T+1 state.  Disks are N = $NDISKS; with
# new = N - k disks at T+1 and majority = max(N/2 + 1, N - 2):
#
#   new >= majority  ->  mount at T+1, the k lagging disks FAILED
#   new <  majority  ->  mount at T, nothing FAILED, orphans ignored
#
# Cases (N = 4):
#   S1  1 disk behind (3 new + 1 old)   -> T+1, that disk FAILED, replace
#   S2  2 disks behind (2 + 2)          -> T, rollback by one, no failure
#   S3  3 disks behind (1 + 3)          -> T, rollback by one, no failure
#   S4  disk 0 (space map, metadata) behind -> as S1
# On the current code S2 and S3 fail to mount, and S1/S4 leave the
# lagging disk ONLINE.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"

kldstat -q -m hammer2 || kldload hammer2

echo "=== Group S: Split Volume Headers (NDISKS=$NDISKS) ==="

ZONE_BLKS=32768		# HAMMER2_ZONE_BYTES64 / 64 KB

# zone_seqs <idx>: "<zone> <txg_seq>" for every valid v3 header zone of
# disk idx, from the userland dump (it lists the zones that read back).
zone_seqs() {
    local idx="$1"
    hammer2 volhdr $DEVSPEC 2>/dev/null |
        awk -v v="$idx" '$1 == "Volume" && $2 == v && $6 ~ /^txg_seq=/ {
                z = $4; sub(/:/, "", z); s = $6; sub(/txg_seq=/, "", s)
                print z, s }' | sort -u
}

# max_seq <idx>: the newest header seq on disk idx.
max_seq() {
    zone_seqs "$1" | awk '$2 + 0 >= m + 0 { m = $2 } END { if (m != "") print m }'
}

# overall_max: the newest seq on any disk.
overall_max() {
    local i=0 m=0 s
    while [ "$i" -lt "$NDISKS" ]; do
        s=$(max_seq $i)
        [ "${s:-0}" -gt "$m" ] && m=$s
        i=$((i + 1))
    done
    echo $m
}

# rewind_disk <idx>: zero the newest header zone of disk idx.
rewind_disk() {
    local idx="$1" z
    z=$(zone_seqs "$idx" | awk 'NR == 1 || $2 + 0 > m + 0 { m = $2; z = $1 } END { print z }')
    [ -n "$z" ] || return 1
    dd if=/dev/zero of="$(disk_dev "$idx")" bs=65536 count=1 \
        seek=$((z * ZONE_BLKS)) conv=notrunc 2>/dev/null
}

# split_case <label> <first-disk> <k>: run one case.
split_case() {
    local label="$1" first="$2" k="$3"
    local i b a majority newn expect chosen_new late

    setup_fresh
    check_v3
    write_ref_data "base"
    umount $MNTPT || { result FAIL "$label: umount 1"; return; }
    a=$(overall_max)

    # Advance the headers with one more cycle; "late" is committed by it.
    mount -t hammer2 $PFSPATH $MNTPT ||
        { result FAIL "$label: remount for phase 2"; return; }
    dd if=/dev/urandom of=$MNTPT/late bs=65536 count=64 2>/dev/null
    sha256 $MNTPT/late > /var/tmp/s_late.txt
    sync; sync
    umount $MNTPT || { result FAIL "$label: umount 2"; return; }
    b=$(overall_max)
    if [ "${b:-0}" -le "${a:-0}" ]; then
        result FAIL "$label: header seq did not advance ($a -> $b)"
        return
    fi
    echo "  $label: header seq $a -> $b; zones disk0: $(zone_seqs 0 | tr '\n' ' ')"

    # Crash: the last k members never got their newest header.
    i=$first
    while [ "$i" -lt $((first + k)) ]; do
        if ! rewind_disk $i; then
            result FAIL "$label: cannot find the newest header zone of disk $i"
            return
        fi
        i=$((i + 1))
    done
    printf "  %s: newest seq per disk after rewind:" "$label"
    i=0
    while [ "$i" -lt "$NDISKS" ]; do
        printf " d%d=%s" $i "$(max_seq $i)"
        i=$((i + 1))
    done
    echo

    majority=$((NDISKS / 2 + 1))
    [ "$majority" -lt $((NDISKS - 2)) ] && majority=$((NDISKS - 2))
    newn=$((NDISKS - k))
    if [ "$newn" -ge "$majority" ]; then
        chosen_new=1
    else
        chosen_new=0
    fi

    kmsg_clear
    if ! mount -t hammer2 $PFSPATH $MNTPT 2>/var/tmp/s_mount.txt; then
        result FAIL "$label: mount refused after split header ($newn of $NDISKS at T+1)"
        kmsg | grep -i "quorum" | tail -3 | sed 's/^/      /'
        return
    fi
    result PASS "$label: mount succeeded ($newn of $NDISKS at T+1)"

    # Lagging disks: below the chosen seq -> FAILED; others ONLINE.
    i=0
    while [ "$i" -lt "$NDISKS" ]; do
        if [ "$chosen_new" = 1 ] && [ "$i" -ge "$first" ] &&
           [ "$i" -lt $((first + k)) ]; then
            expect=FAILED
        else
            expect=ONLINE
        fi
        if disk_state_is $i $expect; then
            result PASS "$label: disk $i is $expect"
        else
            result FAIL "$label: disk $i is not $expect"
        fi
        i=$((i + 1))
    done

    # Data of the chosen TXG.
    sha256 $MNTPT/base_a > /var/tmp/s_chk.txt 2>&1
    sha256 $MNTPT/base_b >> /var/tmp/s_chk.txt 2>&1
    if diff -q /var/tmp/v4_base.txt /var/tmp/s_chk.txt > /dev/null 2>&1; then
        result PASS "$label: synced base data intact"
    else
        result FAIL "$label: synced base data wrong or missing"
    fi
    if [ -e $MNTPT/late ]; then
        sha256 $MNTPT/late > /var/tmp/s_chk.txt 2>&1
        if diff -q /var/tmp/s_late.txt /var/tmp/s_chk.txt > /dev/null 2>&1; then
            result PASS "$label: late file whole"
        else
            result FAIL "$label: late file torn"
        fi
    elif [ "$chosen_new" = 1 ]; then
        result FAIL "$label: late file missing at the newest TXG"
    elif [ "$b" = "$((a + 1))" ]; then
        result PASS "$label: late file absent (rolled back to the TXG before it)"
    else
        result PASS "$label: late file absent (rolled back past it)"
    fi

    # Lagging disks need a resilver.
    if [ "$chosen_new" = 1 ]; then
        i=$first
        while [ "$i" -lt $((first + k)) ]; do
            if hammer2 -s $MNTPT raid replace "$(disk_dev $i)" \
                    "$(disk_dev $i)" > /var/tmp/s_replace.txt 2>&1 &&
               disk_state_is $i ONLINE; then
                result PASS "$label: replace of lagging disk $i"
            else
                result FAIL "$label: replace of lagging disk $i"
                sed 's/^/      /' /var/tmp/s_replace.txt
            fi
            i=$((i + 1))
        done
    fi

    # New writes must commit over any orphan header zones, and the
    # result must survive a cold mount.
    dd if=/dev/urandom of=$MNTPT/after bs=65536 count=32 2>/dev/null
    sha256 $MNTPT/after > /var/tmp/s_after.txt
    sync; sync
    if remount; then
        sha256 $MNTPT/after > /var/tmp/s_chk.txt 2>&1
        if diff -q /var/tmp/s_after.txt /var/tmp/s_chk.txt > /dev/null 2>&1; then
            result PASS "$label: new write survives a cold remount"
        else
            result FAIL "$label: new write lost after remount"
        fi
        i=0
        while [ "$i" -lt "$NDISKS" ]; do
            disk_state_is $i ONLINE ||
                result FAIL "$label: disk $i not ONLINE after remount"
            i=$((i + 1))
        done
        scrub_clean "$label"
    else
        result FAIL "$label: cold remount failed after the new commit"
    fi
    teardown "$label"
}

split_case S1 $((NDISKS - 1)) 1
split_case S2 $((NDISKS - 2)) 2
split_case S3 $((NDISKS - 3)) 3
split_case S4 0 1

summary
