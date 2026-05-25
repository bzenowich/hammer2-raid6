# Outstanding work

Phase 1 of `docs/newplan.md` is content-complete on `v4-rebuild` (see
`phase1_changelog.md`).  Remaining work is organized by phase.

## Phase 2 exit gate

Phase 2 exit criteria are met on `v4-rebuild` HEAD.  Full
`tests/v3` suite on NDISKS=4 vbd substrate runs **44/44 PASS**
(A 4, B 6, C 6, D 8, E 4, F 4, G 4, H 4, I 4), no panics, no
`CHECK FAIL` in dmesg.

| Group | Result | Coverage |
|---|---|---|
| A | 4/4 | basic R/W + small files + COW slot uniqueness |
| B | 6/6 | single-disk fail at every position |
| C | 6/6 | C(4,2) dual-disk fail pairs |
| D | 8/8 | resilver basic + sequential + concurrent-write |
| E | 4/4 | EIO injection — no panic, error surfaces cleanly |
| F | 4/4 | COW invariant + snapshot COW healthy |
| G | 4/4 | absent-disk degraded mount + fail-state persistence |
| H | 4/4 | snapshot under healthy + degraded states |
| I | 4/4 | unclean unmount, degraded, bulkfree-after-crash |

Stretch items (non-blocking for Phase 3):

A. NDISKS=4..10 matrix test for ndata edges.
B. Cull dead pre-rewrite scripts under `tests/other/`.

## Known latent bugs

**Stale data in unwritten packed-row columns.** When `write_row`
seals a packed row with `ncols < ndata` (e.g. only one chain
landed before a TXG flush or before open_rows[] capped at 16
forced an early seal), P/Q are computed assuming the unwritten
data columns are zero — but those columns aren't actually zeroed
on disk.  If the slot ever held content from a previously-freed
chain (or the underlying disk was never zeroed past the metadata
zone), the disk carries non-zero bytes there, so parity
reconstruction of the *written* column in that row yields wrong
data on resilver or scrub.

Surfaced by:

- **M3 Group K** scrub repair test.  K2 dodges by writing a small
  file (2 MB / 16 rows ≤ open_rows cap) so every row packs full,
  plus `dd if=/dev/zero` over every disk before its `setup_fresh`.
- **Group D** resilver, intermittently.  D1's "post-remount data
  mismatch" reproduces: after the resilver onto a fresh disk
  completes, remounting and re-reading the test file finds N
  CHECK FAIL chains.  The resilver's stripe-by-stripe
  reconstruction used the same zero-assumed-but-non-zero columns
  the scrub repair did, so it wrote wrong bytes to the
  replacement.  This failure is *not* caused by the M3 scrub
  changes (scrub doesn't run in Group D); it was always latent
  and happens to surface depending on the slot reuse pattern.

Real fix is in the allocator/seal path: either zero the unwritten
cols at seal time (cheap — one extra bwrite per missing data col
per partial row), or zero stripe slots at allocation time (more
work but bounds the cost).

## Phase 3 — real hardware bring-up

Blocked on Phase 2 exit + a physical target machine (spec is in
host-local agent memory, not the repo).  Items from `physical_disk_tasks.md`
not yet implemented:

- SMART → auto-fail (item 8).
- I/O timeout handling so a hung disk doesn't block the array (item 9).
- TRIM/discard for freed stripes (item 10).
- Always-on TXG-commit cache flush (deferred from Group C per newplan §6).
- Pull-the-cable test on physical SATA.

## Phase 4 — upstream prep

Blocked on Phase 3.  Per newplan §7 Phase 4:

- Rebase against current DragonFlyBSD `master`; convert `local_*` copies
  to clean per-file diffs.
- Man-page entries for `hammer2 raid {status,fail-disk,replace}`.
- **Performance baseline matrix.**  The `tests/perf/` scaffold (commit
  `aaddef6`) is the infrastructure.  Numbers are only meaningful on
  Phase 3 hardware — qcow2-backed VM disk perf is dominated by host SSD
  characteristics, not RAID6 algorithmic costs.  Calibrate the
  thresholds in `tests/perf/README.md` against the first physical run.
- Crash-safety statement (COW + TXG-commit atomicity, no WIB needed,
  failure modes enumerated).
- Draft `docs/RAID6_MERGE_REQUEST.md` v2.

## Phase 5 — optional follow-ups (post-v1)

Listed in newplan §7 Phase 5; non-blocking.

## Already closed (for reference)

- Upstream-grade Makefile/conf-files update so `hammer2_raid6.c` lands
  in the kernel build path — closed by commit `8d1c551`.
- Userspace `mkfs_hammer2.h` `RaidType`/`Ndisks` gap — same commit.
- `tests/perf/` scaffold (fio harness, JSON ingest, sysctl/dmesg
  snapshots) — commit `aaddef6`.  Calibration belongs to Phase 4.
