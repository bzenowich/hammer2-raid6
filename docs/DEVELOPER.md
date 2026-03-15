# HAMMER2 RAID6 — Developer Reference

Technical reference for developers maintaining or extending the RAID6 implementation
in DragonFlyBSD's HAMMER2 filesystem. This document covers the on-disk format,
I/O architecture, parity write path, failure/resilver mechanics, and known pitfalls.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [On-Disk Format](#2-on-disk-format)
3. [Address Mapping (Left-Symmetric Layout)](#3-address-mapping-left-symmetric-layout)
4. [GF(2^8) Math Library](#4-gf28-math-library)
5. [I/O Layer (hammer2_io.c)](#5-io-layer)
6. [Background Parity Thread](#6-background-parity-thread)
7. [Degraded Read Path](#7-degraded-read-path)
8. [Resilver (Online Disk Replacement)](#8-resilver-online-disk-replacement)
9. [Volume Management (hammer2_ondisk.c)](#9-volume-management)
10. [Mount-Time Initialization](#10-mount-time-initialization)
11. [newfs_hammer2 Changes](#11-newfs_hammer2-changes)
12. [Userspace Tool Changes (hammer2)](#12-userspace-tool-changes)
13. [Key Invariants and Pitfalls](#13-key-invariants-and-pitfalls)
14. [Testing](#14-testing)
15. [Future Work](#15-future-work)

---

## 1. Architecture Overview

RAID6 is implemented **inside HAMMER2's I/O layer**, below the freemap but above
the raw block devices. This makes it transparent to all upper layers (chain, inode,
freemap). The freemap sees a single logical address space; striping is invisible to it.

```
  User/kernel reads/writes
          │
  hammer2_io.c  ← RAID6 logic lives here
          │
  hammer2_ondisk.c  ← logical→physical mapping (hammer2_raid6_map)
          │
  per-disk vnode I/O (breadnx/bwrite)
          │
  /dev/vn0  /dev/vn1  /dev/vn2  /dev/vn3
```

Key design choices:

- **Stripe unit = 64KB (`HAMMER2_PBUFSIZE`)** — one DIO maps to exactly one stripe column
- **Left-symmetric rotation** — P/Q disk rotates across stripes for even wear
- **COW-friendly** — new writes always go to freshly allocated blocks; no
  read-modify-write for partial stripes
- **Freemap uses reduced logical size** — `total_size = ndata * min_disk_size`
- **ZONE_SEG offset** — physical layout skips first `HAMMER2_ZONE_SEG` (4MB) on each
  disk to avoid clobbering volume headers

---

## 2. On-Disk Format

### Volume Header Changes (`hammer2_disk.h`)

```c
#define HAMMER2_VOL_VERSION_RAID6   3   /* new version for RAID6 volumes */
#define HAMMER2_RAID_TYPE_JBOD      0
#define HAMMER2_RAID_TYPE_RAID6     6

struct hammer2_raid_config {
    uint8_t  raid_type;         /* 0=JBOD, 6=RAID6 */
    uint8_t  ndisks;            /* total disks (data + 2 parity) */
    uint8_t  ndata;             /* data disk count (ndisks - 2) */
    uint8_t  stripe_shift;      /* log2(stripe_unit), default 16 (64KB) */
    uint32_t flags;             /* HAMMER2_RAID6_FLAG_* */
    uint64_t stripe_unit;       /* bytes, default 65536 */
    uint64_t array_size;        /* usable (logical) bytes */
    uint8_t  disk_state[HAMMER2_MAX_VOLUMES]; /* per-disk state */
    uint8_t  reserved[424];     /* pad to 512 bytes */
};
```

The `raid_config` is stored in sector3 of the volume header via an anonymous union,
reusing the `volu_bytes[3]` slot (offset 0x0600–0x07FF). Pre-RAID6 volumes have
this sector zeroed, which is valid because `raid_type=0 = JBOD`. This is backwards
compatible: old kernels can mount old volumes, and new kernels check `raid_type`
before using RAID6 paths.

### Per-Disk States (`disk_state[]`)

```c
#define HAMMER2_RAID6_DISK_ONLINE     0
#define HAMMER2_RAID6_DISK_FAILED     1
#define HAMMER2_RAID6_DISK_REBUILDING 2
#define HAMMER2_RAID6_DISK_SPARE      3
```

### Flags

```c
#define HAMMER2_RAID6_FLAG_DEGRADED   0x0001  /* one or more disks failed */
#define HAMMER2_RAID6_FLAG_REBUILDING 0x0002  /* resilver in progress */
```

### Physical Layout Per Disk

```
Offset 0                    HAMMER2_ZONE_SEG (4MB)
[  Volume Header x4  ][     RAID6 stripe data     ]
```

RAID6 data starts at `HAMMER2_ZONE_SEG` on every disk. `hammer2_raid6_map()` adds
this offset; `format_raid6_pwrite()` in newfs also adds it.

---

## 3. Address Mapping (Left-Symmetric Layout)

```
stripe_num  = logical_off / (ndata * stripe_unit)
col         = (logical_off / stripe_unit) % ndata
p_disk      = stripe_num % ndisks
q_disk      = (stripe_num + 1) % ndisks
data_disks  = remaining disks in order, skipping p_disk and q_disk
```

Implementation: `hammer2_raid6_map()` in `hammer2_ondisk.c`

```c
void
hammer2_raid6_map(hammer2_dev_t *hmp, hammer2_off_t logical_off,
                  int *disk_idx_out, hammer2_off_t *phys_off_out)
{
    /* ... left-symmetric rotation logic ... */
    *phys_off_out += HAMMER2_ZONE_SEG64;  /* skip header zone */
}
```

**Critical**: Both the kernel mapper and `newfs_hammer2` must use identical
left-symmetric formulas or parity/data placement will be inconsistent.

---

## 4. GF(2^8) Math Library

**Files**: `hammer2_raid6.c`, `hammer2_raid6.h`

The irreducible polynomial is `x^8 + x^4 + x^3 + x^2 + 1` (reduction constant `0x1d`).

### Tables (global, computed at module load)

| Table | Size | Purpose |
|-------|------|---------|
| `hammer2_gf_exp[256]` | 256B | Generator powers: `gf_exp[i] = 2^i mod poly` |
| `hammer2_gf_log[256]` | 256B | Discrete log: `gf_log[gf_exp[i]] = i` |
| `hammer2_gf_inv[256]` | 256B | Multiplicative inverse: `gf_inv[x] = x^(-1)` |
| `hammer2_gf_mul_table[256][256]` | 64KB | Full multiplication table |

Call `hammer2_raid6_init()` once at module load (in `hammer2_vfs_init()`).

### Inline helpers (`hammer2_raid6.h`)

```c
/* Multiply by 2 in GF(2^8): shift left, XOR with 0x1d if bit 7 set */
static inline uint8_t hammer2_gf_mul2(uint8_t x) {
    return (x << 1) ^ ((x & 0x80) ? 0x1d : 0);
}

/* General multiply using table lookup */
static inline uint8_t hammer2_gf_mul(uint8_t a, uint8_t b) {
    return hammer2_gf_mul_table[a][b];
}
```

### Syndrome Generation

`hammer2_raid6_gen_syndrome(ndisks, bytes, ptrs)`:
- `ptrs[0..ndata-1]` = data buffers
- `ptrs[ndata]` = P output
- `ptrs[ndata+1]` = Q output

Uses Horner's method starting from the highest-numbered data disk:
```
P = data[0] ^ data[1] ^ ... ^ data[ndata-1]
Q = (...((data[ndata-1] * 2) ^ data[ndata-2]) * 2 ^ ...) ^ data[0]
```

### Recovery Functions

| Function | Failures Handled |
|----------|-----------------|
| `hammer2_raid6_2data_recov` | Two data disks failed |
| `hammer2_raid6_datap_recov` | One data disk + P failed |
| `hammer2_raid6_dual_recov` | Router: dispatches to correct function |

`hammer2_raid6_dual_recov(ndisks, bytes, faila, failb, ptrs)`:
- If both failed are parity → regenerate P+Q
- If one data + Q → recover from P, regenerate Q
- If one data + P → `datap_recov` (uses Q)
- If two data → `2data_recov` (uses P+Q)

---

## 5. I/O Layer

**File**: `hammer2_io.c`

### DIO Allocation (`hammer2_io_alloc`)

For RAID6 volumes, `hammer2_io_alloc()` calls `hammer2_raid6_map()` to translate
the logical offset to `(disk_idx, phys_off)`, then opens the DIO against the
correct physical vnode. The DIO also records `disk_idx` in `dio->disk_idx` for
fast failure checking: if `hmp->raid_failed[disk_idx]` is set, reads are immediately
redirected to the degraded-read path instead of submitting to the device.

### Write Path

HAMMER2's COW design writes every block to a freshly allocated location. Normal
writes do:

1. `_hammer2_io_putblk` is called when a dirty DIO is disposed (lastdrop)
2. A pre-modification copy of the buffer is saved in `dio->raid6_old_data`
   before any modifications (for RMW delta parity computation)
3. Data is copied to `parity_work->data` and the bio is released via `bdwrite`
4. A `hammer2_parity_work_t` item `{pbase, psize, data, old_data}` is enqueued to
   `hmp->raid6_parity_q`
5. The background parity thread dequeues and calls `hammer2_io_raid6_write`

**Degraded-mode exception**: When `hmp->raid_nfailed > 0`, parity is computed
**inline** (synchronously) in `_hammer2_io_putblk` instead of being queued to
the background thread. This avoids a race where flush reads back a block via
degraded reconstruction before the parity thread has updated P/Q.

### `hammer2_io_raid6_write(hmp, logical_off, data, old_data, bytes)`

Uses **RMW delta** parity update (always, not just in degraded mode):

```
P_new = P_old XOR D_old XOR D_new
Q_new = Q_old XOR (gf_coeff * D_old) XOR (gf_coeff * D_new)
```

1. Calls `hammer2_raid6_map()` to find which disk/column this DIO covers
2. Reads P_old and Q_old from their respective disks
3. Computes delta parity using `old_data` (pre-modification) and `data` (new)
4. Writes new P and Q to their physical disks

When `old_data` is available, sibling data columns are **not** read — the delta
formula only needs P_old, Q_old, D_old, and D_new. This significantly reduces
buffer cache pressure during degraded-mode flush.

**Write modes by context**:
- Healthy mode (background thread): `bawrite` for P/Q (async, throughput)
- Degraded mode (inline): `bwrite` for data and P/Q (synchronous, prevents
  runningbufspace deadlock)

**IMPORTANT**: Each `breadnx` call must be preceded by `bp = NULL`. `breadnx` checks
`*bpp` and will reuse a stale/freed pointer if it's non-NULL. This has caused
use-after-free GPFs in the past (see `img/FatalTrap9.png`).

---

## 6. Background Parity Thread

**Problem**: Computing parity requires reading sibling columns, which requires
blocking I/O. If done in the DIO lastdrop path (which may hold locks), the blocking
`breadnx` calls deadlock because the vn device's backing file vnode is locked
exclusively for pending writes.

**Solution**: A per-mount background thread `h2par-<devname>`.

### Lifecycle

```c
hammer2_parity_init(hmp);    /* called at mount time */
hammer2_parity_uninit(hmp);  /* called before closing device vnodes at unmount */
```

`hammer2_parity_uninit` sets `hmp->raid6_parity_exiting = 1`, wakes the thread,
and waits for it to drain the queue and exit.

### Queue Entry

```c
struct hammer2_parity_work {
    TAILQ_ENTRY(hammer2_parity_work) entry;
    hammer2_off_t   pbase;   /* logical offset */
    int             psize;   /* size in bytes */
    char           *data;    /* data buffer (kmalloc'd, freed by thread) */
};
```

Protects the queue with `hmp->raid6_parity_spin` (spinlock). The thread sleeps on
`tsleep(&hmp->raid6_parity_q, ...)` when the queue is empty.

### Why This Avoids Deadlock

`_hammer2_io_putblk` calls `bdwrite(bp)` to release the buffer before enqueueing
the parity work. By the time the parity thread calls `breadnx` for sibling columns,
the buffer holding the data write is no longer locked. The parity thread holds no
filesystem locks — it's a plain kernel thread doing I/O.

**Note**: In degraded mode (`raid_nfailed > 0`), parity is computed inline in
`_hammer2_io_putblk` instead of being queued to the background thread. This ensures
parity is up-to-date before any subsequent degraded-reconstruction read of the same
block. The background thread is still used for healthy-mode writes.

---

## 7. Degraded Read Path

**Function**: `hammer2_io_raid6_read_degraded(hmp, logical_off, buf, bytes)`

Called when `_hammer2_io_getblk` detects the target DIO's disk is in the failed
set (`hmp->raid_failed[disk_idx] != 0`). Pre-empts the normal I/O path.

Algorithm:
1. Read all `ndisks` columns: data columns + P + Q
2. For each failed disk, zero-fill its buffer and record its index
3. Call `hammer2_raid6_dual_recov()` to reconstruct missing columns
4. Copy the reconstructed target column into `buf`

**IMPORTANT**: Use `bp = NULL` before each `breadnx` call inside the loop.

### Failure Tracking

```c
/* In hammer2_dev_t (hmp): */
int raid_failed[HAMMER2_MAX_VOLUMES];  /* 1 = failed, 0 = online */
int raid_nfailed;                       /* count */
int disk_idx;                           /* in hammer2_io_t: which disk */
```

`HAMMER2IOC_RAID_FAIL_DISK` ioctl sets `hmp->raid_failed[disk_idx] = 1` and calls
`VOP_CLOSE` on the device vnode so `vnconfig -u` can succeed.

---

## 8. Resilver (Online Disk Replacement)

**Kernel function**: `hammer2_io_raid6_resilver(hmp, failed_disk_idx, new_devvp)`

**Userspace command**: `hammer2 raid replace <old_dev> <new_dev>`

**Ioctl**: `HAMMER2IOC_RAID_REPLACE` → `hammer2_ioctl_raid_replace()`

### Resilver Phases

**Phase 1 — Volume Header**:
1. Read a volume header from a surviving disk (not the failed one)
2. Patch three fields:
   - `voldata->volu_id = failed_disk_idx`
   - `voldata->raid_config.disk_state[failed_disk_idx] = HAMMER2_RAID6_DISK_ONLINE`
   - `voldata->raid_config.flags &= ~HAMMER2_RAID6_FLAG_DEGRADED`
3. Recompute `ICRC_SECT0` (covers offset 0x3B where `volu_id` lives)
4. Recompute `ICRC_VOLHEADER` (covers the entire header; must be last)
5. Write to the new disk

**Phase 2 — Stripe Data**:
For each stripe in the array:
1. Read all surviving data columns using blocking `breadnx` (`bp = NULL` before each)
2. Reconstruct the missing column using `hammer2_raid6_dual_recov()`
3. Write the reconstructed data to the new disk at the correct physical offset
4. Update `hmp->resilver_stripes_done` for progress reporting

**Phase 3 — Parity Refresh**:
Recompute and write P and Q for each stripe to incorporate the new (recovered) data.
Again, `bp = NULL` before each `breadnx`.

**Progress Reporting**:
`HAMMER2IOC_RAID_RESILVER_STATUS` ioctl reads `hmp->resilver_stripes_done` /
`hmp->resilver_stripes_total` and computes 0–100%.

### Concurrent Writes During Resilver

**The problem**: The resilver reads P/Q from surviving disks using `breadnx`. A
concurrent degraded write updates P/Q in two separate synchronous `bwrite` calls
(data first, then P/Q via `hammer2_io_raid6_write`). If the resilver reads parity
between those two bwrites — or before either, for a freshly allocated stripe where
parity is still zero — it reconstructs stale/zero data and writes it to the
replacement disk.

**Our solution — two-pass resilver with dirty-range tracking**:

1. **Pre-sync** (`hammer2_vfs_sync_pmp(pmp, MNT_WAIT)`) before Phase 3 drains all
   in-flight degraded writes, so parity on surviving disks is current when we start.

2. **Dirty-range tracking** (`hmp->resilver_dirty_lo / resilver_dirty_hi`): whenever
   `_hammer2_io_putblk` completes a degraded parity update while `resilver_running`,
   it records the stripe number. Because HAMMER2's sequential allocator
   (`allocator_beg`) gives new COW blocks increasing stripe numbers, the dirty range
   is small and bounded.

3. **Phase 4 — second pass**: after the main loop, sync again and re-resilver only
   `resilver_dirty_lo..resilver_dirty_hi`. The second pass reads committed parity and
   writes the correct data to the replacement disk.

**Why this works for HAMMER2**: COW semantics guarantee that new writes go to new
physical addresses (new stripes), never overwriting existing stripes. The dirty range
captures the small set of stripes allocated during Phase 3 that may have raced. A
single additional pass after a sync is sufficient.

### Comparison with md RAID

md RAID5/6 solves the same problem with a fundamentally different mechanism.

**md's approach — recovery checkpoint (`recovery_cp`) + stripe cache locking**:

md maintains a `recovery_cp` cursor that divides the array into two regions and
routes concurrent writes differently depending on which side of the checkpoint they
fall:

- **Ahead of checkpoint** (not yet recovered): writes go to surviving disks only.
  The replacement disk is not touched — the resilver will reach that sector later
  and reconstruct it correctly from the then-current parity.
- **Behind checkpoint** (already recovered): writes go to **all** disks including
  the replacement, since the resilver has already written correct data there.

The boundary stripe is handled by a **per-stripe lock in the stripe cache** (md's
in-memory LRU of active RAID stripes). A concurrent write to a stripe being actively
resilvered blocks until the resilver commits that stripe, then the write proceeds to
all disks. No parity-read race is possible.

The `recovery_cp` is also persisted to disk (via the **write intent bitmap**), so a
crash during recovery only requires re-syncing the unprocessed region.

**Comparison table**:

| Aspect | Our approach | md RAID |
|--------|-------------|---------|
| Concurrency control | Pre-sync + dirty-range second pass | Per-stripe lock (stripe cache) |
| Write routing during recovery | Unchanged (always to surviving disks) | Checkpoint-aware (all or surviving only) |
| Correctness guarantee | Bounded — second pass fixes races | Strict — no races possible |
| Handles in-place overwrites | No (relies on COW) | Yes |
| Crash-safe recovery position | No | Yes (write intent bitmap) |
| Complexity | Low | High |

**Why COW lets us use the simpler approach**: in HAMMER2 a write never overwrites
an existing stripe. New data always goes to a new physical address. Concurrent
writes during resilver therefore land on new, high-addressed stripes that the
resilver has not yet reached. The dirty-range second pass catches the small
number that race with Phase 3. For md, which must handle in-place overwrites to
arbitrary stripes, the stripe-cache lock and checkpoint routing are necessary for
correctness.

**Remaining limitation**: Phase 4 does only one additional pass. Under sustained
heavy write load throughout a long resilver, writes could race with Phase 4 itself.
For a correct solution under that workload, the md approach (per-stripe locking or
a forward checkpoint) would be required. For typical HAMMER2 usage (mostly
quiescent during resilver, or writes concentrated at the high end of the allocation
range), Phase 4 is sufficient.

### Post-Resilver State

After resilver completes:
- `hmp->raid_failed[failed_disk_idx] = 0`
- `hmp->raid_nfailed--`
- `hmp->devvp[failed_disk_idx]` points to the new device vnode
- The `DEGRADED` flag is cleared in `hmp->voldata`

---

## 9. Volume Management

**File**: `hammer2_ondisk.c`

### `hammer2_verify_volumes_3()`

Called for version-3 (RAID6) volumes. Validates:
- `raid_type == HAMMER2_RAID_TYPE_RAID6`
- `ndisks >= 4`
- `ndata == ndisks - 2`
- `stripe_unit == HAMMER2_PBUFSIZE` (64KB)
- All disks consistent in config

### `hammer2_raid6_map(hmp, logical_off, disk_idx_out, phys_off_out)`

Translates logical offset → (disk index, physical offset). Adds `HAMMER2_ZONE_SEG64`
to the physical offset to skip the volume header zone.

---

## 10. Mount-Time Initialization

**File**: `hammer2_vfsops.c`

At mount time, for RAID6 volumes:

```c
/* Set logical (usable) size */
hmp->total_size = voldata->raid_config.array_size;

/* Copy RAID config to in-memory struct */
hmp->raid_config = voldata->raid_config;
hmp->raid_type   = voldata->raid_config.raid_type;

/* Initialize failed-disk tracking */
memset(hmp->raid_failed, 0, sizeof(hmp->raid_failed));
hmp->raid_nfailed = 0;

/* Restore any pre-existing failed-disk state from on-disk headers */
for (i = 0; i < ndisks; i++) {
    if (disk_state[i] == HAMMER2_RAID6_DISK_FAILED) {
        hmp->raid_failed[i] = 1;
        hmp->raid_nfailed++;
    }
}

/* Start background parity thread */
hammer2_parity_init(hmp);
```

At module load time, `hammer2_vfs_init()` calls `hammer2_raid6_init()` to precompute
GF(2^8) tables.

At unmount time, `hammer2_parity_uninit(hmp)` is called before closing device vnodes.

---

## 11. newfs_hammer2 Changes

**Files**: `newfs_hammer2.c`, `mkfs_hammer2.c`, `mkfs_hammer2.h`

### Usage

```sh
newfs_hammer2 -R 6 -L LABEL /dev/da0 /dev/da1 /dev/da2 /dev/da3
```

### `format_raid6_pwrite()`

Writes HAMMER2 data to disk images using the left-symmetric RAID6 layout,
computing P and Q parity inline. This ensures that what `newfs` writes is
consistent with what the kernel later reads via `hammer2_raid6_map()`.

**Parity computation uses full recompute** (not delta-update):
1. Read all sibling data columns from disk
2. Compute P and Q from scratch: `P = new_data XOR sibling; Q = GF(new_data) XOR ...`
3. Write P and Q ignoring any existing P/Q on disk

This is correct even when disk images are reused (stale P/Q from a previous session
is always overwritten).

### array_size Calculation

```c
uint64_t array_size = (uint64_t)ndata * (min_size - HAMMER2_ZONE_SEG);
```

`HAMMER2_ZONE_SEG` (4MB) is subtracted per disk to account for the header zone.

---

## 12. Userspace Tool Changes

### `hammer2 raid status <dev>`

Reads the RAID config directly from the on-disk volume header (no mount required).
Displays: raid type, disk count, stripe unit, array size, flags, per-disk states.

```sh
hammer2 raid status /dev/vn0
```

### `hammer2 raid fail-disk <dev>`

Marks a disk as failed via `HAMMER2IOC_RAID_FAIL_DISK` ioctl. The kernel stops
submitting I/O to the device and calls `VOP_CLOSE` so the device can be
unconfigured (`vnconfig -u`).

### `hammer2 raid replace <old_dev> <new_dev>`

Triggers online resilver via `HAMMER2IOC_RAID_REPLACE`. Blocks until resilver
completes. Calls against any mounted path on the RAID6 filesystem.

### Multi-Device Path Handling (`subs.c`)

The userspace `hammer2` tool handles multi-device paths (`/dev/vn0:/dev/vn1:...`)
for ioctl-based commands. When `open()` fails on a `:` path, it scans `getfsstat()`
for a matching `f_mntfromname` and opens `f_mntonname` instead.

---

## 13. Key Invariants and Pitfalls

### `bp = NULL` Before Every `breadnx`

`breadnx` checks `*bpp` and reuses the buffer if it's non-NULL, skipping `getblk`.
**Always** reset `bp = NULL` before each call inside loops. Failure causes:
- First iteration: uninitialized stack pointer → GPF on `bp->b_cmd` access
- Later iterations: use-after-free of post-`brelse` pointer

Affected functions: `hammer2_io_raid6_write`, `hammer2_io_raid6_read_degraded`,
`hammer2_io_raid6_resilver` (all three phases).

### ZONE_SEG Offset in Both Kernel and newfs

Both `hammer2_raid6_map()` and `format_raid6_pwrite()` must add `HAMMER2_ZONE_SEG`
to physical offsets. Missing this in either place causes writes to collide with
volume headers (corrupting them silently).

### vnconfig -u Requires VOP_CLOSE

`hammer2_ioctl_raid_fail_disk` must call `VOP_CLOSE` on the device vnode before
returning. Without this, `vnconfig -u` fails with "device busy".

### Always Use RMW Delta Parity (Not Full Recompute)

When two data columns in the same stripe are modified in the same flush cycle,
reading the sibling's data from disk for full parity recompute returns stale data
(the sibling's `bdwrite` is async and may not have reached disk). The fix is to
**always** use the RMW delta formula: `P_new = P_old XOR D_old XOR D_new`. This
only needs the pre-modification buffer (`old_data`), not sibling data. The
`old_data` buffer is saved in `dio->raid6_old_data` before any modifications.

### Pre-emptive Degraded Check Covers All DIO Ops

The pre-emptive degraded check in `_hammer2_io_getblk` must handle all DIO
operations (READ, NEW, NEWNZ), not just READ. Sub-buffer `DOP_NEW` operations
(e.g., inode creation: 1KB within a 64KB DIO) on a failed disk will call
`breadnx` on a closed vnode, producing I/O errors and garbage `old_data`.

### Synchronous I/O in Degraded Mode Prevents Deadlock

During degraded-mode flush, async `bawrite` for data and parity buffers can
exhaust `runningbufspace` faster than the backing store can drain it. This
causes a circular deadlock: hammer2 flush → `bawrite` → vn driver → UFS →
buffer cache → needs prior `bawrite`s to complete. Fix: use synchronous
`bwrite` for both data and parity in degraded mode (`raid_nfailed > 0` and
`DIO_FLUSH` set).

### Parity Thread Must Exit Before Closing Vnodes

`hammer2_parity_uninit` must be called before closing device vnodes during unmount.
The parity thread accesses device vnodes (`hmp->devvp[i]`) for I/O; if vnodes are
closed first, the thread will panic on subsequent `breadnx` calls.

### Volume Header Written Only to devvp[0]

The kernel's flush (`hammer2_flush.c`) writes volume headers only to `hmp->devvp`
(disk 0). Other disks' headers are not auto-updated. After resilver, the new disk
needs a manually constructed volume header (Phase 1 of the resilver procedure).

### Files on Failed Disk Cannot Be Read After Disk Swap

If data was written when disk N was replaced with a different image (e.g., disk4
vs disk2 in test scenarios), those files' data blocks are on the replacement disk.
Switching back to the original disk makes them unreadable. Delete those files before
switching. The CHECK FAIL diagnostic comes from HAMMER2's per-block CRC, not from
RAID6 parity.

### v-chain/f-chain Messages Are Normal

```
hammer2: v-chain 1 refs 1 (warning)
hammer2: f-chain 2 refs 1 (warning)
```

These are printed at every unmount from `hammer2_vfsops.c:1892-1894`. They are
diagnostic messages, not errors.

---

## 14. Testing

All test scripts are in `tests/mdraid/`:

| Script | What it tests | Status |
|--------|---------------|--------|
| `test_b.sh` | Single disk failure: write data, mark disk failed, verify degraded reads, detach disk | PASS |
| `test_c.sh` | Dual disk failure: extends Test B, marks a second disk failed, verifies dual-degraded reads | PASS |
| `test_d.sh` | Online resilver: format, write, fail disk, resilver to replacement, verify data, unmount+remount | PASS |
| `test_e_write_degraded.sh` | Write during degraded mode: 4 scenarios with single/dual failure and data integrity | 4/4 PASS |
| `test_f_sequential_fail.sh` | Sequential disk failures: fail one, write, fail another, verify all data | 3/3 PASS |
| `test_g_repair_no_destroy.sh` | Repair without data loss (needs h2parity_fix on VM) | Skipped |
| `test_h_mount_missing.sh` | Mount with absent disk scenarios | 3/4 PASS |
| `test_i_write_during_resilver.sh` | Write during active resilver | 2/3 PASS |
| `test_j_unclean_unmount.sh` | Unclean unmount recovery | 3/4 PASS |
| `test_k_concurrent_io.sh` | Concurrent I/O during degraded + resilver, deadlock detection | 6/6 PASS |
| `test_all_fail_combos.sh` | All 4 single-fail + 6 dual-fail read + 6 dual-fail write combinations | 32/32 PASS |
| `test_l_snapshot_compression.sh` | Snapshots during degraded mode, COW with snapshots, LZ4 compression with disk failures | 18/18 PASS |

### Parity Checker (`h2parity_fix`)

Standalone userspace tool compiled and run on the VM. Scans all stripes and
recomputes P/Q from data disks to verify (or fix) parity.

```sh
# Check parity (dry run, no writes):
/var/tmp/h2parity_fix -n /var/tmp/disk0.img /var/tmp/disk1.img \
                         /var/tmp/disk2.img /var/tmp/disk3.img

# Fix parity:
/var/tmp/h2parity_fix /var/tmp/disk0.img /var/tmp/disk1.img \
                       /var/tmp/disk2.img /var/tmp/disk3.img
```

### Unit Tests (`test_raid6.c`)

Located in `sys/vfs/hammer2/test_raid6.c`. A standalone userspace binary that
verifies the GF(2^8) math (135,457/135,457 tests pass). Build and run on the VM:

```sh
cd /usr/src/sys/vfs/hammer2
cc -o test_raid6 test_raid6.c hammer2_raid6.c && ./test_raid6
```

---

## 15. Unimplemented Upstream Features

### `hammer2 volume-add` / `hammer2 volume-del`

As of DragonFlyBSD 6.4.2, these commands **do not exist**. Only `hammer2 volume-list`
is implemented. There is no `HAMMER2IOC_VOLUME_ADD` or `HAMMER2IOC_VOLUME_DEL` ioctl
in the kernel, and no userspace handler in `cmd_volume.c` or `main.c`.

The FlyNAS plan document references these as if they were available — they are
aspirational/planned upstream features that were never implemented.

**If `volume-add` / `volume-del` are ever added**, each handler must include an
early RAID6 guard:

```c
static int
hammer2_ioctl_volume_add(hammer2_inode_t *ip, void *data)
{
    hammer2_dev_t *hmp = ip->pmp->iroot->cluster.focus->hmp;

    if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6) {
        kprintf("hammer2: volume-add not supported on RAID6 arrays; "
                "use 'hammer2 raid replace' to swap a failed disk\n");
        return ENOTSUP;
    }
    /* ... JBOD implementation ... */
}
```

The reason: JBOD `volume-add` is O(1) — it just extends the logical address space.
Adding a disk to a RAID6 array requires a full re-stripe: every block must be read
and rewritten under the new column rotation, and P/Q must be recomputed for every
stripe. This is an O(total-data) operation that should never be silently triggered
by an apparently routine command. Without this guard, a user running `volume-add`
against an array they believed was JBOD (but is actually RAID6) could corrupt the
array or trigger an unexpected multi-hour re-stripe.

The same guard applies to `volume-del`. There is no graceful "remove a disk" in
RAID6 — the only valid operations are `raid fail-disk` (failure path) and a future
`raid shrink` command (capacity reduction, extremely complex, not yet designed).

---

## 16. Future Work

| Item | Notes |
|------|-------|
| SIMD syndrome generation | SSE2/AVX2 optimization for `hammer2_raid6_gen_syndrome` |
| Stripe-aligned allocation | Allocate full stripes at once to avoid partial-stripe parity cost |
| Hot spare support | Designate a spare disk, auto-resilver on failure detection |
| Background scrubbing | Periodic parity verification to catch silent corruption |
| Multiple RAID6 arrays | Per-volume rather than per-mount RAID config |
| HAMMER2 version bump | `HAMMER2_VOL_VERSION_WIP = 4` currently; bump to 3 as stable |
| ioctl for resilver abort | Allow cancelling an in-progress resilver |
| Progress display in resilver | `hammer2 raid replace` currently blocks; add `-n` / polling |
| Pool alias config (`/etc/hammer2.conf`) | Map short names to full device strings so `hammer2 raid status pool0` works instead of typing `/dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST`; add `hammer2 pool-set/del/list` subcommands to manage entries; resolution goes in `hammer2_ioctl_handle()` in `subs.c` as an early lookup step before existing `open()` and `getfsstat()` attempts; applies to JBOD multi-volume paths too, not RAID6-specific |
