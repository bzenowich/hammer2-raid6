# HAMMER2 RAIDZ2-Native: Test Specification

This document specifies the test suite for the RAIDZ2-native HAMMER2
implementation described in `raidz2_in_hammer2.md` and
`raidz2_snapshot_interaction.md`. The RAIDZ2-native format (volume version 4)
differs from the current RAID6 overlay (version 3) in three fundamental ways:

1. `blockref.data_off` stores a **physical column offset** on the data disk,
   not a logical offset in HAMMER2's address space.
2. `blockref.copyid` stores the **physical disk index** (0–ndisks-1), not a
   copy identifier.
3. Every data block write allocates a **fresh stripe slot** (unconditional COW,
   `newmod = 1` always in RAID6 mode) — no in-place overwrites, no RMW.

The test suite must verify all three invariants hold under every combination
of healthy, degraded, and recovery conditions.

---

## Test Infrastructure

### Device Setup: Swap-Backed vn Devices

All tests use swap-backed vn devices (`vnconfig -S`), which bypass the UFS
layer entirely. This eliminates the runningbufspace accumulation that required
Fix 13 (IO_SYNC in `vn.c`) and makes Fix 15 (`BUF_CMD_FLUSH`) a true no-op
rather than a UFS drain operation. The test infrastructure matches what
physical hardware would see, minus real disk latency.

```sh
# Format: swap-backed 1 GB devices — no image files, no UFS layer
for i in 0 1 2 3 4 5; do
    vnconfig -S 1073741824 vn${i}
done

# Format with v4 (RAIDZ2-native) — requires -R 6 and new volume version
newfs_hammer2 -R 6 -L TEST \
    /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 /dev/vn4 /dev/vn5

mount -t hammer2 \
    /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3:/dev/vn4:/dev/vn5@TEST /mnt/test
```

The `-R 6` flag instructs `newfs_hammer2` to produce a v4 (RAIDZ2-native)
volume. A v3 (RAID6 overlay) volume must not mount as v4 and vice versa; the
kernel must reject the wrong version with a clear error message.

Teardown:
```sh
umount /mnt/test 2>/dev/null || true
for i in 0 1 2 3 4 5; do
    vnconfig -u vn${i} 2>/dev/null || true
done
```

No image files to delete. No `sync` needed to drain UFS (no UFS layer).

### Directory Layout

```
tests/raidz2native/
    common.sh               # shared setup/teardown/result functions
    test_a_basic.sh         # Group A: healthy read/write + format verification
    test_b_single_fail.sh   # Group B: single disk failure
    test_c_dual_fail.sh     # Group C: dual disk failure (all 15 combinations)
    test_d_resilver.sh      # Group D: resilver
    test_e_snapshot.sh      # Group E: snapshot + RAID6 interaction
    test_f_cow_invariant.sh # Group F: COW invariant verification
    test_g_autofail.sh      # Group G: auto-fail and degraded mount
    test_h_all_combos.sh    # Group H: all 15 dual-failure combinations automated
    test_i_unclean.sh       # Group I: unclean unmount
    run_all.sh              # run every group, collect results
```

### `common.sh` Shared Functions

```sh
#!/bin/sh
# common.sh — shared helpers for raidz2native tests

DISKDIR_NONE=swap   # sentinel: no image files
MNTPT=/mnt/rz2test
NDISKS=6
DEVSPEC="/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3:/dev/vn4:/dev/vn5"
PFSPATH="${DEVSPEC}@RZ2TEST"

PASS=0; FAIL=0; TOTAL=0; ERRORS=""

result() {
    TOTAL=$((TOTAL + 1))
    if [ "$1" = "PASS" ]; then
        echo "  PASS: $2"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $2"; FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}  FAIL: $2\n"
    fi
}

setup_fresh() {
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3 4 5; do
        vnconfig -u vn$i 2>/dev/null || true
        vnconfig -S 1073741824 vn$i
    done
    newfs_hammer2 -R 6 -L RZ2TEST \
        /dev/vn0 /dev/vn1 /dev/vn2 /dev/vn3 /dev/vn4 /dev/vn5 \
        > /dev/null 2>&1
    mkdir -p $MNTPT
    if ! mount -t hammer2 $PFSPATH $MNTPT; then
        echo "  FATAL: mount failed"; exit 1
    fi
    dmesg -c > /dev/null 2>&1
}

teardown() {
    local label="$1"
    local cfails=$(dmesg | grep -c "CHECK FAIL" 2>/dev/null || echo 0)
    if [ "$cfails" != "0" ]; then
        result FAIL "$label: $cfails CHECK FAIL(s) in dmesg"
    fi
    umount $MNTPT 2>/dev/null || true
    for i in 0 1 2 3 4 5; do
        vnconfig -u vn$i 2>/dev/null || true
    done
}

# Write two random files and record their checksums
write_ref_data() {
    local prefix="${1:-ref}"
    dd if=/dev/urandom of=$MNTPT/${prefix}_a bs=65536 count=128 2>/dev/null
    dd if=/dev/urandom of=$MNTPT/${prefix}_b bs=65536 count=64  2>/dev/null
    sha256 $MNTPT/${prefix}_a > /var/tmp/rz2_${prefix}.txt
    sha256 $MNTPT/${prefix}_b >> /var/tmp/rz2_${prefix}.txt
    sync; sync
}

# Verify two files against saved checksums
verify_ref() {
    local label="$1"
    local prefix="${2:-ref}"
    sha256 $MNTPT/${prefix}_a > /var/tmp/rz2_check.txt 2>&1
    sha256 $MNTPT/${prefix}_b >> /var/tmp/rz2_check.txt 2>&1
    if diff -q /var/tmp/rz2_${prefix}.txt /var/tmp/rz2_check.txt \
            > /dev/null 2>&1; then
        result PASS "$label"
    else
        result FAIL "$label"
    fi
}

summary() {
    echo "========================================="
    echo "=== $PASS/$TOTAL PASS, $FAIL FAIL ==="
    echo "========================================="
    [ -n "$ERRORS" ] && printf "Failures:\n$ERRORS"
    [ $FAIL -eq 0 ]
}
```

### Target VM

- DragonFlyBSD 6.4.2 at 192.168.25.66
- `kldstat -q -m hammer2 || kldload hammer2` at the top of each script
- All scripts must run under `/bin/sh` (not bash) unless a bash feature is
  explicitly required; DragonFlyBSD's default shell is tcsh but `/bin/sh` is
  available

---

## Version Guard

Before any test runs, verify the on-disk format version:

```sh
# After newfs_hammer2 -R 6 + mount, check volume version
hammer2 -s $MNTPT volconf | grep -q "version.*4"
```

Expected output fragment:
```
HAMMER2 volume version: 4
```

