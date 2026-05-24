# Phase 1 Deletion Tracker (v4-rebuild)

**Status**: Phase 0 punch list. Drives Phase 1 cleanup.
**Cross-refs**: `newplan.md` §6 (carry-over and deletion lists), §7 Phase 1.
**Branch**: `v4-rebuild` (created from `harness` @ 18f4420).

> **Numbering note (2026-05-24).** "v3" below means the pre-rewrite
> RAID6-below-HAMMER2 layer that this tracker plans to delete; "v4"
> means the RAIDZ2-native design that replaces it.  Both unshipped
> numbers were later collapsed into a single
> `HAMMER2_VOL_VERSION_RAIDZ2 = 3` on disk.  See `newplan.md`'s
> numbering note for context.

Goal: enter Phase 2 with code that **compiles, mounts a single-disk
volume, and contains no v3-era complexity**. Per-item commits, each
small enough to revert independently.

Reference counts (grep `raid6_old_data | raid_nfailed | absent_data |
resilver_dirty`) at audit time:

| File                              | Hits |
|-----------------------------------|------|
| `src/sys/local_hammer2_io.c`      | 73   |
| `src/sys/local_hammer2_ioctl.c`   | 7    |
| `src/sys/local_hammer2.h`         | 5    |
| `src/sys/local_hammer2_vfsops.c`  | 4    |
| `src/sys/local_hammer2_raid6.c`   | 1    |

After Phase 1 these should drop to zero (`absent_data` may survive
in modified form if degraded-mount needs it for the new metadata
mirror path; reevaluate then).

---

## Group A: RMW delta parity infrastructure — DELETE

The COW write path (`newplan.md` §5.4) issues exactly three parallel
`bawrite`s per data write: D, P, Q. No reads, no old_data, no delta.

| Item | Symbol / region                                        | Location                                | Action |
|------|--------------------------------------------------------|-----------------------------------------|--------|
| A1   | `dio->raid6_old_data` field                            | `local_hammer2.h` (struct hammer2_io)   | Delete |
| A2   | All `kmalloc`s saving `old_data` in `_hammer2_io_getblk` | `local_hammer2_io.c:336-345`, ~478-549, ~603-642, ~716-724 | Delete |
| A3   | `old_data` parameter on `hammer2_io_raid6_write`       | `local_hammer2_io.c`, signature + callers | Delete |
| A4   | RMW delta path in `hammer2_io_raid6_write`             | Read-P, read-Q, XOR, compute new P/Q    | Delete |
| A5   | RMW pre-emptive `breadnx` of sibling data columns      | `local_hammer2_io.c:~234`               | Delete |
| A6   | Comments in `local_hammer2.h` describing old_data invariant | header comments                    | Delete |
| A7   | Background parity thread (`h2par-<dev>`)                | `local_hammer2_raid6.c`                 | Delete; COW path is synchronous-write of three columns directly |

Acceptance: `grep -rn 'old_data' src/sys/local_*.c` returns nothing.

---

## Group B: Logical→physical mapping — DELETE

| Item | Symbol                                                 | Location                       | Action |
|------|--------------------------------------------------------|--------------------------------|--------|
| B1   | `hammer2_raid6_map`                                    | `local_hammer2_io.c:~234`      | Delete |
| B2   | Callers of `hammer2_raid6_map`                         | `local_hammer2_io.c`           | Delete; v4 uses bref->copyid and bref->data_off directly |
| B3   | "Two-layer addressing" comments                        | various                        | Delete |

Acceptance: `grep -rn 'raid6_map\|logical.*stripe\|stripe.*logical' src/sys/local_*.c` returns nothing.

---

## Group C: vn test-infrastructure patches — DELETE

| Item | Symbol / file                                          | Location                                | Action |
|------|--------------------------------------------------------|-----------------------------------------|--------|
| C1   | `hammer2_flush_vn_backing`                             | `local_hammer2_io.c:~757-810`           | Delete |
| C2   | Declaration in header                                  | `local_hammer2.h:1953`                  | Delete |
| C3   | Caller in `hammer2_vfs_sync_pmp`                       | `local_hammer2_vfsops.c:2939`           | Delete; replace with unconditional `BUF_CMD_FLUSH` per disk at TXG commit (`newplan.md` §6 "issue device cache flush before TXG commit — but always run it") |
| C4   | `src/sys/local_vn.c` (Fix 13 IO_SYNC patch)            | entire file                             | Delete (revert to stock DragonFlyBSD vn.c) |

