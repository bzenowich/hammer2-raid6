# HAMMER2 v3 RAIDZ2-native — In-Place-Overwrite Audit

**Status**: Phase 0 audit. Phase 1 inserts the guards itemized here.
**Cross-refs**: `newplan.md` §5.3, §9.5. `raidz2_snapshot_interaction.md`.

> **Numbering note (2026-05-24).** The text below uses "v3" for the
> pre-rewrite RAID6-below-HAMMER2 path that this audit plans to remove,
> and "v4" for the RAIDZ2-native design that replaces it.  The on-disk
> format that now ships is `HAMMER2_VOL_VERSION_RAIDZ2 = 3`; the
> intermediate `=4` numbering used during Phase 1 was collapsed.
> Treat every "v4 guard" / "v4 array" / "v4 design" in this doc as
> referring to what now ships as v3 RAIDZ2-native.

---

## Architectural rule

> Under v4 RAID6, **every chain modification allocates a fresh stripe
> slot.** No in-place sector overwrite. The TXG-commit boundary becomes
> the atomic point that promotes new stripes from tentative to live.

If any code path can update an existing on-disk sector for a DATA or
DIRENT block without going through `hammer2_freemap_alloc` /
`hammer2_raid6_stripe_alloc`, the v4 design is broken — the no-RMW
invariant fails and the write hole reopens.

This document enumerates **every** path in the kernel that can lead
to a non-allocating modification, and the guard that closes each.

---

## Sites

### Site 1: `hammer2_chain_modify`, overwrite-in-place branch
**File**: `src/sys/local_hammer2_chain.c:1719-1731`
**Behavior (v3)**: For `DATA | DIRENT`, if check method = NONE *and*
`modify_tid > pfs_lsnap_tid`, set `newmod = 0` (in-place overwrite).
This is HAMMER2's "no checksum, no snapshot pinning the prior version"
optimization.

**v4 guard (already present)**: lines 1755-1761 force `newmod = 1` if
`hmp->raid_type == HAMMER2_RAID_TYPE_RAID6`.

**Issue (Phase 1 fix)**: the guard is keyed on `raid_type` only. v3
RAID6 (pre-`HAMMER2_VOL_VERSION_RAIDZ2`) was also `raid_type ==
RAID6` but relied on in-place RMW for parity. The guard should
predicate on `voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2`:

```c
if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
    hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2)
    newmod = 1;
```

Since Phase 1 drops v3 support entirely (`raid_type == RAID6` will
only mean v4), the version check is defensive — preferred for clarity
even when redundant.

---

### Site 2: `hammer2_chain_modify`, HMNT2_EMERG branch (first path)
**File**: `src/sys/local_hammer2_chain.c:1732-1747`
**Behavior**: If `HMNT2_EMERG` is set and `modify_tid > pfs_lsnap_tid`,
set `newmod = 0` for any chain type. Comment explicitly says: "NOT
SAFE. A storage failure, power failure, or panic can corrupt the
filesystem."

**v4 guard**: deny `HMNT2_EMERG` at the ioctl layer for RAID6 mounts
(see Site 5 below). The branch then becomes unreachable. As a
belt-and-braces, add the same `raid_type == RAID6 && version >=
RAIDZ2 → newmod = 1` override after the if/else cascade (the existing
guard at line 1760 already does this for both branches).

---

### Site 3: `hammer2_chain_modify`, HMNT2_EMERG branch (second path)
**File**: `src/sys/local_hammer2_chain.c:1888-1917`
**Behavior**: If allocation fails *and* `HMNT2_EMERG`, reuse the same
block (`error = 0`, set `BREF_FLAG_EMERG_MIP`). Comment: "virtually
guaranteed to corrupt any snapshots."

**v4 guard**: deny `HMNT2_EMERG` (Site 5). Belt-and-braces: refuse
this fallback on RAID6+v4 by checking `hmp->voldata.version >=
HAMMER2_VOL_VERSION_RAIDZ2` and returning the original error instead
of overriding to 0. Phase 1 patch:

```c
if (error && (hmp->hflags & HMNT2_EMERG) &&
    !(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
      hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2)) {
    error = 0;
    chain->bref.flags |= HAMMER2_BREF_FLAG_EMERG_MIP;
    ...
}
```

---

### Site 4: `hammer2_chain_modify`, dedup-off reuse path
**File**: `src/sys/local_hammer2_chain.c:1833-1862`
**Behavior**: When `dedup_off != 0`, the caller is requesting that the
chain point at an existing duplicate-content block. `chain->bref.data_off
= dedup_off`; no new allocation. This is correct (and intended) —
no new sector is written.

**v4 status**: **safe by construction**. Dedup is a pointer change,
not a write. The chain references existing data that already lives in
a stripe slot. P+Q for that slot exist as written when the original
block was placed. No RMW.

However, **dedup must be restricted to blocks within the *same* RAID6
array**. Cross-array dedup makes no sense (it does not happen in
HAMMER2 today; flag as Phase 1 audit checkpoint only).

No code change. Add a comment at the dedup_off branch noting why it
is safe under v4.

---

