# HAMMER2 RAID6 — Snapshots and RAIDZ2-Native Integration

> **STATUS (2026-05-21)**: Foundational design input. The core finding —
> `hammer2_chain_modify` in-place overwrite contradicts the no-RMW
> invariant, and must be COW-only under RAID6 — is adopted as the
> architectural rule in `newplan.md` §5.3 ("in-place overwrite is
> forbidden in RAID6 mode") and §5.7. The exact set of in-place sites
> is being catalogued in `docs/inplace_audit.md` as a Phase 0 task;
> the Phase 1 punch list (`docs/tracker.md`) carries the one-line
> guard insertion.

This document analyzes how HAMMER2's snapshot and atomic-restore features
interact with the RAIDZ2-native design from `raidz2_in_hammer2.md`. The
central finding is that snapshots do not merely coexist with the RAIDZ2-native
design — they are architecturally necessary for its key property (no RMW) to
hold in the general case.

---

## HAMMER2 Is Not Purely Copy-on-Write

`raidz2_in_hammer2.md` stated: "because HAMMER2 already does COW at the chain
level, every `hammer2_freemap_alloc` gives a brand-new stripe slot."

This is not always true. `hammer2_chain_modify` in `local_hammer2_chain.c:1693`
has three distinct modes:

```c
if ((chain->flags & HAMMER2_CHAIN_MODIFIED) == 0) {

    if (bref.type == DATA or DIRENT          &&
        !(chain->flags & CHAIN_INITIAL)      &&
        !(chain->flags & CHAIN_DEDUPABLE)    &&
        check_method == HAMMER2_CHECK_NONE   &&
        chain->bref.modify_tid >
         chain->pmp->iroot->meta.pfs_lsnap_tid) {

        newmod = 0;   /* IN-PLACE OVERWRITE — no new allocation */

    } else if (HMNT2_EMERG && modify_tid > lsnap_tid) {

        newmod = 0;   /* emergency in-place (explicitly unsafe) */

    } else {
        newmod = 1;   /* COPY-ON-WRITE — allocate new location */
    }
}
```

**`newmod = 0` (in-place overwrite)**: The chain's existing `data_off` is
reused. No new allocation. The block is modified at its current physical
location. This fires when:
- The block has no integrity check (check=NONE), AND
- The block was written after the last explicit snapshot
  (`modify_tid > pfs_lsnap_tid`)

**`newmod = 1` (copy-on-write)**: A new `data_off` is allocated. This fires
when any of the above conditions is not met — including when a snapshot exists
and the block predates it.

**`pfs_lsnap_tid`** is updated to the current transaction ID whenever
`hammer2 snapshot` is taken (`ip->meta.pfs_lsnap_tid = mtid`). When no
snapshot has ever been taken, `pfs_lsnap_tid = 0`, so `modify_tid > 0` is
always true and all blocks eligible for in-place overwrite.

---

## Consequence for RAIDZ2-Native

`raidz2_in_hammer2.md` argued that P/Q can always be computed from scratch
over `[new_data, 0, 0, 0]` because new stripe slots have zero content in
all other columns. That argument depends entirely on the physical stripe slot
being freshly allocated — it fails for in-place overwrites.

**In-place overwrite with RAIDZ2-native**:

The block sits at existing physical stripe slot S. Column 0 of that stripe
holds the block's current data. Columns 1–3 may hold other blocks' data
(packed into the same stripe slot). P/Q were computed over `[col0, col1, col2,
col3]` when the stripe was first written.

When col0 is modified in-place:
- `data_off` does not change — still points to stripe slot S, column 0
- The other columns (1–3) are unchanged on disk
- P/Q at stripe slot S is now stale: it was computed for the old col0
- Reading any surviving columns via degraded reconstruction will give wrong
  answers until P/Q is updated

RMW delta parity is required: read old col0 and old P/Q, compute new P/Q.
This is identical to the current implementation's write path. The
"no RMW" benefit of RAIDZ2-native disappears for in-place writes.

---

## When Each Mode Applies

| Condition | `newmod` | Physical stripe slot | RAIDZ2-native P/Q |
|-----------|----------|---------------------|-------------------|
| First write to any block | 1 (INITIAL) | Fresh allocation | From scratch ✓ |
| Modify block, no snapshot ever taken, check=NONE | 0 (in-place) | Same slot | RMW required ✗ |
| Modify block, snapshot exists, block predates snapshot | 1 (COW) | Fresh allocation | From scratch ✓ |
| Modify block, created after snapshot, check=NONE | 0 (in-place) | Same slot | RMW required ✗ |
| Modify DEDUPABLE block | 1 (COW) | Fresh allocation | From scratch ✓ |
| Metadata blocks (BREF_TYPE_INODE etc.) | Always 1 | Fresh allocation | From scratch ✓ |

The in-place path fires exactly when `modify_tid > pfs_lsnap_tid`. A snapshot
raises the `lsnap_tid` floor: all blocks written before the snapshot must COW
on next modification. Blocks written after the snapshot can in-place again.

The effect of taking snapshots on the RAIDZ2 write path:

- **No snapshots ever**: `pfs_lsnap_tid = 0`, all data blocks can in-place →
  RMW required for every block after first write.
- **Snapshot just taken**: all existing live blocks have `modify_tid ≤ lsnap_tid`
  → must COW on next write → fresh stripe slots → P/Q from scratch.
- **Time passes, new blocks written post-snapshot**: new blocks have
  `modify_tid > lsnap_tid` → can in-place again → RMW needed for those blocks.
- **Second snapshot taken**: floor raised again, cycle repeats.

Snapshots do not permanently eliminate in-place overwrites. They create a
boundary: blocks that existed at snapshot time must COW when modified; blocks
created after the snapshot can be overwritten in-place until the next snapshot.

---

## The Clean Solution: Unconditional COW in RAID6 Mode

For RAIDZ2-native to eliminate RMW entirely — the clean architectural goal —
in-place overwrite must be disabled for RAID6 arrays. This is a one-line
change in `hammer2_chain_modify`:

```c
} else {
    /* Sector overwrite allowed only on non-RAID6 arrays.
     * RAID6 always COWs to preserve the fresh-stripe invariant:
     * a freshly-allocated stripe slot has zero content in all
     * other columns, so P/Q can be computed from scratch with
     * no disk reads required. */
    if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6)
        newmod = 1;
    else
        newmod = 0;
}
```

With this change:
- Every data block write in RAID6 mode allocates a new stripe slot.
- The stripe slot's other columns are zero (never written).
- P/Q = `[new_data XOR 0 XOR 0 XOR 0, ...]` = `[new_data, ...]`.
- No reads needed. No RMW. No delta parity machinery.
- `raid6_old_data`, `old_data` fields in `hammer2_io_t` can be deleted.
- `hammer2_io_raid6_write`'s read path (breadnx calls for P/Q and siblings)
  can be deleted. The function reduces to a pure write-only operation.

**Performance cost**: For workloads that never take snapshots, each data write
now incurs an allocation + a future free (via bulkfree) instead of an in-place
overwrite. This means slightly more freemap I/O and more physical writes (the
old stripe slot must be processed by bulkfree before its space is reclaimed).
For snapshot-heavy workloads — where in-place overwrites already don't apply —
there is no cost.

**This is exactly what ZFS does**: the SPA's RAIDZ2 vdev never overwrites. ZFS
has no notion of "in-place overwrite" because the SPA always allocates new
stripe space for every write. The check=NONE optimization does not exist in ZFS
because ZFS relies on the block pointer hash (SHA-256/Fletcher) for all blocks.

---

## Snapshot Semantics: Fully Preserved

With unconditional COW in RAID6 mode, snapshot behavior is identical to the
current implementation at the semantic level. The only difference is the
encoding in `blockref.data_off` (physical vs logical address).

### Snapshot creation

`hammer2_ioctl_pfs_snapshot` copies `pmp->pfs_iroot_blocksets[0]` (the
current root blockref array) into a new PFS inode under the super-root:

```c
wipdata->u.blockset = pmp->pfs_iroot_blocksets[0];
```

This copies the physical stripe slot addresses (stored in `blockref.data_off`)
into the snapshot's root inode. The snapshot now holds a permanent reference
to the same physical stripe slots as the live tree at that moment.

No physical data is copied. No stripe slots are duplicated. The snapshot adds
no space overhead beyond the snapshot PFS inode itself (~64 KB).

### After snapshot: COW on modification

With `modify_tid ≤ lsnap_tid` (or, under unconditional COW, always):
1. `hammer2_freemap_alloc` allocates a new physical stripe slot S'.
2. Block data is written to column `c` of S'.
3. Other columns of S' are zero (freshly allocated, `HAMMER2_CHAIN_INITIAL`
   path zeroes the buffer in `hammer2_io_new`).
