# Test duration: what each test needs to prove, and what it costs

Goal: the shortest workload that still fails on the bug each test targets.
A workload is only cut when the trimmed test keeps (or gains) an assertion
that the condition it depends on really happened.

Measured on h2dev (4 disks of 4 GB, virtio, kernel at 475a4ae) with the
per-result `[+Ns]` timing.  Pi figures: the 2 h 20 min run on 4 x 16 GB
slices (kernel #16) for N3, D and N1+N2; everything else is h2dev time
x ~5 (the small groups on the Pi measured ~15 min against 162 s on
h2dev; their time is mount/newfs/scrub overhead, not data).  "Pi new" is
an estimate, not a measurement.

## Per test

| Test | Must prove (bug class) | Workload today | h2dev before | Pi before | Minimum that still proves it | Change | h2dev after | Pi new (est.) |
|---|---|---|---|---|---|---|---|---|
| A1-A4 | write/cold read, small files, bref layout, COW slot allocation | 100 MB, small files, 8 blocks | 12 s (group) | ~1 min | already minimal | none | 10-13 s | ~1 min |
| A5 (new) | newfs onto a smaller size must clear the old array's header zones (found while trimming) | newfs twice, 3 dd reads of 64 KB | n/a | n/a | media check only, nothing mounted | added; FAILs until newfs is fixed | ~1 s | seconds |
| B1-B6 | degraded cold read per disk, absent-disk write | 4 x ~16 MB files, one remount per case | 25 s | ~2 min | already minimal; time is mount/newfs | none | 25 s | ~2 min |
| C01-C06 | every dual-failure pair reconstructs | 6 pairs, remount each | 31 s | ~2.5 min | all 6 pairs (that is the coverage) | none | 31 s | ~2.5 min |
| D1, D2, D5, D6 | resilver correctness (single, sequential, stale re-add, dual) | 5-50 MB of data, 4 GB disks | ~35 s | ~3 min | data volume is already small; walk is allocated-only (skip=1) | none | ~35 s | ~3 min |
| D3 | writes concurrent with a skip=0 (whole-disk) resilver | skip=0 walk of the whole disk: 97 s here, ~11 min on 16 GB | 104 s | ~11 min | the walk only has to outlast `sleep 1` + the write, and the overlap is asserted (`kill -0`) | array built with 2 GB per disk (`setup_fresh 2g`) | 30-35 s | ~1.5 min |
| D4 | skip=1 skips unallocated slots, skip=0 walks them | skip=0 whole-disk walk (121 s here, 11 min Pi) then skip=1; asserted by wall clock | 129 s | ~11.5 min | a marker slot far past the allocated rows: skip=0 must overwrite it, skip=1 must leave it. Deterministic, size independent, no clock | 2 GB array + media marker check; times printed only | 38-60 s | ~1.5 min |
| E1-E3 | EIO on a surviving column during reconstruction/resilver | small | 13 s | ~1 min | already minimal | none | - | ~1 min |
| F1-F3 | COW slot invariant, P/Q parity, snapshot | 1-4 MB files | 12 s | ~1 min | already minimal | none | - | ~1 min |
| G1-G3 | fail-disk persistence, degraded mount | small | 13 s | ~1 min | already minimal | none | - | ~1 min |
| H1-H2 | snapshot semantics under degradation | 4 MB | 8 s | <1 min | already minimal | none | - | <1 min |
| I1-I3 | forced unmount loses nothing, bulkfree after | 16 MB | 11 s | ~1 min | already minimal; keep full array (header rotation) | none | - | ~1 min |
| J1-J2 | packed rows survive sibling deletes / snapshot | 4-file packs | 8 s | <1 min | already minimal | none | - | <1 min |
| K1-K3 | scrub clean / detects / runs with writes | 5-32 MB, 100 MB reads | 14 s | ~1 min | already minimal | none | - | ~1 min |
| L1-L3 | read-path self-heal, metadata failover | 32 MB, 200 files | 15 s | ~1 min | already minimal | none | - | ~1 min |
| N1 | data past the old 2 GB-per-disk limit reads back and counts in df | 72 x 64 MB = 4.6 GB (old limit 4 GB for 2 data disks + 512 MB) | 124 s | ~2.5 min | MUST exceed 2 GB per disk x ndata; cannot shrink. Needs the whole-disk array (`setup_fresh full`) | none (marked `full`) | 97 s | ~2.5 min |
| N2 | two-stage bulkfree, reuse of freed rows | free 3/4, then write ~4.1 GB, which must exceed what was free before the frees | ~290 s | ~2.5 min | the write must be larger than the pre-free free space (3.3 GB), set by N1's fill; fixed | none | ~270 s | ~2.5 min |
| N3 | snapshot rows are never freed while the allocator wraps with bulkfree running | churn 3 x capacity: 192 rounds of 512 MB on a 32 GB array, bulkfree after each | 448 s | ~95 min | wrapping needs churn > capacity whatever the capacity; keep 3x. Smallest array is 2 GB per disk (one zone), 4 GB usable: 24 rounds | `setup_fresh 2g`; added assertion that the churn exceeded 3x capacity with every write succeeding and the array not full at the end (only possible if freed rows were reused) | 267 s (24 x 512 MB) | ~12 min |
| P1-P3 | I/O shape (no read-before-write, balanced, amp) | 256 MB, 4000 small files | 42 s | ~3.5 min | P2 writes 5 GB: needs the whole array | none (marked `full`) | 42 s | ~3.5 min |
| S1-S4 | split volume-header quorum | header zeroing, replace | 24 s | ~2 min | needs several header zones: keep full array | none | 24 s | ~2 min |

h2dev "-" means not rerun; those tests are unchanged.

Total: h2dev 1294 s -> ~880 s (group N 807 -> 541/548 s over two runs, D 269 -> 89/129 s; the VM is shared, so timings move by ~30%).  Pi: ~140 min -> about
35-40 min (N3 -83 min, D -20 min, the rest unchanged).

## How the trims are justified

### N3
`newfs_hammer2 -s a:b:c:d` takes one size per disk (a single `-s` is a total
for JBOD and would give disk 0 everything).  RAID6 rounds each disk down to a
multiple of the 2 GB zone (`local_mkfs_hammer2.c`, `min_size &= ~ZONE_MASK`),
so 2g is the smallest array: 4 GB usable with 4 disks.  Capacity is what the
allocator has to get through, so the old workload (3 x the capacity of 16 GB
disks) was simply 8x more than the 3x needed.

The trimmed test still fails on its bug.  Mutation check: in
`hammer2_raid6_bf_end` the test `isset(stripe_bf_seen, slot) || isset(stripe_bf_new, slot)`
was reduced to the second term (bulkfree no longer honours references).
Result on h2dev: `N3: snapshot data lost after allocator wrap`, live file
wrong, scrub bad=4096, 4097 CHECK FAILs (N2 failed as well: 4 files wrong,
bad=3906).  The mutant was not committed.

Wrap assertion: the allocator cursor is not exported.  The churn writes only
new bytes, so writing 3x capacity with no ENOSPC while the array ends less
than full (`used < capacity`) proves that freed rows were handed out again.
A counter of cursor wraps (or of rows allocated below the snapshot's
offsets) would make this direct; none is added here.

### D3 / D4
The 11 minute figures on the Pi are the walk of every slot of a 16 GB
disk.  The cost scales with the array, and nothing tested depends on the
size, so both use the 2 GB array (walk 29-53 s here, ~1.5 min on the Pi).

D4 used to assert `skip=1` faster than `skip=0` by the clock.  It is now a
media check: after `fresh_disk` a 4 MB marker of random data is written
into the rebuilt disk 125 MB into the data zone, far past the 10 MB of
allocated rows.  Resilver with skip=0 reconstructs and writes every slot, so
the marker must change; with skip=1 the slot is unallocated and the marker
must stay.  That is deterministic and fails if either mode stops behaving as
named.  A kernel counter of stripes actually rebuilt (`resilver_stripes_done`
counts the loop position, not the work, and equals the total in both modes)
would be better still.

D3 keeps its overlap assertion (`kill -0` on the resilver after the
writes), which is what the shorter walk could break; it held (writes
finished while the resilver ran, 27 s walk).

### Why other tests are not trimmed
Everything else costs 3-8 s per case, nearly all mount/newfs/scrub overhead
on a few MB.  An optional `FRESH_SIZE=2g sh run_all.sh ...` builds the small
groups' arrays at the smallest size (B C E F G H J K L passed on h2dev, but
saved only ~15%); it is not used by default, and must not be used for S and
I: a 2 GB array has one volume-header zone, so header rotation and crash
recovery are not exercised.  N1/N2 and P pass `full` explicitly.

