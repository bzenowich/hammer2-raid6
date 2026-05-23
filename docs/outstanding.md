# Outstanding work after Phase 1 close

**Phase 1 of newplan.md is content-complete on `v4-rebuild`** (see
`phase1_changelog.md`).  Remaining work for the v1 v4 release:

**Phase 3 — real hardware bring-up.**  Needs the physical target machine
(target spec lives in host-local memory, not the repo).  Real SATA cache
flushes, real NCQ, real EIO.
Always-on TXG-commit cache flush (deferred from Group C) is a Phase 3 task
per newplan §6.

**Perf baseline.**  `tests/perf/` (commit `aaddef6`) is wired but
uncalibrated.  Needs:
1. fio installed on VM (README documents source build — not in DragonFly
   base pkg).
2. First clean run against `v4-rebuild` HEAD to establish numbers.
3. Calibrate the acceptance thresholds proposed in `tests/perf/README.md`
   based on observed ratios.

**Test-suite expansion** (newplan §7 Phase 2 Exit):
- Snapshot stress (no coverage yet).
- EIO injection — needs a `sysctl vfs.hammer2.fault_inject_disk_idx` hook
  before a `tests/v4/test_e_eio_inject.sh` can land.
- Power-cut sim (qemu monitor `quit` mid-write + remount).
- NDISKS=4..10 matrix test for ndata edges (off-by-one risk in parity-
  width math).
- Cull dead vn-era scripts under `tests/other/` (most are pre-Group-K).

**Pre-existing (not Phase 1):** upstream-grade Makefile/conf-files
update so `hammer2_raid6.c` is in the kernel build path was closed by
commit `8d1c551`.  Userspace `mkfs_hammer2.h` RaidType/Ndisks gap was
also closed in the same commit.