4. P/Q computed: `P = col_c, Q = GF_mult(col_c)` (other columns zero).
5. `chain->bref.data_off = phys_off(S', c)`, `bref.copyid = c_disk`.
6. The old stripe slot S is still referenced by the snapshot's blockref.
   It is not freed.

The snapshot continues to see the old data at slot S. The live tree now sees
the new data at slot S'. The stripe slots are independent.

### Snapshot restore

Reverting to a snapshot is an in-memory operation:

```
pmp->pfs_iroot_blocksets[0] = snapshot->u.blockset
TXG commit (volume header write)
```

After the TXG commit, the root blockref array points to the snapshot's
physical stripe slot addresses. All stripe slots allocated between the snapshot
and the restore are now unreferenced from the live tree (though the snapshot
itself still references its own blocks). They will be freed by bulkfree.

The bulkfree scan walks all chain trees (live tree + all snapshots) and
identifies all referenced physical stripe slots. Unreferenced slots are marked
free in the physical stripe bitmap. This is unchanged from the current
implementation, with "physical stripe slot" replacing "logical byte range."

### Deduplication

Content-addressable dedup assigns `chain->bref.data_off = dedup_off` where
`dedup_off` is the physical stripe slot address of an existing identical block.
The `CHAIN_DEDUPABLE` flag is set. On next modification, `DEDUPABLE` triggers
`newmod = 1` (COW to a new stripe slot), preserving the dedup'd block at its
original slot for other references.

