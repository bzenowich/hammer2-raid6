# RAID6 Test Status — 2026-03-01

## Completed This Session

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

### Immediate (after VM reboot)
1. **Verify sync error fix** — run `dmesg_full.sh` test and confirm no more "sync error" messages
2. **Check partition table error** — `vn0: reading primary partition table: error...` is from `vnconfig -u`, benign vn driver noise, may still appear (not a hammer2 issue)
3. **Run `test_all_fail_combos.sh`** — should get 16/16 pass with 0 CHECK FAILs and 0 sync errors
4. **Run `test_sequential_resilver.sh`**
5. **Run `test_5disk.sh`** and **`test_6disk.sh`**

### Volume Header Write During Degraded Mode
The flush code at `hammer2_flush.c:1492-1540` only writes the volume header to `hmp->devvp` (the first device). In RAID6:
- If disk 0 is failed, `hmp->devvp` is the failed disk's devvp — volume header write fails silently
- Volume headers should be written to all healthy disks
- Not critical for correctness (data is in parity), but should be addressed

### Remaining Work from Previous Sessions
- Mount with absent disk (Tests H/I/J scenario 3) — not yet implemented
- Test G: needs `h2parity_fix` compiled on VM
- Regenerate patch from VM git repo

## File Locations
- Fixed flush source: `src/sys/local_hammer2_flush.c` (local) → `/usr/src/sys/vfs/hammer2/hammer2_flush.c` (VM)
- Built module: `/boot/kernel/hammer2.ko` (VM) — awaiting reboot
- Test scripts: `tests/mdraid/test_*.sh` (local) and `/var/tmp/test_*.sh` (VM)
- Quick repro: `tests/other/repro_fail_hang.sh`
- dmesg capture helpers: `/var/tmp/dmesg_full.sh`, `/var/tmp/quick_test.sh` (VM)
