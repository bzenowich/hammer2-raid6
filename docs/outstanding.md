# Outstanding work

Phase 1 of `docs/newplan.md` is content-complete on `v4-rebuild` (see
`phase1_changelog.md`).  Remaining work is organized by phase.

## Phase 2 exit gate (must close before Phase 3)

The implementation side of Phase 2 is in (COW write path, hybrid
metadata zone, multi-disk volhdr quorum, blockref-walk resilver,
tests/v4 ported to virtio-blk).  Test-suite status against `v4-rebuild`
HEAD on NDISKS=4 vbd substrate:

| Group | Result | Notes |
|---|---|---|
| A | n/a (basic) | covered by smoke |
| B | n/a (single-disk fail) | covered by D |
| C | n/a (dual-disk fail) | covered by D |
| D | **8/8 PASS** | resilver basic + sequential + concurrent-write |
| E | **4/4 PASS** | EIO injection, no panic |
| F | **4/4 PASS** | COW invariant + snapshot COW healthy |
| G | **3/4 PASS** | G2 fail: disk fail-state not persisted across remount |
| H | **2/4 PASS** | H1+H2 fail: snapshot content diverges from pre-failure state |
| I | **4/4 PASS** | unclean unmount + degraded + bulkfree-after-crash |

Phase 2 exit-criterion items now closed:

1. ~~EIO injection sysctl~~ — `vfs.hammer2.inject_eio_disk_mask` shipped;
   synthesized post-bread so buf/lock state stays consistent.
2. ~~`tests/v4/test_e_eio_inject.sh`~~ — landed, 4/4 PASS.
3. ~~Snapshot-during-degraded test~~ — `test_h_snap_degraded.sh` landed
   (H1 + H2).  Test infrastructure complete; **kernel bug exposed**:
   snapshot content diverges from pre-failure state — needs fix.
4. ~~Bulkfree-after-crash check~~ — `test_i_unclean.sh` I3 landed,
   PASS.

Open Phase 2 exit gaps (must close before Phase 3):

A. **G2 — disk fail-state not persisted across remount.**  After
   `hammer2 raid fail-disk`, the disk is failed in-memory but the
   `voldata.raid_config.disk_state[]` bump is either not flushed or
   not read back on next mount.  The raid_config addendum was added
   in Group J; the fail-disk ioctl writes it via `hammer2_voldata_modify`
   but the flush path or the init_volumes load path is dropping it.

B. **H1 / H2 — snapshot content diverges in degraded mode.**
   Snapshot taken healthy → fail disk → modify live → snapshot mount
   shows the *modified* content, not the pre-failure content.  Looks
   like the snapshot's chain reads in degraded mode fall through to
   the live chain or skip COW.  Suspect interaction between snapshot
   PFS mount and the RAID6 degraded read path.

C. *(Stretch)* NDISKS=4..10 matrix test for ndata edges.
D. *(Stretch)* Cull dead pre-v4 scripts under `tests/other/`.

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
