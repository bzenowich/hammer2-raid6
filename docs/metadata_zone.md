# HAMMER2 v3 RAIDZ2-native — Metadata Zone On-Disk Format

**Status**: Phase 0 spec. Implementation in Phase 1.
**Supersedes**: any metadata-layout text in `raidz2_in_hammer2.md` §5.
**Cross-refs**: `newplan.md` §5.3, §9.1.

> **Numbering note (2026-05-24).** "v4" below = the RAIDZ2-native
> design that now ships as `HAMMER2_VOL_VERSION_RAIDZ2 = 3`.  "v3"
> below = the pre-rewrite RAID6-below-HAMMER2 layer, deleted in
> Phase 1.  The dev-tree split numbering was collapsed.

---

## Purpose

A dedicated per-disk LBA range that holds **all** non-data block types
(INODE, INDIRECT, FREEMAP_NODE, FREEMAP_LEAF) as an **N-way mirror**
across every disk in the array. Mirrored writes (same byte offset on
all N disks) eliminate per-stripe parity for metadata, give HDDs
sequential locality for cold metadata workloads, and make metadata
resilver a sequential copy rather than a blockref-walk.

Data and DIRENT blocks use the **stripe bitmap zone** (zone 41) and
parity P+Q; see `stripe_bitmap.md`.

---

## Block types in the metadata zone

| Block type            | In metadata zone? | Notes                              |
|-----------------------|-------------------|------------------------------------|
| `HAMMER2_BREF_TYPE_INODE`      | **yes**  | 1KB sub-buffer in 64KB DIO         |
| `HAMMER2_BREF_TYPE_INDIRECT`   | **yes**  | up to 64KB                         |
| `HAMMER2_BREF_TYPE_FREEMAP_NODE` | **yes** | freemap radix interior nodes      |
| `HAMMER2_BREF_TYPE_FREEMAP_LEAF` | **yes** | freemap radix leaves              |
| `HAMMER2_BREF_TYPE_DATA`       | **no**   | stripe bitmap zone, P+Q parity    |
| `HAMMER2_BREF_TYPE_DIRENT`     | **no**   | stripe bitmap zone, P+Q parity    |
| `HAMMER2_BREF_TYPE_VOLUME`     | **no**   | reserved zone 0 (per-disk copies) |

Rationale: INODE and INDIRECT are O(1) per file/dir and rarely modified
in steady state, but are pathologically scattered under v3 — random
4KB-ish writes across the entire LBA range. Mirroring them in a tight
band trades a small amount of capacity (~5%) for a large reduction in
HDD seeks during `find`, mount, and resilver.

DIRENT could theoretically live in the metadata zone (it is small and
read-heavy at directory traversal time), but it is allocator-coupled
to DATA in the existing chain code paths. Keep DIRENT in the stripe
bitmap zone for v1; revisit in Phase 5 if metadata-zone churn permits.

---

## Physical layout (per disk)

```
+---------------------+  byte 0
| Reserved zones 0–40 |   (volume headers, existing HAMMER2 reserved zones)
| 41× HAMMER2_ZONE_SEG64 = 164 MB
+---------------------+  byte 164 MB
| Stripe bitmap zone  |   zone 41 (4 MB)
+---------------------+  byte 168 MB
| Metadata zone       |   ~5% of per-disk LBA = ~50 GB on a 1 TB disk
| (extent 0)          |   identical content on all N disks
+---------------------+  byte 168 MB + zone_size
| Metadata zone ext.  |   (optional; allocated on demand by offline tool)
| (extents 1..M)      |
+---------------------+
| Data area           |   stripe slots; per-disk parity P+Q
| (zone 42 .. end)    |
+---------------------+  byte = per-disk LBA range
```

`HAMMER2_ZONE_SEG64` is the existing 4 MB zone-segment constant; do not
change it. The metadata zone extends zone numbering past 41 with a
*range* rather than discrete 4 MB slots:

- **Zone 42** = first metadata zone extent (extent 0).
- **Zone 42 + k** (for k > 0) = optional extent k, allocated by the
  offline zone-extend tool.

Each extent is a contiguous LBA range whose length is recorded in the
volume header. Extents need not be adjacent on disk, but extent 0
**must** start immediately after the stripe bitmap zone.

---

## Volume header fields

Two new fields in the on-disk volume header (or in the RAID config
substructure, TBD during Phase 1):

```c
struct hammer2_md_extent {
    uint64_t md_off;     /* byte offset of extent on this disk */
    uint64_t md_size;    /* extent size in bytes */
};

#define HAMMER2_MD_MAX_EXTENTS 8

struct hammer2_volhdr_v4_addendum {
    uint32_t md_nextents;
    uint32_t md_reserved;
    struct hammer2_md_extent md_extents[HAMMER2_MD_MAX_EXTENTS];
};
```

The extent table is identical across all N disks (mirrored). Extent 0
is populated by `newfs_hammer2 --raid6` at format time.

---

## Initial sizing

At format time, extent 0 size = `max(64 MB, 5% of per-disk LBA)`.

| Per-disk LBA | Extent 0 size |
|--------------|---------------|
| ≤ 1 GB       | 64 MB         |
| 100 GB       | 5 GB          |
| 1 TB         | 50 GB         |
| 10 TB        | 500 GB        |

Lower bound (64 MB) avoids extent-extend churn on small test images
during Phase 2 virtio-blk bring-up. Upper bound is none; 500 GB on
a 10 TB disk is unremarkable.

