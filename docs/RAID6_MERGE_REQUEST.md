# Add RAID 6 (Dual-Parity Striping) Support to HAMMER2

## Context

DragonFlyBSD's HAMMER2 filesystem currently supports only single-volume and JBOD-style multi-volume (linear concatenation) configurations. The DESIGN document mentions RAID 1 (copies) as a future feature, but it was never implemented. There is no parity-based redundancy at all.

This change adds RAID 6 (dual-parity striping) support, allowing any 2 disks to fail without data loss. Minimum 4 disks required (2 data + 2 parity).

## Architecture: I/O Layer Integration

RAID 6 is implemented **inside HAMMER2's I/O and volume layer**, below the freemap but above the raw block devices. This approach was chosen because:

- HAMMER2 already has multi-volume infrastructure (`hammer2_volume_t`, `hammer2_get_volume()`, `hammer2_ondisk.c`)
- The I/O layer (`hammer2_io.c`) already abstracts physical device access via `hammer2_io_t` (DIO) structures
- The freemap sees a single logical address space — RAID 6 striping is transparent to it
- Copy-on-write means writes are always to new locations, simplifying the write path (no read-modify-write for partial stripes)
- This avoids needing a separate block device layer (simpler than Linux md-raid)

The freemap manages a **reduced** logical address space (total\_size minus parity overhead). The I/O layer transparently maps logical offsets to striped physical locations with P and Q parity.

## Reference Implementation

The GF(2^8) math uses the standard Reed-Solomon algebra (clean BSD-licensed implementation):

- **Polynomial**: x^8 + x^4 + x^3 + x^2 + 1 (0x11d, reduction constant 0x1d)
- **P parity**: XOR of all data blocks (same as RAID 5)
- **Q parity**: Reed-Solomon syndrome using GF(2^8) multiplication by powers of generator 2
- **Recovery**: Solve 2-equation system using GF(2^8) inverse/multiply tables

---

## Implementation Plan (8 Phases)

### Phase 1: GF(2^8) Math Library

Created a self-contained, BSD-licensed GF(2^8) arithmetic module in `hammer2_raid6.c` / `hammer2_raid6.h`:

- `hammer2_gf_mul_table[256][256]` — precomputed full multiplication table
- `hammer2_gf_exp[256]` — powers of generator (2)
- `hammer2_gf_log[256]` — discrete logarithm base 2
- `hammer2_gf_inv[256]` — multiplicative inverses
- `hammer2_raid6_init()` — computes all tables at module load time

Core functions:
- `hammer2_raid6_gen_syndrome()` — generate P and Q parity for a stripe using Horner's method
- `hammer2_raid6_2data_recov()` — recover 2 failed data disks using P and Q
- `hammer2_raid6_datap_recov()` — recover 1 data disk + P using Q
- `hammer2_raid6_dual_recov()` — router dispatching to the correct recovery function

Syndrome generation inner loop (portable integer version):
```
for each byte position d:
    wp = wq = data[highest_disk][d]
    for z = highest_disk-1 down to 0:
        wd = data[z][d]
        wp ^= wd                    // P accumulation
        wq = gf_mul2(wq) ^ wd      // Q accumulation
    P[d] = wp
    Q[d] = wq
```

Where `gf_mul2(x) = (x << 1) ^ ((x & 0x80) ? 0x1d : 0)`.

### Phase 2: On-Disk Format Changes

Added to `hammer2_disk.h`:

- `HAMMER2_VOL_VERSION_RAID6` (version 3)
- `HAMMER2_RAID_TYPE_JBOD` (0) and `HAMMER2_RAID_TYPE_RAID6` (6)
- `hammer2_raid_config_t` struct (exactly 512 bytes, packed):

```c
struct hammer2_raid_config {
    uint8_t  raid_type;         // 0=JBOD, 6=RAID6
    uint8_t  ndisks;            // total disks in array
    uint8_t  ndata;             // number of data disks (ndisks - 2)
    uint8_t  stripe_shift;      // log2(stripe_unit), default 16 (64KB)
    uint32_t flags;             // RAID flags (degraded, rebuilding, etc.)
    uint64_t stripe_unit;       // stripe unit in bytes (default 64KB)
    uint64_t array_size;        // usable data size after parity overhead
    uint8_t  disk_state[HAMMER2_MAX_VOLUMES]; // per-disk state
    uint8_t  reserved[424];     // pad to 512 bytes
};
```