Acceptance: `ls src/sys/local_vn.c` returns ENOENT; `grep -rn 'flush_vn_backing' src/sys/local_*.{c,h}` returns nothing.

---

## Group D: Degraded-mode throttling — DELETE / REWRITE

Per `newplan.md` §6: "Sync-bwrite-in-degraded throttling (Fixes 10
framing). The COW design removes the need."

| Item | Symbol                                                 | Location                                | Action |
|------|--------------------------------------------------------|-----------------------------------------|--------|
| D1   | "if `raid_nfailed > 0` → sync write" branches          | `local_hammer2_io.c:919, ~1004, ~478, ~403` | Delete; COW writes are uniformly `bawrite` |
| D2   | "skip sibling-column breadnx" conditional              | `local_hammer2_io.c:919`                | Delete (no sibling reads at all in COW) |
| D3   | `hmp->raid_nfailed` field itself                       | `local_hammer2.h`                       | Keep (status display + auto-fail), but remove write-path branches |
| D4   | `_hammer2_io_putblk` inline parity for `raid_nfailed > 0` | `local_hammer2_io.c`                  | Delete; single write path |

Acceptance: `grep -n 'raid_nfailed > 0' src/sys/local_hammer2_io.c` returns only the status-display callers (read path, auto-fail), not write-path branches.

---

## Group E: Resilver dirty-range tracking — DELETE

Per `newplan.md` §5.9: "Dirty-range tracking, Phase 4 re-scan,
vfs_sync_pmp in mid-resilver — all unnecessary."

| Item | Symbol                                                 | Location                                | Action |
|------|--------------------------------------------------------|-----------------------------------------|--------|
| E1   | `hmp->resilver_dirty_lo` / `_hi`                       | `local_hammer2.h`                       | Delete |
| E2   | Update of dirty range in `_hammer2_io_putblk`          | `local_hammer2_io.c:~1067`              | Delete |
| E3   | Phase 4 dirty-range re-resilver                        | `local_hammer2_ioctl.c` (resilver path) | Delete |

Acceptance: `grep -rn 'resilver_dirty\|dirty_lo\|dirty_hi' src/sys/local_*.{c,h}` returns nothing.

---

## Group F: In-place-overwrite guards — REWRITE

Per `docs/inplace_audit.md`:

| Item | Symbol                                                 | Location                                | Action |
|------|--------------------------------------------------------|-----------------------------------------|--------|
| F1   | Existing v4 guard on `raid_type == RAID6`              | `local_hammer2_chain.c:1755-1761`       | Tighten with `voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2` check |
| F2   | `HMNT2_EMERG` allocation-fail fallback                 | `local_hammer2_chain.c:1888-1917`       | Refuse on v4 (return original error) |
| F3   | `hammer2_ioctl_emerg_mode`                             | `local_hammer2_ioctl.c:1063-1090`       | Return EOPNOTSUPP on v4 |
| F4   | `hammer2_freemap_alloc` dispatch                       | `local_hammer2_freemap.c`               | Inside the function: for DATA/DIRENT on v4, redirect to `hammer2_raid6_stripe_alloc`; for METADATA, restrict search to metadata extents |

Acceptance: in-place test (`docs/inplace_audit.md` §Test) shows `data_off` changes on every write.

---

## Group G: DIO key encoding — REWRITE

Per `newplan.md` §9.5. The current WIP at `9d537be` placed v4 DATA/DIRENT
keys at `total_size + disk*disk_size + phys_off` to avoid v3 aliasing.
That fix is correct but stacked on a confused encoding. Re-derive cleanly:

| Item | Symbol                                                 | Location                                | Action |
|------|--------------------------------------------------------|-----------------------------------------|--------|
| G1   | DIO key derivation in `hammer2_io_alloc`               | `local_hammer2_io.c:~146-260`           | Rewrite: single function `hammer2_dio_key(bref) → pbase` that produces a unique key per (disk_idx, phys_off) by construction |
| G2   | `dev_pbase` computation                                | `local_hammer2_io.c:~204-220`           | Mirror G1: `dev_pbase = pbase & per_disk_mask` falls out of the encoding |
| G3   | Comments documenting "v4 vs v3" alias avoidance        | various                                 | Replace with one canonical comment at the new `hammer2_dio_key` |

