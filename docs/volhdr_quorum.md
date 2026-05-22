# HAMMER2 v4 RAIDZ2-native — Volume Header Sequence + Quorum

**Status**: Phase 0 spec. Implementation in Phase 1.
**Cross-refs**: `newplan.md` §5.6, §9.2.

---

## Purpose

The volume header is the **TXG commit point**. It installs the new
blockref tree root after all dirty chains (data + parity + bitmap +
metadata mirrors) have been drained to disk. Crash safety requires:

1. Every disk's volume header carries a strictly increasing **sequence
   number** per TXG.
2. Mount discovers the most-recent volume header that is durable on
   a **majority** of disks.
3. Partial commits (volume header durable on some disks but not a
   majority) are rolled back to the prior TXG.

This matches ZFS uberblock discovery in spirit; the details differ
because HAMMER2 already has dual volume-header copies per disk
(zones 0 and 1).

---

## On-disk fields

The existing HAMMER2 volume header (`hammer2_volume_data`) has these
relevant fields:

- `voldata.mirror_tid` — bumped on every TXG.
- `voldata.icrc_volheader` — CRC over the volume header.
- HAMMER2 already writes the volume header to all N disks in the
  TXG-commit flush loop.

**New for v4** (in the existing raid_config substructure or volhdr
addendum):

```c
uint64_t v4_txg_seq;        /* per-TXG sequence number, monotonic */
uint64_t v4_array_uuid[2];  /* identifies this RAID6 array */
uint8_t  v4_disk_id;        /* this disk's slot in the array (0..N-1) */
uint8_t  v4_ndisks;         /* total disks in the array */
uint8_t  v4_pad[6];
```

`v4_txg_seq` increases by 1 per TXG commit. Initial value (after
`newfs_hammer2 --raid6`) = 1.

`v4_array_uuid` is generated at format time and identical across all
N disks. It distinguishes "this is a disk that belongs to this array"
from "this is a disk with a HAMMER2 header that came from somewhere
else." Detected mismatches at mount: refuse to attach the disk; log a
clear error.

`v4_disk_id` lets a disk identify its column index even when device
names reshuffle between reboots (a known DragonFlyBSD behavior on real
hardware). HAMMER2 already has `volu_id`; this is the v4-aware
equivalent.

---

## TXG commit write order

For each TXG:

1. All chains flush their data + parity to data area.
2. Stripe bitmap header/data/footer written to disk 0.
3. Metadata mirror writes complete (already drained by existing
   VOP_FSYNC loop).
4. `BUF_CMD_FLUSH` per disk — hardware cache flush. (This already
   exists; reuse unconditionally per `newplan.md` §6.)
5. **Volume header write loop**: for each disk i in 0..N−1:
   - Set `voldata.v4_txg_seq = current_txg`.
   - Compute `voldata.icrc_volheader`.
   - `bwrite` (synchronous) to the disk's primary volume header
     zone.
6. Wait for all N writes to complete via biowait. If any write
   returns EIO, the disk is marked failed (auto-fail path); the TXG
   still commits if a majority succeeded.

Step 5 uses synchronous `bwrite` to avoid ambiguity at the commit
boundary. Volume header writes are 64 KB each — the cost is N*64KB
sequential, negligible vs the TXG itself.

The pre-existing dual volume-header pattern (zones 0 and 1) is
retained: each disk gets two copies, written in alternating order
across TXGs (TXG even → zone 0 first, TXG odd → zone 1 first). A
torn write to one zone leaves the other zone intact at the prior
seqno.

---

## Mount-time discovery

