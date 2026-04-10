# HAMMER2 RAID6 — ZFS-Like RAID Freemap: Design Analysis

This document analyzes the changes required to implement a ZFS RAIDZ2-like
copy-on-write stripe layout at the HAMMER2-RAID6 layer, eliminating the write
hole. It is a design study, not a plan for immediate implementation.

---

## Background: What ZFS Does Differently

ZFS RAIDZ2 avoids the write hole through two structural properties:

**1. Copy-on-write**: Every write goes to a freshly-allocated stripe, never
in-place. The old stripe remains valid until the TXG uberblock commit atomically
advances the block pointer. At no point does any on-disk state reference a
partially-written stripe.

**2. Variable-width full-stripe writes**: A RAIDZ2 stripe covers exactly the
logical block being written, padded to a full stripe boundary. Because the
entire new stripe (data + P + Q) is written in one batch before the block
pointer advances, parity is always consistent with data — there is no "old P/Q
that needs to be updated."

The structural elimination of the write hole in ZFS is a consequence of these
two properties together. Neither property alone is sufficient. In-place writes
with variable-width stripes still have a write hole if any partial-stripe state
is observable. COW with fixed-width stripes (as in the current HAMMER2-RAID6)
would also have a write hole if the indirection table is not updated atomically.

---

## Current HAMMER2-RAID6 Addressing Model

The current implementation uses a **direct, formulaic mapping** from logical
address to (disk, physical offset). There is no indirection and no allocator
at the RAID layer.

```
hammer2_raid6_map(logical_off):
    logical_off &= HAMMER2_ZONE_MASK64          // within 2GB zone
    stripe_num = logical_off / (ndata * stripe_unit)
    column     = (logical_off / stripe_unit) % ndata
    p_disk     = stripe_num % ndisks
    q_disk     = (p_disk + 1) % ndisks
    phys_disk  = first non-P/Q disk at position 'column'
    phys_off   = HAMMER2_ZONE_SEG64 + stripe_num * stripe_unit
                 + (logical_off % stripe_unit)
```

This is a pure computation. Given a logical offset, the physical location is
completely determined by the formula — no table lookup, no freemap, no state.
Stripe N is always at `ZONE_SEG + N * stripe_unit` on each disk that holds a
column of it.

This makes `hammer2_raid6_map` O(1) and branch-free (aside from the column-
skip loop), but it means every stripe has a fixed, permanent physical home.
Updating stripe N always overwrites stripe N. There is no way to write a new
version of stripe N to a different location.

---

## What a RAID-Layer COW Would Require

For HAMMER2-RAID6 to eliminate the write hole structurally, stripe N must be
able to migrate: the new contents of stripe N must be written to physical
stripe N' (at a different disk location), and only then must the mapping
`N → N'` become visible. This requires the following six components.

---

### Component 1: Physical Stripe Allocator

A new allocator manages which physical stripes are free. A physical stripe
occupies one `stripe_unit`-sized block on each of the N disks, at the same
physical offset. Unlike HAMMER2's existing freemap (which tracks logical byte
ranges per filesystem), this allocator tracks physical stripe slots per RAID
array.

**Scope**: Each 2GB HAMMER2 zone has `(ZONE_BYTES - ZONE_SEG) / stripe_unit`
physical stripe slots. For 64 KB stripes and 2 GB zones that is `(2 GB − 4 MB)
/ 64 KB = 32,704` slots per zone. With, say, 16 zones on a 32 GB disk, there
are `16 × 32,704 = 523,264` total physical stripe slots per array.

**Bitmap representation**: A simple on-disk bitmap can track used/free status.
`523,264` bits = 64 KB. This fits comfortably in a single 64 KB block on disk 0
(or, for redundancy, one copy per disk).

**Allocation granularity**: Always one full stripe (all N disks simultaneously).
Partial-stripe allocation is not meaningful in this model because every write
is a full-stripe write.

**Where stored**: In the HAMMER2_ZONE_SEG reserved area. Two of the 41 unused
zone slots (slots 41–63, each 64 KB) can hold the physical stripe bitmap plus
a backup copy. Alternatively, the existing HAMMER2 freemap (which already has
dedicated zone slots 1–40 across 8 rotation copies) could be extended with a
new freemap "type" for RAID stripes — but that would require modifications to
the existing freemap code. A simpler approach is a dedicated flat bitmap in
unused zone slots.