## Measured on the Pi (4x 16 GB USB SSD slices)
Kernel #18 ran the untrimmed suite, kernel #19 the trimmed one (both
2026-10-11, same disks).  Seconds per group:

| Group | untrimmed (#18) | trimmed (#19) |
|-------|-----------------|---------------|
| A     | 23   | 25 (A5 added) |
| B     | 51   | 47   |
| C     | 78   | 77   |
| D     | 1519 | 320  |
| E-L   | 194  | 192  |
| N     | 5902 | 1456 |
| P     | 149  | 127  |
| S     | 21*  | 67   |
| total | ~7940 (2 h 12 min) | 2331 (39 min) |

\* S failed on a `sort -k` portability bug in #18's run (fixed in
15951ea); its fixed rerun took 65 s.

- N3: 5170 s for 187 x 512 MB on the full array; 731 s for 24 x 512 MB on
  the 2 GB array, wrap asserted (12288 MB written > 3 x 3960 MB, 1190 MB
  in use at the end).
- D4: skip=0 walk 730 s on the full disk, 83 s at 2 GB; skip=1 21 s /
  4.5 s.  Both media-marker assertions passed.
- D3: the 2 GB walk took 76 s and the concurrent writes finished inside
  it (7 s), so the overlap margin is wide.
- What is left is N1+N2 (~12 min: 4.6 GB written and read back cold on
  the full disks, which they need to cross 2 GB per disk) and D's other
  resilvers (~4 min).  Neither has a cheap trim that keeps what it proves.

## Found while doing this (kernel / newfs, not fixed here)
1. `newfs_hammer2 -s` onto a disk that held a bigger array leaves that
   array's volume headers at 2/4/6 GB.  Mount adopts the highest
   rz_txg_seq, so it takes a stale header, finds no valid space map,
   fails the rebuild and refuses the RW mount.  `setup_fresh` with a size
   zeroes all four header zones as a workaround; A5 tests newfs directly
   and fails until it is fixed.
2. The refused mount leaves the member devices open (`malloc_uninit`
   leak at kldunload): newfs, dd on the disks "Device busy" until the
   guest is rebooted.
