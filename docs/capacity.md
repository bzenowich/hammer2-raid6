# v3 capacity: full-disk stripe space

**Status**: in progress (2026-10-04).
**Replaces**: the zone-41 bitmap and refcount blocks in `stripe_bitmap.md`.

## Problem

The stripe bitmap covers slots in the first 2 GB zone of each disk only
(`hammer2_raid6_bitmap_init`: `usable = HAMMER2_ZONE_BYTES64 - SEG64`), so a
v3 array holds at most 2 GB x ndata of file data. The bitmap and the row
refcount are each one 64 KB block, rewritten whole on every flush. A 1 TB
disk has 16M slots: a 2 MB bitmap and a 16 MB refcount.

Related defects found while mapping the layout:

1. **Frees are not deferred.** `hammer2_chain_modify` frees the old slot
   before the tree that still references it is committed. Once the
   allocator cursor wraps, the slot can be reused and overwritten before
   the new volume header lands; a crash then loses committed data.
2. **The persisted bitmap can run ahead of the tree.** It is written before
   the volume header, and the header does not say which bitmap generation
   it commits. A crash between the two leaves frees from an uncommitted
   TXG on disk.
3. **Overlapping areas on disk 0.** mkfs puts the boot area at 4-68 MB and
   the aux area at 68-324 MB, and its sroot/root inodes at 324 MB, which
   is a data-slot address. The bitmap (164 MB) and the metadata zone
   (168 MB) sit inside the aux area.
4. **Zone reserved segments.** Volume header backups and freemap blocks
   live in the first 4 MB of each 2 GB zone. They line up with per-disk
   offsets only when the volume size is a multiple of 2 GB.
5. **Resilver Phase A** copies the metadata zone but not the zone-0
   reserved segment (freemap blocks).
6. **Packing did not count.** An allocation packed into an open row left
   the row refcount at 1, so freeing any one chain of a packed row freed
   the row under its other live chains. Fixed with the space map: the
   pack path increments the count.
7. **Deletes never free data slots, and COW frees ignore snapshots.**
   The only free is in `hammer2_chain_modify` (COW of a DATA/DIRENT
   chain). Deleting a file frees nothing; upstream bulkfree covers only
   the freemap, which on v3 tracks metadata. A COW of a block that a
   snapshot shares frees a slot the snapshot still references. Neither
   showed while the cursor could not wrap. See "Freeing" below.

## Layout (per disk, identical on every disk)

```
0            4 MB                     sm_end      md_off           md_end
| reserved   | space map: copy A, B  | aux (8MB) | metadata zone   | data slots ...
| zone 0     | (boot area)           |           | (sroot/root at  |
| volhdr+fm  |                       |           |  md_off)        |
```

- Each 2 GB zone `z` keeps its reserved segment `[z*2GB, z*2GB + 4MB)`.
  Data slots skip it.
- The volume size is truncated to a multiple of 2 GB by mkfs, so the
  logical zones of every volume line up with per-disk offsets.
- The boot area holds the space map (`boot_beg` = 4 MB). The aux area is
  the 8 MB minimum. mkfs's static allocations (sroot, root inodes) start
  at `md_off`, inside the metadata zone.
- `num_slots = (volume size - 4 MB) / stripe_unit`. A slot is a data slot
  unless it lies below the end of metadata extent 0, overlaps a zone
  reserved segment, or overlaps another metadata extent.
- The boot area is sized to hold both copies, rounded up to 8 MB. Metadata
  extent 0 is 5% of the disk (64 MB minimum), rounded down to 8 MB.
  `md_off` must stay under 1 GB, which limits a disk to about 30 TB at a
  64 KB stripe unit (also bounded by `HAMMER2_SM_MAX_PAGES`).

On the 4 GB test disks: space map 4-12 MB, aux 12-20 MB, metadata extent 0
20-220 MB, data from 220 MB (64 KB block 3520, `DATA_BLK` in
`tests/v3/common.sh`).

`raid_config` gains (carved from `reserved[]`):

| field | meaning |
|---|---|
| `rz_layout` | 2 for this layout. A v3 array with anything else is refused at mount (re-mkfs). |
| `rz_num_slots` | slot count |
| `rz_sm_pages` | data pages per copy |
| `rz_sm_off`, `rz_sm_copy` | space map offset and the size of one copy |
| `rz_sm_gen` | the space map generation this volume header commits |

## Space map

Only the row refcount (one byte per slot) is persisted; the bitmap bit is
`refcount != 0` and is derived at load.

One copy, in 64 KB pages (the size of every other device buffer):

```
page 0          header: magic, version, ndisks, stripe_unit, num_slots,
                generation, cursor, npages, copy, header CRC;
                then at byte 128 one icrc32 per data page
pages 1..npages refcount data, 65536 slots per page
```

The header CRC covers the header fields and the CRC table.

Copies A and B alternate: generation `N` is written to copy `N & 1`.

**Write (TXG flush, before the volume header):**

1. `N = rz_sm_gen + 1`, copy `c = N & 1`.
2. Every data page dirty for copy `c`: copy it to a buffer, CRC the buffer,
   write it to every live disk. Clear its dirty bit for `c`.
3. Write the header with generation `N` and the CRC table.
4. The existing `BUF_CMD_FLUSH` before each volume header write makes the
   copy durable; the header then carries `rz_sm_gen = N`.

A change to a page marks it dirty for both copies, so each copy catches up
with what changed since it was last written.

A page that fails to write on any disk stays dirty. If no live disk took
the whole copy, the flush skips the volume headers. The new generation
becomes current only once the headers are written; until then the next
flush rewrites the same copy.

**Load (mount):** read copy `rz_sm_gen & 1`. The CRC table comes from the
first live disk whose header is valid with generation `rz_sm_gen`; each
page is taken from the first disk whose copy matches its CRC. A copy newer
than the volume header is ignored. A page bad on every disk: rebuild with
the blockref walk and mark every page dirty. After a load, every page is
dirty for the other copy, since nothing says what it holds.

**Full writes:** after a walk rebuild, and after a disk is replaced or
re-added (its copies are stale), every page is marked dirty for both
copies, so the next two flushes write both copies whole.

**Consistency without locking:** the copy written for generation `N` may
include allocations made after flush `N` started. After a crash those
slots are leaked (refcount too high), never lost. Frees are safe because
of deferral (below).

## Freeing

Two problems remain (defect 7). Deferring the COW free fixes the crash
window but not snapshots: chain_modify cannot tell whether a snapshot
shares the block. A snapshot-safe design frees data slots the HAMMER2 way,
with a bulkfree pass over every PFS and snapshot that recomputes the
refcounts and frees what nothing references, and drops the COW free. That
also covers deletes. Pending a decision; the deferred-free design below is
the interim plan if COW frees are kept.

### Deferred frees

`hammer2_raid6_stripe_free` no longer changes the refcount. It queues the
slot on the open free list. At the start of each volume flush the open list
is closed; after that flush's volume header is written on every disk, the
closed list is applied (refcount decrement, bitmap clear, page dirty).
A free recorded during a flush waits for the next one.

Until then the slot cannot be allocated, so no committed tree's data is
overwritten before a tree without it is durable.

## Steps

1. Layout fields, mkfs layout, geometry (`num_slots`, `slot_is_data`),
   resilver range and Phase A reserved segments. (Done.)
2. Space map A/B with dirty pages, `rz_sm_gen`, load, full writes on
   replace and rebuild. (Done.)
3. Freeing: deferred frees, or bulkfree (see Freeing).
4. Tests: an array larger than 2 GB per disk filled past 2 GB; free/reuse
   across a crash; statfs.
