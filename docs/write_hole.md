# HAMMER2 RAID6 — The Write Hole Problem

> **STATUS (2026-05-21)**: v3-era analysis. WIB ("write-intent bitmap")
> approach is **not applicable** to v4. Under v4 RAIDZ2-native (COW +
> TXG-commit atomicity, `newplan.md` §5.6), the write hole closes
> structurally: pre-commit crashes orphan fresh stripes that bulkfree
> reclaims; post-commit crashes inherit a valid new TXG. Sections of
> this document on synchronous-writes and md-RAID WIB describe
> abandoned designs. The ZFS RAIDZ2 comparison remains useful as
> background.

Analysis of the write hole in RAID5/RAID6 implementations and a comparison of
the three main approaches used to eliminate or mitigate it: synchronous writes
(current HAMMER2-RAID6 approach), md RAID's write-intent bitmap, and ZFS
RAIDZ2's copy-on-write design.

---

## What Is the Write Hole?

A RAID5/RAID6 "write hole" is the window of vulnerability between the first and
last disk write in a stripe update. Updating a stripe requires writing to at
least three separate locations on different physical disks:

```
Disk 0   Disk 1   Disk 2   Disk 3   Disk 4   Disk 5
 data0    data1    data2    data3      P        Q
   │                                  │        │
   ▼ write 1                          ▼        ▼
 new_d0                            new_P    new_Q
                                  write 2  write 3
```

If the system crashes (power failure, kernel panic) between write 1 and write 3,
the on-disk state is inconsistent: `new_d0` is on disk 0, but `new_P` and `new_Q`
may or may not have been committed. On recovery, reconstruction of any column
using the stale parity gives a wrong answer — silently.

In **healthy mode**, the write hole exists between the data write and the
asynchronous P/Q writes issued by the background parity thread. The data is on
disk, but parity has not yet been updated.

In **degraded mode**, the write hole is more acute: all three writes are
synchronous `bwrite` calls, and the window exists between them. The current
HAMMER2-RAID6 implementation closes this window by making all three writes
synchronous before returning to the caller — but this is not sufficient to
eliminate inconsistency from a mid-write crash; it only makes the window
smaller.

---

## Approach 1: Synchronous Writes (Current HAMMER2-RAID6)

### Degraded mode

`hammer2_io_raid6_write` issues three sequential `bwrite` calls:

```
bwrite(data_bp)   → disk commits data
bwrite(parity_P)  → disk commits P
bwrite(parity_Q)  → disk commits Q
```

The caller does not return until all three complete. This makes the write
**durable** but not **atomic**. A crash between `bwrite(data_bp)` and
`bwrite(parity_P)` leaves inconsistent parity on disk.

### Healthy mode

The background parity thread uses `bawrite` for P/Q, so the write hole is
potentially much wider: data may be on disk seconds before parity is updated.

### Consequence

On crash recovery, the filesystem must be `fsck`'d. HAMMER2 has a journal (the
undo/redo log), but that journal does not cover the RAID6 stripe parity — only
HAMMER2 metadata. A crash with stale parity means that any degraded-mode
reconstruction of a block whose data was written but parity was not yet updated
will produce the wrong answer. In practice this means **silent data corruption
after a crash in degraded mode**.

### Performance

- **Degraded writes on SSDs**: ~0.3ms × 3 = ~1ms per stripe → ~60 MB/s maximum
- **Degraded writes on HDDs**: ~10ms × 3 = ~30ms per stripe → ~2 MB/s maximum
- **Healthy mode**: The `bawrite`/`bdwrite` path achieves near full aggregate
  disk bandwidth; the parity thread overlaps P/Q writes with the next stripe's
  data writes.

---

## Approach 2: md RAID Write-Intent Bitmap (WIB)

### Mechanism

Before writing any stripe, md RAID sets a "dirty" bit for that stripe in a
persistent bitmap stored on disk (or on a dedicated journal device). The
sequence is:

```
1. Set bitmap bit for stripe S  →  fdatasync (synchronous, ~1 IOPS)
2. Write data_d0, P, Q          →  can be async (bawrite), truly parallel
3. Clear bitmap bit for stripe S  →  async
```

On recovery after a crash, the kernel scans the bitmap. Any stripe with a set
bit is re-synced: all N disks' data is read and fresh P/Q is recomputed and
written. This eliminates the write hole: the worst case is re-syncing a stripe
unnecessarily (if the crash happened between step 1 and step 2, before the
first disk write), but re-syncing a consistent stripe is always safe.

### Key properties

- Writes in steps 2–3 can be **fully asynchronous and parallel** — no
  synchronous stall per stripe update.
- The cost paid upfront is one synchronous `fdatasync` to the bitmap device per
  dirty stripe. For small random writes, this one sync dominates. For large
  sequential writes that dirty many stripes, the bitmap updates can be batched:
  all stripes in a write request are marked dirty in one fsync.
- The bitmap itself is small: 1 bit per stripe. For a 1 TB array with 64 KB
  stripes, the bitmap is 1 TB / 64 KB = ~16 million bits = 2 MB.
- A **journal device** (md RAID's `--write-behind` or dedicated journal disk) is
  a higher-performance variant: writes go to the journal first (sequential, fast),
  then are destaged to their final stripe locations. This adds a separate fsync
  but allows the main array writes to be batched and reordered for throughput.

### Recovery cost

On recovery, only stripes whose bitmap bit is set need to be re-synced. In the
common case (clean shutdown), the bitmap is fully clear and recovery is
instantaneous. After an unclean shutdown with N dirty stripes, recovery reads
and reconstructs N stripes — proportional to write activity at the time of the
crash, not to array size.

### Applicability to HAMMER2-RAID6

A WIB for HAMMER2-RAID6 would require:

1. A per-stripe dirty bitmap stored in the volume header or a reserved zone
   (HAMMER2_ZONE_SEG already provides per-disk reserved space).
2. A `hammer2_raid6_stripe_mark_dirty(hmp, stripe_num)` function that updates
   the in-memory and on-disk bitmap before issuing any data/P/Q write.
3. A recovery scan in `hammer2_mount` that re-syncs dirty stripes after
   unclean unmount.
4. After step 3, all three writes (data, P, Q) can use `bawrite` — no
   synchronous penalty.

The main complexity is the recovery scan: it must read all columns for each
dirty stripe and recompute parity, which requires iterating the stripe address
space and is O(array size in the worst case).

---

## Approach 3: ZFS RAIDZ2 Copy-on-Write

### Mechanism

ZFS never overwrites data in-place. Every write goes to a new, previously-unused
location on disk. The sequence for a write is:

```
1. Allocate new stripe S' at a fresh location
2. Write all columns of S' (data + P + Q) in one full-stripe write
3. Commit the new block pointer (uberblock TXG commit)
4. Free the old stripe S
```

Step 3 is the only synchronous barrier: the TXG uberblock is a single atomic
sector write. Before that write completes, the old data is still valid and
accessible via the old block pointer. After that write completes, the new data
is valid and accessible via the new block pointer. There is no moment in time
when neither the old nor the new data is valid — and parity is always consistent
because the entire stripe was written together.

### Why RAIDZ2 eliminates the write hole structurally

RAIDZ2 uses **variable-width stripes**: a stripe covers exactly the number of
disks needed to hold the logical block, rounded up to the next RAIDZ boundary.
For a 64 KB block on a 6-disk RAIDZ2, the stripe is exactly 6 × (64 KB / 4 +
parity) wide. Because the stripe is always a full-stripe write (no partial stripe
update, no RMW), there is no "old parity" that needs to be updated — the stripe
is computed fresh and written in its entirety.

The write hole requires two conditions:
1. An in-place partial stripe update (RMW: read old P/Q, compute new P/Q, write)
2. Non-atomic multi-disk commit

RAIDZ2 violates condition 1 (it does full-stripe writes, never partial updates),
and the COW uberblock commit makes condition 2 irrelevant.

### Performance

- No RMW penalty on writes (full-stripe writes always)
- No write hole recovery scan ever needed
- The per-TXG synchronous uberblock write (one sector) is the only stall
- On recovery, ZFS replays the intent log (ZIL) for uncommitted TXGs — this is
  O(ZIL size), typically seconds, not O(array size)

### Why HAMMER2-RAID6 cannot directly adopt the ZFS approach

HAMMER2 already does COW at the filesystem layer — it never overwrites live
metadata in-place; each modification chains a new block pointer. However, this
COW is above the RAID layer, not within it. The RAID6 stripe layout is fixed:
logical offset 0 always maps to the same (disk, physical_offset) via
`hammer2_raid6_map`. There is no per-stripe freemap that allows stripe S to be
replaced by a new stripe S' at a different physical location.

Adding COW at the RAID layer would mean:
1. A RAID-layer freemap tracking which physical stripes are in use
2. An indirection table mapping logical stripe addresses to physical stripe
   locations (similar to a flash translation layer)
3. A TXG-like commit mechanism for the RAID-layer indirection table

This is essentially stacking two COW layers (HAMMER2 + RAID6), which would be
a major redesign. The space overhead and complexity would be substantial.

---

## Comparison Table

| Property | HAMMER2-RAID6 (current) | md RAID WIB | ZFS RAIDZ2 |
|----------|------------------------|-------------|------------|
| Write hole eliminated? | No (window exists between 3 bwrites) | Yes (bitmap ensures re-sync) | Yes (structurally impossible) |
| Degraded write latency (SSD) | 3× sequential bwrite (~1ms/stripe) | 1× bitmap sync + async bwrites | N/A (not in-place update) |
| Degraded write latency (HDD) | 3× sequential bwrite (~30ms/stripe) | 1× bitmap sync + async bwrites | N/A |
| Healthy write latency | bawrite (async, fast) | Same as current (WIB adds ~1 sync) | Full-stripe COW write |
| Crash recovery cost | O(array size) fsck if degraded | O(dirty stripes) re-sync | O(ZIL size) replay |
| Implementation complexity | Low (already done) | Medium (bitmap + recovery scan) | Very high (RAID-layer freemap) |
| RMW required? | Yes (delta parity) | Yes | No |

---

## Current Status and Recommendations

The current HAMMER2-RAID6 implementation uses synchronous bwrites in degraded
mode (Fix 10). This is correct in the sense that each individual write is
durable before the next begins, but it does not eliminate the write hole — a
crash between the second and third `bwrite` produces stale parity on disk.

For the initial implementation, this is acceptable because:
1. HAMMER2's own undo/redo log protects filesystem metadata consistency.
2. Data blocks that were written before the crash are readable via the surviving
   disk's data column; only reconstruction of the *specific stripe being written*
   at the moment of the crash is compromised.
3. SSD deployments (~1ms per stripe) make the window small.

The write-intent bitmap is the correct medium-term fix. It allows async parallel
writes (eliminating the HDD performance problem) while guaranteeing that any
stripe in an inconsistent state is identified and re-synced on recovery. The
per-stripe dirty bit can be stored in the reserved HAMMER2_ZONE_SEG area on
disk 0 (64 MB reserved, more than sufficient for any practical stripe count).

The ZFS approach is not feasible without a ground-up redesign of the RAID
layer's addressing model.

**Recommended path**:

1. **Short term (current)**: Synchronous degraded bwrites. Correct for SSD
   deployments. Document the residual write hole risk.
2. **Medium term**: Implement a write-intent bitmap in HAMMER2_ZONE_SEG.
   Mark stripes dirty before writing, allow async parallel P/Q writes, re-sync
   on unclean unmount recovery.
3. **Long term (HDD)**: Per-stripe locking (similar to md RAID's per-stripe
   spinlock) to allow concurrent stripe writes without parity races, combined
   with the WIB for crash safety.