**Key data structure**:
```c
/* On-disk: flat bitmap of physical stripe slots, 2 bits each */
/* Bit pattern: 00 = free, 01 = allocated, 10 = pending-free, 11 = reserved */
/* Stored in HAMMER2_ZONE_SEG slot 41 (first unused zone), 64KB */
/* Backed up in slot 42 */
#define HAMMER2_ZONE_STRIPE_BITMAP  41
#define HAMMER2_ZONE_STRIPE_BACKUP  42
```

**In-memory representation**: A simple spinlock-protected bitset in
`hammer2_dev_t`, loaded at mount, flushed on TXG commit.

---

### Component 2: Stripe Indirection Table (SIT)

The SIT maps each **logical stripe number** (the stripe index in the RAID
logical address space) to a **physical stripe slot** (a (zone, slot_within_zone)
pair). It replaces the formulaic `hammer2_raid6_map` computation.

Without the SIT, there is no way to redirect a write: physical stripe `N`
always means the columns at `ZONE_SEG + N * stripe_unit` on each disk.

**Size analysis**:

For a 6-disk array with 64 KB stripes and 4 data disks, each logical stripe
holds `4 × 64 KB = 256 KB` of logical data. A 200 GB logical address space
(50 GB per data disk × 4 disks) requires `200 GB / 256 KB = 819,200` logical
stripe entries. Each entry needs a 32-bit physical slot number (or 64-bit for
arrays > 256 TB): minimum 3.2 MB for 32-bit entries, 6.4 MB for 64-bit.

That is too large to hold in the ZONE_SEG (4 MB per zone), and far too large
to keep entirely in RAM on low-memory systems.