Stored in the volume header at sector3 (0x0600-0x07FF) via an anonymous union, maintaining backwards compatibility with pre-RAID6 volumes.

**Address space mapping (left-symmetric layout):**
```
stripe_number = logical_offset / (ndata * stripe_unit)
column        = (logical_offset / stripe_unit) % ndata
P disk        = stripe_number % ndisks
Q disk        = (stripe_number + 1) % ndisks
Data columns fill remaining disk slots in order
```

The stripe unit of 64KB aligns naturally with HAMMER2's `HAMMER2_PBUFSIZE`, meaning one DIO = one stripe unit.

### Phase 3: Volume Management Changes

**`hammer2.h`** — Added to `hammer2_dev_t`:
```c
hammer2_raid_config_t raid_config;  // RAID configuration
int     raid_type;                  // 0=JBOD, 6=RAID6
int     raid_failed[HAMMER2_MAX_VOLUMES]; // failed disk tracking
int     raid_nfailed;               // count of failed disks
```

**`hammer2_ondisk.c`**:
- `hammer2_verify_volumes_3()` — validates RAID 6 volumes: correct raid\_type, minimum 4 disks, consistent ndata/ndisks, stripe\_unit matches PBUFSIZE
- Updated `hammer2_verify_volumes()` dispatcher to call version-3 verification
- `hammer2_raid6_map()` — maps logical offset to (disk\_index, physical\_offset) using left-symmetric rotation

### Phase 4: I/O Layer Changes

**`hammer2_io.c`** — The most complex change. Made the DIO layer RAID-aware.

**DIO allocation (`hammer2_io_alloc`):**
For RAID 6 volumes, calls `hammer2_raid6_map()` instead of `hammer2_get_volume()` to find the correct physical disk and offset for a given logical address.

**Write path (`hammer2_io_raid6_write`):**
1. Given a logical offset and data for one PBUFSIZE block
2. Identifies the stripe and reads other data columns (or treats unallocated as zeros)
3. Calls `hammer2_raid6_gen_syndrome()` to compute P and Q
4. Writes P and Q to their respective physical disks

HAMMER2's COW nature is a major advantage: every write is to a freshly allocated block, avoiding partial-stripe update complexity.

**Degraded read path (`hammer2_io_raid6_read_degraded`):**
1. Reads all columns in the stripe (data + P + Q)
2. For failed disks, uses zero-filled buffers and tracks failure indices
3. Calls `hammer2_raid6_dual_recov()` to reconstruct missing data
4. Returns the reconstructed target column

**Degraded mode:**
- Failed disks tracked in `hmp->raid_failed[]`
- nfailed <= 2: continue with reconstruction on reads
- nfailed > 2: return EIO

### Phase 5: Freemap Integration

**`hammer2_vfsops.c`** — At mount time:
- For version >= `HAMMER2_VOL_VERSION_RAID6`: sets `hmp->total_size` to `raid_config.array_size` (the reduced logical size after parity overhead)
- Copies RAID config from volume header to in-memory `hmp->raid_config`
- Initializes failed-disk tracking arrays

The freemap operates on the logical address space using `hmp->total_size` as its limit. Since this is set to the reduced size, the freemap naturally limits allocations to the usable data space. The physical striping is handled transparently by the I/O layer below.

Also calls `hammer2_raid6_init()` during `hammer2_vfs_init()` to precompute GF(2^8) tables at module load time.

### Phase 6: newfs\_hammer2 Changes

**`newfs_hammer2.c`**:
- Added `-R` option: `newfs_hammer2 -R 6 /dev/da0 /dev/da1 /dev/da2 /dev/da3`
- Validates minimum 4 devices for RAID 6
- Auto-sets version to `HAMMER2_VOL_VERSION_RAID6`

**`mkfs_hammer2.c`**:
- When RAID 6 is selected, writes `raid_config` to all volume headers
- Sets ndisks, ndata, stripe\_unit (64KB), stripe\_shift (16), array\_size
- Adjusts `total_size` to the logical (usable) size: `ndata * min_disk_size`
- Prints RAID 6 summary during format

**`mkfs_hammer2.h`**:
- Added `RaidType` field to `hammer2_mkfs_options_t`

### Phase 7: hammer2 Userspace Tool Changes