`newfs_hammer2 --raid6 --metadata-size=<N>[KMGT]` overrides the default.

---

## Allocator behavior

The existing freemap radix (`local_hammer2_freemap.c`) is **unchanged
in algorithm**. The only change:

- When allocating for a metadata block type (INODE, INDIRECT,
  FREEMAP_NODE, FREEMAP_LEAF), the search is restricted to LBA ranges
  inside the metadata extents.
- Each extent is a separate radix region. Allocator iterates extents
  in order (extent 0 first); fails over to extent 1 on full, and so on.
- When allocating for a data block type (DATA, DIRENT), the search
  uses the **stripe bitmap zone** — see `stripe_bitmap.md`. The
  freemap radix is **not** consulted for DATA/DIRENT in v4.

Concretely: `hammer2_freemap_alloc` gains an early dispatch on
`bref.type`. For metadata types it returns an LBA inside an extent;
for data types it forwards to `hammer2_stripe_alloc` (new).

---

## Mirrored write rule

Any write to the metadata zone is a **synchronous mirrored write**:

1. Allocator returns a byte offset `md_off` inside some extent.
2. The chain layer writes the block to `md_off` on **every** disk
   `vol[0..N-1]`. Identical content.
3. All N writes use `bawrite` and are drained together at TXG commit
   via the existing `VOP_FSYNC(devvp)` loop in
   `hammer2_inode_chain_flush`.

This is a degenerate case of parity: with N copies and `ndata = N`,
no Reed-Solomon math is needed. Reconstruction = read any surviving
copy; CHECK FAIL on one copy = read another.

DIO key encoding for metadata blocks: use a separate key space than
v4 DATA/DIRENT so the cache lookup unambiguously selects "per-disk
read" vs "stripe reconstruction". Concrete encoding: TBD in Phase 1
(`newplan.md` §9.5).

---

## Read path

- **Healthy**: read from disk 0 by default; on CHECK FAIL, try disk 1,
  disk 2, ... until a copy verifies or all N exhausted.
- **Degraded (1 disk down)**: skip the failed disk; read from any
  surviving disk; identical correctness.
- **Degraded (2 disks down)**: same — N−2 copies remain, all
  identical.
- **All N down**: ENXIO.

There is no parity reconstruction path for metadata. Metadata
*cannot* be corrupted by parity computation bugs.

---

## Resilver path

Sequential copy:

1. Identify the failed disk's byte range corresponding to each
   metadata extent.
2. For each extent, read the extent from a surviving disk in large
   sequential I/Os (e.g. 4 MB chunks).
3. Write each chunk to the corresponding LBA on the replacement disk.
4. Verify checksums on read; abort with EIO if a surviving copy is
   corrupted.

Order of magnitude: 50 GB metadata zone, 150 MB/s HDD sequential =
~6 minutes. Compare to v3 blockref-walk resilver of metadata: hours.

---

## Extension (offline tool)

When extent 0 (or the highest extent) approaches full, an offline
tool grows the zone:

```
hammer2 raid metadata-extend <pool> --size=<N>[KMGT]
```

Requires the pool unmounted. Steps:

1. Verify the highest extent's tail is followed by free data-area
   LBA on **every** disk (no live stripe overlap). If not, refuse
   with a clear error pointing to overlapping stripes.
2. If yes: mark the overlapping data-area LBA range as reserved,
   add a new extent to `md_extents[]`, bump `md_nextents`, write
   updated volume headers to all disks.

The tool need not move existing metadata — extents are read-only with
respect to address; only new allocations land in the new extent.

---

## Per-disk zone-full handling

Mirrored writes mean all N disks fill at the same rate, so true
per-disk imbalance is not normally possible. The two cases where it
occurs:

1. **Replace-with-larger disk**: replacement disk has extra LBA range
   the others lack. Not a zone-full case; replacement contributes the
   array minimum, extras are ignored until all disks ≥ that size.
2. **Allocation bug / out-of-band corruption**: zone fills below the
   extent ceiling.

Behavior in either case: array transitions to **read-only** with
`vfs.hammer2.md_zone_full=1` set. Mount messages clearly identify
the cause. Admin runs `hammer2 raid metadata-extend` (after unmount)
or restores from backup.

No silent fallback to data-area allocation for metadata: that would
contaminate the data area with un-extent-managed metadata and break
the resilver-by-sequential-copy invariant.

---

## Snapshots

Snapshots are blockref-tree references (per `raidz2_snapshot_interaction.md`).
A snapshot's reachable blockrefs may point at any metadata block in
any extent — extents are not snapshot-partitioned. Bulkfree walks
live + all snapshots; a metadata block is freed only when no root
references it.

Snapshot creation cost is unchanged: O(1), zero-copy. Snapshot
delete walks the snapshot's blockref tree and decrements refcounts
in the freemap radix (existing logic).

---

## Format-version gate

The metadata zone exists **only on v4** (`HAMMER2_VOL_VERSION_RAIDZ2`)
volumes formatted with `--raid6`. v1/v2/v3 mounts are read-only and
do not see the metadata zone; `md_nextents == 0` on those versions.

---

## Open items deferred to Phase 1

- Exact volume-header field placement (existing volhdr layout vs new
  RAID config substructure).
- DIO key encoding for metadata vs data blocks (`newplan.md` §9.5).
- `newfs_hammer2 --metadata-size` flag parsing in `local_mkfs_hammer2.c`.
- Allocator dispatch point: `hammer2_freemap_alloc` early return vs
  caller-side type check.
