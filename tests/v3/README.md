# v3 RAID6 integration tests

Run on the h2dev guest from `/root/hammer2-tests/v3` (`./deploy.sh tests`
syncs them):

    sh run_all.sh            # groups A..M
    sh run_all.sh B D        # selected groups

`crash_host.sh` runs on the **host** (it resets the guest):

    sh tests/v3/crash_host.sh          # CRASH_AFTER, CRASH_ROUNDS, CRASH_FAIL

Test disks are `/dev/vbd1..vbd4`; `vbd0` is the system disk.

## Groups

| Group | What it checks |
|---|---|
| A | healthy write/read, remount |
| B | each single-disk failure, read cold with the failed disk's data overwritten (it must not be read); degraded write with a disk absent; absent disk + corrupt disk (dual) |
| C | every pair of failed disks, read cold (mdadm `01raid6integ` shape) |
| D | resilver: basic, sequential, concurrent writes, bitmap skip timing, stale re-add, rebuild with a second disk failed |
| E | injected EIO: one failed + one EIO column, metadata primary EIO, EIO during resilver |
| F | COW invariant, parity correctness |
| G | degraded mount, fail state persisted, degraded write + resilver |
| H | snapshots across degraded states |
| I | forced unmount (**not** a crash test, `umount -f` still flushes), bulkfree after it |
| J | packed rows: delete siblings, bulkfree, reuse the freed slots, snapshot holds rows |
| K | scrub: clean array is not modified, corruption detected and repaired, scrub under writes |
| L | read-path self-heal, data and metadata |
| M | silent corruption in two columns (runs last: it can hang the kernel) |
| crash_host.sh | power loss under a write+sync loop: every synced file must survive, scrub clean |

## Rules for writing tests

- **Read cold.** A read made while the filesystem stays mounted after
  `fail-disk` or a corrupting `dd` is served from the buffer cache.
  Such a read passed with three of four columns destroyed. Use `remount`
  (or mount a degraded spec) before every read that is meant to exercise
  reconstruction.
- **Make the missing data unrecoverable any other way.** Overwrite a
  failed disk's data zone (`corrupt_data_zone`) so a pass cannot come
  from the stale column, and wipe a replacement with `fresh_disk` (512
  MB, through the data and metadata zones).
- **After a resilver**, check the data cold, `cksum_err == 0` on the
  rebuilt disk (a column the resilver skipped is parity-healed on read,
  so the data alone still matches) and `scrub_clean`.
- **One-sided assertions.** Every check must be able to fail. Decide what
  RAID6 guarantees (two erasures of any kind are recoverable) and assert
  that.
- **Check that the setup took effect**: `fail_disk` verifies the ioctl
  and the FAILED state.
- **dmesg**: use `kmsg` / `kmsg_clear`. Plain `dmesg` and `dmesg -c` fail
  with ENOMEM while the kernel is logging heavily, which silently hides
  messages or leaves them for the next test's CHECK FAIL check.
- A reader that can hang the kernel runs under `guarded` (a watchdog;
  `timeout(1)` is broken on DragonFly master).
- `run_all.sh` counts a group that aborts (FATAL, skip, no summary line)
  as a failure.

## Coverage against mdadm's test suite

| mdadm | here |
|---|---|
| `01raid6integ`: every single and double failure, data compared | B, C |
| recovery then `check`, `mismatch_cnt == 0` | `check_rebuilt` (D, E3, G3) |
| `01replace`, re-add of a stale member | D5 |
| RAID6 recovery with two members missing | D6 |
| `19repair-does-not-destroy` | K1 |
| `19raid6repair` / `19raid6auto-repair` (corrupt data/P/Q) | K2, L, M (rotation puts P and Q on every disk) |
| throttled recovery with I/O during it | D3 (overlap asserted) |
| interrupted operation / unclean shutdown | crash_host.sh |
| reshape, grow, level change, bitmap, journal, DDF/IMSM | not applicable |

Not covered: real device I/O errors leading to auto-fail (needs a
host-side QEMU fault: blkdebug or device_del), and a foreign or
wrong-array member at mount time.

## Known kernel failures (2026-10-04)

These tests fail because of kernel bugs, not test problems:

- **C01–C06**: an array with two failed disks will not mount (v3 quorum
  counts failed disks' stale headers as votes).
- **B1** (disk 0): an array with disk 0 failed will not mount ("volume
  id 1 must be 0", `hammer2_ondisk.c` RAID6 header check).
- **B6, M1, E3**: reconstruction erases only the target column, so one
  failed disk plus a corrupt or EIO row-mate is not recovered.
- **M2**: two silently corrupt columns deadlock (two self-heals each hold
  one column buffer and wait in getblk for the other's); reboot after.
- **D3**: writes made during a resilver are missing on the rebuilt disk.
- **crash_host.sh**: `r != NULL` assertion in
  `hammer2_raid6_open_row_add_data` under the write+sync loop.
