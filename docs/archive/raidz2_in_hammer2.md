# HAMMER2 RAID6 — Integrating RAIDZ2 Into HAMMER2's COW Layer

> **STATUS (2026-05-21)**: Foundational design input. Folded into
> `newplan.md` §5 (Clean Design v4) and §9.1 (metadata layout). Where
> this document and `newplan.md` disagree, `newplan.md` wins. Specific
> divergences:
> - §5.2/5.3 here predate the **hybrid metadata zone** decision in
>   `newplan.md` §9.1. Metadata is **not** parity-protected — it lives
>   in a dedicated zone, freemap-allocated, N-way mirrored at zone
>   offsets across all disks. See `docs/metadata_zone.md`.
> - "Three-bwrite window" / vn-backing analysis (Fixes 13, 15) is
>   test-infrastructure history; **deleted** in Phase 1.
> - DIO key encoding discussion is superseded by the Phase 1 audit
>   item in `newplan.md` §9.5.
>
> **Numbering note (2026-05-24).** Treat any "v4" / "version 4" /
> "HAMMER2_VOLHDR_VERSION_RAID6 = 4" in this doc as referring to the
> RAIDZ2-native format that now ships as `HAMMER2_VOL_VERSION_RAIDZ2 = 3`.
> The dev-tree split numbering was collapsed because neither v3
> (RAID6-below) nor v4 (intermediate RAIDZ2-native) ever escaped.

This document analyzes what it would take to bring RAID6 parity into HAMMER2's
existing chain/COW machinery — making HAMMER2 "RAID-native" the way ZFS is,
rather than layering a separate RAID translation below the filesystem.

---

## Starting Point: What HAMMER2 COW Already Provides

Before describing what would need to change, it is worth understanding what the
current implementation already provides through HAMMER2's COW machinery.

### The TXG commit sequence

`hammer2_vfs_sync_pmp` ends with `hammer2_inode_chain_flush(..., HAMMER2_XOP_VOLHDR)`,
which runs the following sequence inside `local_hammer2_flush.c:1460-1609`:

```
1. Flush all dirty chains (data DIO released → _hammer2_io_putblk →
   bawrite(data) + bawrite(P) + bawrite(Q) for healthy mode)

2. VOP_FSYNC(devvp, MNT_WAIT) for every open device in hmp->devvpl
   — waits for ALL pending bios on each disk to complete,
     including the P and Q bawrite buffers from step 1

3. BUF_CMD_FLUSH per disk → hardware write-cache flush (ATA FLUSH CACHE)

4. bwrite(volume header) → TXG commit
```

Step 2 is the critical guarantee: all P/Q `bawrite` buffers obtained via
`getblk(devvp_p/q, ...)` are drained before the volume header is written. The
volume header (TXG commit point) becomes durable only after P/Q is durable.

**Consequence**: On physical hardware, HAMMER2-RAID6 already eliminates the
write hole by ensuring P/Q is durable before the TXG commit through the
VOP_FSYNC drain. The three-bwrite window described in `write_hole.md` exists
as an in-flight gap, but P/Q bwrites are always ordered before the TXG commit,
so no crash can leave the array in a state where canonical (committed) data
has inconsistent parity.

The reason this is not obvious from the code is that the ordering guarantee
comes from the VOP_FSYNC loop, not from making individual P/Q writes
synchronous. On vn devices backed by UFS, VOP_FSYNC on the vn vnode does not
flush the UFS backing file — which is why Fix 13 (IO_SYNC in vn.c) and Fix 15
(BUF_CMD_FLUSH via hammer2_flush_vn_backing) were needed. Those fixes are
test-infrastructure workarounds, not correctness fixes for physical hardware.

### What the current implementation lacks

The existing architecture has two remaining differences from ZFS RAIDZ2:

1. **RMW parity computation**: Updating one data column in a stripe requires
   reading the old P/Q (and old data, via delta parity) to compute new P/Q.
   ZFS eliminates this by writing full stripes — each block allocation covers
   an entire RAIDZ2 stripe, so all data columns are known at write time and
   P/Q can be computed from scratch.

