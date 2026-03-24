# HAMMER2 RAID6 — Physical Disk Deployment: Issues and Tasks

Analysis of the changes required when moving from vn devices (virtual disks
backed by UFS files) to real physical block devices. The current implementation
was developed and tested exclusively on vn devices; this document identifies
every known difference in behavior and categorizes the work required.

---

## What Goes Away

**The runningbufspace deadlock (Fix 15) disappears entirely.** There is no UFS
intermediate layer on physical disks. A `bwrite` on a physical disk bio completes
when the disk controller acknowledges the write — the buffer is immediately
retired from `runningbufspace`. The `hammer2_flush_vn_backing` call becomes
either a no-op or a real ATA `FLUSH CACHE` command (see item 5 below).

---

## Data-Loss Risks

These must be resolved before deploying on real hardware.

### 1. Undetected I/O Errors on Surviving Disks

**Severity**: Data loss (silent wrong reconstruction)
**Effort**: Medium

On vn devices, disk failure is always explicit — someone calls
`hammer2 raid fail-disk`. A vn device either works or is detached; there is no
middle ground.

Physical disks can return `EIO` at any time on any I/O to any surviving disk,
without prior notice. The degraded read path (`hammer2_io_raid6_read_degraded`)
reads all surviving columns and passes them to `dual_recov`. If one of those
surviving reads returns `EIO`, the buffer fed to the reconstruction function
contains garbage — and `dual_recov` will produce a wrong answer silently. The
code currently has no logic to detect "we received EIO from a disk that is not
in `raid_failed[]`; this is now a triple failure and the result is
unrecoverable."

The resilver path has the same exposure: `breadnx` on a surviving column during
Phase 2 or 3 could silently fail. The reconstructed data written to the
replacement disk would then be wrong, with no indication anything went wrong.

**Required change**: Every `breadnx` call in the RAID6 paths
(`hammer2_io_raid6_read_degraded`, `hammer2_io_raid6_write`,
`hammer2_io_raid6_resilver`) must check `bp->b_error` after the call. An
unexpected `EIO` on a non-failed disk must be treated as a new failure event:
immediately mark that disk as failed in `hmp->raid_failed[]`, increment
`raid_nfailed`, and return `EIO` to the caller rather than passing garbage
buffers to reconstruction.

---

### 2. Drive Naming Instability Across Reboots

**Severity**: Data loss (wrong disk mapped to wrong array slot)
**Status**: Partially resolved — `volu_id`-based slot assignment already
implemented; `volu_id < ndisks` bounds check added.

DragonFlyBSD assigns `da0`, `da1`, etc. in CAM probe order, which is not
guaranteed to be stable across reboots. If a disk is removed and reinserted,
or if the bus is rescanned, what was `da2` may come back as `da3`.

The current mount command is positional:
```sh
mount -t hammer2 /dev/da0:/dev/da1:/dev/da2:/dev/da3@LABEL /mnt
```

**Already implemented**: `hammer2_init_volumes` uses each disk's `volu_id`
field (stored in the volume header) to assign it to the correct
`hmp->volumes[]` slot — the order of devices in the mount command is
irrelevant. A disk that appears as `da3` after a reboot but has `volu_id=2`
in its header is correctly placed in slot 2.

**Remaining gap (now fixed)**: `hammer2_verify_volumes_3()` previously only
checked `volu_id < HAMMER2_MAX_VOLUMES` but not `volu_id < ndisks`. A disk
with an out-of-range `volu_id` would have been silently assigned to a slot
beyond the array size. This bounds check is now present.

**Remaining gap (deferred)**: Store a persistent array UUID in the volume
header alongside `volu_id`. At mount time, verify both the UUID (all disks
belong to the same array) and the `volu_id` (each disk is in the right slot).
This allows the mount code to auto-detect the correct ordering from an
unordered device list, similar to how `mdadm --assemble --scan` works.

---

### 3. Volume Header Written Only to Disk 0

**Severity**: Data loss (stale config after disk 0 failure)
**Status**: Already implemented — the flush path writes the volume header to
all open devices.

The flush code in `hammer2_flush.c` already iterates over `hmp->devvpl`
(all open device vnodes) with a `TAILQ_FOREACH` loop, writing the updated
volume header to every open, non-failed disk on every sync. For each
non-root disk, the code patches `volu_id` in the copy and recomputes both
CRCs (`ICRC_SECT0` and `ICRC_VOLHEADER`) before calling `bwrite`.

If disk 0 fails and is replaced, the replacement disk receives a correctly
constructed header during resilver Phase 1, and all surviving disks already
have up-to-date headers from the last flush. Remounting from any surviving
disk will see the current `disk_state[]`.