**`cmd_raid.c`** (new):
- `hammer2 raid status <devpath>` — displays RAID type, disk count, stripe unit, array size, flags, and per-disk states (online/failed/rebuilding/spare)
- `hammer2 raid fail-disk <devpath>` — marks a disk as failed (sends `HAMMER2IOC_RAID_FAIL_DISK` ioctl, which calls `VOP_CLOSE` on the device so `vnconfig -u` succeeds)
- `hammer2 raid replace <old_dev> <new_dev>` — triggers online resilver (`HAMMER2IOC_RAID_REPLACE`); blocks until complete

**`main.c`** — Added `raid` subcommand dispatch and usage text

**`subs.c`** — Updated `hammer2_ioctl_handle()` to handle multi-device RAID6 paths
(when `open()` fails on a `:` path, scans `getfsstat()` for a matching
`f_mntfromname` and opens `f_mntonname` instead, so all ioctl commands work with
`/dev/vn0:/dev/vn1:...@PFS`-style paths)

### Phase 8.5: Additional Kernel Changes (Post-Initial-Commit)

The following kernel changes were required to make the implementation correct and
production-worthy; they are included in the final patch:

**`hammer2_ioctl.c`**:
- `HAMMER2IOC_RAID_FAIL_DISK` ioctl (`hammer2_ioctl_raid_fail_disk`): marks disk
  failed in `hmp->raid_failed[]`, calls `VOP_CLOSE` on device vnode so it can
  be unconfigured, sets `DEGRADED` flag in on-disk RAID config
- `HAMMER2IOC_RAID_REPLACE` ioctl (`hammer2_ioctl_raid_replace`): triggers the
  online resilver kernel function `hammer2_io_raid6_resilver()`
- `HAMMER2IOC_RAID_RESILVER_STATUS` ioctl: returns resilver progress (0–100%)
  by reading `hmp->resilver_stripes_done` / `hmp->resilver_stripes_total`

**`hammer2_ioctl.h`**:
- Added `hammer2_ioc_raid_replace_t`, `hammer2_ioc_raid_fail_disk_t`,
  `hammer2_ioc_resilver_status_t` structs
- Added ioctl numbers 98, 99, 100

**`hammer2.h`** — Added to `hammer2_dev_t`:
- `volatile uint64_t resilver_stripes_done` / `resilver_stripes_total` — progress tracking
- `int resilver_disk_idx` — which disk is being resilvered (-1 = none)
- `struct spinlock raid6_parity_spin` — protects parity work queue
- `TAILQ_HEAD(, hammer2_parity_work) raid6_parity_q` — parity work queue
- `thread_t raid6_parity_td` — background parity thread
- `int raid6_parity_exiting` — thread exit flag
- `hammer2_parity_work_t` struct definition (`pbase`, `psize`, `data`)
- `int disk_idx` field in `hammer2_io_t` (replaces `unused01`) for O(1) disk failure checks

**`hammer2_io.c`**:
- `hammer2_io_raid6_write()` — complete stripe parity computation; moved to be
  called from background parity thread (not from DIO lastdrop path, to avoid deadlock)
- `hammer2_io_raid6_read_degraded()` — reconstructs missing column(s) from surviving
  disks; called pre-emptively when target disk is in `hmp->raid_failed[]`
- `hammer2_io_raid6_resilver()` — three-phase resilver: volume header, stripe data,
  parity refresh; uses `hammer2_raid6_dual_recov()` for each stripe
- `hammer2_parity_init()` / `hammer2_parity_uninit()` — background thread lifecycle
- Pre-emptive degraded read in `_hammer2_io_getblk`: if the target disk is already
  in the failed set, reconstruct directly rather than submitting I/O to the device

**`hammer2_ondisk.c`**:
- `hammer2_raid6_map()`: added `HAMMER2_ZONE_SEG64` to `*phys_off` to ensure RAID6
  stripe data starts after the 4MB header zone on each disk
  (without this, stripe 0 maps to physical offset 0, overwriting volume headers)

### Phase 8: Testing (Complete)

All tests implemented and passing in the VM environment:

1. **Unit test GF(2^8) math** (`test_raid6.c`):
   - 135,457/135,457 tests pass
   - Tests: `gf_mul(a,b) * gf_inv(b) == a` for all non-zero pairs, syndrome
     generation + recovery roundtrip for all single and dual failure combinations
   - Run: `cc -o test_raid6 test_raid6.c hammer2_raid6.c && ./test_raid6`

