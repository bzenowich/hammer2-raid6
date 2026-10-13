# Outstanding work

The current gap list for the v3 (RAIDZ2-native) array, as of 2026-10-13.
The detail and the reasoning live in `oct-review.md` (section numbers
below refer to it).  Done and merged, so not listed any more: the
reliability findings R1-R11, performance items P1, P3 (steps 1-2), P4,
P5/S9 (sub-columns, on by default at 32 KB), P6 (batching, resume
cursor, allocated-only Phase A), P8 (up to a9bd49f), P10 (flush
counters), scrub through the self-heal helper (S-f), the row-ordered
parity scrub (S6), write-fault injection and the lifecycle tests
(groups W and X), the refused-mount unwind test (L6), the ENOSPC fix,
and metadata on 3 copies (layout 3, section 12).  What is left:

## Needs a person at the hardware (Pi)

- **Disk gone, real unplug.** Disk gone under load is fixed and tested
  on h2dev with the `inject_gone` hook (group X, X9: failed and closed
  without waiting for the member's buffers, replace onto the same path).
  A real USB unplug on the Pi is not yet tested; it needs the user
  present.
- **CAM `xpt_bus_flush` fix.** The fix for the fork
  (`sys/bus/cam/cam_xpt.c`) is written but waits for a disconnect test
  with the user present.
- **A USB WRITE timeout on da0 (Pi)** happened once, during group N on
  2026-10-13, and never before in full runs. Not explained; if it
  returns, capture the Pi console around it.

## Correctness and robustness

- **Characterize-failed-chain fault (review 7.6).** One page fault in
  `hammer2_characterize_failed_chain` seen once, not reproduced. The
  NULL-deref and the parent walk were hardened (516f0fb);
  `tests/v3/stress_failchain.sh` (not in `run_all.sh`) ran 53 rounds
  clean. `H2_DEBUG=1 ./deploy.sh` builds with `-g` for the next dump.
- **Refused-mount unwinds (review 7.7).** L6 covers the super-root
  check and a missing PFS label. The space map init, space map rebuild,
  PFS acquire and `init_volumes` unwinds share the helper but have no
  test.
- **Layout 3 has no way back.** Metadata is on 3 copies on new arrays;
  a layout 2 array converts with `vfs.hammer2.md_3copy_upgrade` on a RW
  mount, and a converted (or new) array is refused by a kernel that
  predates layout 3. Downgrade is not supported.

## Performance

- **Async P/Q sealing (P3 step 3).** Row sealing still waits for the P
  and Q writes in putblk. Moving it to an async per-mount path is
  blocked by `read_sibling`'s raw fallback, which reads the disk
  directly and would see a row whose parity is still in flight.
- **P8: seal a degraded row once every writable data disk is
  allocated** (f4d39b0 on branch `fix/p8-alloc`) was deliberately not
  merged: the measured gain is about nothing. Degraded rows still stay
  open until the flush.

## Test gaps

- **`NDISKS=6` in a periodic matrix.** X6 only reports the disk count;
  the h2dev guest has vbd1..4, so a 6-disk run needs two more images
  and a harness change.
- **Disk gone on the x86 guest.** `DISK_GONE` is compiled in only for
  the Pi (arm64) config, so a QMP `device_del` test cannot run on h2dev;
  X9 injects the event instead.

## Product and upstream

- Pi 4 hardware work (USB/UAS robustness, SMART -> auto-fail) is tracked
  in `dfly/docs/dfly-arm.md`.
- DTL: a resilver resumes from its committed cursor but still
  re-syncs every live slot above it; a dirty-time log would sync only
  what was written while the disk was out.
- TRIM/discard for freed stripes.
- Upstream prep: rebase on DragonFly master, convert `local_*` copies to
  per-file diffs, man pages for `hammer2 raid ...`, a performance
  baseline on real hardware (`tests/perf/`), a crash-safety statement.
- `hammer2_raid6.patch` lags `src/` (regenerate with `bin/mkpatch`;
  `deploy.sh` warns). `docs/metadata_zone.md` (N-way mirror) and
  `docs/stripe_bitmap.md` (replaced by `docs/capacity.md`) still
  describe the old layout; `docs/DEVELOPER.md` is current.
