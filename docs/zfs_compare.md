# HAMMER2 v3 RAIDZ2-Native vs ZFS — Design Comparison and Improvement Plan

**Status (2026-05-24).**  M1 (item 6 — variable-width stripes)
shipped.  M2/M3 pending.  Item 4 absorbed into M1.  Item 5 deferred.

This document compares the v3 RAIDZ2-native HAMMER2 RAID6 design to
ZFS RAIDZ2 along the axes where ZFS made deliberate, well-considered
choices.  It enumerates concrete improvements ordered by ROI and
tracks which ones have landed.

---

## 1. Where we already match ZFS

- **COW write-only-to-new-blocks** → write hole closed without an
  intent log on the data path.  HAMMER2's chain machinery COWs at
  the block level, exactly like ZFS.
- **Merkle-tree block checksums in the parent blockref**
  (`CHECK_XXHASH64` etc.) — same model as ZFS's checksum-in-block-
  pointer scheme.
- **Synchronous P/Q ordered before volhdr commit via `VOP_FSYNC`
  drain.** Same TXG-commit-as-atomic-cut model.
- **Per-disk addressing in `bref.copyid` + `bref.data_off`** mirrors
  the ZFS DVA — no logical→physical translation layer.
- **Snapshot is a frozen PFS root** (similar to ZFS snapshot = frozen
  ubertree).

## 2. Where we diverge — biggest issues first

**A. Stripe-row column waste (architectural).** Pre-M1 each
`stripe_slot` used **1 data column + P + Q** at one phys_off.  The
remaining `ndisks − 3` data-column positions at that phys_off were
unused.  Waste fraction = `(ndisks − 3) / ndisks`:

| ndisks | waste (pre-M1) | efficiency |
|---|---|---|
| 4 | 25% | 50% (ndata/ndisks = 2/4) |
| 6 | 50% | 25% |
| 8 | 62.5% | ~37% |
| 10 | 70% | 30% |

ZFS RAIDZ2 hits `(ndisks − 2) / ndisks` because each block's stripe
is sized to the block (variable-width stripes) and multiple blocks
share rows.  **M1 — Item 6 closed this.**

**B. Resilver doesn't use the bitmap.** Iterates every stripe slot
between 0 and `stripe_num_slots`, even though the in-memory bitmap
knows which slots are live.  At 10% used capacity the resilver is
10× slower than necessary.  ZFS resilvers only live blocks via the
block-pointer tree.  **Item 1 — pending (M2).**

**C. No DTL (Dirty Time Log).** Every replace is a full resilver.
ZFS keeps a per-vdev dirty time log: only stripes written *while*
the disk was failed need resilver after replace.  A 1-hour outage on
a quiet array → minutes to resync, not hours.  Phase 1 deleted the
broken v3 dirty-range tracking and didn't replace it.  **Item 5 —
deferred.**

**D. No scrub.** ZFS scrub walks every live block, verifies the
checksum, and optionally repairs from parity.  HAMMER2 has no
scrub — bit rot accumulates silently.  `tests/v3/test_f_cow_invariant.sh`
mentions `h2stripe_check` but it's still SKIP.  **Item 3 — pending
(M3).**

**E. No TRIM/discard on row free.** ZFS issues `BLKDISCARD` on free
for thin-provisioned/SSD storage.  HAMMER2 doesn't.  **Item 2 —
pending (M2).**

**F. Synchronous P + Q writes block putblk.** `write_row` does
`getblk + bcopy + bwrite` for P then Q inline.  ZFS's ZIO pipeline
issues data + P + Q in parallel.  On rotational disks the difference
is ~3× per write.  **Item 4 — partially landed (the multi-col
`write_row` API ships in M1 6C-1, but the bwrites are still
synchronous because a prior `bawrite` attempt deadlocked the buffer
cache under virtio-blk load).  Full async needs runningbufspace
throttling first.**

## 3. Less critical / longer term (items 7–10)

These weren't in the prioritized 1-6 list but are worth tracking.

