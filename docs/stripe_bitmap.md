# HAMMER2 v4 RAIDZ2-native — Stripe Bitmap On-Disk Format

**Status**: Phase 0 spec. Implementation in Phase 1.
**Cross-refs**: `newplan.md` §5.2, §5.3, §9.3, §9.4. `metadata_zone.md`.

---

## Purpose

The stripe bitmap zone tracks per-stripe allocation state at **column
granularity** for DATA and DIRENT blocks. It is the allocator for the
data area; the freemap radix is not consulted for DATA/DIRENT in v4.

---

## Stripe geometry

Recapped from `newplan.md` §5.2:

- `ndisks` = total disks in the array (4 ≤ ndisks ≤ 8 for v1).
- `ndata` = `ndisks − 2`.
- Stripe unit = `HAMMER2_PBUFSIZE` = 64 KB.
- A **stripe slot** is one row of `ndisks` columns, each 64 KB, on
  consecutive 64 KB byte ranges of every disk at the same offset.
- P+Q positions rotate per slot in left-symmetric order.

For `ndisks = 4`, each stripe slot occupies 256 KB (4 × 64 KB)
of total array space, of which 128 KB is usable data (2 columns) and
128 KB is parity.

---

## On-disk physical layout

Zone 41 (one `HAMMER2_ZONE_SEG64` slot, 4 MB) on **disk 0 only**:

```
+---------------------+  byte 164 MB on disk 0
| Bitmap header       |   1 page (4 KB)
+---------------------+  byte 164 MB + 4 KB
| Bitmap data         |   up to 4 MB − 4 KB − 4 KB = ~4 MB
| (1 bit per slot)    |
+---------------------+
| Bitmap footer       |   1 page (4 KB)
+---------------------+  byte 168 MB on disk 0
```

The bitmap is **not mirrored** the way the metadata zone is. It lives
once on disk 0; on disk 0 failure the bitmap is rebuilt from blockref
reachability at mount time (§ Mount-time verify). This avoids burning
zone 41 on every disk; only disk 0 carries the canonical copy.

Future option: dual copy at zone 41 on disk 0 and zone 41 on disk 1 if
mount-time rebuild proves too slow at scale. Not required for v1.

### Header (4 KB, page-aligned)

```c
struct hammer2_stripe_bitmap_header {
    uint64_t magic;            /* 'H2STRBM' + version byte */
    uint32_t version;          /* 1 */
    uint32_t ndisks;           /* must match volhdr */
    uint64_t stripe_unit;      /* 65536 */
    uint64_t num_slots;        /* total slots covered by bitmap */
    uint64_t slot_origin;      /* byte offset of slot 0 on a disk */
    uint64_t cursor;           /* sequential allocator cursor (slot id) */
    uint64_t generation;       /* bumped on every TXG flush */
    uint8_t  crc[16];          /* CRC of header + footer + bitmap */
    uint8_t  pad[4040];
} __packed;
```

`slot_origin` is the per-disk byte offset where slot 0 begins. By
default this equals `metadata_zone_end` — the byte after the last
metadata extent on each disk. The bitmap covers slots `[0,
num_slots)`; `num_slots = (per_disk_size − slot_origin) / stripe_unit`.

### Bitmap data (variable, page-aligned)

One **bit per stripe slot**:

- `0` = slot is free; allocator may take it.
- `1` = slot is in use; some live blockref points at one or more
  columns inside this slot.

`ceil(num_slots / 8)` bytes; padded up to page-aligned size.

The bitmap is *per-slot*, not per-column. Rationale: under
sequential-cursor allocation, an entire slot is consumed at a time
(all `ndata` columns filled with the same TXG's writes, in order, or
left zero). Sparse-stripe space (one column used, rest zero) is
tolerated as fragmentation; column-granular freeing is a Phase 5
repack concern, not v1.

### Footer (4 KB, page-aligned)

```c
struct hammer2_stripe_bitmap_footer {
    uint64_t magic_end;        /* 'EMBRTS2H' (reversed) */
    uint64_t generation;       /* must match header */
    uint8_t  crc[16];          /* CRC of header + bitmap (same as header) */
    uint8_t  pad[4072];
} __packed;
```

Header + footer pair detects torn writes: a TXG that crashed
mid-bitmap-write leaves `header.generation != footer.generation` or
a CRC mismatch; mount falls back to blockref-walk reconstruction.

---

## Allocator behavior — sequential cursor

`hammer2_stripe_alloc(hmp, bref_type) → slot_id` rules:

1. Reject if `bref_type` is not `DATA` or `DIRENT`.
2. Start from `cursor`. Find the next `0` bit at position ≥ `cursor`.
3. If found: mark bit `1`, set `cursor = slot_id + 1`, return.
4. If not found (wrap): scan from slot 0; on hit, set
   `cursor = slot_id + 1`, return.
