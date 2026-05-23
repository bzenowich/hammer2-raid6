# Outstanding work

Phase 1 of `docs/newplan.md` is content-complete on `v4-rebuild` (see
`phase1_changelog.md`).  Remaining work is organized by phase.

## Phase 2 exit gate (must close before Phase 3)

The implementation side of Phase 2 is in (COW write path, hybrid
metadata zone, multi-disk volhdr quorum, blockref-walk resilver,
tests/v4 ported to virtio-blk).  The newplan §7 Phase 2 *exit
criteria* are not yet all met:

1. **EIO injection sysctl** — `debug.hammer2.inject_eio_disk`
   per-disk bitmask, plumbed into the `breadnx`-fronted read paths
   (`read_degraded`, `metadata_mirror_read`, putblk write).  Needed
   so a surviving-column failure during reconstruction is testable
   without yanking a virtual cable.
2. **`tests/v4/test_e_eio_inject.sh`** — drives the sysctl above to
   verify surviving-column EIO during reconstruction returns a clean
   error, does not panic.
3. **Snapshot-during-degraded test** — extend `test_f_cow_invariant.sh`
   or add a new group: snapshot under healthy → fail a disk → modify
   live tree → read snapshot → verify snapshot content unchanged.
   Current F3 only covers healthy-mode snapshot COW.
4. **Bulkfree-after-crash check** — extend `test_i_unclean.sh` to run
   `hammer2 bulkfree` after the forced-umount recovery and verify it
   reclaims orphan stripes (newplan Phase 2 exit: "bulkfree reclaims
   orphaned stripes").
5. *(Stretch)* NDISKS=4..10 matrix test for ndata edges.
6. *(Stretch)* Cull dead pre-v4 scripts under `tests/other/`.

## Phase 3 — real hardware bring-up

Blocked on Phase 2 exit + a physical target machine (spec is in
host-local agent memory, not the repo).  Items from `physical_disk_tasks.md`
not yet implemented:

- SMART → auto-fail (item 8).
- I/O timeout handling so a hung disk doesn't block the array (item 9).
- TRIM/discard for freed stripes (item 10).
- Always-on TXG-commit cache flush (deferred from Group C per newplan §6).
- Pull-the-cable test on physical SATA.

## Phase 4 — upstream prep

Blocked on Phase 3.  Per newplan §7 Phase 4:

- Rebase against current DragonFlyBSD `master`; convert `local_*` copies
  to clean per-file diffs.
- Man-page entries for `hammer2 raid {status,fail-disk,replace}`.
- **Performance baseline matrix.**  The `tests/perf/` scaffold (commit
  `aaddef6`) is the infrastructure.  Numbers are only meaningful on
  Phase 3 hardware — qcow2-backed VM disk perf is dominated by host SSD
  characteristics, not RAID6 algorithmic costs.  Calibrate the
  thresholds in `tests/perf/README.md` against the first physical run.
- Crash-safety statement (COW + TXG-commit atomicity, no WIB needed,
  failure modes enumerated).
- Draft `docs/RAID6_MERGE_REQUEST.md` v2.

## Phase 5 — optional follow-ups (post-v1)

Listed in newplan §7 Phase 5; non-blocking.

## Already closed (for reference)

- Upstream-grade Makefile/conf-files update so `hammer2_raid6.c` lands
  in the kernel build path — closed by commit `8d1c551`.
- Userspace `mkfs_hammer2.h` `RaidType`/`Ndisks` gap — same commit.
- `tests/perf/` scaffold (fio harness, JSON ingest, sysctl/dmesg
  snapshots) — commit `aaddef6`.  Calibration belongs to Phase 4.
