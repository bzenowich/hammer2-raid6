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
7.  [Stripe Bitmap Zone](#7-stripe-bitmap-zone)
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
   │                      N-way mirror)
   │
  hammer2_raid6.c      ← GF math, write_row (packed seal),
   │                     metadata mirror writes
   │
  hammer2_ondisk.c     ← stripe-slot allocator, stripe bitmap,
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
- **Metadata is N-way mirrored**, not parity-protected — INODE /
  INDIRECT / FREEMAP_NODE / FREEMAP_LEAF / DIRENT chains land on
  the same per-disk byte offset on *every* disk.  Reads pick any
  surviving sibling.  See §8.
- **In-memory stripe bitmap** — one bit per stripe slot, persisted
  to a dedicated zone on disk 0; if invalid at mount the H4-deep
  walker rebuilds it from the chain tree.  See §7.
- **Synchronous P/Q on seal** — `hammer2_io_raid6_write_row` issues
  `bwrite` (not `bawrite`) for P and Q.  The seal_row caller also
  zero-writes any data-disk column not in the row's `alloc_mask`
  before invoking write_row (see §6).  An earlier async P/Q
  attempt deadlocked the buffer cache under virtio-blk load;
  revisit if Phase 3 hardware perf needs it.
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
    └─ scans bitmap for a free slot; opens a new row entry.
  Chain bytes flow through hammer2_io_putblk →
    hammer2_raid6_open_row_add_data → tracker buffers them.

  At seal (n_alloc==ndata OR TXG flush):
    hammer2_io_raid6_write_row(hmp, phys_off, cols[], ncols, bytes)
      For d in data disks not in cols[]:  bwrite(zeros)
      P = XOR(cols[].data)
      Q = sum(gf_coeff_i * cols[i].data)
      bwrite(P), bwrite(Q)