If the version is not 4, all tests in this suite must be skipped with a clear
message: `SKIP: v4 (RAIDZ2-native) format not yet implemented`. This ensures
the test suite can be added to CI before the implementation is complete without
producing misleading failures.

---

## Group A: Basic Read/Write (Healthy Array)

**Purpose**: Verify that a freshly-formatted v4 array accepts writes and
returns correct data under normal (no-failure) conditions.

### A1: Format, write, checksum, unmount, remount, verify

**Setup**: `setup_fresh`

**Steps**:
```sh
# Write 100 MB file
dd if=/dev/urandom of=$MNTPT/bigfile bs=65536 count=1600 2>/dev/null
sha256 $MNTPT/bigfile > /var/tmp/a1_ref.txt
sync; sync

# Unmount and remount
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT

# Verify
sha256 $MNTPT/bigfile > /var/tmp/a1_check.txt
diff /var/tmp/a1_ref.txt /var/tmp/a1_check.txt
```

**Pass criteria**:
- Mount succeeds after `newfs_hammer2 -R 6`
- `sha256` before and after unmount/remount matches exactly
- No `CHECK FAIL` in dmesg
- No kernel panic

### A2: Small file variety

**Setup**: `setup_fresh`

**Steps**: Write one file of each size from the table below, record checksums,
unmount, remount, verify all checksums.

| File | Size | Write command |
|------|------|---------------|
| `f_1k` | 1 KB | `dd if=/dev/urandom of=$MNTPT/f_1k bs=1024 count=1` |
| `f_4k` | 4 KB | `dd if=/dev/urandom of=$MNTPT/f_4k bs=4096 count=1` |
| `f_64k` | 64 KB | `dd if=/dev/urandom of=$MNTPT/f_64k bs=65536 count=1` |
| `f_1m` | 1 MB | `dd if=/dev/urandom of=$MNTPT/f_1m bs=65536 count=16` |
| `f_16m` | 16 MB | `dd if=/dev/urandom of=$MNTPT/f_16m bs=65536 count=256` |

```sh
for f in f_1k f_4k f_64k f_1m f_16m; do
    sha256 $MNTPT/$f >> /var/tmp/a2_ref.txt
done
sync; sync
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT
for f in f_1k f_4k f_64k f_1m f_16m; do
    sha256 $MNTPT/$f >> /var/tmp/a2_check.txt
done
diff /var/tmp/a2_ref.txt /var/tmp/a2_check.txt
```

**Pass criteria**: All five checksums match after remount. No `CHECK FAIL`.

### A3: Blockref encoding verification (`copyid` = disk_idx, `data_off` = physical)

**Purpose**: Confirm that `blockref.copyid` encodes the physical disk index and
`blockref.data_off` encodes a physical column offset, not a logical address.

**Setup**: `setup_fresh`, write one file, `sync`.

**Steps**:
```sh
# Write a file, ensure it flushes to disk
dd if=/dev/urandom of=$MNTPT/probe bs=65536 count=4 2>/dev/null
sync; sync

# Dump the block tree — look for DATA blockrefs
hammer2 -s $MNTPT show > /var/tmp/a3_show.txt 2>&1

# Extract copyid and data_off for DATA blocks
grep -A 2 "type=DATA" /var/tmp/a3_show.txt | head -30
```

**Expected output** (v4 format):

For each DATA blockref, `copyid` must be in the range `[0, ndisks-1]` = `[0, 5]`
and `data_off` must be aligned to the stripe unit (64 KB = 0x10000):

```
type=DATA copyid=2 data_off=0x1030000 ...
type=DATA copyid=0 data_off=0x1030000 ...
```

**Automated check**:
```sh
# All DATA blockrefs must have copyid in [0..5] and
# data_off & 0xffff == 0 (radix bits stripped: data_off >> 6 is 64KB-aligned)
# The lower 6 bits encode the radix; the address portion is data_off & ~0x3f
# For 64KB allocations: (data_off & ~0x3f) must be 64KB-aligned

python3 - <<'EOF'
import re, sys
data = open('/var/tmp/a3_show.txt').read()
blocks = re.findall(r'type=DATA copyid=(\d+) data_off=(0x[0-9a-f]+)', data)
if not blocks:
    print("SKIP: no DATA blocks found (file too small for separate DIO?)")
    sys.exit(0)
errors = []
for copyid, data_off in blocks:
    c = int(copyid)
    d = int(data_off, 16)
    addr = d & ~0x3f   # strip radix bits
    if not (0 <= c <= 5):
        errors.append(f"copyid={c} out of range [0,5]")
    if addr % (64 * 1024) != 0:
        errors.append(f"data_off={hex(d)} addr={hex(addr)} not 64KB-aligned")
if errors:
    print("FAIL:", "; ".join(errors)); sys.exit(1)
print(f"PASS: {len(blocks)} DATA blocks checked, all valid")
EOF
```

**Pass criteria**:
- All DATA blockrefs have `copyid` in `[0, 5]`
- Address portion of `data_off` (bits 63:6) is 64 KB-aligned for 64 KB DIOs
- Sub-64 KB blocks may land at sub-64 KB offsets within a DIO (linear packing)
- A v3 (RAID6 overlay) format would show `copyid=255` and logical addresses
  in `data_off`; v4 must not show `copyid=255` for any data block

### A4: COW — overwrite allocates new stripe slot

**Purpose**: Verify that overwriting a file allocates a new physical stripe
slot (fresh `data_off`) rather than reusing the old one. This is the
unconditional COW invariant from `raidz2_snapshot_interaction.md`.

**Setup**: `setup_fresh`

**Steps**:
```sh
# Write a file and record its blockref addresses
dd if=/dev/urandom of=$MNTPT/cow_test bs=65536 count=8 2>/dev/null
sync; sync
hammer2 -s $MNTPT show > /var/tmp/a4_before.txt 2>&1
grep -A 1 "cow_test" /var/tmp/a4_before.txt | grep "data_off" \
    > /var/tmp/a4_addrs_before.txt

# Overwrite the file
dd if=/dev/urandom of=$MNTPT/cow_test bs=65536 count=8 2>/dev/null
sync; sync
hammer2 -s $MNTPT show > /var/tmp/a4_after.txt 2>&1
grep -A 1 "cow_test" /var/tmp/a4_after.txt | grep "data_off" \
    > /var/tmp/a4_addrs_after.txt

# No address should appear in both before and after
sort /var/tmp/a4_addrs_before.txt > /var/tmp/a4_sorted_before.txt
sort /var/tmp/a4_addrs_after.txt  > /var/tmp/a4_sorted_after.txt
comm -12 /var/tmp/a4_sorted_before.txt /var/tmp/a4_sorted_after.txt \
    > /var/tmp/a4_overlap.txt
```