---

## Behavioral Changes

These items change in character but are not immediate correctness risks.

### 4. Hot-Swap Device Identity

**Severity**: Operational
**Effort**: Low (API change)

The current resilver API is:
```sh
hammer2 -s /mnt raid replace /dev/da2 /dev/da2
```

This assumes the replacement disk appears at the same device node as the
failed one. On a physical system, when a failed disk is pulled and a new one
inserted, the new disk may appear as a different device node (e.g., `/dev/da4`
instead of `/dev/da2`).

The `replace` ioctl as currently designed has no way to say "array slot 2 is
now occupied by `/dev/da4`." The old device path no longer exists (the disk was
removed), so opening it to get the array slot index is not possible.

**Required change**: The `replace` ioctl and its userspace command need to
accept either an array slot index or the old device path (for the "which slot"
argument), plus the new device path (for "what to resilver onto") as
independent arguments. The new device must be opened, verified as blank or
belonging to a different array, before the resilver begins.

---

### 5. BUF_CMD_FLUSH Now Issues a Real Disk Flush

**Severity**: Performance (degraded mode only)
**Effort**: Low (parallelize the loop)

On vn devices, `hammer2_flush_vn_backing` submits a `BUF_CMD_FLUSH` bio to
each vn volume, which triggers `VOP_FSYNC` on the UFS backing file — a
software operation. On a physical disk, the same bio causes the disk driver to
issue an ATA `FLUSH CACHE` (`0xE7`) or SCSI `SYNCHRONIZE CACHE` command,
forcing the disk's volatile write cache to commit to persistent storage.

This is correct and desirable behavior for durability. However, `FLUSH CACHE`
on a spinning HDD can take 50–100ms. The current implementation issues flushes
sequentially — one disk at a time. For an N-disk array in degraded mode, this
adds N × 100ms of synchronous stall at the end of every `sync(2)`.

On SSDs the per-flush latency is ~1ms, making this less urgent.

**Required change**: Issue all N `BUF_CMD_FLUSH` bios simultaneously (without
the `BIO_SYNC` flag initially), then `biowait` on all N in a second pass. This
reduces the stall from O(N × latency) to O(1 × latency).

---

### 6. Synchronous Degraded Writes Are Slow on Spinning Disks

**Severity**: Performance (degraded mode on HDDs)
**Effort**: High

On vn devices, `bwrite` in degraded mode was "synchronous to UFS page cache"
— essentially a memory copy plus a buffer handoff, taking microseconds. On
spinning HDDs, each `bwrite` is a real disk operation: rotational latency +
seek + transfer, typically 5–15ms. Writing one stripe in degraded mode makes 3
synchronous disk writes (data + P + Q) before returning to the caller:

```
~10ms × 3 writes/stripe × 64KB/stripe = ~2 MB/s maximum degraded write throughput
```

For SSDs (~0.3ms per write), degraded throughput is ~60 MB/s, which is
acceptable. For HDDs, 2 MB/s is severe.

The synchronous writes are required for correctness: async writes in degraded
mode reintroduce the parity race where a reconstruction read may see stale P/Q
before the async write completes. The only correct fix is a per-stripe lock (as
md RAID uses) or a write-intent bitmap — both significantly more complex than
the current two-pass resilver approach (see Section 8 of `DEVELOPER.md` for a
detailed comparison).

**Short-term mitigation**: For SSD deployments, accept the current behavior.
For HDD deployments, consider limiting the write rate in degraded mode at the
application layer, or issuing data/P/Q bios in parallel rather than sequentially
(data write, then P write, then Q write; fire all three, then wait for all
three). This does not eliminate the correctness concern but reduces wall-clock
time by overlapping the three disk writes.

**Long-term fix**: Per-stripe locking or a forward checkpoint (the md RAID
approach). This is the largest single piece of unfinished work for HDD
deployments.

---

### 7. Bawrite in Healthy Mode Is Now Genuinely Parallel

**Severity**: Performance improvement (no action needed)

In healthy mode, the background parity thread uses `bawrite` for P/Q writes.
On vn devices, "async" meant queued to UFS page cache, written to the backing
disk later by `buf_daemon` — still serialized through a single ad1 device.

On a physical multi-disk array, `bawrite` on disk N is truly parallel to reads
and writes on disks 0..N-1. The background parity thread can overlap its P/Q
writes with the next stripe's data writes, achieving close to full aggregate
disk bandwidth in healthy mode. No code changes are needed; this is a pure
improvement.

---

## Operational Issues

### 8. Automatic Disk Failure Detection

