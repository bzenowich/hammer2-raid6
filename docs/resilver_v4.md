# HAMMER2 v4 RAIDZ2-native — Resilver Order

**Status**: Phase 0 spec. Implementation in Phase 1–2.
**Cross-refs**: `newplan.md` §5.9. `metadata_zone.md`, `stripe_bitmap.md`.

---

## Purpose

Reconstruct a failed disk's contents onto a replacement disk by:

1. **Metadata zone**: sequential copy from any surviving disk (mirror).
2. **Data area**: blockref-reachability walk, reconstructing only the
   columns that the failed disk hosted for live blockrefs.

The v4 resilver is structurally simpler than v3's because the
per-stripe physical layout is encoded in `bref.copyid` + `bref.data_off`,
not in a logical-to-physical mapping that has to be re-derived per
stripe range.

---

## Pre-conditions

- Replacement disk is attached and recognized at `v4_disk_id`
  matching the failed slot.
- Array is mounted and the failed slot is marked replaced via
  `hammer2 raid replace`.
- `v4_array_uuid` of the replacement is the array's UUID (newly
  written, or zeroed for a blank disk).

---

## Phase A — Metadata zone resilver

Driven by sequential I/O on a surviving disk, **no blockref walk**:

1. For each metadata extent `e` in `volhdr.md_extents[]`:
   - Pick source disk = any surviving disk (preference: lowest disk
     index that is online and not the replacement).
   - For each 4 MB chunk in `[e.md_off, e.md_off + e.md_size)`:
     - Read 4 MB from source disk via `cluster_read`.
     - Verify HAMMER2 block checksums on every block in the chunk;
       on failure, try a different surviving disk; on all-fail,
       record corruption and continue (the corruption is real and
       the block was never recoverable).
     - Write 4 MB to the same offset on the replacement disk via
       `bawrite`.
   - Drain via `VOP_FSYNC(replacement_devvp)` at extent boundary.

Cost estimate: 50 GB / 150 MB/s ≈ 6 min per extent on HDD.
Negligible CPU.

Phase A is interruptible — restart re-reads the source from scratch.
No per-chunk checkpoint state.

---

## Phase B — Data area resilver

Walk blockrefs reachable from live tree + all snapshots. For each
DATA or DIRENT blockref whose `copyid == failed_disk_id`:

1. Locate the stripe slot: `slot_id = (bref.data_off - slot_origin)
   / stripe_unit`.
2. Identify the column the failed disk hosts in that slot. Possibilities:
   - **Failed disk hosted a DATA column** for this blockref: reconstruct
     the data from surviving data columns + P (or P, if 2 disks failed
     and the other is also a data column, also use Q via `dual_recov`).
   - **Failed disk hosted P** for this slot: recompute P = XOR(data
     columns).
   - **Failed disk hosted Q** for this slot: recompute Q = RS(data
     columns).
3. Read `ndata` (or `ndata + 1` for the 2-failed case) surviving columns
   from disk(s). 64 KB each, single `breadn` per column.
4. Compute the missing column via existing `dual_recov` / `gen_syndrome`
   (carried over from v3, `local_hammer2_raid6.c`, memory: Fix 14).
5. `bawrite` 64 KB to the replacement disk at the per-slot byte offset.
6. After every `RESILVER_BATCH` blockrefs (default 256), drain via
   `VOP_FSYNC(replacement_devvp)`.

### Traversal order

Depth-first walk in `mirror_tid` ascending order. For each chain visited:

- If `chain->bref.type == INODE || INDIRECT || FREEMAP_*`: descend.
  These blocks are in the metadata zone — already resilvered by
  Phase A.
- If `chain->bref.type == DATA || DIRENT`: check
  `chain->bref.copyid == failed_disk_id`. If yes, queue for column
  reconstruction (step 2 above). If no, the failed disk hosted P or Q
  for this slot — also queue (P/Q columns are reconstructed
  identically; the difference is which math path).

A blockref may be referenced by both the live tree and a snapshot.
Walk **the live tree only**, then iterate snapshots. A `slot_id`
visited multiple times short-circuits after the first reconstruction
(in-memory `resilvered[]` bitmap).

### Why blockref-reachability, not LBA scan

LBA scan reconstructs every stripe slot regardless of liveness.
Wastes time on free slots; for sparse-stripe usage (most workloads),
this is significant. Blockref walk reconstructs only the slots that
matter.

Memory: v3 had a `resilver_dirty_lo/hi` range (Fix 12) tracking
stripes that races with concurrent writes during resilver. v4
doesn't need this: concurrent writes land in fresh slots; if those
slots happen to use the replacement disk's `copyid`, the write goes
to the replacement directly (no race). If they don't, the resilver
doesn't care.

---

## Concurrent writes during resilver

- **Healthy write**: allocator picks the next free slot via the
  cursor. Slot's `copyid` for data may be the replacement disk
  (replacement is online by now). Write goes directly there. No
  resilver interaction.
- **Snapshot create during resilver**: pins additional blockrefs.
  Resilver picks them up if it has not yet walked them; otherwise
  the slots are already done.
- **Snapshot delete during resilver**: a blockref that was queued
  for resilver may be deleted before its slot is processed. The
  resilver still writes the reconstruction — the block's data is
  still on disk via the COW chain until bulkfree, so the write is
  not harmful. After resilver, bulkfree may free the slot.

No locking changes are required vs steady-state operation. The
resilver runs as a kernel thread reading via the normal DIO path.

---

## Idempotency and restart

Mid-resilver crash or unmount → on next mount, if the failed disk
is still marked "replacing":

1. **Phase A** restarts from extent 0, chunk 0. No per-chunk
   checkpointing. Pure re-copy; idempotent (mirrored write to the
   same offset, no consequences).
2. **Phase B** restarts from blockref-walk root. Slots already
   reconstructed in the prior attempt are overwritten with the
   same reconstruction (deterministic from surviving columns).
   Idempotent.

Cost of restart: O(metadata zone size + live data blocks). Worst
case = full resilver from scratch. Acceptable; resilvers in v1 are
not expected to require checkpointing.

---

## Completion

After Phase B finishes:

1. Run a final blockref walk to verify every queued reconstruction
   has been written. (Trivially true if no exception was raised;
   the walk is for paranoia.)
2. Update `voldata.raid_config.disk_state[failed_disk_id]` =
   `ONLINE`, clear the replacing flag.
3. Bump `v4_txg_seq`, commit.

The replacement disk is now part of the live array; subsequent
writes treat it as ordinary.

---

## Failure during resilver

If a *second* disk fails while resilvering:

- **Failed disk is reading source for Phase A**: switch to another
  surviving disk for the same chunk. Phase A continues.
- **Failed disk is one of the surviving data/parity columns in Phase B**:
  reconstruction now requires *both* parity columns. Compute via
  `dual_recov`. Existing v3 code handles this; carry over.
- **Replacement disk fails during write**: replacement is now "failed
  during replace." Mark and abort the resilver; admin replaces the
  replacement.

ENXIO if at any point fewer than `ndata` disks remain online.

---

## Open items deferred to Phase 1

- Throttle policy: how aggressively to read/write during resilver
  vs serve live traffic. ZFS uses `zfs_resilver_min_time_ms`; pick
  an analog.
- `hammer2 raid status` should report Phase A progress (bytes/sec,
  ETA) and Phase B progress (blockrefs reconstructed / total).
- Whether to validate post-resilver checksum-walk as a separate
  manual step (`hammer2 raid scrub`) or as part of completion.