**Pass criteria**:
- `/var/tmp/a4_overlap.txt` is empty (no stripe slot reused)
- If the file is small enough to fit in a single DIO and that DIO is packed
  with other blocks, the DIO address may be the same (linear packing within
  a stripe column is expected); the test should operate on a file large enough
  to span at least 4 DIOs (512 KB = 8 × 64 KB) to avoid this case

---

## Group B: Single Disk Failure

**Purpose**: Verify degraded reads via single-column reconstruction for every
disk position (data and parity columns).

The 6-disk array has positions vn0–vn5. With left-symmetric rotation, the
P and Q disks rotate per stripe; failing a parity disk is different from
failing a data disk. All 6 positions must be tested.

### B1–B6: Fail each disk in turn

For each `DISK` in `0 1 2 3 4 5`:

**Setup**: `setup_fresh`, `write_ref_data`

**Steps**:
```sh
DISK=<0..5>
hammer2 -s $MNTPT raid fail-disk /dev/vn${DISK}
vnconfig -u vn${DISK} 2>/dev/null || true
# Verify reads via reconstruction
verify_ref "B${DISK}: single-fail vn${DISK}"
teardown "B${DISK}"
```

**Pass criteria** for each of the six cases:
- `verify_ref` passes (checksums match)
- No `CHECK FAIL` in dmesg
- `hammer2 -s $MNTPT raid status` (or equivalent) shows disk `DISK` as FAILED
  and exactly 1 disk failed

### B7: Degraded write after single failure

**Setup**: `setup_fresh`, `write_ref_data "ref"`

**Steps**:
```sh
hammer2 -s $MNTPT raid fail-disk /dev/vn2
vnconfig -u vn2

# Write new data while degraded
dd if=/dev/urandom of=$MNTPT/dw_a bs=65536 count=128 2>/dev/null
dd if=/dev/urandom of=$MNTPT/dw_b bs=65536 count=64  2>/dev/null
sha256 $MNTPT/dw_a > /var/tmp/b7_new.txt
sha256 $MNTPT/dw_b >> /var/tmp/b7_new.txt
sync; sync

# Fail a second disk (force dual-degraded read path)
hammer2 -s $MNTPT raid fail-disk /dev/vn5
vnconfig -u vn5

# Verify both the pre-fail reference data and the degraded-written data
verify_ref "B7: pre-fail reference (dual-degraded)" "ref"
sha256 $MNTPT/dw_a > /var/tmp/b7_check.txt 2>&1
sha256 $MNTPT/dw_b >> /var/tmp/b7_check.txt 2>&1
if diff -q /var/tmp/b7_new.txt /var/tmp/b7_check.txt > /dev/null 2>&1; then
    result PASS "B7: degraded-written data correct (dual reconstruction)"
else
    result FAIL "B7: degraded-written data mismatch"
fi
teardown "B7"
```

**Pass criteria**:
- Reference data checksums match through dual-degraded reconstruction
- Degraded-written data checksums match through dual-degraded reconstruction
- No `CHECK FAIL` in dmesg

### B8: Auto-fail triggers on EIO

**Context**: On swap-backed vn devices, EIO cannot be reliably injected
because vn never returns EIO (it returns zeros for unwritten pages). This test
is documented here for completeness and must be run on real hardware or with
a fault-injection mechanism.

**On real hardware or with vn fault injection**:
```sh
# Inject a sector error at a known physical offset on vn2
# (mechanism is platform-specific; on QEMU: hdparm --yes-i-know-what-i-am-doing
#  or a custom sector-corruptor for the backing image)
<inject error on vn2 sector at offset 0x1000000>

# Read a file whose data falls on vn2 at that offset
sha256 $MNTPT/ref_a > /dev/null 2>&1

# Expected: kernel calls hammer2_raid6_auto_fail_disk(hmp, 2)
dmesg | grep "auto-failed"
```

**Expected dmesg**:
```
hammer2: RAID6 auto-failed disk 2 (dev vn2) after EIO at offset 0x1000000
```

**Pass criteria**:
- dmesg contains the auto-fail message referencing disk 2
- Subsequent reads still return correct data (reconstruction from remaining 5)
- After unmount/remount, disk 2 is still marked FAILED (state persisted)

---

## Group C: Dual Disk Failure

**Purpose**: Verify dual-reconstruction (`dual_recov`) for all 15 unique pairs
of 2 disks failing from a 6-disk array.

The 15 pairs are:

| # | Pair | Type |
|---|------|------|
| C01 | vn0, vn1 | data + data |
| C02 | vn0, vn2 | data + data |
| C03 | vn0, vn3 | data + data |
| C04 | vn0, vn4 | data + parity (rotated) |
| C05 | vn0, vn5 | data + parity (rotated) |
| C06 | vn1, vn2 | data + data |
| C07 | vn1, vn3 | data + data |
| C08 | vn1, vn4 | data + parity (rotated) |
| C09 | vn1, vn5 | data + parity (rotated) |
| C10 | vn2, vn3 | data + data |
| C11 | vn2, vn4 | data + parity (rotated) |
| C12 | vn2, vn5 | data + parity (rotated) |
| C13 | vn3, vn4 | data + parity (rotated) |
| C14 | vn3, vn5 | data + parity (rotated) |
| C15 | vn4, vn5 | parity + parity (rotated) |

Note: which disks hold P/Q for a given stripe depends on the stripe number
(left-symmetric rotation). At the array level, each disk will act as a data
disk for some stripes and a P or Q disk for others. The labeling above is
approximate; the test verifies reconstruction correctness regardless of
per-stripe column assignments.

**Template for each C-test**:

```sh
setup_fresh
write_ref_data "ref"

A=<first disk>
B=<second disk>
hammer2 -s $MNTPT raid fail-disk /dev/vn${A}
hammer2 -s $MNTPT raid fail-disk /dev/vn${B}
vnconfig -u vn${A} 2>/dev/null || true
vnconfig -u vn${B} 2>/dev/null || true

verify_ref "C$(printf '%02d' $N): dual-fail vn${A}+vn${B}" "ref"
teardown "C$(printf '%02d' $N)"
```

**Pass criteria for every C-test**:
- Checksums match (both files)
- No `CHECK FAIL` in dmesg
- `raid status` shows exactly 2 disks FAILED

All 15 C-tests should be run by `test_c_dual_fail.sh` in a single script
that iterates the pair list and produces per-pair PASS/FAIL output followed
by a summary line: `C: N/15 PASS`.

---

## Group D: Resilver

**Purpose**: Verify that a failed disk can be replaced and all data is
reconstructed correctly.

### D1: Basic resilver

**Setup**: `setup_fresh`, write 50 MB reference data

