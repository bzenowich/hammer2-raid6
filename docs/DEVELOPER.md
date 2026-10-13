# HAMMER2 RAID6 — Developer Reference

Technical reference for the v3 RAIDZ2-native HAMMER2 RAID6
implementation on DragonFlyBSD.  Covers on-disk format, I/O paths,
parity, resilver, scrub, and the design choices that diverge from
ZFS / md RAID.

The v1/v2 era (background parity thread, RMW delta parity,
logical→physical address mapping, runningbufspace deadlock) was
deleted in Phase 1 — see `docs/phase1_changelog.md`.  This document
describes only the code that currently ships.

---

## Table of Contents

1.  [Architecture Overview](#1-architecture-overview)
2.  [On-Disk Format (v3)](#2-on-disk-format-v3)
3.  [Stripe Slot Addressing](#3-stripe-slot-addressing)
4.  [GF(2^8) Math Library](#4-gf28-math-library)
5.  [I/O Layer](#5-io-layer)
6.  [Open-Row Packing (M1/6C)](#6-open-row-packing-m16c)
7.  [Stripe Space Map](#7-stripe-space-map)
8.  [Metadata Mirror Zone](#8-metadata-mirror-zone)
9.  [Degraded Read Path](#9-degraded-read-path)
10. [Resilver](#10-resilver)
11. [Scrub (M3)](#11-scrub-m3)
12. [Mount-Time Initialization](#12-mount-time-initialization)
13. [newfs_hammer2](#13-newfs_hammer2)
14. [Userspace `hammer2 raid` Command](#14-userspace-hammer2-raid-command)
15. [Key Invariants and Pitfalls](#15-key-invariants-and-pitfalls)
16. [Testing](#16-testing)
17. [Outstanding / Future Work](#17-outstanding--future-work)

---

## 1. Architecture Overview

RAID6 is implemented **inside HAMMER2's I/O layer** between the chain
machinery and the raw block devices.  The freemap sees a single
logical address space; striping is invisible above the I/O layer.

```
  User/kernel reads/writes
          │
  hammer2_chain.c
          │
  hammer2_io.c         ← v3 RAIDZ2 read/write paths live here
   │                     (data/dirent: bref.copyid → disk; metadata:
   │                      primary + 2 mirrors, 3 copies)
   │
  hammer2_raid6.c      ← GF math, write_row (packed seal),
   │                     metadata mirror writes
   │
  hammer2_ondisk.c     ← stripe-slot allocator, space map,
   │                     open-row tracker, volume open/init
   │
  per-disk vnode I/O (bread/breadnx/bwrite)
   │
  /dev/vbd1 /dev/vbd2 ...
```

### Key v3 design choices

- **Stripe unit = 64 KB (`HAMMER2_PBUFSIZE`)** — one column = one DIO.
- **No logical→physical mapping.**  A DATA/DIRENT bref's primary
  disk is carried in `bref.copyid`; its byte offset on that disk is
  `bref.data_off & ~HAMMER2_OFF_MASK_RADIX`.  The freemap thinks in
  logical bytes; the I/O layer is per-disk-addressed.
- **Left-symmetric P/Q rotation** — P at `slot % ndisks`, Q at
  `(slot+1) % ndisks`.  Distributes parity-disk wear evenly across
  the array.  See §3.
- **Packed open rows (6C)** — up to `ndata` data chains share one
  row's P/Q.  At NDISKS=4 (ndata=2) this brings the per-row
  efficiency from 1/4 to 1/2.  At NDISKS≥6 it recovers most of the
  ZFS-style `(ndata)/(ndisks)` ratio.  See §6.
- **Metadata is mirrored on 3 disks**, not parity-protected — INODE /
  INDIRECT / FREEMAP_NODE / FREEMAP_LEAF chains land on the same
  per-disk byte offset on 3 disks: the primary (disk 0) and 2
  mirrors picked from the block number by `hammer2_md_copy_disk()`
  (`local_hammer2_raid6.h`).  That survives any 2 failures, like the
  data.  Layout 2 arrays (made before the change) keep a copy on
  every disk until converted with `vfs.hammer2.md_3copy_upgrade`.
  DIRENT is not metadata in this sense: like DATA it is striped with
  P/Q.  Reads pick any surviving copy.  See §8.
- **Space map** — one byte per stripe slot (in use or not; the bitmap
  bit is derived), persisted as two alternating copies on every disk;
  if no valid copy loads at mount it is rebuilt by walking the
  chain tree.  See §7 and `docs/capacity.md`.
- **Parallel P/Q on seal** — `hammer2_io_raid6_write_row` starts the
  P and Q writes together (`hammer2_bwrite_start`) and then waits for
  both (`hammer2_bwrite_wait`); the caller still blocks until they are
  done.  The parity buffers are `B_RELBUF | B_NOCACHE`.  Moving the
  seal off the putblk path (async sealing) is an open item in
  `docs/outstanding.md`.
- **COW** — every write goes to a freshly allocated stripe slot.
  No in-place update, no RMW parity.

### Read / write path summary

```
READ DATA/DIRENT
  hammer2_io_alloc(bref) → DIO keyed by (disk_idx<<56)|phys_off
    │
    ├─ disk online?   → breadnx on dev[copyid]
    │
    └─ disk failed?   → hammer2_io_raid6_read_degraded:
                         reads N-1 surviving columns from devvps,
                         dual_recov reconstructs target_col

WRITE DATA/DIRENT
  Allocator (hammer2_raid6_stripe_alloc):
    ├─ open_row_pack_locked picks an existing open row, OR
    └─ scans the slot mask for a free slot; opens a new row entry.
  Chain bytes flow through hammer2_io_putblk →
    hammer2_raid6_open_row_add_data → tracker buffers them.

  At seal (n_alloc==ndata OR TXG flush):
    (seal_row first zero-writes the data columns no chain reserved)
    hammer2_io_raid6_write_row(hmp, phys_off, cols[], ncols, bytes)
      P = XOR(cols[].data)
      Q = sum(gf_coeff_i * cols[i].data)
      bwrite_start(P), bwrite_start(Q), then wait for both

WRITE INODE/INDIRECT/FREEMAP_*
  hammer2_io_putblk (metadata branch):
    bwrite/bdwrite/cluster_write to primary disk,
    then the mirror copy is queued (hammer2_io_metadata_mirror_defer)
    and written to the block's 2 mirror disks at the same per-disk
    byte offset once per sync (hammer2_io_metadata_mirror_flush).
```

---

## 2. On-Disk Format (v3)

### Volume header

```c
#define HAMMER2_VOL_VERSION_RAIDZ2  3   /* the v3 version */
#define HAMMER2_RAID_TYPE_JBOD      0
#define HAMMER2_RAID_TYPE_RAID6     6
```

The `raid_config` sub-structure lives in sector 3 of the volume
header (offset 0x600–0x7FF, sharing the `volu_bytes[3]` slot via
union).  Pre-RAID6 volumes have it zeroed; `raid_type == 0` reads
as JBOD.

```c
struct hammer2_raid_config {
    uint8_t  raid_type;       /* 0=JBOD, 6=RAID6 */
    uint8_t  ndisks;          /* total disks */
    uint8_t  ndata;           /* ndisks - 2 */
    uint8_t  stripe_shift;    /* log2(stripe_unit) = 16 */
    uint32_t flags;           /* DEGRADED, REBUILDING */
    uint64_t stripe_unit;     /* HAMMER2_PBUFSIZE = 65536 */
    uint64_t array_size;      /* usable bytes */
    uint8_t  disk_state[HAMMER2_MAX_VOLUMES];
    /* pad to 512 bytes */
};
```

### Per-disk states

```c
#define HAMMER2_RAID6_DISK_ONLINE     0
#define HAMMER2_RAID6_DISK_FAILED     1
#define HAMMER2_RAID6_DISK_REBUILDING 2
#define HAMMER2_RAID6_DISK_SPARE      3
```

### Flags

```c
#define HAMMER2_RAID6_FLAG_DEGRADED   0x0001
#define HAMMER2_RAID6_FLAG_REBUILDING 0x0002
```

### Physical layout per disk

```
0           4 MB                   sm_end     md_off          md_end
│ reserved  │ space map copies A,B │ aux (8 MB)│ metadata extent 0 │ data slots ...
│ zone 0    │ (boot area)          │           │ (sroot/root at    │
│ volhdr+fm │                      │           │  md_off)          │
```

Identical on every disk (`docs/capacity.md` has the arithmetic).

- The first 4 MB of every 2 GB zone is reserved for volume headers and
  freemap blocks; data slots skip it.
- The space map (two copies) sits in the boot area, after zone 0's
  reserved segment, on every disk.
- Metadata extent 0 is 5% of the disk (64 MB minimum, rounded down to
  8 MB); its start `md_off` and size are in `raid_config`
  (`md_extents[]`).  A slot is a data slot unless it lies below the end
  of extent 0, overlaps a zone reserved segment, or overlaps another
  metadata extent.
- On the 4 GB test disks data starts at 220 MB.

### bref encoding (v3)

For DATA/DIRENT brefs:

- `bref.data_off` carries `per_disk_phys_off | radix`.  The
  per-disk-phys-off is the absolute byte offset on the disk
  holding this chain's data column.
- `bref.copyid` carries the disk index (0..ndisks-1) of that
  primary disk.  Used everywhere from `hammer2_dio_key` (DIO
  cache) to `hammer2_io_raid6_read_degraded` (target column
  identification).

For INODE/INDIRECT/FREEMAP_* brefs:

- `bref.data_off` carries a normal per-disk byte offset within
  the metadata zone (see §8).  The block's copies (3 on layout 3,
  every disk on layout 2) hold the same bytes at the same offset.
- `bref.copyid` is the originating disk index (set at write
  time); reads tolerate any surviving copy.

`HAMMER2_RAID6_DISK_SHIFT = 56`, `HAMMER2_RAID6_DISK_MASK =
0xFF00000000000000`.  The disk_idx<<56 bits are **never** stored
on disk; they're synthesized into the DIO cache key in memory so
distinct disks at the same phys_off don't alias.

---

## 3. Stripe Slot Addressing

### Left-symmetric rotation

```
stripe_slot = (per_disk_phys_off - HAMMER2_ZONE_SEG64) / stripe_unit
p_disk      = stripe_slot % ndisks
q_disk      = (p_disk + 1) % ndisks
data_disks  = remaining disks in numeric order, skipping p_disk / q_disk
```

There is **no `hammer2_raid6_map()` function in v3** — the
allocator chooses a disk_idx when picking an open-row data column,
records it in `bref.copyid`, and the I/O layer reads it back from
the bref.  Both newfs and the kernel agree on the rotation formula
above when computing P / Q positions during write_row.

### Worked example (NDISKS=4, ndata=2)

P/Q rotate through all four disks every 4 slots:

| Slot | Disk 0 | Disk 1 | Disk 2 | Disk 3 |
|------|--------|--------|--------|--------|
| 1024 | P      | Q      | D[0]   | D[1]   |
| 1025 | D[0]   | P      | Q      | D[1]   |
| 1026 | D[0]   | D[1]   | P      | Q      |
| 1027 | Q      | D[0]   | D[1]   | P      |
| 1028 | P      | Q      | D[0]   | D[1]   | ← repeats

A chain allocated at slot 1026 col 0 lands on disk 0 with
`bref.copyid = 0` and `bref.data_off = 0x4820000 | 16` (offset
68 MB + 2 slots × 64 KB = 68.125 MB, radix 16 = 64 KB).

### Why distributed parity (over dedicated P/Q drives)

- **Wear and write hotspot.**  Dedicated parity sends every data
  write to vn0 + vn1 — those drives accumulate `(ndisks-2)×` the
  write load.  Distributed parity spreads load evenly.
- **Read bandwidth.**  Healthy reads never touch P or Q.  With
  fixed P/Q drives the read-bandwidth ceiling is `ndata` drives
  instead of `ndisks`.
- **No simplification benefit.**  Recovery still needs the full
  `dual_recov` matrix for every data+parity combination — the
  modular-arithmetic in `hammer2_io_raid6_read_degraded` would
  collapse to fixed constants but the GF math wouldn't.

---

## 4. GF(2^8) Math Library

**Files:** `hammer2_raid6.c`, `hammer2_raid6.h`.

Irreducible polynomial `x^8 + x^4 + x^3 + x^2 + 1` (0x1d).  Tables
are computed once at module load via `hammer2_raid6_init()` from
`hammer2_vfs_init`.

| Table | Size | Purpose |
|-------|------|---------|
| `hammer2_gf_exp[256]` | 256 B | `gf_exp[i] = 2^i mod poly` |
| `hammer2_gf_log[256]` | 256 B | discrete log |
| `hammer2_gf_inv[256]` | 256 B | multiplicative inverse |
| `hammer2_gf_mul_table[256][256]` | 64 KB | full multiplication |

Inline helpers in `hammer2_raid6.h`:

```c
static inline uint8_t hammer2_gf_mul2(uint8_t x);   /* shift+reduce */
static inline uint8_t hammer2_gf_mul(uint8_t a, uint8_t b);  /* table */
```

### Syndromes

P = XOR over data columns.  Q uses Horner from the highest-numbered
column:

```
P = data[0] ^ data[1] ^ ... ^ data[ndata-1]
Q = (...((data[ndata-1] * 2) ^ data[ndata-2]) * 2 ^ ...) ^ data[0]
```

### Recovery functions

| Function | Failures handled |
|----------|------------------|
| `hammer2_raid6_2data_recov` | two data columns |
| `hammer2_raid6_datap_recov` | one data column + P |
| `hammer2_raid6_dual_recov`  | dispatcher — handles every case |

`dual_recov(ndisks, bytes, faila, failb, ptrs)` accepts column
indices `0..ndata-1` for data, `ndata` for P, `ndata+1` for Q.  It
normalises so the parity column (if present) is in `faila`, then
dispatches:

- Both parity → regenerate from data.
- One data + P → `datap_recov` (uses Q).
- One data + Q → recover from P (`D[f] = P ^ XOR(surviving data)`).
- Two data → `2data_recov` (uses P+Q).

#### Two data columns failed (`2data_recov`)

```
P' = P ^ XOR(surviving data) = D[fa] ^ D[fb]
Q' = Q ^ Σ g^i * D[i]        = g^fa * D[fa] ^ g^fb * D[fb]
D[fb] = gf_mul(gf_inv(g^fa ^ g^fb), gf_mul(g^fa, P') ^ Q')
D[fa] = P' ^ D[fb]
```

`g^fa ^ g^fb` is non-zero for `fa != fb`, guaranteed by the
caller's normalisation.

---

## 5. I/O Layer

**File:** `hammer2_io.c`.

### DIO cache key (`hammer2_dio_key`)

For v3 DATA/DIRENT brefs, the DIO cache key is synthesized as
`(disk_idx<<56) | phys_off` so different disks at the same
per-disk phys_off don't alias.  `dio->dbase` = `disk_idx<<56`,
`dio->pbase` = `dbase | phys_off`.  The high 8 bits are never
written to disk.

For metadata brefs the key falls back to `pbase = data_off & pmask`
keyed against the originating disk's volume offset.

### Read path

`_hammer2_io_getblk`:

1. `hammer2_io_alloc` builds/looks up the DIO.
2. If the DIO's `disk_idx` is in `hmp->raid_failed[]`, the read
   diverts to `hammer2_io_raid6_read_degraded` (DATA/DIRENT) or
   `hammer2_io_metadata_mirror_read` (everything else under v3
   RAIDZ2) and the surrounding op proceeds against an in-memory
   buffer.
3. Otherwise normal `breadnx` on `dio->devvp`.  For an INODE /
   INDIRECT / FREEMAP_* dio the cold read (a buffer-cache miss) may
   come from a mirror instead (`hammer2_io_meta_spread_read`, gated
   by `vfs.hammer2.md_read_spread`, default 1): one copy per 64 KB
   block, round robin over the block's copies
   (`hammer2_io_metadata_read_disk`), so a walk of the metadata uses
   every copy-holding disk.  A cached buffer is always used as is.
   The mirror read never fails the member; if it fails the primary
   read follows.

EIO injection: `vfs.hammer2.inject_eio_disk_mask` (bit per
disk_idx) forces `EIO` on `breadnx` calls without auto-failing
the disk — used by Group E tests.

### Write path (DATA/DIRENT)

`_hammer2_io_putblk` on a dirty DATA/DIRENT DIO under v3 RAID6:

1. `bcopy` the bp's contents into a kmalloc'd `raid6_data` buffer
   (so the bp can be released before parity is computed).
2. Dispose the bp (`bdwrite` / `cluster_write` / `bawrite`).
3. Hand `raid6_data` ownership to the open-row tracker via
   `hammer2_raid6_open_row_add_data(hmp, phys_off, disk_idx,
   raid6_data, psize)`.  The tracker frees it after the row seals.

Sealing is triggered by `hammer2_raid6_seal_all_open_rows` at
every TXG flush (called from `hammer2_flush.c`), or by the
allocator when an open row's `n_alloc == ndata`.

### `hammer2_io_raid6_write_row`

Single seal entry point (`hammer2_raid6.c`):

```c
int hammer2_io_raid6_write_row(hammer2_dev_t *hmp,
                               hammer2_off_t phys_off,
                               const hammer2_row_col_t *cols,
                               int ncols, size_t bytes);
```

- Assumes any data-disk column not represented in `cols[]` is zero
  on disk.  The seal step (not `write_row`) zero-writes those
  columns first; without it parity reconstruction on a later
  read/scrub yields wrong data — see §6 *Partial-row parity fix*
  and §15.
- Allocates P / Q bufs (one per surviving parity disk; failed
  disks are skipped).
- XOR-accumulates into P; GF-multiply-accumulates into Q with
  per-column coefficient `gf_exp[my_col % 255]` where `my_col`
  counts data-disk positions less than `col->disk_idx`.
- Starts the P and Q writes together (`bwrite_start`) and waits for
  both before returning.

### Write path (metadata)

`_hammer2_io_putblk` on a dirty metadata DIO under v3 RAID6:

1. Capture the bp's bytes into a kmalloc'd `md_mirror_data`
   buffer.
2. Dispose the bp on the primary disk (normal bdwrite path).
3. Queue the bytes with `hammer2_io_metadata_mirror_defer` (a
   rewrite in the same TXG replaces the queued copy).  The TXG flush
   calls `hammer2_io_metadata_mirror_flush`, which writes each queued
   block with `hammer2_io_metadata_mirror_write` to the block's
   other copies (`hammer2_md_copy_disk()` for k = 1..`md_ncopies`-1)
   at the same per-disk byte offset, with `bawrite` and no wait;
   the sync's per-member `VOP_FSYNC` waits for them before the
   volume header.  A disk being rebuilt gets the copy too, under
   `rebuild_lk`.

Reads without the buffer-cache copy use `hammer2_io_metadata_mirror_read`
when the primary is failed: it starts at the copy
`hammer2_io_metadata_read_disk` names, then tries the other copies
of the block in order, skipping failed or closed disks and disks that
hold no copy.  Metadata reconstruction does not need GF math.

---

## 6. Open-Row Packing (M1/6C)

**Files:** `hammer2_ondisk.c` (open rows, allocator), plus
`hammer2_raid6.c::hammer2_io_raid6_write_row`.

### In-memory row tracker

```c
struct hammer2_open_row {                /* abridged */
    TAILQ_ENTRY(hammer2_open_row) entry;   /* all open rows, by registration */
    TAILQ_ENTRY(hammer2_open_row) pentry;  /* packable rows */
    RB_ENTRY(hammer2_open_row)    rbnode;  /* by row_id */
    hammer2_off_t phys_off;            /* row's slot byte offset */
    uint64_t      row_id;              /* slot index */
    int           p_disk, q_disk;
    uint32_t      alloc_mask;          /* bit per allocated data disk */
    int           n_alloc;             /* popcount(alloc_mask) */
    uint32_t      gen;                 /* hmp->rz_gen at registration */
    uint32_t      sub_mask;            /* columns managed as sub-columns */
    void         *col_data[HAMMER2_MAX_VOLUMES];
    struct hammer2_subcol *sub[HAMMER2_MAX_VOLUMES];
};
```

Open rows are on `hmp->open_rows` (registration order, so by
generation), an RB tree by `row_id` (`open_rows_tree`) and, while they
can take another column, on `open_rows_pack`.  Entries are
`kmalloc`'d at register time and `kfree`'d at seal — no fixed cap,
never force-seal an incomplete row.  Memory peak per hmp is
(in-flight rows) × ndata × stripe_unit for col_data buffers,
briefly at TXG flush.  hmp also tracks `open_rows_count` and
`open_rows_high` for diagnostics.

### Sub-columns (P5/S9)

With `vfs.hammer2.raid6_subcol_max` (default 32768; 0 = off), data
chains of 1-32 KB share one 64 KB column of an open row instead of
taking a column each: an open `hammer2_subcol` per row column, with a
1 KB-granular `used_mask`, size-aligned placement and one pinned dio.
`bref.data_off` then carries column offset + sub-offset + radix.  The
sub-column closes when full, when a chain finds no range, or at the
flush.  Design and decisions: `oct-review.md` §11.

### Allocator (`hammer2_raid6_stripe_alloc`)

Under the bitmap spinlock:

1. `hammer2_raid6_open_row_pack_locked` — try to place the new
   chain in an existing open row with a free data-disk slot.
   On hit, sets `chain->bref.data_off = pack_off | radix` and
   `chain->bref.copyid = pack_disk_idx`, returns 0.
2. Otherwise scan the stripe bitmap from `stripe_cursor` for a
   free slot, open a new `open_row` entry, claim the first data
   disk.  Returns 0; sets bref likewise.

Packing is always on; the former `raid6_pack_open_rows` bisect
knob was removed.

### Sealing

`hammer2_raid6_seal_all_open_rows` (called from
`hammer2_flush.c` at every TXG commit, before the volume header):

- Advances `rz_gen`, which closes every existing row to new
  reservations (packing takes only rows of the current generation),
  then seals each older row once all its reserved columns have
  arrived.  It does *not* seal every row: rows of the new
  generation stay open and keep packing.  See `oct-review.md` §10.
- If a reserved column does not arrive within
  `vfs.hammer2.raid6_gen_wait_ms` the flush fails with `EAGAIN` and
  the header is not written.

A row also seals from inside `add_data` when packing fills it
(`n_alloc == ndata` *and* all data cols have received their
chain bytes).

`hammer2_raid6_open_row_add_data` itself never sees a
"row-not-found" — every chain reaches it via the allocator's
`open_row_register_locked` path, which TAILQ_INSERTs an entry
before the chain's bp is ever flushed.  The function
KKASSERTs the row is present.

### Partial-row parity fix (seal-time zero-fill)

If a row seals with `ncols < ndata` (TXG flush before the row
fills), P/Q is computed assuming the unwritten data cols are zero.
`hammer2_raid6_seal_row_locked_to_unlocked` issues a sync
`bwrite(zeros)` to each data-disk column NOT in `r->alloc_mask`
*before* calling `write_row`, so the disk actually holds zeros
there.

Without this, a freshly-allocated slot whose disk previously
held a different chain (reuse-after-free) keeps the prior bytes
on disk for the unwritten col.  Parity reconstruction on a
later read/resilver/scrub uses those stale bytes and produces
wrong data for the *written* cols.

Critically, the zero-fill only fires for disks **not in**
`alloc_mask`.  Disks reserved by a chain (alloc_mask bit set)
but whose chain has not yet putblk'd — `col_data[d] == NULL`
in the seal snapshot — are left alone; the chain's own
`bp.bdwrite` path will deliver fresh bytes there.  Issuing a
zero-write to such a disk would race the chain's bp and could
overwrite the chain's data.

Full rows (`ncols == ndata`, all data disks in alloc_mask) take
no extra writes.

### Row use (space map)

Whether a row is in use is one byte per slot in the space map (§7);
the allocator's free-slot oracle is the derived bitmap bit
(`stripe_bitmap`, set iff the byte is non-zero).  Nothing decrements
it: only bulkfree clears a slot, and only after two passes both find
it unreferenced (`docs/capacity.md`, *Freeing*).  The former
in-memory per-row refcount (`stripe_row_refcount`) is gone.

---

## 7. Stripe Space Map

**Spec:** `docs/capacity.md` (replaces `docs/stripe_bitmap.md`).
**Implementation:** `hammer2_ondisk.c` (`hammer2_raid6_sm_init`,
`_sm_read`, `_sm_write`, `hammer2_raid6_rebuild_stripe_bitmap`).

One byte per stripe slot, non-zero = in use.  Two copies, A and B,
on **every** disk at `raid_config.rz_sm_off` and `+ rz_sm_copy`:
generation N goes to copy `N & 1` and the volume header records the
generation it commits (`rz_sm_gen`).  A copy is a header page (magic,
generation, cursor, a CRC per data page) followed by 64 KB data pages
of 65536 slots each; dirty pages are tracked per copy.

- **Write:** the TXG flush writes the dirty pages of copy `N & 1` to
  every live disk, then the header, before the volume header.  A page
  that fails on a disk stays dirty; if no live disk took the whole
  copy the flush skips the volume headers.
- **Load:** copy `rz_sm_gen & 1`; each page is taken from the first
  disk whose page matches its CRC.  A page bad on every disk makes
  `stripe_bitmap_invalid`, and the mount rebuilds the map by walking
  the chain tree (`hammer2_raid6_rebuild_stripe_bitmap`) before RW.
- A replaced or re-added disk has every page marked dirty for both
  copies.  A volume made before the current layout (`rz_layout`
  outside `HAMMER2_RZ_LAYOUT_MIN..HAMMER2_RZ_LAYOUT`) is refused.

---

## 8. Metadata Mirror Zone

**Spec:** `docs/metadata_zone.md`.  **Implementation:**
`hammer2_raid6.c` (`hammer2_io_metadata_mirror_write`,
`_mirror_defer`, `_mirror_flush`, `_metadata_read_disk`),
`hammer2_io.c` (`hammer2_io_metadata_mirror_read`, `_mirror_heal`,
`hammer2_io_meta_spread_read`), `hammer2_ondisk.c` (extent tracking
via `hmp->md_extents[]`).  Placement rule: `hammer2_md_ncopies()` and
`hammer2_md_copy_disk()` in `local_hammer2_raid6.h`.

INODE / INDIRECT / FREEMAP_NODE / FREEMAP_LEAF chains (not DIRENT,
which is striped like DATA) are kept in **3 copies** at the same
per-disk byte offset: copy 0 is the primary (the disk the dio is
keyed on, disk 0), copies 1 and 2 are two distinct mirrors chosen
from the other disks by the 64 KB block number, so that every
ordered pair of disks occurs and the mirrors spread over disks
1..N-1.  RAID6 survives any 2 failures and so does the metadata.  No
parity, no GF math — read any surviving copy.

`rz_layout` selects the rule (`HAMMER2_RZ_LAYOUT` is 3):

- **Layout 3** (arrays made by the current `newfs_hammer2`):
  `md_ncopies = 3` (arrays of 3 or fewer disks use every disk).
- **Layout 2** (older arrays): a copy on every disk, as before.  It
  mounts and works unchanged.  `vfs.hammer2.md_3copy_upgrade=1` at
  a read-write mount converts it to layout 3: new writes make 3
  copies and the copies on the other disks are never read again.
  Downgrade is not supported: a kernel that predates layout 3
  refuses the array.

### Read, heal, resilver

- **Cold read:** one copy per 64 KB block, round robin over the
  block's copies (§5), so reads use every copy-holding disk.
- **Failed primary:** `hammer2_io_metadata_mirror_read` tries the
  block's copies from the round-robin start, skipping failed or
  closed disks and disks that hold no copy of that block, with a
  second pass for copies still being written.
- **Self-heal:** a metadata block that fails its CHECK is repaired by
  `hammer2_io_metadata_mirror_heal`: it tries the block's other
  copies until one verifies, and queues repairs.  If the bad read
  came from a mirror (spread read), the primary is tried too, not
  skipped, and the bad mirror is repaired as well.
- **Resilver Phase A** (§10) copies, for the replacement, only the
  metadata chunks the freemap has allocated and only those of which
  the target holds a copy (`hammer2_md_is_copy`).

### Why mirror metadata

- Parity-protect metadata would require either spreading the
  bref across multiple disks (would break the per-disk cache
  key + freemap accounting) or going through `write_row` for
  each tiny chain (huge amplification).
- Mirroring is the same model ZFS uses for its `ditto blocks`
  at the metadata level.
- Read-path fallback is trivial: failed primary → try another
  copy.  No reconstruction.
- 3 copies instead of one per disk: a metadata release used to
  write the block to every disk (oct-review P10, §12).

---

## 9. Degraded Read Path

**Function:** `hammer2_io_raid6_read_degraded(hmp, logical_off,
data_disk_idx, buf, bytes, is_physical)`.

Triggered from `_hammer2_io_getblk` when the target DIO's
disk_idx is in `hmp->raid_failed[]`.  `is_physical = 1` for
v3 DATA/DIRENT (the chain machinery hands us a phys_off-encoded
data_off); metadata does not come here but takes
`hammer2_io_metadata_mirror_read` (§8).

Steps:

1. Compute slot, p_disk, q_disk, target_col for `data_disk_idx`.
2. Allocate per-column scratch buffers.
3. `breadnx` every *surviving* disk at `phys_off`.  Disks that
   error during this read get an auto-fail check
   (`hammer2_raid6_auto_fail_disk`) — except when EIO is
   injection-only (`hammer2_inject_eio_disk_mask`).
4. For each failed-or-error column, mark it as failed (data /
   P / Q) in `fail_data_a/b`, `fail_p`, `fail_q`.
5. Dispatch to recovery:
   - One data + P → `datap_recov` using Q.
   - Two data → `2data_recov` using P+Q.
   - One data alone → `dual_recov(faila=data, failb=Q)`
     (recovers via P).
   - Both P+Q failed (data intact) → just use surviving data.
   - Three+ failures → EIO.
6. `bcopy` the reconstructed `target_col` into the caller's
   buffer.

### Failure tracking

```c
int  raid_failed[HAMMER2_MAX_VOLUMES];
int  raid_nfailed;
```

`hammer2_raid6_mark_failed()` is the one place a member is declared
failed: read and write errors, a hung disk, a gone disk, the
fail-disk ioctl and absent disks at mount.  It counts the disk once,
refuses a third failure (`ENXIO`; `HAMMER2_FAIL_FORCE` at mount
overrides) and records `disk_state[i] = FAILED` and the DEGRADED
flag in both `hmp->raid_config` and `hmp->voldata.raid_config`.
Callers that cannot sleep (read completions) pass
`HAMMER2_FAIL_DEFER`: the in-memory state changes at once and a task
persists it.  Closing the device is separate
(`hammer2_raid6_close_member[_locked]`: `VOP_CLOSE` after purging the
vnode; `EAGAIN` while I/O to it is still in the driver, `EBUSY` for
the resilver target), so that the media can be detached.

Ways a member fails:

- **`HAMMER2IOC_RAID_FAIL_DISK`** (`hammer2 raid fail-disk`): fail,
  then close.
- **I/O error:** a failed write to a member calls
  `hammer2_raid6_write_failed()`; a read error in the degraded and
  column read paths auto-fails the disk (not for injection-only EIO).
- **Hung disk (R11):** `vfs.hammer2.io_timeout_ms` (default 30000, 0
  = off).  A once-a-second tick fails a member that has reads
  outstanding on its device and no completion for that long
  (`hammer2_iowd_*`); synchronous writes (P/Q, zero fill, space map,
  flush) are waited for with `hammer2_biowait_timed`, which gives up
  after the timeout, abandons the buffer to its late completion and
  fails the member.  The flush returns `ETIMEDOUT`.  A bio already in
  the driver cannot be taken back; it returns when the driver gives up.
- **Disk gone:** the `disk_gone` event handler (compiled in only under
  `DISK_GONE`, the arm64/Pi config, not in the h2dev x86 guest)
  queues a task that fails the member at once and then closes it
  (`hammer2_disk_gone_hmp`), retrying while writers hold `rebuild_lk`,
  so an idle array notices an unplug and the replugged disk comes
  back under its old name for `raid replace`.  Without a real
  unplug, `vfs.hammer2.inject_gone` runs the same handler on a
  member (group X, X9).  A real USB unplug on the Pi is not yet
  tested.

---

## 10. Resilver

**Kernel:** `hammer2_io_raid6_resilver(hmp, pmp, failed_disk_idx,
new_devvp)`.  **Ioctl:** `HAMMER2IOC_RAID_REPLACE`.  **Cmd:**
`hammer2 raid replace <old> <new>`.

### Phase 1 — volume header

Copy a volume header from a surviving disk to `new_devvp`,
patching `voldata->volu_id = failed_disk_idx`,
`raid_config.disk_state[idx] = ONLINE`, clearing the DEGRADED
flag.  Recompute `ICRC_SECT0` then `ICRC_VOLHEADER` (in that
order) before `bwrite`.  Repeats for every `HAMMER2_NUM_VOLHDRS`
header slot.

### Phase A — metadata zone

For each extent in `hmp->md_extents[]`, and the reserved segment of
every 2 GB zone the extents reach (freemap blocks): bulk
`bread(surviving)` → `bwrite(new_devvp)` in 64 KB chunks, a window of
chunks at a time under exclusive `rebuild_lk`.  Only chunks the
target holds a copy of are copied (`hammer2_md_is_copy`: 3 of N disks
on layout 3, every chunk on layout 2), and with
`vfs.hammer2.resilver_skip_unalloc` (default 1) only the chunks the
freemap has allocated (`hammer2_freemap_md_alloc_map`); a block
allocated from now on is written to the new disk by its flush.  The
source is a surviving copy other than the primary when there is one
(those are written synchronously), else the serial
`hammer2_resilver_md_read` which tries every copy.  This phase is
*much* faster than walking the chain tree for metadata
reconstruction.

### Phase 3 — stripe data

Batched (`hammer2_resilver_pass`: the survivors' columns of a batch
of slots are read together, rebuilt, and written to the new disk).
Schematically:

```
for stripe_num in resume..num_stripes:
    if vfs.hammer2.resilver_skip_unalloc:
        skip if stripe_bitmap[stripe_num] == 0
    skip slots that never hold a row (hammer2_raid6_slot_is_data)
    phys_off = HAMMER2_ZONE_SEG64 + stripe_num * stripe_unit
    p_disk = stripe_num % ndisks
    q_disk = (p_disk + 1) % ndisks
    Read every surviving disk at phys_off; zero the failed col.
    Track second-failure column (dual_recov needs faila/failb).
    Reconstruct via dual_recov.
    bwrite reconstructed col to new_devvp at phys_off.
```

### Resume cursor

The volume header carries a resume record in `raid_config`
(`rz_rb_id`, `rz_rb_disk`, `rz_rb_cursor`, `rz_rb_cwgen`).  Every
`vfs.hammer2.resilver_ckpt_slots` slots (2048) the resilver commits
the cursor with a sync (`hammer2_resilver_ckpt`); slots below it are
on the target.  A later `raid replace` of the same disk onto the same
target resumes at the cursor (`hammer2_resilver_resume_point`) if the
target's own volume header carries the same `rz_rb_id` and no row was
sealed past the failed member since (`rb_wgen` equals `rz_rb_cwgen`).
Any other disk, a wiped one included, starts at slot 0 under a new id.
Phase A is always redone.

### M2 sysctl gate

`vfs.hammer2.resilver_skip_unalloc` (default 1) wraps the
bitmap skip.  Set to 0 to force the baseline (iterate every
slot) — used by `tests/v3/test_d_resilver.sh::D4` to measure
the speedup (43 s vs 2 s at 0.3 % slot fill on the vbd
substrate, ~20×).

### COW + resilver concurrency

HAMMER2's COW design means concurrent writes during resilver
land in **freshly-allocated** slots whose data column may or may
not be on the replacement disk.  If it is, the write goes there
directly via the normal write path; if not, the resilver
doesn't care.  No second-pass dirty-range tracking is needed
(the v1/v2 mechanism that lived here was deleted in Phase 1
Group E — see changelog).

### Post-resilver state

- `hmp->raid_failed[failed_disk_idx] = 0`, `raid_nfailed--`.
- `hmp->volumes[idx].dev->devvp = new_devvp`.
- `disk_state[idx] = ONLINE` in both `raid_config` and
  `voldata.raid_config`.
- DEGRADED flag cleared (in both copies) iff `raid_nfailed == 0`.
- Flushed via `hammer2_vfs_sync(pmp->mp, MNT_WAIT)`.

---

## 11. Scrub (M3)

**Kernel:** `hammer2_io_raid6_scrub(hmp, pmp, flags)` in `hammer2_io.c`.
**Ioctls:** `HAMMER2IOC_RAID_SCRUB` (blocking) and
`HAMMER2IOC_RAID_SCRUB_STATUS` (poll).  **Cmd:** `hammer2 raid
scrub`.  **Tests:** `tests/v3/test_k_scrub.sh`.

### Walker

`hammer2_scrub_walk(ctx, parent)` recursively follows
`hammer2_chain_scan(parent, ..., HAMMER2_LOOKUP_ALWAYS)` like
the bitmap walker, but only recurses into interior types
(INODE / INDIRECT / VOLUME / FREEMAP / FREEMAP_NODE).  Leaves
(DATA / DIRENT) get verified in place.

### Per-bref verify

```c
hammer2_io_bread(hmp, bref->type, bref->data_off, lsize,
                 &dio, bref);
bdata = hammer2_io_data(dio, bref->data_off);
if (hammer2_scrub_check_match(bref, bdata, lsize))
    /* OK — putblk and continue */
```

`hammer2_scrub_check_match` recomputes the CHECK code per
`bref.methods` (XXHASH64 / ISCSI32 / FREEMAP) and compares to
the stored value.  SHA192 returns "match" without verifying —
the SHA headers aren't pulled in by `hammer2_io.c` and DATA
chains use XXHASH64 by default; revisit if SHA verification
becomes important.

### Repair

On CHECK FAIL the scrub calls the read-path self-heal helper,
`hammer2_raid6_selfheal_data(hmp, bref, bdata, lsize, disk_idx)`: P/Q
reconstruction of the bref's column verified against the bref CHECK
code (retrying with a second erasure for a second bad column), with
the corrected bytes queued to the repair kthread.  The scrub drops
its dio, waits for the queue to drain
(`hammer2_io_repair_drain`) and only then counts the chain repaired;
a repair dropped from a full queue counts as unrepairable.  Repairs
never go through the DIO write path (see §15).  The scrub reads each
block uncached (`ctx->rbuf`), so it does not fill the buffer cache.

### Parity pass (S6)

After the tree walk, `hammer2_scrub_parity` reads every allocated row
on all members in slot order, batched, recomputes P and Q and
compares.  A mismatch is re-verified under `rebuild_lk`, localised
with the syndrome (a single bad column) and repaired through the
repair queue (P/Q are rewritten from the data; a data column only if
every chain the fix changes verifies against its CHECK code); rows it
cannot localise count as `parity_unlocatable`.
It runs by default; `raid scrub -t` skips it (checksum walk only)
and `-p` skips the tree walk (parity pass only).  A degraded array
or a running rebuild skips the parity pass.  A scrub syncs first so
the rows it examines are sealed and on the media.  Tests: group Y.

### Concurrency model

Scrub operates on a **chain snapshot** taken via
`hammer2_chain_bulksnap(hmp)` — the snapshot points at the
on-disk root from the last TXG flush (`hmp->volsync`).  Live
writes proceed against the real `hmp->vchain` unimpeded;
chains created since the last sync aren't covered by *this*
scrub (the next one picks them up).

The walker uses `HAMMER2_LOOKUP_NODATA | HAMMER2_LOOKUP_SHARED`
on `hammer2_chain_scan` and the bulkfree-style
unlock-recurse-relock pattern: before descending into a child
chain it drops both `parent` and `child` SHARED locks, recurses
(which re-locks at every level), and reacquires both on return.
Writers that need EXCLUSIVE on a chain we already walked past
proceed without waiting for the full scrub.

### Serialisation

`atomic_cmpset_int` on `hmp->scrub_running` (volatile int)
rejects a second scrub with EBUSY.

### Counters

```c
volatile uint64_t scrub_brefs_done;
volatile uint64_t scrub_brefs_bad;
volatile uint64_t scrub_brefs_repaired;
volatile uint64_t scrub_brefs_unrepairable;
volatile int      scrub_running;
int               scrub_error;
```

Updated every bref so `HAMMER2IOC_RAID_SCRUB_STATUS` from
another thread sees live progress.  The parity pass adds
`scrub_rows_checked`, `scrub_parity_bad`, `scrub_parity_repaired`,
`scrub_parity_unlocatable` and `scrub_rows_skipped`.

---

## 12. Mount-Time Initialization

**File:** `hammer2_vfsops.c::hammer2_vfs_mount`.

For v3 RAID6 volumes (after `hammer2_verify_volumes_3` passes):

```
1. hmp->total_size = voldata->raid_config.array_size.
2. hmp->raid_config  = voldata->raid_config.
3. hmp->raid_type    = RAID_TYPE_RAID6.
4. memset(hmp->raid_failed, 0, ...).  Then for each i,
   if disk_state[i] == FAILED: raid_failed[i] = 1; raid_nfailed++;
5. hammer2_init_volumes(): assign each disk to hmp->volumes[]
   based on its on-disk volu_id (mount-command order
   irrelevant).
6. Metadata copies: if rz_layout == 2 and vfs.hammer2.md_3copy_upgrade
   and the mount is RW, rz_layout = 3.  hmp->md_ncopies =
   hammer2_md_ncopies(ndisks, rz_layout).
7. Load the metadata-zone extents (voldata.raid_config.md_extents
   into hmp->md_extents[]).
8. hammer2_raid6_sm_init(hmp) (a volume with an unsupported rz_layout
   is refused), then hammer2_raid6_sm_read(hmp) (§7):
   - on success: the space map is loaded from the first disks whose
     pages match their CRCs, the bitmap derived.
   - on a page bad on every disk: hmp->stripe_bitmap_invalid = 1.
9. If stripe_bitmap_invalid: hammer2_raid6_rebuild_stripe_bitmap(hmp)
   walks hmp->vchain; if that fails the RW mount is refused (RO stays
   allowed).
10. Mount completes RW (or RO under -uall I-group test scenarios).
```

At module load, `hammer2_vfs_init` calls `hammer2_raid6_init` to
populate the GF tables.  A plain unmount is refused (EBUSY) while a
resilver or scrub runs; a forced one stops it (`raid_abort`).  At
unmount queued failure records are persisted and the queued metadata
mirror copies are flushed, then the usual final sync.  No parity thread to stop, no DTL
to persist.

---

## 13. newfs_hammer2

**Files:** `src/sbin/local_newfs_hammer2.c`,
`src/sbin/local_mkfs_hammer2.{c,h}`.

### Usage

```sh
newfs_hammer2 -R 6 -L LABEL /dev/vbd1 /dev/vbd2 /dev/vbd3 /dev/vbd4
```

`-R 6` selects RAID6.  All listed devices must be the same size
(or `min_size` rules).  newfs writes the volume headers, the
RAID config sector, the initial stripe-bitmap zone (zeros + valid
header), the metadata zone extents, and the empty freemap.

### Space map initialization

`mkfs_hammer2.c` (`raid6_layout`, `format_raid6_sm_copy`) lays out
the per-disk geometry (`docs/capacity.md`), writes both space map
copies, empty and with valid headers and page CRCs, to every disk
(`rz_sm_gen = 1`), and records `rz_layout = HAMMER2_RZ_LAYOUT` (3),
`rz_num_slots`, `rz_sm_*` and metadata extent 0 in the RAID config.
Without a valid space map the first mount would walk the whole
(empty) chain tree to rebuild it.  The environment variable
`HAMMER2_MKFS_LAYOUT2` makes a layout 2 array (all-disk metadata
mirrors) for tests (group Z, Z6).

### array_size

```c
array_size = (uint64_t)ndata * (min_size - HAMMER2_ZONE_SEG);
```

Subtracts the volume-header zone per disk.

---

## 14. Userspace `hammer2 raid` Command

**File:** `src/sbin/local_cmd_raid.c`.

| Subcommand | Ioctl | Purpose |
|---|---|---|
| `raid status [path]` | `HAMMER2IOC_RAID_RESILVER_STATUS` | resilver progress + per-disk state |
| `raid fail-disk <dev>` | `HAMMER2IOC_RAID_FAIL_DISK` | mark a disk failed (so it can be detached) |
| `raid replace <old> <new>` | `HAMMER2IOC_RAID_REPLACE` | online resilver, then a full scrub (the resilver does not verify checksums) |
| `raid scrub [-n] [-t \| -p]` | `HAMMER2IOC_RAID_SCRUB` (+ `_STATUS` for `-n`) | tree walk (verify, self-heal repair) then the row-ordered P/Q parity pass; `-t` tree walk only, `-p` parity pass only (progress polling under `-n`) |

The same-path-twice form of `replace` (`hammer2 raid replace
/dev/vbdN /dev/vbdN`) is the "reattach the same slot" case used
by tests after `fresh_disk()` re-zeros the failed device.

`raid scrub [-n]` flag: `-n` runs the scrub in a forked child
(blocking ioctl in the child) while the parent polls
`HAMMER2IOC_RAID_SCRUB_STATUS` once a second and prints live
bref progress.  Without `-n`, the command blocks until the
scrub completes.  Exit code is 0 only when the scrub had no error,
**all** bad chains were repaired (`brefs_unrepairable == 0`) and no
parity mismatch was left unlocated (`parity_unlocatable == 0`).

---

## 15. Key Invariants and Pitfalls

### `bp = NULL` before every `breadnx`

`breadnx` checks `*bpp` and reuses it if non-NULL, skipping
`getblk`.  In loops over disks/slots, reset `bp = NULL` before
each call.  Failure produces use-after-free of an already-brelse'd
pointer.  Affects every multi-disk read loop in `hammer2_io.c`
(resilver Phase 3, read_degraded, metadata_mirror_read).

### bref.copyid must be a real disk index

Every DATA/DIRENT chain must have `bref.copyid` set to a valid
disk index *before* any I/O is issued for the chain.  The
allocator sets it at `stripe_alloc` time; downstream code
(`hammer2_dio_key`, `read_degraded`, scrub) trusts it
implicitly via `KKASSERT(disk_idx >= 0 && disk_idx <
nvolumes)`.

### Seal-time zero-fill of unused data cols

`hammer2_raid6_seal_row_locked_to_unlocked` must zero-write
every data-disk column whose bit is NOT set in `r->alloc_mask`
before calling `write_row`.  Otherwise stale on-disk bytes
break parity reconstruction.  Crucially, do NOT zero cols
whose `alloc_mask` bit IS set but `col_data` is NULL — the
chain reserved that disk and will deliver bytes through its
own `bp.bdwrite`; a zero-write here would race and corrupt the
chain's data.  See §6.

### Scrub repair bypasses the DIO write path

`hammer2_io_bwrite` on a v3 DATA/DIRENT DIO routes the payload
through `hammer2_raid6_open_row_add_data` (the open-row
packing tracker).  Scrub repair must NOT use it — write
directly via raw `getblk` + `bwrite` on the failed disk's
`devvp` at `phys_off`.

Also: scrub must `hammer2_io_putblk(&dio)` **before** issuing
the write-side `getblk`.  The read-side dio holds the buf busy
via its internal getblk; a second getblk on the same `(devvp,
pbase)` would deadlock waiting for that ref to drop.  See §11.

### disk_state must be written to both `raid_config` and `voldata.raid_config`

Mount restores `hmp->raid_config` from `voldata.raid_config`.
Any ioctl that changes disk failure / replacement state must
update **both** copies, then call `hammer2_voldata_modify` so
the change reaches disk on the next TXG flush.  Affects
`hammer2_ioctl_raid_fail_disk` and `hammer2_ioctl_raid_replace`.

### `dual_recov` argument normalization

When one failure is a parity column (index `>= ndata`) and the
other is data (`< ndata`), the parity index must be in `faila`.
`dual_recov` normalizes at the top with a swap; callers can
pass either order.  The resilver `data_disk_idx` rotation
puts parity in `failb` for some slots, hitting this case.

### V-chain / F-chain refs at unmount are normal

```
hammer2: v-chain 1 refs 1 (warning)
hammer2: f-chain 2 refs 1 (warning)
```

Diagnostic only.  Printed by `hammer2_vfsops.c` from upstream
HAMMER2 — pre-existing, not RAID6-specific.

### Module reload after VM reboot

`./deploy.sh fast` does kldunload+kldload of the in-memory
module, but `/boot/kernel/hammer2.ko` is what the boot loader
picks up at reboot.  After every VM reboot, re-run `deploy.sh
fast` (or `deploy.sh install`) before testing — otherwise the
running module is the boot-time one, missing recent changes.

---

## 16. Testing

All test scripts live under `tests/v3/`.  Run individually or
through `run_all.sh`:

```sh
ssh h2dev 'cd /root/hammer2-tests/v3 && sh run_all.sh'         # default groups
ssh h2dev 'cd /root/hammer2-tests/v3 && sh run_all.sh K'       # one group
ssh h2dev 'cd /root/hammer2-tests/v3 && NDISKS=6 sh run_all.sh'
```

Groups currently in the suite (`run_all.sh`; "default" = in the list
that runs with no arguments):

| Group | Default | Coverage |
|-------|---------|----------|
| A | yes | basic RW, COW slot uniqueness, bref encoding |
| B | yes | single-disk fail at every position (cold reads after garbling the disk) |
| C | yes | C(NDISKS,2) dual-disk fail pairs |
| D | yes | resilver basic, sequential, write-during-resilver, **D4 bitmap-skip timing (M2)** |
| E | yes | EIO injection — a surviving-column error surfaces cleanly (or is recovered as a second erasure); metadata failover to mirrors |
| F | yes | COW invariant + snapshot COW, `h2stripe_check` parity |
| G | yes | absent-disk degraded mount + fail-state persistence |
| H | yes | snapshots under healthy + degraded |
| I | yes | forced unmount (full flush) + bulkfree afterwards; the power-loss case is `crash_host.sh` |
| J | yes | packed-row delete + snapshot pinning (M1/6C-6D) |
| K | yes | **scrub (M3)** — K1 clean, K2 corrupt + parity-repair, K3 concurrent writes during scrub |
| L | yes | read-path self-heal: corrupt a column while unmounted, read, and the data is healed in memory and on disk; metadata copy trust (L4), refused-mount unwind (L6) |
| N | yes | capacity and freeing: fill past 2 GB per disk, delete, two bulkfree passes (`docs/capacity.md`) |
| P | yes | I/O shape with devstat (`h2devstat`): no array reads on a write, even per-disk write bytes, write amplification bounds, P5 space targets |
| Q | yes | 4000 small files on sub-column allocation: remove half, bulkfree, degraded reads, one corrupt sub-block, same-TXG overwrite, churn counters |
| S | yes | split volume headers: a crash part-way through the header loop; quorum and rollback |
| T | yes | disks reused from an earlier filesystem, refused mount |
| U | yes | chains at a non-zero offset inside their column (test hook `raid6_subcol_test`): cold read, self-heal, scrub, degraded, resilver |
| V | yes | sub-column allocation (`raid6_subcol_max`): layout on the media, close at sync, rewrite/grow, self-heal, degraded |
| W | yes | write-side fault injection (`inject_wio_disk_mask`, `inject_delay_ms_disk_mask`, `inject_hold_ms`): write EIO on one/two disks, slow disk within `io_timeout_ms`, hung disk (R11), late completions |
| X | yes | lifecycle: umount and `umount -f` during resilver/scrub, ENOSPC (X4/X4b), `raid6_simd` cross-check, resilver resume cursor (X7/X8), disk gone under load (X9) |
| Y | yes | row-ordered parity scrub: P/Q/data/zero-fill column corruption, `-t`/`-p`, concurrent writes, degraded skip |
| Z | yes | metadata on 3 copies: placement, any 2 failures, heal from the third copy / the primary, resilver, layout 2 and `md_3copy_upgrade` |
| M | yes, last | silent corruption in more than one column (two erasures); runs last, can hang the guest |
| R | **no** (opt-in) | rewrite after a row seal (R5 repro), then `h2stripe_check`; `sh run_all.sh R`. Not in `GROUPS` in `run_all.sh`; on a kernel without the R4/R5 fixes it panics the guest |

The default list is `A B C D E F G H I J K L N P Q S T U V W X Y Z M`.
Name groups to run only those (`sh run_all.sh A B R`).

Substrate is virtio-blk (`/dev/vbd*`); see `docs/workflow.md` for
the harness details.  No vn-backed runs are supported.

### Unit tests

`src/diag/h2raid6_test.c` (run it on the VM from `/root/h2diag`) verifies GF(2^8) math against a reference table — ~135 k
tests covering every dual-recov combination.

### EIO injection

`vfs.hammer2.inject_eio_disk_mask` (sysctl) forces `EIO` on
`breadnx` calls against any disk whose bit is set, without
auto-failing the disk (unless `inject_eio_autofail`).  Used by Group E
to test clean error surfacing.

Write side and hung disks (groups W and X): `inject_wio_disk_mask`
completes hammer2's writes to the masked disks with EIO;
`inject_delay_ms_disk_mask` / `inject_delay_ms` delay its reads and
synchronous writes; `inject_hold_ms_disk_mask` / `inject_hold_ms`
complete bios late, after the waiter gave up; `inject_gone` runs the
`disk_gone` handler on a member.

---

## 17. Outstanding / Future Work

See `docs/outstanding.md` for the full list.  RAID6-specific
items:

- **Async P/Q sealing.**  `write_row` starts P and Q together but
  the caller waits for them.  An earlier `bawrite` attempt
  deadlocked the buffer cache under virtio-blk; a throttled
  per-mount seal path is the plan (blocked by `read_sibling`'s raw
  fallback; see `docs/outstanding.md`).
- **DTL (Dirty Time Log).**  After a fail-disk + replace, the
  current resilver re-syncs every live slot.  ZFS's DTL would
  let us re-sync only slots written *while* the disk was down.
  Needs a small on-disk DTL zone and bookkeeping at stripe
  alloc/free.
- **TRIM on row free.**  Issue `BUF_CMD_DISCARD` for the data +
  P + Q disks when a slot's refcount drops to 0.  Not visible
  on the vbd substrate; defer to Phase 3 SSD hardware for
  verification.
