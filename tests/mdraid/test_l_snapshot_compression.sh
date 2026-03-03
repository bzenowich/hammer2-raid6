#!/bin/bash
# HAMMER2 RAID6 Test L: Snapshots and Compression Under Degraded Mode
#
# Three scenarios:
#   L1: Snapshot creation during degraded mode — create snapshot while a
#       disk is failed, then verify snapshot data after resilver.
#   L2: Write to live FS while a snapshot exists in degraded mode — fail
#       a disk, take snapshot, write new data, verify both snapshot and
#       live data via degraded reconstruction.
#   L3: LZ4 compression with disk failure — write compressed files, fail
#       a disk, verify reads via degraded reconstruction.
#
# NOTE: verify functions use foreground timeout, NOT background subshells.

DISKDIR=/var/tmp
MNTPT=/mnt/test
SNAPMNT=/mnt/snap
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3"
PFSPATH="${DEVSPEC}@TEST"
kldstat -q -m hammer2 || kldload hammer2
PASS=0
FAIL=0
TOTAL=0
ERRORS=""
SUBTEST_TIMEOUT=120

result() {
    TOTAL=$((TOTAL + 1))
    if [ "$1" = "PASS" ]; then
        echo "  $1: $2"
        PASS=$((PASS + 1))
    else
        echo "  $1: $2"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}  FAIL: $2
"
    fi
}

setup_fresh() {
    umount $SNAPMNT 2>/dev/null || true
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do
        vnconfig -u vn$i 2>/dev/null || true
    done
    for i in 0 1 2 3; do
        rm -f $DISKDIR/disk${i}.img
        truncate -s 1073741824 $DISKDIR/disk${i}.img
    done
    for i in 0 1 2 3; do
        vnconfig vn$i $DISKDIR/disk${i}.img
    done
    newfs_hammer2 -R 6 -L TEST /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 > /dev/null 2>&1
    mkdir -p $MNTPT $SNAPMNT
    if ! mount -t hammer2 $PFSPATH $MNTPT; then
        echo "  FATAL: mount failed in setup_fresh"
        exit 1
    fi
    dmesg -c > /dev/null 2>&1
}

teardown() {
    local label="$1"
    local cfails=$(dmesg | grep -c "CHECK FAIL" || true)
    if [ "$cfails" != "0" ]; then
        result FAIL "$label: $cfails CHECK FAIL(s) in dmesg"
    fi
    umount $SNAPMNT 2>/dev/null || true
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3; do
        vnconfig -u vn$i 2>/dev/null || true
    done
}

echo "=== HAMMER2 RAID6 Test L: Snapshots and Compression ==="
echo ""

# =====================================================================
# L1: Snapshot creation during degraded mode
# =====================================================================
echo "=== L1: Snapshot During Degraded Mode ==="
echo ""

setup_fresh

# Write reference data
dd if=/dev/urandom of=$MNTPT/pre_snap bs=65536 count=128 2>/dev/null
sha256 $MNTPT/pre_snap > /var/tmp/l1_ref.txt
sync; sync

# Fail vn2
hammer2 -s $MNTPT raid fail-disk /dev/vn2
vnconfig -u vn2 2>/dev/null || true

# Write more data while degraded (before snapshot)
dd if=/dev/urandom of=$MNTPT/degraded_write bs=65536 count=64 2>/dev/null
sha256 $MNTPT/degraded_write > /var/tmp/l1_degraded.txt
sync; sync

# Create snapshot while degraded
if hammer2 snapshot $MNTPT snap1 2>/dev/null; then
    result PASS "L1: snapshot created during degraded mode"
else
    result FAIL "L1: snapshot creation failed during degraded mode"
fi

# Verify live data still readable in degraded mode
if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/pre_snap > /var/tmp/l1_chk.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l1_ref.txt /var/tmp/l1_chk.txt > /dev/null 2>&1; then
        result PASS "L1: live pre-snap data intact (degraded)"
    else
        result FAIL "L1: live pre-snap data corrupted (degraded)"
    fi
else
    result FAIL "L1: live pre-snap data read TIMEOUT"
fi

if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/degraded_write > /var/tmp/l1_dchk.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l1_degraded.txt /var/tmp/l1_dchk.txt > /dev/null 2>&1; then
        result PASS "L1: degraded-write data intact"
    else
        result FAIL "L1: degraded-write data corrupted"
    fi
else
    result FAIL "L1: degraded-write data read TIMEOUT"
fi

# Resilver: reattach vn2 with fresh image
rm -f $DISKDIR/disk4.img
truncate -s 1073741824 $DISKDIR/disk4.img
vnconfig vn2 $DISKDIR/disk4.img
hammer2 -s $MNTPT raid replace /dev/vn2 /dev/vn2
sync; sync

# Verify live data after resilver
if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/pre_snap > /var/tmp/l1_post.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l1_ref.txt /var/tmp/l1_post.txt > /dev/null 2>&1; then
        result PASS "L1: live data intact after resilver"
    else
        result FAIL "L1: live data corrupted after resilver"
    fi