2. **Test A** — Parallel write + SHA-256 verify:
   - 4 parallel workers each write 32MB of random data
   - Hashes verified before and after: **PASS**

3. **Test B** — Single disk failure, degraded read:
   - Write data, mark disk failed via `hammer2 raid fail-disk`, verify reads in
     degraded mode (disk still attached), then detach via `vnconfig -u` and verify
     reads continue: **PASS**

4. **Test C** — Dual disk failure, dual-degraded read:
   - Extends Test B: marks a second disk failed, verifies reads with both disks
     failed and detached: **PASS**

5. **Test D** — Online resilver + remount integrity:
   - Write data, fail disk, attach fresh image to replacement vnode, run online
     resilver (`hammer2 raid replace`), verify data integrity, unmount and remount,
     verify data integrity again: **PASS**

6. **Stress test** — Parallel I/O during degraded operation:
   - Concurrent writes and reads while in degraded mode: **PASS**

Test scripts: `tests/test_b.sh`, `tests/test_c.sh`, `tests/test_d.sh`,
`tests/test_stress.sh`, `tests/test_resilver.sh`, `tests/verify_parity.sh`

---

## Summary of Changes

### New Files (4)

| File | Description |
|------|-------------|
| `sys/vfs/hammer2/hammer2_raid6.h` | GF(2^8) header: table declarations, inline mul/div/pow, syndrome/recovery prototypes |
| `sys/vfs/hammer2/hammer2_raid6.c` | GF(2^8) math library: table init, syndrome generation (P+Q), all recovery modes |
| `sys/vfs/hammer2/test_raid6.c` | Standalone userspace unit test: 135,457 GF math + syndrome/recovery tests |
| `sbin/hammer2/cmd_raid.c` | Userspace `hammer2 raid status/fail-disk/replace` commands |

### Modified Files (14)

| File | Changes |
|------|---------|
| `sys/vfs/hammer2/hammer2_disk.h` | `hammer2_raid_config_t`, version 3 constant, RAID type/state/flag defines, anonymous union in volume header |
| `sys/vfs/hammer2/hammer2.h` | RAID state fields and parity thread fields in `hammer2_dev_t`; `disk_idx` in `hammer2_io_t`; `hammer2_parity_work_t`; all RAID6 function prototypes |
| `sys/vfs/hammer2/hammer2_ondisk.c` | `hammer2_verify_volumes_3()`, version dispatch update, `hammer2_raid6_map()` (with ZONE_SEG offset) |
| `sys/vfs/hammer2/hammer2_io.c` | RAID-aware DIO allocation; `hammer2_io_raid6_write()`; `hammer2_io_raid6_read_degraded()`; `hammer2_io_raid6_resilver()`; `hammer2_parity_init/uninit()`; background parity thread; pre-emptive degraded read in `_hammer2_io_getblk` |
| `sys/vfs/hammer2/hammer2_ioctl.c` | `HAMMER2IOC_RAID_FAIL_DISK`, `HAMMER2IOC_RAID_REPLACE`, `HAMMER2IOC_RAID_RESILVER_STATUS` ioctls |
| `sys/vfs/hammer2/hammer2_ioctl.h` | New ioctl structs and ioctl numbers 98–100 |
| `sys/vfs/hammer2/hammer2_vfsops.c` | Mount-time RAID config loading, reduced total\_size, `hammer2_raid6_init()` + `hammer2_parity_init()` calls; `hammer2_parity_uninit()` at unmount |
| `sys/vfs/hammer2/Makefile` | Added `hammer2_raid6.c` to SRCS |
| `sbin/newfs_hammer2/mkfs_hammer2.h` | Added `RaidType` to mkfs options |
| `sbin/newfs_hammer2/newfs_hammer2.c` | `-R` option, RAID 6 validation (min 4 disks) |
| `sbin/newfs_hammer2/mkfs_hammer2.c` | RAID config in volume header; `format_raid6_pwrite()` with full parity recompute; `array_size` calculation with ZONE_SEG offset |
| `sbin/hammer2/main.c` | `raid` subcommand dispatch, usage text |
| `sbin/hammer2/hammer2.h` | `cmd_raid()` prototype |
| `sbin/hammer2/Makefile` | Added `cmd_raid.c` to SRCS |
| `sbin/hammer2/subs.c` | Multi-device path handling in `hammer2_ioctl_handle()` |

