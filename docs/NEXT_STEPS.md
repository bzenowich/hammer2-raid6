# RAID6 Test Status — 2026-03-03

## Completed 2026-03-03

### Degraded Mount Support
Implemented mounting with 1-2 disks absent (physically removed / vnconfig -u):
- `hammer2_init_devvp()`: dummy vnode placeholder when device node doesn't exist
- `hammer2_open_devvp()`: tolerate VOP_OPEN failures for multi-device mounts
- `hammer2_init_volumes()`: tolerate volume header read failures, assign absent entries to empty volume slots, fallback rootvoldevvp for absent root disk
- `hammer2_verify_volumes_3()`: allow `nvolumes >= ndisks-2` for degraded RAID6
- `hammer2_vfsops.c`: mark absent disks as failed at mount time, NULL-safe logging
- `hammer2_flush.c`: write volume headers to ALL open disks (not just first), with per-disk volu_id fixup and CRC recalculation
- `hammer2_ioctl.c`: NULL dev guards for absent volumes

### Bug Fixed: volu_id Overwrite in Multi-Disk Volume Header Write
The multi-disk volume header write was copying `hmp->volsync` (with `volu_id=0` from root volume) to all disks. After mount→sync→unmount, all disks had `volu_id=0`, causing "volume id 0 already initialized" on remount. Fixed by setting the correct `volu_id` per disk and recalculating SECT0 + volume header CRCs.

### Test Results (2026-03-03)
| Test | Result | Notes |
|------|--------|-------|
| Test H (mount missing) | **6/6 PASS** | Was 3/4 — now all 3 scenarios pass |
| Combo test | **32/32 PASS** | No regressions |
| Test L (snapshot/compression) | **18/18 PASS** | No regressions |
| Test I (write during resilver) | 1/3 PASS | Pre-existing resilver bug |
| Test J (unclean unmount) | Panic | Indirect block CHECK FAIL in degraded flush |

## Remaining Issues

### Issue 1: CHECK FAIL on Indirect Blocks During Degraded Flush (Test J panic)
**Symptom**: After `fail-disk` + writing large files (>= 8MB, enough to require indirect blocks) + sync, the flush reads an indirect block via degraded reconstruction and gets a CRC mismatch. This cascades into:
```
chain 000000000900800c.02 (indirect) meth=30 CHECK FAIL
CHILD ERROR DURING FLUSH LOCK
panic: assertion "parent->error == 0" failed in hammer2_chain_create at hammer2_chain.c:3320
```
**Why combo test passes**: Combo test writes only 1×64KB files — no indirect blocks needed. Test J writes 128×64KB (8MB), which requires indirect block allocation.
**Root cause hypothesis**: During degraded flush, the filesystem reads back an indirect block for CRC verification. The degraded reconstruction (from P/Q + surviving data) returns incorrect data for indirect blocks. May be related to the block's physical location landing on the failed disk and the reconstruction path not handling metadata blocks correctly.
**Secondary issue**: Even if CHECK FAIL occurs, the kernel should not panic. The assertion `parent->error == 0` in `hammer2_chain_create` (hammer2_chain.c:3320) is too aggressive — it should propagate the error instead of asserting.

### Issue 2: Data Corruption During Concurrent Resilver Writes (Test I)
**Symptom**: When writing new files concurrently with an ongoing resilver, some files have CRC mismatches after resilver completes. On remount, the super-root inode itself fails CRC validation ("error Check Error reading super-root"), preventing mount.
**Test I result**: 1/3 PASS (resilver completes, but data verification and remount both fail).
**Relation to sequential resilver**: Memory notes `test_sequential_resilver.sh` had 11/20 pass — resilver has known bugs independent of the degraded mount work.

### Issue 3: Test G (h2parity_fix verification)
Needs `h2parity_fix` compiled on VM. Not yet attempted.

## Completed 2026-03-01

### Test Scripts Created (4 new scripts)
- `tests/mdraid/test_all_fail_combos.sh` — 4-disk, all 4 single + 6 dual-fail read + 6 dual-fail write = 16 sub-tests
- `tests/mdraid/test_sequential_resilver.sh` — sequential dual-disk resilver (Scenario A: fail 2/resilver both; Scenario B: serial singles)
- `tests/mdraid/test_5disk.sh` — 5-disk array (3 data + P + Q): healthy, single, dual, degraded write, resilver
- `tests/mdraid/test_6disk.sh` — 6-disk array (4 data + P + Q): same structure

### Bugs Found and Fixed

**Bug 1: Stale disk images from `dd if=/dev/zero` / bare `truncate`**
- `dd if=/dev/zero of=existing_file` overwrites in-place, causing vn device partition table re-read noise
- `truncate -s SIZE` on an existing same-size file does NOTHING — old data (including degraded state) survives
- Fix: all scripts now use `rm -f && truncate` to create fresh zero sparse files
- This also caused a `delete base` panic on reboot (stale `disk_state[]=FAILED` from previous run)

**Bug 2: Sync error spam from flush code (`hammer2_flush.c`)**
- `hammer2_flush_core()` at line 1468 iterates ALL devices in `devvpl` and calls `VOP_FSYNC` on each
- After `fail-disk` does `VOP_CLOSE`, the devvp is closed but still in the list
- `VOP_FSYNC` on a closed devvp returns ENXIO, producing 6 "sync error fsync=6" messages per flush
- Fix: skip devices where `e->open == 0` in the fsync loop
- **Fix is built and installed on VM (`/boot/kernel/hammer2.ko`) — needs reboot to take effect**

**Bug 3 (not a bug): vn0+vn1 dual-fail write hang**
- Originally appeared to be a kernel bug (sha256 stuck in `getblk`)
- Actually caused by stale disk state from in-place `dd if=/dev/zero` overwrite
- With fresh disk images (`rm -f && truncate`), vn0+vn1 passes cleanly (verified)

### Test Script Improvements
- Per-operation timeouts in `test_all_fail_combos.sh` (background sha256 + deadline)
- All scripts use `rm -f && truncate` for disk images

## Next Steps

### High Priority
1. **Investigate indirect block CHECK FAIL** (Issue 1) — reproduce with smaller test, add debug kprintfs to trace degraded reconstruction of indirect blocks during flush
2. **Investigate resilver data corruption** (Issue 2) — run `test_sequential_resilver.sh`, compare with concurrent resilver to isolate race condition
3. **Soften assertion in hammer2_chain_create** — `parent->error == 0` at hammer2_chain.c:3320 should propagate error, not panic

### Medium Priority
4. **Test G**: compile `h2parity_fix` on VM and run
5. **Run `test_5disk.sh`** and **`test_6disk.sh`** (need vn4/vn5 via clone handler)
6. **Regenerate patch** from VM git repo
7. **Commit degraded mount changes** to git

## File Locations
- Fixed flush source: `src/sys/local_hammer2_flush.c` (local) → `/usr/src/sys/vfs/hammer2/hammer2_flush.c` (VM)
- Built module: `/boot/kernel/hammer2.ko` (VM) — awaiting reboot
- Test scripts: `tests/mdraid/test_*.sh` (local) and `/var/tmp/test_*.sh` (VM)
- Quick repro: `tests/other/repro_fail_hang.sh`
- dmesg capture helpers: `/var/tmp/dmesg_full.sh`, `/var/tmp/quick_test.sh` (VM)