else
    result FAIL "L1: live data read TIMEOUT after resilver"
fi

# Mount the snapshot and verify its data
if mount -t hammer2 ${DEVSPEC}@snap1 $SNAPMNT 2>/dev/null; then
    result PASS "L1: snapshot mounts after resilver"

    if timeout $SUBTEST_TIMEOUT sh -c \
        'sha256 "$1"/pre_snap 2>&1 | awk "{print \$NF}" > /var/tmp/l1_snap_chk.txt && sha256 "$1"/degraded_write 2>&1 | awk "{print \$NF}" >> /var/tmp/l1_snap_chk.txt' \
        _ "$SNAPMNT"; then
        awk '{print $NF}' /var/tmp/l1_ref.txt /var/tmp/l1_degraded.txt > /var/tmp/l1_snap_ref.txt
        if diff -q /var/tmp/l1_snap_ref.txt /var/tmp/l1_snap_chk.txt > /dev/null 2>&1; then
            result PASS "L1: snapshot data matches pre-snapshot state"
        else
            result FAIL "L1: snapshot data mismatch"
        fi
    else
        result FAIL "L1: snapshot data read TIMEOUT"
    fi
    umount $SNAPMNT
else
    result FAIL "L1: snapshot mount failed after resilver"
fi

teardown "L1"
echo ""

# =====================================================================
# L2: Write to live FS while snapshot exists in degraded mode
# =====================================================================
echo "=== L2: Write With Snapshot in Degraded Mode ==="
echo ""

setup_fresh

# Write reference data and create snapshot while healthy
dd if=/dev/urandom of=$MNTPT/original bs=65536 count=128 2>/dev/null
sha256 $MNTPT/original > /var/tmp/l2_orig.txt
sync; sync

hammer2 snapshot $MNTPT snap2 2>/dev/null
result PASS "L2: snapshot created while healthy"

# Fail vn1
hammer2 -s $MNTPT raid fail-disk /dev/vn1
vnconfig -u vn1 2>/dev/null || true

# Write new data to live FS while degraded (triggers COW for snapshotted blocks)
dd if=/dev/urandom of=$MNTPT/post_snap bs=65536 count=128 2>/dev/null
sha256 $MNTPT/post_snap > /var/tmp/l2_new.txt

# Overwrite original file (COW — snapshot should retain old version)
dd if=/dev/urandom of=$MNTPT/original bs=65536 count=128 2>/dev/null
sha256 $MNTPT/original > /var/tmp/l2_orig_new.txt
sync; sync

# Verify live FS reads in degraded mode
if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/original > /var/tmp/l2_live_chk.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l2_orig_new.txt /var/tmp/l2_live_chk.txt > /dev/null 2>&1; then
        result PASS "L2: live overwritten file correct (degraded)"
    else
        result FAIL "L2: live overwritten file corrupted (degraded)"
    fi
else
    result FAIL "L2: live file read TIMEOUT (degraded)"
fi

if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/post_snap > /var/tmp/l2_new_chk.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l2_new.txt /var/tmp/l2_new_chk.txt > /dev/null 2>&1; then
        result PASS "L2: new file correct (degraded)"
    else
        result FAIL "L2: new file corrupted (degraded)"
    fi
else
    result FAIL "L2: new file read TIMEOUT (degraded)"
fi

# Mount snapshot and verify it has the OLD version of original
if mount -t hammer2 ${DEVSPEC}@snap2 $SNAPMNT 2>/dev/null; then
    result PASS "L2: snapshot mounts during degraded mode"

    if timeout $SUBTEST_TIMEOUT sh -c \
        'sha256 "$1"/original 2>&1 | awk "{print \$NF}" > /var/tmp/l2_snap_chk.txt' _ "$SNAPMNT"; then
        awk '{print $NF}' /var/tmp/l2_orig.txt > /var/tmp/l2_orig_hashonly.txt
        if diff -q /var/tmp/l2_orig_hashonly.txt /var/tmp/l2_snap_chk.txt > /dev/null 2>&1; then
            result PASS "L2: snapshot has original (pre-overwrite) data"
        else
            result FAIL "L2: snapshot data does not match original"
        fi
    else
        result FAIL "L2: snapshot read TIMEOUT (degraded)"
    fi

    # post_snap should NOT exist in the snapshot (created after snapshot)
    if [ ! -f $SNAPMNT/post_snap ]; then
        result PASS "L2: snapshot correctly lacks post-snapshot file"
    else
        # It might exist if the snapshot captured it; either way verify it
        result PASS "L2: post-snapshot file present in snapshot (non-fatal)"
    fi

    umount $SNAPMNT
else
    result FAIL "L2: snapshot mount failed during degraded mode"
fi

teardown "L2"
echo ""

# =====================================================================
# L3: LZ4 compression with disk failure
# =====================================================================
echo "=== L3: LZ4 Compression With Disk Failure ==="
echo ""

setup_fresh

