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

**`main.c`** — Added `raid` subcommand dispatch and usage text

### Phase 8: Testing (Not Yet Implemented)

Planned test strategy:
1. **Unit test GF(2^8) math**: Standalone userspace test verifying `gf_mul(a,b) * gf_inv(b) = a` and syndrome generation + recovery roundtrip for all 2-failure combinations
2. **Functional test on VM**: Create 4 virtual disks, format with RAID 6, mount, write files, verify checksums, simulate 1 and 2 disk failures, verify reads succeed
3. **Stress test**: Heavy write workload during degraded operation

---

## Summary of Changes

### New Files (3)

| File | Description |
|------|-------------|
| `sys/vfs/hammer2/hammer2_raid6.h` | GF(2^8) header: table declarations, inline mul/div/pow, syndrome/recovery prototypes |
| `sys/vfs/hammer2/hammer2_raid6.c` | GF(2^8) math library: table init, syndrome generation (P+Q), all recovery modes |
| `sbin/hammer2/cmd_raid.c` | Userspace `hammer2 raid status` command |

### Modified Files (12)

| File | Changes |
|------|---------|
| `sys/vfs/hammer2/hammer2_disk.h` | `hammer2_raid_config_t`, version 3 constant, RAID type defines, anonymous union in volume header |
| `sys/vfs/hammer2/hammer2.h` | RAID state fields in `hammer2_dev_t`, RAID 6 function prototypes |
| `sys/vfs/hammer2/hammer2_ondisk.c` | `hammer2_verify_volumes_3()`, version dispatch update, `hammer2_raid6_map()` |
| `sys/vfs/hammer2/hammer2_io.c` | RAID-aware DIO allocation, `hammer2_io_raid6_write()`, `hammer2_io_raid6_read_degraded()` |
| `sys/vfs/hammer2/hammer2_vfsops.c` | Mount-time RAID config loading, reduced total\_size, `hammer2_raid6_init()` call |
| `sys/vfs/hammer2/Makefile` | Added `hammer2_raid6.c` to SRCS |
| `sbin/newfs_hammer2/mkfs_hammer2.h` | Added `RaidType` to mkfs options |
| `sbin/newfs_hammer2/newfs_hammer2.c` | `-R` option, RAID 6 validation (min 4 disks) |
| `sbin/newfs_hammer2/mkfs_hammer2.c` | RAID config in volume header, adjusted total\_size for RAID 6 |
| `sbin/hammer2/main.c` | `raid` subcommand dispatch, usage text |
| `sbin/hammer2/hammer2.h` | `cmd_raid()` prototype |
| `sbin/hammer2/Makefile` | Added `cmd_raid.c` to SRCS |

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

Format a RAID 6 filesystem:
```
newfs_hammer2 -R 6 -L DATA /dev/da0 /dev/da1 /dev/da2 /dev/da3
```

Check RAID status:
```
hammer2 raid status /dev/da0
```

### Future Work

- Disk replacement and rebuild (`hammer2 raid replace`, `hammer2 raid rebuild`)
- SIMD-optimized syndrome generation (SSE2/AVX2)
- Stripe-aware freemap allocation (allocate full stripes at once for better write efficiency)
- Hot spare support
- Scrubbing (background parity verification and repair)
