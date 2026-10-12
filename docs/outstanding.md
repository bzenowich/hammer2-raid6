# Outstanding work

The current gap list for the v3 (RAIDZ2-native) array.  The detail and
the reasoning live in `oct-review.md` (section numbers below refer to
it); the reliability findings R1-R10 and performance items P1, P3
(steps 1-2), P4, P6 (batching) and P8 are merged.  What is left:

## Correctness and robustness

- **Refused-mount panic (review 7.7).** A mount refused after the
  super-root fails its check can panic in `hammer2_pfsfree_scan` ->
  `hammer2_inode_drop` ("panic with 1 spinlocks held").  Repro: corrupt
  the super-root on every copy and mount; it must fail with an error.
- **Scrub vs. self-heal (S-f).** Scrub still uses its own, weaker
  reconstruction.  Make it call the read-path self-heal helper; this is
  a behaviour change, so it needs its own test pass (groups K and L).
- **Scrub parity sweep (S6).** Scrub is a queue-depth-1 tree walk and
  never checks P/Q.  Add a row-ordered sweep that reads consecutive
  allocated rows on all disks in parallel, checks P and Q, and uses the
  syndrome to localise a single bad column.
- **Characterize-failed-chain fault (review 7.6).** One page fault in
  `hammer2_characterize_failed_chain` (deref of `chain->dio->bp`) during
  group B with a concurrent self-heal repair.  Not reproduced; needs a
  debug-info module build on h2dev for the next dump.

## Performance

- **Sub-column allocation (P5/S9).** Every chain takes a whole 64 KB
  column, so small and compressed blocks waste space and compression
  saves nothing on v3.  Needs the per-row column mask (S1) or a
  sub-allocator; the design is being written up in `oct-review.md`.
  The refcount byte (S-d, an 8x larger bitmap that is only ever tested
  for non-zero) is part of the same change.
- **Resilver resume cursor.** The batched resilver is not resumable;
  an interrupted run starts over.  Persist a cursor.
- **Phase A allocated-only copy.** The resilver metadata copy still
  copies the whole metadata zone (about 5% of each disk) instead of only
  the freemap-allocated segments.
- **Async P/Q sealing (P3 step 3).** Row sealing still waits for the P
  and Q writes in putblk.  Moving it to an async per-mount path is
  blocked by `read_sibling`'s raw fallback, which reads the disk
  directly and would see a row whose parity is still in flight.

## Test gaps (review section 7)

- **4. Write-side fault injection:** `inject_wio_disk_mask` (EIO on
  write) and `inject_delay_ms_disk_mask` (hang without failure), a group
  that writes during a hang and asserts no panic and an auto-failed
  member, and a QMP `device_del` test for `disk_gone`.
- **5. Lifecycle tests:** unmount and `umount -f` during a resilver or
  scrub; fill to ENOSPC, then delete + bulkfree + write; `NDISKS=6` in a
  periodic matrix; `raid6_simd=0` vs. `1` parity cross-check on arm64.
- **6. Characterize-failed-chain** (above): add a stress test once it
  reproduces.
- **7. Refused-mount unwind** (above): add the corrupt-super-root test
  with the fix.

## Product and upstream

- Pi 4 hardware work (USB/UAS robustness, SMART -> auto-fail, I/O
  timeouts for a hung, not detached, disk: review R11) is tracked in
  `dfly/docs/dfly-arm.md`.
- TRIM/discard for freed stripes.
- Upstream prep: rebase on DragonFly master, convert `local_*` copies to
  per-file diffs, man pages for `hammer2 raid ...`, a performance
  baseline on real hardware (`tests/perf/`), a crash-safety statement.