This is unchanged under RAIDZ2-native. Two blockrefs pointing to the same
physical stripe slot share the same P/Q (which is consistent with the shared
data). Modification of either always COWs to a new slot.

---

## Stripe Fragmentation Under COW-Heavy Workloads

With unconditional COW, every data write allocates a new stripe slot with one
data column occupied and three zero. Over time, with many writes and no compaction,
the array accumulates stripe slots where only one of the four data columns is
non-zero. The 256 KB physical stripe holds 64 KB of meaningful data — 25%
utilization.

This is the same fragmentation ZFS experiences. ZFS mitigates it in two ways:

1. **Sequential allocation**: the SPA allocates stripe slots sequentially,
   so a sustained sequential write fills stripes completely before advancing
   to new ones. Random writes create sparse stripes.

2. **Gang blocks for small allocations**: Very small blocks are packed
   into 512-byte sectors within a stripe column. Multiple small blocks share
   one stripe column, and the column is filled before advancing.

HAMMER2's existing `bmap_data.linear` packing already does the equivalent of
gang blocks at the 64 KB DIO granularity: many small HAMMER2 blocks share a
single 64 KB physical buffer (DIO). Under RAIDZ2-native, this 64 KB buffer
IS the stripe column. When the DIO is newly allocated, all the small blocks
packed into it together are "written at the same time," and P/Q is computed
over the entire 64 KB column.

For snapshot-heavy workloads, fragmentation is inherent and manageable through
periodic rebalancing (analogous to ZFS's `zpool scrub` + compaction). The
space efficiency during the live window between snapshots is a function of
the write pattern: sequential writes fill stripes; random small overwrites
fragment them.

---

## Summary

| Aspect | Impact on RAIDZ2-native |
|--------|------------------------|
| In-place overwrite (`newmod=0`) | Breaks no-RMW invariant. Fix: unconditionally set `newmod=1` for RAID6. One-line change in `hammer2_chain_modify`. |
| Snapshot creation | Fully preserved. Copies physical stripe slot addresses from live tree root. No data duplication. |
| Post-snapshot COW enforcement | Naturally enforces RAIDZ2 fresh-stripe invariant for all blocks that predate the snapshot. Blocks written after a snapshot can in-place again — this is handled by the unconditional COW fix above. |
| Snapshot restore | Fully preserved. Rewinds root blockref addresses. Freed slots reclaimed by bulkfree scan. |
| Dedup | Fully preserved. `DEDUPABLE → newmod=1` (COW). Physical stripe slot sharing between dedup'd blocks is identical to logical address sharing today. |
| Emergency mode (`HMNT2_EMERG`) | Already documented as unsafe. In RAID6 mode, emergency in-place would still require RMW parity. Acceptable since the mode is explicitly marked unsafe. |
| Stripe fragmentation | Inherent to COW on random writes. Same as ZFS RAIDZ2. Mitigated by DIO-level sub-stripe packing (already implemented) and periodic bulkfree. |

The key relationship: snapshots and RAIDZ2-native need each other for their
best properties. Snapshots need COW to protect the snapshot's data from live-tree
modifications. RAIDZ2-native needs COW to guarantee fresh-stripe allocations
and eliminate RMW. Disabling in-place overwrites satisfies both simultaneously
and is the right architectural choice for a RAID6 array.