**7. ZIL-style sync log.** Every `fsync` triggers a full TXG commit;
ZFS amortizes via the intent log.  Not RAID-specific but a major
perf win.  Big project.

**8. Snapshot via birth-txg.** ZFS uses `birth_txg` on each
blockptr; a block is alive in snap S iff `birth_txg ≤ S.txg`.
Constant-space refcount regardless of snap count.  HAMMER2 uses
freemap refcounts + chain refs — works, but worse asymptotic with
many snaps.  Big refactor; defer.

**9. Metaslab / space-map allocator.** ZFS log-structured space
maps handle high fragmentation well.  Our sequential cursor on a
flat bitmap is fine at low load but will fragment under churn.
Worth revisiting only after observed fragmentation.

**10. dRAID (distributed spare).** Faster rebuild via spreading
spare capacity.  Too advanced for v1; mark for post-Phase-4.

## 4. The prioritized 1-6 plan, with status

### Item 1 — Bitmap-aware resilver — **PENDING (M2)**

**Goal.** Resilver iterates only allocated stripe slots, not all
0..stripe_num_slots.

**Design.** `hammer2_io_raid6_resilver` (`local_hammer2_io.c:1604`)
Phase B loops `slot = 0..num_stripes − 1`.  Change to test
`hmp->stripe_bitmap[slot/8] & (1 << slot%8)` before issuing reads.
Free slots: skip (their on-disk content is don't-care; P/Q computed
over zeros is also zeros).  Also skip slots whose `data_disk_idx`
isn't `failed_disk_idx` AND parity disks aren't `failed_disk_idx` —
that slot doesn't touch the failed disk.

**Files.** `local_hammer2_io.c` Phase B loop (~50 LOC).  No header
or format change.

**Tests.** Extend Group D: fill 10% of array → fail disk → time
resilver.  Compare to full-iteration baseline (sysctl
`vfs.hammer2.resilver_skip_unalloc` defaults on, off for
regression).

**Effort.** ~half day.  No blast radius outside resilver.

**Risk.** Bitmap stale wrt on-disk after crash — but the H4-deep
walker (`hammer2_raid6_rebuild_stripe_bitmap`) already rebuilds an
invalid bitmap before mount, so this is safe.

### Item 2 — TRIM on row free — **PENDING (M2)**

**Goal.** Issue `BUF_CMD_DISCARD` on data + P + Q when a slot is
freed.

**Design.** In `hammer2_raid6_stripe_free`
(`local_hammer2_ondisk.c`) after clearing the bitmap bit (when
`stripe_row_refcount[slot]` reaches 0), for each of (data_disk,
p_disk, q_disk):

- skip if `raid_failed[disk]`
- skip if `volumes[disk].dev->devvp` doesn't advertise
  `IO_CMD_BLOCK_DISCARD` (probe once at mount, cache in
  `hmp->raid_trim_supported[ndisks]`)
- issue discard buf — DragonFly uses `vn_bdev_discard` / similar
  (verify the right primitive)