2. **Two-layer addressing**: `blockref.data_off` stores a logical offset in
   HAMMER2's address space. The RAID6 layer maps this to (disk, phys_off) via
   `hammer2_raid6_map` at runtime. ZFS block pointers store physical column
   addresses directly; there is no separate translation layer.

The rest of this document analyzes what changes would be required to address
both of these, integrating RAIDZ2 semantics into HAMMER2's existing COW layer
rather than adding a second COW layer on top.

---

## The Architectural Difference: Two Layers vs One

### Current HAMMER2-RAID6 architecture

```
hammer2_blockref_t.data_off  →  logical offset  →  hammer2_raid6_map()  →  (disk_idx, phys_off)
                                 (HAMMER2 addr)                              (one disk, one column)
```

- `data_off` is a scalar logical offset in HAMMER2's continuous address space.
- `hammer2_raid6_map` translates it to a physical disk and offset via
  `stripe_num = logical_off / (ndata × stripe_unit)`.
- P/Q are at the same `phys_off` on the P/Q disks — implicit from the formula.
- The DIO at `data_off` covers only one data column; P/Q are separate DIOs on
  separate disks.

The RAID6 layer is entirely invisible to the chain/blockref machinery. A
`blockref` pointing to a data block has no idea it is stored as one column of
a six-disk RAID6 stripe.

### ZFS RAIDZ2 architecture

```
ZFS block pointer DVA  →  (vdev_id, physical_offset)  →  directly readable from disk
                           (encodes stripe position)
```

- The block pointer's DVA (Data Virtual Address) encodes the physical disk and
  offset of the data portion of the RAIDZ2 stripe.
- The RAIDZ2 vdev computes P/Q at write time from the data alone (full stripe
  is written, no other columns to read).
- There is no logical→physical translation table or formula. The block pointer
  IS the physical address.
- The SPA (allocator) allocates RAIDZ2 stripes as a unit: one allocation
  places one logical block across all ndisks in one stripe.

### The key distinction

In HAMMER2-RAID6, a `blockref` says: "the data is at logical offset X in the
RAID6 address space." The disk location is computed on demand.

In RAIDZ2, a block pointer says: "the data is physically at disk D, offset P."
The RAID stripe is the unit of allocation, not the logical byte range.

---

## Component-by-Component Changes

### Component 1: `hammer2_blockref_t.data_off` — from logical to physical

This is the structural root of the change. `data_off` must encode the physical
location of the data column, not a logical offset in HAMMER2's address space.

**Current encoding** (`local_hammer2_disk.h:630`):
```
bits 63–6:  logical byte offset in HAMMER2 address space
bits  5–0:  radix (block size = 1 << radix)
```

**Required encoding**:
```
bits 63–6:  physical byte offset of the DATA COLUMN on its disk
bits  5–0:  radix (unchanged)
```

Plus: the blockref needs to identify WHICH disk holds the data column. Options:

**Option A — Encode disk index in `data_off`**: Use bits 62–58 (5 bits,
supports up to 32 disks) as a physical disk index, with bits 57–6 as the
physical offset within that disk. This repurposes 5 bits from the offset
field, reducing the addressable disk size from 2^58 bytes to 2^53 bytes
(~9 PB per disk — sufficient for any foreseeable deployment).

**Option B — Use the `copyid` field**: `copyid` (1 byte) currently identifies
which HAMMER2 multi-copy volume a block belongs to. In a RAID6 deployment,
there is only one copy (`copyid = 255` = local media). Repurposing `copyid`
as a physical disk index (0–ndisks-1) frees `data_off` bits 57–6 for offset.