### Key Design Decisions

- **Stripe unit = 64KB** (`HAMMER2_PBUFSIZE`), aligning naturally with DIO buffers — one DIO maps to exactly one stripe column
- **Left-symmetric** P/Q rotation across stripes for even wear distribution
- **Freemap sees reduced logical space** — `total_size = ndata * min_disk_size`, striping is transparent
- **COW-friendly** — new writes compute full stripe parity; no read-modify-write needed for partial stripes
- **On-disk format** uses existing reserved sector3 space via anonymous union (fully backwards compatible — old volumes have sector3 zeroed, new volumes overlay the RAID config)
- **Version 3** on-disk format — existing version 1 (single volume) and version 2 (multi-volume JBOD) continue to work unchanged
- **Module-load table init** — GF(2^8) tables computed once at `hammer2_vfs_init()`, used for all subsequent operations

### Backwards Compatibility

- Volumes formatted without RAID 6 (versions 1 and 2) are completely unaffected
- The anonymous union in the volume header means the `sector3` field name is still accessible for any code that might reference it (though none currently does)
- `HAMMER2_VOL_VERSION_WIP` is bumped to 4, so older kernels will refuse to mount RAID 6 volumes (correct safety behavior)

### Usage

Format a RAID 6 filesystem (minimum 4 disks):
```sh
newfs_hammer2 -R 6 -L DATA /dev/da0 /dev/da1 /dev/da2 /dev/da3
```

Mount:
```sh
mount -t hammer2 /dev/da0:/dev/da1:/dev/da2:/dev/da3@DATA /mnt/data
```

Check RAID status (no mount required):
```sh
hammer2 raid status /dev/da0
```

Mark a failed disk and detach it:
```sh
hammer2 -s /mnt/data raid fail-disk /dev/da2
vnconfig -u vn2   # on VM; use equivalent on physical hardware
```

Replace a failed disk (online resilver):
```sh
hammer2 -s /mnt/data raid replace /dev/da2 /dev/da4
```

### Known Issues Fixed During Development

| Bug | Symptom | Fix |
|-----|---------|-----|
| ZONE_SEG offset missing from `hammer2_raid6_map()` | Stripe 0 wrote to physical offset 0, corrupting volume headers on remount | Added `HAMMER2_ZONE_SEG64` to `*phys_off` in `hammer2_raid6_map()` and `format_raid6_pwrite()` |
| `bp` not reset before `breadnx` in write path | Fatal Trap 9 GPF in `vn_strategy` (uninitialized `bp` on stack) | Added `bp = NULL` before each `breadnx` call in all three RAID6 I/O functions |
| Parity computed in DIO lastdrop path | Deadlock: `breadnx` for sibling reads blocked because vn device I/O was serialized under write locks | Moved parity computation to background `h2par-<dev>` kernel thread; DIO lastdrop only enqueues work |
| `newfs` parity used delta-update on reused disk images | Stale P/Q from previous session contaminated fresh newfs parity | Changed to full parity recompute: read sibling data, compute P/Q from scratch |
| `vnconfig -u` failed after `raid fail-disk` | `VOP_CLOSE` not called → device still open | Added `VOP_CLOSE` to `hammer2_ioctl_raid_fail_disk()` |
| `bp = NULL` missing in resilver phases 1/2/3 | `panic: brelse` / Fatal Trap 12 | Added `bp = NULL` before every `breadnx` in all three resilver phases |
| `hammer2 raid replace` / ioctl cmds fail on multi-device paths | `open()` rejects `:` in path | `hammer2_ioctl_handle()` in `subs.c` now scans `getfsstat()` for matching mntfromname |

### Future Work

- SIMD-optimized syndrome generation (SSE2/AVX2 for multi-threaded parity at wire speed)
- Stripe-aware freemap allocation (allocate full stripes at once for better sequential write efficiency)
- Hot spare support (auto-resilver on failure detection)
- Background scrubbing (periodic parity verification and silent corruption repair)
- Resilver abort ioctl (`HAMMER2IOC_RAID_RESILVER_ABORT`)
- Non-blocking `hammer2 raid replace --no-wait` with polling via `HAMMER2IOC_RAID_RESILVER_STATUS`
- Multi-array RAID6 (per-volume rather than per-mount config)
- Promote `HAMMER2_VOL_VERSION_RAID6 = 3` to stable release version