Gate behind sysctl `vfs.hammer2.trim_on_free` (default OFF until
tested on real SSD; vbd substrate doesn't care).

**Files.** `local_hammer2_ondisk.c` (stripe_free row-free path),
`local_hammer2_vfsops.c` (mount-time probe), `local_hammer2.h`
(`raid_trim_supported` field).

**Tests.** No vbd TRIM observability in qemu — defer real
verification to Phase 3 hardware.  Add a unit test that just checks
the sysctl gate and no panic on free.

**Effort.** ~1 day.  Probing DFly's discard API is the main
unknown.

### Item 3 — Scrub command — **PENDING (M3)**

**Goal.** `hammer2 raid scrub <mnt>` walks every live blockref,
verifies its CHECK code, repairs from parity on mismatch.

**Design.**

1. **Walker (kernel).** Reuse `hammer2_chain_scan` machinery
   (already used by `hammer2_raid6_rebuild_stripe_bitmap` /
   `hammer2_raid6_walk_chain`).  New kernel function
   `hammer2_raid6_scrub(hmp)`.  For each DATA/DIRENT bref:
   `hammer2_io_getblk` → verify `bref.check.*` against block
   content.  On mismatch: invoke `hammer2_io_raid6_read_degraded`
   against the failed column, compare to parity-reconstructed
   value, write back the corrected version if parity wins.

2. **ioctl + userspace cmd.** New `HAMMER2IOC_RAID_SCRUB` ioctl,
   blocking + progress fields (mirror `resilver_status` struct).
   `cmd_raid scrub` calls it.

For metadata (mirrored, N-way): on mismatch, fall back to a sibling
that does match (already implicit via `metadata_mirror_read`).

**Files.** `local_hammer2_raid6.c` (new scrub function),
`local_hammer2_ioctl.{c,h}` (ioctl), `local_cmd_raid.c` (cmd),
`tests/v3/test_k_scrub.sh` (new).

**Tests.** New group K: write data, deliberately corrupt one disk's
column (write garbage to a known phys_off via `dd`), scrub, verify
`CHECK FAIL` count = 0 after, verify data still readable.

**Effort.** ~3–4 days.  Walker is the bulk; reuse rebuild_stripe_bitmap's
recursion.

**Risk.** Concurrent writes during scrub need to skip in-flight
chains or hold a per-pmp scrub lock.  Defer concurrent-write support
to a follow-up; v1 scrub takes the FS read-only-ish (allow writes
but only verify chains already on-disk).

### Item 4 — Parallel P/Q writes — **PARTIAL (folded into M1/6C-1)**

**Goal.** `write_row` issues `ndata` data + P + Q in parallel
instead of sequentially.

**What landed.** M1/6C-1 (`hammer2_io_raid6_write_row`) accepts
multiple columns and computes P/Q in one pass.  But the actual
P and Q `bwrite`s are still synchronous because a prior `bawrite`
attempt deadlocked the buffer cache under virtio-blk load — parity
bawrites outran completions.

**What's left.** Async P/Q gated on runningbufspace throttling.
On real disks (Phase 3 hardware) the throttling story is different
from qemu; revisit then.  Until then `bwrite` is the safe choice.

### Item 5 — DTL / Dirty Time Log — **DEFERRED**

**Goal.** After a disk is failed and later replaced, resilver only
the slots written *while* the disk was failed.

**Design sketch.**

- *At fail-disk time:* snapshot the stripe bitmap →
  `hmp->raid_failed_bitmap_snapshot[disk_idx]` (persisted in
  volhdr extension).  This is the "clean state" — anything in this
  set was already on the failed disk and is intact on the surviving
  disks.
- *During degraded ops:* new allocations (stripe_alloc) and frees
  (stripe_free) are also recorded in a per-failed-disk DTL — a
  small additional bitmap of "slots changed while disk N was down."
  Persisted incrementally via voldata or a dedicated DTL zone.
- *On replace:* resilver iterates only the DTL.  Drops resilver
  time from O(used) to O(churn).
- *Crash safety:* DTL writes go through the same TXG commit as
  voldata, so a crash after fail-disk but before replace doesn't
  lose the DTL.

**Format change.** Yes — new on-disk DTL structure.  Add to
`raid_config` v3 addendum: `dtl_zone_off`, `dtl_size`, per-failed-
disk DTL extent table.

**Dependencies.** Builds on item 1 — bitmap-aware resilver becomes
"iterate over DTL", with DTL being a subset of the live bitmap.

**Status.** Deferred until user requests it — design questions
remain about DTL persistence and what to do when the DTL zone is
itself on a failed disk.  Pairs with item 1.

### Item 6 — ZFS-style variable-width stripes — **DONE (M1)**

Allocator and putblk path pack up to `ndata` DATA/DIRENT chains
into a single stripe slot (row), with P/Q computed over the union
when the row seals.  For NDISKS=4 this is a near-no-op (ndata=2,
mostly one-chain rows); for NDISKS≥6 it recovers the `(ndisks-3)/ndisks`
space previously wasted.

**Landed in four sub-phases.**

**6A — version collapse + bref simplification** (commit `540752a`).
`HAMMER2_VOL_VERSION_RAIDZ2` is the single on-disk version `= 3`.
`bref.data_off` carries `phys_off | radix` only; disk identity is
exclusively in `bref.copyid`.  The DIO cache key synthesizes
`(disk_idx<<56) | phys_off` in memory only.  Eliminated the
`v4/RAID6 = 3` vs `RAIDZ2 = 4` dual numbering.

**6B — in-memory per-row refcount** (commit `67d71ce`).
`hmp->stripe_row_refcount[]`: one byte per row, indicates how many
live DATA/DIRENT chains currently occupy a data column in that
row.  Bitmap bit is the union (set iff refcount > 0).  Not
persisted on disk — rebuilt at mount.

**6C-1 — write_row multi-col API** (commit `b6d02e9`).
`hammer2_io_raid6_write_row(hmp, phys_off, cols[], ncols, bytes)`.
Accumulates `P = XOR(cols)`, `Q = sum(gf_coeff * col)`.
Single-col wrapper preserved for the not-yet-converted callers.

**6C-2 — open-row packing** (commit `26af087`).
`hmp->open_rows[16]`: in-memory tracker of rows allocated this TXG
that haven't sealed P/Q yet.  Allocator tries
`hammer2_raid6_open_row_pack_locked` first (place chain in an
existing open row with a free data disk).  Putblk DATA path hands
the col bytes to `hammer2_raid6_open_row_add_data` instead of
firing inline `write_scratch`.  Rows seal when full
(`n_alloc == ndata` and every reserved col has data) or at TXG
flush boundary via `hammer2_raid6_seal_all_open_rows`.  Sysctl
`vfs.hammer2.raid6_pack_open_rows` (default 1) gates packing for
bisection.

**6D — walker rebuilds refcount + Group J tests** (commit `934d627`).
After crash recovery the in-memory refcount must reflect the real
per-row chain count or freeing one packed chain could clear the
bitmap bit prematurely.  The mount walker
(`hammer2_raid6_record_bref`) now bumps `stripe_row_refcount` for
each live bref; mount calls `hammer2_raid6_rebuild_row_refcount`
when the bitmap is valid (O(metadata) per mount; persistence is a
future optimization).  Group J packed-row tests: J1 verifies
deleting one chain in a packed row doesn't lose the others; J2
verifies a snapshot keeps the row pinned even after the live tree
drops every column.

**Test coverage.** Full v3 suite 46/46 PASS (A4 B6 C6 D8 E4 F4 G4
H4 I4 **J2**), no panics, no `CHECK FAIL`.

---

## 5. Milestones

| Milestone | Contents | Format break | Status |
|---|---|---|---|
| **M1 — v3 RAIDZ2-native variable-width** | item 6 (6A–6D) | yes (v2→v3) | **DONE** |
| **M2 — resilver + TRIM** | items 1, 2 | none | pending |
| **M3 — scrub** | item 3 | none | pending |
| *(deferred)* item 4 — async P/Q | needs cache throttling | none | partial |
| *(deferred)* item 5 — DTL | future v4 (DTL zone) | yes | not started |

---

## 6. Open questions

- **Item 4 async P/Q.** Re-test `bawrite` for P/Q with
  `runningbufspace` checks before each issue.  Spec out the cap
  and the wait/back-off.
- **Item 5 DTL persistence.** Inside `raid_config` addendum?
  Dedicated zone like the stripe bitmap?  How is the DTL itself
  protected when it lives on a single disk?
- **Item 6 row-refcount persistence.** O(metadata) walk at every
  mount works but is wasted I/O on healthy mounts.  Persisting the
  refcount alongside the bitmap zone would skip the walk.  Sizing
  is the concern — 1 byte per row times millions of rows on a
  multi-TB array.
- **Beyond 1–6**: items 7 (ZIL), 8 (birth-txg snapshots), 9
  (metaslab), 10 (dRAID).  All large; no plan yet.
