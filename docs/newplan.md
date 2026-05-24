# HAMMER2 RAID6 — New Development Plan (Revised)

**Date**: 2026-05-21
**Status**: Proposed
**Supersedes**: NEXT_STEPS.md (v3 work plan); previous draft of newplan.md
**Target**: Production-grade RAID6 for HAMMER2 on real server hardware,
preserving HAMMER2's COW architecture and snapshot model. ZFS RAIDZ2 is the
reference design.

> **Numbering note (2026-05-24).** This document predates the on-disk
> format consolidation.  The dev tree briefly carried two unshipped
> version numbers — `VOL_VERSION_RAID6 = 3` (RAID6-below-HAMMER2) and
> `VOL_VERSION_RAIDZ2 = 4` (RAIDZ2-native) — as separate steps.  Neither
> ever escaped, so they were collapsed into a single
> `HAMMER2_VOL_VERSION_RAIDZ2 = 3`.  Wherever this document refers to
> "v3" as a prior RAID6-below approach and "v4" as the RAIDZ2-native
> redesign, the latter is what now ships as v3.  The git branch name
> `v4-rebuild` is kept for continuity.

---

## 1. The Goal, Stated Cleanly

A HAMMER2 RAID6 implementation that:

1. Survives any two-disk failure on real hardware without data loss or panic.
2. Eliminates the write hole structurally (no WIB band-aid), the way RAIDZ2 does.
3. Preserves HAMMER2 snapshots as first-class — `hammer2 snapshot` continues to
   be O(1), zero-copy, and crash-safe.
4. Avoids read-modify-write for parity. Writes flow at the speed of "data +
   P + Q to fresh space" with no read latency in the critical path.
5. Is upstreamable: a clean diff against DragonFlyBSD `master`, reviewable
   by Matthew Dillon, with a sane crash-safety story.

Everything else — vn test infrastructure, file-backed images, RMW delta
parity, write-intent bitmaps, sync-degraded write throttling — is either
scaffolding to harvest or noise to discard.

## 2. What Changed Since the Previous Plan

The previous draft recommended parking v4 and shipping v3. That was the
wrong call. It optimized for "ship something in 8 weeks" over "ship the
right thing." Re-evaluating against the actual goal:

- **v3 (RAID6-below-HAMMER2) cannot deliver the user's goal.** It does not
  use COW for parity, so RMW is intrinsic. It cannot eliminate the write
  hole structurally — only narrow it via synchronous writes. Snapshots
  still work, but they are unaware of the parity layer underneath. Most
  of the kernel panics and lockups encountered to date are downstream of
  RMW-induced complexity (delta parity, old_data lifecycle, sibling reads,
  pre-emptive degraded interception).

- **v3's stable test results were on vn-backed virtual disks**, which papered
  over real-hardware failure modes (genuine concurrent I/O, real cache
  flushes, real EIO returns) while introducing fake ones (UFS-backed
  runningbufspace deadlocks). A "32/32 PASS" against vn does not predict
  behavior on a real disk array.

- **v3 (RAIDZ2-native) is the right destination.** Its design eliminates
  RMW by writing every block to a freshly-allocated stripe slot, exactly
  like ZFS. The bugs that have surfaced in v4 are implementation-level
  (DIO key collisions, prefetch races, addressing edge cases), not
  architectural. They are fixable.

- **`raidz2_snapshot_interaction.md` already specifies the architecture
  cleanly.** It identified the one place HAMMER2's in-place overwrite
  contradicts the no-RMW invariant, and proposed the one-line fix
  (unconditional COW in RAID6 mode). This is the right design, not a
  workaround.

- **The write hole closes structurally under COW-native RAID6**, the same
  way it does in ZFS. No WIB needed. The TXG commit (HAMMER2 volume header
  update) is the atomic point that promotes new stripes from "tentative"
  to "live." Pre-commit crashes leave fresh stripes orphaned in free space;
  bulkfree reclaims them later. The previous plan called for a WIB — that
  was a hangover from v3 thinking and should be removed.

## 3. Why COW-Native RAID6 Is Cleaner Than RMW

The user's intuition is correct. RMW is intrinsic to fixed-stripe RAID
designs (mdraid, LVM RAID) because they overwrite data in place. The cost
chain is:

```
write D[i] → read old D[i] → read old P, Q → compute new P, Q
           → write D[i] → write P → write Q
```

Three of the four operations are reads or writes that exist solely to
maintain parity invariants. The cost grows worse under failure: degraded
RMW must reconstruct from surviving columns before each write.

ZFS RAIDZ2 sidesteps this by treating the disk array as a log-structured
allocator. New writes go to fresh space. The stripe and its P+Q are written
once, together, then never modified. There is nothing to read, nothing to
recompute. The cost chain reduces to:

```
write D[i] → write P → write Q   (three parallel writes to fresh sectors)
```

HAMMER2 has all the prerequisites:

- Blockref tree (the equivalent of ZFS's block pointers).
- COW at the chain level (already implemented).
- Atomic volume-header commit (the equivalent of ZFS's uberblock write).
- Freemap that reclaims space lazily via bulkfree (the equivalent of
  ZFS's space maps).
- Snapshots as zero-copy blockref-tree references (already implemented).

The HAMMER2 chain layer is already a COW transactional store. v4's
insight was that adding RAID6 *underneath* the chain layer (v3) gives up
that property, while adding it *at* the chain layer (encoded in
`blockref.copyid` and `blockref.data_off`) keeps it.

## 4. ZIL: Optimization, Not Requirement

ZFS's ZIL exists to make `fsync(2)` fast without waiting for a full TXG
commit. Sync writes are appended to a log; the log is short-flushed; the
caller returns. The data later flows into a normal TXG.

HAMMER2 has no equivalent today. `fsync` triggers `hammer2_vfs_sync_pmp`,
which performs a full flush. This is slow but correct.

For this plan: **ZIL is explicitly out of scope for v1.** Reasons:

1. RAID6 correctness does not depend on ZIL. The TXG commit boundary
   already provides atomicity.
2. ZIL on a RAID6 array has its own complications (every log record
   needs P+Q, defeating the latency win). A useful ZIL needs a separate
   low-latency device — out of scope for the core RAID6 work.
3. Adding ZIL is a project of its own. Mixing it with RAID6 development
   would tangle the patch series and the failure modes.

What this plan *does* require, to leave the door open for ZIL later:

- Reserve a zone slot (or a region within `HAMMER2_ZONE_SEG`) for an
  optional intent log. Document the format as TBD.
- Keep `hammer2_vfs_sync_pmp` paths free of RAID6-specific assumptions
  so a future ZIL can be slotted in without rework.

If fsync latency becomes a problem after v1 ships, ZIL becomes Phase 5.
Until then, HAMMER2's existing flush-on-sync is the answer.

## 5. The Clean Design (Revised v4)

This consolidates `raidz2_in_hammer2.md` and `raidz2_snapshot_interaction.md`
with the lessons from v4's bugs.

### 5.1 On-disk encoding

- `blockref.copyid` = physical disk index hosting the data column.
- `blockref.data_off` = physical byte offset on that disk.
- Old "logical-then-RAID6-map" addressing (v3) is gone. Physical encoding
  only.
- New on-disk format version: `HAMMER2_VOLHDR_VERSION_RAID6` (call it v4
  as already chosen). Existing v1/v2/v3 volumes remain mountable read-only;
  upgrade requires `newfs_hammer2 --raid6`.

### 5.2 Stripe model

- Fixed stripe geometry: `ndisks` columns total, of which `ndata = ndisks - 2`
  are data and 2 are P+Q. P+Q positions rotate per stripe (left-symmetric).
- Stripe unit = `HAMMER2_PBUFSIZE` (64 KB). Data column buffer = 64 KB
  per disk per stripe. Aligns with HAMMER2's existing DIO size; no
  sub-buffer parity math needed.
- Stripe bitmap zone tracks per-stripe allocation state at column
  granularity. (v4 already places this at zone 41; keep that.)

### 5.3 Allocation rules

- **DATA and DIRENT blocks** are allocated against the stripe bitmap.
  A fresh stripe slot is chosen for every write; the data column is
  written and P+Q computed across the slot's data columns (other
  columns are guaranteed zero by `hammer2_io_new`).
- **INODE, INDIRECT, FREEMAP_NODE, FREEMAP_LEAF** are allocated by the
  existing freemap, but restricted to a dedicated **metadata zone**.
  Every metadata block is written at the same byte offset on every disk
  (N-way mirror at zone offsets — no per-disk slot lookup needed). This
  is the **hybrid layout** (see §9.1): freemap radix for allocation
  bookkeeping (handles snapshots, bulkfree, growth without preset
  capacity), reserved-zone locality for HDD seek behavior and fast
  sequential metadata resilver. Much simpler than parity-protecting
  metadata; matches ZFS's `ditto blocks` philosophy with better
  locality.
- **In-place overwrite is forbidden in RAID6 mode.** The one-line guard
  in `hammer2_chain_modify` is the architectural fix. `HMNT2_EMERG`
  is denied on RAID6 arrays (return EROFS or similar).

### 5.4 Write path

Single path, no RMW branch:

```
1. hammer2_freemap_alloc returns a fresh stripe slot S, data column c.
2. Write D to (S, c). Other columns are guaranteed zero.
3. Compute P = XOR over data columns of S (= D).
   Compute Q = Reed-Solomon over data columns of S (= GF_mult(D)).
4. Write P to (S, p_col(S)). Write Q to (S, q_col(S)).
5. Update blockref.data_off = phys_off(S, c), blockref.copyid = c_disk.
6. Steps 2/4 are 3 independent disk writes — submit in parallel via
   bawrite. No fsync between them; the TXG commit is the durability
   point.
```

The function `hammer2_io_raid6_write` collapses to 3 parallel writes.
No `breadnx`. No `old_data`. No `raid6_old_data`. No delta parity. The
~700 lines of code those features required can be deleted.

### 5.5 Read path

- **Healthy read**: `blockref.copyid` identifies the disk; issue a single
  read at `data_off`. No reconstruction.
- **Degraded read (1 disk down)**: read surviving data columns + P,
  reconstruct from P.
- **Degraded read (2 disks down)**: read surviving data columns + P + Q,
  run `dual_recov`.
- The pre-emptive degraded interception (currently in `_hammer2_io_getblk`)
  remains, but no longer has to handle in-place overwrites or sub-buffer
  ops — DATA/DIRENT always reads a whole 64 KB column. Indirect blocks
  are read from the metadata mirror, not reconstructed.

### 5.6 Atomicity / write-hole closure

The TXG commit boundary closes the write hole structurally:

- During a TXG, all dirty chains write their data+P+Q to fresh stripe
  slots. None of these blocks are referenced by any blockref that is
  reachable from the volume header *yet*.
- TXG commit = volume header write to all disks. The new volume header
  installs the new blockref tree root.
- Crash before volume header write completes on a quorum of disks: the
  prior volume header (still valid on remaining disks) describes the
  prior TXG. All freshly-written stripes are orphaned in free space;
  bulkfree reclaims them on next mount.
- Crash after volume header write completes: new TXG is live.
- The volume header write itself needs care: writing it to all N disks
  is not atomic across disks. **Mitigation**: HAMMER2 already has dual
  volume header copies and a sequence number. Use the highest valid
  sequence number found across all disks at mount, falling back to the
  prior copy if the latest is partially written. This is standard
  uberblock-discovery logic from ZFS.

This is the entire write-hole closure mechanism. No WIB. No write-intent
zones. No stripe-dirty bitmap. Delete the WIB design from `write_hole.md`
or reframe it as "alternative design for non-COW RAID6, not applicable
here."

### 5.7 Snapshots

Unchanged from `raidz2_snapshot_interaction.md`. The snapshot inode
copies `pmp->pfs_iroot_blocksets[0]`, which already contains physical
stripe addresses. Snapshots share stripe slots with the live tree until
the live tree COWs them away. Bulkfree walks all roots (live + snapshots)
to compute reachable stripe slots.

### 5.8 Fragmentation

Same as ZFS RAIDZ2. Sequential writes pack stripes densely; random
overwrites create sparse stripes (one of four columns filled, rest zero).
HAMMER2's existing `bmap_data.linear` packing already groups small writes
into 64 KB DIOs, which mitigates fragmentation at the small-write end.
Long-term mitigation is a `bulkfree`-driven repack pass (Phase 5; not
required for v1).

### 5.9 Resilver

Mostly inherits the existing v4 logic, simplified:

- Walk reachable blockrefs (from live tree + snapshots).
- For each blockref where `copyid` = failed disk: reconstruct the data
  column from surviving columns + P/Q, write to replacement disk.
- Stripes with no live blockref pointing to a failed column: skip (they
  are free space; nothing to reconstruct).
- Concurrent writes during resilver: new writes land in fresh stripe
  slots whose `copyid` may or may not be on the replacement disk. The
  dirty-range mechanism is no longer needed; resilver scans by blockref
  reachability, not by physical address range. If a new blockref's
  `copyid` happens to be the replacement disk, the write goes there
  directly. Otherwise, resilver doesn't care.

This is a major simplification over v3's resilver. Dirty-range tracking,
Phase 4 re-scan, vfs_sync_pmp in mid-resilver — all unnecessary.

## 6. What Carries Over From v3

Code, harvested intact:

- GF math, `dual_recov`, `gen_syndrome` (`local_hammer2_raid6.c` algorithm
  core). 135K unit tests pass; do not retouch.
- ioctl interface (`fail-disk`, `replace`, `status`), and the disk_state
  persistence fix (Fix 11).
- `hammer2_raid6_auto_fail_disk` and `volu_id` bounds check.
- `absent_data` path for degraded mount with missing disks.
- Test harness skeleton (rewire to virtio-blk; reuse case structure).

Code, deleted:

- v3 logical→physical mapping (`hammer2_raid6_map`). v4 uses physical
  addresses directly.
- RMW delta parity (`old_data`, `raid6_old_data`, `hammer2_io_raid6_write`'s
  read path).
- Sync-bwrite-in-degraded throttling (Fixes 10 framing). The COW design
  removes the need.
- BUF_CMD_FLUSH workaround framed as "drain UFS backing." Reframe and
  retain as "issue device cache flush before TXG commit" — but always
  run it, not conditionally on raid_nfailed.
- vn.c IO_SYNC patch (`src/sys/local_vn.c`, Fix 13). Pure test-infra
  artifact.
- `hammer2_flush_vn_backing`. Replaced by an unconditional cache-flush
  call at TXG commit.

Tests, recovered:

- Combo test structure (all failure combinations × R/W). Port to virtio-blk.
- Resilver tests. Simplify per new resilver logic.
- Test J (indirect block CHECK FAIL on degraded flush): becomes a test of
  the metadata mirror, not the parity reconstruction. Expect it to pass
  trivially once metadata mirror is in place; the original failure was
  caused by reconstructing metadata through parity, which v4 no longer
  does.
- Test I (concurrent resilver writes): simplified; resilver scans by
  blockref reachability, no race window.

## 7. Phased Plan

### Phase 0 — Design closure (1 week)

Goal: lock the design before any more code.

- Finalize this document. Address every "TBD" inline.
- Reconcile against `raidz2_in_hammer2.md` and
  `raidz2_snapshot_interaction.md`. Resolve any conflict in favor of
  this plan and update those docs.
- Decide and write down: metadata mirror layout, stripe bitmap on-disk
  format, volume header sequence-number policy across N disks, resilver
  reachability traversal order.
- Catalog all in-place-overwrite paths in HAMMER2 (`hammer2_chain_modify`,
  `HMNT2_EMERG`, dedup, growfile). Document the RAID6 guard at each.
- **Target physical machine — LOCKED:**
  - Motherboard: ASRock B450M Pro4 (AM4)
  - CPU: AMD Ryzen 7 2700 (8C/16T, Zen+)
  - RAM: 32 GB DDR4 ECC
  - HBA: Supermicro AOC-USAS-L8i, 8-port SAS, PCIe x16, IT mode
  - Disks: 4× WD Red WD10JFCX, 1 TB, 2.5" SATA (5400 RPM HDD)

  HDD substrate validates §9.1 metadata-zone locality choice. SAS HBA in
  IT mode gives clean per-disk EIO and SMART pass-through.

Exit: a single design doc with no open questions; per-topic specs
landed (§13); Phase 1 punch list (`docs/tracker.md`) populated.

### Phase 1 — Clean rebase (1–2 weeks)

Goal: branch `devel` to `v4-rebuild`. Strip v3 cruft and v4 quirks. End
with a kernel that compiles and mounts a single-disk volume, with the
RAID6 code paths fully present but inert until ≥4 disks are presented.

- Branch: `git checkout -b v4-rebuild devel`.
- Delete v3 logical→physical mapping. Delete RMW path. Delete vn.c patch.
  Delete `hammer2_flush_vn_backing` (replace with always-on cache flush
  at TXG commit).
- Apply the one-line `hammer2_chain_modify` RAID6 COW guard.
- Audit `local_hammer2_io.c`: any remaining "raid_nfailed > 0 → do X"
  conditional should be re-examined. Most can go.
- Single-disk mount/unmount/read/write must work — sanity gate before
  moving on.

Exit: clean compile, single-disk mount works, code size shrinks by
several hundred lines (deletions, not additions).

### Phase 2 — RAID6 bring-up on virtio-blk (4–6 weeks)

Goal: a 4-disk RAID6 array that mounts, reads, writes, snapshots,
fails-and-replaces correctly under QEMU virtio-blk.

- Implement the COW write path end-to-end. `hammer2_io_raid6_write` =
  3 parallel `bawrite`s. No reads.
- Implement the **hybrid metadata zone** for INODE/INDIRECT/FREEMAP_*
  (see §5.3 and §9.1). Reserve a dedicated zone slot at format time;
  use the existing freemap radix to allocate within it; write each
  metadata block at the same byte offset on all N disks. Initial zone
  sizing: ~5% of per-disk LBA range, with room to extend.
- Implement the volume-header multi-disk write with sequence-number
  recovery on mount.
- Implement resilver by blockref reachability traversal. Walk live
  tree + all snapshots; for each blockref whose `copyid` is the failed
  disk, reconstruct and write to the replacement.
- Implement EIO injection (sysctl: `debug.hammer2.inject_eio_disk=N`)
  to test surviving-disk failures during reconstruction.
- Port tests to `tests/v3/` against virtio-blk. Re-baseline.

Exit: against virtio-blk, all of:
- Healthy R/W passes.
- Single-disk failure: read/write/resilver pass.
- Two-disk failure: read passes; write passes after resilver of one.
- Snapshot create + modify + restore passes across degraded states.
- EIO injection on a surviving column during reconstruction returns a
  clean error to the caller, does not panic.
- Power-cut simulation (kill QEMU during write) leaves array consistent;
  bulkfree reclaims orphaned stripes.

### Phase 3 — Real-hardware bring-up (3–4 weeks)

Goal: identical functionality on real disks.

- Stand up the target machine. DragonFlyBSD install + custom kernel.
- Reproduce `tests/v3/` against real disks. Expect surprises:
  - Real cache flushes have real latency. Measure TXG commit time.
  - NCQ/parallel writes are genuinely parallel; bawrite throughput
    should be higher than under vn or virtio-blk.
  - Disk identity stability across reboots. Use HAMMER2 volume UUID +
    `volu_id`, not device names.
- Implement the items from `physical_disk_tasks.md` that the new design
  still needs:
  - SMART monitoring → auto-fail (item 8).
  - I/O timeout handling so a hung disk does not block the array (item 9).
  - TRIM/discard for freed stripes (item 10).
- Pull-the-cable test: yank SATA cables on a live array, verify auto-fail,
  hot-replace, resilver, data verifies.

Exit: 4-disk physical RAID6 survives two-disk failure injected by
physical cable removal; data verifies after resilver. No panics across
the test suite running for 24 hours.

### Phase 4 — Upstream prep (2–3 weeks)

Goal: a patch series Dillon can review.

- Rebase against current DragonFlyBSD `master`. Stop carrying local
  `local_*` copies; convert to clean per-file diffs.
- Man-page entries for `hammer2 raid status|fail-disk|replace`.
- Performance baseline numbers: sequential R/W, random R/W, healthy vs
  single-degraded vs dual-degraded vs resilvering. Compared against a
  baseline of HAMMER2 on a single disk and on a 4-disk N-way mirror.
- Crash-safety statement: COW + TXG-commit atomicity, no WIB needed,
  failure modes enumerated.
- Draft `RAID6_MERGE_REQUEST.md` v2.

Exit: patch series posted to upstream.

### Phase 5 — Optional follow-ups (post-v1)

Not blocking. Listed for completeness so they aren't accidentally
folded back into earlier phases:

- ZIL-equivalent intent log for fast fsync.
- Periodic stripe repack to reduce sparse-stripe fragmentation.
- RAID-Z3 (triple parity) using the same framework.
- `hammer2` userland tools beyond ioctls (status daemon, monitoring).

## 8. What This Plan Says No To

- No WIB. Closed structurally by COW + TXG commit.
- No RMW path. Single write path only.
- No vn-backed test runs as authoritative. Virtio-blk is the minimum
  test substrate. vn is fine for unit-test smoke runs of algorithm
  changes, but its results never gate progress.
- No format-stable migration from v3 to v4. v3 volumes are not upgraded
  in place; they would be re-created. Since v3 was always a research
  prototype, this is acceptable.
- No ZIL in v1. Reserve the zone, defer the work.
- No emergency in-place writes on RAID6. `HMNT2_EMERG` denied.
- No half-finished resilver state machines. If resilver fails partway,
  restart from blockref traversal — idempotent, no per-stripe checkpoint.

## 9. Phase 0 Decisions

All five questions from the prior draft are resolved here. Full per-topic
specs land in dedicated docs (referenced in §13).

### 9.1 Metadata layout — hybrid (dedicated zone + freemap + N-way mirror)

Dedicated metadata zone on each disk; allocator = existing freemap radix
restricted to that zone; every metadata block written at the same byte
offset on all N disks (no per-disk slot lookup).

- **Initial sizing**: ~5% of per-disk LBA range, with room to extend.
- **HDD wins**: metadata I/O clustered in narrow LBA band; cold `find` /
  mount / metadata resilver sequential.
- **No capacity cliff**: zone extends into adjacent unused space if it
  fills; offline tool grows the zone.
- **Per-disk zone-full**: array goes read-only with a clear error;
  admin extends via offline tool. Can only happen after replace-with-
  larger-disk asymmetry; normal mirrored writes keep all N disks
  in lockstep.
- **Code reuse**: freemap radix unchanged. Snapshot reachability,
  bulkfree, dedup work as today; only physical location differs.
- **Resilver**: sequential read from surviving disk → sequential write
  to replacement. Order-of-magnitude faster than blockref-walk on HDD.

Alternatives considered and rejected for v1:
- **Alternative A — reserved region with bitmap/bump allocator**: same
  locality, but adds a new allocator and a fixed capacity ceiling.
  Hybrid wins without either downside.
- **Alternative B — freemap + N-replica flag, no dedicated zone**:
  cleanest composability, best capacity flexibility, but metadata
  scatters across the platter. Cold metadata reads and resilver become
  random I/O, costing minutes-to-hours on HDD. Target hardware (§7
  Phase 3) is HDD; this is the regret path. Reconsider if a future
  deployment is SSD-only.

Full spec: `docs/metadata_zone.md`.

### 9.2 Volume-header quorum — ZFS-style majority

On mount, scan all N disks for the highest sequence number presented
by a *majority* (⌈N/2⌉+1) of disks. That seqno is the live volume
header. If no majority, fall back to the highest seqno present on a
majority for the *prior* generation.

- Per-disk seqno is monotonically increasing; written as part of the
  TXG-commit volume-header copy.
- Partial-write detection: per-disk seqno + CRC mismatch invalidates
  that disk's copy.
- Majority threshold matches ZFS uberblock discovery.

Full spec: `docs/volhdr_quorum.md`.

### 9.3 Free-stripe selection — sequential cursor

Allocator maintains a per-pool cursor; each new stripe slot is taken
at the cursor and the cursor advances. On wrap, restart from the
first free slot. Matches ZFS metaslab allocator behavior for
sequential-write throughput on HDD.

- No first-fit or best-fit scanning.
- Cursor persists across TXG via stripe bitmap zone footer.

### 9.4 Stripe bitmap durability — persist + mount-time verify

The stripe bitmap is a TXG-flushed structure (written at zone 41 as
part of TXG commit). At mount, the bitmap is *verified* against
blockref reachability — any divergence (stripes marked live with no
referencing blockref) is reconciled in favor of the blockref walk,
and bulkfree reclaims the orphans.

- Persistence avoids full-traversal cost on every mount.
- Mount-time verify catches corruption and torn TXG commits.
- Matches ZFS space-map lazy-reconstruction policy.

Full spec: `docs/stripe_bitmap.md`.

### 9.5 DIO key encoding under physical addressing — moved to Phase 1 punch list

v4's recent fix ("Fix v3 RAIDZ2-native addressing, races, and stability
issues", commit `53cc7cd`, expanded in `9d537be`) placed v4 DATA/DIRENT
DIO tree keys above `total_size` to avoid aliasing v3 logical keys.
This must be re-derived from the design in Phase 1, not stacked on the
WIP patch.

- Move to Phase 1 punch list (§7 Phase 1, `docs/tracker.md`).
- Acceptance criterion: collision-avoidance falls out of the address
  encoding by construction, not by post-hoc offset arithmetic.

## 10. Risk Register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Real hardware reveals issues virtio-blk did not | High | Budget 50% buffer in Phase 3. Target machine identified in Phase 0. |
| Metadata zone fills before data does (HDD-heavy small-file workload) | Medium | Initial sizing ~5% per-disk LBA. Zone extensible via offline tool. Read-only fail with clear error if exhausted. |
| Stripe fragmentation hurts long-term performance | Medium | Defer to Phase 5 repack. Document the steady-state utilization for the user. |
| Upstream rejects the design | Low/Medium | Engage Dillon early in Phase 2 with the design doc, not at Phase 4 with code. |
| Snapshot semantics regress in some subtle way | Medium | Dedicated snapshot test pass in Phase 2 (`tests/v3/snapshot_*`). |
| Scope creep (ZIL, RAID-Z3, etc., pulled into v1) | High by historical pattern | This document. Phase 5 list is binding — items there do not move forward. |

## 11. Concrete First Week

1. Review and red-line this document with the user. Resolve §9 open
   questions or schedule them.
2. `git checkout -b v4-rebuild devel`. Verify it builds.
3. Inventory and identify the target physical machine.
4. Audit `local_hammer2_chain.c:1693` (the `newmod` decision). Add the
   one-line RAID6 COW guard. Verify with a small write test that COW
   triggers as expected.
5. Open a tracking issue (or `docs/tracker.md`) listing every v3-era
   workaround to be deleted in Phase 1, file by file, function by
   function. This becomes the Phase 1 punch list.

After this week, the project has a clear design, a clean branch, an
identified hardware target, and a deletion list. From there, Phases 1–4
are mechanical.

## 12. Honest Assessment

This plan throws away most of the test infrastructure investment from
the past 8 months and a meaningful fraction of the v3 kernel code.
That is a real cost. The reason to accept it: the alternative is
shipping v3 to real hardware, hitting RMW-induced races that vn never
exposed, and spending another 6 months patching them. The cleanest path
to "works on real hardware" is to build the right thing now.

ZFS got this architecture right. HAMMER2 already has the substrates
(COW, blockref tree, atomic uberblock-equivalent, freemap, snapshots)
to do the same. The v4 design was already aimed at this destination;
the bugs were implementation-level, not architectural. This plan
restarts v4 from a cleaner baseline, with the lessons from both v3
and the early v4 stumbles applied.

Estimated time to upstreamable patch series: **3–4 months** from Phase 0
start. Less than the time already spent; more than wishful thinking.

## 13. Phase 0 Design Docs (per-topic specs)

Detail bodies live in their own files so this document stays the index:

- `docs/metadata_zone.md` — hybrid metadata zone layout (§9.1).
- `docs/stripe_bitmap.md` — zone-41 stripe bitmap on-disk format (§9.4).
- `docs/volhdr_quorum.md` — volume-header seqno + majority quorum (§9.2).
- `docs/resilver_v4.md` — blockref-reachability resilver order (§5.9).
- `docs/inplace_audit.md` — catalog of in-place-overwrite paths and the
  RAID6 guard at each (§5.3, §11.4).
- `docs/tracker.md` — Phase 1 deletion punch list (§11.5).

Superseded v3-era docs that need reconciliation in Phase 0:

- `docs/raidz2_in_hammer2.md` — original v4 design proposal. Reconcile
  against this plan; flag any divergence as superseded.
- `docs/raidz2_snapshot_interaction.md` — snapshot interaction analysis.
  Folded into §5.7; verify no contradictions remain.
- `docs/write_hole.md` — v3 WIB design. Reframe as "v3-era; not
  applicable under COW + TXG-commit closure (§5.6)."