Acceptance criterion: collision-avoidance falls out of the encoding,
not from `if (v4) { ... } else { ... }` arithmetic. Unit test in
Phase 1 verifies no two valid (disk, off) pairs map to the same key.

---

## Group H: Stripe allocator scaffolding — REPLACE

The current `hammer2_raid6_stripe_alloc` (v4 WIP) allocates from the
zone-41 bitmap. Per `stripe_bitmap.md`, the bitmap format formalizes
in Phase 1 with a header, footer, generation, CRC, and on-disk
cursor.

| Item | Symbol                                                 | Location                                | Action |
|------|--------------------------------------------------------|-----------------------------------------|--------|
| H1   | `hammer2_raid6_stripe_alloc`                           | (new file?) `local_hammer2_stripe.c`    | Implement against new bitmap format |
| H2   | `hammer2_raid6_stripe_free`                            | same                                    | Implement |
| H3   | Bitmap header/footer read+verify                       | same                                    | Implement |
| H4   | Mount-time blockref-walk reconstruction                | same                                    | Implement |
| H5   | TXG-flush write sequence                               | same                                    | Implement |

H1–H5 are not strictly "deletions" but are listed here to keep the
tracker complete.

---

## Group I: Metadata zone — NEW

| Item | Symbol                                                 | Location                                | Action |
|------|--------------------------------------------------------|-----------------------------------------|--------|
| I1   | `hammer2_md_extent` struct                             | `local_hammer2.h`                       | Add |
| I2   | `newfs_hammer2 --raid6` zone-0 sizing                  | `local_mkfs_hammer2.c`                  | Add (default 5% per-disk LBA, min 64 MB) |
| I3   | `--metadata-size` flag                                 | `local_mkfs_hammer2.c`                  | Add |
| I4   | Allocator dispatch for metadata types                  | `local_hammer2_freemap.c`               | Already part of F4 |
| I5   | Mirrored write rule for metadata DIOs                  | `local_hammer2_io.c`                    | Add (write to every disk at same offset) |
| I6   | Resilver Phase A (sequential metadata copy)            | `local_hammer2_raid6.c` (resilver)      | Add |

---

## Group J: Volume header v4 fields — NEW

Per `volhdr_quorum.md`:

| Item | Symbol                                                 | Location                                | Action |
|------|--------------------------------------------------------|-----------------------------------------|--------|
| J1   | `rz_txg_seq`, `rz_array_uuid`, `rz_disk_id`, `rz_ndisks` | `local_hammer2.h` (volhdr or raid_config) | Add |
| J2   | Mount-time quorum logic                                | `local_hammer2_vfsops.c` (mount path)   | Add |
| J3   | Per-TXG seqno bump                                     | `local_hammer2_flush.c` (volhdr write)  | Add |
| J4   | `newfs_hammer2 --raid6` UUID gen                       | `local_mkfs_hammer2.c`                  | Add |

---

## Group K: Test infrastructure — REPLACE

Per `newplan.md` §6 "Tests, recovered" and §8 "no vn-backed test runs
as authoritative":

| Item | Action                                                 |
|------|--------------------------------------------------------|
| K1   | Delete `tests/mdraid/` (v3 test suite)                |
| K2   | Replace `tests/raidz2native/` with `tests/v3/` against virtio-blk |
| K3   | Keep combo-test structure; rewrite per v4 semantics    |
| K4   | Delete vn-backed test paths from `common.sh`           |

---

## Sequencing

Suggested commit order (each group ≤ 1 day of work):

1. **G (DIO key)** — affects everything downstream. Get this clean first.
2. **B (raid6_map)** — once G is clean, B falls out.
3. **F (in-place guards)** — small, mostly textual.
4. **C (vn patches)** — pure deletion.
5. **A (RMW)** — large but mechanical once G and B are in.
6. **D (throttling)** — depends on A.
7. **E (resilver dirty)** — independent.
8. **H–J (new code)** — driven by per-topic specs (`docs/{stripe_bitmap,metadata_zone,volhdr_quorum}.md`).
9. **K (tests)** — last; rewrite against the new code.

Phase 1 exit: single-disk mount works; multi-disk RAID6 code compiles
but is inert until ≥4 disks attached (gated by `rz_ndisks` field).
