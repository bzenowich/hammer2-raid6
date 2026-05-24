# Phase 1 changelog (v4-rebuild)

Chronological log of `v4-rebuild` Phase 1 commit groups A–K plus the
build-path closeout — what each landed and what it deferred.

**Phase 1 of newplan.md on `v4-rebuild` branch.**  Phase 0 doc commit
`181c1d9`.  Phase 1 commits land roughly bottom-up — code deletion first,
then v4-specific format/quorum/recovery layers, then test + build closeout.

> **Numbering note (2026-05-24).** "v3" in entries below = the prior
> RAID6-below-HAMMER2 path that was deleted; "v4" = the RAIDZ2-native
> design these commits built.  The dev-tree later collapsed both
> unshipped on-disk numbers into a single `HAMMER2_VOL_VERSION_RAIDZ2 = 3`,
> so anything an entry calls "v4" is what now ships as v3.  Branch
> name `v4-rebuild` is kept.

- `eb9593a` Group G interim (DIO key dispatch consolidated).
- `dc02c9a` Group F (in-place guards: F1 chain newmod version check,
  F2 EMERG alloc-fail refused on v4, F3 ioctl emerg_mode → EOPNOTSUPP,
  F4 v4 DATA/DIRENT dispatch inside `hammer2_freemap_alloc`).
- `c56afc3` Group B (deleted `hammer2_raid6_map` + v3 logical fallback
  in `hammer2_io_raid6_read_degraded`; net -169).  `read_degraded` now
  returns EIO for non-physical metadata calls; Group I replaces that
  with metadata-mirror reads.
- `00530b9` Group C (deleted `hammer2_flush_vn_backing` + caller +
  `local_vn.c`; deploy.sh no longer special-cases vn.c.  Net -1009.)
  Always-on TXG-commit cache flush deferred to later real-hardware work
  per newplan §6.
- `27c8fd8` Group A (deleted RMW delta parity + async parity thread).
  `hammer2_io_raid6_write` gone; `raid6_old_data` field gone;
  `hammer2_parity_{thread,init,uninit,drain}` and struct
  `hammer2_parity_work` + queue fields gone.  `_hammer2_io_putblk`
  calls `write_scratch` synchronously for v4 DATA/DIRENT only;
  INODE/INDIRECT no longer trigger parity.  Net +39/-681.
- `360805c` Group D (dropped RAID6 sync-bwrite branch in putblk; data
  writes uniform cluster_write/bawrite).  D2/D4 already in A; D3 kept
  raid_nfailed field.  Net +1/-14.
- `9534084` Group E (deleted resilver dirty-range tracking).
  `resilver_dirty_lo/_hi` fields, putblk update, Phase 4 re-resilver
  all gone.  Net +5/-144.
- `d9a8246` Group G-final (v4 DIO key now `(disk_idx<<56)|phys_off`).
  New macros `HAMMER2_RAID6_DISK_SHIFT/_MASK/_PHYS_MASK`.  Collision-
  avoidance structural; no `vol->offset` arithmetic in v4 hot path.
  stripe_alloc/free, dio_key Path A, write_scratch, read_degraded all
  updated.  Net +42/-60.
- `a156d10` Group H (stripe bitmap header/footer/CRC + cursor).  On-disk
  header + matching footer; bitmap_read verifies; bitmap_write bumps
  gen + recomputes CRC.  stripe_alloc uses persistent cursor.  mkfs
  writes valid initial header.  Net +280/-57.  **H4 deferred** at
  the time: blockref-walk reconstruction on bitmap_invalid.
- `9c7da6b` Group I (foundation): metadata-zone extent table.  Net +68.
- `a2b3d49` Group J (volhdr addendum + per-TXG seqno + UUID check).
  raid_config gains v4 addendum (168 bytes); reserved pad now 256.
  mkfs writes at format; flush bumps; init_volumes validates UUID +
  disk_id + ndisks; vfs_mount loads md_extents from voldata.  Net
  +111/-6.  **Deferred in J**: majority-quorum + multi-TXG rollback in
  init_volumes; pre-scan all disks for highest-seqno root voldata;
  auto-resilver on disk seqno-lag.