```
for each disk in pool:
    read volhdr zone 0 → vh0
    read volhdr zone 1 → vh1
    if vh0 valid and vh1 valid:
        disk_seq[i] = max(vh0.v4_txg_seq, vh1.v4_txg_seq)
        disk_uuid[i] = (the valid header's array_uuid)
    elif vh0 valid:
        disk_seq[i] = vh0.v4_txg_seq
    elif vh1 valid:
        disk_seq[i] = vh1.v4_txg_seq
    else:
        disk_seq[i] = INVALID
        mark disk failed

verify all valid disks share the same v4_array_uuid (reject foreign
disks with EINVAL).

# Sort surviving disks by seqno, descending.
# Find the highest seqno present on a majority (⌈N/2⌉+1).
majority = (N // 2) + 1
for seq in sorted(disk_seq, descending):
    count = number of disks with disk_seq[i] == seq
    if count >= majority:
        mount with TXG = seq, using only disks at this seqno
        return

# No majority for any seqno → unmountable.
return ENXIO
```

For `N = 4`, majority = 3. Two disks can fail; mount survives. Three
disks failed → ENXIO (and also no RAID6 recovery possible).

For `N = 6`, majority = 4. Two disks can fail; mount survives. Three
disks failed → still unmountable even though RAID6 could in principle
recover the data — the volume header is the gate.

**Tradeoff**: requiring majority means a 2-disk failure where the
two failed disks happen to be the "ahead" ones (committed seqno N+1
while the surviving 4 still see N) silently rolls back to seqno N.
This is the correct ZFS-equivalent behavior — the user loses any
writes between N and N+1, but the filesystem state is consistent.

---

## Disks at different seqnos

If `disk_seq[i] == seq − 1` for one disk and the rest are at `seq`,
that disk fell behind a TXG. Two interpretations:

- **Intermittent failure**: the disk missed the most recent TXG
  commit write but otherwise survived. Mount mounts the array at
  `seq` using N−1 disks, marks the lagging disk as degraded, and
  resilvers it.
- **Stale insert**: a disk from a prior point-in-time was reinserted.
  Same handling — resilver promotes it.

The mount code does **not** attempt to merge across seqnos. The
lagging disk's metadata zone is overwritten during resilver.

---

## Rollback on majority failure

If no seqno achieves majority:

1. Try `seq − 1`. If majority at `seq − 1`, mount there (admin
   intervention recommended; data between `seq − 1` and `seq` is
   lost but the array is consistent).
2. If not, continue back one TXG at a time, up to a configurable
   limit (default 8). Beyond that, refuse to mount.

`hammer2 raid mount --rollback=N` lets the admin override the limit
explicitly, but the default refuses silent multi-TXG rollback.

---

## Recovery scenarios

| Disks survived | Volume header outcome           | Result                |
|----------------|---------------------------------|-----------------------|
| N (healthy)    | All at seqno T                  | Mount at T            |
| N−1            | N−1 at T, 1 dead                | Mount at T            |
| N−1            | N−2 at T, 1 at T−1, 1 dead      | Mount at T            |
| N−2            | N−2 at T                        | Mount at T (degraded) |
| N−2 (4-disk N) | 2 at T, 2 dead                  | ENXIO (2 < majority=3)|
| N−2 (6-disk N) | 4 at T, 2 dead                  | Mount at T            |
| All N          | Disks split T+1 vs T (no majority) | Roll back to T     |

---

## Interaction with `hammer2 raid fail-disk` / `replace`

The runtime fail/replace ioctls (memory: Fix 11) update `voldata.raid_config.disk_state`
and `flags`. Those fields persist via the same volhdr write path. v4
inherits this mechanism — the ioctls also need to bump `v4_txg_seq`
and synchronously commit so a subsequent crash doesn't lose the
state change.

---

## Open items deferred to Phase 1

- Exact placement of v4 addendum fields in `voldata` (need to verify
  there's a clean reserved area or use the raid_config substructure).
- Mount-time UI: how clearly to surface "rolled back to seqno T−1"
  to the user. `dmesg` line plus a sysctl indicator.
- Resilver trigger from mount-time disk-at-lower-seqno detection
  (auto-resilver vs require admin invocation).