**Option B is cleaner**: `copyid` is already semantically close ("which
physical volume"), and it avoids reducing offset precision. It requires no
structural size change to `hammer2_blockref_t` (already exactly 128 bytes).

**On-disk format implications**: This is a format-version change. Existing
filesystems must be converted or mounted read-only. A new
`HAMMER2_VOL_VERSION_RAIDZ2 = 4` (current is 3 for RAID6) would guard the
new interpretation of `copyid` and `data_off`. The upgrade path would require
running a conversion tool (`newfs_hammer2 --upgrade` or a dedicated
`hammer2 raidz2-convert`) that rewrites all blockrefs with physical addresses.
This is a hard break — no backward compatibility.

---

### Component 2: `hammer2_freemap_alloc` — allocate physical stripe slots

Currently (`local_hammer2_chain.c:1596`):
```c
error = hammer2_freemap_alloc(chain, nbytes);
/* sets chain->bref.data_off = new logical offset */
```

The freemap allocator scans `bmap_data` entries (each covering 4 MB of logical
space) for free 16 KB-aligned slots, using the `bigmask` hint to find
sufficient-sized free runs.

**Required change**: The allocator must be aware that allocating a data block
simultaneously allocates:
- One data column of `stripe_unit` bytes on the data disk
- One P column of `stripe_unit` bytes on the P disk
- One Q column of `stripe_unit` bytes on the Q disk

For this to work, `hammer2_freemap_alloc` must:
1. Select a free stripe slot (a `stripe_num` not currently in use).
2. Return the physical column address: `(data_disk, phys_off)` where
   `phys_off = HAMMER2_ZONE_SEG + stripe_num * stripe_unit + intra_col_offset`.
3. Mark the stripe slot as allocated in the freemap (covering all disks
   simultaneously, since all N column slots in a stripe are claimed together).

**Stripe-unit vs block-size mismatch**: HAMMER2 allocates blocks as small as
1 KB (inode data). If each allocation claims a full 64 KB stripe column, a
1 KB block wastes 63 KB of column space plus the 128 KB of P/Q columns. Two
approaches:

**Sub-stripe packing (keep current 64 KB DIO, pack blocks within it)**:
HAMMER2's existing freemap already packs multiple small blocks into a 64 KB
physical buffer via the `bmap_data.linear` iterator. Under RAIDZ2, the DIO
granularity stays at 64 KB = `stripe_unit`. Multiple small blocks in the same
DIO share one stripe column. P/Q covers the full 64 KB column. When a new
64 KB column is allocated (fresh stripe), P/Q is computed over
`[column_data, 0, 0, 0]` — no RMW. When multiple blocks are packed into an
existing column (within the same TXG), P/Q covers all of them together.

This approach requires minimal changes to the allocation granularity. The
allocator unit remains 64 KB (one DIO), and the freemap's existing block-range
bitmap can be repurposed to track physical stripe slot usage.

**Variable-width columns (true ZFS-style)**:
Each block of radix R occupies a column of width `(1 << R) / ndata` bytes
on each disk. A 64 KB block (radix 16): column = 16 KB. A 1 KB block (radix
10): column = 256 bytes. P/Q are the same width as the data columns.

This eliminates wasted capacity but requires the DIO layer to handle variable
`psize`. Currently, `_hammer2_io_getblk` sets `dio->psize = HAMMER2_PBUFSIZE`
(always 64 KB). Changing this to `psize = max(lsize, HAMMER2_PBUFSIZE)`
preserves current behavior for large blocks. For sub-64 KB blocks, multiple
sub-stripe DIOs would need to be packed, which reintroduces complexity.

**Sub-stripe packing is strongly preferred**: it is the lower-complexity path
and matches the existing HAMMER2 DIO model. Variable-width columns would
require pervasive changes to the DIO layer.

---

### Component 3: Freemap data structure — track physical stripe slots

The existing `hammer2_bmap_data` tracks logical byte ranges using a 512-bit
bitmap where 2 bits represent 16 KB of logical space. One `bmap_data` entry
covers 4 MB of logical space.

Under the physical-stripe model, the freemap must track whether a physical
stripe slot is free or allocated. A 4 MB zone holds `(4 MB - 4 MB reserved) /
64 KB = 0` usable stripes in zone 0, and `(2 GB - 4 MB) / 64 KB = 32,704`
usable stripes per 2 GB zone on each disk.

**Required change**: `bmap_data.bitmapq` (8 × 64 bits = 512 bits) currently
represents logical byte ranges. Under RAIDZ2, it represents physical stripe
slot availability: 512 bits × (1 bit per stripe) = 512 stripes per `bmap_data`
entry. Each entry covers `512 × 64 KB = 32 MB` of physical stripe slots.

This requires reinterpreting (and for upgrades, rewriting) all existing
`bmap_data` entries. The hierarchical freemap structure (Levels 0–5) and
rotation scheme (8 copies, cycle through on flush) remain unchanged — only the
interpretation of the leaf bitmap changes.

**Sizing**: For a 32 GB disk with 64 KB stripes:
- Usable stripe slots: `(32 GB - 16 × 4 MB) / 64 KB = 512,000` stripes
- Bitmap size: `512,000 / 8 bits = 64 KB` — fits in one 64 KB leaf block
- With the existing two-level freemap, the root block and one leaf handle any
  practical disk size.

---

### Component 4: `_hammer2_io_putblk` — P/Q written alongside data

Currently, the write path for a dirty DIO in `_hammer2_io_putblk`:
```
1. [degraded] bwrite(data_bp)
2. hammer2_io_raid6_write(hmp, pbase, data_buf, psize)
   → reads P_old, Q_old  (RMW)
   → computes P_new, Q_new via delta parity
   → bwrite(P_bp), bwrite(Q_bp)
```

Under RAIDZ2, for a **freshly-allocated stripe** (column first written):
```
1. Compute P = data_buf (XOR with zeros for other columns = identity)
   Compute Q = same (GF multiplication over zeros)
2. bawrite(data_bp)  — to data disk
3. bawrite(P_bp)     — to P disk, same phys_off
4. bawrite(Q_bp)     — to Q disk, same phys_off
```
No reads. No RMW. P/Q are computed entirely from the new data and the implicit
zeros of the unoccupied columns.

For a **column already written in a previous TXG** (partial stripe update —
still requires RMW):
```
1. Read old_data via breadnx (from data disk, phys_off = same stripe)
2. Read P_old, Q_old via breadnx (from P/Q disks)
3. Compute P_new = P_old XOR old_data XOR new_data  (delta parity)
4. Compute Q_new similarly
5. bawrite(data_bp), bawrite(P_bp), bawrite(Q_bp)
```

**When does the fresh-stripe vs partial-stripe case apply?**

Under HAMMER2's COW: `hammer2_freemap_alloc` always allocates a NEW logical
address for a modified block. With the RAIDZ2 allocator always choosing fresh
stripe slots, the first write to any stripe slot is always a fresh-stripe
write. Subsequent modifications to a block at that stripe slot always produce
a new `data_off` (via COW), at a new stripe slot — so EVERY write is a
fresh-stripe write.

**This is the key insight**: because HAMMER2 already does COW at the chain
level, every `hammer2_freemap_alloc` gives a brand-new stripe slot. The column
at that slot has never been written. P/Q can always be computed from scratch —
RMW is never required.

This holds as long as:
- The RAIDZ2 allocator never reuses a stripe slot that still holds live data
  (ensured by the freemap: allocated slots are marked used until freed via
  `hammer2_freemap_adjust`)
- The column at a freshly-allocated stripe slot is zeroed (ensured by the
  existing `HAMMER2_CHAIN_INITIAL` path in `_hammer2_io_getblk` which zeroes
  new allocations via `hammer2_io_new`)

**Consequence**: Under a RAIDZ2-native HAMMER2, `hammer2_io_raid6_write` is
always computing P/Q over `[new_data, 0, 0, 0]` (for ndata=4). The function
simplifies to a single XOR pass (P = new_data, Q = GF mult of new_data) with
no disk reads at all. `dual_recov`, `gen_syndrome`, and the RMW machinery
(`raid6_old_data`, `old_data`) become unnecessary.

The P/Q buffer allocation still uses `getblk(devvp_p/q, phys_off, stripe_unit)`
exactly as today — this code barely changes. The change is in the *inputs* to
the computation: no reads required.

---

### Component 5: `_hammer2_io_getblk` — read directly from physical column

Currently (`local_hammer2_io.c:179-203`):
```c
/* RAID6: compute physical disk/offset via formula */
hammer2_raid6_map(hmp, pbase, &disk_idx, &phys_off);
vol = &hmp->volumes[disk_idx];
dio->devvp = vol->dev->devvp;
dio->dbase = pbase - phys_off;
```

Under RAIDZ2: `pbase` is the physical column offset (from `blockref.data_off`).
`disk_idx` comes from `copyid` (Option B above) or from the high bits of
`data_off` (Option A). No call to `hammer2_raid6_map` is needed:

```c
/* RAIDZ2: disk_idx from bref.copyid, phys_off directly from pbase */
disk_idx = chain->bref.copyid;   /* physical disk index */
vol = &hmp->volumes[disk_idx];
dio->devvp = vol->dev->devvp;
dio->dbase = 0;  /* phys_off = pbase directly */
```

`hammer2_raid6_map` becomes unused and can be deleted.

**Degraded reads**: `hammer2_io_raid6_read_degraded` still needs to know
which disk holds which column for the stripe at a given physical offset. Under
RAIDZ2, the P/Q disk indices are still deterministic:
`p_disk = (stripe_num % ndisks)` where `stripe_num = phys_off / stripe_unit`
(same formula, but now derived from the physical offset, not the logical
offset). The degraded read path changes minimally — the physical offset is
now in `blockref.data_off` directly rather than computed from a logical offset,
but the reconstruction arithmetic is identical.

---

### Component 6: `hammer2_io_raid6_resilver` — now stripe-slot based

The resilver currently reads surviving columns for each logical stripe (via
`hammer2_raid6_map`) and writes reconstructed data to the replacement disk.

Under RAIDZ2, the resilver must know which stripe slots are allocated (from
the freemap) rather than iterating all possible logical stripe numbers. This
is actually simpler: only iterate over allocated physical stripe slots. Unallocated
slots (never written, all zeros) need not be resilvered — their P/Q is all-zeros
by definition.

The resilver can scan the freemap bitmap, find all allocated stripe slots, and
resilver only those. This makes the resilver proportional to the amount of data
on the array, not the raw capacity.

---

### Component 7: On-disk format change

This change touches every `hammer2_blockref_t` on every disk. The `data_off`
field goes from "logical offset in HAMMER2 address space" to "physical column
offset on the data disk" and `copyid` goes from "copy ID (unused for RAID6)"
to "physical disk index."

**Impact**:
- All existing HAMMER2 RAID6 filesystems are incompatible with the new format.
- `hammer2_version` in the volume header must advance to distinguish old format
  from new.
- `newfs_hammer2` must write physical column addresses in initial blockrefs.
- `fsck_hammer2` must understand both formats (for conversion tools).
- The conversion tool (`hammer2 raidz2-convert`) would need to walk every
  blockref in the entire chain tree and rewrite `data_off`/`copyid` to the
  new encoding. This is an O(all metadata) operation, equivalent to a full
  fsck pass, but also needs to compute the correct physical address for each
  block — which requires knowing the logical-to-physical mapping from the
  current formula. This is feasible: for each old `data_off`, apply
  `hammer2_raid6_map(old_data_off)` to get (disk_idx, phys_off), then store
  `phys_off` in `data_off` and `disk_idx` in `copyid`.

---

### Component 8: `hammer2_freemap_adjust` — physical-aware free

When a chain is freed (`hammer2_freemap_adjust(bref, HAMMER2_FREEMAP_FREE)`),
the freemap marks the corresponding physical stripe slot as free. Currently
this frees a logical byte range. Under RAIDZ2, it frees a physical stripe slot
(which implicitly frees the P/Q columns on P/Q disks — they are always freed
together).

The call site in `local_hammer2_chain.c` is unchanged:
```c
hammer2_freemap_adjust(hmp, &chain->bref, HAMMER2_FREEMAP_FREE);
```
Only the implementation changes: instead of clearing a logical bitmap entry,
clear the physical stripe slot bit.

This is also the natural hook for TRIM (from `physical_disk_tasks.md` item 10):
when a physical stripe slot is freed, issue TRIM for `(phys_off, stripe_unit)`
on each of the ndisks — the P/Q disks receive the same TRIM as the data disk.

---

## What Disappears

| Removed | Replacement |
|---------|-------------|
| `hammer2_raid6_map()` formula | Direct physical address from `blockref.data_off` + `bref.copyid` |
| `hammer2_io_raid6_write` (RMW path) | Direct P/Q computation over new data + zeros (no reads) |
| `raid6_old_data` / `old_data` fields in `hammer2_io_t` | Unused (no RMW) |
| Logical RAID6 address space (2 GB zone masking) | Physical stripe slot space per disk |
| The SIT and physical stripe bitmap from `zfs_freemap_design.md` | Not needed — HAMMER2's freemap already serves this purpose |

The elaborate SIT (stripe indirection table) described in `zfs_freemap_design.md`
is **not needed** in this architecture. There is no indirection table because
there is no two-level mapping: the blockref directly encodes the physical
address. The COW happens at the HAMMER2 chain level (new blockref = new
physical address), not at a separate RAID indirection layer. This is
architecturally equivalent to what ZFS does, without stacking two COW systems.

---

## What Stays the Same

- HAMMER2 chain/TXG structure: unchanged. The COW machinery, flush ordering,
  and volume header commit are all unchanged. The write-hole guarantee already
  present (VOP_FSYNC before volume header write) remains.
- `hammer2_io_raid6_read_degraded`: still needed for reads when a disk has
  failed. The reconstruction arithmetic (dual_recov, gen_syndrome) is unchanged.
- `hammer2_io_raid6_resilver`: structure unchanged, input changes from
  "iterate all logical stripes" to "iterate allocated physical stripe slots."
- P/Q disk selection (left-symmetric rotation): unchanged.
- Freemap rotation (8 copies per zone): unchanged.
- Volume header written to all disks: unchanged.

---

## Complexity Comparison

| Task | Current (separate RAID layer) | RAIDZ2-native |
|------|-------------------------------|---------------|
| Write path RMW | Required (read P/Q + old data) | Eliminated (P/Q from scratch) |
| `blockref` format change | None | Yes — `data_off` + `copyid` semantics change |
| Freemap change | None | Physical stripe slot tracking |
| New data structures | `hammer2_raid6_auto_fail_disk`, `absent_data` | Remove `raid6_old_data`; change freemap leaf |
| On-disk compatibility | Compatible with non-RAID6 HAMMER2 | Format version bump; conversion tool required |
| TRIM integration | Needs explicit hook | Naturally falls out of `freemap_adjust` |
| Degraded reads | Unchanged | Unchanged |
| Resilver | Iterate all logical stripes | Iterate allocated stripe slots (simpler) |

The RAIDZ2-native approach is architecturally simpler at runtime (no formula,
no translation layer, no RMW). The one-time cost is the on-disk format change
and the conversion tool. The freemap data structure change is medium complexity.

The total code delta is likely **smaller** than the current RAID6 implementation
because the RMW machinery (old_data, raid6_old_data, delta parity reads, the
degraded write synchronization) can all be removed. The main additions are:
- `bref.copyid` interpretation in `_hammer2_io_getblk` (few lines)
- Physical stripe allocator in `hammer2_freemap_alloc` (replace logical scan)
- Freemap bitmap reinterpretation (mostly a data-format change)
- P/Q computation without RMW in `_hammer2_io_putblk` (deletion of RMW reads)

---

## The Fundamental Insight

HAMMER2 already performs COW at the block level: every write to a block creates
a new `blockref.data_off` at a freshly-allocated address. In the RAIDZ2-native
model, that fresh address IS a freshly-allocated physical stripe slot. Because
the slot is new, all other columns in the stripe are zero, and P/Q can be
computed from the new data alone with no disk reads.

The write hole is avoided not by additional mechanisms but by the same TXG
commit ordering that already exists: P/Q `bawrite` calls are drained by
VOP_FSYNC before the volume header `bwrite`. This is already present and
correct on physical hardware.

The primary engineering investment is the on-disk format change (blockref
reinterpretation + conversion tool) and the freemap physical-stripe tracking.
The runtime complexity — especially the RMW path — decreases. The degraded
read path is unchanged. The net effect is a simpler and more correct
implementation at the cost of a one-time format migration.