### Site 5: `hammer2_ioctl_emerg_mode`
**File**: `src/sys/local_hammer2_ioctl.c:1063-1090`
**Behavior**: Enables `HMNT2_EMERG`.

**v4 guard (Phase 1)**:

```c
static int
hammer2_ioctl_emerg_mode(hammer2_inode_t *ip, u_int mode)
{
    hammer2_dev_t *hmp = ip->pmp->pfs_hmps[0];

    if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
        hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2)
        return (EOPNOTSUPP);

    /* existing body unchanged */
}
```

`man hammer2` should be updated to note this restriction.

---

### Site 6: `hammer2_freemap_alloc` direct callers
**Files**: callers of `hammer2_freemap_alloc` outside `hammer2_chain_modify`.

**Behavior**: any caller that allocates a DATA/DIRENT block via the
v3 freemap instead of v4's `hammer2_raid6_stripe_alloc` would bypass
the stripe bitmap and break the no-RMW invariant.

**v4 guard**: dispatch in `hammer2_freemap_alloc` itself. For
`bref->type == DATA || DIRENT` on a v4 array, redirect to
`hammer2_raid6_stripe_alloc`. For all other types (INODE, INDIRECT,
FREEMAP_*) restrict the freemap search to the metadata extents
(see `metadata_zone.md` §Allocator behavior).

Existing v4 WIP at `local_hammer2_chain.c:1879-1880` already does this
dispatch at the caller (line 1879: stripe_alloc; line 1882: freemap_alloc).
Phase 1 will move the dispatch inside `hammer2_freemap_alloc` so every
caller is covered, not just `hammer2_chain_modify`.

---

### Site 7: `hammer2_chain_growfile` / inode truncate-grow
**Files**: search for grow paths.
**Behavior**: extending an inode's size beyond its current block. This
involves a new chain allocation, not an in-place modification.

**v4 status**: should be safe (allocates new DATA blocks via the
freemap → v4 stripe-alloc dispatch in Site 6). Phase 1 audit
checkpoint: confirm by grep that no shortcut sets `chain->bref.data_off`
directly without invoking the allocator.

---

### Site 8: Snapshot creation and rollback
**Files**: `local_hammer2_vfsops.c`, `local_hammer2_ioctl.c` snapshot
ioctls.
**Behavior**: snapshot creation = blockset copy at the PFS root
(pmp->pfs_iroot_blocksets[0]). No data overwrite.
Snapshot restore (atomic rollback) replaces the live blockset with
the snapshot's. No data overwrite.

**v4 status**: **safe by construction**. Snapshots are blockref-tree
references, not data movement. See `newplan.md` §5.7 and
`raidz2_snapshot_interaction.md`.

No code change.

---

### Site 9: Bulkfree
**File**: `local_hammer2_bulkfree.c`.
**Behavior**: scans live blockrefs, frees unreferenced blocks via the
freemap radix.

**v4 changes** (Phase 1):
- For DATA/DIRENT blocks, clear bits in the stripe bitmap rather than
  the freemap.
- Walk now includes verifying stripe bitmap reachability against
  the on-disk bitmap (per `stripe_bitmap.md` §Mount-time verify).

Not strictly an "in-place overwrite" site, but the dispatch logic
mirrors Site 6.

---

### Site 10: `hammer2_chain_load_data` writes back stale buffer cache?
**File**: `local_hammer2_chain.c:1685`.
**Behavior**: loads `chain->data` from disk via `hammer2_io_bread`.
Read-only on this path.

**v4 status**: **safe by construction**. No write side.

---

## Summary

| Site | File:line                       | Phase 1 action                            |
|------|--------------------------------|-------------------------------------------|
| 1    | chain.c:1719-1761              | Tighten guard with version check          |
| 2    | chain.c:1732-1747              | Unreachable once Site 5 lands             |
| 3    | chain.c:1888-1917              | Refuse EMERG fallback on v4               |
| 4    | chain.c:1833-1862              | Document as safe; comment added           |
| 5    | ioctl.c:1063-1090              | Return EOPNOTSUPP on v4 RAID6             |
| 6    | freemap.c                      | Dispatch on bref.type to stripe-alloc     |
| 7    | growfile path                  | Audit (no code change expected)           |
| 8    | snapshot ioctls                | Safe by construction                      |
| 9    | bulkfree.c                     | Stripe-bitmap-aware free path             |
| 10   | chain.c:1685                   | Safe (read-only)                          |

Sites 1, 3, 5, 6, and 9 require Phase 1 patches. The rest are audit
checkpoints — verify, document, no code change expected.

---

## Test

A simple test confirms the no-in-place invariant holds end-to-end:

```sh
# Format v4 array, write a block, snapshot, overwrite the block.
hammer2 raid newfs --raid6 ...
mount ...
dd if=/dev/urandom of=/mnt/file bs=64k count=1
hammer2 snapshot /mnt /mnt/.snap1
echo "modified" | dd of=/mnt/file bs=64k count=1 conv=notrunc

# Check that the data_off of file's chain has changed.
hammer2 raid stripe-trace /mnt/file > after.txt
# data_off should differ from the snapshot's reference.
```

Phase 2 adds this as `tests/v3/test_inplace_guard.sh`.