WRITE INODE/INDIRECT/FREEMAP_*
  hammer2_io_putblk (metadata branch):
    bwrite/bdwrite/cluster_write to primary disk,
    then hammer2_io_metadata_mirror_write to surviving siblings
    at the same per-disk byte offset.
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
0                              HAMMER2_ZONE_SEG (4 MB)
│  Volume Header ×4            │  Stripe bitmap zone (slot 41)
│                              │  Metadata zone extents
│                              │  Stripe data (slot HAMMER2_STRIPE_RAID6_START
│                              │              = 1024 = byte 68 MB)
```

- Slot 0..40 of each `HAMMER2_ZONE_SEG`-multiple offset → reserved
  for volume headers + freemap zones (HAMMER2 pre-existing).
- Slot 41 (byte 164 MB) → on-disk stripe bitmap (`hammer2_ondisk.c`
  `hammer2_raid6_bitmap_read/write`).  Lives on disk 0 only.
- Stripe data starts at slot `HAMMER2_STRIPE_RAID6_START = 1024`,
  byte offset `HAMMER2_ZONE_SEG64 + 1024 * stripe_unit = 68 MB`.

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
  the metadata zone (see §8).  All disks hold the same bytes at
  the same offset.
- `bref.copyid` is the originating disk index (set at write
  time); reads tolerate any surviving sibling.

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
3. Otherwise normal `breadnx` on `dio->devvp`.

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

- Zero-writes any data-disk column not represented in `cols[]`.
  Without this, parity reconstruction on a later read/scrub
  yields wrong data — see §6 *Partial-row parity fix* and §15.
- Allocates P / Q bufs (one per surviving parity disk; failed
  disks are skipped).
- XOR-accumulates into P; GF-multiply-accumulates into Q with
  per-column coefficient `gf_exp[my_col % 255]` where `my_col`
  counts data-disk positions less than `col->disk_idx`.
- Synchronous `bwrite` on the P and Q buffers.

### Write path (metadata)

`_hammer2_io_putblk` on a dirty metadata DIO under v3 RAID6:

1. Capture the bp's bytes into a kmalloc'd `md_mirror_data`
   buffer.
2. Dispose the bp on the primary disk (normal bdwrite path).
3. Call `hammer2_io_metadata_mirror_write(hmp, primary_disk,
   per_disk_off, md_mirror_data, psize)` to write the same
   bytes to *every* other surviving disk at the same per-disk
   byte offset.

Reads use `hammer2_io_metadata_mirror_read` — try the primary,
fall back to any surviving sibling on failure.  Metadata
reconstruction does not need GF math.

---

## 6. Open-Row Packing (M1/6C)

**Files:** `hammer2_ondisk.c` (`open_rows[]`, allocator), plus
`hammer2_raid6.c::hammer2_io_raid6_write_row`.

### In-memory row tracker

```c
#define HAMMER2_OPEN_ROWS_MAX 16
struct hammer2_open_row {
    hammer2_off_t phys_off;     /* row's slot byte offset */
    uint8_t       n_alloc;      /* reserved data-col count */
    uint8_t       n_data;       /* col bytes received */
    int           disk[ndata];  /* per-col disk_idx */
    char         *data[ndata];  /* col buffers (kmalloc'd) */
    /* in_use, generation, etc. */
};
```

`hmp->open_rows[HAMMER2_OPEN_ROWS_MAX]` is per-mount.  Memory
ceiling = MAX × ndata × stripe_unit (2 MB at NDISKS=4).

### Allocator (`hammer2_raid6_stripe_alloc`)

Under the bitmap spinlock:

1. `hammer2_raid6_open_row_pack_locked` — try to place the new
   chain in an existing open row with a free data-disk slot.
   On hit, sets `chain->bref.data_off = pack_off | radix` and
   `chain->bref.copyid = pack_disk_idx`, returns 0.
2. Otherwise scan the stripe bitmap from `stripe_cursor` for a
   free slot, open a new `open_row` entry, claim the first data
   disk.  Returns 0; sets bref likewise.

Sysctl bisect knob: `vfs.hammer2.raid6_pack_open_rows` (default
1) — set 0 to force single-chain-per-row while keeping the
deferred-P/Q seal hook.

### Sealing

`hammer2_raid6_seal_all_open_rows` (called from
`hammer2_flush.c` at every TXG commit):

- For each `open_row` with `in_use`, calls
  `hammer2_raid6_seal_row_locked_to_unlocked` which builds
  `cols[]` from the row's tracker and invokes `write_row`.
- After write_row returns, the row entry is freed and its data
  buffers kfree'd.

A row also seals from inside the allocator when packing fills it
(`n_alloc == ndata` *and* all data cols have received their
chain bytes).

### Partial-row parity fix (seal-time zero-fill)

If a row seals with `ncols < ndata` (TXG flush before the row
fills, or `n_alloc < ndata` because open_rows[] capped out),
P/Q is computed assuming the unwritten data cols are zero.
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

### Per-row refcount (M1/6D)

`hmp->stripe_row_refcount[num_slots]` — one byte per row,
tracks how many live DATA/DIRENT chains share each row.  The
stripe bitmap bit is the union (set iff refcount > 0).  Pre-6C
the bitmap *was* the per-row refcount (single-chain rows); 6C
needs both — refcount drives stripe_free's "last chain leaving"
decision; bitmap drives the allocator's "is this slot free".

In-memory only, rebuilt at mount when the bitmap is valid
(`hammer2_raid6_rebuild_row_refcount`) by walking the chain
tree and `++`-ing per-bref.  When the bitmap is invalid the
H4-deep walker (`hammer2_raid6_rebuild_stripe_bitmap`) also
populates the refcount as it goes.

---

## 7. Stripe Bitmap Zone

**Spec:** `docs/stripe_bitmap.md`.  **Implementation:**
`hammer2_ondisk.c` (`hammer2_raid6_bitmap_read`,
`hammer2_raid6_bitmap_write`,
`hammer2_raid6_rebuild_stripe_bitmap`).

### Purpose

The bitmap is the v3 allocator's "is slot S free?" oracle.  One
bit per stripe slot; set iff at least one live DATA/DIRENT chain
occupies a data column in that slot.

### Layout

Single 64 KB on-disk block at zone slot
`HAMMER2_ZONE_RAID6_BITMAP = 41` (byte 164 MB) of **disk 0
only** — the bitmap is not mirrored.  If disk 0 is the failed
disk at mount time, the H4-deep walker rebuilds the bitmap from
the chain tree.

```
page 0:                          header (magic, version, crc, size)
pages 1..bitmap_pages:           bitmap data (1 bit/slot)
final page:                      footer (crc + magic)
```

CRCs cover header + bitmap + footer with both CRC fields zeroed.

### Persistence

`hammer2_raid6_bitmap_write` is called from the TXG commit path
in `hammer2_flush.c` whenever the bitmap is dirty.  The
`stripe_generation` counter bumps every flush so debugging tools
can detect torn writes.

### Mount-time validity check

`hammer2_raid6_bitmap_read` validates header magic, footer
magic, CRC, and size.  On any mismatch — torn write, corrupt
header, disk 0 absent at mount — it sets
`hmp->stripe_bitmap_invalid = 1` and zeros the bitmap.  The
mount path then runs `hammer2_raid6_rebuild_stripe_bitmap`
before allowing RW operations.

---

## 8. Metadata Mirror Zone

**Spec:** `docs/metadata_zone.md`.  **Implementation:**
`hammer2_raid6.c` (`metadata_mirror_write`, `metadata_mirror_read`),
`hammer2_ondisk.c` (extent tracking via `hmp->md_extents[]`).

INODE / INDIRECT / FREEMAP_NODE / FREEMAP_LEAF / DIRENT chains
are **N-way mirrored** across every disk at the same per-disk
byte offset.  No parity, no GF math — read any surviving sibling.

### Why mirror metadata

- Parity-protect metadata would require either spreading the
  bref across multiple disks (would break the per-disk cache
  key + freemap accounting) or going through `write_row` for
  each tiny chain (huge amplification).
- Mirroring is the same model ZFS uses for its `ditto blocks`
  at the metadata level.
- Read-path fallback is trivial: failed primary → try any
  sibling.  No reconstruction.

### Resilver implications

The resilver's Phase A copies metadata zone extents in bulk
(`bread`/`bwrite` page-aligned 64 KB chunks) from any surviving
disk to the replacement.  No stripe walk; orders of magnitude
faster than reconstructing every metadata chain via GF math.

---

## 9. Degraded Read Path

**Function:** `hammer2_io_raid6_read_degraded(hmp, logical_off,
data_disk_idx, buf, bytes, is_physical)`.

Triggered from `_hammer2_io_getblk` when the target DIO's
disk_idx is in `hmp->raid_failed[]`.  `is_physical = 1` for
v3 DATA/DIRENT (the chain machinery hands us a phys_off-encoded
data_off); 0 for metadata (which falls through to
`metadata_mirror_read`).

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

`HAMMER2IOC_RAID_FAIL_DISK` sets `raid_failed[i] = 1`, persists
`disk_state[i] = FAILED` to **both** `hmp->raid_config` and
`hmp->voldata.raid_config`, then `VOP_CLOSE`s the device so the
underlying media can be detached.

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

For each extent in `hmp->md_extents[]`: bulk
`bread(surviving)` → `bcopy` → `bwrite(new_devvp)` in 64 KB
chunks.  Metadata is N-way mirrored so any surviving disk's copy
is authoritative.  This phase is *much* faster than walking the
chain tree for metadata reconstruction.

### Phase 3 — stripe data

```
for stripe_num in 0..num_stripes:
    if vfs.hammer2.resilver_skip_unalloc:
        skip if stripe_bitmap[stripe_num] == 0
    phys_off = HAMMER2_ZONE_SEG64 + stripe_num * stripe_unit
    p_disk = stripe_num % ndisks
    q_disk = (p_disk + 1) % ndisks
    Read every surviving disk at phys_off; zero the failed col.
    Track second-failure column (dual_recov needs faila/failb).
    Reconstruct via dual_recov.
    bwrite reconstructed col to new_devvp at phys_off.