```sh
dd if=/dev/urandom of=$MNTPT/resilver_ref bs=65536 count=800 2>/dev/null
sha256 $MNTPT/resilver_ref > /var/tmp/d1_ref.txt
sync; sync
```

**Steps**:
```sh
# Fail vn3, detach it
hammer2 -s $MNTPT raid fail-disk /dev/vn3
vnconfig -u vn3

# Attach a fresh replacement at the same device node
vnconfig -S 1073741824 vn3

# Resilver
hammer2 -s $MNTPT raid replace /dev/vn3 /dev/vn3
echo "Resilver complete: $?"

# Verify data mounted (post-resilver, all 6 disks online)
sha256 $MNTPT/resilver_ref > /var/tmp/d1_check.txt 2>&1
diff /var/tmp/d1_ref.txt /var/tmp/d1_check.txt

# Unmount and remount — full 6-disk healthy read
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT
sha256 $MNTPT/resilver_ref > /var/tmp/d1_check2.txt 2>&1
diff /var/tmp/d1_ref.txt /var/tmp/d1_check2.txt
```

**Pass criteria**:
- Resilver command exits 0
- Both post-resilver checks match the reference
- No `CHECK FAIL` in dmesg at any point
- After remount the array is healthy (0 failed disks) and data is correct

### D2: Sequential resilvers (fail, resilver, fail again)

**Setup**: `setup_fresh`, `write_ref_data "r1"`

**Steps**:
```sh
# First failure: fail vn1, resilver
hammer2 -s $MNTPT raid fail-disk /dev/vn1
vnconfig -u vn1
vnconfig -S 1073741824 vn1
hammer2 -s $MNTPT raid replace /dev/vn1 /dev/vn1
verify_ref "D2: after first resilver" "r1"

# Write more data
write_ref_data "r2"

# Second failure: fail vn4, resilver
hammer2 -s $MNTPT raid fail-disk /dev/vn4
vnconfig -u vn4
vnconfig -S 1073741824 vn4
hammer2 -s $MNTPT raid replace /dev/vn4 /dev/vn4
verify_ref "D2: r1 after second resilver" "r1"
verify_ref "D2: r2 after second resilver" "r2"
```

**Pass criteria**:
- Both reference datasets intact after each resilver
- No `CHECK FAIL` in dmesg

### D3: Write during resilver (concurrent I/O)

**Setup**: `setup_fresh`, `write_ref_data "pre"`

**Steps**:
```sh
# Fail vn2, attach replacement
hammer2 -s $MNTPT raid fail-disk /dev/vn2
vnconfig -u vn2
vnconfig -S 1073741824 vn2

# Start resilver in background
hammer2 -s $MNTPT raid replace /dev/vn2 /dev/vn2 &
RPID=$!

# Write concurrently while resilver runs
sleep 1
dd if=/dev/urandom of=$MNTPT/concurrent bs=65536 count=256 2>/dev/null
sha256 $MNTPT/concurrent > /var/tmp/d3_concurrent.txt
sync; sync

# Wait for resilver
wait $RPID
RESILVER_RC=$?
if [ $RESILVER_RC -eq 0 ]; then
    result PASS "D3: resilver completed"
else
    result FAIL "D3: resilver exited $RESILVER_RC"
fi

# Verify both datasets
verify_ref "D3: pre-resilver data intact" "pre"
sha256 $MNTPT/concurrent > /var/tmp/d3_check.txt 2>&1
if diff -q /var/tmp/d3_concurrent.txt /var/tmp/d3_check.txt > /dev/null 2>&1; then
    result PASS "D3: concurrent-write data intact"
else
    result FAIL "D3: concurrent-write data mismatch"
fi

# Unmount + remount to confirm persistence
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT
verify_ref "D3: pre-resilver data after remount" "pre"
```

**Pass criteria**:
- Resilver exits 0
- Pre-resilver data and concurrent-write data both match
- No deadlock (resilver must complete within 5 minutes; use a watchdog)
- No `CHECK FAIL` in dmesg

### D4: Dirty-range re-resilver (Phase 4 verification)

**Purpose**: Verify that writes to the array while a resilver is in progress
are covered by the dirty-range re-resilver (Phase 4, implemented in Fix 12).
A write that races with Phase 3 must not leave stale data on the replacement
disk.

**Setup**: `setup_fresh`, write 50 MB reference data

**Steps**:
```sh
# Fail vn0
hammer2 -s $MNTPT raid fail-disk /dev/vn0
vnconfig -u vn0
vnconfig -S 1073741824 vn0

# Start resilver in background
hammer2 -s $MNTPT raid replace /dev/vn0 /dev/vn0 &
RPID=$!

# Write a known pattern to every 1MB boundary while resilver is running
# This exercises the dirty-range tracking
for off in $(seq 0 50); do
    dd if=/dev/urandom of=$MNTPT/dirty_${off} bs=65536 count=16 2>/dev/null
    sha256 $MNTPT/dirty_${off} >> /var/tmp/d4_ref.txt
done
sync; sync

wait $RPID
result $( [ $? -eq 0 ] && echo PASS || echo FAIL ) "D4: resilver exit"

# Fail vn1 (force reconstruction from the resilvered vn0)
hammer2 -s $MNTPT raid fail-disk /dev/vn1
vnconfig -u vn1

# All dirty files must be reconstructable via vn0 + remaining disks
for off in $(seq 0 50); do
    sha256 $MNTPT/dirty_${off} >> /var/tmp/d4_check.txt 2>&1
done
if diff -q /var/tmp/d4_ref.txt /var/tmp/d4_check.txt > /dev/null 2>&1; then
    result PASS "D4: all dirty-range files intact via reconstruction"
else
    result FAIL "D4: dirty-range files mismatch (re-resilver may not have fired)"
fi
teardown "D4"
```

**Pass criteria**:
- Resilver exits 0
- All 51 dirty files have correct checksums when read via degraded
  reconstruction through the replacement disk
- No `CHECK FAIL` in dmesg

---

## Group E: Snapshot and RAID6 Interaction

**Purpose**: Verify that HAMMER2 snapshots function correctly under RAIDZ2-native
with the unconditional COW invariant from `raidz2_snapshot_interaction.md`.

### E1: Snapshot visibility isolation

**Setup**: `setup_fresh`