**Severity**: Operational
**Effort**: Medium

On vn devices, the kernel detects a detached device immediately (the vnode is
invalidated, the next I/O returns `ENXIO`). On physical disks, a failing drive
may respond slowly rather than returning immediate `EIO`. The current code has
no path for the disk driver to notify HAMMER2 that a device is degraded —
there is no equivalent of the device-failure callbacks that enterprise storage
stacks receive from CAM or SMART daemons.

Without automatic failure detection, a disk that starts returning intermittent
errors requires manual intervention (`hammer2 raid fail-disk`) before the
RAID6 degraded paths engage. Until that happens, reads to the affected disk
return wrong data (or EIO) rather than being reconstructed from parity.

**Minimum viable approach**: Define an error threshold in the `breadnx` paths —
e.g., N consecutive `EIO`s from the same disk index trigger the equivalent of
`fail-disk` automatically, setting `hmp->raid_failed[i] = 1` and logging to
`dmesg`. This requires a per-disk error counter in `hammer2_dev_t`.

**Better approach**: Register a CAM async callback for `AC_LOST_DEVICE` to
detect physical disk removal, and integrate with `smartd` to act on
SMART pre-failure thresholds before data errors occur.

---

### 9. Disk I/O Timeouts Block the Entire Array

**Severity**: Availability
**Effort**: Medium

On vn devices, I/O completes in microseconds (memory copy). A hung vn device
is not a realistic scenario. On physical disks, a hung (but not yet failed)
drive can hold a `breadnx` call for the full CAM timeout period — often 30
seconds for SCSI/SAS, 20–30 seconds for SATA.

During that timeout, any HAMMER2 operation that needs data from that disk
stalls. Because the degraded read path reads *all* columns before calling
`dual_recov`, even reads for blocks that are not on the hung disk will stall
if that disk is included in the column set for that stripe.

**Required change**: Add a shorter I/O timeout in the RAID6 read paths. If a
bio submitted via `breadnx` does not complete within a configurable threshold
(e.g., 5 seconds), cancel it, treat the disk as failed (same path as item 1
above), and retry the read via degraded reconstruction. This requires using
`dev_dstrategy` with a `bio_done` callback and a callout timer, rather than
the synchronous `breadnx` interface.

---

### 10. TRIM/Discard Support

**Severity**: SSD longevity and performance over time
**Effort**: High

When HAMMER2's freemap reclaims logical blocks, the corresponding physical
columns (data + P + Q) on the SSDs should be TRIMmed so the SSD's internal
garbage collector can reclaim those cells. The current code has no TRIM path
at all.

TRIM in a RAID6 context is not trivial:
- A TRIM for logical offset X maps to a specific (disk, physical offset) via
  `hammer2_raid6_map`, plus the corresponding P and Q columns.
- TRIMming a data column does not simplify the parity (the stripe may still be
  live if other data columns are in use). Full-stripe TRIM requires all data
  columns in the stripe to be freed simultaneously — which HAMMER2's freemap
  does not currently track at stripe granularity.
- Sending per-column TRIM commands to individual SSDs is correct but requires
  hooking into the freemap's block-release path.

**Practical approach**: Implement a background scrub that periodically scans
the freemap for free ranges and issues TRIM commands for the corresponding
physical columns. This avoids needing to modify the hot I/O path and handles
the stripe-granularity problem by processing free ranges that are already
computed by the freemap.

---

## Summary Table

| # | Issue | Severity | Status |
|---|-------|----------|--------|
| 1 | EIO on surviving disk → silent wrong reconstruction | **Data loss** | **Fixed** (auto-fail) |
| 2 | Drive naming instability across reboots | **Data loss** | **Largely resolved** (volu_id assignment + bounds check; UUID deferred) |
| 3 | Volume header only written to disk 0 | **Data loss** | **Already implemented** |
| 4 | Hot-swap device identity (API mismatch) | Operational | Deferred |
| 5 | BUF_CMD_FLUSH is sequential across disks | Performance | Deferred |
| 6 | Synchronous degraded writes slow on HDDs | Performance | Deferred |
| 7 | Healthy-mode bawrite is now genuinely parallel | Improvement | None needed |
| 8 | No automatic disk failure detection | Operational | **Addressed by item 1** |
| 9 | I/O timeout blocks entire array | Availability | Deferred |
| 10 | No TRIM/discard support | SSD longevity | Deferred |

Items 1–3 were the data-loss prerequisites for production deployment; all three
are now resolved or confirmed implemented. Items 6, 9, and 10 are required for
HDD deployments. Items 4, 5, and 10 can be deferred to a follow-on release.