```

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

**Kernel:** `hammer2_io_raid6_scrub(hmp)` in `hammer2_io.c`.
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

### Parity repair

On CHECK FAIL:

1. `recon = kmalloc(stripe_unit)`.
2. `hammer2_io_raid6_read_degraded(hmp, lbase, bref->copyid,
   recon, stripe_unit, /*is_physical=*/1)` — treats the bref's
   primary disk as failed and reconstructs the full
   stripe_unit-sized column from P+Q.
3. `hammer2_scrub_check_match(bref, recon + offset, lsize)` —
   if the reconstruction's CHECK matches the bref, parity wins.
4. **Release the read-side dio first** with
   `hammer2_io_putblk(&dio)` — the dio's internal getblk holds
   the buf busy on `(devvp, pbase)`; without this release the
   write-side getblk in step 5 deadlocks waiting for the ref to
   drop.
5. **Direct write** via raw `getblk(devvp, phys_off,
   stripe_unit) + bcopy(recon) + bwrite`.  This bypasses
   `hammer2_io_bwrite` because the v3 putblk path for
   DATA/DIRENT btypes hands the payload to
   `hammer2_raid6_open_row_add_data` — i.e. a "repair" via the
   DIO write API would land in an open packed-row tracker with
   `ncols=1` and at seal would zero the surviving sibling data
   column, silently corrupting the row.

### Serialisation

`atomic_cmpset_int` on `hmp->scrub_running` (volatile int)
rejects a second scrub with EBUSY.  The walker holds
`hmp->vchain` `RESOLVE_ALWAYS` for the full walk — concurrent
FS writes that need to traverse vchain will serialise with the
scrub.  A snapshot-based version (`hammer2_chain_bulksnap` +
`RESOLVE_SHARED` + bulkfree-style unlock-recurse-relock)
deadlocked on first attempt and is left as a follow-up.

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
another thread sees live progress.

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
6. hammer2_raid6_load_md_extents(): pull metadata-zone extents
   from voldata into hmp->md_extents[].
7. hammer2_raid6_bitmap_read(hmp):
   - on success: hmp->stripe_bitmap loaded from disk 0.
   - on torn-write / missing disk0 / CRC fail:
     hmp->stripe_bitmap_invalid = 1; bitmap zeroed.
8. If stripe_bitmap_invalid:
     hammer2_raid6_rebuild_stripe_bitmap(hmp)
   else:
     hammer2_raid6_rebuild_row_refcount(hmp)
   (Both walk hmp->vchain; only the second skips data leaves.)
9. Mount completes RW (or RO under -uall I-group test scenarios).
```