**Steps**:
```sh
# Write version 1 of a file
dd if=/dev/urandom of=$MNTPT/versioned bs=65536 count=64 2>/dev/null
sha256 $MNTPT/versioned > /var/tmp/e1_v1.txt
sync; sync

# Take snapshot
hammer2 snapshot $MNTPT snap_e1

# Overwrite the file (COW — must allocate new stripe slots)
dd if=/dev/urandom of=$MNTPT/versioned bs=65536 count=64 2>/dev/null
sha256 $MNTPT/versioned > /var/tmp/e1_v2.txt
sync; sync

# The live file should have v2
sha256 $MNTPT/versioned > /var/tmp/e1_live.txt 2>&1
if diff -q /var/tmp/e1_v2.txt /var/tmp/e1_live.txt > /dev/null 2>&1; then
    result PASS "E1: live FS has post-snapshot version"
else
    result FAIL "E1: live FS has wrong version"
fi

# Mount the snapshot — must have v1
SNAPMNT=/mnt/rz2snap
mkdir -p $SNAPMNT
if mount -t hammer2 ${DEVSPEC}@snap_e1 $SNAPMNT; then
    sha256 $SNAPMNT/versioned > /var/tmp/e1_snap.txt 2>&1
    if diff -q /var/tmp/e1_v1.txt /var/tmp/e1_snap.txt > /dev/null 2>&1; then
        result PASS "E1: snapshot retains pre-overwrite version"
    else
        result FAIL "E1: snapshot has wrong version"
    fi
    umount $SNAPMNT
else
    result FAIL "E1: snapshot mount failed"
fi
teardown "E1"
```

**Pass criteria**:
- Live FS returns v2 of the file
- Snapshot returns v1 of the file
- No `CHECK FAIL` in dmesg

### E2: COW allocates new stripe slots after snapshot

**Purpose**: Confirm that after a snapshot is taken, overwriting a file
allocates new stripe slot addresses (not in-place). This is the core RAIDZ2
invariant: `modify_tid ≤ lsnap_tid` forces `newmod = 1`.

**Setup**: `setup_fresh`

**Steps**:
```sh
dd if=/dev/urandom of=$MNTPT/cowfile bs=65536 count=8 2>/dev/null
sync; sync

# Record stripe slot addresses before snapshot
hammer2 -s $MNTPT show > /var/tmp/e2_before.txt 2>&1

# Take snapshot (raises lsnap_tid floor)
hammer2 snapshot $MNTPT snap_e2

# Overwrite the file
dd if=/dev/urandom of=$MNTPT/cowfile bs=65536 count=8 2>/dev/null
sync; sync

# Record stripe slot addresses after overwrite
hammer2 -s $MNTPT show > /var/tmp/e2_after.txt 2>&1

# Extract data_off values for cowfile in both dumps
grep "data_off" /var/tmp/e2_before.txt | sort > /var/tmp/e2_addrs_before.txt
grep "data_off" /var/tmp/e2_after.txt  | sort > /var/tmp/e2_addrs_after.txt

# The addresses in use by the live file must not overlap with the old ones
# (the old ones are now pinned by the snapshot)
comm -12 /var/tmp/e2_addrs_before.txt /var/tmp/e2_addrs_after.txt \
    > /var/tmp/e2_overlap.txt
# Note: some overlap is expected for metadata blocks that were not COW'd
# For the data blocks specifically, there must be no overlap
```

**Pass criteria**:
- The `data_off` values for the file's DATA blocks are different before and
  after the overwrite (COW allocated new stripe slots)
- This can be verified by diffing the `hammer2 show` output for the relevant
  inode before vs. after the overwrite

### E3: Snapshot restore

**Setup**: `setup_fresh`

**Steps**:
```sh
dd if=/dev/urandom of=$MNTPT/restoreme bs=65536 count=32 2>/dev/null
sha256 $MNTPT/restoreme > /var/tmp/e3_orig.txt
sync; sync
hammer2 snapshot $MNTPT snap_e3

# Write new data (overwrites restoreme, allocates new stripe slots)
dd if=/dev/urandom of=$MNTPT/restoreme bs=65536 count=32 2>/dev/null
dd if=/dev/urandom of=$MNTPT/extra     bs=65536 count=16 2>/dev/null
sync; sync

# Live FS: restoreme has new content, extra exists
sha256 $MNTPT/restoreme > /var/tmp/e3_new.txt 2>&1

# Restore from snapshot
# (hammer2 snapshot-restore or pfs manipulation — exact command TBD)
hammer2 -s $MNTPT snapshot-restore snap_e3 $MNTPT
sync; sync
umount $MNTPT
mount -t hammer2 $PFSPATH $MNTPT

# After restore: restoreme must match original, extra must not exist
sha256 $MNTPT/restoreme > /var/tmp/e3_check.txt 2>&1
if diff -q /var/tmp/e3_orig.txt /var/tmp/e3_check.txt > /dev/null 2>&1; then
    result PASS "E3: file matches pre-snapshot state after restore"
else
    result FAIL "E3: file does not match pre-snapshot state"
fi
if [ ! -f $MNTPT/extra ]; then
    result PASS "E3: post-snapshot file absent after restore"
else
    result FAIL "E3: post-snapshot file still present after restore"
fi
teardown "E3"
```

**Pass criteria**:
- After restore, the file has its original content
- Files created after the snapshot do not appear after restore
- No `CHECK FAIL` in dmesg

### E4: Snapshot readable through degraded reconstruction

**Setup**: `setup_fresh`, write data, take snapshot, fail a disk

**Steps**:
```sh
dd if=/dev/urandom of=$MNTPT/snap_data bs=65536 count=64 2>/dev/null
sha256 $MNTPT/snap_data > /var/tmp/e4_ref.txt
sync; sync
hammer2 snapshot $MNTPT snap_e4

# Fail disk 2
hammer2 -s $MNTPT raid fail-disk /dev/vn2
vnconfig -u vn2

# Mount snapshot in degraded mode
SNAPMNT=/mnt/rz2snap
mkdir -p $SNAPMNT
if mount -t hammer2 \
    /dev/vn0:/dev/vn1:/dev/vn3:/dev/vn4:/dev/vn5@snap_e4 $SNAPMNT \
    2>/dev/null; then
    sha256 $SNAPMNT/snap_data > /var/tmp/e4_check.txt 2>&1
    if diff -q /var/tmp/e4_ref.txt /var/tmp/e4_check.txt > /dev/null 2>&1; then
        result PASS "E4: snapshot data correct via degraded reconstruction"
    else
        result FAIL "E4: snapshot data mismatch (degraded)"
    fi
    umount $SNAPMNT
else
    result FAIL "E4: snapshot mount failed in degraded mode"
fi
teardown "E4"
```

**Pass criteria**:
- Snapshot mounts in degraded mode (5 of 6 disks)
- Snapshot data matches reference checksum via reconstruction
- No `CHECK FAIL` in dmesg

---

## Group F: COW Invariant Verification

**Purpose**: Directly verify that the RAIDZ2-native implementation never
performs in-place overwrites for data blocks — the fundamental correctness
property that enables RMW-free parity computation.

### F1: No in-place overwrite of data blocks (freemap inspection)

**Purpose**: After overwriting a file, the old physical stripe slot must be
reclaimed by the freemap (not still in use) and the new data must be at a
different physical stripe slot.