- `29fcbac` Group I follow-ups (I4-restrict + I5 infra + I6).  Net +190/-2.
- `e3359aa` H4: refuse RW mount when stripe_bitmap_invalid.  Safe-by-
  default close of the Group H bitmap walker gap.  Net +17.
- `38899bf` J2-full: per-disk pre-scan + majority quorum + rollback
  envelope (default 8 TXGs).  rootvoldata now reflects the highest
  rz_txg_seq; no-majority returns ENXIO.  Net +127/-3.
- `1153d91` I5-wire: putblk mirrors v4 metadata payloads; getblk
  failover via metadata_mirror_read.  Net +181/-36.
- `4e325fa` Hard freemap restriction (bmin/bmax in fiterate; v4 metadata
  bounded to md_extents[0]) + J2 rollback sysctl opt-in
  (`vfs.hammer2.j2_allow_rollback`, `vfs.hammer2.j2_rollback_max`).
  Net +68/-17.
- `c587511` H4-deep: blockref-walk reconstruction.  Walker uses
  `hammer2_chain_scan` recursively from vchain, records DATA/DIRENT
  bref slots, advances cursor.  vfs_mount runs it when
  `stripe_bitmap_invalid` is set; success clears the flag and RW mount
  proceeds.  Net +115/-9.
- `37d3808` Group K: tests/mdraid deleted; tests/raidz2native →
  tests/v3 against /dev/vbd* only.  vn paths stripped from common.sh +
  run_all.sh.  Net +33/-3834.
- `8d1c551` Build-path gaps (session 2026-05-23).  Ships
  `src/sys/local_Makefile` (adds `hammer2_raid6.c` to SRCS);
  `src/sbin/local_mkfs_hammer2.h` adds `RaidType`/`Ndisks`;
  `src/sbin/local_newfs_hammer2.c` adds `-R raid` + `-N ndisks`
  (matches `newfs_hammer2 -R 6 ...` in tests/v3); deploy.sh syncs the
  Makefile and idempotently appends the matching `optional hammer2`
  line to `/usr/src/sys/conf/files`; deploy.sh now uses the `h2dev`
  SSH alias.  `hammer2_raid6_auto_fail_disk` no longer `static`;
  prototype in `local_hammer2.h`.  Net +300/-6.  hammer2.ko 471648 →
  477024 (raid6.o actually linked).
- `aaddef6` tests/perf scaffold (session 2026-05-23).  Fio-based
  harness for {h2-1disk, v4-healthy, v4-degraded} × {seq-read-1m,
  seq-write-1m, rand-read-4k, pg-mix} × numjobs={1,4,8,16}.  Records
  pre/post `vfs.hammer2.*` sysctls and dmesg CHECK FAIL delta per run.
  `report.sh` emits markdown table with BW/IOPS/p99/% vs h2-1disk.
  fio not in DragonFly base pkg — README documents source build.
  Net +408.

**v4 v1 merge content complete (kernel side):**
- Stripe data: COW + P+Q in stripe bitmap zone (H + Group A).
- Metadata: N-way mirror in extent table persisted in voldata
  (I + J + I5-wire + I4-restrict hard).
- TXG commit: per-TXG seqno + majority quorum + sysctl rollback
  (J2-full + sysctl).
- Failure handling: bitmap_invalid auto-rebuilds via blockref walk
  (H4-deep); failed-disk metadata reads fall back to siblings.
- Tests: tests/v3 against virtio-blk; tests/perf scaffold.

All builds clean -Werror on VM (hammer2.ko 477024 bytes incl. raid6.o).
Userspace newfs_hammer2 + hammer2 utility also build clean.
