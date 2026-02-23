# HAMMER2 RAID6 Test Plan

Tests derived from analysis of the mdadm/mdraid test suite
(https://github.com/md-raid-utilities/mdadm), filtered to scenarios
that apply to our implementation.

---

## What We Skipped and Why

| mdadm category | Reason skipped |
|---|---|
| IMSM / DDF metadata formats | HAMMER2-specific on-disk format |
| RAID 0/1/4/5/10 tests | Wrong level |
| Bitmap management | HAMMER2 has no write-intent bitmap |
| Level conversion (RAID5→RAID6) | Not supported |
| Autodetect / autoassemble | HAMMER2 uses explicit device paths |
| Write-journal / write-cache | Not applicable |
| Chunk size migration | Not applicable |
| Array growth / shrink (02r6grow) | Not yet implemented |
| All layout variants (07layouts) | We implement left-symmetric only |

---

## Coverage Map

| mdadm test | Our equivalent | Status |
|---|---|---|
| `00raid6` | Basic create/mount/IO/unmount | Covered in test_d setup |
| `01raid6integ` (single fail) | `test_b.sh` | ✅ Done |
| `01raid6integ` (dual fail) | `test_c.sh` | ✅ Done |
| `01replace` | `test_d.sh` | ✅ Done |
| `19raid6check` | `verify_parity.sh` + h2parity_fix | ✅ Done |
| `19raid6repair` | `test_g_repair_no_destroy.sh` | Test E–K below |
| `19repair-does-not-destroy` | `test_g_repair_no_destroy.sh` | Test E–K below |
| `24raid456deadlock` | `test_k_concurrent_io.sh` | Test E–K below |
| `25raid456-recovery-while-reshape` | `test_i_write_during_resilver.sh` | Test E–K below |
| *(no mdadm equiv)* | `test_e_write_degraded.sh` | HAMMER2 COW specific |
| *(no mdadm equiv)* | `test_f_sequential_fail.sh` | Not in mdadm suite |
| *(no mdadm equiv)* | `test_h_mount_missing.sh` | HAMMER2 mount semantics |
| *(no mdadm equiv)* | `test_j_unclean_unmount.sh` | COW consistency |

---

## Tests E–K: Descriptions

### Test E — Write During Degraded Mode (`test_e_write_degraded.sh`)
*Derived from: mdadm `01raid6integ` extended write scenario*

Write reference data, mark a disk failed, then write NEW data while the array
is in degraded mode. Verify both old and new data are readable in degraded state,
then detach the failed disk and verify both datasets are still readable.

Tests: COW write path during degraded operation; parity thread handles new
allocations correctly when one data column is unavailable.

### Test F — Sequential Disk Failures (`test_f_sequential_fail.sh`)
*Derived from: mdadm `01r5fail` sequential failure scenario*

Fail disk 1, write additional data while in single-degraded mode, then fail
disk 2 while still single-degraded. Verify all data (pre-failure, between
failures, after second failure) is readable in dual-degraded mode with both
disks detached.

Tests: Transition from healthy → single-degraded → dual-degraded; data written
in degraded mode is recoverable after a second failure.

### Test G — Repair Does Not Destroy (`test_g_repair_no_destroy.sh`)
*Derived from: mdadm `19repair-does-not-destroy` and `19raid6repair`*

Run h2parity_fix in check mode (-n) on a freshly formatted array, after a
mount+write+unmount cycle, and after running h2parity_fix in repair mode.
All three checks must report 0 mismatches. Remount after repair and verify
data integrity.

Tests: Parity repair tool is idempotent and does not corrupt clean data or
data written by the kernel; h2parity_fix and the kernel use the same mapping.

### Test H — Mount With Missing Device (`test_h_mount_missing.sh`)
*No direct mdadm equivalent — tests HAMMER2 mount semantics*

Three scenarios:
1. Attempt mount when a device is simply not configured (vnconfig missing) —
   should fail cleanly with a useful error.
2. Mark a disk failed via `raid fail-disk`, unmount, remount with the disk
   still attached but marked failed in the on-disk header — should mount in
   degraded mode.
3. Mark a disk failed, unmount, unconfigure it (vnconfig -u), remount —
   should mount degraded using the on-disk FAILED state to skip the missing
   device.

Tests: Mount-time degraded state restoration from on-disk `disk_state[]`;
clean refusal vs. degraded mount behavior.

### Test I — Writes During Online Resilver (`test_i_write_during_resilver.sh`)
*Derived from: mdadm `25raid456-recovery-while-reshape`*

Write reference data, fail a disk, attach a fresh replacement, then start the
resilver in the background while concurrently writing new data to the mounted
filesystem. Wait for resilver to complete. Verify both the reference data and
the data written during resilver are intact. Unmount and remount to confirm
persistence.

Tests: Concurrent COW writes and resilver do not conflict; new block allocations
on the new disk during resilver are handled correctly.

### Test J — Unclean Unmount / Power-Loss Simulation (`test_j_unclean_unmount.sh`)
*Derived from: mdadm write-consistency tests; HAMMER2 COW specific*

Write data and sync (checkpoint). Write more data without an explicit sync.
Force unmount with `umount -f` to simulate an abrupt shutdown. Remount and
verify: (a) the filesystem mounts cleanly, (b) data written before the last
sync is intact, (c) no CHECK FAIL or bad-magic errors.

Tests: HAMMER2 COW ensures the on-disk state is always a consistent snapshot;
parity is never left half-written in a way that prevents remount.

### Test K — Concurrent I/O During Degraded + Resilver (`test_k_concurrent_io.sh`)
*Derived from: mdadm `24raid456deadlock`*

Run multiple parallel writers and readers while simultaneously failing a disk
and running a resilver. Verify: (a) no process hangs indefinitely (deadlock
check via timeout), (b) all writers complete successfully, (c) all data is
intact after resilver completes.

Tests: Background parity thread, degraded read path, and resilver do not
deadlock under concurrent I/O pressure; spinlock contention on raid6_parity_q
is handled correctly.

---

## Environment

All tests are self-contained and run on the DragonFlyBSD VM.

```
VM:       192.168.25.102 (root access via dfly-exec.sh)
Disks:    /var/tmp/disk0.img .. disk3.img (1GB each)
Spare:    /var/tmp/disk4.img (1GB, used for resilver tests)
Devices:  /dev/vn0 .. /dev/vn3 (configured via vnconfig)
Mount:    /mnt/test  @TEST PFS
```

### Running All Tests

```sh
for t in tests/test_[b-k]*.sh; do
    echo "=== $t ==="
    ./dfly-exec.sh "sh /var/tmp/$(basename $t)" && echo PASS || echo FAIL
done
```

Or copy all tests to the VM first:
```sh
scp tests/test_*.sh root@192.168.25.102:/var/tmp/
```