**Setup**: `setup_fresh`

**Steps**:
```sh
# Write a 512 KB file (8 × 64 KB DIOs)
dd if=/dev/urandom of=$MNTPT/overwrite_me bs=65536 count=8 2>/dev/null
sha256 $MNTPT/overwrite_me > /var/tmp/f1_v1.txt
sync; sync

# Record physical addresses of the file's blocks
hammer2 -s $MNTPT show > /var/tmp/f1_show_v1.txt 2>&1
grep -B2 "type=DATA" /var/tmp/f1_show_v1.txt | grep "data_off" \
    | awk '{print $NF}' | sort -u > /var/tmp/f1_addrs_v1.txt

# Overwrite the file
dd if=/dev/urandom of=$MNTPT/overwrite_me bs=65536 count=8 2>/dev/null
sha256 $MNTPT/overwrite_me > /var/tmp/f1_v2.txt
sync; sync

# Record physical addresses after overwrite
hammer2 -s $MNTPT show > /var/tmp/f1_show_v2.txt 2>&1
grep -B2 "type=DATA" /var/tmp/f1_show_v2.txt | grep "data_off" \
    | awk '{print $NF}' | sort -u > /var/tmp/f1_addrs_v2.txt

# Find any address used in both v1 and v2 (indicates in-place overwrite)
comm -12 /var/tmp/f1_addrs_v1.txt /var/tmp/f1_addrs_v2.txt \
    > /var/tmp/f1_inplace.txt
if [ ! -s /var/tmp/f1_inplace.txt ]; then
    result PASS "F1: no in-place overwrite detected"
else
    result FAIL "F1: in-place overwrite detected at: $(cat /var/tmp/f1_inplace.txt)"
fi

# Checksums must still be correct
sha256 $MNTPT/overwrite_me > /var/tmp/f1_check.txt 2>&1
if diff -q /var/tmp/f1_v2.txt /var/tmp/f1_check.txt > /dev/null 2>&1; then
    result PASS "F1: overwritten file reads back correctly"
else
    result FAIL "F1: overwritten file checksum mismatch"
fi
teardown "F1"
```

**Pass criteria**:
- `/var/tmp/f1_inplace.txt` is empty (no physical address reused)
- Overwritten file reads back correctly
- No `CHECK FAIL` in dmesg

### F2: check=NONE blocks use newmod=1 in RAID6 mode

**Purpose**: Normally, `hammer2_chain_modify` sets `newmod=0` (in-place) for
blocks with `check=NONE` when `modify_tid > pfs_lsnap_tid`. In RAID6 mode,
the implementation must override this and always use `newmod=1`.

**Setup**: `setup_fresh`

**Steps**:
```sh
# Create a file with check=NONE (no integrity checksum)
hammer2 -s $MNTPT setcheck none $MNTPT
dd if=/dev/urandom of=$MNTPT/nocheck bs=65536 count=8 2>/dev/null
sha256 $MNTPT/nocheck > /var/tmp/f2_v1.txt
sync; sync

# Record addresses before overwrite
hammer2 -s $MNTPT show > /var/tmp/f2_before.txt 2>&1

# Overwrite (with no snapshot — this is the case where newmod=0 could fire
# without the RAID6 override)
dd if=/dev/urandom of=$MNTPT/nocheck bs=65536 count=8 2>/dev/null
sha256 $MNTPT/nocheck > /var/tmp/f2_v2.txt
sync; sync

# Record addresses after overwrite
hammer2 -s $MNTPT show > /var/tmp/f2_after.txt 2>&1

# Check for in-place overwrite
grep "data_off" /var/tmp/f2_before.txt | sort -u > /var/tmp/f2_a_before.txt
grep "data_off" /var/tmp/f2_after.txt  | sort -u > /var/tmp/f2_a_after.txt
comm -12 /var/tmp/f2_a_before.txt /var/tmp/f2_a_after.txt \
    > /var/tmp/f2_inplace.txt

if [ ! -s /var/tmp/f2_inplace.txt ]; then
    result PASS "F2: check=NONE block uses newmod=1 in RAID6 mode"
else
    result FAIL "F2: check=NONE block used in-place overwrite in RAID6 mode"
fi

# If in-place overwrite occurred, verify parity is still consistent
# by failing a disk and checking reconstruction
if [ -s /var/tmp/f2_inplace.txt ]; then
    hammer2 -s $MNTPT raid fail-disk /dev/vn3
    sha256 $MNTPT/nocheck > /var/tmp/f2_rec.txt 2>&1
    if diff -q /var/tmp/f2_v2.txt /var/tmp/f2_rec.txt > /dev/null 2>&1; then
        result PASS "F2: data correct even with in-place (parity updated)"
    else
        result FAIL "F2: data corrupted by in-place overwrite (parity not updated)"
    fi
fi
teardown "F2"
```

**Pass criteria**:
- No in-place overwrite for check=NONE data blocks in RAID6 mode
- If the implementation does allow in-place overwrite (regression), the parity
  must still be updated correctly (fallback to RMW) — this would be a
  performance regression but not a data-loss bug

---

## Group G: Auto-Fail and Degraded Mount

**Purpose**: Verify the auto-fail state machine and degraded mount behavior.

### G1: EIO triggers auto-fail and persists across remount

See B8 above for the EIO-injection approach. For the swap-backed vn case,
this test exercises the manual-fail path as a proxy.

**Steps** (proxy test on swap-backed vn):
```sh
setup_fresh
write_ref_data "g1"
dmesg -c > /dev/null 2>&1

# Manual fail (same state machine as auto-fail on EIO)
hammer2 -s $MNTPT raid fail-disk /dev/vn3
if dmesg | grep -q "CHECK FAIL\|panic"; then
    result FAIL "G1: CHECK FAIL or panic after fail-disk"
else
    result PASS "G1: no CHECK FAIL after fail-disk"
fi

# Verify fail state is persisted: unmount, remount without vn3
umount $MNTPT
if mount -t hammer2 \
    /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn4:/dev/vn5@RZ2TEST $MNTPT \
    2>/dev/null; then
    verify_ref "G1: data readable after degraded remount (fail state persisted)" "g1"
    umount $MNTPT
else
    result FAIL "G1: degraded remount failed (fail state not persisted?)"
fi
teardown "G1"
```

**Pass criteria**:
- `fail-disk` succeeds, no CHECK FAIL
- Degraded remount (5-disk) succeeds
- Data is still readable via reconstruction
- `raid status` after degraded remount shows disk 3 as FAILED

### G2: Degraded mount — one disk absent (never configured)