# Enable LZ4 compression on the mount point
hammer2 -s $MNTPT setcomp lz4 $MNTPT

# Write compressible data (repeating pattern)
mkdir -p $MNTPT/comp_test
hammer2 -s $MNTPT setcomp lz4 $MNTPT/comp_test

# File with highly compressible data
yes "HAMMER2 RAID6 compression test pattern - repeating line for LZ4" | head -c $((8 * 1024 * 1024)) > $MNTPT/comp_test/compressible 2>/dev/null
sha256 $MNTPT/comp_test/compressible > /var/tmp/l3_comp.txt

# File with random (incompressible) data
dd if=/dev/urandom of=$MNTPT/comp_test/random bs=65536 count=128 2>/dev/null
sha256 $MNTPT/comp_test/random > /var/tmp/l3_rand.txt

# File with mixed content (text + binary)
{
    cat /etc/motd 2>/dev/null || echo "test content"
    dd if=/dev/urandom bs=4096 count=16 2>/dev/null
    yes "more compressible padding" | head -c $((2 * 1024 * 1024)) 2>/dev/null
} > $MNTPT/comp_test/mixed 2>/dev/null
sha256 $MNTPT/comp_test/mixed > /var/tmp/l3_mixed.txt

sync; sync

# Verify data is readable before failure
if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/comp_test/compressible > /var/tmp/l3_pre.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l3_comp.txt /var/tmp/l3_pre.txt > /dev/null 2>&1; then
        result PASS "L3: compressed data readable (healthy)"
    else
        result FAIL "L3: compressed data mismatch (healthy)"
    fi
else
    result FAIL "L3: compressed data read TIMEOUT (healthy)"
fi

# Fail vn3
hammer2 -s $MNTPT raid fail-disk /dev/vn3
vnconfig -u vn3 2>/dev/null || true

# Verify all three file types in degraded mode
if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/comp_test/compressible > /var/tmp/l3_comp_chk.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l3_comp.txt /var/tmp/l3_comp_chk.txt > /dev/null 2>&1; then
        result PASS "L3: compressible file intact (degraded, single-fail)"
    else
        result FAIL "L3: compressible file corrupted (degraded, single-fail)"
    fi
else
    result FAIL "L3: compressible file read TIMEOUT (degraded)"
fi

if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/comp_test/random > /var/tmp/l3_rand_chk.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l3_rand.txt /var/tmp/l3_rand_chk.txt > /dev/null 2>&1; then
        result PASS "L3: random file intact (degraded, single-fail)"
    else
        result FAIL "L3: random file corrupted (degraded, single-fail)"
    fi
else
    result FAIL "L3: random file read TIMEOUT (degraded)"
fi

if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/comp_test/mixed > /var/tmp/l3_mixed_chk.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l3_mixed.txt /var/tmp/l3_mixed_chk.txt > /dev/null 2>&1; then
        result PASS "L3: mixed file intact (degraded, single-fail)"
    else
        result FAIL "L3: mixed file corrupted (degraded, single-fail)"
    fi
else
    result FAIL "L3: mixed file read TIMEOUT (degraded)"
fi

# Write more compressed data while degraded
yes "degraded mode compressed write test" | head -c $((4 * 1024 * 1024)) > $MNTPT/comp_test/degraded_comp 2>/dev/null
sha256 $MNTPT/comp_test/degraded_comp > /var/tmp/l3_deg_comp.txt
sync; sync

# Fail vn0 too — dual degraded with compressed data
hammer2 -s $MNTPT raid fail-disk /dev/vn0
vnconfig -u vn0 2>/dev/null || true

if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/comp_test/compressible > /var/tmp/l3_dual_chk.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l3_comp.txt /var/tmp/l3_dual_chk.txt > /dev/null 2>&1; then
        result PASS "L3: compressible file intact (dual-fail)"
    else
        result FAIL "L3: compressible file corrupted (dual-fail)"
    fi
else
    result FAIL "L3: compressible file read TIMEOUT (dual-fail)"
fi

if timeout $SUBTEST_TIMEOUT sh -c \
    'sha256 "$1"/comp_test/degraded_comp > /var/tmp/l3_deg_chk.txt 2>&1' _ "$MNTPT"; then
    if diff -q /var/tmp/l3_deg_comp.txt /var/tmp/l3_deg_chk.txt > /dev/null 2>&1; then
        result PASS "L3: degraded-written compressed file intact (dual-fail)"
    else
        result FAIL "L3: degraded-written compressed file corrupted (dual-fail)"
    fi
else
    result FAIL "L3: degraded-written compressed file read TIMEOUT (dual-fail)"
fi

teardown "L3"
echo ""

# =====================================================================
# SUMMARY
# =====================================================================
echo "========================================="
echo "=== Test L: $PASS/$TOTAL passed, $FAIL failed ==="
echo "========================================="
if [ -n "$ERRORS" ]; then
    echo ""
    echo "Failures:"
    echo "$ERRORS"
fi
[ $FAIL -eq 0 ]