The SIT therefore needs to be:
- **Hierarchical** (like HAMMER2's existing freemap): a multi-level tree
  mapping logical stripe ranges to physical slots, stored on disk across
  multiple blocks.
- **Cached in memory**: hot entries kept in a hash table or LRU cache,
  on-demand loading from disk on miss.

A two-level tree is sufficient for practical array sizes:
- **Level 1 leaf**: 64 KB block on disk, holds 8192 × 32-bit physical slot
  entries → covers `8192 × 256 KB = 2 GB` of logical address space per leaf.
- **Level 2 root**: One 64 KB block, holds 8192 × 32-bit level-1 block
  pointers → supports `8192 × 2 GB = 16 TB` of logical address space.

The root block fits in a single reserved zone slot on disk. Level-1 leaves
are stored in the normal HAMMER2 data area and managed by the existing freemap
(they are just 64 KB blocks of metadata, no different from any other HAMMER2
metadata block).

**Alternative — flat array for small arrays**: For arrays up to 128 GB logical
(500K logical stripes × 32 bits = 2 MB), the entire SIT can be stored in two
contiguous 1 MB blocks in the zone reserved area and held entirely in RAM.
This removes the need for tree traversal and on-demand loading.

---

### Component 3: Modified `hammer2_raid6_map`

The current O(1) direct formula becomes an O(1) table lookup (with O(log N)
fallback for cache misses):

```c
void
hammer2_raid6_map(hammer2_dev_t *hmp, hammer2_off_t logical_off,
                  int *disk_idx, hammer2_off_t *phys_off)
{
    uint64_t logical_stripe = logical_off / (ndata * stripe_unit);
    uint64_t phys_slot = sit_lookup(hmp, logical_stripe); /* hash cache */
    uint64_t zone      = phys_slot / STRIPES_PER_ZONE;
    uint64_t slot      = phys_slot % STRIPES_PER_ZONE;

    /* P/Q rotation still uses physical_stripe (phys_slot), not logical */
    int p_disk = (int)(phys_slot % ndisks);
    int q_disk = (p_disk + 1) % ndisks;
    /* ... column-to-disk mapping as before ... */

    *disk_idx = phys_disk;
    *phys_off = zone * HAMMER2_ZONE_BYTES64 + HAMMER2_ZONE_SEG64
                + slot * stripe_unit
                + (logical_off % stripe_unit);
}
```

**P/Q rotation note**: In the current scheme, `p_disk = stripe_num % ndisks`
where `stripe_num` is the logical stripe. After indirection, P/Q rotation
should be based on the **physical** slot number to keep disk load balanced.
This is a subtle but important detail: if ten logical stripes all map to the
same physical slot neighborhood, their P/Q columns should still rotate across
all disks, which is guaranteed if `p_disk = phys_slot % ndisks`.

---

### Component 4: COW Write Path

The `hammer2_io_raid6_write` function currently reads old P/Q (RMW), computes
new P/Q, and overwrites in-place. Under the COW model:

**Step 1 — Allocate a new physical stripe slot**:
```
new_slot = stripe_bitmap_alloc(hmp)  /* atomic, under stripe_bitmap_lock */
```
This is a bit scan on the in-memory bitmap, O(1) with a free-list hint.

**Step 2 — Write the complete new stripe to the new slot**:
Because the write always covers the full stripe (all `ndata` data columns plus
P and Q), there is no RMW. All `ndisks` bios are issued simultaneously to the
new physical location:
```
for i in 0..ndisks-1:
    bawrite(disk[i], new_phys_off(new_slot, i), column_data[i])
```
All writes are truly async and parallel — no bwrite stalls.

**Step 3 — Wait for all bios to complete**:
```
biowait_all()
```

**Step 4 — Atomically update the SIT**:
```
old_slot = sit_update(hmp, logical_stripe, new_slot)
```
This is the **commit point**. Before this write is durable, the old stripe
is the valid version. After it is durable, the new stripe is valid.
The SIT update must be a single atomic write (or use the existing HAMMER2 COW
chain: the SIT is a HAMMER2 metadata block, so modifying it creates a new
chain node that becomes visible only when the parent block pointer is updated).

**Step 5 — Free the old physical slot**:
```
stripe_bitmap_free(hmp, old_slot)  /* mark as pending-free */
```
The old slot is marked "pending-free" (not immediately free) because concurrent
readers may still be reading from it. It becomes truly free after the next TXG
commit that has no in-flight readers from the old generation.

**No more write hole**: Between steps 2 and 4, the old stripe is still the
authoritative version. After step 4, the new stripe is. Neither state has
inconsistent parity, because each full-stripe write includes fresh P/Q.

---

### Component 5: TXG Integration

The SIT update (step 4 above) must be atomic. There are two ways to achieve
this:

**Option A — SIT as HAMMER2 metadata chain**:
Treat SIT leaf blocks as ordinary HAMMER2 metadata blocks managed by the
existing chain/COW infrastructure. A modification to logical stripe N causes:
1. Allocate a new L1 leaf block (via existing `hammer2_freemap_alloc`).
2. Copy the old leaf into the new leaf, update entry N.
3. The new leaf's block pointer is recorded in the HAMMER2 inode/chain for
   the SIT.
4. On TXG commit, the chain flush writes the new leaf and advances the
   super-root pointer — atomically making the new SIT version visible.

This is the cleanest approach. It piggybacks on all existing HAMMER2 crash
safety guarantees. The SIT becomes a special inode (like the freemap inode
`hmp->schain`) whose chain is flushed on every TXG commit.

**Option B — Dedicated SIT journal**:
Maintain a sequential append-only journal of `(logical_stripe, new_phys_slot)`
updates in a ring buffer at a reserved zone location. On TXG commit, write a
"commit record" to the journal. On recovery, replay the journal from the last
committed record to reconstruct the current SIT state. This is simpler to
implement but adds a second crash-recovery path that is entirely separate from
HAMMER2's own undo/redo log.

Option A is strongly preferred because it requires no new recovery code and
because the existing HAMMER2 chain flush already handles everything correctly.

---

### Component 6: Recovery Path

Under the current (direct-mapping) scheme, crash recovery is handled entirely
by HAMMER2's own undo/redo log. The RAID6 layer has no recovery code because
the physical stripe locations are immutable — if HAMMER2 committed block
pointer X before the crash, X is at a fixed physical location, and the parity
at that location was either fully written or not yet written (in which case
HAMMER2 won't reference it because the chain pointer was not committed).

Under the COW scheme:
- If the crash happens between step 2 (bio completion) and step 4 (SIT commit),
  the new stripe exists on disk but the SIT still points to the old stripe.
  The new stripe is unreachable — it will be freed on the next stripe bitmap
  scan for leaked allocations (analogous to `fsck -n` marking orphaned blocks).
  The old stripe is still valid. No corruption.
- If the crash happens after step 4 (SIT committed) but before step 5
  (old slot freed), the old slot is in a "live" state that references now-dead
  data. The stripe bitmap "pending-free" flag prevents it from being
  reallocated; a post-crash scan clears the pending-free bits for slots that
  are not referenced by the current SIT, reclaiming them.

Both cases are safe: no stripe has inconsistent P/Q because each was written
as a complete stripe before the SIT was updated.

**Leaked stripe detection**: On mount after unclean shutdown, scan the stripe
bitmap for any slots not referenced by any current SIT entry. Free them. This
is O(number of stripe slots), not O(array size — it scans the bitmap, not
the data.

---

## Impact on Existing Code

### `local_hammer2_io.c` — write path

`hammer2_io_raid6_write` currently:
1. Reads sibling data columns (or uses RMW delta from `old_data`).
2. Computes new P/Q.
3. Issues `bwrite(data)`, `bwrite(P)`, `bwrite(Q)` in degraded mode.

Under COW it becomes:
1. Allocate new physical slot.
2. Assemble all `ndata` data column buffers (from DIO cache or reconstruction).
3. Compute P/Q from scratch (no RMW needed — full stripe is known).
4. Issue `bawrite` for all `ndisks` columns in parallel to the new slot.
5. `biowait_all()`.
6. Update SIT atomically (via chain modification).
7. Mark old slot pending-free.

**RMW disappears**: Because the full stripe is always written, there is no
need to read old P/Q. The `old_data` / `raid6_old_data` field in `hammer2_io_t`
becomes unnecessary. `dual_recov`, `gen_syndrome`, and the degraded reconstruction
paths still exist for *reads* in degraded mode, but not for writes.

**Healthy vs degraded write path unification**: The distinction between healthy
and degraded write paths largely disappears. Both now write a complete stripe
to a new slot. In degraded mode, the one failed column is zeroed and P/Q is
computed over the `ndata-1` available columns — the reconstruction formula
becomes the write formula.

### `local_hammer2_ondisk.c` — volume layout

`hammer2_raid6_map` is changed from a direct formula to a SIT lookup (as shown
in Component 3). A new function `sit_lookup` is added:

```c
static uint64_t
sit_lookup(hammer2_dev_t *hmp, uint64_t logical_stripe)
{
    /* Fast path: in-memory hash table */
    sit_entry_t *e = sit_hash_lookup(&hmp->sit_cache, logical_stripe);
    if (e)
        return e->phys_slot;

    /* Slow path: load L1 leaf from disk, cache it */
    return sit_load_and_cache(hmp, logical_stripe);
}
```

`hammer2_init_volumes` gains:
- Loading the stripe bitmap from disk into RAM.
- Loading the SIT root block.
- Initializing `hmp->sit_cache` (hash table or radix tree).

`hammer2_verify_volumes_3` gains:
- Checking that the stripe bitmap block(s) exist and have valid CRCs.
- Checking that the SIT root block exists.

### `local_hammer2_flush.c` — flush path

The existing flush already writes the volume header to all disks. It gains:
- Flushing dirty SIT leaves (if Option A: these are already flushed via the
  chain mechanism, so no new code is needed).
- Flushing the stripe bitmap block to disk (one `bwrite` per copy, one to each
  disk for redundancy, using the existing `TAILQ_FOREACH` loop over `devvpl`).

### `local_hammer2_vfsops.c` — sync

`hammer2_vfs_sync_pmp` currently calls `hammer2_flush_vn_backing` in degraded
mode to drain UFS buffers. Under the COW model this call remains (for vn-based
testing), but it becomes less critical because there are no synchronous `bwrite`
stalls in the write path.

### `local_mkfs_hammer2.c` — newfs

`newfs_hammer2` currently writes initial P/Q parity for stripe 0 (the superroot
stripe). Under COW, `newfs` must also:
- Initialize the stripe bitmap (all slots free except those used by the initial
  HAMMER2 metadata stripes).
- Write the initial SIT: for each logical stripe N used by the initial HAMMER2
  layout, insert an entry `N → physical_slot(N)`. For a freshly-formatted
  filesystem, the initial mapping is identity (`N → N`); the COW machinery
  starts diverging after the first write.
- Write the SIT root block and the stripe bitmap to the reserved zone slots.

### `hammer2` userspace tool — resilver

The resilver ioctl currently reads surviving columns and writes reconstructed
data to replacement disk columns at the *same* physical offsets. Under COW,
the resilver is more complex:
- The replacement disk starts with zero data.
- For each logical stripe, the SIT gives the current physical slot.
- The physical columns are read from the surviving disks at the slot's physical
  offset, reconstructed via `dual_recov`, and written to the replacement disk
  at that same physical offset (same slot — resilver writes to an existing
  allocated slot, not to a new slot).
- After resilver completes, the stripe bitmap is not changed (the replaced disk's
  columns at each slot are now valid), and the SIT is not changed (physical slot
  numbers are stable across disk replacement).

This is actually simpler than the current resilver in one respect: the SIT
provides an authoritative list of all logical stripes that are actually in use
(allocated physical slots), so the resilver can skip physical slots that were
never allocated.

---

## Two-Layer COW: HAMMER2 + RAID6

HAMMER2 already does COW at the filesystem layer. Adding COW at the RAID layer
creates a stacked two-level COW:

```
HAMMER2 COW (chain + TXG):
    Logical block at chain pointer X → new block Y (new data, new P/Q)
    Block pointer X→Y visible after TXG commit

RAID COW (SIT):
    Physical stripe at logical_stripe N → new physical slot S'
    Mapping N→S' visible after SIT commit (which is a HAMMER2 chain commit)
```

Because the SIT itself is a HAMMER2 chain (Option A), both levels of COW are
committed in the same TXG flush. From the outside, a write is:

1. HAMMER2 allocates a new logical block (via `hammer2_freemap_alloc`).
2. The RAID layer allocates a new physical stripe slot for that logical block.
3. Data + P/Q is written to the new physical slot.
4. TXG commit: HAMMER2 chain flush writes new SIT leaf, new freemap leaf, and
   advances the super-root pointer — all in one fsync.

This is exactly what ZFS does: the TXG uberblock commit is the single atomic
commit point. Old data remains valid until the uberblock advances.

**No additional fsync is needed beyond what HAMMER2 already does.** The write
hole is closed not by adding synchronous barriers between individual disk
writes, but by making the indirection table (the HAMMER2 chain pointer that
references the SIT leaf) the single commit point.

---

## Interaction with the Existing HAMMER2 Freemap

HAMMER2's existing freemap tracks **logical** byte ranges in the filesystem:
which 16 KB logical blocks are free or allocated. This does not change under
the COW RAID model. The logical address space remains the same; only the
physical location of each logical stripe changes over time.

The RAID-layer stripe bitmap tracks **physical** stripe slots on each disk:
which physical slots are free or allocated. These two freemaps serve different
purposes and are managed independently.

One subtle point: when HAMMER2 frees a logical block (via `hammer2_freemap_free`
called from `hammer2_chain_drop`), the RAID layer must also free the physical
stripe that held that logical block's columns. This requires a hook from the
HAMMER2 logical freemap into the RAID-layer stripe bitmap freeing path.

Currently there is no such hook. The logical freemap tracks free space at the
logical level; physical stripe locations are implicit (direct formula). Adding
the hook is straightforward: `hammer2_freemap_free` is called with the logical
offset and radix of the freed block. From the logical offset,
`sit_lookup(logical_stripe)` gives the physical slot; `stripe_bitmap_free(slot)`
marks it free. The P/Q columns for that slot are freed implicitly (the slot
covers all disks).

**TRIM implication**: This hook is also the natural place to issue TRIM commands
to the SSDs. When a physical slot is freed, issue TRIM for
`(phys_slot * stripe_unit)` on each of the `ndisks` disks — exactly the
per-column TRIM described in `physical_disk_tasks.md` item 10. The COW model
makes TRIM straightforward because freed physical slots correspond precisely to
dead data.

---

## Space Overhead

The COW model requires physical capacity greater than the logical array size,
because old physical stripes must remain valid until the next TXG commit. The
worst case is: every stripe in the array is written once per TXG cycle, before
any old stripes are freed. In practice, TXG commits are frequent (every 5
seconds in the default HAMMER2 configuration), so the outstanding "live but
pending-free" stripes at any moment correspond to at most a few seconds of
writes.

For a sustained write workload of 1 GB/s and a 5-second TXG interval, the
maximum pending-free space is `5 GB / (ndata/ndisks)` of physical space above
the logical size. For a 4+2 array: `5 × 6/4 = 7.5 GB` of extra physical
capacity. This is analogous to ZFS's requirement that each VDEV pool have some
free space; a 100%-full RAIDZ2 pool cannot accept any writes.

For typical workloads (not 100% write saturation), the pending-free accumulation
is small. The minimum "headroom" requirement — extra physical capacity over the
logical size — is roughly the same as what ZFS recommends: 10–20% free space
to maintain performance.

---

## Summary of Required Changes

| Component | Files Changed | Complexity |
|-----------|---------------|------------|
| Physical stripe bitmap (on-disk structure) | `local_hammer2_disk.h` | Low |
| Stripe bitmap allocator/freeholder (in-memory) | `local_hammer2_ondisk.c`, `local_hammer2.h` | Low |
| Stripe indirection table (two-level, on-disk + hash cache) | `local_hammer2_disk.h`, `local_hammer2_ondisk.c`, `local_hammer2.h` | **High** |
| Modified `hammer2_raid6_map` (formula → SIT lookup) | `local_hammer2_ondisk.c` | Medium |
| COW write path in `hammer2_io_raid6_write` | `local_hammer2_io.c` | **High** |
| RMW removal (`old_data`, `raid6_old_data` fields) | `local_hammer2_io.c`, `local_hammer2.h` | Medium |
| SIT as HAMMER2 chain (TXG integration) | `local_hammer2_chain.c`, `local_hammer2_flush.c` | **High** |
| Stripe bitmap flush on TXG commit | `local_hammer2_flush.c` | Low |
| Hook: `hammer2_freemap_free` → `stripe_bitmap_free` | `local_hammer2_chain.c` | Low |
| Leaked stripe detection on unclean mount | `local_hammer2_ondisk.c` | Medium |
| `hammer2_init_volumes` load stripe bitmap + SIT | `local_hammer2_ondisk.c` | Low |
| `newfs_hammer2` initial SIT + stripe bitmap | `local_mkfs_hammer2.c` | Medium |
| Resilver uses SIT to enumerate live stripes | `local_hammer2_ioctl.c` | Medium |
| TRIM on stripe free (bonus, from item 10) | `local_hammer2_chain.c` | Low |

**Total estimate**: Large. The SIT data structure and the TXG integration are
the hard parts. Everything else is mechanical once those two are in place.
The removal of RMW from the write path is a significant simplification that
partially offsets the new complexity.

---

## Conclusion

Implementing ZFS-like COW at the RAID6 layer is architecturally coherent for
HAMMER2. The key insight is that the SIT (stripe indirection table) can be
implemented as an ordinary HAMMER2 chain — a special metadata inode flushed
on every TXG commit, just like `hmp->schain` (the volume-level freemap inode).
This means no new crash-recovery infrastructure is needed; the existing
HAMMER2 undo/redo log and chain flush already provide the necessary atomicity.

The structural consequence is that the write hole disappears for exactly the
same reason it disappears in ZFS: the single atomic commit point (TXG uberblock
in ZFS; HAMMER2 super-root block-pointer advance here) is the only moment when
new data becomes visible, and at that moment P/Q is already consistent with
data in the newly-allocated physical stripes.

The cost is:
1. O(1) SIT lookup replacing O(1) arithmetic in the hot I/O path (cache miss
   cost is O(1) HAMMER2 block read, amortized).
2. Physical capacity headroom (10–20%) for pending-free stripes.
3. Space and flush cost for the stripe bitmap and SIT chain.
4. Implementation complexity dominated by the two-level SIT data structure
   and its integration into the HAMMER2 chain/flush machinery.

Compared to the write-intent bitmap approach (also described in
`docs/write_hole.md`), the COW approach eliminates the write hole completely
and also eliminates the need for a crash-recovery re-sync pass, at the cost of
significantly higher implementation complexity and the physical headroom
requirement.