**Steps**:
```sh
setup_fresh
write_ref_data "g2"
sync; sync
umount $MNTPT

# Detach vn4 entirely
vnconfig -u vn4

# Mount without vn4 in the device list
if mount -t hammer2 \
    /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3:/dev/vn5@RZ2TEST $MNTPT \
    2>/dev/null; then
    result PASS "G2: degraded mount with one absent disk"
    verify_ref "G2: data readable with one absent disk" "g2"
    umount $MNTPT
else
    result FAIL "G2: degraded mount failed with one absent disk"
fi

# Restore
vnconfig -S 1073741824 vn4
teardown "G2"
```

**Pass criteria**:
- Mount succeeds with 5 disks
- Data readable via reconstruction (RAID6 single-column reconstruction)
- No `CHECK FAIL`

### G3: Degraded mount — two disks absent

**Steps**:
```sh
setup_fresh
write_ref_data "g3"
sync; sync
umount $MNTPT

# Detach vn1 and vn4
vnconfig -u vn1
vnconfig -u vn4

# Mount without vn1 and vn4
if mount -t hammer2 \
    /dev/vn0:/dev/vn2:/dev/vn3:/dev/vn5@RZ2TEST $MNTPT \
    2>/dev/null; then
    result PASS "G3: degraded mount with two absent disks"
    verify_ref "G3: data readable with two absent disks" "g3"
    umount $MNTPT
else
    result FAIL "G3: degraded mount failed with two absent disks"
fi

# Restore
vnconfig -S 1073741824 vn1
vnconfig -S 1073741824 vn4
teardown "G3"
```

**Pass criteria**:
- Mount succeeds with 4 disks
- Data readable via dual reconstruction (`dual_recov`)
- No `CHECK FAIL`

---

## Group H: All 15 Dual-Failure Combinations (Automation)

**Purpose**: Exhaustively verify all 15 pair combinations, heal each, and
confirm the array returns to healthy after resilver.

The script `test_h_all_combos.sh` iterates all pairs and for each:
1. `setup_fresh`, `write_ref_data`
2. Fail both disks
3. `verify_ref` (dual reconstruction)
4. Attach two fresh replacements, resilver both (sequential)
5. `verify_ref` again (healthy, post-resilver)
6. `teardown`

```sh
#!/bin/sh
# test_h_all_combos.sh

. tests/raidz2native/common.sh

PAIRS="0,1 0,2 0,3 0,4 0,5 1,2 1,3 1,4 1,5 2,3 2,4 2,5 3,4 3,5 4,5"
N=0
for pair in $PAIRS; do
    N=$((N + 1))
    A=$(echo $pair | cut -d, -f1)
    B=$(echo $pair | cut -d, -f2)
    echo "--- H$(printf '%02d' $N): fail vn${A}+vn${B} ---"

    setup_fresh
    write_ref_data "h${N}"

    hammer2 -s $MNTPT raid fail-disk /dev/vn${A}
    hammer2 -s $MNTPT raid fail-disk /dev/vn${B}
    vnconfig -u vn${A} 2>/dev/null || true
    vnconfig -u vn${B} 2>/dev/null || true
    verify_ref "H$(printf '%02d' $N): dual-fail read vn${A}+vn${B}" "h${N}"

    # Resilver both disks
    vnconfig -S 1073741824 vn${A}
    vnconfig -S 1073741824 vn${B}
    hammer2 -s $MNTPT raid replace /dev/vn${A} /dev/vn${A}
    hammer2 -s $MNTPT raid replace /dev/vn${B} /dev/vn${B}
    verify_ref "H$(printf '%02d' $N): post-resilver read vn${A}+vn${B}" "h${N}"

    teardown "H$(printf '%02d' $N)"
done

summary
```

**Pass criteria**:
- All 15 pairs: degraded read PASS
- All 15 pairs: post-resilver read PASS
- Total: 30/30 PASS
- No `CHECK FAIL` in dmesg at any point

---

## Group I: Unclean Unmount

**Purpose**: Verify that HAMMER2's COW TXG invariant ensures the on-disk state
is always a consistent snapshot, even after an abrupt shutdown.

### I1: Data written before last sync survives unclean unmount

**Setup**: `setup_fresh`

**Steps**:
```sh
# Write data and sync (committed TXG)
dd if=/dev/urandom of=$MNTPT/pre_sync bs=65536 count=64 2>/dev/null
sha256 $MNTPT/pre_sync > /var/tmp/i1_ref.txt
sync; sync

# Write more data WITHOUT sync (uncommitted, may be lost)
dd if=/dev/urandom of=$MNTPT/post_sync bs=65536 count=32 2>/dev/null
# Do NOT sync here

# Force unmount to simulate power loss / panic
umount -f $MNTPT 2>/dev/null || true
for i in 0 1 2 3 4 5; do
    vnconfig -u vn$i 2>/dev/null || true
done

# Reconnect the swap devices (data persists in swap)
for i in 0 1 2 3 4 5; do
    vnconfig -S 1073741824 vn$i   # NEW devices — old data is lost for swap-vn
done
```

**Note on swap-backed vn devices**: Swap-backed vn devices do not persist
across a `vnconfig -u` + `vnconfig -S` cycle — the data is in swap and is
released when the device is unconfigured. This means I1 cannot be tested as
described using swap-backed devices alone.

**Alternative for swap-backed vn (test mount-time consistency only)**:
```sh
# Instead of a real power-loss simulation, test that umount -f leaves
# the filesystem in a mountable state. The key invariant: the last
# committed TXG (before the forced unmount) is intact.

setup_fresh
dd if=/dev/urandom of=$MNTPT/committed bs=65536 count=32 2>/dev/null
sha256 $MNTPT/committed > /var/tmp/i1_ref.txt
sync; sync  # Ensure this TXG is committed

# Force unmount (simulates abrupt process death, not power loss)
umount -f $MNTPT 2>/dev/null || umount $MNTPT

# Reconnect swap devices (same vnconfig -S, data still in swap because
# the devices were not unconfigured — only the filesystem was unmounted)
mount -t hammer2 $PFSPATH $MNTPT
sha256 $MNTPT/committed > /var/tmp/i1_check.txt 2>&1
if diff -q /var/tmp/i1_ref.txt /var/tmp/i1_check.txt > /dev/null 2>&1; then
    result PASS "I1: synced data intact after forced unmount"
else
    result FAIL "I1: synced data lost after forced unmount"
fi
# Also verify no bad-magic or CHECK FAIL errors
if dmesg | grep -qE "bad magic|CHECK FAIL|panic"; then
    result FAIL "I1: errors in dmesg after forced unmount + remount"
else
    result PASS "I1: no errors after forced unmount + remount"
fi
teardown "I1"
```

**For file-backed vn or physical hardware**, the full power-loss simulation:
```sh
# Write committed data, sync, write uncommitted data, kill power
# (simulate with: sysctl kern.panic=1 or similar)
# On remount: committed data must be intact; uncommitted data may be absent
```