5. If no `0` bit anywhere: return `ENOSPC`.

Cursor advances are in-memory only between TXGs; persisted to the
bitmap header on TXG flush. A crash before flush replays from the
last-committed cursor — safe, because any allocations not yet
committed are also not reachable from the post-crash volume header.

### Why sequential cursor

- HDD sequential write throughput at the device level.
- Matches ZFS metaslab cursor behavior.
- No fragmentation search overhead (vs first-fit, best-fit).
- Stripe locality follows write order — TXG-coherent writes land
  contiguously.

### Free path

`hammer2_stripe_free(slot_id)`:

- Clear bit `slot_id`. Do **not** move the cursor backwards — cursor
  only advances. Bulkfree may free thousands of slots; the next
  alloc will wrap and re-use them.

---

## TXG flush sequence

At TXG commit, before the volume header write:

1. Bump `header.generation += 1`, `footer.generation = header.generation`.
2. Compute CRC over (header without crc field) + bitmap + (footer without
   crc field).
3. Write the bitmap zone (header + bitmap + footer) via a **single**
   `bawrite` chain on disk 0 — header first, then bitmap, then footer.
4. Drain via the existing `VOP_FSYNC(devvp[0])` in
   `hammer2_inode_chain_flush`.
5. Then write the volume header to all disks (TXG commit).

Step ordering matters: footer durable before volume header durable
means a post-commit mount sees `header.generation == footer.generation`
or the bitmap is rolled back.

---

## Mount-time verify

Procedure on every mount:

1. Read bitmap header on disk 0. If header magic / CRC bad, set
   `bitmap_invalid = true` and skip to step 3.
2. Read footer; if `footer.generation != header.generation` or CRC bad,
   set `bitmap_invalid = true`.
3. **Walk all blockrefs** reachable from the volume header (live tree
   plus all snapshots). For each blockref of type DATA or DIRENT,
   compute `slot_id = (bref.data_off - slot_origin) / stripe_unit` and
   mark a memory-resident `live[]` bitmap.
4. If `bitmap_invalid`: persist `live[]` as the new bitmap at the next
   TXG flush. Cursor = highest live slot + 1.
5. Else: compare `live[]` against on-disk bitmap.
   - Bits set in on-disk but not in `live[]`: orphan stripes; queue
     for bulkfree.
   - Bits set in `live[]` but not in on-disk: **fatal corruption**;
     refuse to mount RW, suggest `hammer2 raid scrub` or fsck.

Mount-time cost: the blockref walk is O(reachable blocks). For most
filesystems this is fast (seconds to minutes). The walk happens once
at mount; subsequent reads use the in-memory bitmap.

---

## Disk 0 failure

If disk 0 is the failed disk at mount:

1. Skip steps 1–2 (zone 41 unreadable).
2. Force `bitmap_invalid = true` and proceed with the blockref-walk
   reconstruction.
3. On replacement/resilver, the next TXG flush writes a fresh bitmap
   to the new disk 0.

Reconstruction-from-blockref ensures disk 0 is not a single point of
failure for the allocator state.

---

## Snapshot interaction

Snapshots reference DATA blocks at specific slot/column positions.
A snapshot pins the slot in `live[]` for as long as the snapshot
exists, exactly like the freemap radix pins metadata blocks.

Bulkfree walks live + snapshots before freeing slots; a slot is freed
only if no root references any column of that slot.

---

## Stripe slot → column layout

Within a slot:

- Disk index 0 .. ndisks−1.
- `p_col = slot_id % ndisks`.
- `q_col = (slot_id + 1) % ndisks`.
- Data columns = the remaining `ndata` disk indices, in increasing order.

A DATA blockref encodes `(copyid = disk_idx, data_off = byte_offset)`.
`byte_offset = slot_origin + slot_id * stripe_unit`. `disk_idx` selects
which data column on this slot.

Within a slot, data columns are allocated **left-to-right** in disk-index
order. The bitmap does not record per-column state; the chain layer
manages per-slot column fill via in-memory state (TBD Phase 1).

---

## Format-version gate

Stripe bitmap zone exists only on v4 (`HAMMER2_VOL_VERSION_RAIDZ2`)
volumes formatted with `--raid6`.

---

## Open items deferred to Phase 1

- Exact bit ordering inside the bitmap (LSB-first per byte vs MSB-first).
  Match the kernel's existing `freemap_node` convention.
- Whether to dual-host the bitmap (disk 0 + disk 1) — defer unless
  mount-time rebuild proves slow on large arrays.
- In-memory representation of partially-filled slots awaiting more
  data columns in the same TXG.