At module load, `hammer2_vfs_init` calls `hammer2_raid6_init` to
populate the GF tables.  At unmount, nothing special — no
parity thread to stop, no DTL to persist.

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

### Stripe bitmap initialization

`format_raid6_stripe_bitmap` (in mkfs_hammer2.c) writes the
on-disk zone slot 41 with an all-zero bitmap wrapped in a valid
header/footer.  Otherwise the first mount would `H4-deep` the
entire (empty) chain tree to "rebuild" it.

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
| `raid replace <old> <new>` | `HAMMER2IOC_RAID_REPLACE` | online resilver |
| `raid scrub` | `HAMMER2IOC_RAID_SCRUB` | walk, verify, parity-repair |

The same-path-twice form of `replace` (`hammer2 raid replace
/dev/vbdN /dev/vbdN`) is the "reattach the same slot" case used
by tests after `fresh_disk()` re-zeros the failed device.

`hammer2 raid scrub`'s exit code is 0 only when **all** bad
chains were repaired (`brefs_unrepairable == 0`).

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
ssh h2dev 'cd /root/hammer2-tests/v3 && sh run_all.sh'         # all groups
ssh h2dev 'cd /root/hammer2-tests/v3 && sh run_all.sh K'       # one group
ssh h2dev 'cd /root/hammer2-tests/v3 && NDISKS=6 sh run_all.sh'
```

Groups currently in the suite:

| Group | Tests | Coverage |
|-------|-------|----------|
| A | 4 | basic RW, COW slot uniqueness, bref encoding |
| B | 6 | single-disk fail at every position |
| C | 6 | C(NDISKS,2) dual-disk fail pairs |
| D | 11 + skip-sysctl regression | resilver basic, sequential, write-during-resilver, **D4 bitmap-skip timing (M2)** |
| E | 4 | EIO injection — surface clean error, no panic |
| F | 4 | COW invariant + snapshot COW |
| G | 4 | absent-disk degraded mount + fail-state persistence |
| H | 4 | snapshots under healthy + degraded |
| I | 4 | unclean unmount + bulkfree after crash |
| J | 2 | packed-row delete + snapshot pinning (M1/6C-6D) |
| K | 5 | **scrub (M3)** — K1 clean array, K2 corrupt + parity-repair |

Substrate is virtio-blk (`/dev/vbd*`); see `docs/workflow.md` for
the harness details.  No vn-backed runs are supported.

### Unit tests

`src/diag/test_raid6.c` (built by `deploy.sh tests`, runs on the
VM) verifies GF(2^8) math against a reference table — ~135 k
tests covering every dual-recov combination.

### EIO injection

`vfs.hammer2.inject_eio_disk_mask` (sysctl) forces `EIO` on
`breadnx` calls against any disk whose bit is set, without
auto-failing the disk.  Used by Group E to test clean error
surfacing.

---

## 17. Outstanding / Future Work

See `docs/outstanding.md` for the full list.  RAID6-specific
items:

- **Async P/Q writes.**  `write_row` issues synchronous `bwrite`
  for P and Q (and zero-fill cols).  An earlier `bawrite`
  attempt deadlocked the buffer cache under virtio-blk; revisit
  on Phase 3 hardware where the throttling story differs.
- **DTL (Dirty Time Log).**  After a fail-disk + replace, the
  current resilver re-syncs every live slot.  ZFS's DTL would
  let us re-sync only slots written *while* the disk was down.
  Needs a small on-disk DTL zone and bookkeeping at stripe
  alloc/free.
- **TRIM on row free.**  Issue `BUF_CMD_DISCARD` for the data +
  P + Q disks when a slot's refcount drops to 0.  Not visible
  on the vbd substrate; defer to Phase 3 SSD hardware for
  verification.
- **Concurrent-write scrub.**  Switch the scrub walker to a
  `hammer2_chain_bulksnap` snapshot + SHARED locks +
  bulkfree-style unlock-recurse-relock so live writes proceed
  while scrub runs.  Earlier attempt deadlocked the IPI
  machinery.
- **Persist `stripe_row_refcount`.**  The mount walker rebuilds
  refcount in O(metadata) time on every mount.  Persisting a
  byte-per-slot table alongside the on-disk bitmap would skip
  the walk; the cost is bitmap-zone size (`num_slots` bytes).
- **`raid scrub` progress polling.**  `HAMMER2IOC_RAID_SCRUB_STATUS`
  is wired but the userspace command only calls the blocking
  variant.  Adding a `-n` polling mode is a few lines.