**Pass criteria**:
- Filesystem mounts cleanly after forced unmount (no journal replay errors)
- Data written before `sync` is intact
- No `CHECK FAIL` in dmesg

### I2: Degraded write + unclean unmount — no silent corruption

**Steps**:
```sh
setup_fresh
write_ref_data "i2"

# Fail a disk, write in degraded mode
hammer2 -s $MNTPT raid fail-disk /dev/vn2
dd if=/dev/urandom of=$MNTPT/degraded_write bs=65536 count=64 2>/dev/null
sha256 $MNTPT/degraded_write > /var/tmp/i2_dw.txt
sync; sync

# Force unmount without further sync
umount -f $MNTPT 2>/dev/null || umount $MNTPT

# Remount in degraded mode
if mount -t hammer2 \
    /dev/vn0:/dev/vn1:/dev/vn3:/dev/vn4:/dev/vn5@RZ2TEST $MNTPT \
    2>/dev/null; then
    result PASS "I2: degraded remount after forced unmount"
    verify_ref "I2: pre-fail reference data intact" "i2"
    sha256 $MNTPT/degraded_write > /var/tmp/i2_dw_check.txt 2>&1
    if diff -q /var/tmp/i2_dw.txt /var/tmp/i2_dw_check.txt > /dev/null 2>&1; then
        result PASS "I2: degraded-written data intact after forced unmount"
    else
        result FAIL "I2: degraded-written data lost/corrupted (was it committed?)"
    fi
    umount $MNTPT
else
    result FAIL "I2: degraded remount failed after forced unmount"
fi

if dmesg | grep -qE "CHECK FAIL|bad magic|panic"; then
    result FAIL "I2: errors in dmesg"
else
    result PASS "I2: no errors in dmesg"
fi
teardown "I2"
```

**Pass criteria**:
- Filesystem mounts cleanly in degraded mode after forced unmount
- Data committed before `sync` is intact (no silent corruption)
- No `CHECK FAIL` or bad-magic in dmesg

---

## Pass/Fail Criteria Reference

The following table summarizes the global pass/fail criteria that apply across
all groups.

| Criterion | PASS | FAIL |
|-----------|------|------|
| File checksum | `sha256` output matches reference | Any mismatch |
| dmesg CHECK FAIL | Zero occurrences | One or more occurrences |
| dmesg bad magic | Zero occurrences | One or more occurrences |
| Kernel panic | Zero occurrences | Any panic |
| Mount success | Command exits 0 | Non-zero exit |
| Resilver exit | Command exits 0 | Non-zero exit |
| fail-disk exit | Command exits 0 | Non-zero exit |
| Deadlock | All operations complete within timeout | Any operation hangs |
| In-place overwrite | Zero stripe slots reused across overwrite | Any reuse |
| `copyid` range | All DATA blockrefs have `copyid` in `[0, ndisks-1]` | Out-of-range |
| `data_off` alignment | Address portion 64 KB-aligned for full-DIO blocks | Misaligned |
| Auto-fail persistence | Failed disk shows FAILED after remount | ONLINE after remount |

### Timeout Policy

All subtests must complete within the following limits:

| Operation | Timeout |
|-----------|---------|
| `write_ref_data` (6 MB) | 30 seconds |
| `verify_ref` (checksum read) | 60 seconds |
| `resilver` (50 MB) | 300 seconds |
| Full test group | 600 seconds |

If any operation exceeds its timeout, the subtest is marked FAIL with the
annotation `(TIMEOUT)`.

---

## Expected Test Counts

| Group | Subtests | Expected PASS (all green) |
|-------|----------|--------------------------|
| A: Basic read/write | 4 | 4/4 |
| B: Single disk failure | 9 | 9/9 |
| C: Dual disk failure | 15 | 15/15 |
| D: Resilver | 6 | 6/6 |
| E: Snapshot | 6 | 6/6 |
| F: COW invariant | 3 | 3/3 |
| G: Auto-fail + degraded mount | 5 | 5/5 |
| H: All dual-fail combinations | 30 | 30/30 |
| I: Unclean unmount | 4 | 4/4 |
| **Total** | **82** | **82/82** |

---

## Differences from Existing RAID6 Overlay Tests

The existing test suite in `tests/mdraid/` (test_b through test_l, test_6disk,
test_auto_fail) targets the RAID6 overlay (v3 format) and runs on **file-backed
vn devices**. The RAIDZ2-native suite differs in the following ways:

| Aspect | Existing RAID6 overlay | RAIDZ2-native |
|--------|------------------------|---------------|
| vn device type | File-backed (`vnconfig vn0 disk0.img`) | Swap-backed (`vnconfig -S 1073741824 vn0`) |
| UFS layer | Present (Fix 13 + Fix 15 required) | Absent (no Fix 13/15 needed) |
| Volume version | 3 | 4 |
| `newfs_hammer2` | `-R 6` → v3 | `-R 6` → v4 (requires implementation) |
| `blockref.data_off` | Logical HAMMER2 address | Physical column offset |
| `blockref.copyid` | 255 (unused) | Physical disk index 0–5 |
| Write path | RMW delta parity | P/Q from new data only (no reads) |
| In-place overwrite | Possible (check=NONE, no snapshot) | Disabled (`newmod = 1` always) |
| `hammer2_raid6_map` | Required for every I/O | Deleted (address is direct) |
| A3 blockref check | Would show copyid=255 | Must show copyid in [0,5] |
| F1/F2 COW checks | Would sometimes FAIL (in-place allowed) | Must always PASS |

The C-group dual-failure tests and D-group resilver tests are structurally
identical to the existing `test_all_fail_combos.sh` and `test_d.sh`
respectively; they are reproduced here because the device setup changes
(swap-backed) and the verification includes the new blockref encoding checks.

---

## Running the Suite

```sh
# On the DragonFlyBSD VM at 192.168.25.66:
kldstat -q -m hammer2 || kldload hammer2
cd /var/tmp
# Copy test files
scp -r tests/raidz2native/ root@192.168.25.66:/var/tmp/

# Run all groups
ssh root@192.168.25.66 'sh /var/tmp/raidz2native/run_all.sh'
```

`run_all.sh`:
```sh
#!/bin/sh
PASS=0; FAIL=0
for t in /var/tmp/raidz2native/test_[a-i]*.sh; do
    echo "=== $(basename $t) ==="
    if sh "$t"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
    echo ""
done
echo "=== TOTAL: $PASS groups passed, $FAIL groups failed ==="
[ $FAIL -eq 0 ]
```

The full suite is expected to complete in under 30 minutes on the test VM.
Groups C and H are the longest due to iterating all 15 dual-failure
combinations with a fresh format per combination.
